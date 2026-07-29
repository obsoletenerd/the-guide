# The Guide

Everything you need to build, flash, and run **The Guide** — an offline, searchable, text-only Wikipedia (or any Kiwix ZIM archive) reader — on your own Xteink X3 e-reader. This folder is meant to be self-contained: you shouldn't need anything outside `the-guide/` to use the project.

## What's here

- **[`converter/`](converter/)** — `guidec`, the desktop tool that turns a Kiwix ZIM file into a pack your device can read, plus a terminal UI for browsing packs on your computer before you have (or without needing) hardware. Run this first.
- **[`firmware/`](firmware/)** — the on-device software for the Xteink X3. Working on real hardware — see its README for current status.
- **[`web-flasher/`](web-flasher/)** — a browser-based flashing tool, so you don't need to install any command-line tools to put the firmware on your device. Working (Chrome/Edge only).

## Quick start

1. Get a ZIM archive (e.g. from https://download.kiwix.org/zim/wikipedia/) and build a pack: see [`converter/README.md`](converter/README.md).
2. Flash your X3 — either `pio run -e guide_x3 -t upload --upload-port /dev/cu.usbmodemXXXXXX` from `firmware/`, or open `web-flasher/index.html` in Chrome/Edge and click through.
3. Copy the pack onto the device's SD card as `/pack`, insert it, and boot.

## Hardware details

See [`../hardware/xteink-x3/`](../hardware/xteink-x3/) for the hardware writeup and the vendored `freeink-sdk` the firmware builds against.
