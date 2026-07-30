# OpenRGB Thermaltake AIO LCD Plugin

An [OpenRGB](https://gitlab.com/CalcProgrammer1/OpenRGB) plugin for the round
LCD screen on Thermaltake AIO CPU coolers (USB `264A:233C`, "HKC OVERSEAS
LIMITED 2.1 inch Round TFT LCD Display Module-B").

Lets you push a static image or animated GIF to the panel from OpenRGB
instead of Thermaltake's own software.

## Status

Early scaffold. Static images and animated GIFs both work. Not yet done:
CPU/GPU/RAM sensor overlay, WebM support, Windows/Mac testing (Linux only
so far), tray menu/icon.

## Building

Requires Qt (match whatever Qt major version your OpenRGB build uses --
mixing Qt5/Qt6 between the plugin and the host produces a plugin that
builds fine but silently fails to load) and `hidapi` (`hidapi-hidraw` or
`hidapi-libusb` on Linux).

```
git submodule update --init
qmake OpenRGBThermaltakeAIOPlugin.pro   # or qmake-qt5 / qmake6, matching your OpenRGB build
make
cp libOpenRGBThermaltakeAIOPlugin.so ~/.config/OpenRGB/plugins/
```

## Protocol notes

The panel has no documented protocol. It was reverse-engineered from a
USBPcap capture of Thermaltake's own control software. The short version:
only interface 1 (the 1024-byte HID interrupt endpoint) matters, the panel
must be continuously re-sent frames (it reverts to its own default graphic
if writes stop), and each JPEG frame is chunked with a 4-byte header where
the chunk containing the JPEG SOI marker is sent *last*, not first -- see
`ThermaltakeAIODevice.cpp` for the exact framing.

## License

GPL-2.0-or-later, matching OpenRGB itself.
