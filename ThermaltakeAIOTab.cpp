/*---------------------------------------------------------*\
| ThermaltakeAIOTab.cpp                                       |
|                                                             |
|   This file is part of the OpenRGB project                 |
|   SPDX-License-Identifier: GPL-2.0-or-later                |
\*---------------------------------------------------------*/

#include "ThermaltakeAIOTab.h"
#include "ui_ThermaltakeAIOTab.h"

#include <QFileDialog>
#include <QBuffer>
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
    | QImageReader exposes every frame of an animated GIF   |
    | (or any other multi-frame format Qt supports); for a  |
    | plain static image imageCount() is just 1. Each frame |
    | gets the same center-crop/resize/JPEG-encode treatment|
    | and the device cycles through them one per refresh.   |
    \*-----------------------------------------------------*/
    QImageReader reader(path);
    reader.setAutoTransform(true);

    QVector<QByteArray> frames;
    QImage first_frame;

    while(true)
    {
        QImage image = reader.read();
        if(image.isNull())
        {
            break;
        }

        int side = qMin(image.width(), image.height());
        QRect crop_rect((image.width() - side) / 2, (image.height() - side) / 2, side, side);
        QImage square = image.copy(crop_rect).scaled(ThermaltakeAIODevice::PANEL_SIZE, ThermaltakeAIODevice::PANEL_SIZE,
                                                      Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

        if(first_frame.isNull())
        {
            first_frame = square;
        }

        QByteArray jpeg_bytes;
        QBuffer buffer(&jpeg_bytes);
        buffer.open(QIODevice::WriteOnly);
        square.save(&buffer, "JPEG", 90);
        frames.push_back(jpeg_bytes);

        if(!reader.jumpToNextImage())
        {
            break;
        }
    }

    if(frames.isEmpty())
    {
        ui->connection_status_label->setText("Failed to load image");
        return;
    }

    device->SetFrames(frames);

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

void ThermaltakeAIOTab::UpdateStartStopButtonLabel()
{
    ui->start_stop_button->setText(device->IsStreaming() ? "Stop Streaming" : "Start Streaming");
}
