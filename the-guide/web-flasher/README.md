# Web flasher

A static page using [ESP Web Tools](https://esphome.github.io/esp-web-tools/) so anyone can flash The Guide onto an X3 over USB from a browser without PlatformIO/esptool install needed.

## Status

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

To test locally: `python3 -m http.server` from this directory, then open `http://localhost:8000` in a compatible browser (see Browser requirements below -- Web Serial needs a secure context, and `localhost` counts as one).

## Browser requirements

- **Chrome/Edge or Firefox, desktop only** - Web Serial isn't implemented in Safari, or any mobile browser.
- **Secure context** - serve over HTTPS, or from `http://localhost` for local testing (`python3 -m http.server` from this directory works fine for local testing; plain `http://` on a non-localhost address will not).

## Hosting

I will host this web flasher somewhere soon.
