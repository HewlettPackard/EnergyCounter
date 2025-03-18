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
* dram.c: Module for DRAM.
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

#define MSR_INTEL_DRAM_PACKAGE_ENERGY  0x619

static uint32_t _dram_package_to_core[N_SIBLINGS_MAX] = {0};

/* Prototypes used externaly */
void dram_fini(Component_t *ram);
void dram_update(Component_t *ram);

#ifdef DRAM_PACKAGE
/**
 * Retrieve the current value of the DRAM energy counter for one CPU package
 *
 * @param   package[out]  Unit structure for the package
 * @param   vendor[in]    Vendor type
 */
static void _dram_package_fetch_energy(Unit_t *package, const int vendor)
{
    uint32_t core_id = _dram_package_to_core[package->id];

    switch (vendor)
    {
        case INTEL:
            package->energy_raw = read_msr(core_id, MSR_INTEL_DRAM_PACKAGE_ENERGY);
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
#endif /* DRAM_PACKAGE */

/**
 * Write latest DRAM counter value in the destination file for each CPU package
 *
 * @param   package[in]  Unit structure for the package
 * @param   vendor[in]   CPU vendor
 */
static void _dram_package_update_files(Unit_t *package, const int vendor)
{
#ifdef DRAM_PACKAGE
    uint64_t last_energy_raw = package->energy_raw;
    _dram_package_fetch_energy(package, vendor);

    if (package->energy_raw >= last_energy_raw)
        package->energy_interval = package->energy_resolution * (package->energy_raw - last_energy_raw);
    else /* Counter may wraparound  */
        package->energy_interval = package->energy_resolution * (((1LU << 32) - last_energy_raw) + package->energy_raw);

    package->energy_acc += package->energy_interval;

    /* Updating the file */
    fprintf(package->energy_fd, "%lu Joules", package->energy_acc);
    rewind(package->energy_fd); /* Flush buffer and prepare for overwriting next value */
#endif /* DRAM_PACKAGE */
}

/**
 * Initialize this DRAM module
 *
 * @param   drams[out]      DRAM structure to initialize all packages
 * @param   dest_dir[in]    Directory contaning the files with the energy counters
 * @param   is_verbose[in]  Whether the verbose mode should be enabled
 * @param   is_disabled[in] Whether the module should be disabled
 */
void dram_init(Component_t *drams, const char *dest_dir, const bool is_verbose, const bool is_disabled)
{
    memset(drams, 0, sizeof(Component_t));
    drams->is_verbose = is_verbose;
    drams->type = DRAM;
    drams->vendor = get_vendor();
    drams->fini = dram_fini;
    drams->update = dram_update;

#ifdef DRAM_PACKAGE
    if (drams->vendor != INTEL)
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
        drams->n_siblings = MAX(drams->n_siblings, package_id + 1);
        _dram_package_to_core[package_id] = i;
    }

    if (is_verbose)
        printf("DRAM(s) found with %u CPU package(s)\n", drams->n_siblings);

    assert(drams->n_siblings < N_SIBLINGS_MAX);

    for (uint32_t i = 0; i < drams->n_siblings; i++) {
        Unit_t *package = &drams->siblings[i];
        package->id = i;

        /* Fetching first raw value */
        _dram_package_fetch_energy(package, drams->vendor);

        char output_path[PATH_MAX];

        /* Opening normalized file (Joules) */
        snprintf(output_path, sizeof(output_path), "%s/dram_package_%d_energy", dest_dir, package->id);
        package->energy_fd = fopen(output_path, "w");
        if (!package->energy_fd)
        {
            fprintf(stderr, "Failed to open output file: %s\n", output_path);
            exit(EXIT_FAILURE);
        }
    }
#endif /* DRAM_PACKAGE */
}

/**
 * Cleanup the module
 *
 * @param   drams[in]     DRAM structure to clean up
 */
void dram_fini(Component_t *drams)
{
#ifdef DRAM_PACKAGE
    for (uint32_t i = 0; i < drams->n_siblings; ++i)
    {
        Unit_t *package = &drams->siblings[i];
        fclose(package->energy_fd);
    }
#endif /* DRAM_PACKAGE */
}

/**
 * Retrieve last DRAM energy value for each CPU package and update the destination files
 *
 * @param   drams[inout] DRAM structure
 */
void dram_update(Component_t *drams)
{
    const bool is_verbose = drams->is_verbose;

    for (uint32_t i = 0; i < drams->n_siblings; ++i)
    {
        Unit_t *package = &drams->siblings[i];
        _dram_package_update_files(package, drams->vendor);

        if (is_verbose)
            printf("DRAM package %u: %lu J (accumulator: %lu J, raw: %lu)\n",
                   i, package->energy_interval, package->energy_acc, package->energy_raw);
    }
}

