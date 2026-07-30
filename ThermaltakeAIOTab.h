/*---------------------------------------------------------*\
| ThermaltakeAIOTab.h                                        |
|                                                             |
|   This file is part of the OpenRGB project                 |
|   SPDX-License-Identifier: GPL-2.0-or-later                |
\*---------------------------------------------------------*/

#pragma once

#include <QWidget>
#include "ThermaltakeAIODevice.h"

namespace Ui
{
    class ThermaltakeAIOTab;
}

class ThermaltakeAIOTab : public QWidget
{
    Q_OBJECT

public:
    explicit ThermaltakeAIOTab(QWidget* parent = nullptr);
    ~ThermaltakeAIOTab();

private slots:
    void on_choose_image_button_clicked();
    void on_start_stop_button_clicked();
    void on_sensor_overlay_checkbox_toggled(bool checked);
    void RefreshConnectionStatus();

private:
    Ui::ThermaltakeAIOTab* ui;
    ThermaltakeAIODevice*  device;
    QString                current_image_path;

    void UpdateStartStopButtonLabel();
};
