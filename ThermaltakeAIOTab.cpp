/*---------------------------------------------------------*\
| ThermaltakeAIOTab.cpp                                       |
|                                                             |
|   This file is part of the OpenRGB project                 |
|   SPDX-License-Identifier: GPL-2.0-or-later                |
\*---------------------------------------------------------*/

#include "ThermaltakeAIOTab.h"
#include "ui_ThermaltakeAIOTab.h"

#include <QFileDialog>
#include <QImage>
#include <QImageReader>
#include <QTimer>
#include <QPixmap>

ThermaltakeAIOTab::ThermaltakeAIOTab(QWidget* parent) :
    QWidget(parent),
    ui(new Ui::ThermaltakeAIOTab)
{
    ui->setupUi(this);

    device = new ThermaltakeAIODevice();

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

    /*-----------------------------------------------------*\
    | QImageReader::read() auto-advances to the next frame  |
    | on its own for animated formats like GIF -- no        |
    | jumpToNextImage() needed (and combining the two just  |
    | double-advances, or the plugin refuses the sequence,  |
    | which is why an earlier version of this only ever     |
    | captured frame 0). For a plain static image, read()   |
    | just returns one frame then a null image next call.   |
    | Cropped/resized QImages are handed to the device as-  |
    | is -- it re-encodes JPEG per send cycle so the sensor  |
    | overlay (if enabled) can be redrawn with fresh values. |
    \*-----------------------------------------------------*/
    QImageReader reader(path);
    reader.setAutoTransform(true);

    QVector<QImage> loaded_images;
    QImage first_frame;
    QImage image;

    while(!(image = reader.read()).isNull())
    {
        int side = qMin(image.width(), image.height());
        QRect crop_rect((image.width() - side) / 2, (image.height() - side) / 2, side, side);
        QImage square = image.copy(crop_rect).scaled(ThermaltakeAIODevice::PANEL_SIZE, ThermaltakeAIODevice::PANEL_SIZE,
                                                      Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                              .convertToFormat(QImage::Format_RGB32);

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

void ThermaltakeAIOTab::UpdateStartStopButtonLabel()
{
    ui->start_stop_button->setText(device->IsStreaming() ? "Stop Streaming" : "Start Streaming");
}
