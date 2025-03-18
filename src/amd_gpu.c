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
* amd_gpu.c: Interface for AMD GPUs.
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
void amd_gpu_fini(Component_t *gpus);
void amd_gpu_update(Component_t *gpus);

enum amd_model {
    AMD_MODEL_UNKNOWN,
    MI250 = 2828,
};

#ifdef AMD_GPU
#include <rocm_smi/rocm_smi.h>

/**
 * Retrieve the current value of the energy counter of a GPU
 *
 * @param   dev[out]  Unit structure for the GPU
 */
static void _amd_device_fetch_energy(Unit_t *dev)
{
    uint64_t last_energy_raw = dev->energy_raw;
    float energy_resolution;

    rsmi_status_t err = rsmi_dev_energy_count_get(dev->id,
                                                  &dev->energy_raw,
                                                  &energy_resolution,
                                                  &dev->timestamp);
    if (err != RSMI_STATUS_SUCCESS)
    {
        fprintf(stderr, "Failed to get energy counter for AMD device %u\n", dev->id);
        exit(EXIT_FAILURE);
    }

    /* XXX: This type should be large enough to never overflow */
    assert(dev->energy_raw >= last_energy_raw);

    dev->energy_resolution = (double)energy_resolution;

    /* Don't compute energy consumption during first iteration */
    if (last_energy_raw == 0)
        return;

    dev->energy_interval = dev->energy_resolution * (dev->energy_raw - last_energy_raw) / 1E6;
    dev->energy_acc += dev->energy_interval;
}

/**
 * Retrieve the current GPU activity
 *
 * @param   dev[out]  Unit structure for the GPU
 */
static void _amd_device_fetch_activity(Unit_t *dev)
{
    rsmi_status_t err = rsmi_dev_busy_percent_get(dev->id, &dev->busy_percent);
    if (err != RSMI_STATUS_SUCCESS)
    {
        fprintf(stderr, "Failed to get GPU utilization for AMD device %u\n", dev->id);
        exit(EXIT_FAILURE);
    }
}

/**
 * Retrieve energy from a MI250 and split across GCDs
 *
 * @param   dev[out]  Unit structure for the GPU
 */
static void _amd_device_fetch_energy_mi250(Unit_t *dev)
{
    /* Already hanlded by peer GCD */
    if (dev->peer == NULL)
        return;

    uint64_t last_energy_raw = dev->energy_raw;
    float energy_resolution;
    const uint32_t gcd_idle_power = 40;   /* Eache GCD consumes 40W when idle */
    uint64_t last_timestamp = dev->timestamp;

    _amd_device_fetch_activity(dev);
    _amd_device_fetch_activity(dev->peer);

    rsmi_status_t err = rsmi_dev_energy_count_get(dev->id,
                                                  &dev->energy_raw,
                                                  &energy_resolution,
                                                  &dev->timestamp);
    if (err != RSMI_STATUS_SUCCESS)
    {
        fprintf(stderr, "Failed to get energy counter for AMD device %u\n", dev->id);
        exit(EXIT_FAILURE);
    }

    dev->energy_resolution = (double)energy_resolution;

    /* XXX: This type should be large enough to never overflow */
    assert(dev->energy_raw >= last_energy_raw);

    /* Don't compute energy consumption during first iteration */
    if (last_energy_raw == 0)
        return;

    const uint64_t energy = dev->energy_resolution * (dev->energy_raw - last_energy_raw) / 1E6;

    /* The model to slipt the energy across the GCDs is the following:
     * 1) Subtract the idle consumption (GCD overhead) from the measured one
     * 2) A formula computes the share of each GCD based on the activity
     * 3) The idle and active share is added the the final energy counter */

    /* 1) GCD overhead (based on fixed Power consumption) and deduce it from the measured value */
    const uint64_t elapsed_s = (dev->timestamp - last_timestamp) / 1E9;
    const uint64_t energy_idle = gcd_idle_power * elapsed_s;
    const uint64_t energy_min_idle = (energy > (2 * energy_idle)) ? (energy - (2 * energy_idle)) : 0;

    /* 2) Compute the share coefficient of the first GCD (0 <= value <= 1) */
    const double energy_ratio = (0.005 * dev->busy_percent) - (0.005 * dev->peer->busy_percent) + 0.5;

    /* 3) Assign overhead and energy share to each GCD */
    dev->energy_interval = energy_idle + (energy_ratio * energy_min_idle);
    dev->energy_acc += dev->energy_interval;
    dev->peer->energy_interval = energy_idle + ((1.0 - energy_ratio) * energy_min_idle);
    dev->peer->energy_acc += dev->peer->energy_interval;
}
#endif /* AMD_GPU */

/**
 * Write latest counter value in the destination file for a given GPU
 *
 * @param   dev[in]  Unit structure for the GPU
 */
static void _amd_device_update_files(Unit_t *dev)
{
#ifdef AMD_GPU
    /* With MI250 we need to split energy across GCDs  */
    if (dev->model == MI250)
        _amd_device_fetch_energy_mi250(dev);
    else
        _amd_device_fetch_energy(dev);

    /* Updating the file */
    fprintf(dev->energy_fd, "%lu Joules", dev->energy_acc);
    rewind(dev->energy_fd); /* Flush buffer and prepare for overwriting next value */
#endif /* AMD_GPU */
}

/**
 * Initialize this GPU module
 *
 * @param   gpus[out]       GPU structure to initialize all GPUs
 * @param   dest_dir[in]    Directory contaning the files with the energy counters
 * @param   is_verbose[in]  Whether the verbose mode should be enabled
 * @param   is_disabled[in] Whether the module should be disabled
 */
void amd_gpu_init(Component_t *gpus, const char *dest_dir, const bool is_verbose, const bool is_disabled)
{
    memset(gpus, 0, sizeof(Component_t));
    gpus->is_verbose = is_verbose;
    gpus->vendor = AMD;
    gpus->type = GPU;
    gpus->fini = amd_gpu_fini;
    gpus->update = amd_gpu_update;

#ifdef AMD_GPU
    if (is_disabled)
        return;

    rsmi_status_t err;

    /* Initialize ROCm SMI */
    err = rsmi_init(0);
    if (err != RSMI_STATUS_SUCCESS)
    {
        fprintf(stderr, "Failed to initialize RSMI\n");
        exit(EXIT_FAILURE);
    }

    /* Get number of AMD GPU devices */
    err = rsmi_num_monitor_devices(&gpus->n_siblings);
    if (err != RSMI_STATUS_SUCCESS)
    {
        fprintf(stderr, "Failed to get number of devices\n");
        rsmi_shut_down();
        exit(EXIT_FAILURE);
    }

    if (is_verbose)
        printf("%u AMD GPU devices found\n", gpus->n_siblings);

    assert(gpus->n_siblings < N_SIBLINGS_MAX);


    for (uint32_t i = 0; i < gpus->n_siblings; ++i) {
        Unit_t *dev = &gpus->siblings[i];
        dev->id = i;

        /* Retrieve serial number to match GCDs on the same board */
        err = rsmi_dev_serial_number_get(i, dev->serial, sizeof(dev->serial));
        if (err != RSMI_STATUS_SUCCESS) {
            fprintf(stderr, "Failed to get the serial number of device %u: %d\n", i);
            exit(EXIT_FAILURE);
        }

        /* Check if 2 consecutive GCDs belong to the same board */
        if (i > 0)
        {
            Unit_t *prev_dev = &gpus->siblings[i-1];
            if (strncmp(dev->serial, prev_dev->serial, sizeof(dev->serial)) == 0)
            {
                prev_dev->peer = dev;
                if (is_verbose)
                    printf("AMD GCD %u and %u share the same board\n", prev_dev->id, dev->id);
            }
        }

        /* Retrieve GPU model */
        uint16_t model_id;
        err = rsmi_dev_subsystem_id_get(i, &model_id);
        if (err != RSMI_STATUS_SUCCESS) {
            fprintf(stderr, "Failed to get the model id of device %u: %d\n", i);
            exit(EXIT_FAILURE);
        }
        dev->model = model_id;

        if (is_verbose && dev->model == MI250)
            printf("AMD MI250 found, enabling model to split energy consumption across GCDs\n");

        /* Retrieving the PCIe address of the device */
        /* TODO: check if this is the most convenient way of identifying a device with Slurm */
        err = rsmi_dev_pci_id_get(i, &dev->bus_id);
        if (err != RSMI_STATUS_SUCCESS)
        {
            fprintf(stderr, "Failed to get PCI ID for device %u\n", i);
            exit(EXIT_FAILURE);
        }

        /* Fixing PCIe address shifted by a byte */
        dev->bus_id = dev->bus_id >> 8;

        /* Fetching first raw value */
        _amd_device_fetch_energy(dev);

        char output_path[PATH_MAX];

        /* Opening normalized file (Joules) */
        snprintf(output_path, sizeof(output_path), "%s/gpu_%2.2lx_energy", dest_dir, dev->bus_id);
        dev->energy_fd = fopen(output_path, "w");
        if (!dev->energy_fd)
        {
            fprintf(stderr, "Failed to open output file: %s\n", output_path);
            exit(EXIT_FAILURE);
        }
    }
#endif /* AMD_GPU */
}

/**
 * Cleanup the module
 *
 * @param   gpus[in]     GPU structure to clean up
 */
void amd_gpu_fini(Component_t *gpus)
{
#ifdef AMD_GPU
    for (uint32_t i = 0; i < gpus->n_siblings; ++i)
    {
        Unit_t *dev = &gpus->siblings[i];
        fclose(dev->energy_fd);
    }

    rsmi_shut_down();
#endif /* AMD_GPU */
}

/**
 * Retrieve last energy value for each GPU and update the destination files
 *
 * @param   gpus[inout] GPU structure
 */
void amd_gpu_update(Component_t *gpus)
{
    const bool is_verbose = gpus->is_verbose;

    for (uint32_t i = 0; i < gpus->n_siblings; ++i)
    {
        Unit_t *dev = &gpus->siblings[i];
        _amd_device_update_files(dev);

        if (is_verbose)
            printf("AMD GPU %u (0x%2.2lx): %lu J (accumulator: %lu J, raw: %lu)\n",
                   i, dev->bus_id, dev->energy_interval, dev->energy_acc, dev->energy_raw);
    }
}

