/**
* (C) Copyright 2025 Hewlett Packard Enterprise Development LP
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
* OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
******************************************************************************
* EnergyCounter: Fetch and expose energy counters.
* nvidia_gpu.c: Interface for NVIDIA GPUs.
*
* URL       https://github.com/HewlettPackard/EnergyCounter
******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <linux/limits.h>
#include "interface.h"

/* Prototypes used externaly */
void nvidia_gpu_fini(Component_t *gpus);
void nvidia_gpu_update(Component_t *gpus);

#ifdef NVIDIA_GPU
#include "dcgm_agent.h"
#include "dcgm_structs.h"

dcgmHandle_t _dcgm_handle        = { 0 };
dcgmGpuGrp_t _dcgm_group         = { 0 };
dcgmFieldGrp_t _dcgm_field_group = { 0 };
char _dcgm_group_name[]          = "energy_group";

static uint64_t _dcgm_energy[DCGM_MAX_NUM_DEVICES] = { 0 };

static int get_total_energy(unsigned int gpu_id, dcgmFieldValue_v1 *field, int num_values, void *user_data)
{
    _dcgm_energy[gpu_id] = field[0].value.i64;
    return 0;
}
#endif /* NVIDIA_GPU */

/**
 * Write latest counter value in the destination file for a given GPU
 *
 * @param   dev[in]  Unit structure for the GPU
 */

static void _nvidia_device_update_files(Unit_t *dev)
{
#ifdef NVIDIA_GPU
    uint64_t last_energy_raw = dev->energy_raw;

    dev->energy_raw = _dcgm_energy[dev->id];

    /* First iteration */
    if (!last_energy_raw)
        return;

    /* XXX: This type should be large enough to never overflow */
    assert(dev->energy_raw >= last_energy_raw);

    dev->energy_interval = (dev->energy_raw - last_energy_raw) / 1E3;
    dev->energy_acc += dev->energy_interval;

    /* Updating the file */
    fprintf(dev->energy_fd, "%lu Joules", dev->energy_acc);
    rewind(dev->energy_fd); /* Flush buffer and prepare for overwriting next value */

#endif /* NVIDIA_GPU */
}

/**
 * Initialize this GPU module
 *
 * @param   gpus[out]       GPU structure to initialize all GPUs
 * @param   dest_dir[in]    Directory contaning the files with the energy counters
 * @param   is_verbose[in]  Whether the verbose mode should be enabled
 * @param   is_disabled[in] Whether the module should be disabled
 */
void nvidia_gpu_init(Component_t *gpus, const char *dest_dir, const bool is_verbose, const bool is_disabled)
{
    memset(gpus, 0, sizeof(Component_t));
    gpus->is_verbose = is_verbose;
    gpus->vendor = NVIDIA;
    gpus->type = GPU;
    gpus->fini = nvidia_gpu_fini;
    gpus->update = nvidia_gpu_update;

#ifdef NVIDIA_GPU
    if (is_disabled)
        return;

    dcgmReturn_t ret;

    /* Initialize DCGM */
    ret = dcgmInit();
    if (ret != DCGM_ST_OK)
    {
        fprintf(stderr, "Unable initialize DCGM engine: %s\n", errorString(ret));
        exit(EXIT_FAILURE);
    }

    /* Use embedded mode here */
    ret = dcgmStartEmbedded(DCGM_OPERATION_MODE_MANUAL, &_dcgm_handle);
    if (ret != DCGM_ST_OK)
    {
        fprintf(stderr, "Unable to start embedded DCGM engine: %s\n", errorString(ret));
        goto exit;
    }

    /* Fetch all available devices */
    uint32_t gpu_ids[DCGM_MAX_NUM_DEVICES];
    int count;
    ret = dcgmGetAllSupportedDevices(_dcgm_handle, gpu_ids, &count);
    if (ret != DCGM_ST_OK)
    {
        fprintf(stderr, "Unable to list NVIDIA devices: %s\n", errorString(ret));
        goto exit;
    }

    /* Clean up if no NVIDIA GPU */
    if (count == 0)
        goto exit;

    gpus->n_siblings = (uint32_t)count;
    if (is_verbose)
        printf("%u NVIDIA GPU devices found\n", gpus->n_siblings);

    assert(gpus->n_siblings < N_SIBLINGS_MAX);

    /* Create a group. */
    ret = dcgmGroupCreate(_dcgm_handle, DCGM_GROUP_DEFAULT, _dcgm_group_name, &_dcgm_group);
    if (ret != DCGM_ST_OK)
    {
        fprintf(stderr, "Cannot create a DGCM group: %s\n", errorString(ret));
        goto exit;
    }
    /* Total energy consumption for each GPU in mJ since the driver was last reloaded */
    unsigned short field_id = DCGM_FI_DEV_TOTAL_ENERGY_CONSUMPTION;

    /* Create a field group. */
    ret = dcgmFieldGroupCreate(_dcgm_handle, 1, &field_id, (char *)"TOTAL_ENERGY", &_dcgm_field_group);
    if (ret != DCGM_ST_OK)
    {
        fprintf(stderr, "Cannot create a DGCM field group: %s\n", errorString(ret));
        goto exit;
    }

    for (uint32_t i = 0; i < gpus->n_siblings; ++i) {
        Unit_t *dev = &gpus->siblings[i];
        dev->id = gpu_ids[i];

        /* Retrieving the PCIe address of the device */
        dcgmDeviceAttributes_t attributes = { .version = dcgmDeviceAttributes_version };
        ret = dcgmGetDeviceAttributes(_dcgm_handle, dev->id, &attributes);
        if (ret != DCGM_ST_OK) {
            fprintf(stderr, "Cannot retrieve GPU %d PCIe address: %s\n", dev->id, errorString(ret));
            goto exit;
        }

        /* Converting the PCIe address */
        attributes.identifiers.pciBusId[11] = '\0';
        dev->bus_id = (uint32_t)strtol(&attributes.identifiers.pciBusId[9], NULL, 16);

        char output_path[PATH_MAX];

        /* Opening normalized file (Joules) */
        snprintf(output_path, sizeof(output_path), "%s/gpu_%2.2lx_energy", dest_dir, dev->bus_id);
        dev->energy_fd = fopen(output_path, "w");
        if (!dev->energy_fd)
        {
            fprintf(stderr, "Failed to open output file: %s\n", output_path);
            ret = DCGM_ST_GENERIC_ERROR;
            goto exit;
        }
    }

    return;

exit:
    dcgmStopEmbedded(_dcgm_handle);
    dcgmShutdown();

    if (ret != DCGM_ST_OK)
        exit(EXIT_FAILURE);
#endif /* NVIDIA_GPU */
}

/**
 * Cleanup the module
 *
 * @param   gpus[in]     GPU structure to clean up
 */
void nvidia_gpu_fini(Component_t *gpus)
{
#ifdef NVIDIA_GPU
    for (uint32_t i = 0; i < gpus->n_siblings; ++i)
    {
        Unit_t *dev = &gpus->siblings[i];
        fclose(dev->energy_fd);
    }

    dcgmGroupDestroy(_dcgm_handle, _dcgm_group);
    dcgmShutdown();

#endif /* NVIDIA_GPU */
}

/**
 * Retrieve last energy value for each GPU and update the destination files
 *
 * @param   gpus[inout] GPU structure
 */
void nvidia_gpu_update(Component_t *gpus)
{
    const bool is_verbose = gpus->is_verbose;

#ifdef NVIDIA_GPU
    dcgmReturn_t ret;

    /* Set a watch on the energy consumption field */
    ret = dcgmWatchFields(_dcgm_handle, _dcgm_group, _dcgm_field_group, 100000, 60.0, 100);
    if (ret != DCGM_ST_OK)
    {
        fprintf(stderr, "Cannot set DGCM field watch: %s\n", errorString(ret));
        goto error;
    }

    /* Force fields update */
    dcgmUpdateAllFields(_dcgm_handle, 1);

    /* Stop the watch */
    dcgmUnwatchFields(_dcgm_handle, _dcgm_group, _dcgm_field_group);

    /* Retrieve the total energy consumption for all selected devices */
    ret = dcgmGetLatestValues(_dcgm_handle, _dcgm_group, _dcgm_field_group, &get_total_energy, NULL);
    if (ret != DCGM_ST_OK)
    {
        fprintf(stderr, "Cannot get latest values: %s\n", errorString(ret));
        goto error;
    }
#endif /* NVIDIA_GPU */

    for (uint32_t i = 0; i < gpus->n_siblings; ++i)
    {
        Unit_t *dev = &gpus->siblings[i];
        _nvidia_device_update_files(dev);

        if (is_verbose)
            printf("Nvidia GPU %u (0x%2.2lx): %lu J (accumulator: %lu J, raw: %lu)\n",
                   dev->id, dev->bus_id, dev->energy_interval, dev->energy_acc, dev->energy_raw);
    }

#ifdef NVIDIA_GPU
    return;

error:
    nvidia_gpu_fini(gpus);
    exit(EXIT_FAILURE);
#endif /* NVIDIA_GPU */
}
