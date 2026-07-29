![The Guide on Xteink X3](https://github.com/obsoletenerd/the-guide/blob/main/meta/the-guide-cover.jpg?raw=true)

# The Guide

Life-long project idea of building the "real" Hitchhikers Guide to the Galaxy (the device in the novel, not the novel itself) by putting Wikipedia in my pocket/bag, fully offline and title-searchable. Finally possible thanks to ESP32's and e-ink displays. I've done this same idea many times over the decades, from a Netbook in my backpack in the 2000s to a Raspberry Pi with a 7" LCD display and a huge battery bank, to apps on my iPhone, but none felt like the real HHGTTG until this project now.

This puts basically all of English wikipedia (text only for now, I have some ideas) onto an off-the-shelf mini eBook reader. Currently works on the Xteink X3 and also aiming to get it working on the X4. 

This project is just built for myself and some friends. It may or may not work for you, but it *worked on my machine*.

![The Guide on Xteink X3](https://github.com/obsoletenerd/the-guide/blob/main/meta/the-guide-search.jpg?raw=true)

## Hardware Tested

**Xteink X3** — ESP32-C3, no PSRAM, 792×528 UC8253 e-ink, 6 buttons + power, microSD, BQ27220 fuel gauge, DS3231 RTC. See [`hardware/xteink-x3/`](hardware/xteink-x3/).

## Layout

- **[`the-guide/`](the-guide/)** — the converter, the device firmware, and the browser-based flasher. See its README.
- **[`packs/`](packs/)** — pack data. Only a small sample is committed; see its README for building your own.
- **[`hardware/xteink-x3/`](hardware/xteink-x3/)** — hardware notes, plus the vendored `freeink-sdk` the firmware builds against (symlinked from `the-guide/firmware/platformio.ini`).

## Status

- **Converter** (`the-guide/converter/`) — done. Builds a pack from a ZIM file; tested to 431 MB / ~44k articles (~144 s build, 370 MB peak RSS, sub-millisecond search). Full English Wikipedia (~7M articles) is untested.
- **Firmware** (`the-guide/firmware/`) — done, running on real hardware: buttons, SD/pack search, paginated reader, deep sleep with wake-resume, battery %/clock. Still open: Reader-mode LEFT/RIGHT/CONFIRM button behavior isn't decided yet.
- **Web flasher** (`the-guide/web-flasher/`) — working, but goes stale whenever firmware changes (manual rebuild step — see its README).

## Quick start

1. Build a pack (or use the bundled `packs/sample/`): `cd the-guide/converter && uv run guidec.py build --zim your.zim --out ../../packs/whatever`.
2. Flash the X3: `pio run -e guide_x3 -t upload --upload-port /dev/cu.usbmodemXXXXXX` from `the-guide/firmware/`, or open `the-guide/web-flasher/index.html` in Chrome/Edge.
3. Copy the pack onto the SD card as `/pack`, insert it, boot.
