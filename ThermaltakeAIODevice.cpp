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

ThermaltakeAIODevice::ThermaltakeAIODevice()
    : device(nullptr)
    , worker_thread(nullptr)
    , streaming(false)
{
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

void ThermaltakeAIODevice::SetFrames(const QVector<QByteArray>& jpeg_frames)
{
    QMutexLocker locker(&frames_mutex);
    frames = jpeg_frames;
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
        QByteArray frame;

        {
            QMutexLocker locker(&frames_mutex);
            if(frames.isEmpty())
            {
                locker.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(REFRESH_INTERVAL_MS));
                continue;
            }
            frame = frames[frame_index % frames.size()];
            frame_index++;
        }

        auto cycle_start = std::chrono::steady_clock::now();

        QVector<QByteArray> chunks = ChunkFrame(frame);
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
