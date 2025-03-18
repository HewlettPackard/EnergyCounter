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
* mock.c: Module for mock component.
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
#include "interface.h"
#include "common.h"

/* Prototypes used externaly */
void mock_fini(Component_t *mocks);
void mock_update(Component_t *mocks);

/**
 * Write latest counter value in the destination file for a given mock unit
 *
 * @param   mock[in]  Mock unit structure
 */
static void _mock_update_files(Unit_t *mock)
{
    mock->energy_acc += mock->energy_interval;

    /* Updating the file */
    fprintf(mock->energy_fd, "%lu Joules", mock->energy_acc);
    rewind(mock->energy_fd); /* Flush buffer and prepare for overwriting next value */
}

/**
 * Initialize this mock module
 *
 * @param   mocks[out]     Mock structure to initialize all mock units
 * @param   dest_dir[in]   Directory contaning the files with the energy counters
 * @param   is_verbose[in] Whether the verbose mode should be enabled
 * @param   n_mocks[in]    Amount of mock units
 * @param   mock_watts[in] Fixed power consumption for each mock unit
 */
void mock_init(Component_t *mocks, const char *dest_dir, const bool is_verbose,
               const uint32_t n_mocks, uint32_t *mock_watts, const uint32_t interval)
{
    memset(mocks, 0, sizeof(Component_t));
    mocks->is_verbose = is_verbose;
    mocks->type = MOCK;
    mocks->fini = mock_fini;
    mocks->update = mock_update;
    mocks->n_siblings = n_mocks;

    if (is_verbose)
        printf("Using %u mock units(s)\n", mocks->n_siblings);

    assert(mocks->n_siblings < N_SIBLINGS_MAX);

    for (uint32_t i = 0; i < mocks->n_siblings; i++) {
        Unit_t *mock = &mocks->siblings[i];
        mock->id = i;
        mock->fixed_watts = mock_watts[i];
        mock->energy_interval = mock_watts[i] * interval;

        char output_path[PATH_MAX];

        /* Opening normalized file (Joules) */
        snprintf(output_path, sizeof(output_path), "%s/mock_%d_energy", dest_dir, mock->id);
        mock->energy_fd = fopen(output_path, "w");
        if (!mock->energy_fd)
        {
            fprintf(stderr, "Failed to open output file: %s\n", output_path);
            exit(EXIT_FAILURE);
        }
    }
}

/**
 * Cleanup the module
 *
 * @param   mocks[in]     Mock structure to clean up
 */
void mock_fini(Component_t *mocks)
{
    for (uint32_t i = 0; i < mocks->n_siblings; ++i)
    {
        Unit_t *package = &mocks->siblings[i];
        fclose(package->energy_fd);
    }
}

/**
 * Compute last energy value for mock and update the destination files
 *
 * @param   mocks[inout] Mock structure
 */
void mock_update(Component_t *mocks)
{
    const bool is_verbose = mocks->is_verbose;

    for (uint32_t i = 0; i < mocks->n_siblings; ++i)
    {
        Unit_t *mock = &mocks->siblings[i];
        _mock_update_files(mock);

        if (is_verbose)
            printf("Mock %u: %lu J (fixed: %u W, accumulator: %lu J)\n",
                   i, mock->energy_interval, mock->fixed_watts, mock->energy_acc);
    }
}

