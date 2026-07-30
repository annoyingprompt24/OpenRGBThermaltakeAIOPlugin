/*---------------------------------------------------------*\
| ThermaltakeAIODevice.cpp                                   |
|                                                             |
|   This file is part of the OpenRGB project                 |
|   SPDX-License-Identifier: GPL-2.0-or-later                |
\*---------------------------------------------------------*/

#include "ThermaltakeAIODevice.h"
#include <hidapi.h>
#include <thread>
#include <chrono>
#include <QBuffer>
#include <QPainter>
#include <QFont>
#include <QFontMetrics>

ThermaltakeAIODevice::ThermaltakeAIODevice()
    : device(nullptr)
    , worker_thread(nullptr)
    , streaming(false)
    , overlay_enabled(false)
{
    /* Force an immediate sensor read on the first overlay-enabled frame. */
    cached_readings_time = std::chrono::steady_clock::now() - std::chrono::hours(1);
}

ThermaltakeAIODevice::~ThermaltakeAIODevice()
{
    Stop();
    Close();
}

bool ThermaltakeAIODevice::Open()
{
    if(device != nullptr)
    {
        return(true);
    }

    hid_init();

    hid_device_info*   info_full = hid_enumerate(VENDOR_ID, PRODUCT_ID);
    hid_device_info*   info_temp = info_full;

    /*-----------------------------------------------------*\
    | Interface 1 is the 1024-byte interrupt interface that |
    | accepts the chunked JPEG stream -- interface 0 is a    |
    | separate serial-number/telemetry channel, unrelated    |
    | to display.                                            |
    \*-----------------------------------------------------*/
    while(info_temp)
    {
        if(info_temp->interface_number == 1)
        {
            device = hid_open_path(info_temp->path);
            break;
        }
        info_temp = info_temp->next;
    }

    hid_free_enumeration(info_full);

    return(device != nullptr);
}

void ThermaltakeAIODevice::Close()
{
    if(device != nullptr)
    {
        hid_close(device);
        device = nullptr;
    }
}

bool ThermaltakeAIODevice::IsConnected() const
{
    return(device != nullptr);
}

void ThermaltakeAIODevice::SetImages(const QVector<QImage>& new_images)
{
    QMutexLocker locker(&images_mutex);
    images = new_images;
}

void ThermaltakeAIODevice::SetOverlayEnabled(bool enabled)
{
    overlay_enabled = enabled;
}

void ThermaltakeAIODevice::DrawOverlay(QImage* image, const ThermaltakeAIOSensorReadings& readings)
{
    QStringList parts;
    if(readings.cpu_ok) parts << QString("CPU %1°C").arg(readings.cpu_temp_c, 0, 'f', 0);
    if(readings.gpu_ok) parts << QString("GPU %1°C").arg(readings.gpu_temp_c, 0, 'f', 0);
    if(readings.ram_ok) parts << QString("RAM %1°C").arg(readings.ram_temp_c, 0, 'f', 0);

    if(parts.isEmpty())
    {
        return;
    }

    QString text = parts.join("   ");

    QPainter painter(image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    QFont font = painter.font();
    font.setPixelSize(image->height() / 16);
    font.setBold(true);
    painter.setFont(font);

    QFontMetrics metrics(font);
    QRect text_rect = metrics.boundingRect(text);

    /*-----------------------------------------------------*\
    | The panel is round -- keep the bar horizontally       |
    | centered and near vertical-center (widest safe band   |
    | on a circle) rather than near the top/bottom edges     |
    | where a wide bar would get clipped by the bezel.       |
    \*-----------------------------------------------------*/
    int    bar_height = text_rect.height() + image->height() / 20;
    int    bar_y       = static_cast<int>(image->height() * 0.72) - bar_height / 2;
    QRect  bar_rect(0, bar_y, image->width(), bar_height);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 140));
    painter.drawRect(bar_rect);

    painter.setPen(Qt::white);
    painter.drawText(bar_rect, Qt::AlignCenter, text);
}

QVector<QByteArray> ThermaltakeAIODevice::ChunkFrame(const QByteArray& jpeg_bytes)
{
    /*-----------------------------------------------------------------------*\
    | Split the JPEG into normal front-to-back content chunks (content chunk  |
    | 1 = SOI..., content chunk N = ...EOI), then reorder for the wire as     |
    | [chunk2, chunk3, ..., chunkN, chunk1] with header idx = 1..N-1, N and   |
    | flag 0x80 only on the last-sent (SOI) chunk. Verified byte-for-byte     |
    | against a real capture -- see project notes, this ordering is          |
    | intentional and not a bug despite looking backwards.                    |
    \*-----------------------------------------------------------------------*/
    QVector<QByteArray> content_chunks;
    for(int offset = 0; offset < jpeg_bytes.size(); offset += CHUNK_PAYLOAD)
    {
        content_chunks.push_back(jpeg_bytes.mid(offset, CHUNK_PAYLOAD));
    }

    int n = content_chunks.size();
    QVector<QByteArray> wire_chunks;
    wire_chunks.reserve(n);

    for(int pos = 1; pos <= n; pos++)
    {
        /* pos 1..N-1 -> content chunks 2..N ; pos N -> content chunk 1 (SOI) */
        const QByteArray& payload = (pos == n) ? content_chunks[0] : content_chunks[pos];

        QByteArray chunk;
        chunk.reserve(CHUNK_TOTAL);
        chunk.append(char(0x08));
        chunk.append(char(pos & 0xFF));
        chunk.append(char(0x00));
        chunk.append(char((pos == n) ? 0x80 : 0x00));
        chunk.append(payload);
        chunk.append(CHUNK_TOTAL - chunk.size(), char(0x00));

        wire_chunks.push_back(chunk);
    }

    return(wire_chunks);
}

void ThermaltakeAIODevice::Start()
{
    if(streaming.load())
    {
        return;
    }

    streaming = true;

    worker_thread = QThread::create([this]() { RunLoop(); });
    worker_thread->start();
}

void ThermaltakeAIODevice::Stop()
{
    streaming = false;

    if(worker_thread != nullptr)
    {
        worker_thread->wait();
        delete worker_thread;
        worker_thread = nullptr;
    }
}

bool ThermaltakeAIODevice::IsStreaming() const
{
    return(streaming.load());
}

void ThermaltakeAIODevice::RunLoop()
{
    size_t frame_index = 0;

    while(streaming.load())
    {
        QImage base_image;

        {
            QMutexLocker locker(&images_mutex);
            if(images.isEmpty())
            {
                locker.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(REFRESH_INTERVAL_MS));
                continue;
            }
            base_image = images[frame_index % images.size()];
            frame_index++;
        }

        auto cycle_start = std::chrono::steady_clock::now();

        if(overlay_enabled.load())
        {
            if(cycle_start - cached_readings_time >= std::chrono::milliseconds(SENSOR_REFRESH_INTERVAL_MS))
            {
                cached_readings      = ThermaltakeAIOSensors::ReadAll();
                cached_readings_time = cycle_start;
            }

            DrawOverlay(&base_image, cached_readings);
        }

        QByteArray jpeg_bytes;
        QBuffer buffer(&jpeg_bytes);
        buffer.open(QIODevice::WriteOnly);
        base_image.save(&buffer, "JPEG", 90);

        QVector<QByteArray> chunks = ChunkFrame(jpeg_bytes);
        for(const QByteArray& chunk : chunks)
        {
            if(!streaming.load())
            {
                break;
            }

            hid_write(device, reinterpret_cast<const unsigned char*>(chunk.constData()), chunk.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(INTER_CHUNK_DELAY_MS));
        }

        auto elapsed = std::chrono::steady_clock::now() - cycle_start;
        auto remaining = std::chrono::milliseconds(REFRESH_INTERVAL_MS) - elapsed;
        if(remaining > std::chrono::milliseconds(0))
        {
            std::this_thread::sleep_for(remaining);
        }
    }
}
