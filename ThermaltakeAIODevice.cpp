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
#include <cstdio>
#include <cstdlib>
#include <csetjmp>
#include <QPainter>
#include <QFont>
#include <QFontMetrics>

extern "C" {
#include <jpeglib.h>
}

namespace
{
    struct JpegErrorContext
    {
        struct jpeg_error_mgr pub;
        jmp_buf setjmp_buffer;
    };

    void JpegErrorExit(j_common_ptr cinfo)
    {
        JpegErrorContext* err = reinterpret_cast<JpegErrorContext*>(cinfo->err);
        std::longjmp(err->setjmp_buffer, 1);
    }
}

/*-----------------------------------------------------------------------*\
| Encodes with 4:4:4 chroma subsampling (no subsampling at all) instead  |
| of Qt's default JPEG writer, which always produces 4:2:0 and has no    |
| public API to change it. This specific panel requires 4:4:4 for the    |
| continuous EP3 streaming path -- 4:2:0 causes corrupted garbage in      |
| part of the displayed frame (documented independently by another         |
| reverse-engineer of this exact device, 264A:233C -- boot/standby image    |
| upload is a separate path that reportedly wants 4:2:0, opposite of        |
| streaming, but we only use the streaming path here).                       |
\*-----------------------------------------------------------------------*/
static QByteArray EncodeJpeg444(const QImage& image, int quality)
{
    QImage rgb = image.convertToFormat(QImage::Format_RGB888);

    struct jpeg_compress_struct cinfo;
    JpegErrorContext jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = JpegErrorExit;

    unsigned char* out_buffer = nullptr;
    unsigned long  out_size   = 0;

    if(setjmp(jerr.setjmp_buffer))
    {
        jpeg_destroy_compress(&cinfo);
        if(out_buffer != nullptr)
        {
            free(out_buffer);
        }
        return(QByteArray());
    }

    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &out_buffer, &out_size);

    cinfo.image_width      = rgb.width();
    cinfo.image_height     = rgb.height();
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    for(int c = 0; c < cinfo.num_components; c++)
    {
        cinfo.comp_info[c].h_samp_factor = 1;
        cinfo.comp_info[c].v_samp_factor = 1;
    }

    jpeg_start_compress(&cinfo, TRUE);

    while(cinfo.next_scanline < cinfo.image_height)
    {
        JSAMPROW row = const_cast<JSAMPROW>(rgb.constScanLine(cinfo.next_scanline));
        jpeg_write_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_compress(&cinfo);

    QByteArray result(reinterpret_cast<const char*>(out_buffer), static_cast<int>(out_size));

    jpeg_destroy_compress(&cinfo);
    free(out_buffer);

    return(result);
}

ThermaltakeAIODevice::ThermaltakeAIODevice()
    : device(nullptr)
    , worker_thread(nullptr)
    , streaming(false)
    , overlay_enabled(false)
    , debug_frame_index_enabled(false)
    , images_version(0)
    , render_version(0)
    , inter_chunk_delay_us(1000)
    , refresh_interval_ms(50)
    , ack_pace(false)
{
    /* Force an immediate sensor read on the first overlay-enabled frame. */
    cached_readings_time = std::chrono::steady_clock::now() - std::chrono::hours(1);

    if(const char* env_delay = std::getenv("THERMALTAKE_AIO_CHUNK_DELAY_US"))
    {
        inter_chunk_delay_us = std::atoi(env_delay);
    }
    if(const char* env_refresh = std::getenv("THERMALTAKE_AIO_REFRESH_MS"))
    {
        refresh_interval_ms = std::atoi(env_refresh);
    }
    if(const char* env_ack = std::getenv("THERMALTAKE_AIO_ACK_PACE"))
    {
        ack_pace = (std::atoi(env_ack) != 0);
    }

    std::fprintf(stderr, "[ThermaltakeAIOPlugin] chunk delay = %dus, refresh interval = %dms, ack_pace = %s\n",
                 inter_chunk_delay_us, refresh_interval_ms.load(), ack_pace ? "on" : "off");
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
    images_version++;
    render_version++;
}

void ThermaltakeAIODevice::SetOverlayEnabled(bool enabled)
{
    overlay_enabled = enabled;
    render_version++;
}

void ThermaltakeAIODevice::SetDebugFrameIndexEnabled(bool enabled)
{
    debug_frame_index_enabled = enabled;
    render_version++;
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

void ThermaltakeAIODevice::SetTargetFps(int fps)
{
    if(fps < 1)
    {
        fps = 1;
    }
    refresh_interval_ms = 1000 / fps;
}

int ThermaltakeAIODevice::GetTargetFps() const
{
    int ms = refresh_interval_ms.load();
    return(ms > 0 ? 1000 / ms : 1000);
}

void ThermaltakeAIODevice::RunLoop()
{
    size_t   frame_index  = 0;
    long     sent_frames  = 0;
    long     encode_count = 0;
    auto     loop_start   = std::chrono::steady_clock::now();

    /*-----------------------------------------------------*\
    | Only this thread touches these -- no mutex needed. Each  |
    | slot's JPEG is (re-)encoded lazily, the cycle right      |
    | before it's due to be sent, if its stamped version is    |
    | behind the current render_version -- NOT all at once.    |
    | A many-hundred-frame GIF used to re-encode every single   |
    | frame synchronously on every overlay/sensor update,        |
    | stalling the send loop for over a second at a time; this   |
    | spreads that cost to ~one frame's encode time per cycle,    |
    | regardless of total frame count.                              |
    \*-----------------------------------------------------*/
    QVector<QImage>     local_images;
    QVector<QByteArray> encoded_cache;
    QVector<int>        encoded_cache_version;
    int                 synced_images_version = -1;

    while(streaming.load())
    {
        auto cycle_start = std::chrono::steady_clock::now();

        int current_images_version = images_version.load();
        if(current_images_version != synced_images_version)
        {
            QMutexLocker locker(&images_mutex);
            local_images = images;
            locker.unlock();

            synced_images_version = current_images_version;
            encoded_cache.clear();
            encoded_cache.resize(local_images.size());
            encoded_cache_version = QVector<int>(local_images.size(), -1);
            frame_index = 0;
        }

        if(local_images.isEmpty())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(refresh_interval_ms));
            continue;
        }

        if(overlay_enabled.load())
        {
            if(cycle_start - cached_readings_time >= std::chrono::milliseconds(SENSOR_REFRESH_INTERVAL_MS))
            {
                cached_readings      = ThermaltakeAIOSensors::ReadAll();
                cached_readings_time = cycle_start;
                render_version++;
            }
        }

        size_t idx = frame_index % local_images.size();
        frame_index++;

        int current_render_version = render_version.load();
        if(encoded_cache_version[idx] != current_render_version)
        {
            QImage image = local_images[idx];
            if(overlay_enabled.load())
            {
                DrawOverlay(&image, cached_readings);
            }
            if(debug_frame_index_enabled.load())
            {
                QPainter painter(&image);
                painter.setRenderHint(QPainter::Antialiasing);
                QFont font = painter.font();
                font.setPixelSize(image.height() / 10);
                font.setBold(true);
                painter.setFont(font);
                QString text = QString("Frame %1/%2").arg(idx + 1).arg(local_images.size());
                QRect bar(0, 0, image.width(), image.height() / 8);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0, 0, 0, 180));
                painter.drawRect(bar);
                painter.setPen(Qt::yellow);
                painter.drawText(bar, Qt::AlignCenter, text);
            }

            QByteArray jpeg_bytes = EncodeJpeg444(image, 90);

            encoded_cache[idx]         = jpeg_bytes;
            encoded_cache_version[idx] = current_render_version;
            encode_count++;
        }

        const QByteArray& jpeg_bytes = encoded_cache[idx];

        QVector<QByteArray> chunks = ChunkFrame(jpeg_bytes);
        bool write_failed = false;
        for(const QByteArray& chunk : chunks)
        {
            if(!streaming.load())
            {
                break;
            }

            int result = hid_write(device, reinterpret_cast<const unsigned char*>(chunk.constData()), chunk.size());
            if(result < 0)
            {
                std::fprintf(stderr, "[ThermaltakeAIOPlugin] hid_write failed after %ld frame(s) at "
                             "%dus/chunk -- stopping stream (endpoint likely wedged, needs a physical replug)\n",
                             sent_frames, inter_chunk_delay_us);
                write_failed = true;
                break;
            }

            if(inter_chunk_delay_us > 0)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(inter_chunk_delay_us));
            }
        }

        if(write_failed)
        {
            streaming = false;
            break;
        }

        sent_frames++;
        if(sent_frames % 100 == 0)
        {
            double elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - loop_start).count();
            std::fprintf(stderr, "[ThermaltakeAIOPlugin] %ld frames sent, %.1f fps average, %ld (re-)encodes so far\n",
                         sent_frames, sent_frames / elapsed_s, encode_count);
        }

        if(ack_pace)
        {
            /*-------------------------------------------------*\
            | Wait for the panel's own 16-byte EP4 reply (sent    |
            | after each frame's commit chunk) instead of a        |
            | fixed wall-clock sleep -- syncs our send cadence to    |
            | the device's actual readiness. Falls back to moving     |
            | on immediately if no reply shows up within the timeout,  |
            | so a device that doesn't ack (or a dropped ack) can't      |
            | stall the stream outright.                                  |
            \*-----------------------------------------------------*/
            unsigned char ack_buf[16];
            hid_read_timeout(device, ack_buf, sizeof(ack_buf), ACK_TIMEOUT_MS);
        }
        else
        {
            auto elapsed = std::chrono::steady_clock::now() - cycle_start;
            auto remaining = std::chrono::milliseconds(refresh_interval_ms) - elapsed;
            if(remaining > std::chrono::milliseconds(0))
            {
                std::this_thread::sleep_for(remaining);
            }
        }
    }
}
