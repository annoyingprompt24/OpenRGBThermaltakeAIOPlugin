# OpenRGB Thermaltake AIO LCD Plugin

An [OpenRGB](https://gitlab.com/CalcProgrammer1/OpenRGB) plugin for the round
LCD screen on Thermaltake AIO CPU coolers (USB `264A:233C`, "HKC OVERSEAS
LIMITED 2.1 inch Round TFT LCD Display Module-B").

Lets you push a static image or animated GIF to the panel from OpenRGB
instead of Thermaltake's own software.

## Status

Working. Static images and animated GIFs stream cleanly, with a live system
monitor overlay, brightness control, and a persistent standby image.

Features:
- **Static images and animated GIFs** streamed to the panel (with a background
  colour for images that have transparency).
- **No frame tearing.** Fast-changing GIFs used to tear along the top of the
  panel; sending the JPEG's SOI chunk first (rather than last) eliminates it --
  see the protocol notes below. Set `THERMALTAKE_AIO_SOI_LAST=1` to restore the
  old order for comparison.
- **Radial gauge overlay** showing CPU / GPU / RAM **temperature** or
  **utilisation**, with a per-module colour picker. Turns the panel into a
  little system monitor (Linux sensor reads: `hwmon` for CPU/RAM temp, DDR5
  SPD for RAM, `nvidia-smi`/`amdgpu` for the GPU).
- **Brightness slider** and a **Set Standby Image** button (writes an image to
  the panel's own flash so it persists while the PC is idle / powered off).
- Live **target-FPS** slider.

Not yet done: WebM support, Windows/Mac testing (Linux only so far),
tray menu/icon.

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
only interface 1 (the 1024-byte HID interrupt endpoint) carries the image
stream, the panel must be continuously re-sent frames (it reverts to its own
default graphic if writes stop), and each JPEG frame is split into chunks with
a 4-byte header. The chunk containing the JPEG SOI marker is tagged with a
`0x80` flag byte and an index of `N` (the frame's chunk count), regardless of
where it sits on the wire.

The vendor's software transmits that flagged SOI chunk *last*. Doing the same
works, but tears fast-changing frames along the top of the panel: the panel
can't begin decoding until the SOI arrives, so it receives every other chunk,
then gets the SOI and repaints the whole frame top-down in a burst that races
its own refresh at row 0. Transmitting the SOI chunk **first** (natural order,
same bytes, same headers) removes that race and the tearing with it -- this is
the default. Interface 0 (a separate 440-byte HID endpoint) carries brightness,
the standby/boot flash-write protocol, and telemetry. See
`ThermaltakeAIODevice.cpp` for the exact framing.

## License

GPL-2.0-or-later, matching OpenRGB itself.
