#pragma once

// GuideText -> screen: parses the line-oriented markup guidec.py emits
// (# / ## / ### headings, plain paragraphs, * bullets, N. numbered lists,
// [text|id] links) straight off the SD cache file Pack::loadArticleToCache()
// writes, word-wrapping and drawing one page at a time via FreeInkUI's
// DisplayTarget. See ../../hardware/xteink-x3/PLAN.md #6 Phase X2.
//
// Deliberately doesn't hold the article in RAM (the whole point of the
// cache-file design in PLAN.md #2b) -- reads the file a block (source line)
// at a time instead.

#include <SdFat.h>

#include <cstdint>

namespace freeink {
namespace ui {
class DisplayTarget;
}
}  // namespace freeink

namespace guidetext {

// A position within the cached article precise enough to resume mid-
// paragraph: the source line's file byte offset, plus how many characters
// of that line's *stripped* (link-flattened) text to skip before
// continuing. A plain byte offset isn't enough on its own -- link stripping
// changes a line's length, so "resume at file byte N" doesn't survive
// re-deriving the stripped text on a later visit; "resume after M stripped
// characters of the line starting at file byte N" does.
struct PagePos {
  uint32_t blockOffset = 0;
  uint16_t skipChars = 0;
  bool operator==(const PagePos& o) const { return blockOffset == o.blockOffset && skipChars == o.skipChars; }
};

// Renders as much GuideText as fits on one page, starting at `start`, into
// the rectangle [marginX, marginY] .. [ui.logicalWidth()-marginX,
// ui.logicalHeight()-marginY]. `file` must already be open (read-only) on
// the decompressed article cache (Pack::cachePath()). Returns the position
// the *next* page should start at -- compare with atEnd() to know if this
// was the last page.
PagePos renderPage(FsFile& file, PagePos start, freeink::ui::DisplayTarget& ui, int fontId, int16_t marginX,
                    int16_t marginY);

bool atEnd(const PagePos& pos, FsFile& file);

}  // namespace guidetext
