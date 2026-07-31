/*---------------------------------------------------------*\
| ThermaltakeAIOSensors.h                                     |
|                                                             |
|   CPU/GPU/RAM temperature AND utilization reads for the    |
|   overlay. Linux only for now (hwmon/proc sysfs +          |
|   nvidia-smi) -- no attempt at cross-platform sensor        |
|   access yet.                                              |
|                                                             |
|   This file is part of the OpenRGB project                 |
|   SPDX-License-Identifier: GPL-2.0-or-later                |
\*---------------------------------------------------------*/

#pragma once

struct ThermaltakeAIOSensorReadings
{
    /* Temperatures, degrees C. */
    bool   cpu_ok   = false;
    double cpu_temp_c = 0.0;

    bool   gpu_ok   = false;
    double gpu_temp_c = 0.0;

    bool   ram_ok   = false;
    double ram_temp_c = 0.0;

    /* Utilization, percent 0-100. */
    bool   cpu_util_ok  = false;
    double cpu_util_pct = 0.0;

    bool   gpu_util_ok  = false;
    double gpu_util_pct = 0.0;

    bool   ram_util_ok  = false;
    double ram_util_pct = 0.0;
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

    /*-----------------------------------------------------*\
    | CPU utilization is a delta between successive samples  |
    | of /proc/stat, so the first call after startup has no  |
    | baseline and returns false -- it starts reporting on   |
    | the next read. ReadAll() samples it every cycle        |
    | regardless of the active overlay mode so the delta      |
    | stays continuous even while temperature is showing.     |
    \*-----------------------------------------------------*/
    static bool ReadCPUUtilPct(double* out_util_pct);
    static bool ReadGPUUtilPct(double* out_util_pct);
    static bool ReadRAMUtilPct(double* out_util_pct);
};
