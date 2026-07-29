# Web flasher

A static page using [ESP Web Tools](https://esphome.github.io/esp-web-tools/) so anyone can flash The Guide onto an X3 over USB from a browser — no PlatformIO/esptool install needed.

## Status

Wired up and working as of Phase X4 (`../firmware/`'s current state -- search/reader UI, deep sleep, battery/clock). `firmware/` here holds a real build (`bootloader.bin`/`partitions.bin`/`firmware.bin`), and the offsets in `manifest.json` (`0x0` / `0x8000` / `0x10000`) are verified against `../firmware/`'s actual `pio run -t upload -v` output, not just assumed ESP32-C3 defaults -- confirmed to match. (`pio`'s own upload also writes a stock `boot_app0.bin` at `0xE000`, but that's inert here: `../firmware/partitions.csv` has no `otadata` partition for anything to read it from, so the web flasher's 3-part manifest is complete as-is -- no 4th part needed.)

**This snapshot goes stale the moment `../firmware/` changes.** There's no build automation wiring the two together yet, so after any firmware change:

1. Rebuild: `pio run -e guide_x3` from `../firmware/`.
2. Copy the three artifacts over the ones here, replacing them:
   ```bash
   cp ../firmware/.pio/build/guide_x3/bootloader.bin firmware/
   cp ../firmware/.pio/build/guide_x3/partitions.bin firmware/
   cp ../firmware/.pio/build/guide_x3/firmware.bin firmware/
   ```
3. **Re-verify the offsets** if `../firmware/partitions.csv` or `platformio.ini`'s `board_upload.offset_address` ever changes -- `pio run -t upload -v --upload-port <anything>` prints the real esptool command (and its offsets) even without a device actually connected, since it fails only once it tries to open the port.
4. Bump `"version"` in `manifest.json` so returning users get the "update available" prompt instead of silently reflashing the same build.

To test locally: `python3 -m http.server` from this directory, then open `http://localhost:8000` in Chrome or Edge (see Browser requirements below -- Web Serial needs a secure context, and `localhost` counts as one).

## Browser requirements

- **Chrome or Edge, desktop only** — Web Serial isn't implemented in Firefox, Safari, or any mobile browser.
- **Secure context** — serve over HTTPS, or from `http://localhost` for local testing (`python3 -m http.server` from this directory works fine for local testing; plain `http://` on a non-localhost address will not).

## Hosting

Not decided yet. Options once there's a real build to ship: GitHub Pages off this repo (needs the repo public, or a Pages-only mirror), or bundling the page + `firmware/` as a downloadable zip users open locally. Pick this when there's an actual firmware build to distribute — no point deciding earlier.
