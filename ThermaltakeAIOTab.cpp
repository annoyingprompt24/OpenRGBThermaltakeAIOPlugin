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
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QTimer>
#include <QPixmap>

ThermaltakeAIOTab::ThermaltakeAIOTab(QWidget* parent) :
    QWidget(parent),
    ui(new Ui::ThermaltakeAIOTab),
    background_color(Qt::black)
{
    ui->setupUi(this);

    device = new ThermaltakeAIODevice();

    /* Reflect whatever target fps the device actually started with (default, or env override). */
    int initial_fps = device->GetTargetFps();
    ui->fps_slider->setValue(initial_fps);
    ui->fps_slider_label->setText(QString("Target FPS: %1").arg(initial_fps));

    UpdateBackgroundColorButtonSwatch();
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
        ui->start_stop_button->setEnabled(!current_image_path.isEmpty());
    }
    else
    {
        ui->connection_status_label->setText("Panel not connected");
        ui->start_stop_button->setEnabled(false);
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

    current_image_path = path;
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

void ThermaltakeAIOTab::on_sensor_overlay_checkbox_toggled(bool checked)
{
    device->SetOverlayEnabled(checked);
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

void ThermaltakeAIOTab::UpdateStartStopButtonLabel()
{
    ui->start_stop_button->setText(device->IsStreaming() ? "Stop Streaming" : "Start Streaming");
}
