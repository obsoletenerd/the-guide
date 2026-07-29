# Firmware

A PlatformIO project targeting the Xteink X3 (ESP32-C3, no PSRAM, UC8253 e-ink), built on the third-party **freeink-sdk** and reusing pieces of the **CrossPoint Reader** firmware (both MIT). See [`../../hardware/xteink-x3/PLAN.md`](../../hardware/xteink-x3/PLAN.md) for the full hardware writeup, RAM budget, button scheme, and phased build plan.

Confirmed working on real hardware: Panel + buttons, SD/pack access (on-disk search, streaming zstd decompress to an SD cache file), a paginated reader, and the full search/list UI wired to it (async button input, letter-cycling text entry). See PLAN.md #6 for what each phase covers, and PLAN.md #2 for the RAM budget -- corrected during X3 bring-up after real hardware testing found the original estimate was badly wrong (packs need `--window-log 11`, not 16 -- rebuild any existing pack before copying it to a card).

Deep sleep / GPIO3 wake / NVS resume (the core of Phase X4) is also done and confirmed on real hardware, including battery-only operation -- idle timeout or a long-press on POWER saves state and shows a cover graphic (`src/cover_bitmap.h`, regenerate with `tools/img_to_bitmap.py`), and POWER wakes it back to the same article/page or search query. The top bar also shows battery %/clock now (`settime YYYY-MM-DD HH:MM:SS` over Serial seeds the DS3231 once; its coin-cell backup keeps it after that), and the bottom bar's BACK button jumps the search-list selection back to the search box from wherever it's scrolled to. The results list is also properly paginated now (PgUp/PgDn on the two right-side bottom buttons, plus scrolling past a page edge flips to the next page instead of animating) with a "Page N/M" toast, and refresh mode is content-triggered rather than a periodic counter -- full refresh only on an actual page flip (list or reader), fast/debounced for everything else. Still open from Phase X4: the one leftover Phase X3 button-mapping question (Reader-mode LEFT/RIGHT/CONFIRM).

```bash
pio run -e guide_x3 -t upload --upload-port /dev/cu.usbmodemXXXXXX
```

The build artifacts also feed [`../web-flasher/`](../web-flasher/) so people who don't want PlatformIO installed can flash from a browser instead (not wired up yet -- see that directory's README).
