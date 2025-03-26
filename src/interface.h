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
* interface.h: Interface for all component types.
*
* URL       https://github.com/HewlettPackard/EnergyCounter
******************************************************************************/

#ifndef INTERFACE_H
#define INTERFACE_H

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>

#define N_SIBLINGS_MAX 16

enum interface {
    AMD_GPUS,
    INTEL_GPUS,
    NVIDIA_GPUS,
    CPUS,
    DRAMS,
    MOCKS,
    INTERFACES_MAX
};

enum vendor {
    AMD,
    INTEL,
    NVIDIA,
    VENDOR_UNKNOWN
};

static const char * const vendor_str[] =
{
    [AMD]     = "AMD",
    [INTEL]   = "INTEL",
    [NVIDIA]  = "NVIDIA",
    [VENDOR_UNKNOWN] = "unknown",
};

enum type {
    CPU,
    GPU,
    DRAM,
    MOCK,
    TYPE_UNKNOWN
};

static const char * const type_str[] =
{
    [CPU]     = "CPU",
    [GPU]     = "GPU",
    [DRAM]    = "DRAM",
    [MOCK]    = "MOCK",
    [TYPE_UNKNOWN] = "unknown",
};

typedef struct Unit
{
    uint64_t     timestamp;
    uint64_t     bus_id;
    double       energy_resolution;
    uint64_t     energy_raw;
    uint64_t     energy_acc;           /* Energy accumulator in Joules */
    uint64_t     energy_interval;      /* Energy during last interval in Joules */
    FILE        *energy_fd;
    uint32_t     id;
    uint32_t     model;
    uint32_t     busy_percent;
    uint32_t     fixed_watts;
    char         serial[64];
    struct Unit *peer;
} Unit_t;

typedef struct Component
{
    Unit_t    siblings[N_SIBLINGS_MAX];
    int       type;
    int       vendor;
    uint32_t  n_siblings;
    bool      is_verbose;
    void      (*fini)(struct Component*);
    void      (*update)(struct Component*);
} Component_t;

#endif /* INTERFACE_H */

