EnergyCounter
=============

This application is designed to periodically fetch hardware energy counters,
standardize (no wraparound) and expose the accumulated value of each counter
(in joules) in a file. Ressources are automatically detected.
The following hardware devices are supported:

* **AMD Instinct GPUs** (starting from MI200 generation)
* **Intel Max GPUs** (starting from Ponte Vecchio generation)
* **NVIDIA Tesla GPUs** (starting from Volta generation)
* **AMD CPUs** (Starting from Ryzen)
* **Intel CPUs** (Starting from Sandy Bridge?)
* **DRAM (Intel CPUs only)** (Starting from Sandy Bridge?)

The main goal is to create a lightweight daemon to collect and expose energy
metrics which could used by other tools. The novelty of this approach relies
on the fact that all metrics are energy counter based (best precision, very
small overhead). There is no software sampling (no power metric is collected).

> EnergyCounter will eventually expose most of the counters (except node level energy) exposed by the Cray's
> PM Counters. While EnergyCounter won't be necessary on Cray EX platforms, it could bring similar capabilities to all the other types of nodes
> (SGI 8600, Apollo 6500, Cray XD2000, etc.).


Dependencies
------------

* **AMD ROCm** (AMD GPUs)
* **Intel oneAPI Level Zero** (Intel GPUs)
* **NVIDIA DCGM** (NVIDIA GPUs)


How to build EnergyCounter
--------------------------

Ecounter uses CMake with a configure wrapper.

    % ./configure
    % make
    % sudo make install

By default the installation directory is /opt/ecounter. Use ./configure --prefix=path to define a new destination directory. Modules can also be disabled (check ./configure --help).


How to run EnergyCounter in standalone mode
-------------------------------------------

    % ./ecounter [ARGS...]

Arguments are :

        --disable-cpu          Disable CPU energy support
        --disable-dram         Disable DRAM energy support
        --disable-gpu-amd      Disable AMD GPU energy support
        --disable-gpu-intel    Disable Intel GPU energy support
        --disable-gpu-nvidia   Disable NVIDIA GPU energy support
    -d, --dir=<path>           Directory path where the files are stored. Should
                               be in a tmpfs or ramfs mount point to avoid
                               wearing out a storage device [default:
                               "/tmp/ecounter"]
    -i, --interval=<seconds>   Specify the intertval time in seconds before
                               collecting new values [default: 10s]
    -m, --mock=<watts>         Add a mock energy counter based on a fixed power
                               consumption budget defined in watts. Multiple mock
                               counters can be created by repeating this option
    -o, --find-overhead=<cmd>  Mode to find the power overhead. This option takes
                               a bash command or script as argument which should
                               return the instantaneous power consumption of the
                               node
    -v, --verbose              Enable verbosity
    -?, --help                 Give this help list
        --usage                Give a short usage message
    -V, --version              Print program version


How to use the find-overhead mode
---------------------------------

This mode can be used to determine the sum of the power overheads which are not
changing a lot (power supply, RAM, network adapters, PCIe switches, etc.).

First launch a workload like HPL or DGEMMs on the CPUs/GPUs.

The node should offer a way to retrieve the instantaneous power consumption for
the whole node.


Nodes with DCMI support with IPMI:

    $ ./ecounter -o "ipmitool dcmi power reading | sed -rn 's/.*power reading:.* ([0-9]+) .*/\1/p'"

Cray nodes with PM counters:

    % ./ecounter -o "sed -rn 's/([0-9]+) .*/\1/p' /sys/cray/pm_counters/power"


Then this mode would deduce the measured values from the node power consumption.


How to run EnergyCounter as a systemd service
---------------------------------------------

    % sudo cp systemd/ecounter.service /etc/systemd/system/
    % sudo systemctl daemon-reload
    % sudo systemctl start ecounter.service

The service creates a /energy tmpfs and starts ecounter.


How to generate mock units
--------------------------

Ecounter may generate mock units based on fixed power consumption values (in watts).

    % ./ecounter --mock=200 --mock=50 --mock=500 --verbose
    Using 3 mock units(s)
    Starting ecounter -- Directory path: /tmp/ecounter -- Interval: 10
    Mock 0: 2000 J (fixed: 200 W, accumulator: 2000 J)
    Mock 1: 500 J (fixed: 50 W, accumulator: 500 J)
    Mock 2: 5000 J (fixed: 500 W, accumulator: 5000 J)
    ------------------------------ [Next data collection in 10s]
    Mock 0: 2000 J (fixed: 200 W, accumulator: 4000 J)
    Mock 1: 500 J (fixed: 50 W, accumulator: 1000 J)
    Mock 2: 5000 J (fixed: 500 W, accumulator: 10000 J)
    ------------------------------ [Next data collection in 10s]


Results with 5x NVIDIA GPUs (H100)
----------------------------------

    % ./ecounter --verbose
    5 NVIDIA GPU devices found
    Starting ecounter -- Directory path: /tmp/ecounter -- Interval: 10
    Nvidia GPU 0 (0x88): 0 Joules (raw: 10691105633)
    Nvidia GPU 1 (0x8c): 0 Joules (raw: 11625135207)
    Nvidia GPU 2 (0xc8): 0 Joules (raw: 11730525441)
    Nvidia GPU 3 (0xcc): 0 Joules (raw: 12503533899)
    Nvidia GPU 4 (0xcd): 0 Joules (raw: 10970219536)
    ------------------------------ [Next data collection in 10s]
    Nvidia GPU 0 (0x88): 441 Joules (raw: 10691546792)
    Nvidia GPU 1 (0x8c): 478 Joules (raw: 11625613786)
    Nvidia GPU 2 (0xc8): 487 Joules (raw: 11731013257)
    Nvidia GPU 3 (0xcc): 516 Joules (raw: 12504050251)
    Nvidia GPU 4 (0xcd): 451 Joules (raw: 10970670780)
    ------------------------------ [Next data collection in 10s]
    Nvidia GPU 0 (0x88): 886 Joules (raw: 10691992348)
    Nvidia GPU 1 (0x8c): 956 Joules (raw: 11626092643)
    Nvidia GPU 2 (0xc8): 970 Joules (raw: 11731496517)
    Nvidia GPU 3 (0xcc): 1032 Joules (raw: 12504566516)
    Nvidia GPU 4 (0xcd): 907 Joules (raw: 10971127135)


Results with 4x AMD Instinct GPUs (MI250X)
------------------------------------------

> The MI250(X) accelerator is composed of 2 chips aka Graphics Compute Die (GCD).
> Each GCD can be compared as a single GPU. By default the first GCD of each MI250(X)
> gets the consumption for the whole accelerator. In other words, only half of the
> GCDs are associated with an energy counter.

EnergyCounter applies a mathematical model to split the consumption across GCDs on
the same accelerator. The model is the following:

1) Subtract the idle consumption (GCD overhead = 40W) from the measured one.
2) A formula computes the share of each GCD based on the activity:
> energy_ratio_gcd0 = (0.005 * gcd0_busy_percent) - (0.005 * gcd1_busy_percent) + 0.5;
3) The overhead and active share is added the the final energy counters.


Output is the following (all GCDs loaded with DGEMM):

    % ./ecounter --verbose
    8 AMD GPU devices found
    AMD MI250 found, enabling model to split energy consumption accross GCDs
    AMD GCD 0 and 1 share the same board
    AMD MI250 found, enabling model to split energy consumption accross GCDs
    AMD MI250 found, enabling model to split energy consumption accross GCDs
    AMD GCD 2 and 3 share the same board
    AMD MI250 found, enabling model to split energy consumption accross GCDs
    AMD MI250 found, enabling model to split energy consumption accross GCDs
    AMD GCD 4 and 5 share the same board
    AMD MI250 found, enabling model to split energy consumption accross GCDs
    AMD MI250 found, enabling model to split energy consumption accross GCDs
    AMD GCD 6 and 7 share the same board
    AMD MI250 found, enabling model to split energy consumption accross GCDs
    Starting ecounter -- Directory path: /tmp/ecounter -- Interval: 10
    AMD GPU 0 (0xc1): 0 Joules (raw: 452449973086)
    AMD GPU 1 (0xc6): 0 Joules (raw: 0)
    AMD GPU 2 (0xc9): 0 Joules (raw: 423076816461)
    AMD GPU 3 (0xce): 0 Joules (raw: 0)
    AMD GPU 4 (0xd1): 0 Joules (raw: 426582043834)
    AMD GPU 5 (0xd6): 0 Joules (raw: 0)
    AMD GPU 6 (0xd9): 0 Joules (raw: 420772385401)
    AMD GPU 7 (0xde): 0 Joules (raw: 0)
    ------------------------------ [Next data collection in 10s]
    AMD GPU 0 (0xc1): 2717 Joules (raw: 452805166480)
    AMD GPU 1 (0xc6): 2717 Joules (raw: 0)
    AMD GPU 2 (0xc9): 2675 Joules (raw: 423426502471)
    AMD GPU 3 (0xce): 2675 Joules (raw: 0)
    AMD GPU 4 (0xd1): 2755 Joules (raw: 426942270508)
    AMD GPU 5 (0xd6): 2755 Joules (raw: 0)
    AMD GPU 6 (0xd9): 2776 Joules (raw: 421135337975)
    AMD GPU 7 (0xde): 2776 Joules (raw: 0)
    ------------------------------ [Next data collection in 10s]
    AMD GPU 0 (0xc1): 5438 Joules (raw: 453160892034)
    AMD GPU 1 (0xc6): 5438 Joules (raw: 0)
    AMD GPU 2 (0xc9): 5353 Joules (raw: 423776666080)
    AMD GPU 3 (0xce): 5353 Joules (raw: 0)
    AMD GPU 4 (0xd1): 5512 Joules (raw: 427302740720)
    AMD GPU 5 (0xd6): 5512 Joules (raw: 0)
    AMD GPU 6 (0xd9): 5553 Joules (raw: 421498460919)
    AMD GPU 7 (0xde): 5553 Joules (raw: 0)


Results with 2x AMD EPYC Milan CPUs and 5x NVIDIA GPUs (H100)
-------------------------------------------------------------

    % ./ecounter --verbose
    5 NVIDIA GPU devices found
    AMD CPU(s) found with 2 package(s)
    Starting ecounter -- Directory path: /tmp/ecounter -- Interval: 10
    Nvidia GPU 0 (0x88): 0 Joules (raw: 35212379004)
    Nvidia GPU 1 (0x8c): 0 Joules (raw: 38257028391)
    Nvidia GPU 2 (0xc8): 0 Joules (raw: 38590724770)
    Nvidia GPU 3 (0xcc): 0 Joules (raw: 41128151749)
    Nvidia GPU 4 (0xcd): 0 Joules (raw: 36092577627)
    AMD CPU package 0: 1 Joules (raw: 2051602531)
    AMD CPU package 1: 2 Joules (raw: 798227070)
    ------------------------------ [Next data collection in 10s]
    Nvidia GPU 0 (0x88): 443 Joules (raw: 35212822490)
    Nvidia GPU 1 (0x8c): 482 Joules (raw: 38257510972)
    Nvidia GPU 2 (0xc8): 485 Joules (raw: 38591210569)
    Nvidia GPU 3 (0xcc): 517 Joules (raw: 41128668940)
    Nvidia GPU 4 (0xcd): 452 Joules (raw: 36093030567)
    AMD CPU package 0: 1087 Joules (raw: 2122780654)
    AMD CPU package 1: 1136 Joules (raw: 872606829)
    ------------------------------ [Next data collection in 10s]
    Nvidia GPU 0 (0x88): 886 Joules (raw: 35213266031)
    Nvidia GPU 1 (0x8c): 964 Joules (raw: 38257993899)
    Nvidia GPU 2 (0xc8): 970 Joules (raw: 38591696266)
    Nvidia GPU 3 (0xcc): 1039 Joules (raw: 41129191661)
    Nvidia GPU 4 (0xcd): 905 Joules (raw: 36093484054)
    AMD CPU package 0: 2173 Joules (raw: 2194012159)
    AMD CPU package 1: 2271 Joules (raw: 947045390)


    % ls /tmp/ecounter/
    cpu_package_0_energy  cpu_package_1_energy
    gpu_88_energy  gpu_8c_energy  gpu_c8_energy  gpu_cc_energy  gpu_cd_energy


Results with 6x Intel Max GPUs (1550)
-------------------------------------

> The Max 1550 accelerator is composed of 2 tiles on the same package.
> Each tile can be compared as a single GPU. By default each tile reports
> the consumption for the whole accelerator.

EnergyCounter splits the energy consuption in a 50/50 fashion across the tiles.
Output is the following (GPUs are idle):

    %ecounter --verbose
    12 Intel GPU devices found
    Intel Max 1550 found, enabling split (50/50) energy consumption across tiles
    Intel GPU 0 and 1 share the same board
    Intel Max 1550 found, enabling split (50/50) energy consumption across tiles
    Intel Max 1550 found, enabling split (50/50) energy consumption across tiles
    Intel GPU 2 and 3 share the same board
    Intel Max 1550 found, enabling split (50/50) energy consumption across tiles
    Intel Max 1550 found, enabling split (50/50) energy consumption across tiles
    Intel GPU 4 and 5 share the same board
    Intel Max 1550 found, enabling split (50/50) energy consumption across tiles
    Intel Max 1550 found, enabling split (50/50) energy consumption across tiles
    Intel GPU 6 and 7 share the same board
    Intel Max 1550 found, enabling split (50/50) energy consumption across tiles
    Intel Max 1550 found, enabling split (50/50) energy consumption across tiles
    Intel GPU 8 and 9 share the same board
    Intel Max 1550 found, enabling split (50/50) energy consumption across tiles
    Intel Max 1550 found, enabling split (50/50) energy consumption across tiles
    Intel GPU 10 and 11 share the same board
    Intel Max 1550 found, enabling split (50/50) energy consumption across tiles
    Starting ecounter -- Directory path: /tmp/ecounter -- Interval: 10
    Intel GPU 0 (0x18): 0 J (accumulator: 0 J, raw: 969259265502)
    Intel GPU 1 (0x18): 0 J (accumulator: 0 J, raw: 969259265502)
    Intel GPU 2 (0x42): 0 J (accumulator: 0 J, raw: 1099592646545)
    Intel GPU 3 (0x42): 0 J (accumulator: 0 J, raw: 1099592646545)
    Intel GPU 4 (0x6c): 0 J (accumulator: 0 J, raw: 1125002989624)
    Intel GPU 5 (0x6c): 0 J (accumulator: 0 J, raw: 1125002989624)
    Intel GPU 6 (0x18): 0 J (accumulator: 0 J, raw: 1186158357666)
    Intel GPU 7 (0x18): 0 J (accumulator: 0 J, raw: 1186158357666)
    Intel GPU 8 (0x42): 0 J (accumulator: 0 J, raw: 1148474365905)
    Intel GPU 9 (0x42): 0 J (accumulator: 0 J, raw: 1148474365905)
    Intel GPU 10 (0x6c): 0 J (accumulator: 0 J, raw: 1004817547973)
    Intel GPU 11 (0x6c): 0 J (accumulator: 0 J, raw: 1004817547973)
    ------------------------------ [Next data collection in 10s]
    Intel GPU 0 (0x18): 1110 J (accumulator: 1110 J, raw: 971481203125)
    Intel GPU 1 (0x18): 1110 J (accumulator: 1110 J, raw: 971481203125)
    Intel GPU 2 (0x42): 1092 J (accumulator: 1092 J, raw: 1101778004150)
    Intel GPU 3 (0x42): 1092 J (accumulator: 1092 J, raw: 1101778004150)
    Intel GPU 4 (0x6c): 1107 J (accumulator: 1107 J, raw: 1127217792053)
    Intel GPU 5 (0x6c): 1107 J (accumulator: 1107 J, raw: 1127217792053)
    Intel GPU 6 (0x18): 1150 J (accumulator: 1150 J, raw: 1188458907165)
    Intel GPU 7 (0x18): 1150 J (accumulator: 1150 J, raw: 1188458907165)
    Intel GPU 8 (0x42): 1152 J (accumulator: 1152 J, raw: 1150779471069)
    Intel GPU 9 (0x42): 1152 J (accumulator: 1152 J, raw: 1150779471069)
    Intel GPU 10 (0x6c): 1119 J (accumulator: 1119 J, raw: 1007055587219)
    Intel GPU 11 (0x6c): 1119 J (accumulator: 1119 J, raw: 1007055587219)
    ------------------------------ [Next data collection in 10s]
    Intel GPU 0 (0x18): 1046 J (accumulator: 2156 J, raw: 973575121582)
    Intel GPU 1 (0x18): 1046 J (accumulator: 2156 J, raw: 973575121582)
    Intel GPU 2 (0x42): 1031 J (accumulator: 2123 J, raw: 1103841433105)
    Intel GPU 3 (0x42): 1031 J (accumulator: 2123 J, raw: 1103841433105)
    Intel GPU 4 (0x6c): 1042 J (accumulator: 2149 J, raw: 1129302330993)
    Intel GPU 5 (0x6c): 1042 J (accumulator: 2149 J, raw: 1129302330993)
    Intel GPU 6 (0x18): 1089 J (accumulator: 2239 J, raw: 1190636917968)
    Intel GPU 7 (0x18): 1089 J (accumulator: 2239 J, raw: 1190636917968)
    Intel GPU 8 (0x42): 1089 J (accumulator: 2241 J, raw: 1152958808532)
    Intel GPU 9 (0x42): 1089 J (accumulator: 2241 J, raw: 1152958808532)
    Intel GPU 10 (0x6c): 1052 J (accumulator: 2171 J, raw: 1009161289978)
    Intel GPU 11 (0x6c): 1052 J (accumulator: 2171 J, raw: 1009161289978)


    %ls /tmp/ecounter/
    gpu_18_0_energy  gpu_18_6_energy   gpu_42_2_energy
    gpu_42_8_energy  gpu_6c_10_energy  gpu_6c_4_energy
    gpu_18_1_energy  gpu_18_7_energy   gpu_42_3_energy
    gpu_42_9_energy  gpu_6c_11_energy  gpu_6c_5_energy


Future Work
-----------

- [ ] Add better energy split model on Intel's Ponte Vecchio GPUs with 2 tiles (Max 1550)
