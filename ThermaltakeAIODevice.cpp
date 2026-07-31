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
#include <cmath>
#include <cstring>
#include <algorithm>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif
#include <QPainter>
#include <QFont>
#include <QFontMetrics>
#include <QBuffer>
#include <QtMath>

/*-----------------------------------------------------------------------*\
| Qt's built-in JPEG writer (4:2:0 chroma subsampling). A Windows USB     |
| capture of the real TT RGB PLUS software confirmed the device's own     |
| encoder also uses plain 4:2:0 (SOF0 sampling factors 2x2/1x1/1x1) for    |
| this exact panel over the streaming path, so there is no need for the   |
| hand-rolled libjpeg 4:4:4 encoder this used to be.                       |
\*-----------------------------------------------------------------------*/
static QByteArray EncodeJpeg(const QImage& image, int quality)
{
    QByteArray result;
    QBuffer buffer(&result);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPG", quality);
    return(result);
}

ThermaltakeAIODevice::ThermaltakeAIODevice()
    : device(nullptr)
    , device_iface0(nullptr)
    , worker_thread(nullptr)
    , streaming(false)
    , overlay_mode(OverlayMode::Off)
    , debug_frame_index_enabled(false)
    , module_color_cpu(qRgb(255, 99, 71))
    , module_color_gpu(qRgb(94, 214, 108))
    , module_color_ram(qRgb(84, 170, 255))
    , images_version(0)
    , render_version(0)
    , smoothed_cpu(0.0f)
    , smoothed_gpu(0.0f)
    , smoothed_ram(0.0f)
    , smoothing_initialized(false)
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
    | accepts the chunked JPEG stream. Interface 0 is a 440-  |
    | byte serial-number/telemetry channel -- unrelated to     |
    | the pixel data itself, but a real capture showed the      |
    | vendor software sends one specific one-way report on it    |
    | (see SendResyncCommand()) right as streaming starts or       |
    | the content source changes, so we open it too even though     |
    | we don't use it for the image data.                             |
    \*-----------------------------------------------------*/
    while(info_temp)
    {
        if(info_temp->interface_number == 1)
        {
            device = hid_open_path(info_temp->path);
        }
        else if(info_temp->interface_number == 0)
        {
            device_iface0 = hid_open_path(info_temp->path);
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
    if(device_iface0 != nullptr)
    {
        hid_close(device_iface0);
        device_iface0 = nullptr;
    }
}

void ThermaltakeAIODevice::SendResyncCommand()
{
    if(device_iface0 == nullptr)
    {
        return;
    }

    unsigned char report[440] = { 0 };
    report[0] = 0x12;
    report[1] = 0x01;
    report[2] = 0x00;
    report[3] = 0x80;
    report[4] = 0x64;

    int result = hid_write(device_iface0, report, sizeof(report));
    std::fprintf(stderr, "[ThermaltakeAIOPlugin] sent interface-0 resync command (0x12), result=%d\n", result);
}

bool ThermaltakeAIODevice::IsConnected() const
{
    return(device != nullptr);
}

void ThermaltakeAIODevice::SetImages(const QVector<QImage>& new_images)
{
    {
        QMutexLocker locker(&images_mutex);
        images = new_images;
        images_version++;
        render_version++;
    }

    /* Mirrors the real capture: sent right as the streamed content source changes. */
    if(streaming.load())
    {
        SendResyncCommand();
    }
}

void ThermaltakeAIODevice::SetOverlayMode(OverlayMode mode)
{
    overlay_mode = mode;
    /* Snap the eased display values to the new metric's scale on the next frame. */
    smoothing_initialized = false;
    render_version++;
}

void ThermaltakeAIODevice::SetModuleColors(const QColor& cpu, const QColor& gpu, const QColor& ram)
{
    module_color_cpu = cpu.rgb();
    module_color_gpu = gpu.rgb();
    module_color_ram = ram.rgb();
    render_version++;
}

void ThermaltakeAIODevice::SetDebugFrameIndexEnabled(bool enabled)
{
    debug_frame_index_enabled = enabled;
    render_version++;
}

void ThermaltakeAIODevice::DrawRing(QPainter& painter, const QPointF& center, float radius, float thickness,
                                     float value, float min_val, float max_val, const QColor& color)
{
    /*-----------------------------------------------------------------------*\
    | Classic 270-degree tachometer sweep with a 90-degree gap at the        |
    | bottom. Qt's drawArc()/angle math both use "0 degrees = 3 o'clock,     |
    | positive = counter-clockwise", so the manual trig below for the        |
    | marker position lines up with the arc without needing a coordinate     |
    | flip. A small marker dot at the tip of the filled arc stands in for    |
    | a needle here -- three concentric rings sharing one center can't       |
    | each draw a full needle to that center without them all overlapping    |
    | in a tangle, so the marker keeps the "current position" read at a      |
    | glance without the clutter, leaving the shared center free for text.   |
    \*-----------------------------------------------------------------------*/
    const float start_angle_deg = 225.0f;
    const float span_deg        = 270.0f;

    float fraction = (max_val > min_val) ? (value - min_val) / (max_val - min_val) : 0.0f;
    fraction = qBound(0.0f, fraction, 1.0f);

    QRectF rect(center.x() - radius, center.y() - radius, radius * 2.0f, radius * 2.0f);

    QPen track_pen(QColor(255, 255, 255, 30), thickness, Qt::SolidLine, Qt::FlatCap);
    painter.setPen(track_pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawArc(rect, static_cast<int>(start_angle_deg * 16), static_cast<int>(-span_deg * 16));

    QPen value_pen(color, thickness, Qt::SolidLine, Qt::FlatCap);
    painter.setPen(value_pen);
    painter.drawArc(rect, static_cast<int>(start_angle_deg * 16), static_cast<int>(-span_deg * fraction * 16));

    float marker_angle_rad = qDegreesToRadians(start_angle_deg - span_deg * fraction);
    QPointF marker(center.x() + radius * std::cos(marker_angle_rad),
                    center.y() - radius * std::sin(marker_angle_rad));

    painter.setPen(QPen(Qt::white, thickness * 0.18f));
    painter.setBrush(color);
    painter.drawEllipse(marker, thickness * 0.52, thickness * 0.52);
}

void ThermaltakeAIODevice::DrawOverlay(QImage* image, const ThermaltakeAIOSensorReadings& readings, OverlayMode mode)
{
    /*-----------------------------------------------------------------------*\
    | Pick each module's raw value + whether it's available from the active   |
    | mode. Temperature and utilization share the exact same layout and       |
    | 0-100 sweep (degrees C and percent both map cleanly onto it) -- only    |
    | the data source and the readout suffix differ.                          |
    \*-----------------------------------------------------------------------*/
    bool   is_util = (mode == OverlayMode::Utilization);

    bool   cpu_ok  = is_util ? readings.cpu_util_ok  : readings.cpu_ok;
    bool   gpu_ok  = is_util ? readings.gpu_util_ok  : readings.gpu_ok;
    bool   ram_ok  = is_util ? readings.ram_util_ok  : readings.ram_ok;

    double cpu_val = is_util ? readings.cpu_util_pct : readings.cpu_temp_c;
    double gpu_val = is_util ? readings.gpu_util_pct : readings.gpu_temp_c;
    double ram_val = is_util ? readings.ram_util_pct : readings.ram_temp_c;

    QString suffix = is_util ? "%" : "°";

    /*-----------------------------------------------------------------------*\
    | Ease the displayed value a little closer to the latest raw reading      |
    | every single frame (called every RunLoop cycle while the overlay is     |
    | on -- see the render_version++ in RunLoop) rather than only when a      |
    | new sensor sample lands once/second. That keeps the marker moving       |
    | smoothly at whatever the target FPS is instead of visibly jumping       |
    | once a second, and incidentally means every frame's pixels genuinely    |
    | differ from the last even against a plain background -- useful for      |
    | the "does a high, ever-changing frame rate glitch on a static           |
    | background" test. A mode switch clears smoothing_initialized so the      |
    | values snap to the new metric rather than sliding across scales.        |
    \*-----------------------------------------------------------------------*/
    const float ease = 0.15f;

    if(!smoothing_initialized)
    {
        smoothed_cpu          = cpu_ok ? cpu_val : 0.0f;
        smoothed_gpu          = gpu_ok ? gpu_val : 0.0f;
        smoothed_ram          = ram_ok ? ram_val : 0.0f;
        smoothing_initialized = true;
    }
    else
    {
        if(cpu_ok) smoothed_cpu += (cpu_val - smoothed_cpu) * ease;
        if(gpu_ok) smoothed_gpu += (gpu_val - smoothed_gpu) * ease;
        if(ram_ok) smoothed_ram += (ram_val - smoothed_ram) * ease;
    }

    struct RingSpec { float value; QColor color; QString label; };
    QVector<RingSpec> rings;
    if(cpu_ok) rings.push_back({smoothed_cpu, QColor::fromRgb(module_color_cpu.load()), "CPU"});
    if(gpu_ok) rings.push_back({smoothed_gpu, QColor::fromRgb(module_color_gpu.load()), "GPU"});
    if(ram_ok) rings.push_back({smoothed_ram, QColor::fromRgb(module_color_ram.load()), "RAM"});

    if(rings.isEmpty())
    {
        return;
    }

    QPainter painter(image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    /*-----------------------------------------------------------------------*\
    | Concentric rings sharing the panel's own center -- this maximizes use  |
    | of the round display, unlike three separate side-by-side gauges which  |
    | left most of the circle's area unused. Outermost ring first (CPU),     |
    | stepping inward by thickness+gap for each subsequent metric.           |
    \*-----------------------------------------------------------------------*/
    QPointF center(image->width() / 2.0f, image->height() / 2.0f);
    float   thickness    = qMin(image->width(), image->height()) * 0.067f;
    float   gap          = thickness * 0.3f;
    float   outer_radius = qMin(image->width(), image->height()) * 0.44f;

    for(int i = 0; i < rings.size(); i++)
    {
        float radius = outer_radius - i * (thickness + gap);
        DrawRing(painter, center, radius, thickness, rings[i].value, 0.0f, 100.0f, rings[i].color);
    }

    /* Stacked, color-matched numeric readout in the shared center. */
    float line_height = image->height() * 0.096f;
    float block_top    = center.y() - (line_height * rings.size()) / 2.0f;

    QFont font = painter.font();
    font.setPixelSize(static_cast<int>(image->height() * 0.075f));
    font.setBold(true);
    painter.setFont(font);

    for(int i = 0; i < rings.size(); i++)
    {
        QRectF line_rect(center.x() - image->width() * 0.42f, block_top + i * line_height,
                          image->width() * 0.84f, line_height);
        painter.setPen(rings[i].color);
        QString text = QString("%1  %2%3").arg(rings[i].label).arg(rings[i].value, 0, 'f', 0).arg(suffix);
        painter.drawText(line_rect, Qt::AlignCenter, text);
    }
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

    /* Mirrors the real capture: sent right as continuous streaming begins. */
    SendResyncCommand();

    worker_thread = QThread::create([this]() { RunLoop(); });

    /*-----------------------------------------------------------------------*\
    | Experimental: a live capture during a real CPU/GPU load spike (gaming) |
    | showed hid_write() itself blocking up to ~25ms inside the kernel's      |
    | USB/HID stack (mean gap 393us/max 592us idle -> mean 1.3ms/max 25ms      |
    | under load), which lines up with when panel corruption was visible.       |
    | Asking for a higher scheduling priority for this thread specifically       |
    | can't fix kernel-side USB controller contention, but it does reduce the      |
    | chance this thread itself sits waiting for CPU time behind the game's         |
    | own threads, which is part of the same failure mode. Qt falls back to          |
    | normal priority silently if the OS refuses the request (e.g. no                 |
    | CAP_SYS_NICE) -- not a guaranteed fix, but low-risk to try.                       |
    \*-----------------------------------------------------------------------*/
    worker_thread->start(QThread::TimeCriticalPriority);
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
    /*-----------------------------------------------------------------------*\
    | QThread::TimeCriticalPriority (set in Start()) measurably did nothing   |
    | under a real CPU/GPU load test -- on Linux that's typically just a       |
    | nice-value nudge within the normal SCHED_OTHER class, not real POSIX      |
    | real-time scheduling. This session's rtprio ulimit is 99 (confirmed via   |
    | `ulimit -Hr`/`-Sr`, granted through the @realtime/@audio PAM limits         |
    | groups), so request actual SCHED_FIFO directly and log whether it really    |
    | took -- don't assume, the last attempt silently not working is exactly       |
    | why this is being verified explicitly this time.                              |
    \*-----------------------------------------------------------------------*/
#ifdef __linux__
    {
        sched_param sch_params;
        sch_params.sched_priority = 50;
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sch_params);
        std::fprintf(stderr, "[ThermaltakeAIOPlugin] pthread_setschedparam(SCHED_FIFO, prio=50) => %s\n",
                     rc == 0 ? "success" : std::strerror(rc));
    }
#endif

    size_t   frame_index    = 0;
    long     sent_frames    = 0;
    long     encode_count   = 0;
    auto     loop_start     = std::chrono::steady_clock::now();
    auto     last_chunk_time = loop_start;

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

        OverlayMode current_overlay_mode = overlay_mode.load();
        if(current_overlay_mode != OverlayMode::Off)
        {
            if(cycle_start - cached_readings_time >= std::chrono::milliseconds(SENSOR_REFRESH_INTERVAL_MS))
            {
                cached_readings      = ThermaltakeAIOSensors::ReadAll();
                cached_readings_time = cycle_start;
            }

            /*-----------------------------------------------------*\
            | Every frame, not just on new sensor samples -- the    |
            | gauge marker eases toward the latest reading a little |
            | more each call, so it needs a fresh render (and thus  |
            | a fresh encode) every cycle to actually animate.        |
            \*-----------------------------------------------------*/
            render_version++;
        }

        size_t idx = frame_index % local_images.size();
        frame_index++;

        int current_render_version = render_version.load();
        if(encoded_cache_version[idx] != current_render_version)
        {
            QImage image = local_images[idx];
            if(current_overlay_mode != OverlayMode::Off)
            {
                DrawOverlay(&image, cached_readings, current_overlay_mode);
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

            QByteArray jpeg_bytes = EncodeJpeg(image, 90);

            encoded_cache[idx]         = jpeg_bytes;
            encoded_cache_version[idx] = current_render_version;
            encode_count++;
        }

        const QByteArray& jpeg_bytes = encoded_cache[idx];

        QVector<QByteArray> chunks = ChunkFrame(jpeg_bytes);
        bool write_failed = false;
        int  chunk_pos    = 0;
        for(const QByteArray& chunk : chunks)
        {
            if(!streaming.load())
            {
                break;
            }

            /*-----------------------------------------------------------------------*\
            | Stall detection: this thread being descheduled, or hid_write() itself    |
            | blocking longer than expected inside the kernel's USB/HID stack, under    |
            | host load (e.g. a game spiking CPU/GPU/IRQ activity) is a real candidate   |
            | for the panel-side corruption -- a long gap mid-frame means the panel       |
            | goes a long time without expected data, which could desync whatever          |
            | internal state its decoder keeps. Two separate things are timed: the gap      |
            | before this write (catches this thread being slow to get back around to       |
            | it, e.g. after sleep_for() overshooting) and the write call's own duration      |
            | (catches hid_write() blocking internally, e.g. waiting on kernel USB/HID         |
            | buffer space -- this part is NOT visible as a gap between calls, since any        |
            | such delay is absorbed into the call itself rather than the time before it).       |
            \*-----------------------------------------------------------------------*/
            auto pre_write_time = std::chrono::steady_clock::now();
            long gap_us = std::chrono::duration_cast<std::chrono::microseconds>(pre_write_time - last_chunk_time).count();
            long stall_threshold_us = std::max<long>(static_cast<long>(inter_chunk_delay_us) * 5L, 3000L);
            if(chunk_pos > 0 && gap_us > stall_threshold_us)
            {
                double elapsed_s = std::chrono::duration<double>(pre_write_time - loop_start).count();
                std::fprintf(stderr, "[ThermaltakeAIOPlugin] STALL (gap-before-write): %ldus before chunk %d/%d of "
                             "frame idx=%zu (sent_frames=%ld) at t=%.3fs\n",
                             gap_us, chunk_pos + 1, chunks.size(), idx, sent_frames, elapsed_s);
            }

            int result = hid_write(device, reinterpret_cast<const unsigned char*>(chunk.constData()), chunk.size());

            auto post_write_time = std::chrono::steady_clock::now();
            long write_duration_us = std::chrono::duration_cast<std::chrono::microseconds>(post_write_time - pre_write_time).count();
            if(write_duration_us > stall_threshold_us)
            {
                double elapsed_s = std::chrono::duration<double>(post_write_time - loop_start).count();
                std::fprintf(stderr, "[ThermaltakeAIOPlugin] STALL (hid_write blocked): %ldus for chunk %d/%d of "
                             "frame idx=%zu (sent_frames=%ld) at t=%.3fs\n",
                             write_duration_us, chunk_pos + 1, chunks.size(), idx, sent_frames, elapsed_s);
            }

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

            last_chunk_time = std::chrono::steady_clock::now();
            chunk_pos++;
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
