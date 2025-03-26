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
*
* URL       https://github.com/HewlettPackard/EnergyCounter
******************************************************************************/

#include <argp.h>
#include <dirent.h>
#include <linux/limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <signal.h>
#include "interface.h"

/* Expand macro values to string */
#define STR_VALUE(var)  #var
#define STR(var)        STR_VALUE(var)

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

#define VERSION  "0.1"
#define CONTACT  "https://github.com/HewlettPackard/EnergyCounter"

#define INTERVAL_DEFAULT  10              /* Default interval in seconds before next collection */
#define DIR_PATH_DEFAULT  "/tmp/ecounter" /* Default directory path to store the counters       */

#define ARG_CPU        0x200
#define ARG_DRAM       0x300
#define ARG_GPU_AMD    0x400
#define ARG_GPU_INTEL  0x500
#define ARG_GPU_NVIDIA 0x600

extern void cpu_init(Component_t *, const char * dir_path, const bool is_verbose, const bool is_disabled);
extern void dram_init(Component_t *, const char * dir_path, const bool is_verbose, const bool is_disabled);
extern void amd_gpu_init(Component_t *, const char *dir_path, const bool is_verbose, const bool is_disabled);
extern void intel_gpu_init(Component_t *, const char *dir_path, const bool is_verbose, const bool is_disabled);
extern void nvidia_gpu_init(Component_t *, const char *dir_path, const bool is_verbose, const bool is_disabled);
extern void mock_init(Component_t *, const char *dir_path, const bool is_verbose,
                      const uint32_t n_mocks, uint32_t *mock_watts, const uint32_t interval);

typedef struct Overhead
{
    uint32_t min;
    uint32_t max;
    uint32_t mov_average;
    uint32_t n_samples;
} Overhead_t;

typedef struct Ecounter
{
    Component_t  components[INTERFACES_MAX];  /* Structure for all components               */
    bool         is_disabled[INTERFACES_MAX]; /* Defines if the component is disabled       */
    uint32_t     interval;                    /* Interval in seconds before next collection */
    bool         is_verbose;                  /* Defines if verbose mod is enabled          */
    uint32_t     n_mocks;                     /* Amount of mock units                       */
    uint32_t     mock_watts[N_SIBLINGS_MAX];  /* All fixed power consumptions for mocks     */
    char         dir_path[PATH_MAX];          /* Directory path to store the counters       */
    char         power_cmd[PATH_MAX];         /* Command to fetch instantaneous node power  */
    Overhead_t   overhead;                    /* Evaluation of power overhead               */
} Ecounter_t;

Ecounter_t ec_g;

const char *argp_program_version = VERSION;
const char *argp_program_bug_address = CONTACT;

/* Program documentation */
static char doc[] = "This application is designed to periodically fetch energy counters "
                    "and expose the value of each counter in a file. AMD and Intel CPUs, "
                    "DRAM (Intel CPUs only), AMD (MI) and NVIDIA (starting from Volta) GPUs "
                    "are supported. The application accepts the following arguments:";

/* A description of the arguments we accept (in addition to the options) */
static char args_doc[] = "--dir=<path> --interval=<seconds>";

/* Options */
static struct argp_option options[] =
{
    {"dir",           'd', "<path>",          0, "Directory path where the files are stored. "
                                                 "Should be in a tmpfs or ramfs mount point "
                                                 "to avoid wearing out a storage device [default: "
                                                 STR(DIR_PATH_DEFAULT) "]"},
#ifdef CPU_PACKAGE
    {"disable-cpu",  ARG_CPU,              0, 0, "Disable CPU energy support"},
#endif /* CPU_PACKAGE */
#ifdef DRAM_PACKAGE
    {"disable-dram", ARG_DRAM,             0, 0, "Disable DRAM energy support"},
#endif /* DRAM_PACKAGE */
#ifdef AMD_GPU
    {"disable-gpu-amd", ARG_GPU_AMD,       0, 0, "Disable AMD GPU energy support"},
#endif /* AMD_GPU */
#ifdef INTEL_GPU
    {"disable-gpu-intel", ARG_GPU_INTEL,   0, 0, "Disable Intel GPU support"},
#endif /* INTEL_GPU */
#ifdef NVIDIA_GPU
    {"disable-gpu-nvidia", ARG_GPU_NVIDIA, 0, 0, "Disable NVIDIA GPU support"},
#endif /* NVIDA_GPU */
    {"interval",      'i', "<seconds>",       0, "Specify the intertval time in seconds "
                                                 "before collecting new values [default: "
                                                 STR(INTERVAL_DEFAULT) "s]"},
    {"mock",          'm', "<watts>",         0, "Add a mock energy counter based on a fixed power "
                                                 "consumption budget defined in watts. Multiple mock "
                                                 "counters can be created by repeating this option"},
    {"find-overhead", 'o', "<cmd>",           0, "Mode to find the power overhead. This option takes "
                                                 "a bash command or script as argument which should "
                                                 "return the instantaneous power consumption of the node"},
    {"verbose",       'v',  0,                0, "Enable verbosity"},
    {0}
};

/* Parse a single option */
static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    Ecounter_t *ec = (Ecounter_t *)state->input;

    switch (key)
    {
        case ARG_CPU:
            ec->is_disabled[CPUS] = true;
            break;
        case ARG_DRAM:
            ec->is_disabled[DRAMS] = true;
            break;
        case ARG_GPU_AMD:
            ec->is_disabled[AMD_GPUS] = true;
            break;
        case ARG_GPU_INTEL:
            ec->is_disabled[INTEL_GPUS] = true;
            break;
        case ARG_GPU_NVIDIA:
            ec->is_disabled[NVIDIA_GPUS] = true;
            break;
        case 'd':
            strncpy(ec->dir_path, arg, PATH_MAX - 1);
            break;
        case 'i':
            ec->interval = strtol(arg, NULL, 10);
            if (errno == EINVAL || errno == ERANGE || ec->interval < 0)
            {
                fprintf(stderr, "Error: cannot parse the amount of seconds from the "
                                "--interval argument (%s). Exit.\n", arg);
                exit(EXIT_FAILURE);
            }
            break;
        case 'm':
            ec->mock_watts[ec->n_mocks] = strtol(arg, NULL, 10);
            if (errno == EINVAL || errno == ERANGE || ec->mock_watts[ec->n_mocks] < 0)
            {
                fprintf(stderr, "Error: cannot parse the amount of watts from the "
                                "mock argument (%s). Exit.\n", arg);
                exit(EXIT_FAILURE);
            }
            ec->n_mocks++;
            break;
        case 'o':
            strncpy(ec->power_cmd, arg, PATH_MAX - 1);
            break;
        case 'v':
            ec->is_verbose = true;
            break;
        case ARGP_KEY_END:
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }

    return 0;
}

/* Argp parser */
static struct argp argp = { options, parse_opt, args_doc, doc };

/**
 * Run the node power command and check the output
 *
 * @param   ec[in]     Main application structure
 */
uint32_t fetch_node_power(Ecounter_t *ec)
{
    FILE *fp;
    char output[128];
    int64_t power;

    /* Open the command for reading. */
    fp = popen(ec->power_cmd, "r");
    if (fp == NULL)
    {
        fprintf(stderr, "Error: failed to run command (%s). Exit\n",
                ec->power_cmd);
        exit(EXIT_FAILURE);
    }

    /* Read the output a line at a time - output it. */
    if (fgets(output, sizeof(output), fp) == NULL)
    {
        fprintf(stderr, "Error: command (%s) does not return any output. Exit\n",
                ec->power_cmd);
        exit(EXIT_FAILURE);
    }

    /* Ensure the value is a positive integer */
    power = strtol(output, NULL, 10);
    if (errno == EINVAL || errno == ERANGE || power <= 0)
    {
        fprintf(stderr, "Error: command (%s) returns an invalid value: %s. Exit\n",
                ec->power_cmd, output);
        exit(EXIT_FAILURE);
    }

    pclose(fp);

    return (uint32_t) power;
}

/**
 * Compute the power overhead over the last interval
 *
 * @param   ec[in]     Main application structure
 */
void compute_overhead(Ecounter_t *ec)
{
    Overhead_t *overhead = &ec->overhead;
    const uint32_t node_power = fetch_node_power(ec);
    uint32_t energy_interval = 0;

    for (uint32_t i = 0; i < INTERFACES_MAX; i++)
    {
        Component_t *component = &ec->components[i];

        for (uint32_t j = 0; j < component->n_siblings; j++)
            energy_interval += component->siblings[j].energy_interval;
    }

    const uint32_t power_interval = energy_interval / ec->interval;
    const uint32_t overhead_interval = (power_interval < node_power) ?
                                       node_power - power_interval : 0;

    /* Skip a null power interval  */
    if (power_interval == 0)
        return;

    overhead->min = MIN(overhead_interval, overhead->min);
    overhead->max = MAX(overhead_interval, overhead->max);
    overhead->mov_average = (overhead->mov_average * overhead->n_samples +
                            overhead_interval) / (overhead->n_samples + 1);
    overhead->n_samples++;

    printf("Node instant. power: %u W\n", node_power);
    printf("Power overhead - min: %u W, max: %u W, avg: %u W\n",
            overhead->min, overhead->max, overhead->mov_average);
}

/**
 * Initialize the application
 *
 * @param   argc[in]    Amount of arguments
 * @param   argv[in]    Array of arguments
 * @param   ec[out]     Main application structure
 */
void init(int argc, char *argv[], Ecounter_t *ec)
{
    memset(ec, 0, sizeof(Ecounter_t));

    /* Set defaults */
    ec->interval = INTERVAL_DEFAULT;
    strncpy(ec->dir_path, DIR_PATH_DEFAULT, PATH_MAX - 1);

    argp_parse(&argp, argc, argv, 0, 0, ec);

    /* Check the destination directory exists and can be accessed */
    DIR *dir = opendir(ec->dir_path);
    if (!dir)
    {
        fprintf(stderr, "Error: unable to open %s directory (%s). Exit\n",
                ec->dir_path, strerror(errno));
        exit(EXIT_FAILURE);
    }
    else
        closedir(dir);

    ec->overhead.min = INT_MAX;

    amd_gpu_init(&ec->components[AMD_GPUS], ec->dir_path, ec->is_verbose, ec->is_disabled[AMD_GPUS]);
    intel_gpu_init(&ec->components[INTEL_GPUS], ec->dir_path, ec->is_verbose, ec->is_disabled[INTEL_GPUS]);
    nvidia_gpu_init(&ec->components[NVIDIA_GPUS], ec->dir_path, ec->is_verbose, ec->is_disabled[NVIDIA_GPUS]);
    cpu_init(&ec->components[CPUS], ec->dir_path, ec->is_verbose, ec->is_disabled[CPUS]);
    dram_init(&ec->components[DRAMS], ec->dir_path, ec->is_verbose, ec->is_disabled[DRAMS]);
    mock_init(&ec->components[MOCKS], ec->dir_path, ec->is_verbose,
              ec->n_mocks, ec->mock_watts, ec->interval);
}

/**
 * Finalize the application
 *
 * @param   ec[in/out]     Main application structure
 */
void fini(Ecounter_t *ec)
{
    for (int i = 0; i < INTERFACES_MAX; i++)
        ec->components[i].fini(&ec->components[i]);
}

/**
 * Catching SIGTERM for a graceful shutdown
 *
 * @param   signum[in]       Signal type
 */
static void sigterm_handler(int signum)
{
    printf("Stopping ecounter\n");
    fini(&ec_g);
    exit(0);
}

int main(int argc, char *argv[])
{
    init(argc, argv, &ec_g);
    signal(SIGTERM, sigterm_handler);

    const bool is_verbose = ec_g.is_verbose;

    printf("Starting ecounter -- Directory path: %s -- Interval: %u\n",
           ec_g.dir_path, ec_g.interval);

    while(true)
    {
        for (int i = 0; i < INTERFACES_MAX; i++)
            ec_g.components[i].update(&ec_g.components[i]);

        if (strlen(ec_g.power_cmd) > 0)
            compute_overhead(&ec_g);

        if (is_verbose)
            printf("------------------------------ [Next data collection in %us]\n", ec_g.interval);

        sleep(ec_g.interval);
    }

    return 0;
}
