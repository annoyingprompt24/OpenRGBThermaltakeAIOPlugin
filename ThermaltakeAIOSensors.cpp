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

ThermaltakeAIOSensorReadings ThermaltakeAIOSensors::ReadAll()
{
    ThermaltakeAIOSensorReadings readings;

    readings.cpu_ok = ReadCPUTempC(&readings.cpu_temp_c);
    readings.gpu_ok = ReadGPUTempC(&readings.gpu_temp_c);
    readings.ram_ok = ReadRAMTempC(&readings.ram_temp_c);

    return(readings);
}
