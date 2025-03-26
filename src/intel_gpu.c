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
* intel_gpu.c: Interface for Intel GPUs.
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
#include "common.h"

/* Prototypes used externaly */
void intel_gpu_fini(Component_t *gpus);
void intel_gpu_update(Component_t *gpus);

enum intel_model {
    INTEL_MODEL_UNKNOWN,
    MAX1550 = 1550,
};

#ifdef INTEL_GPU
#include <level_zero/ze_api.h>
#include <level_zero/zes_api.h>

zes_driver_handle_t *_zes_drivers       = NULL;
zes_device_handle_t *_zes_devices       = NULL;
zes_pwr_handle_t    *_zes_power_domains = NULL;
zes_pwr_handle_t    *_zes_power         = NULL;
uint32_t _zes_power_domains_max = 0;
#endif /* INTEL_GPU */

/**
 * Write latest counter value in the destination file for a given GPU
 *
 * @param   dev[in]  Unit structure for the GPU
 */
static void _intel_device_update_files(Unit_t *dev)
{
#ifdef INTEL_GPU
    uint64_t last_energy_raw = dev->energy_raw;

    zes_power_energy_counter_t energy_counter;
    ze_result_t ret = zesPowerGetEnergyCounter(_zes_power[dev->id], &energy_counter);
    if (ret != ZE_RESULT_SUCCESS)
    {
        const char *estring;
        zeDriverGetLastErrorDescription(_zes_drivers[0], &estring);
        fprintf(stderr, "Unable to retrieve energy counter from Intel device %u: %s\n", dev->id, estring);
    }
    dev->energy_raw = energy_counter.energy;

    /* First iteration */
    if (!last_energy_raw)
        return;

    /* XXX: This type should be large enough to never overflow */
    assert(dev->energy_raw >= last_energy_raw);

    dev->energy_interval = (dev->energy_raw - last_energy_raw) / 1E6;

    /* TODO: Use a better model like the one for AMD MI250Xs based on GPU usage */
    if (dev->model == MAX1550) /* Half for each tile */
        dev->energy_interval /= 2;

    dev->energy_acc += dev->energy_interval;

    /* Updating the file */
    fprintf(dev->energy_fd, "%lu Joules", dev->energy_acc);
    rewind(dev->energy_fd); /* Flush buffer and prepare for overwriting next value */
#endif /* INTEL_GPU */
}

/**
 * Initialize this GPU module
 *
 * @param   gpus[out]       GPU structure to initialize all GPUs
 * @param   dest_dir[in]    Directory contaning the files with the energy counters
 * @param   is_verbose[in]  Whether the verbose mode should be enabled
 * @param   is_disabled[in] Whether the module should be disabled
 */
void intel_gpu_init(Component_t *gpus, const char *dest_dir, const bool is_verbose, const bool is_disabled)
{
    memset(gpus, 0, sizeof(Component_t));
    gpus->is_verbose = is_verbose;
    gpus->vendor = INTEL;
    gpus->type = GPU;
    gpus->fini = intel_gpu_fini;
    gpus->update = intel_gpu_update;

#ifdef INTEL_GPU
    if (is_disabled)
        return;

    /* Enable driver initialization and dependencies for system management */
    if (setenv("ZES_ENABLE_SYSMAN", "1", 1) != 0)
    {
        fprintf(stderr, "Unable to set ZES_ENABLE_SYSMAN environment variable.\n");
        exit(EXIT_FAILURE);
    }

    ze_result_t ret;

    /* Initialize OneAPI Level Zero */
    ret = zeInit(ZE_INIT_FLAG_GPU_ONLY);
    if (ret != ZE_RESULT_SUCCESS)
    {
        fprintf(stderr, "Unable initialize OneAPI Level Zero.\n");
        exit(EXIT_FAILURE);
    }

    uint32_t driver_count = 0;
    ret = zeDriverGet(&driver_count, NULL);
    if ((ret != ZE_RESULT_SUCCESS) || (driver_count == 0))
    {
        fprintf(stderr, "No OneAPI Level Zero driver available.\n");
        exit(EXIT_FAILURE);
    }

    _zes_drivers = malloc(driver_count * sizeof(ze_driver_handle_t));
    if (_zes_drivers == NULL)
    {
        fprintf(stderr, "Unable to allocate structure for OneAPI Level Zero driver.\n");
        goto exit;
    }

    ret = zeDriverGet(&driver_count, _zes_drivers);
    if (ret != ZE_RESULT_SUCCESS)
    {
        fprintf(stderr, "Unable to retrieve OneAPI Level Zero driver instances.\n");
        exit(EXIT_FAILURE);
    }

    /* Fetch all available devices */
    uint32_t count = 0;
    ret = zeDeviceGet(_zes_drivers[0], &count, NULL);
    if (ret != ZE_RESULT_SUCCESS)
    {
        const char *estring;
        zeDriverGetLastErrorDescription(_zes_drivers[0], &estring);
        fprintf(stderr, "Unable to list Intel devices: %s\n", estring);
        goto exit;
    }

    /* Clean up if no Intel GPU */
    if (count == 0)
        goto exit;

    gpus->n_siblings = count;
    if (is_verbose)
        printf("%u Intel GPU devices found\n", gpus->n_siblings);

    assert(gpus->n_siblings < N_SIBLINGS_MAX);

    _zes_devices = malloc(count * sizeof(zes_device_handle_t));
    if (_zes_devices == NULL)
    {
        fprintf(stderr, "Unable to allocate structure for OneAPI Level Zero devices.\n");
        goto exit;
    }

    ret = zeDeviceGet(_zes_drivers[0], &count, _zes_devices);
    if (ret != ZE_RESULT_SUCCESS)
    {
        const char *estring;
        zeDriverGetLastErrorDescription(_zes_drivers[0], &estring);
        fprintf(stderr, "Unable to retrieve Intel device handles: %s\n", estring);
        goto exit;
    }

    /* Find maximum power domain count */
    for (uint32_t i = 0; i < gpus->n_siblings; ++i)
    {
        zes_device_handle_t dev = _zes_devices[i];
        uint32_t power_count = 0;
        ret = zesDeviceEnumPowerDomains(dev, &power_count, NULL);
        if (ret != ZE_RESULT_SUCCESS || power_count == 0)
        {
            const char *estring;
            zeDriverGetLastErrorDescription(_zes_drivers[0], &estring);
            fprintf(stderr, "Unable to retrieve power domain for GPU %u: %s\n", i, estring);
            goto exit;
        }

        _zes_power_domains_max = MAX(_zes_power_domains_max, power_count);
    }

    _zes_power_domains = malloc(_zes_power_domains_max * gpus->n_siblings * sizeof(zes_pwr_handle_t));
    _zes_power = malloc(gpus->n_siblings * sizeof(zes_pwr_handle_t));
    if ((_zes_power_domains == NULL) || (_zes_power == NULL))
    {
        fprintf(stderr, "Unable to allocate power domain structures for OneAPI Level Zero.\n");
        goto exit;
    }

    for (uint32_t i = 0; i < gpus->n_siblings; ++i)
    {
        zes_device_handle_t zes_dev = _zes_devices[i];
        Unit_t *dev = &gpus->siblings[i];
        dev->id = i;

        /* Retrieving the PCIe address of the device */
        zes_pci_properties_t pci_prop;
        ret = zesDevicePciGetProperties(zes_dev, &pci_prop);
        if (ret != ZE_RESULT_SUCCESS)
        {
            const char *estring;
            zeDriverGetLastErrorDescription(_zes_drivers[0], &estring);
            fprintf(stderr, "Unable to retrieve PCIe address of Intel device %u: %s\n", i, estring);
            goto exit;
        }

        dev->bus_id = (uint32_t)pci_prop.address.bus;

        /* Check if 2 consecutive GPUs belong to the same board */
        if (i > 0)
        {
            Unit_t *prev_dev = &gpus->siblings[i-1];
            if (dev->bus_id == prev_dev->bus_id)
            {
                prev_dev->peer = dev;
                if (is_verbose)
                    printf("Intel GPU %u and %u share the same board\n", prev_dev->id, dev->id);
            }
        }

        /* Retrieve GPU model */
        zes_device_properties_t dev_props;
        dev_props.stype = ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES;
        ret = zesDeviceGetProperties(zes_dev, &dev_props);
        if (ret != ZE_RESULT_SUCCESS)
        {
            const char *estring;
            zeDriverGetLastErrorDescription(_zes_drivers[0], &estring);
            fprintf(stderr, "Unable to retrieve GPU model for Intel device %u: %s\n", i, estring);
            goto exit;
        }

        if (strstr(dev_props.modelName, "Max 1550") != NULL)
            dev->model = MAX1550;

        if (is_verbose && dev->model == MAX1550)
            printf("Intel Max 1550 found, enabling split (50/50) energy consumption across tiles\n");

        /* Retrieve power domains */
        uint32_t power_count = _zes_power_domains_max;
        ret = zesDeviceEnumPowerDomains(zes_dev, &power_count, &_zes_power_domains[i * _zes_power_domains_max]);
        if (ret != ZE_RESULT_SUCCESS)
        {
            const char *estring;
            zeDriverGetLastErrorDescription(_zes_drivers[0], &estring);
            fprintf(stderr, "Unable to retrieve power domains for Intel device %u: %s\n", i, estring);
            goto exit;
        }

        /* Locate domain for the whole package */
        for (uint32_t j = 0; j < power_count; j++)
        {
            zes_power_properties_t props =  { 0 };
            props.stype = ZES_STRUCTURE_TYPE_POWER_PROPERTIES;

            ret = zesPowerGetProperties(_zes_power_domains[i * _zes_power_domains_max + j], &props);
            if (ret != ZE_RESULT_SUCCESS)
            {
                const char *estring;
                zeDriverGetLastErrorDescription(_zes_drivers[0], &estring);
                fprintf(stderr, "Unable to retrieve power domain %u for Intel device %u: %s\n", j, i, estring);
                goto exit;
            }

            /* Whole package is not a subdevice */
            if (!props.onSubdevice)
            {
                _zes_power[i] = _zes_power_domains[i * _zes_power_domains_max + j];
                break;
            }
        }

        char output_path[PATH_MAX];
        /* Opening normalized file (Joules) */
        snprintf(output_path, sizeof(output_path), "%s/gpu_%2.2lx_%u_energy", dest_dir, dev->bus_id, dev->id);
        dev->energy_fd = fopen(output_path, "w");
        if (!dev->energy_fd)
        {
            fprintf(stderr, "Failed to open output file: %s\n", output_path);
            ret = ZE_RESULT_ERROR_UNKNOWN;
            goto exit;
        }
    }

    return;

exit:

    if (_zes_drivers != NULL)
        free(_zes_drivers);

    if (_zes_devices != NULL)
        free(_zes_devices);

    if (_zes_power_domains != NULL)
        free(_zes_power_domains);

    if (_zes_power != NULL)
        free(_zes_power);

    if (ret != ZE_RESULT_SUCCESS)
        exit(EXIT_FAILURE);
#endif /* INTEL_GPU */
}

/**
 * Cleanup the module
 *
 * @param   gpus[in]     GPU structure to clean up
 */
void intel_gpu_fini(Component_t *gpus)
{
#ifdef INTEL_GPU
    for (uint32_t i = 0; i < gpus->n_siblings; ++i)
    {
        Unit_t *dev = &gpus->siblings[i];
        fclose(dev->energy_fd);
    }

    free(_zes_power);
    free(_zes_power_domains);
    free(_zes_devices);
    free(_zes_drivers);
#endif /* INTEL_GPU */
}

/**
 * Retrieve last energy value for each GPU and update the destination files
 *
 * @param   gpus[inout] GPU structure
 */
void intel_gpu_update(Component_t *gpus)
{
    const bool is_verbose = gpus->is_verbose;

    for (uint32_t i = 0; i < gpus->n_siblings; ++i)
    {
        Unit_t *dev = &gpus->siblings[i];
        _intel_device_update_files(dev);

        if (is_verbose)
            printf("Intel GPU %u (0x%2.2lx): %lu J (accumulator: %lu J, raw: %lu)\n",
                   dev->id, dev->bus_id, dev->energy_interval, dev->energy_acc, dev->energy_raw);
    }
}
