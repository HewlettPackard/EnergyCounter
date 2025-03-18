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
* common.h: Shared functions.
*
* URL       https://github.com/HewlettPackard/EnergyCounter
******************************************************************************/

#ifndef COMMON_H
#define COMMON_H

#include <cpuid.h>
#include <stdio.h>

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

#define MSR_PATH_MAX 20
#define MSR_ENERGY_UNIT_MASK     0x1f
#define MSR_AMD_POWER_UNIT       0xc0010299
#define MSR_INTEL_POWER_UNIT     0x606

/**
 * Execute CPUID instruction and read registers to fetch the CPU vendor type
 */
static inline int get_vendor(void)
{
    int info[4] = {0};
    char vendor[13] = {0};

    __cpuid(0, info[0], info[1], info[2], info[3]);
    memcpy(vendor, &info[1], 4);     // EBX
    memcpy(vendor + 4, &info[3], 4); // EDX
    memcpy(vendor + 8, &info[2], 4); // ECX

    if (strcmp(vendor, "GenuineIntel") == 0)
        return INTEL;
    else if (strcmp(vendor, "AuthenticAMD") == 0)
        return AMD;

    return VENDOR_UNKNOWN;
}

/**
 * Read and return the content of a model specific register (MSR) for CPU
 *
 * @param   smt_id[in]  Id of the hardware thread (SMT id)
 * @param   type[in]    MSR type
 */
static inline uint64_t read_msr(const uint32_t smt_id, const uint32_t type)
{
    char file_path[MSR_PATH_MAX];
    uint64_t data;

    snprintf(file_path, MSR_PATH_MAX, "/dev/cpu/%u/msr", smt_id);

    int fd = open(file_path, O_RDONLY);
    if (fd < 0)
    {
        fprintf(stderr, "Unable to open MSR file %s: %s\n", file_path, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (pread(fd, &data, sizeof(data), type) != sizeof(data))
    {
        fprintf(stderr, "Unable to fetch MSR %x in %s\n", type, file_path);
        exit(EXIT_FAILURE);
    }

    close(fd);

    return data;
}

#endif /* COMMON_H */
