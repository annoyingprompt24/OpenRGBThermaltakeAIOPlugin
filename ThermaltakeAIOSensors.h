/*---------------------------------------------------------*\
| ThermaltakeAIOSensors.h                                     |
|                                                             |
|   CPU/GPU/RAM temperature reads for the overlay. Linux     |
|   only for now (hwmon sysfs + nvidia-smi) -- no attempt at  |
|   cross-platform sensor access yet.                        |
|                                                             |
|   This file is part of the OpenRGB project                 |
|   SPDX-License-Identifier: GPL-2.0-or-later                |
\*---------------------------------------------------------*/

#pragma once

struct ThermaltakeAIOSensorReadings
{
    bool   cpu_ok   = false;
    double cpu_temp_c = 0.0;

    bool   gpu_ok   = false;
    double gpu_temp_c = 0.0;

    bool   ram_ok   = false;
    double ram_temp_c = 0.0;
};

class ThermaltakeAIOSensors
{
public:
    /*-----------------------------------------------------*\
    | Reads whatever sensors are available right now. Each   |
    | read touches sysfs (and shells out to nvidia-smi for   |
    | GPU temp) -- callers should throttle/cache this, don't |
    | call it on every frame-send cycle.                      |
    \*-----------------------------------------------------*/
    static ThermaltakeAIOSensorReadings ReadAll();

private:
    static bool ReadCPUTempC(double* out_temp_c);
    static bool ReadGPUTempC(double* out_temp_c);
    static bool ReadRAMTempC(double* out_temp_c);
};
