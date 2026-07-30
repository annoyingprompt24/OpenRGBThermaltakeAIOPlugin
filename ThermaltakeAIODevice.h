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
|     - 1ms delay between chunk writes is the shipped default,  |
|       extensively validated safe on this host; going much      |
|       lower (sub-500us) risks wedging the endpoint on at least   |
|       one tested host (needs a physical USB replug to recover)   |
|       -- tunable via THERMALTAKE_AIO_CHUNK_DELAY_US env var, and  |
|       refresh interval is also live-adjustable via the UI slider  |
|       (SetTargetFps) without needing a rebuild or relaunch          |
|                                                             |
|   This file is part of the OpenRGB project                 |
|   SPDX-License-Identifier: GPL-2.0-or-later                |
\*---------------------------------------------------------*/

#pragma once

#include <atomic>
#include <chrono>
#include <QObject>
#include <QThread>
#include <QVector>
#include <QByteArray>
#include <QImage>
#include <QMutex>
#include "ThermaltakeAIOSensors.h"

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
    | Replace the image set being streamed. A single image  |
    | streams as a static picture; multiple images cycle as |
    | an animation, one per refresh. Each frame's JPEG is    |
    | (re-)encoded lazily, right before it's sent, only if    |
    | it's stale for the current render_version -- NOT all    |
    | frames at once. A many-hundred-frame GIF re-encoding     |
    | synchronously on every overlay/sensor update was a real  |
    | bug (multi-second stall each time), see render_version.  |
    \*-----------------------------------------------------*/
    void                SetImages(const QVector<QImage>& images);

    /*-----------------------------------------------------*\
    | Toggle the CPU/GPU/RAM temperature overlay. Sensor     |
    | values are re-read at most once a second regardless    |
    | of the panel's own refresh rate.                        |
    \*-----------------------------------------------------*/
    void                SetOverlayEnabled(bool enabled);

    /*-----------------------------------------------------*\
    | Debug aid: draws "Frame i/N" on every sent frame, for   |
    | pinpointing exactly which frame index(es) glitch on real |
    | hardware when paired with a slowed-down Target FPS.       |
    \*-----------------------------------------------------*/
    void                SetDebugFrameIndexEnabled(bool enabled);

    /*-----------------------------------------------------*\
    | Start/stop the continuous background send loop. The   |
    | panel reverts to its own default graphic soon after   |
    | Stop() -- there is no set-and-forget on this device.  |
    \*-----------------------------------------------------*/
    void                Start();
    void                Stop();
    bool                IsStreaming() const;

    /*-----------------------------------------------------*\
    | Live-adjustable target refresh rate, takes effect on   |
    | the next send cycle without needing to Stop()/Start(). |
    | The actual achieved rate may be lower if a frame's own  |
    | chunk count takes longer to send than 1000/fps allows.   |
    \*-----------------------------------------------------*/
    void                SetTargetFps(int fps);
    int                 GetTargetFps() const;

private:
    void                RunLoop();
    static QVector<QByteArray> ChunkFrame(const QByteArray& jpeg_bytes);
    static void         DrawOverlay(QImage* image, const ThermaltakeAIOSensorReadings& readings);

    hid_device*         device;
    QThread*            worker_thread;
    std::atomic<bool>   streaming;
    std::atomic<bool>   overlay_enabled;
    std::atomic<bool>   debug_frame_index_enabled;

    QMutex              images_mutex;
    QVector<QImage>     images;

    /*-----------------------------------------------------*\
    | images_version bumps only when SetImages() replaces the |
    | source images (RunLoop needs to re-fetch from images     |
    | under lock when this changes). render_version bumps on   |
    | that AND on overlay toggle AND on each sensor refresh --  |
    | any per-frame encoded-JPEG cache entry stamped with an     |
    | older render_version is stale and gets re-encoded lazily,  |
    | one frame at a time, right before that frame is next up     |
    | to send. RunLoop-only cache state needs no mutex.            |
    \*-----------------------------------------------------*/
    std::atomic<int>    images_version;
    std::atomic<int>    render_version;

    ThermaltakeAIOSensorReadings          cached_readings;
    std::chrono::steady_clock::time_point cached_readings_time;

    static constexpr int  CHUNK_PAYLOAD          = 1020;
    static constexpr int  CHUNK_TOTAL            = 1024;
    static constexpr int  SENSOR_REFRESH_INTERVAL_MS = 1000;
    static constexpr int  ACK_TIMEOUT_MS         = 200;

    /*-----------------------------------------------------*\
    | Tunable at runtime via env vars for frame-rate tuning  |
    | experiments (THERMALTAKE_AIO_CHUNK_DELAY_US /          |
    | THERMALTAKE_AIO_REFRESH_MS) without needing a rebuild  |
    | per test. Defaults are the values already confirmed    |
    | safe on real hardware -- 5ms/chunk is the known floor  |
    | that doesn't wedge the USB endpoint, 50ms/frame matches |
    | the real device's own observed refresh cadence.         |
    |                                                         |
    | THERMALTAKE_AIO_ACK_PACE=1 switches from a fixed wall-  |
    | clock sleep between frames to waiting for the panel's    |
    | own 16-byte EP4 reply (sent after each frame's commit     |
    | chunk) before starting the next one -- syncing our send   |
    | cadence to the device's actual readiness instead of a      |
    | guessed rate. Off by default pending validation; a race    |
    | between our writes and the panel's internal scan-out was    |
    | suspected early in the project (horizontal tearing) but      |
    | every "clean" test since then used static/near-static         |
    | content, which can't reveal a tear since old and new bytes      |
    | are identical -- fast-changing animated content exposed it.      |
    \*-----------------------------------------------------*/
    int               inter_chunk_delay_us;
    std::atomic<int>  refresh_interval_ms;
    bool              ack_pace;
};
