#include "guidetext.h"

#include <FreeInkUIDisplayTarget.h>

#include <cctype>
#include <cstring>

using freeink::ui::DisplayTarget;
using freeink::ui::Rect;
using freeink::ui::TextStyle;

namespace guidetext {
namespace {

// Headings (any of #/##/###) get a bit of extra spacing so they still read
// as headings despite there being only one bundled font size right now (no
// bold, no larger heading font -- FreeInkUI's DisplayTarget doesn't
// synthesize style.bold, and generating extra font sizes via freeink-sdk's
// gen_font.py is future polish, not blocking a working reader). Bullets and
// numbered items keep their "* " / "1. " marker as plain visible text
// instead of a hanging indent, for the same reason: simpler, still readable.
enum class Kind { Heading, Body };

struct Classified {
  Kind kind;
  const char* body;  // points into `raw`, past any stripped "#"/"##"/"###" marker
  size_t bodyLen;
};

Classified classify(char* raw, size_t rawLen) {
  if (rawLen >= 4 && raw[0] == '#' && raw[1] == '#' && raw[2] == '#' && raw[3] == ' ') {
    return {Kind::Heading, raw + 4, rawLen - 4};
  }
  if (rawLen >= 3 && raw[0] == '#' && raw[1] == '#' && raw[2] == ' ') {
    return {Kind::Heading, raw + 3, rawLen - 3};
  }
  if (rawLen >= 2 && raw[0] == '#' && raw[1] == ' ') {
    return {Kind::Heading, raw + 2, rawLen - 2};
  }
  return {Kind::Body, raw, rawLen};
}

// Copies src[0..n) to dst, replacing [text|id] links with just their text --
// the device doesn't follow links yet (Phase X3 open question, PLAN.md #1).
// Same algorithm as the RLCD build's stripLinks() and guidec.py's LINK_RE.
size_t stripLinks(const char* src, size_t n, char* dst, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < n && o + 1 < cap;) {
    if (src[i] == '[') {
      size_t j = i + 1;
      while (j < n && src[j] != '|' && src[j] != ']' && src[j] != '[' && j - i < 400) j++;
      if (j < n && src[j] == '|') {
        size_t k = j + 1;
        while (k < n && isdigit(static_cast<unsigned char>(src[k]))) k++;
        if (k > j + 1 && k < n && src[k] == ']') {  // exact [text|digits] shape
          for (size_t t = i + 1; t < j && o + 1 < cap; t++) dst[o++] = src[t];
          i = k + 1;
          continue;
        }
      }
    }
    dst[o++] = src[i++];
  }
  dst[o] = 0;
  return o;
}

// Word-wraps and draws `text` into [x0,x1) starting at *y, stopping (without
// starting a line it can't finish) once *y would exceed maxY. Returns true
// if the whole block fit; false + *charsConsumed = how many characters of
// `text` were placed, if the page ran out of room first. Room is checked
// before starting each new visual line (never mid-flush), so a line is
// either drawn whole or not started -- the resume point is always exact.
bool layoutBlock(DisplayTarget& ui, int fontId, const char* text, int16_t x0, int16_t x1, int16_t maxY,
                  int16_t lineH, int16_t* y, size_t* charsConsumed) {
  TextStyle style;
  style.font = static_cast<freeink::ui::FontId>(fontId);
  style.maxLines = 1;

  char line[256];
  size_t lineLen = 0;
  int16_t curLineW = 0;  // tracked incrementally -- see below, not re-measured from `line`
  const int16_t spaceW = static_cast<int16_t>(ui.measureText(fontId, " ", style).width);
  const char* p = text;
  const auto haveRoom = [&]() { return static_cast<int16_t>(*y + lineH) <= maxY; };

  while (*p) {
    const char* wordStart = p;
    while (*p && *p != ' ') ++p;
    size_t wordLen = static_cast<size_t>(p - wordStart);
    const char* afterWord = p;
    if (*afterWord == ' ') ++afterWord;

    char word[96];
    size_t wl = wordLen < sizeof(word) - 1 ? wordLen : sizeof(word) - 1;
    memcpy(word, wordStart, wl);
    word[wl] = 0;
    const int16_t wordW = static_cast<int16_t>(ui.measureText(fontId, word, style).width);

    if (lineLen == 0) {
      if (!haveRoom()) {
        *charsConsumed = static_cast<size_t>(wordStart - text);
        return false;
      }
    } else {
      // NOTE: `line` isn't NUL-terminated at this point (only just before a
      // draw call, below) -- measuring it here directly read past the
      // written bytes into whatever garbage followed in the buffer, which is
      // what caused the wildly inconsistent wrap points. Track the width as
      // we go instead of re-measuring the accumulated (non-terminated)
      // buffer on every word.
      if (static_cast<int16_t>(curLineW + spaceW + wordW) > static_cast<int16_t>(x1 - x0)) {
        line[lineLen] = 0;
        ui.text(Rect{x0, *y, static_cast<int16_t>(x1 - x0), lineH}, line, style);
        *y = static_cast<int16_t>(*y + lineH);
        lineLen = 0;
        curLineW = 0;
        if (!haveRoom()) {
          *charsConsumed = static_cast<size_t>(wordStart - text);
          return false;
        }
      }
    }
    if (lineLen > 0) {
      if (lineLen + 1 < sizeof(line)) line[lineLen++] = ' ';
      curLineW = static_cast<int16_t>(curLineW + spaceW);
    }
    size_t copyLen = wl;
    if (lineLen + copyLen >= sizeof(line)) copyLen = sizeof(line) - 1 - lineLen;
    memcpy(line + lineLen, word, copyLen);
    lineLen += copyLen;
    curLineW = static_cast<int16_t>(curLineW + wordW);
    p = afterWord;
  }

  if (lineLen > 0) {
    line[lineLen] = 0;
    ui.text(Rect{x0, *y, static_cast<int16_t>(x1 - x0), lineH}, line, style);
    *y = static_cast<int16_t>(*y + lineH);
  }
  return true;
}

}  // namespace

bool atEnd(const PagePos& pos, FsFile& file) {
  return pos.skipChars == 0 && pos.blockOffset >= static_cast<uint32_t>(file.fileSize());
}

PagePos renderPage(FsFile& file, PagePos start, DisplayTarget& ui, int fontId, int16_t marginX, int16_t marginY) {
  const int16_t x0 = marginX;
  const int16_t x1 = static_cast<int16_t>(ui.logicalWidth() - marginX);
  const int16_t maxY = static_cast<int16_t>(ui.logicalHeight() - marginY);
  const int16_t lineH = ui.lineHeight(fontId);
  int16_t y = marginY;

  const uint64_t fileSize = file.fileSize();
  uint32_t blockOffset = start.blockOffset;
  uint16_t skipChars = start.skipChars;

  static char raw[3072];
  static char stripped[3072];

  while (blockOffset < fileSize) {
    if (!file.seekSet(blockOffset)) break;
    size_t rawLen = 0;
    int c;
    while (rawLen < sizeof(raw) - 1 && (c = file.read()) >= 0 && c != '\n') {
      raw[rawLen++] = static_cast<char>(c);
    }
    raw[rawLen] = 0;
    const uint32_t nextBlockOffset = blockOffset + static_cast<uint32_t>(rawLen) + 1;  // +1 for the '\n'

    if (rawLen == 0) {  // shouldn't normally occur (guidec.py emits one block per line, no blank
                         // separators) but stay defensive rather than assume
      blockOffset = nextBlockOffset;
      skipChars = 0;
      continue;
    }

    const Classified cl = classify(raw, rawLen);
    const size_t strippedLen = stripLinks(cl.body, cl.bodyLen, stripped, sizeof(stripped));
    const char* remaining = stripped + (skipChars < strippedLen ? skipChars : strippedLen);

    if (skipChars == 0 && cl.kind == Kind::Heading) y = static_cast<int16_t>(y + lineH / 2);

    size_t charsConsumed = 0;
    const bool fit = layoutBlock(ui, fontId, remaining, x0, x1, maxY, lineH, &y, &charsConsumed);
    if (!fit) {
      return PagePos{blockOffset, static_cast<uint16_t>(skipChars + charsConsumed)};
    }

    if (cl.kind == Kind::Heading) y = static_cast<int16_t>(y + lineH / 3);
    y = static_cast<int16_t>(y + lineH / 3);  // inter-paragraph gap
    blockOffset = nextBlockOffset;
    skipChars = 0;
    if (y >= maxY) return PagePos{blockOffset, 0};
  }

  return PagePos{static_cast<uint32_t>(fileSize), 0};  // end of article
}

}  // namespace guidetext
