// The Guide — Xteink X3 firmware.
//
// Phase X0 (done): hello panel + button bring-up.
// Phase X1 (done): pack plumbing -- SD card, on-disk search, streaming zstd
// decompress to an SD cache file.
// Phase X2 (done): reader -- paginates the cached article (guidetext.cpp)
// and pages through it with BTN_UP/BTN_DOWN.
// Phase X3 (this addition): index/search + reader wiring -- the finalized
// button scheme from ../../hardware/xteink-x3/PLAN.md #1: one scrollable
// list with the query box pinned at position 0, letter-cycling text entry,
// wired to open into the reader. See PLAN.md #6 Phase X3.
//
// Buttons (search/list mode, PLAN.md #1):
//   BTN_UP/DOWN    move the list selection (search box is item 0)
//   BTN_CONFIRM    on the box: enter typing. on a result: open it (Reader)
// Buttons (typing mode):
//   BTN_LEFT/RIGHT cycle the current letter slot backward/forward
//   BTN_CONFIRM    commit the slot, open a new one, re-filter results
//   BTN_BACK       backspace (clears an uncommitted slot first)
//   BTN_UP/DOWN    leave typing, move the selection (keeps the query)
// Buttons (Reader mode):
//   BTN_UP/DOWN    page back/forward (history stack, PLAN.md #6 Phase X2)
//   BTN_BACK       back to Search, restoring the prior query/selection
//
// Serial commands (still handy for testing without the buttons):
//   search <prefix>   on-disk prefix search, prints up to 20 matches
//   load <id>         decompress article `id` to the SD cache file and dump
//                     it back over Serial (no screen change) -- Phase X1 check
//   read <id>         decompress article `id` and switch the screen into
//                     Reading mode, showing its first page
//   heap              print current + minimum-ever free heap

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <FreeInkUIDisplayTarget.h>
#include <InputManager.h>
#include <Preferences.h>
#include <PowerManager.h>
#include <Rtc.h>
#include <SDCardManager.h>
#include <esp_sleep.h>

#include "cover_bitmap.h"
#include "guidetext.h"
#include "pack.h"

using freeink::ui::BitmapFormat;
using freeink::ui::BitmapMode;
using freeink::ui::BitmapRef;
using freeink::ui::Color;
using freeink::ui::DisplayTarget;
using freeink::ui::Orientation;
using freeink::ui::Paint;
using freeink::ui::Rect;
using freeink::ui::TextStyle;

namespace {

constexpr const char* kPackDir = "/pack";
constexpr int16_t kMarginX = 16;
constexpr int kFontId = 0;  // single bundled Noto Sans -- see guidetext.cpp
constexpr int kMaxResults = 30;  // plenty for one screen's worth of rows

// List/cycle navigation (BTN_LEFT/RIGHT while typing to cycle a letter;
// BTN_UP/DOWN/LEFT/RIGHT while browsing to move the selection or page) coalesces
// its screen refresh: each press just updates state and (re)starts this timer
// instead of redrawing immediately, so mashing through the alphabet or a long
// result list doesn't pay for an e-ink refresh (380 ms-1.3 s) per keystroke.
// loop() draws once this much time has passed since the *last* such press,
// i.e. once the burst settles.
constexpr unsigned long kNavDebounceMs = 500;

// Full refresh for the results list is now content-triggered, not periodic
// (PLAN.md #6 Phase X4: no ghosting observed in practice, so the old "every N
// partials" cadence was just unnecessary lag) -- see drawSearchScreen()'s
// pageChanged logic and showPage(), which now always does a full refresh
// since every reader page turn is definitionally "a new page" of content.
constexpr unsigned long kPageToastMs = 1000;  // how long the "Page N/M" toast stays up

// Phase X4: deep sleep. PowerManager::deepSleep() is a real ESP32-C3 SoC deep
// sleep (RTC domain only, ~5 uA) -- not a hardware battery cutoff -- so it's
// firmware-triggered both ways: we choose when to sleep, and GPIO3 (the power
// button) is the only armed wake source, which resets the chip (deep sleep
// doesn't resume mid-loop; setup() runs again and restores state from NVS,
// see loadSleepState()/resumeFromSleep()). The e-ink panel itself needs no
// power to hold whatever's on it, which is what makes a sleep/cover screen a
// pure UX choice rather than a power requirement (PLAN.md #6 Phase X4 note).
constexpr unsigned long kIdleSleepMs = 180000;     // 3 min with no button activity -> sleep
constexpr unsigned long kManualSleepHoldMs = 800;  // long-press POWER -> sleep now
constexpr const char* kNvsNamespace = "guide";

// Title bar (top) and button-hint bar (bottom) chrome. Both are the same
// height (one line of text + padding), so the content area between them is
// symmetric -- see contentMarginY(). Rounded corners face *into* the
// screen (a full-width top bar only makes sense rounded on the edge away
// from the screen boundary; the bottom bar is 4 separate box "chips").
constexpr int16_t kBarPad = 6;
constexpr int16_t kBarRadius = 8;
constexpr int16_t kBoxGap = 4;
constexpr int16_t kContentGap = 8;  // gap between a bar and the content area
constexpr int16_t kStatusGap = 12;  // gap between a top-bar title and its right-aligned status text

// Letter-cycle character set for typing (PLAN.md #1): space closes the root
// PLAN's "space-in-cycle" gap, the trailing punctuation is cosmetic --
// norm_key() in guidec.py folds all of it to a space anyway, so these keys
// only matter for what the query *looks like* on screen, not for matching.
constexpr char kCycle[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'-.";
constexpr int kCycleLen = sizeof(kCycle) - 1;

EInkDisplay display(BoardConfig::XTEINK_X3.display.sclk, BoardConfig::XTEINK_X3.display.mosi,
                     BoardConfig::XTEINK_X3.display.cs, BoardConfig::XTEINK_X3.display.dc,
                     BoardConfig::XTEINK_X3.display.rst, BoardConfig::XTEINK_X3.display.busy);
InputManager input;
DisplayTarget* ui = nullptr;
Pack pack;
bool packOk = false;
BatteryMonitor battery;  // BQ27220 gauge, lazily brings up its own I2C bus
Rtc rtc;                 // DS3231; begin() below, unset until a `settime` command

enum class Mode { Search, Reading };
Mode mode = Mode::Search;

// Search/list state. Deliberately never reset when a Reader-mode article
// opens, so BTN_BACK from Reading restores it exactly (PLAN.md #6 Phase X3
// exit criterion: "BACK returns to the search page with the query and
// scroll position intact").
char query[48] = "";
char slot = 0;  // 0 = no uncommitted letter yet (not a valid kCycle char, so unambiguous)
bool typing = false;
int selection = 0;  // 0 = search box; 1..resultsCount = a result row
Pack::Result results[kMaxResults];
int resultsCount = 0;
int lastResultsPage = 0;  // page shown by the last actual draw; see drawSearchScreen()'s pageChanged
bool navPending = false;  // a nav redraw (cycle or list scroll/page) is due once the debounce elapses
unsigned long lastNavMs = 0;
unsigned long lastActivityMs = 0;  // any button activity resets this; loop() sleeps after kIdleSleepMs

// Reading-mode state: the cache file, where the current page started, a
// history stack of prior page starts (for BTN_UP page-back).
FsFile readerFile;
guidetext::PagePos pageStart;
guidetext::PagePos pageEnd;
constexpr int kMaxHistory = 64;
guidetext::PagePos history[kMaxHistory];
int historyCount = 0;
uint32_t currentAid = 0;
char currentTitle[96] = "";  // for the top bar; read from the cache file's first line, see enterReading()

void refilter() {
  resultsCount = packOk ? pack.search(query, results, kMaxResults) : 0;
  if (selection > resultsCount) selection = resultsCount;
}

void cycleForward() {
  if (slot == 0) {
    slot = kCycle[0];
    return;
  }
  for (int i = 0; i < kCycleLen; i++) {
    if (kCycle[i] == slot) {
      slot = kCycle[(i + 1) % kCycleLen];
      return;
    }
  }
  slot = kCycle[0];
}

void cycleBackward() {
  if (slot == 0) {
    slot = kCycle[kCycleLen - 1];
    return;
  }
  for (int i = 0; i < kCycleLen; i++) {
    if (kCycle[i] == slot) {
      slot = kCycle[(i - 1 + kCycleLen) % kCycleLen];
      return;
    }
  }
  slot = kCycle[kCycleLen - 1];
}

void commitSlot() {
  if (slot != 0) {
    const size_t len = strlen(query);
    if (len + 1 < sizeof(query)) {
      query[len] = slot;
      query[len + 1] = 0;
    }
    slot = 0;
  }
  refilter();  // PLAN.md #5: re-filter on commit, not on every cycle step
}

void backspace() {
  if (slot != 0) {
    slot = 0;  // first press: drop the uncommitted letter, query unchanged
    return;
  }
  const size_t len = strlen(query);
  if (len > 0) {
    query[len - 1] = 0;
    refilter();
  }
}

// Height shared by both chrome bars (one line of text + padding), and the
// symmetric vertical inset the content area (list rows, article text) needs
// to leave clear of them.
int16_t barHeight() { return static_cast<int16_t>(ui->lineHeight(kFontId) + 2 * kBarPad); }
int16_t contentMarginY() { return static_cast<int16_t>(barHeight() + kContentGap); }

// Rows of the (box + results) list that fit in the content area between the
// two chrome bars -- the results list's page size, used both to lay it out
// and to work out PgUp/PgDn jumps and page-boundary crossings.
int resultsRowsPerPage() {
  const int16_t rowH = ui->lineHeight(kFontId);
  const int16_t viewH = static_cast<int16_t>(ui->logicalHeight() - 2 * contentMarginY());
  return (viewH / rowH) > 0 ? (viewH / rowH) : 1;
}

// Width a status string needs, or 0 for an empty one -- shared by drawTopBar()
// (to lay out the right-aligned segment) and callers that need to reserve the
// same space when truncating their own title text (buildReaderTitleText()).
int16_t statusWidth(const char* status) {
  if (!status[0]) return 0;
  TextStyle measure;
  measure.font = kFontId;
  return ui->measureText(kFontId, status, measure).width;
}

// Builds the top bar's right-aligned status segment: clock and/or battery %,
// whichever is actually available on this board/session. Both are optional
// hardware here -- the DS3231 reports garbage (oscillator-stopped) until a
// `settime` Serial command seeds it at least once; the BQ27220 gauge just
// needs to ACK over I2C. Empty when neither is available, so the bar falls
// back to using its full width for the title, same as before this existed.
void buildStatusText(char* out, size_t outCap) {
  Rtc::DateTime dt;
  const bool haveTime = rtc.present() && rtc.now(dt);
  uint16_t pct = 0;
  const bool haveBattery = battery.readPercentageChecked(pct);
  if (haveTime && haveBattery) {
    snprintf(out, outCap, "%02u:%02u  %u%%", dt.hour, dt.minute, pct);
  } else if (haveTime) {
    snprintf(out, outCap, "%02u:%02u", dt.hour, dt.minute);
  } else if (haveBattery) {
    snprintf(out, outCap, "%u%%", pct);
  } else {
    out[0] = 0;
  }
}

// Full-width black bar across the top, white text, rounded only on the
// bottom corners (the top corners sit on the screen's own edge, where a
// radius wouldn't read as rounded -- it'd just look like a gap). `status`
// (clock/battery, see buildStatusText()) draws right-aligned in the same
// bar when non-empty, and `text` is laid out in whatever width is left.
void drawTopBar(const char* text, const char* status = "") {
  const int16_t h = barHeight();
  ui->fill(Rect{0, 0, ui->logicalWidth(), h}, Paint::solid(Color::Black), kBarRadius, freeink::ui::CornersBottom);

  const int16_t statW = statusWidth(status);
  if (statW > 0) {
    TextStyle statStyle;
    statStyle.font = kFontId;
    statStyle.color = Color::White;
    statStyle.maxLines = 1;
    statStyle.align = freeink::ui::TextAlign::Right;
    ui->text(Rect{static_cast<int16_t>(ui->logicalWidth() - kMarginX - statW), kBarPad, statW,
                  static_cast<int16_t>(h - 2 * kBarPad)},
             status, statStyle);
  }

  const int16_t reserve = statW > 0 ? static_cast<int16_t>(statW + kStatusGap) : 0;
  TextStyle style;
  style.font = kFontId;
  style.color = Color::White;
  style.maxLines = 1;
  ui->text(Rect{kMarginX, kBarPad, static_cast<int16_t>(ui->logicalWidth() - 2 * kMarginX - reserve),
                static_cast<int16_t>(h - 2 * kBarPad)},
           text, style);
}

// Four black "button hint" boxes along the bottom, left to right matching
// the physical front-button order (BACK, CONFIRM, LEFT, RIGHT -- PLAN.md
// #1), each showing what that button currently does. Rounded on top only
// (the bottom edge sits on the screen boundary, right above the physical
// buttons); small gaps between boxes make each one read as its own chip.
void drawBottomBar(const char* backLabel, const char* confirmLabel, const char* leftLabel, const char* rightLabel) {
  const int16_t h = barHeight();
  const int16_t y = static_cast<int16_t>(ui->logicalHeight() - h);
  const int16_t totalW = ui->logicalWidth();
  const int16_t boxW = static_cast<int16_t>((totalW - 3 * kBoxGap) / 4);
  const char* labels[4] = {backLabel, confirmLabel, leftLabel, rightLabel};
  int16_t x = 0;
  for (int i = 0; i < 4; i++) {
    const int16_t w = (i == 3) ? static_cast<int16_t>(totalW - x) : boxW;  // last box absorbs rounding remainder
    ui->fill(Rect{x, y, w, h}, Paint::solid(Color::Black), kBarRadius, freeink::ui::CornersTop);
    TextStyle style;
    style.font = kFontId;
    style.color = Color::White;
    style.maxLines = 1;
    style.align = freeink::ui::TextAlign::Center;
    ui->text(Rect{x, static_cast<int16_t>(y + kBarPad), w, static_cast<int16_t>(h - 2 * kBarPad)}, labels[i], style);
    x = static_cast<int16_t>(x + w + kBoxGap);
  }
}

// Centered black/white popup box, drawn straight onto whatever's already in
// the framebuffer (no clearScreen()) so it reads as an overlay on the current
// screen. Shared by drawSleepPrompt() and showResultsPageToast(); callers
// pick the refresh mode and timing that fits how they're using it.
void drawCenteredToast(const char* msg) {
  TextStyle measure;
  measure.font = kFontId;
  const int16_t textW = ui->measureText(kFontId, msg, measure).width;
  constexpr int16_t kPadX = 20;
  constexpr int16_t kPadY = 14;
  const int16_t boxW = static_cast<int16_t>(textW + 2 * kPadX);
  const int16_t boxH = static_cast<int16_t>(ui->lineHeight(kFontId) + 2 * kPadY);
  const int16_t x = static_cast<int16_t>((ui->logicalWidth() - boxW) / 2);
  const int16_t y = static_cast<int16_t>((ui->logicalHeight() - boxH) / 2);

  ui->fill(Rect{x, y, boxW, boxH}, Paint::solid(Color::Black), kBarRadius);
  TextStyle style;
  style.font = kFontId;
  style.color = Color::White;
  style.maxLines = 1;
  style.align = freeink::ui::TextAlign::Center;
  ui->text(Rect{x, static_cast<int16_t>(y + kPadY), boxW, static_cast<int16_t>(boxH - 2 * kPadY)}, msg, style);
}

// Shown the moment a POWER hold crosses kManualSleepHoldMs -- the only
// feedback the user has that they've held it long enough to let go (a bare
// button gives no other cue). A fast partial refresh flashes it up quickly;
// enterSleep()'s own full-refresh cover replaces it moments later.
void drawSleepPrompt() {
  drawCenteredToast("Sleeping...");
  display.displayBuffer(EInkDisplay::FAST_REFRESH);
}

// Builds "The Guide - <article title>" for the reader's top bar, truncating
// just the title portion (with a trailing "...") if the full line wouldn't
// fit -- never cuts mid-UTF8-sequence. `reservedWidth` is space the caller's
// drawTopBar() status segment (clock/battery) will also take out of the same
// bar -- must match what's passed there, or truncation could undershoot/miss.
void buildReaderTitleText(char* out, size_t outCap, int16_t reservedWidth) {
  TextStyle style;
  style.font = kFontId;
  const int16_t maxWidth = static_cast<int16_t>(ui->logicalWidth() - 2 * kMarginX - reservedWidth);
  const char* prefix = "The Guide - ";
  char candidate[192];
  snprintf(candidate, sizeof candidate, "%s%s", prefix, currentTitle);
  if (ui->measureText(kFontId, candidate, style).width <= maxWidth) {
    strncpy(out, candidate, outCap - 1);
    out[outCap - 1] = 0;
    return;
  }
  char titleBuf[128];
  strncpy(titleBuf, currentTitle, sizeof(titleBuf) - 1);
  titleBuf[sizeof(titleBuf) - 1] = 0;
  size_t tlen = strlen(titleBuf);
  while (tlen > 0) {
    tlen--;
    while (tlen > 0 && (titleBuf[tlen] & 0xC0) == 0x80) tlen--;  // don't cut mid-UTF8
    titleBuf[tlen] = 0;
    snprintf(candidate, sizeof candidate, "%s%s...", prefix, titleBuf);
    if (tlen == 0 || ui->measureText(kFontId, candidate, style).width <= maxWidth) {
      strncpy(out, candidate, outCap - 1);
      out[outCap - 1] = 0;
      return;
    }
  }
  strncpy(out, prefix, outCap - 1);
  out[outCap - 1] = 0;
}

void showResultsPageToast(int rows);  // forward decl -- drawSearchScreen() below calls it, it calls back

// `isListNav` marks this draw as caused by list scrolling or paging (see
// resultsRowsPerPage()'s callers in handleButtonPress()) -- only then does a
// page-boundary crossing get a full refresh + "Page N/M" toast instead of the
// plain fast refresh every other draw here uses (typing feedback, entering
// a search, opening/closing the reader, waking from sleep, etc. -- none of
// which are "the user paging through a list" and none of which reported any
// ghosting, so they stay fast and quiet, per PLAN.md #6 Phase X4).
void drawSearchScreen(bool isListNav = false) {
  navPending = false;  // this draw already reflects the latest nav state
  const int rows = resultsRowsPerPage();
  const int newPage = selection / rows;
  const bool pageChanged = isListNav && newPage != lastResultsPage;
  lastResultsPage = newPage;

  display.clearScreen();
  const int16_t rowH = ui->lineHeight(kFontId);
  const int16_t width = static_cast<int16_t>(ui->logicalWidth() - 2 * kMarginX);
  const int16_t top = contentMarginY();

  const int total = resultsCount + 1;  // the box, then each result
  const int listTop = newPage * rows;  // page-aligned -- a real page flip, not a sliding window

  for (int row = 0; row < rows; row++) {
    const int idx = listTop + row;
    if (idx >= total) break;
    const int16_t y = static_cast<int16_t>(top + row * rowH);
    const bool isSel = (idx == selection);

    char label[128];
    if (idx == 0) {
      snprintf(label, sizeof label, "Search: %s", query);
    } else {
      strncpy(label, results[idx - 1].display, sizeof(label) - 1);
      label[sizeof(label) - 1] = 0;
    }

    TextStyle style;
    style.font = kFontId;
    style.maxLines = 1;
    if (isSel) {
      ui->fill(Rect{kMarginX, y, width, rowH}, Paint::solid(Color::Black));
      style.color = Color::White;
    } else {
      style.color = Color::Black;
    }
    ui->text(Rect{kMarginX, y, width, rowH}, label, style);

    if (idx == 0 && typing) {
      // Cursor cell for the letter being cycled: a small white square (so
      // black text reads on it even though this row is otherwise inverted)
      // right after the committed query text.
      TextStyle measure;
      measure.font = kFontId;
      const int16_t cursorX =
          static_cast<int16_t>(kMarginX + ui->measureText(kFontId, label, measure).width);
      char slotStr[2] = {slot ? slot : '_', 0};
      int16_t slotW = static_cast<int16_t>(ui->measureText(kFontId, slotStr, measure).width);
      if (slotW < 10) slotW = 10;
      ui->fill(Rect{cursorX, y, slotW, rowH}, Paint::solid(Color::White));
      TextStyle curStyle;
      curStyle.font = kFontId;
      curStyle.color = Color::Black;
      ui->text(Rect{cursorX, y, slotW, rowH}, slotStr, curStyle);
    }
  }

  if (!packOk) {
    TextStyle msg;
    msg.font = kFontId;
    msg.maxLines = 3;
    ui->text(Rect{kMarginX, static_cast<int16_t>(top + rows * rowH + 10), width, 100},
              "No pack -- see Serial for why", msg);
  }

  char statusText[24];
  buildStatusText(statusText, sizeof statusText);
  drawTopBar("The Guide", statusText);
  if (typing) {
    drawBottomBar("Del", "OK", "<", ">");
  } else {
    drawBottomBar("Search", selection == 0 ? "Type" : "Open", "PgUp", "PgDn");
  }

  display.displayBuffer(pageChanged ? EInkDisplay::FULL_REFRESH : EInkDisplay::FAST_REFRESH);
  if (pageChanged) showResultsPageToast(rows);
}

// Popped after a results-list page flip (drawSearchScreen()'s pageChanged),
// on top of the just-drawn new page. Holds kPageToastMs, then hands back to
// a plain drawSearchScreen() call to redraw clean -- the page hasn't moved
// since, so that redraw is a quiet fast refresh, not another toast.
void showResultsPageToast(int rows) {
  const int totalItems = resultsCount + 1;
  const int totalPages = (totalItems + rows - 1) / rows;
  const int currentPage = selection / rows + 1;
  char msg[24];
  snprintf(msg, sizeof msg, "Page %d/%d", currentPage, totalPages);
  drawCenteredToast(msg);
  display.displayBuffer(EInkDisplay::FAST_REFRESH);
  delay(kPageToastMs);
  drawSearchScreen();
}

// Renders the page starting at `pos`. Always a full refresh -- every call
// here is definitionally a new page of reading content (opening an article,
// turning a page, or resuming one on wake), which is exactly the "new page"
// case PLAN.md #6 Phase X4 calls out for a full refresh; there's no
// sub-page/cursor movement within a reader screen the way the results list
// has, so unlike drawSearchScreen() there's no "just fast-refresh" case here.
void showPage(guidetext::PagePos pos) {
  pageStart = pos;
  display.clearScreen();
  pageEnd = guidetext::renderPage(readerFile, pageStart, *ui, kFontId, kMarginX, contentMarginY());

  char statusText[24];
  buildStatusText(statusText, sizeof statusText);
  const int16_t reserve =
      statusWidth(statusText) > 0 ? static_cast<int16_t>(statusWidth(statusText) + kStatusGap) : 0;
  char titleText[192];
  buildReaderTitleText(titleText, sizeof titleText, reserve);
  drawTopBar(titleText, statusText);
  drawBottomBar("Search", "-", "-", "-");

  display.displayBuffer(EInkDisplay::FULL_REFRESH);
}

// `startPos` defaults to the article's first page; resumeFromSleep() passes
// the saved position instead so a wake-from-sleep lands back where it left off.
void enterReading(uint32_t aid, guidetext::PagePos startPos = guidetext::PagePos{}) {
  readerFile = SdMan.open(Pack::cachePath(), O_RDONLY);
  if (!readerFile) {
    Serial.println("[reader] can't open cache file");
    return;
  }
  currentAid = aid;
  historyCount = 0;

  // GuideText's first line is always "# Title" (guidec.py's
  // html_to_guidetext) -- read it for the top bar instead of plumbing the
  // title through separately, so this works from the `read <id>` serial
  // command too, not just a search-result CONFIRM.
  char raw[sizeof(currentTitle)];
  size_t n = 0;
  int c;
  while (n < sizeof(raw) - 1 && (c = readerFile.read()) >= 0 && c != '\n') {
    raw[n++] = static_cast<char>(c);
  }
  raw[n] = 0;
  const char* titleStart = (n >= 2 && raw[0] == '#' && raw[1] == ' ') ? raw + 2 : raw;
  strncpy(currentTitle, titleStart, sizeof(currentTitle) - 1);
  currentTitle[sizeof(currentTitle) - 1] = 0;

  mode = Mode::Reading;
  showPage(startPos);
}

void pageForward() {
  if (guidetext::atEnd(pageEnd, readerFile)) return;  // already showing the last page
  if (historyCount < kMaxHistory) history[historyCount++] = pageStart;
  showPage(pageEnd);
}

void pageBack() {
  if (historyCount == 0) return;  // already at the first page
  showPage(history[--historyCount]);
}

// Opens article `aid` in Reader mode, from either a serial command or a
// CONFIRM on a search result. Doesn't touch the search/list state at all --
// that's what makes BTN_BACK's "restore the prior query and scroll
// position" free (PLAN.md #6 Phase X3 exit criterion).
void openArticle(uint32_t aid, guidetext::PagePos startPos = guidetext::PagePos{}) {
  if (!packOk) {
    Serial.println("no pack loaded");
    return;
  }
  if (readerFile) readerFile.close();  // switching articles -- drop the previous handle first
  const int32_t n = pack.loadArticleToCache(aid);
  if (n < 0) return;
  Serial.printf("[reader] article %u loaded, opening on screen\n", aid);
  enterReading(aid, startPos);
}

void exitReading() {
  readerFile.close();
  mode = Mode::Search;
  drawSearchScreen();
}

// Persists just enough to resume across a deep-sleep cycle (which resets the
// chip -- every global above is gone on wake): which screen, and either the
// article + page (Reading) or the query + selection (Search, also kept so a
// Reading resume still restores the search list behind it for BTN_BACK).
void saveSleepState() {
  Preferences prefs;
  prefs.begin(kNvsNamespace, false);
  prefs.putUChar("mode", mode == Mode::Reading ? 1 : 0);
  prefs.putString("query", query);
  prefs.putInt("sel", selection);
  if (mode == Mode::Reading) {
    prefs.putUInt("aid", currentAid);
    prefs.putUInt("blkOff", pageStart.blockOffset);
    prefs.putUShort("skip", pageStart.skipChars);
  }
  prefs.end();
}

// Restores `query`/`selection` unconditionally (harmless for a fresh Search
// resume too), and writes the saved article/page into `outAid`/`outPos`,
// returning true only when the saved mode was Reading. Read-only NVS open.
bool loadSleepState(uint32_t& outAid, guidetext::PagePos& outPos) {
  Preferences prefs;
  prefs.begin(kNvsNamespace, true);
  const uint8_t savedMode = prefs.getUChar("mode", 0xFF);
  if (savedMode > 1) {
    prefs.end();
    return false;  // no valid saved state (e.g. NVS wiped independently of a sleep cycle)
  }
  char q[sizeof query] = "";
  prefs.getString("query", q, sizeof q);
  strncpy(query, q, sizeof(query) - 1);
  query[sizeof(query) - 1] = 0;
  selection = prefs.getInt("sel", 0);
  const bool isReading = savedMode == 1;
  if (isReading) {
    outAid = prefs.getUInt("aid", 0);
    outPos.blockOffset = prefs.getUInt("blkOff", 0);
    outPos.skipChars = static_cast<uint16_t>(prefs.getUShort("skip", 0));
  }
  prefs.end();
  return isReading;
}

// Draws the cover graphic, puts the panel controller to sleep (it needs no
// power to hold that image -- see the kIdleSleepMs comment), then puts the
// SoC into deep sleep with GPIO3 (POWER) armed as the only wake source.
// Never returns: a woken chip re-enters through setup(), not back here.
[[noreturn]] void enterSleep() {
  saveSleepState();

  display.clearScreen();
  const BitmapRef cover{kCoverBitmapData, kCoverBitmapWidth, kCoverBitmapHeight, BitmapFormat::BW1, true};
  ui->bitmap(Rect{0, 0, ui->logicalWidth(), ui->logicalHeight()}, cover, BitmapMode::Center);
  display.displayBuffer(EInkDisplay::FULL_REFRESH);

  display.deepSleep();  // UC8253 into its own deep sleep (CMD_DEEP_SLEEP) before the SoC follows it down
  freeink::PowerManager::deepSleepUntilPowerButton();  // waits for release, arms GPIO3, never returns
}

// Prints the button name/id plus a live raw-ADC sample from both ladder
// groups — the bring-up task from PLAN.md #6 Phase X0 step 3: confirm which
// physical zone produced which logical BTN_*, and catch a mis-banded ladder
// (PLAN.md #8 risk: "ladder ADC variance") before it's a mystery bug later.
// readButtonAdc() is explicitly documented safe to call alongside async
// polling (unlike update()/wasPressed(), which async mode owns instead).
void logButtonPress(uint8_t btn) {
  InputManager::ButtonAdcSample g1, g2;
  input.readButtonAdc(g1, g2);
  Serial.printf("press   %-8s (id %u)  adc1=%4d->btn%-2d  adc2=%4d->btn%-2d\n", InputManager::getButtonName(btn), btn,
                g1.raw, g1.button, g2.raw, g2.button);
}

// Dispatches one button press. Runs once per queued press (see loop()) --
// several can queue up while a slow e-ink refresh has the main loop
// blocked, and all of them get handled once it returns, none dropped.
void handleButtonPress(uint8_t btn) {
  logButtonPress(btn);

  if (mode == Mode::Reading) {
    switch (btn) {
      case InputManager::BTN_UP:
        pageBack();
        break;
      case InputManager::BTN_DOWN:
        pageForward();
        break;
      case InputManager::BTN_BACK:
        exitReading();
        break;
      default:
        break;  // LEFT/RIGHT/CONFIRM in Reader mode: still open questions (PLAN.md #1)
    }
    return;
  }

  // Mode::Search
  if (typing) {
    switch (btn) {
      case InputManager::BTN_LEFT:
        cycleBackward();
        navPending = true;
        lastNavMs = millis();
        break;
      case InputManager::BTN_RIGHT:
        cycleForward();
        navPending = true;
        lastNavMs = millis();
        break;
      case InputManager::BTN_CONFIRM:
        commitSlot();
        drawSearchScreen();
        break;
      case InputManager::BTN_BACK:
        backspace();
        drawSearchScreen();
        break;
      case InputManager::BTN_UP:
        typing = false;
        slot = 0;  // leaving typing discards only the uncommitted letter
        drawSearchScreen();
        break;
      case InputManager::BTN_DOWN:
        typing = false;
        slot = 0;
        selection = resultsCount > 0 ? 1 : 0;
        drawSearchScreen();
        break;
      default:
        break;
    }
  } else {
    switch (btn) {
      case InputManager::BTN_UP:
        if (selection > 0) selection--;
        navPending = true;
        lastNavMs = millis();
        break;
      case InputManager::BTN_DOWN:
        if (selection < resultsCount) selection++;
        navPending = true;
        lastNavMs = millis();
        break;
      case InputManager::BTN_LEFT: {  // Page Up: previous page, landing on its first row
        const int rows = resultsRowsPerPage();
        const int page = selection / rows;
        if (page > 0) selection = (page - 1) * rows;
        navPending = true;
        lastNavMs = millis();
        break;
      }
      case InputManager::BTN_RIGHT: {  // Page Down: next page, landing on its first row
        const int rows = resultsRowsPerPage();
        const int lastPage = resultsCount / rows;
        const int page = selection / rows;
        if (page < lastPage) selection = (page + 1) * rows;
        navPending = true;
        lastNavMs = millis();
        break;
      }
      case InputManager::BTN_CONFIRM:
        if (selection == 0) {
          typing = true;
          drawSearchScreen();
        } else {
          openArticle(results[selection - 1].aid);  // switches to Mode::Reading; no redraw here
        }
        break;
      case InputManager::BTN_BACK:
        // Jumps the selection back to the search box, however far down the
        // result list it's scrolled -- one press instead of holding UP.
        selection = 0;
        drawSearchScreen();
        break;
      default:
        break;
    }
  }
}

void printHeap() {
  Serial.printf("[heap] free=%u min-ever-free=%u (budget: PLAN.md #2 targets ~170 KB peak usage)\n",
                ESP.getFreeHeap(), ESP.getMinFreeHeap());
}

void cmdSearch(const char* prefix) {
  if (!packOk) {
    Serial.println("no pack loaded");
    return;
  }
  Pack::Result r[20];
  const int n = pack.search(prefix, r, 20);
  Serial.printf("%d match(es) for %s\n", n, prefix);
  for (int i = 0; i < n; i++) {
    Serial.printf("  [%u] %s\n", r[i].aid, r[i].display);
  }
}

void cmdLoad(uint32_t aid) {
  if (!packOk) {
    Serial.println("no pack loaded");
    return;
  }
  const uint32_t before = ESP.getFreeHeap();
  const int32_t n = pack.loadArticleToCache(aid);
  if (n < 0) return;

  FsFile f = SdMan.open(Pack::cachePath(), O_RDONLY);
  if (!f) {
    Serial.println("[pack] can't reopen cache file for dump");
    return;
  }
  Serial.println("--- article text ---");
  char buf[512];
  int got;
  while ((got = f.read(reinterpret_cast<uint8_t*>(buf), sizeof buf)) > 0) {
    Serial.write(reinterpret_cast<uint8_t*>(buf), got);
  }
  f.close();
  Serial.println("\n--- end ---");
  Serial.printf("[heap] free before=%u after=%u min-ever-free=%u\n", before, ESP.getFreeHeap(), ESP.getMinFreeHeap());
}

// Seeds the DS3231 -- it has no other way to learn wall-clock time (no
// Wi-Fi/NTP in this firmware yet, see PLAN.md #6 Phase X5). Its own coin-cell
// backup keeps this across power cycles, so it's a one-time thing per unit,
// not per boot.
void cmdSetTime(const char* arg) {
  if (!rtc.present()) {
    Serial.println("[rtc] not detected");
    return;
  }
  int y, mo, d, h, mi, s;
  if (sscanf(arg, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) {
    Serial.println("usage: settime YYYY-MM-DD HH:MM:SS");
    return;
  }
  Rtc::DateTime dt;
  dt.year = static_cast<uint16_t>(y);
  dt.month = static_cast<uint8_t>(mo);
  dt.day = static_cast<uint8_t>(d);
  dt.hour = static_cast<uint8_t>(h);
  dt.minute = static_cast<uint8_t>(mi);
  dt.second = static_cast<uint8_t>(s);
  Serial.println(rtc.set(dt) ? "[rtc] time set" : "[rtc] set failed");
}

void handleSerial() {
  static char line[128];
  static size_t n = 0;
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      line[n] = 0;
      if (n > 0) {
        if (strncmp(line, "search ", 7) == 0) {
          cmdSearch(line + 7);
        } else if (strncmp(line, "load ", 5) == 0) {
          cmdLoad(static_cast<uint32_t>(atoi(line + 5)));
        } else if (strncmp(line, "read ", 5) == 0) {
          openArticle(static_cast<uint32_t>(atoi(line + 5)));
        } else if (strcmp(line, "heap") == 0) {
          printHeap();
        } else if (strncmp(line, "settime ", 8) == 0) {
          cmdSetTime(line + 8);
        } else {
          Serial.println("commands: search <text> | load <id> | read <id> | heap | settime YYYY-MM-DD HH:MM:SS");
        }
      }
      n = 0;
    } else if (n < sizeof(line) - 1) {
      line[n++] = c;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);  // let USB CDC enumerate before the first log line
  Serial.println("\nThe Guide -- Xteink X3 firmware");
  Serial.printf("PSRAM: %s (expected: absent -- the C3 has none)\n",
                psramFound() ? "FOUND (unexpected!)" : "absent, as expected");
  Serial.printf("[heap] largest block at boot: %u (free %u)\n", ESP.getMaxAllocHeap(), ESP.getFreeHeap());

  // SD mounts before the display's begin() -- SDCardManager's own comment:
  // an un-deselected, already-powered panel can drive the shared MISO line
  // and break card detection on this shared-SPI-bus board.
  const bool sdOk = SdMan.begin();
  Serial.printf("[heap] largest block after SD.begin: %u (free %u)\n", ESP.getMaxAllocHeap(), ESP.getFreeHeap());
  if (sdOk) {
    packOk = pack.begin(kPackDir);
  } else {
    Serial.println("[SD] not detected -- insert a card with " + String(kPackDir) +
                    "/ on it (see the-guide/converter to build one)");
  }

  // DS3231 shares the sensors I2C bus with the BQ27220 gauge (BatteryMonitor
  // brings up the bus itself, lazily, on its first read -- no begin() call
  // needed there). Absent an RTC or a `settime` yet, buildStatusText() just
  // omits the clock; nothing here depends on it succeeding.
  const bool rtcOk = rtc.begin();
  Serial.printf("[rtc] %s\n", rtcOk ? "detected" : "not detected / not set");

  display.setDisplayX3();
  display.begin();
  Serial.printf("panel: %ux%u\n", display.getDisplayWidth(), display.getDisplayHeight());

  // Portrait: the X3's controller is landscape-native (792x528) but the
  // device is held tall, same as any e-reader with this panel shape --
  // DisplayTarget's own default for exactly this case (width > height).
  static DisplayTarget target(display.getFrameBuffer(), display.getDisplayWidth(), display.getDisplayHeight(),
                               display.getDisplayWidthBytes(), Orientation::Portrait);
  ui = &target;

  input.begin();
  // Async polling: a background task samples the ladder buttons every
  // ~15 ms and queues each press edge, independent of the main loop. Plain
  // update()/wasPressed() polling (Phases X0-X2) missed presses that landed
  // entirely inside a blocking e-ink refresh (380 ms-1.3 s) -- exactly the
  // "buttons feel glitchy" symptom found testing this phase on hardware.
  // Once async is active the app must drain via popPress(), not
  // update()/wasPressed()/isPressed() (see InputManager.h).
  input.beginAsync();

  // Deep sleep resets the chip -- this is what a wake looks like from here,
  // not a resume mid-loop. ESP_SLEEP_WAKEUP_GPIO is the RISC-V (C3) deep-sleep
  // wake source PowerManager arms for the power button (see enterSleep());
  // any other cause (power-on, reset button, USB replug) is a cold boot.
  const bool wokeFromSleep = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO;
  uint32_t resumeAid = 0;
  guidetext::PagePos resumePos;
  const bool resumeReading = wokeFromSleep && packOk && loadSleepState(resumeAid, resumePos);

  refilter();  // populate the (possibly wake-restored) query's results list
  if (resumeReading) {
    openArticle(resumeAid, resumePos);
  } else {
    drawSearchScreen();
  }
  lastActivityMs = millis();
  printHeap();
  Serial.println("ready -- press BACK / CONFIRM / LEFT / RIGHT / UP / DOWN / POWER, or type a command");
}

void loop() {
  // Drains every press queued since the last loop() call -- including ones
  // that landed mid-refresh -- so a burst of presses during a slow redraw
  // gets handled in order once the loop is free again, never dropped.
  uint8_t btn;
  while (input.popPress(btn)) {
    handleButtonPress(btn);
    lastActivityMs = millis();
  }

  // Catch-up redraw for letter cycling and list scrolling/paging (see
  // kNavDebounceMs): fires once no nav button has landed for the debounce
  // window, so a fast burst of presses costs one e-ink refresh instead of one
  // per press. Always passes isListNav=true -- harmless for a cycling burst,
  // since `selection` (what isListNav's page-change check looks at) never
  // moves during those, so it never spuriously fires the page toast.
  if (navPending && mode == Mode::Search && millis() - lastNavMs >= kNavDebounceMs) {
    drawSearchScreen(/*isListNav=*/true);
  }

  // Power is a real GPIO tracked separately from the ladder edge array (see
  // InputManager.h) -- its own accessors are safe alongside async polling.
  // A long-enough hold (release while held >= kManualSleepHoldMs) sleeps now;
  // enterSleep() itself waits for release again before arming the wake
  // source, so this doesn't matter for correctness, only for how promptly a
  // manual sleep request is noticed.
  static bool powerWasDown = false;
  static bool sleepPromptShown = false;  // "Sleeping..." toast, once per hold
  const bool powerNow = input.isPowerButtonPressed();
  if (powerNow != powerWasDown) {
    powerWasDown = powerNow;
    const unsigned long heldMs = input.getPowerButtonHeldTime();
    Serial.printf("%-7s POWER    held %lums\n", powerNow ? "press" : "release", heldMs);
    lastActivityMs = millis();
    if (powerNow) {
      sleepPromptShown = false;  // fresh press starts a new hold
    } else if (heldMs >= kManualSleepHoldMs) {
      enterSleep();
    }
  }
  // Checked every loop tick (not just on the edge above) so the toast shows
  // up mid-hold, the moment the threshold is crossed, not only on release.
  if (powerNow && !sleepPromptShown && input.getPowerButtonHeldTime() >= kManualSleepHoldMs) {
    sleepPromptShown = true;
    drawSleepPrompt();
  }

  if (millis() - lastActivityMs >= kIdleSleepMs) {
    enterSleep();
  }

  handleSerial();
  delay(5);
}
