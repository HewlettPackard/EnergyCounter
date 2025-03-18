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
* cpu.c: Module for AMD and INTEL CPUs.
*
* URL       https://github.com/HewlettPackard/EnergyCounter
******************************************************************************/

#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <linux/limits.h>
#include <math.h>
#include "interface.h"
#include "common.h"

#define MSR_AMD_PACKAGE_ENERGY   0xc001029b
#define MSR_INTEL_PACKAGE_ENERGY 0x611

static uint32_t _cpu_package_to_core[N_SIBLINGS_MAX] = {0};

/* Prototypes used externaly */
void cpu_fini(Component_t *cpus);
void cpu_update(Component_t *cpus);

#ifdef CPU_PACKAGE
/**
 * Retrieve the current value of the package energy counter
 *
 * @param   package[out]  Unit structure for the package
 * @param   vendor[in]    Vendor type
 */
static void _cpu_package_fetch_energy(Unit_t *package, const int vendor)
{
    uint32_t core_id = _cpu_package_to_core[package->id];

    switch (vendor)
    {
        case INTEL:
            package->energy_raw = read_msr(core_id, MSR_INTEL_PACKAGE_ENERGY);
            break;
        case AMD:
            package->energy_raw = read_msr(core_id, MSR_AMD_PACKAGE_ENERGY);
            break;
        default:
            fprintf(stderr, "Unknown or supported CPU type: %d\n", vendor);
            exit(EXIT_FAILURE);
    }

    /* Return if resolution was already fetched */
    if (package->energy_resolution > 0)
        return;

    uint64_t msr_unit;
    switch (vendor)
    {
        case INTEL:
            msr_unit = read_msr(core_id, MSR_INTEL_POWER_UNIT);
            break;
        case AMD:
            msr_unit = read_msr(core_id, MSR_AMD_POWER_UNIT);
            break;
        default:
            return;
    }

    package->energy_resolution = pow(0.5, (double)((msr_unit >> 8) & MSR_ENERGY_UNIT_MASK));
}
#endif /* CPU_PACKAGE */

/**
 * Write latest counter value in the destination file for a given CPU package
 *
 * @param   package[in]  Unit structure for the package
 * @param   vendor[in]   CPU vendor
 */
static void _cpu_package_update_files(Unit_t *package, const int vendor)
{
#ifdef CPU_PACKAGE
    uint64_t last_energy_raw = package->energy_raw;
    _cpu_package_fetch_energy(package, vendor);

    if (package->energy_raw >= last_energy_raw)
        package->energy_interval = package->energy_resolution * (package->energy_raw - last_energy_raw);
    else /* Counter may wraparound  */
        package->energy_interval = package->energy_resolution * (((1LU << 32) - last_energy_raw) + package->energy_raw);

    package->energy_acc += package->energy_interval;

    /* Updating the file */
    fprintf(package->energy_fd, "%lu Joules", package->energy_acc);
    rewind(package->energy_fd); /* Flush buffer and prepare for overwriting next value */
#endif /* CPU_PACKAGE */
}

/**
 * Initialize this CPU module
 *
 * @param   cpus[out]       CPU structure to initialize all CPU packages
 * @param   dest_dir[in]    Directory contaning the files with the energy counters
 * @param   is_verbose[in]  Whether the verbose mode should be enabled
 * @param   is_disabled[in] Whether the module should be disabled
 */
void cpu_init(Component_t *cpus, const char *dest_dir, const bool is_verbose, const bool is_disabled)
{
    memset(cpus, 0, sizeof(Component_t));
    cpus->is_verbose = is_verbose;
    cpus->type = CPU;
    cpus->vendor = get_vendor();
    cpus->fini = cpu_fini;
    cpus->update = cpu_update;

#ifdef CPU_PACKAGE
    if (cpus->vendor != INTEL && cpus->vendor != AMD)
        return;

    if (is_disabled)
        return;

    /* Get package mapping and amount of packages */
    for(uint32_t i = 0;; i++)
    {
        char file_path[PATH_MAX];
        uint32_t package_id;
        snprintf(file_path, PATH_MAX, "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", i);

        FILE *file = fopen(file_path,"r");
        if (file == NULL)
            break;

        fscanf(file,"%u", &package_id);
        fclose(file);
        cpus->n_siblings = MAX(cpus->n_siblings, package_id + 1);
        _cpu_package_to_core[package_id] = i;
    }

    if (is_verbose)
        printf("%s CPU(s) found with %u package(s)\n", vendor_str[cpus->vendor], cpus->n_siblings);

    assert(cpus->n_siblings < N_SIBLINGS_MAX);

    for (uint32_t i = 0; i < cpus->n_siblings; i++) {
        Unit_t *package = &cpus->siblings[i];
        package->id = i;

        /* Fetching first raw value */
        _cpu_package_fetch_energy(package, cpus->vendor);

        char output_path[PATH_MAX];

        /* Opening normalized file (Joules) */
        snprintf(output_path, sizeof(output_path), "%s/cpu_package_%d_energy", dest_dir, package->id);
        package->energy_fd = fopen(output_path, "w");
        if (!package->energy_fd)
        {
            fprintf(stderr, "Failed to open output file: %s\n", output_path);
            exit(EXIT_FAILURE);
        }
    }
#endif /* CPU_PACKAGE */
}

/**
 * Cleanup the module
 *
 * @param   cpus[in]     CPU structure to clean up
 */
void cpu_fini(Component_t *cpus)
{
#ifdef CPU_PACKAGE
    for (uint32_t i = 0; i < cpus->n_siblings; ++i)
    {
        Unit_t *package = &cpus->siblings[i];
        fclose(package->energy_fd);
    }
#endif /* CPU_PACKAGE */
}

/**
 * Retrieve last energy value for each package and update the destination files
 *
 * @param   cpus[inout] CPU structure
 */
void cpu_update(Component_t *cpus)
{
    const bool is_verbose = cpus->is_verbose;

    for (uint32_t i = 0; i < cpus->n_siblings; ++i)
    {
        Unit_t *package = &cpus->siblings[i];
        _cpu_package_update_files(package, cpus->vendor);

        if (is_verbose)
            printf("%s CPU package %u: %lu J (accumulator: %lu J, raw: %lu)\n",
                   vendor_str[cpus->vendor], i, package->energy_interval, package->energy_acc, package->energy_raw);
    }
}
