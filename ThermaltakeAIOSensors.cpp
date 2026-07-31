/*---------------------------------------------------------*\
| ThermaltakeAIOSensors.cpp                                   |
|                                                             |
|   This file is part of the OpenRGB project                 |
|   SPDX-License-Identifier: GPL-2.0-or-later                |
\*---------------------------------------------------------*/

#include "ThermaltakeAIOSensors.h"

#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <cstdio>

static QString HwmonName(const QString& hwmon_path)
{
    QFile name_file(hwmon_path + "/name");
    if(!name_file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return(QString());
    }
    return(QString::fromUtf8(name_file.readAll()).trimmed());
}

static bool ReadMilliDegreesFile(const QString& path, double* out_temp_c)
{
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return(false);
    }

    bool ok = false;
    long millidegrees = QString::fromUtf8(file.readAll()).trimmed().toLong(&ok);
    if(!ok)
    {
        return(false);
    }

    *out_temp_c = millidegrees / 1000.0;
    return(true);
}

bool ThermaltakeAIOSensors::ReadCPUTempC(double* out_temp_c)
{
    QDir hwmon_root("/sys/class/hwmon");
    for(const QString& entry : hwmon_root.entryList(QStringList() << "hwmon*", QDir::Dirs))
    {
        QString hwmon_path = hwmon_root.filePath(entry);
        QString name       = HwmonName(hwmon_path);

        /* Intel: coretemp, "Package id 0". AMD: k10temp/zenpower, "Tctl"/"Tdie". */
        bool is_intel = (name == "coretemp");
        bool is_amd   = (name == "k10temp" || name == "zenpower" || name == "zenpower3");

        if(!is_intel && !is_amd)
        {
            continue;
        }

        QDir hwmon_dir(hwmon_path);
        for(const QString& label_file : hwmon_dir.entryList(QStringList() << "temp*_label", QDir::Files))
        {
            QFile lf(hwmon_dir.filePath(label_file));
            if(!lf.open(QIODevice::ReadOnly | QIODevice::Text))
            {
                continue;
            }
            QString label = QString::fromUtf8(lf.readAll()).trimmed();

            bool label_matches = (is_intel && label == "Package id 0")
                               || (is_amd   && (label == "Tctl" || label == "Tdie"));

            if(!label_matches)
            {
                continue;
            }

            QString input_file = label_file;
            input_file.replace("_label", "_input");

            if(ReadMilliDegreesFile(hwmon_dir.filePath(input_file), out_temp_c))
            {
                return(true);
            }
        }

        /* No matching label found (unusual layout) -- fall back to temp1_input. */
        if(ReadMilliDegreesFile(hwmon_dir.filePath("temp1_input"), out_temp_c))
        {
            return(true);
        }
    }

    return(false);
}

bool ThermaltakeAIOSensors::ReadGPUTempC(double* out_temp_c)
{
    /* Try NVIDIA first via nvidia-smi. */
    FILE* pipe = popen("nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits 2>/dev/null", "r");
    if(pipe != nullptr)
    {
        char buf[64] = { 0 };
        bool have_line = (std::fgets(buf, sizeof(buf), pipe) != nullptr);
        int  status    = pclose(pipe);

        if(have_line && status == 0)
        {
            bool ok = false;
            double value = QString::fromUtf8(buf).trimmed().toDouble(&ok);
            if(ok)
            {
                *out_temp_c = value;
                return(true);
            }
        }
    }

    /* Fall back to amdgpu via hwmon (edge temp). */
    QDir hwmon_root("/sys/class/hwmon");
    for(const QString& entry : hwmon_root.entryList(QStringList() << "hwmon*", QDir::Dirs))
    {
        QString hwmon_path = hwmon_root.filePath(entry);
        if(HwmonName(hwmon_path) != "amdgpu")
        {
            continue;
        }

        if(ReadMilliDegreesFile(hwmon_path + "/temp1_input", out_temp_c))
        {
            return(true);
        }
    }

    return(false);
}

bool ThermaltakeAIOSensors::ReadRAMTempC(double* out_temp_c)
{
    QDir hwmon_root("/sys/class/hwmon");

    double sum   = 0.0;
    int    count = 0;

    for(const QString& entry : hwmon_root.entryList(QStringList() << "hwmon*", QDir::Dirs))
    {
        QString hwmon_path = hwmon_root.filePath(entry);
        QString name       = HwmonName(hwmon_path);

        /* spd5118 = DDR5 SPD temp sensor, jc42 = the older DDR4-era equivalent. */
        if(name != "spd5118" && name != "jc42")
        {
            continue;
        }

        double dimm_temp_c = 0.0;
        if(ReadMilliDegreesFile(hwmon_path + "/temp1_input", &dimm_temp_c))
        {
            sum += dimm_temp_c;
            count++;
        }
    }

    if(count == 0)
    {
        return(false);
    }

    *out_temp_c = sum / count;
    return(true);
}

bool ThermaltakeAIOSensors::ReadCPUUtilPct(double* out_util_pct)
{
    QFile stat_file("/proc/stat");
    if(!stat_file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return(false);
    }

    /* First line is the aggregate: "cpu  user nice system idle iowait irq softirq steal ...". */
    QStringList parts = QString::fromUtf8(stat_file.readLine()).simplified().split(' ');
    if(parts.size() < 9 || parts[0] != "cpu")
    {
        return(false);
    }

    long long user    = parts[1].toLongLong();
    long long nice    = parts[2].toLongLong();
    long long system  = parts[3].toLongLong();
    long long idle    = parts[4].toLongLong();
    long long iowait  = parts[5].toLongLong();
    long long irq     = parts[6].toLongLong();
    long long softirq = parts[7].toLongLong();
    long long steal   = parts[8].toLongLong();

    long long idle_all = idle + iowait;
    long long total    = idle_all + user + nice + system + irq + softirq + steal;

    /*-----------------------------------------------------*\
    | Utilization is the busy fraction over the interval    |
    | between this sample and the previous one, so the very |
    | first call has no baseline and reports nothing.        |
    \*-----------------------------------------------------*/
    static long long prev_total = 0;
    static long long prev_idle  = 0;
    static bool      primed     = false;

    long long dtotal = total - prev_total;
    long long didle  = idle_all - prev_idle;
    prev_total = total;
    prev_idle  = idle_all;

    if(!primed)
    {
        primed = true;
        return(false);
    }
    if(dtotal <= 0)
    {
        return(false);
    }

    *out_util_pct = 100.0 * (double)(dtotal - didle) / (double)dtotal;
    return(true);
}

bool ThermaltakeAIOSensors::ReadGPUUtilPct(double* out_util_pct)
{
    /* Try NVIDIA first via nvidia-smi. */
    FILE* pipe = popen("nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits 2>/dev/null", "r");
    if(pipe != nullptr)
    {
        char buf[64] = { 0 };
        bool have_line = (std::fgets(buf, sizeof(buf), pipe) != nullptr);
        int  status    = pclose(pipe);

        if(have_line && status == 0)
        {
            bool ok = false;
            double value = QString::fromUtf8(buf).trimmed().toDouble(&ok);
            if(ok)
            {
                *out_util_pct = value;
                return(true);
            }
        }
    }

    /* Fall back to amdgpu via the hwmon device's gpu_busy_percent. */
    QDir hwmon_root("/sys/class/hwmon");
    for(const QString& entry : hwmon_root.entryList(QStringList() << "hwmon*", QDir::Dirs))
    {
        QString hwmon_path = hwmon_root.filePath(entry);
        if(HwmonName(hwmon_path) != "amdgpu")
        {
            continue;
        }

        QFile busy_file(hwmon_path + "/device/gpu_busy_percent");
        if(busy_file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            bool ok = false;
            double value = QString::fromUtf8(busy_file.readAll()).trimmed().toDouble(&ok);
            if(ok)
            {
                *out_util_pct = value;
                return(true);
            }
        }
    }

    return(false);
}

bool ThermaltakeAIOSensors::ReadRAMUtilPct(double* out_util_pct)
{
    QFile mem_file("/proc/meminfo");
    if(!mem_file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return(false);
    }

    long long total_kb = -1;
    long long avail_kb = -1;

    /*-----------------------------------------------------*\
    | Read the whole file at once rather than looping on     |
    | atEnd() -- /proc files report a size of 0, so QFile::   |
    | atEnd() is immediately true and a read loop never runs. |
    \*-----------------------------------------------------*/
    const QStringList lines = QString::fromUtf8(mem_file.readAll()).split('\n');
    for(const QString& line : lines)
    {
        if(line.startsWith("MemTotal:"))
        {
            total_kb = line.split(' ', Qt::SkipEmptyParts).value(1).toLongLong();
        }
        else if(line.startsWith("MemAvailable:"))
        {
            avail_kb = line.split(' ', Qt::SkipEmptyParts).value(1).toLongLong();
        }
    }

    if(total_kb <= 0 || avail_kb < 0)
    {
        return(false);
    }

    *out_util_pct = 100.0 * (double)(total_kb - avail_kb) / (double)total_kb;
    return(true);
}

ThermaltakeAIOSensorReadings ThermaltakeAIOSensors::ReadAll()
{
    ThermaltakeAIOSensorReadings readings;

    readings.cpu_ok = ReadCPUTempC(&readings.cpu_temp_c);
    readings.gpu_ok = ReadGPUTempC(&readings.gpu_temp_c);
    readings.ram_ok = ReadRAMTempC(&readings.ram_temp_c);

    readings.cpu_util_ok = ReadCPUUtilPct(&readings.cpu_util_pct);
    readings.gpu_util_ok = ReadGPUUtilPct(&readings.gpu_util_pct);
    readings.ram_util_ok = ReadRAMUtilPct(&readings.ram_util_pct);

    return(readings);
}
