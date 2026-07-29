# Packs

- **`sample/`** — a small, committed sample pack (the top-100 Wikipedia articles, built with `--window-log 11` so it also works on the PSRAM-less Xteink X3) — just here so the repo has something to browse right away.
- **`computer/`** — a larger local-only test pack (`wikipedia_en_computer_nopic`, ~44k articles). Not committed (its shard file is over GitHub's 100 MB file limit) — gitignored, kept locally for scale testing.

Everything except `sample/` is gitignored — build your own with `the-guide/converter/guidec.py` (see [`the-guide/converter/README.md`](../the-guide/converter/README.md)):

```bash
cd the-guide/converter
uv run guidec.py build --zim your-file.zim --out ../../packs/whatever
```

A curated ~50-article "interesting/useful things" pack is planned to replace `sample/` eventually.
