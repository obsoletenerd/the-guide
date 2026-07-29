#!/usr/bin/env python3
"""guidec — build/browse Guide packs from Kiwix ZIM files.

Commands:
  build --zim FILE --out DIR [--limit N] [--level L]   convert ZIM -> pack
  read  --pack DIR "Title"                             lookup + print one article
  tui   --pack DIR                                     curses browser
  selftest                                             run internal checks

Pack format (see PLAN.md):
  pack.json      metadata
  titles.bin     sorted records: norm_key \\0 display_title \\0 article_id:u32le
  titles.sparse  u32le byte offset of every 64th record in titles.bin
  articles.bin   per article ID: shard:u16le offset:u32le length:u32le
  shard_NNNN.dat zstd frame per article, shards capped below 4 GB (FAT32)
"""

import argparse
import json
import re
import struct
import sys
import unicodedata
from pathlib import Path

STRIDE = 64
SHARD_CAP = 3_500_000_000
ART_CAP = 2_000_000  # max decompressed GuideText bytes (ESP32 PSRAM budget)
ART_REC = struct.Struct("<HII")  # shard, offset, length
LINK_RE = re.compile(r"\[([^\]|]*)\|(\d+)\]")

# ---------------------------------------------------------------- normalize

def norm_key(title: str) -> str:
    t = unicodedata.normalize("NFKD", title)
    t = "".join(c for c in t if not unicodedata.combining(c)).lower()
    t = "".join(c if c.isalnum() else " " for c in t.replace("_", " "))
    t = " ".join(t.split())
    return t or title.lower()

# ---------------------------------------------------------------- HTML -> GuideText

STRIP_TAGS = ("table style script nav aside figure img svg math video audio "
              "noscript iframe form input button object map link meta").split()
STRIP_CSS = [".mw-editsection", ".reflist", ".navbox", ".infobox", ".hatnote",
             ".thumb", ".metadata", ".catlinks", ".toc", ".sidebar", ".noprint",
             ".gallery", ".refbegin", "sup.reference", ".mw-references-wrap",
             ".portalbox", ".sistersitebox", ".mw-jump-link", "#toc"]
CONTAINERS = {"body", "div", "section", "main", "article", "details", "summary",
              "blockquote", "center", "span"}
HEADINGS = {"h1": "#", "h2": "##", "h3": "###", "h4": "###"}


def _clean(text: str) -> str:
    return " ".join(text.split())


def _href_to_path(href: str) -> str | None:
    from urllib.parse import unquote
    if not href or href.startswith(("http:", "https:", "mailto:", "#")):
        return None
    path = unquote(href.split("#")[0])
    while path.startswith("./") or path.startswith("../"):
        path = path[path.index("/") + 1:]
    if path.startswith("A/"):
        path = path[2:]
    return path or None


def _resolve_href(href: str, path2id: dict) -> int | None:
    path = _href_to_path(href)
    return path2id.get(path) if path else None


def _inline(node, path2id: dict) -> str:
    return _clean(_inline_raw(node, path2id))


def _inline_raw(node, path2id: dict) -> str:
    parts = []
    for c in node.iter(include_text=True):
        if c.tag == "-text":
            parts.append(c.text_content or "")
        elif c.tag == "a":
            text = _clean(c.text(deep=True))
            if not text:
                continue
            aid = _resolve_href(c.attributes.get("href") or "", path2id)
            # ponytail: '[', '|', ']' in article text can collide with link
            # syntax; device parser must require [text|digits] exactly
            parts.append(f"[{text}|{aid}]" if aid is not None else text)
        elif c.tag in STRIP_TAGS:
            continue
        else:
            parts.append(_inline_raw(c, path2id))
    return "".join(parts)


def _walk(node, out: list, path2id: dict):
    for c in node.iter():
        tag = c.tag
        if tag in HEADINGS:
            text = _clean(c.text(deep=True))
            if text:
                out.append(f"{HEADINGS[tag]} {text}")
        elif tag == "p":
            text = _inline(c, path2id)
            if text:
                out.append(text)
        elif tag in ("ul", "ol"):
            n = 0
            for li in c.iter():
                if li.tag != "li":
                    continue
                n += 1
                text = _inline(li, path2id)
                if text:
                    out.append(f"{n}. {text}" if tag == "ol" else f"* {text}")
        elif tag in CONTAINERS:
            _walk(c, out, path2id)


def html_to_guidetext(html: str, title: str, path2id: dict) -> str:
    from selectolax.parser import HTMLParser
    tree = HTMLParser(html)
    for sel in STRIP_TAGS + STRIP_CSS:
        for n in tree.css(sel):
            n.decompose()
    out = [f"# {title}"]
    if tree.body is not None:
        _walk(tree.body, out, path2id)
    # drop headings for sections that ended up empty (References etc.)
    kept = []
    for i, line in enumerate(out):
        if line.startswith("#"):
            nxt = out[i + 1] if i + 1 < len(out) else "#"
            if nxt.startswith("#") and i > 0:
                continue
        kept.append(line)
    return "\n".join(kept)

# ---------------------------------------------------------------- build

META_REFRESH_RE = re.compile(r'http-equiv=["\']refresh["\']', re.I)
META_URL_RE = re.compile(r'URL=[\'"]?([^\'">]+)', re.I)


def meta_refresh_target(html: str) -> str | None:
    """Path a meta-refresh stub page redirects to, or None."""
    if not META_REFRESH_RE.search(html):
        return None
    m = META_URL_RE.search(html)
    return _href_to_path(m.group(1)) if m else None


def _iter_entries(archive):
    for i in range(archive.all_entry_count):
        yield archive._get_entry_by_id(i)


def _resolve_redirect(entry, depth=5):
    while entry.is_redirect and depth > 0:
        entry = entry.get_redirect_entry()
        depth -= 1
    return entry


def build(args):
    import zstandard
    from libzim.reader import Archive
    from tqdm import tqdm

    if args.window_log:
        params = zstandard.ZstdCompressionParameters.from_level(
            args.level, window_log=args.window_log)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    archive = Archive(Path(args.zim))

    # pass 1: assign IDs to html articles, collect redirects
    path2id, titles = {}, []  # titles: (key, display, id)
    redirects = []            # (display_title, target_path)
    meta_redirects = {}       # stub path -> target path (meta-refresh pages)
    for entry in tqdm(_iter_entries(archive), total=archive.all_entry_count,
                      desc="pass 1: index", unit="entry"):
        if entry.is_redirect:
            target = _resolve_redirect(entry)
            if not target.is_redirect:
                redirects.append((entry.title, target.path))
            continue
        item = entry.get_item()
        if not item.mimetype.startswith("text/html"):
            continue
        # ponytail: meta-refresh redirect stubs are tiny; only sniff small pages
        if item.size < 4096:
            html = bytes(item.content).decode("utf-8", errors="replace")
            target = meta_refresh_target(html)
            if target:
                meta_redirects[entry.path] = target
                redirects.append((entry.title, target))
                continue
        if args.limit and len(path2id) >= args.limit:
            continue
        aid = len(path2id)
        path2id[entry.path] = aid
        titles.append((norm_key(entry.title), entry.title, aid))
    articles = dict(path2id)  # real articles only; path2id gains aliases below

    def follow(path, depth=5):
        while path in meta_redirects and depth > 0:
            path, depth = meta_redirects[path], depth - 1
        return path

    # alias stub paths to their target's ID so article links resolve too
    for stub, _ in meta_redirects.items():
        aid = path2id.get(follow(stub))
        if aid is not None:
            path2id[stub] = aid
    for title, target_path in redirects:
        aid = path2id.get(follow(target_path))
        if aid is not None:
            titles.append((norm_key(title), title, aid))
    print(f"{len(articles)} articles, {len(titles) - len(articles)} kept redirects")

    # pass 2: convert, compress, shard
    cctx = (zstandard.ZstdCompressor(compression_params=params) if args.window_log
            else zstandard.ZstdCompressor(level=args.level))
    locs = [None] * len(articles)
    shard_no, shard_f = 0, open(out / "shard_0000.dat", "wb")
    errors, log_f = 0, None
    for path, aid in tqdm(articles.items(), desc="pass 2: convert", unit="art"):
        entry = archive.get_entry_by_path(path)
        try:
            html = bytes(entry.get_item().content).decode("utf-8", errors="replace")
            text = html_to_guidetext(html, entry.title, path2id)
        except Exception as e:  # incl. RecursionError; one bad page must not kill the run
            errors += 1
            if log_f is None:
                log_f = open(out / "build.log", "w")
            log_f.write(f"{path}\t{type(e).__name__}: {e}\n")
            text = f"# {entry.title}\n(conversion failed)"
        data = text.encode("utf-8")
        if len(data) > ART_CAP:
            cut = data.rfind(b"\n", 0, ART_CAP)
            if cut <= 0:  # single huge line: cut mid-line but not mid-codepoint
                data = data[:ART_CAP].decode("utf-8", errors="ignore").encode("utf-8")
            else:
                data = data[:cut]
            data += b"\n\n(article truncated)"
        blob = cctx.compress(data)
        if shard_f.tell() + len(blob) > args.shard_cap:
            shard_f.close()
            shard_no += 1
            shard_f = open(out / f"shard_{shard_no:04}.dat", "wb")
        locs[aid] = (shard_no, shard_f.tell(), len(blob))
        shard_f.write(blob)
    shard_f.close()
    if log_f:
        log_f.close()
        print(f"{errors} articles failed conversion (stubs emitted), see {out / 'build.log'}")

    with open(out / "articles.bin", "wb") as f:
        for loc in locs:
            f.write(ART_REC.pack(*loc))

    titles.sort()
    with open(out / "titles.bin", "wb") as tf, \
         open(out / "titles.sparse", "wb") as sf:
        for i, (key, display, aid) in enumerate(titles):
            if i % STRIDE == 0:
                sf.write(struct.pack("<I", tf.tell()))
            tf.write(key.encode() + b"\0" + display.encode() + b"\0"
                     + struct.pack("<I", aid))

    meta = {
        "format": 1, "articles": len(articles), "titles": len(titles),
        "shards": shard_no + 1, "stride": STRIDE,
        "source": Path(args.zim).name,
    }
    if args.window_log:
        # Device refuses packs whose window it can't afford (see Pack.begin
        # in the firmware) -- recording it here is what lets it check.
        meta["window_log"] = args.window_log
    (out / "pack.json").write_text(json.dumps(meta, indent=2))
    print(f"pack written to {out}")

# ---------------------------------------------------------------- pack reading

class Pack:
    def __init__(self, pack_dir):
        self.dir = Path(pack_dir)
        self.meta = json.loads((self.dir / "pack.json").read_text())
        self.titles_f = open(self.dir / "titles.bin", "rb")
        sparse = (self.dir / "titles.sparse").read_bytes()
        self.sparse = struct.unpack(f"<{len(sparse) // 4}I", sparse)
        self.articles_f = open(self.dir / "articles.bin", "rb")
        import zstandard
        self.dctx = zstandard.ZstdDecompressor()

    def _record_at(self, off):
        """Read one titles.bin record at byte offset. Returns (key, display, id, next_off)."""
        f = self.titles_f
        f.seek(off)
        buf = b""
        while buf.count(b"\0") < 2 or len(buf) < buf.index(b"\0", buf.index(b"\0") + 1) + 5:
            chunk = f.read(512)
            if not chunk:
                break
            buf += chunk
        i = buf.index(b"\0")
        j = buf.index(b"\0", i + 1)
        aid = struct.unpack_from("<I", buf, j + 1)[0]
        return buf[:i].decode(), buf[i + 1:j].decode(), aid, off + j + 5

    def search(self, prefix, limit=50):
        """Prefix search via sparse index + binary search — the device algorithm."""
        key = norm_key(prefix) if prefix else ""
        lo, hi = 0, len(self.sparse) - 1
        while lo < hi:  # last sparse entry whose key <= prefix
            mid = (lo + hi + 1) // 2
            if self._record_at(self.sparse[mid])[0] <= key:
                lo = mid
            else:
                hi = mid - 1
        off, size = self.sparse[lo], self.titles_f.seek(0, 2)
        results = []
        while off < size and len(results) < limit:
            k, display, aid, off = self._record_at(off)
            if k.startswith(key):
                results.append((k, display, aid))
            elif k > key:
                break
        return results

    def article(self, aid):
        self.articles_f.seek(aid * ART_REC.size)
        shard, off, length = ART_REC.unpack(self.articles_f.read(ART_REC.size))
        with open(self.dir / f"shard_{shard:04}.dat", "rb") as f:
            f.seek(off)
            return self.dctx.decompress(f.read(length)).decode("utf-8")

    def article_stream(self, aid, chunk_size=4096):
        """Same article, decompressed through the chunked streaming API (what
        the C3 firmware does, one small buffer at a time, never holding the
        whole frame or the whole decompressed text) -- a desktop self-check
        that the format survives that path before device code has to run it.

        Bounds the input to exactly this article's `length` compressed bytes:
        shards are frames concatenated with no separator, so an unbounded
        stream_reader happily keeps decoding into the *next* article's frame
        once this one ends -- caught by this check's first real run."""
        import io
        self.articles_f.seek(aid * ART_REC.size)
        shard, off, length = ART_REC.unpack(self.articles_f.read(ART_REC.size))
        with open(self.dir / f"shard_{shard:04}.dat", "rb") as f:
            f.seek(off)
            blob = f.read(length)
        reader = self.dctx.stream_reader(io.BytesIO(blob))
        out = bytearray()
        while True:
            chunk = reader.read(chunk_size)
            if not chunk:
                break
            out += chunk
        return out.decode("utf-8")

    def all_titles(self):
        # loads every title into RAM — selftest/debug only; TUI and read use search()
        off, size = 0, self.titles_f.seek(0, 2)
        out = []
        while off < size:
            k, display, aid, off = self._record_at(off)
            out.append((k, display, aid))
        return out


def read_cmd(args):
    pack = Pack(args.pack)
    matches = pack.search(args.title)
    exact = [m for m in matches if m[0] == norm_key(args.title)]
    if not (exact or matches):
        sys.exit(f"no match for {args.title!r}")
    key, display, aid = (exact or matches)[0]
    if not exact:
        print(f"(no exact match, showing {display!r})\n", file=sys.stderr)
    if args.stream:
        one_shot = pack.article(aid)
        streamed = pack.article_stream(aid)
        if streamed != one_shot:
            sys.exit("--stream mismatch: chunked decompress != one-shot decompress "
                     "(fix the format/compression before the C3 firmware tries this)")
        print(f"(--stream: {len(streamed)} bytes, matches one-shot decompress)\n",
              file=sys.stderr)
    try:
        print(LINK_RE.sub(r"\1", pack.article(aid)))
    except BrokenPipeError:
        pass

# ---------------------------------------------------------------- TUI

# Hardware key sim, matching hardware/xteink-x3/PLAN.md #1's finalized
# 6-button scheme: 1/2/3/4 = the four front bottom buttons in physical
# left-to-right order (BACK, CONFIRM, LEFT, RIGHT); [ and ] = the two side
# buttons (UP = left side, DOWN = right side). Character-cycling entry uses
# the same set as the firmware's kCycle (main.cpp), space through '-. included.
CYCLE = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'-."


def render_lines(text, width):
    """GuideText -> [(str, attr_name)], plus ordered [(link_text, id)]."""
    import textwrap
    lines, links = [], []
    for raw in text.split("\n"):
        for m in LINK_RE.finditer(raw):
            links.append((m.group(1), int(m.group(2))))
        raw = LINK_RE.sub(r"\1", raw)
        if raw.startswith("#"):
            level = len(raw) - len(raw.lstrip("#"))
            body = raw.lstrip("# ")
            for w in textwrap.wrap(body, width) or [""]:
                lines.append((w, "h1" if level == 1 else "h2"))
        elif raw[:2] == "* " or re.match(r"\d+\. ", raw):
            head = raw.index(" ") + 1
            wrapped = textwrap.wrap(raw, width, subsequent_indent=" " * head)
            lines.extend((w, "text") for w in wrapped)
        else:
            lines.extend((w, "text") for w in textwrap.wrap(raw, width))
        lines.append(("", "text"))
    while lines and not lines[-1][0]:
        lines.pop()
    return lines, links


def tui(args):
    import os
    os.environ.setdefault("ESCDELAY", "25")
    import curses
    pack = Pack(args.pack)

    def main(stdscr):
        curses.curs_set(0)
        mode = "search"           # "search" (one list, box + results) or "reading"
        typing = False
        query, slot = "", None   # slot: char being cycled, or None
        selection = 0            # 0 = the search box (list position 0), N = matches[N-1]
        matches, match_query, match_limit = [], None, 0
        rendered_w = 0

        aid, title, lines = None, "", []
        page_start, history = 0, []  # history: prior page_start values, for the UP/back key

        def refilter(view):
            nonlocal matches, match_query, match_limit, selection
            match_query, match_limit = query, max(4 * view, 128)
            matches = pack.search(query, limit=match_limit)
            selection = min(selection, len(matches))

        def page_end_of(start, view):
            return min(start + view, len(lines))

        def load(new_aid, w):
            nonlocal aid, title, lines, page_start, history, rendered_w
            text = pack.article(new_aid)
            nl = text.find("\n")
            first = text[:nl] if nl != -1 else text
            aid, title = new_aid, first[2:] if first.startswith("# ") else first
            rendered_w = max(10, w - 2)
            lines, _ = render_lines(text, rendered_w)  # link markup stripped, same as the
            page_start, history = 0, []                # firmware's stripLinks() -- it doesn't
                                                         # follow links either (open question,
                                                         # see hardware/xteink-x3/PLAN.md #1)

        while True:
            h, w = stdscr.getmaxyx()
            view = max(1, h - 3)  # rows left for content: top bar, side-hint row, bottom bar

            if mode == "reading" and aid is not None and max(10, w - 2) != rendered_w:
                keep = page_start
                load(aid, w)
                page_start = min(keep, max(0, len(lines) - 1))

            if mode == "search":
                if match_query != query or match_limit < view + 1:
                    refilter(view)
                while len(matches) == match_limit and selection >= match_limit:
                    match_limit *= 2
                    matches = pack.search(query, limit=match_limit)
                selection = min(selection, len(matches))

            stdscr.erase()

            top_text = f"The Guide - {title}" if mode == "reading" else "The Guide"
            stdscr.addnstr(0, 0, top_text[:w - 1].ljust(w - 1), w - 1, curses.A_REVERSE)

            if mode == "search":
                total = len(matches) + 1  # the box, then each result
                page = selection // view
                list_top = page * view
                for row in range(view):
                    idx = list_top + row
                    if idx >= total:
                        break
                    y = 1 + row
                    is_sel = idx == selection
                    label = f"Search: {query}" if idx == 0 else matches[idx - 1][1]
                    attr = curses.A_REVERSE if is_sel else curses.A_NORMAL
                    stdscr.addnstr(y, 0, label[:w - 1].ljust(w - 1) if is_sel else label[:w - 1],
                                   w - 1, attr)
                    if idx == 0 and typing:  # cursor cell for the letter being cycled
                        x = min(len(label), w - 2)
                        stdscr.addnstr(y, x, slot or "_", 1, curses.A_BOLD)
            else:  # reading: paginated, no line-level scroll (matches the firmware)
                page_end = page_end_of(page_start, view)
                for row, (text_line, kind) in enumerate(lines[page_start:page_end]):
                    attr = curses.A_BOLD if kind != "text" else curses.A_NORMAL
                    stdscr.addnstr(1 + row, 0, text_line[:w - 1], w - 1, attr)

            if mode == "reading":
                side, bottom = ("Back", "Fwd"), ("Search", "-", "-", "-")
            elif typing:
                side, bottom = ("Up", "Down"), ("Del", "OK", "<", ">")
            else:
                side = ("Up", "Down")
                bottom = ("Search", "Type" if selection == 0 else "Open", "PgUp", "PgDn")

            side_row = h - 2
            left_chip, right_chip = f"[ {side[0]}", f"{side[1]} ]"
            stdscr.addnstr(side_row, 0, left_chip, min(len(left_chip), w - 1), curses.A_REVERSE)
            if len(right_chip) < w:
                stdscr.addnstr(side_row, w - 1 - len(right_chip), right_chip, len(right_chip),
                               curses.A_REVERSE)

            bottom_row = h - 1
            box_w = max(1, (w - 3) // 4)
            x = 0
            for i, label in enumerate(bottom):
                # last chip stops one column short of the bottom-right cell --
                # writing into it raises a curses error (ncurses tries to
                # advance the cursor past the window on the last line)
                bw = max(1, w - 1 - x) if i == 3 else box_w
                chip = f"{i + 1}:{label}".center(bw)[:bw]
                stdscr.addnstr(bottom_row, x, chip, bw, curses.A_REVERSE)
                x += bw + 1

            stdscr.refresh()

            k = stdscr.get_wch()
            if k == curses.KEY_RESIZE:
                continue

            if mode == "reading":
                if k == "[":  # UP: page back
                    if history:
                        page_start = history.pop()
                elif k == "]":  # DOWN: page forward
                    pe = page_end_of(page_start, view)
                    if pe < len(lines):
                        history.append(page_start)
                        page_start = pe
                elif k == "1":  # BACK: return to Search, prior query/scroll untouched
                    mode = "search"
                elif k in ("\x1b", "\x03"):
                    return
                # 2/3/4 (CONFIRM/LEFT/RIGHT) in Reader mode: still open questions
                # on the real hardware too (hardware/xteink-x3/PLAN.md #1) -- no-op here.
            elif typing:
                if k == "[":  # UP: leave typing, move selection up (keeps the query)
                    typing, slot = False, None
                elif k == "]":  # DOWN: leave typing, move selection into the results
                    typing, slot = False, None
                    selection = 1 if matches else 0
                elif k == "3":  # LEFT: cycle backward
                    slot = CYCLE[-1] if slot is None else CYCLE[(CYCLE.index(slot) - 1) % len(CYCLE)]
                elif k == "4":  # RIGHT: cycle forward
                    slot = CYCLE[0] if slot is None else CYCLE[(CYCLE.index(slot) + 1) % len(CYCLE)]
                elif k == "2":  # CONFIRM: commit the slot, open a new one, re-filter
                    if slot is not None:
                        query += slot
                        slot = None
                elif k == "1":  # BACK: backspace (clears an uncommitted slot first)
                    if slot is not None:
                        slot = None
                    elif query:
                        query = query[:-1]
                elif k in ("\x1b", "\x03"):
                    return
            else:  # search mode, list focus
                if k == "[":  # UP
                    selection = max(0, selection - 1)
                elif k == "]":  # DOWN
                    selection = min(len(matches), selection + 1)
                elif k == "3":  # LEFT: page up -- previous page, landing on its first row
                    page = selection // view
                    if page > 0:
                        selection = (page - 1) * view
                elif k == "4":  # RIGHT: page down -- next page, landing on its first row
                    last_page = len(matches) // view
                    page = selection // view
                    if page < last_page:
                        selection = (page + 1) * view
                elif k == "2":  # CONFIRM: on the box, enter typing; on a result, open it
                    if selection == 0:
                        typing = True
                    elif matches:
                        load(matches[selection - 1][2], w)
                        mode = "reading"
                elif k == "1":  # BACK: jump the selection back to the search box
                    selection = 0
                elif k in ("\x1b", "\x03"):
                    return

    curses.wrapper(main)

# ---------------------------------------------------------------- selftest

def selftest(args):
    assert norm_key("Douglas_Adams") == "douglas adams"
    assert norm_key("Café-au-lait!") == "cafe au lait"
    assert norm_key("  A   B ") == "a b"

    html = """<html><body><section>
      <h2>Life</h2><table><tr><td>infobox junk</td></tr></table>
      <p>Born in <a href="./Cambridge">Cambridge</a>, <b>he</b> wrote books.</p>
      <ul><li>Item one</li><li>Item two</li></ul>
      <ol><li>First</li><li>Second</li></ol>
      <h2>Empty section</h2>
    </section></body></html>"""
    text = html_to_guidetext(html, "Douglas Adams", {"Cambridge": 42})
    assert text.splitlines() == [
        "# Douglas Adams", "## Life",
        "Born in [Cambridge|42], he wrote books.",
        "* Item one", "* Item two", "1. First", "2. Second",
    ], text

    stub = """<html><head><title>Addis Abeba lion</title>
      <meta http-equiv="refresh" content="0;URL='./Lion#Subspecies'" /></head>
      <body><a href="./Lion#Subspecies">Addis Abeba lion</a></body></html>"""
    assert meta_refresh_target(stub) == "Lion"
    assert meta_refresh_target(html) is None

    lines, links = render_lines(text, 20)
    assert links == [("Cambridge", 42)]
    assert lines[0] == ("Douglas Adams", "h1")
    assert all("[" not in l for l, _ in lines)

    # titles.bin roundtrip through Pack.search
    import tempfile, zstandard
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        rows = sorted((norm_key(t), t, i) for i, t in
                      enumerate(["Apple", "Apricot", "Banana", "apple pie"]))
        with open(d / "titles.bin", "wb") as tf, open(d / "titles.sparse", "wb") as sf:
            for i, (k, disp, aid) in enumerate(rows):
                if i % STRIDE == 0:
                    sf.write(struct.pack("<I", tf.tell()))
                tf.write(k.encode() + b"\0" + disp.encode() + b"\0" + struct.pack("<I", aid))
        blob = zstandard.ZstdCompressor().compress(b"# Apple\nhello")
        blob2 = zstandard.ZstdCompressor().compress(b"# Banana\nsecond shard")
        (d / "shard_0000.dat").write_bytes(blob)
        (d / "shard_0001.dat").write_bytes(blob2)
        with open(d / "articles.bin", "wb") as f:
            f.write(ART_REC.pack(0, 0, len(blob)) * 2)
            f.write(ART_REC.pack(1, 0, len(blob2)))  # Banana lives in shard 1
            f.write(ART_REC.pack(0, 0, len(blob)))
        (d / "pack.json").write_text(json.dumps({"format": 1, "stride": STRIDE}))
        p = Pack(d)
        assert [r[1] for r in p.search("app")] == ["Apple", "apple pie"]
        assert p.search("banana")[0][2] == 2
        assert p.article(0) == "# Apple\nhello"
        assert p.article(2) == "# Banana\nsecond shard"
        assert len(p.all_titles()) == 4
    print("selftest ok")

# ---------------------------------------------------------------- cli

def main():
    ap = argparse.ArgumentParser(prog="guidec")
    sub = ap.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build", help="convert ZIM to pack")
    b.add_argument("--zim", required=True)
    b.add_argument("--out", required=True)
    b.add_argument("--limit", type=int, default=0, help="max articles (0 = all)")
    b.add_argument("--level", type=int, default=9, help="zstd level")
    b.add_argument("--shard-cap", type=int, default=SHARD_CAP,
                   help="max shard bytes (small values exercise rollover)")
    b.add_argument("--window-log", type=int, default=0,
                   help="cap the zstd window (e.g. 16 = 64 KB, for the "
                        "PSRAM-less Xteink X3); 0 = uncapped (default)")
    b.set_defaults(fn=build)

    r = sub.add_parser("read", help="print one article")
    r.add_argument("--pack", required=True)
    r.add_argument("--stream", action="store_true",
                   help="decompress via the chunked streaming API (what the "
                        "C3 firmware does) and verify it matches the normal "
                        "one-shot decompress, before device code has to")
    r.add_argument("title")
    r.set_defaults(fn=read_cmd)

    t = sub.add_parser("tui", help="browse a pack")
    t.add_argument("--pack", required=True)
    t.set_defaults(fn=tui)

    s = sub.add_parser("selftest")
    s.set_defaults(fn=selftest)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
