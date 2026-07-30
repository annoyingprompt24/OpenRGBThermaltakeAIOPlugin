/*---------------------------------------------------------*\
| ThermaltakeAIODevice.h                                     |
|                                                             |
|   Streams JPEG frames to the Thermaltake AIO round LCD      |
|   (264A:233C) over its interface 1 HID interrupt endpoint.  |
|                                                             |
|   Protocol reverse-engineered from a USBPcap capture of     |
|   TT RGB PLUS, see project notes.  Key points:               |
|     - Interface 1 only, no control transfers needed          |
|     - Continuous re-send required, device reverts to its     |
|       own default graphic if writes stop                     |
|     - 4-byte chunk header 08 <idx> 00 <flag>; the chunk       |
|       containing the JPEG SOI marker is sent LAST within a    |
|       frame and carries idx == N (total chunks) and flag      |
|       0x80; all other chunks carry flag 0x00                  |
|     - 5ms delay between chunk writes; going faster wedges     |
|       the endpoint on at least one tested host and needs a    |
|       physical USB replug to recover                          |
|                                                             |
|   This file is part of the OpenRGB project                 |
|   SPDX-License-Identifier: GPL-2.0-or-later                |
\*---------------------------------------------------------*/

#pragma once

#include <atomic>
#include <QObject>
#include <QThread>
#include <QVector>
#include <QByteArray>
#include <QMutex>

struct hid_device_;
typedef struct hid_device_ hid_device;

class ThermaltakeAIODevice : public QObject
{
    Q_OBJECT

public:
    ThermaltakeAIODevice();
    ~ThermaltakeAIODevice();

    static constexpr unsigned short VENDOR_ID  = 0x264A;
    static constexpr unsigned short PRODUCT_ID = 0x233C;
    static constexpr int            PANEL_SIZE = 480;

    /*-----------------------------------------------------*\
    | Opens interface 1 of the panel. Returns false if the  |
    | device isn't connected.                                |
    \*-----------------------------------------------------*/
    bool                Open();
    void                Close();
    bool                IsConnected() const;

    /*-----------------------------------------------------*\
    | Replace the frame set being streamed. A single frame  |
    | streams as a static image; multiple frames cycle as   |
    | an animation, one frame per refresh.                  |
    \*-----------------------------------------------------*/
    void                SetFrames(const QVector<QByteArray>& jpeg_frames);

    /*-----------------------------------------------------*\
    | Start/stop the continuous background send loop. The   |
    | panel reverts to its own default graphic soon after   |
    | Stop() -- there is no set-and-forget on this device.  |
    \*-----------------------------------------------------*/
    void                Start();
    void                Stop();
    bool                IsStreaming() const;

private:
    void                RunLoop();
    static QVector<QByteArray> ChunkFrame(const QByteArray& jpeg_bytes);

    hid_device*         device;
    QThread*            worker_thread;
    std::atomic<bool>   streaming;

    QMutex              frames_mutex;
    QVector<QByteArray> frames;

    static constexpr int  CHUNK_PAYLOAD        = 1020;
    static constexpr int  CHUNK_TOTAL          = 1024;
    static constexpr int  INTER_CHUNK_DELAY_MS = 5;
    static constexpr int  REFRESH_INTERVAL_MS  = 50;   /* ~20fps target, matches captured device cadence */
};
