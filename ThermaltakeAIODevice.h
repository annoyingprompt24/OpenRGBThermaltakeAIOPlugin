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
#include <QPainter>
#include <QRectF>
#include <QPointF>
#include <QColor>
#include <QString>
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
    | What the radial gauge overlay renders. Off draws       |
    | nothing; Temperature and Utilization share the same    |
    | three-ring layout and per-module colours, differing    |
    | only in the data source and the readout suffix          |
    | (degrees vs percent).                                   |
    \*-----------------------------------------------------*/
    enum class OverlayMode
    {
        Off         = 0,
        Temperature = 1,
        Utilization = 2,
    };

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
    | Select the radial gauge overlay's data source (or Off).|
    | Sensor values are re-read at most once a second        |
    | regardless of the panel's own refresh rate. Switching   |
    | mode re-snaps the eased display values to the new        |
    | metric so a temp reading doesn't visibly slide toward a  |
    | percentage (or vice versa).                              |
    \*-----------------------------------------------------*/
    void                SetOverlayMode(OverlayMode mode);

    /*-----------------------------------------------------*\
    | Per-module ring/readout colours, shared across the     |
    | temperature and utilization views. Safe to call from   |
    | the UI thread while the send loop is running.           |
    \*-----------------------------------------------------*/
    void                SetModuleColors(const QColor& cpu, const QColor& gpu, const QColor& ram);

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

    /*-----------------------------------------------------*\
    | Panel backlight brightness, 0-100 linear percent.      |
    | Carried in byte 4 of the interface-0 0x12 report (the  |
    | same report SendResyncCommand() sends) -- confirmed by  |
    | a Windows USBPcap capture of the TT RGB PLUS slider:     |
    | 0x00-0x64 maps linearly to 0-100%, applied immediately   |
    | with no separate commit step. Takes effect whenever the   |
    | device is connected, independent of streaming.             |
    \*-----------------------------------------------------*/
    void                SetBrightness(int percent);
    int                 GetBrightness() const;

    /*-----------------------------------------------------*\
    | Persistent Standby image. Unlike the live "General"    |
    | stream (interface 1, lost on power-off), this writes    |
    | the JPEG into the panel's onboard flash over the         |
    | interface-0 0x0a/0x0b flash-write protocol reverse-       |
    | engineered from a Windows TT RGB PLUS capture and          |
    | validated byte-exact against the vendor's own wire bytes.   |
    | Blocking (~200ms for a typical JPEG); call off the UI       |
    | thread if freezing matters. Returns false on write error.   |
    \*-----------------------------------------------------*/
    bool                SetStandbyImage(const QImage& image);

    /*-----------------------------------------------------*\
    | The shared flash-write primitive behind SetStandbyImage |
    | (and, later, boot animation). is_animation sets the      |
    | 0x0a announce flag field (0 = static, 1 = animation).     |
    \*-----------------------------------------------------*/
    bool                FlashBlob(const QByteArray& data, bool is_animation);

private:
    void                RunLoop();
    static QVector<QByteArray> ChunkFrame(const QByteArray& jpeg_bytes, bool soi_first);

    /*-----------------------------------------------------*\
    | Draws the CPU/GPU/RAM radial gauges. Not static -- it   |
    | needs the per-instance smoothed_* state below, which     |
    | eases toward each new sensor reading a little every        |
    | frame so the needle moves continuously even though raw       |
    | sensor reads only happen once/second (SENSOR_REFRESH_          |
    | INTERVAL_MS). Called every frame while the overlay is on,       |
    | RunLoop-thread-only, no mutex needed.                             |
    \*-----------------------------------------------------*/
    void                DrawOverlay(QImage* image, const ThermaltakeAIOSensorReadings& readings, OverlayMode mode);
    static void         DrawRing(QPainter& painter, const QPointF& center, float radius, float thickness,
                                  float value, float min_val, float max_val, const QColor& color);

    /*-----------------------------------------------------*\
    | Sends the one-way, unreplied report seen on interface  |
    | 0 (EP1 OUT, report ID 0x12, 440 bytes: 12 01 00 80 64  |
    | then zero-padded) in a real TT RGB PLUS capture. It      |
    | appeared exactly 4 times in a 168s session, always       |
    | right as continuous EP3 streaming was about to start      |
    | or right as the streamed content source was about to       |
    | change (idle graphic -> loading animation -> user's final    |
    | image) -- never once during steady unchanging playback.        |
    | Experimental: testing whether replicating it (which we never    |
    | send at all currently) affects the per-frame GIF corruption.      |
    \*-----------------------------------------------------*/
    void                SendResyncCommand();

    hid_device*         device;
    hid_device*         device_iface0;
    QThread*            worker_thread;
    std::atomic<bool>   streaming;
    std::atomic<OverlayMode> overlay_mode;
    std::atomic<bool>   debug_frame_index_enabled;
    std::atomic<int>    brightness;

    /*-----------------------------------------------------*\
    | Per-module colours as packed QRgb so they can be read  |
    | lock-free from the send loop while the UI thread sets   |
    | them (QColor itself isn't atomic).                       |
    \*-----------------------------------------------------*/
    std::atomic<QRgb>   module_color_cpu;
    std::atomic<QRgb>   module_color_gpu;
    std::atomic<QRgb>   module_color_ram;

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

    /*-----------------------------------------------------*\
    | Eased-toward-target display values so the gauge markers |
    | move smoothly every frame. These hold whatever the      |
    | active overlay mode's metric is (temp or util) -- a      |
    | mode switch clears smoothing_initialized so they snap    |
    | to the new metric instead of sliding across from the     |
    | old one's scale.                                         |
    \*-----------------------------------------------------*/
    float               smoothed_cpu;
    float               smoothed_gpu;
    float               smoothed_ram;
    std::atomic<bool>   smoothing_initialized;

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

    /*-----------------------------------------------------*\
    | Transmit the SOI/flagged chunk first (default) vs the |
    | original SOI-last order. SOI-first eliminates the top- |
    | of-frame tearing on hardware. THERMALTAKE_AIO_SOI_LAST=1|
    | restores SOI-last. See ChunkFrame().                  |
    \*-----------------------------------------------------*/
    bool              soi_first;
};
