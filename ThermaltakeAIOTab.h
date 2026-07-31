/*---------------------------------------------------------*\
| ThermaltakeAIOTab.h                                        |
|                                                             |
|   This file is part of the OpenRGB project                 |
|   SPDX-License-Identifier: GPL-2.0-or-later                |
\*---------------------------------------------------------*/

#pragma once

#include <QWidget>
#include <QColor>
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
    void on_blank_canvas_button_clicked();
    void on_background_color_button_clicked();
    void on_start_stop_button_clicked();
    void on_visualisation_combo_currentIndexChanged(int index);
    void on_radial_off_radio_toggled(bool checked);
    void on_radial_temp_radio_toggled(bool checked);
    void on_radial_util_radio_toggled(bool checked);
    void on_cpu_color_button_clicked();
    void on_gpu_color_button_clicked();
    void on_ram_color_button_clicked();
    void on_debug_frame_index_checkbox_toggled(bool checked);
    void on_fps_slider_valueChanged(int value);
    void RefreshConnectionStatus();

private:
    Ui::ThermaltakeAIOTab* ui;
    ThermaltakeAIODevice*  device;
    QString                current_image_path;
    QColor                 background_color;

    /*-----------------------------------------------------*\
    | True once anything streamable has been set on the      |
    | device -- an image/GIF file OR a blank canvas. Used to  |
    | gate the Start button instead of current_image_path     |
    | emptiness, which is false for the blank canvas (it has   |
    | content but no file path) and so wrongly re-disabled     |
    | the button on the next connection-status tick.           |
    \*-----------------------------------------------------*/
    bool                   has_streamable_content = false;

    /* Per-module gauge colours, shared across the temperature and utilisation views. */
    QColor                 module_color_cpu;
    QColor                 module_color_gpu;
    QColor                 module_color_ram;

    void LoadAndStreamImage(const QString& path);
    void UpdateBackgroundColorButtonSwatch();
    void UpdateStartStopButtonLabel();

    void PickModuleColor(QColor* target, const QString& module_name);
    void UpdateModuleColorSwatch(class QPushButton* button, const QColor& color);
    void UpdateModuleColorSwatches();
};
