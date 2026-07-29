# guidec — The Guide's ZIM-to-pack converter

Converts a Kiwix ZIM archive into a custom pack format designed for random access on a microcontroller with no OS filesystem cache: sharded article blobs, a sorted title index, and a sparse search-acceleration index, all readable by seek + binary search alone. 

Also includes a curses TUI that doubles as a simulator for the device's button-driven UI.

## Details

- **`guidec build`** — converts a Kiwix ZIM file into a pack: cleaned articles in a line-oriented markup ("GuideText"), zstd-compressed one frame per article, sharded into <4 GB files (FAT32-safe), with a sorted title index, sparse search-acceleration index, and fixed-size article location table. A malformed page can't abort a long run (it's logged to `build.log` and emitted as a stub), and articles are capped at ~2 MB of text to fit the device's RAM budget.
- **`guidec read`** — looks up one article by title and prints it, using the exact on-disk lookup path the device firmware will use (sparse index → binary search → seek → decompress).
- **`guidec tui`** — a curses browser that doubles as a simulator for the button-driven hardware UI (see [The TUI](#the-tui--hardware-ux-simulator) below). Search is the same on-disk algorithm the device will run, so it stays instant regardless of pack size.
- **`guidec selftest`** — checks normalization, HTML conversion, link rewriting, meta-refresh redirect detection, index round-trips, and multi-shard reads.

Handled correctly: ZIM redirect entries, meta-refresh redirect stub pages (the ZIM's redirects-to-a-section, which look like empty articles if treated naively), internal links rewritten to article IDs at build time, title normalization (diacritics, case, punctuation), empty-section removal, multi-shard packs (verified byte-identical against single-shard builds).

Scale so far: a 431 MB ZIM (`wikipedia_en_computer_nopic`, ~44k articles + 200k redirects) builds in ~144 s at 370 MB peak RSS with zero conversion failures, and browses with sub-millisecond search. Full English Wikipedia (~7M articles) is the next milestone — see the main project `PLAN.md`.

## Usage

Requires [uv](https://docs.astral.sh/uv/). Dependencies (`libzim`, `zstandard`, `selectolax`, `tqdm`) are declared in `pyproject.toml`.

```bash
cd the-guide/converter

# 1. Put a ZIM file somewhere — e.g. from https://download.kiwix.org/zim/wikipedia/
curl -L -o wikipedia_en_100_nopic_2026-04.zim \
  https://download.kiwix.org/zim/wikipedia/wikipedia_en_100_nopic_2026-04.zim

# 2. Build a pack
uv run guidec.py build --zim wikipedia_en_100_nopic_2026-04.zim --out pack
#    --limit N      to convert only the first N articles
#    --level L      zstd level (default 9)
#    --shard-cap N  max shard bytes (default just under 4 GB; tiny values
#                   exercise multi-shard rollover for testing)
#    --window-log N cap the zstd decompression window -- needed for the
#                   PSRAM-less Xteink X3; use N=11, not a larger "should be
#                   fine" guess (hardware-measured, see hardware/xteink-x3/
#                   PLAN.md #2 -- the window size is not what mainly drives
#                   the device's decompressor RAM use)

# 3. Read / browse
uv run guidec.py read --pack pack "Bob Dylan"
#    --stream  decompress via the chunked streaming API instead of one-shot,
#              and assert it matches -- the same self-check that caught a
#              real bug (unbounded reads spilling into the next article's
#              frame) before the X3 firmware's C port hit the same mistake
uv run guidec.py tui --pack pack
```

## The TUI — hardware UX simulator

The Xteink X3 has no keyboard — just 6 physical buttons (2 side buttons plus a 4-zone front rocker) — so the TUI maps keyboard keys onto exactly those buttons instead of designing its own interaction. It exists to let the interaction design be tuned on a desktop before (or without) hardware. The scheme below is the finalized one from `hardware/xteink-x3/PLAN.md` #1 and matches what `the-guide/firmware/src/main.cpp` actually runs on the device — this is not a desktop-only approximation.

| Key | Button | Physical location | List/Search focus | Typing (query entry) | Reader |
|-----|--------|--------------------|--------------------|-----------------------|--------|
| `[` | `BTN_UP` | left side | selection up (the search box is list position 0, reachable by scrolling all the way up) | leave typing, move selection up (keeps what's typed) | page back (history) |
| `]` | `BTN_DOWN` | right side | selection down | leave typing, move selection into the results | page forward |
| `1` | `BTN_BACK` | front, zone 1 | jump selection back to the search box | backspace (clears an uncommitted slot first) | back to Search (restores prior query + scroll position) |
| `2` | `BTN_CONFIRM` | front, zone 2 | on the box: enter typing. on a result: open it | commit the slot, open a new one, re-filter results | — (open question, see firmware README) |
| `3` | `BTN_LEFT` | front, zone 3 | page up (previous screenful of the list) | cycle the current slot's character backward | — |
| `4` | `BTN_RIGHT` | front, zone 4 | page down (next screenful of the list) | cycle the current slot's character forward | — |

The screen layout matches the device too: one continuous list with the search box pinned at row 0 and results below it (no side column), a top title bar, and a bottom hint bar showing what `1`–`4` currently do — plus a `[ … ]` hint row for the two side buttons. Opening a result switches to a full-screen, paginated Reader; `1` returns to Search exactly where you left it.

**Typing** is character-cycling entry, like arcade high-score screens: a cursor cell shows the letter being cycled, `3`/`4` step it backward/forward and `2` commits it, opening a new slot. Matching the firmware exactly, the result list only re-filters on commit, not on every cycle step. The character set is `" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'-."` — space and a little title punctuation included, same as the firmware's `kCycle` — so multi-word titles can be matched past their first word.

Example: to find "CD-ROM", press `2` to start typing, `4444` to cycle to `C`, `2` to commit it, `4444` for `D`, `2` to commit — the list has already narrowed to `cd…` titles — then `]` to drop into the results, `[`/`]` to pick the entry, `2` to open it.

Esc or Ctrl-C quits — a desktop-only convenience, not a hardware button.

One thing the device firmware hasn't decided yet, and this TUI mirrors by leaving it a no-op too: `2`/`3`/`4` (CONFIRM/LEFT/RIGHT) do nothing in Reader mode — see `the-guide/firmware/README.md` and `hardware/xteink-x3/PLAN.md` #1 for that open question.

## Pack format

```
pack/
  pack.json      metadata (format version, counts, source ZIM)
  titles.bin     sorted records: norm_key \0 display_title \0 article_id:u32le
  titles.sparse  u32le byte offset of every 64th titles.bin record
  articles.bin   per article ID: shard:u16le offset:u32le length:u32le
  shard_NNNN.dat one zstd frame per article, shards capped below 4 GB
```

Redirects are extra rows in `titles.bin` pointing at the canonical article ID. Links appear in article text as `[visible text|article_id]`.

## Current limitations

- **Not yet tested at full-Wikipedia scale** (~7M articles): the build keeps its title/path maps in RAM and sorts titles in memory (unmeasured at 7M scale — fallback is an external sort if it blows up). Mid-size ZIMs (hundreds of thousands of entries) build and browse fine; the TUI and `read` both use the on-disk sparse-index search, so title count doesn't affect startup or memory.
- **Failed conversions become stubs**: a malformed page is logged to `build.log` in the pack directory and emitted as a stub article rather than aborting the run.
- **Articles are capped at ~2 MB** of GuideText (truncated with a visible marker) to fit the device's RAM budget.
- **Formatting is minimal by design**: headings, paragraphs, bullets, numbered lists, links. No tables, images, math, bold/italic, references.
- **Link syntax is unescaped**: literal `[text|123]` in article prose would be misread as a link. Parsers must require the exact `[…|digits]` shape.
- **Search is title-prefix only** — no fuzzy or full-text search (deliberate; see the main project `PLAN.md`).
- **Links aren't followed**, in the TUI or on the device: `[text|id]` markup is stripped down to its visible text at render time. Following links is an open question for the firmware's Reader mode (see `the-guide/firmware/README.md`).
