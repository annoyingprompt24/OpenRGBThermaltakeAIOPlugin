/*---------------------------------------------------------*\
| ThermaltakeAIOTab.cpp                                       |
|                                                             |
|   This file is part of the OpenRGB project                 |
|   SPDX-License-Identifier: GPL-2.0-or-later                |
\*---------------------------------------------------------*/

#include "ThermaltakeAIOTab.h"
#include "ui_ThermaltakeAIOTab.h"

#include <QFileDialog>
#include <QColorDialog>
#include <QPushButton>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QTimer>
#include <QPixmap>

ThermaltakeAIOTab::ThermaltakeAIOTab(QWidget* parent) :
    QWidget(parent),
    ui(new Ui::ThermaltakeAIOTab),
    background_color(Qt::black),
    /* Defaults must match ThermaltakeAIODevice's own initial module colours. */
    module_color_cpu(255, 99, 71),
    module_color_gpu(94, 214, 108),
    module_color_ram(84, 170, 255)
{
    ui->setupUi(this);

    device = new ThermaltakeAIODevice();

    /*-----------------------------------------------------*\
    | Visualisation selector -- only the radial gauge for    |
    | now, but the dropdown is here so more views can be      |
    | added without reworking the UI.                         |
    \*-----------------------------------------------------*/
    ui->visualisation_combo->addItem("Radial Gauge");

    /* Reflect whatever target fps the device actually started with (default, or env override). */
    int initial_fps = device->GetTargetFps();
    ui->fps_slider->setValue(initial_fps);
    ui->fps_slider_label->setText(QString("Target FPS: %1").arg(initial_fps));

    int initial_brightness = device->GetBrightness();
    ui->brightness_slider->setValue(initial_brightness);
    ui->brightness_slider_label->setText(QString("Brightness: %1%").arg(initial_brightness));

    device->SetModuleColors(module_color_cpu, module_color_gpu, module_color_ram);

    UpdateBackgroundColorButtonSwatch();
    UpdateModuleColorSwatches();
    RefreshConnectionStatus();

    /*-----------------------------------------------------*\
    | Re-check connection periodically so the tab notices    |
    | the panel being plugged in/out without a manual        |
    | refresh action.                                        |
    \*-----------------------------------------------------*/
    QTimer* connection_timer = new QTimer(this);
    connect(connection_timer, &QTimer::timeout, this, &ThermaltakeAIOTab::RefreshConnectionStatus);
    connection_timer->start(3000);
}

ThermaltakeAIOTab::~ThermaltakeAIOTab()
{
    device->Stop();
    delete device;
    delete ui;
}

void ThermaltakeAIOTab::RefreshConnectionStatus()
{
    if(!device->IsConnected())
    {
        device->Open();
    }

    if(device->IsConnected())
    {
        ui->connection_status_label->setText("Panel connected");
        ui->start_stop_button->setEnabled(has_streamable_content);
        /* Standby is a self-contained flash write -- it picks its own image, so it only needs a connected panel. */
        ui->set_standby_button->setEnabled(true);
    }
    else
    {
        ui->connection_status_label->setText("Panel not connected");
        ui->start_stop_button->setEnabled(false);
        ui->set_standby_button->setEnabled(false);
    }
}

void ThermaltakeAIOTab::on_choose_image_button_clicked()
{
    QString path = QFileDialog::getOpenFileName(this, "Choose Image", QString(),
                                                  "Images (*.png *.jpg *.jpeg *.bmp *.gif)");
    if(path.isEmpty())
    {
        return;
    }

    LoadAndStreamImage(path);
}

void ThermaltakeAIOTab::on_blank_canvas_button_clicked()
{
    QImage blank(ThermaltakeAIODevice::PANEL_SIZE, ThermaltakeAIODevice::PANEL_SIZE, QImage::Format_RGB32);
    blank.fill(Qt::black);

    QVector<QImage> single_image;
    single_image.push_back(blank);
    device->SetImages(single_image);

    /* Not a file -- clear so the background-color re-composite path doesn't try to reload one. */
    current_image_path.clear();
    has_streamable_content = true;

    ui->preview_label->setPixmap(QPixmap::fromImage(blank).scaled(ui->preview_label->size(),
                                                                     Qt::KeepAspectRatio, Qt::SmoothTransformation));
    ui->start_stop_button->setEnabled(device->IsConnected());
}

void ThermaltakeAIOTab::on_set_standby_button_clicked()
{
    QString path = QFileDialog::getOpenFileName(this, "Choose Standby Image", QString(),
                                                  "Images (*.png *.jpg *.jpeg *.bmp *.gif)");
    if(path.isEmpty())
    {
        return;
    }

    /*-----------------------------------------------------*\
    | Prepare the frame exactly like the live-stream path:  |
    | composite onto the background color first (the panel   |
    | is a plain JPEG raster with no transparency), then     |
    | center-crop to square and scale to the panel size. For |
    | an animated source we just take the first frame.       |
    \*-----------------------------------------------------*/
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if(image.isNull())
    {
        ui->standby_status_label->setText("Failed to load image");
        return;
    }

    QImage composited(image.size(), QImage::Format_RGB32);
    composited.fill(background_color);
    QPainter painter(&composited);
    painter.drawImage(0, 0, image);
    painter.end();

    int   side = qMin(composited.width(), composited.height());
    QRect crop_rect((composited.width() - side) / 2, (composited.height() - side) / 2, side, side);
    QImage square = composited.copy(crop_rect).scaled(ThermaltakeAIODevice::PANEL_SIZE, ThermaltakeAIODevice::PANEL_SIZE,
                                                        Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    /*-----------------------------------------------------*\
    | The flash write blocks ~200ms. Show status and force   |
    | an immediate repaint so the label updates before the    |
    | UI thread stalls, then disable the button for the       |
    | duration to prevent re-entrancy.                        |
    \*-----------------------------------------------------*/
    ui->set_standby_button->setEnabled(false);
    ui->standby_status_label->setText("Writing standby image...");
    ui->standby_status_label->repaint();

    bool ok = device->SetStandbyImage(square);

    ui->standby_status_label->setText(ok ? "Standby image saved to panel" : "Standby write failed");
    ui->set_standby_button->setEnabled(device->IsConnected());
}

void ThermaltakeAIOTab::on_background_color_button_clicked()
{
    QColor chosen = QColorDialog::getColor(background_color, this, "Background Color for Transparent Pixels");
    if(!chosen.isValid())
    {
        return;
    }

    background_color = chosen;
    UpdateBackgroundColorButtonSwatch();

    /* Re-composite the already-loaded image against the new background, if any. */
    if(!current_image_path.isEmpty())
    {
        LoadAndStreamImage(current_image_path);
    }
}

void ThermaltakeAIOTab::LoadAndStreamImage(const QString& path)
{
    /*-----------------------------------------------------*\
    | QImageReader::read() auto-advances to the next frame  |
    | on its own for animated formats like GIF -- no        |
    | jumpToNextImage() needed (and combining the two just  |
    | double-advances, or the plugin refuses the sequence,  |
    | which is why an earlier version of this only ever     |
    | captured frame 0). For a plain static image, read()   |
    | just returns one frame then a null image next call.   |
    \*-----------------------------------------------------*/
    QImageReader reader(path);
    reader.setAutoTransform(true);

    QVector<QImage> loaded_images;
    QImage first_frame;
    QImage image;

    while(!(image = reader.read()).isNull())
    {
        /*-------------------------------------------------*\
        | Composite onto a solid background BEFORE cropping/ |
        | scaling, not after -- the panel has no concept of   |
        | transparency (it's a plain JPEG raster), and GIF     |
        | encoders routinely leave undefined/garbage RGB data  |
        | under fully-transparent pixels since it's never       |
        | meant to be shown. Compositing first also avoids      |
        | smooth-scaling blending that garbage color into        |
        | semi-transparent edge pixels.                           |
        \*-------------------------------------------------*/
        QImage composited(image.size(), QImage::Format_RGB32);
        composited.fill(background_color);
        QPainter painter(&composited);
        painter.drawImage(0, 0, image);
        painter.end();

        int side = qMin(composited.width(), composited.height());
        QRect crop_rect((composited.width() - side) / 2, (composited.height() - side) / 2, side, side);
        QImage square = composited.copy(crop_rect).scaled(ThermaltakeAIODevice::PANEL_SIZE, ThermaltakeAIODevice::PANEL_SIZE,
                                                            Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

        if(first_frame.isNull())
        {
            first_frame = square;
        }

        loaded_images.push_back(square);
    }

    if(loaded_images.isEmpty())
    {
        ui->connection_status_label->setText("Failed to load image");
        return;
    }

    device->SetImages(loaded_images);

    current_image_path     = path;
    has_streamable_content = true;
    ui->preview_label->setPixmap(QPixmap::fromImage(first_frame).scaled(ui->preview_label->size(),
                                                                          Qt::KeepAspectRatio, Qt::SmoothTransformation));
    ui->start_stop_button->setEnabled(device->IsConnected());
}

void ThermaltakeAIOTab::UpdateBackgroundColorButtonSwatch()
{
    QString text_color = (background_color.lightness() > 128) ? "black" : "white";
    ui->background_color_button->setStyleSheet(
        QString("background-color: %1; color: %2;").arg(background_color.name(), text_color));
}

void ThermaltakeAIOTab::on_start_stop_button_clicked()
{
    if(device->IsStreaming())
    {
        device->Stop();
    }
    else
    {
        device->Start();
    }

    UpdateStartStopButtonLabel();
}

void ThermaltakeAIOTab::on_visualisation_combo_currentIndexChanged(int index)
{
    /*-----------------------------------------------------*\
    | Only the radial gauge exists today, so there's nothing |
    | to switch between yet -- this hook is here so adding a  |
    | second visualisation later is a UI-only change.         |
    \*-----------------------------------------------------*/
    (void)index;
}

void ThermaltakeAIOTab::on_radial_off_radio_toggled(bool checked)
{
    if(checked)
    {
        device->SetOverlayMode(ThermaltakeAIODevice::OverlayMode::Off);
    }
}

void ThermaltakeAIOTab::on_radial_temp_radio_toggled(bool checked)
{
    if(checked)
    {
        device->SetOverlayMode(ThermaltakeAIODevice::OverlayMode::Temperature);
    }
}

void ThermaltakeAIOTab::on_radial_util_radio_toggled(bool checked)
{
    if(checked)
    {
        device->SetOverlayMode(ThermaltakeAIODevice::OverlayMode::Utilization);
    }
}

void ThermaltakeAIOTab::on_cpu_color_button_clicked()
{
    PickModuleColor(&module_color_cpu, "CPU");
}

void ThermaltakeAIOTab::on_gpu_color_button_clicked()
{
    PickModuleColor(&module_color_gpu, "GPU");
}

void ThermaltakeAIOTab::on_ram_color_button_clicked()
{
    PickModuleColor(&module_color_ram, "RAM");
}

void ThermaltakeAIOTab::PickModuleColor(QColor* target, const QString& module_name)
{
    QColor chosen = QColorDialog::getColor(*target, this, QString("%1 Gauge Color").arg(module_name));
    if(!chosen.isValid())
    {
        return;
    }

    *target = chosen;
    device->SetModuleColors(module_color_cpu, module_color_gpu, module_color_ram);
    UpdateModuleColorSwatches();
}

void ThermaltakeAIOTab::UpdateModuleColorSwatch(QPushButton* button, const QColor& color)
{
    QString text_color = (color.lightness() > 128) ? "black" : "white";
    button->setStyleSheet(QString("background-color: %1; color: %2;").arg(color.name(), text_color));
}

void ThermaltakeAIOTab::UpdateModuleColorSwatches()
{
    UpdateModuleColorSwatch(ui->cpu_color_button, module_color_cpu);
    UpdateModuleColorSwatch(ui->gpu_color_button, module_color_gpu);
    UpdateModuleColorSwatch(ui->ram_color_button, module_color_ram);
}

void ThermaltakeAIOTab::on_debug_frame_index_checkbox_toggled(bool checked)
{
    device->SetDebugFrameIndexEnabled(checked);
}

void ThermaltakeAIOTab::on_fps_slider_valueChanged(int value)
{
    device->SetTargetFps(value);
    ui->fps_slider_label->setText(QString("Target FPS: %1").arg(value));
}

void ThermaltakeAIOTab::on_brightness_slider_valueChanged(int value)
{
    device->SetBrightness(value);
    ui->brightness_slider_label->setText(QString("Brightness: %1%").arg(value));
}

void ThermaltakeAIOTab::UpdateStartStopButtonLabel()
{
    ui->start_stop_button->setText(device->IsStreaming() ? "Stop Streaming" : "Start Streaming");
}
