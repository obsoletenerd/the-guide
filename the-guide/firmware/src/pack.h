#pragma once

// On-disk pack access for the Xteink X3 (no PSRAM). Mirrors the desktop
// `Pack` class in the-guide/converter/guidec.py -- same titles.bin/
// titles.sparse/articles.bin/shard_NNNN.dat format, same sparse-index binary
// search algorithm -- just run against the SD card instead of RAM, and
// streaming-decompressing to a cache file instead of a RAM buffer. See
// ../../hardware/xteink-x3/PLAN.md #2 for why (no PSRAM: window_log-capped
// zstd, on-disk search, decompress-to-cache-file).

#include <SdFat.h>

#include <cstddef>
#include <cstdint>

class Pack {
 public:
  struct Meta {
    uint32_t format = 0;
    uint32_t articles = 0;
    uint32_t titles = 0;
    uint32_t shards = 0;
    uint32_t stride = 64;
    uint32_t windowLog = 0;  // 0 = pack.json omitted it -- refused, see begin()
  };

  static constexpr int MAX_KEY = 96;  // titles.bin norm_key/display cap; longer is truncated
  struct Result {
    char key[MAX_KEY];
    char display[MAX_KEY];
    uint32_t aid;
  };

  // Largest zstd window this firmware will decompress: 2 KB, not the 64 KB
  // PLAN.md #2 originally guessed. Measured on hardware, in order:
  //  - zstd's *static* DStream workspace isn't sized by the window alone --
  //    it budgets a full ring buffer (~window + 2*maxBlockSize) since it
  //    doesn't know a frame's content size in advance, so 64 KB needed
  //    358 KB of workspace (blew the whole heap).
  //  - Even the C3's *largest allocatable block* at boot -- before any of
  //    our own code runs -- is only ~115 KB, well under its ~250 KB total
  //    free heap (framework/RTOS startup fragmentation we don't control).
  //    A window_log of 13 (8 KB) needs a 128 KB workspace: doesn't fit.
  //  - Pack size vs. window_log has a cliff, not a smooth curve, on this
  //    corpus: window_log 13 -> 12 roughly *doubles* pack size (2.1 MB ->
  //    3.2 MB shard, top-100 test ZIM), but 12 -> 11 -> 10 barely moves
  //    (3.2 -> 3.3 -> 3.4 MB). 11 is the sweet spot: same size as 12
  //    (already past the cliff) but with a real ~10 KB safety margin
  //    against the ~115 KB ceiling instead of ~2 KB.
  // Costs guidec.py packs roughly 55% more disk space than window_log 13
  // would have (measured on the top-100 test ZIM) -- fine; storage is
  // cheap, RAM on a PSRAM-less C3 is not. Packs built with a bigger window
  // (or none, i.e. `guidec build` without --window-log) are refused by
  // begin().
  static constexpr uint32_t MAX_WINDOW_LOG = 11;

  ~Pack();

  // Opens pack.json/titles.bin/titles.sparse/articles.bin under `dir` (an SD
  // path, e.g. "/pack"). SD must already be mounted (SdMan.begin()). Returns
  // false (logging why to Serial) on a missing pack or a window_log this
  // firmware can't afford.
  bool begin(const char* dir);
  const Meta& meta() const { return meta_; }

  // Prefix search over the on-disk sparse index -- same algorithm as
  // Pack.search() in guidec.py, zero extra RAM beyond `out[0..limit)`.
  int search(const char* prefix, Result* out, int limit);

  // Streaming-decompresses article `aid`'s frame straight to a cache file on
  // SD (PLAN.md #2b) -- never holds the compressed frame or the decompressed
  // text in RAM at once. Returns the decompressed byte count, or -1 on error
  // (logged to Serial). Read the text back from cachePath() afterward.
  int32_t loadArticleToCache(uint32_t aid);
  static const char* cachePath() { return kCachePath; }

 private:
  static constexpr const char* kCachePath = "/.guide/cur.txt";

  // Reads one titles.bin record at byte offset `off`: norm_key\0 display\0
  // aid:u32le. Writes the byte length consumed (for the caller's linear-scan
  // offset advance) to `consumed`. Returns false on a read failure.
  bool recordAt(uint32_t off, char* key, char* display, uint32_t* aid, uint32_t* consumed);
  uint32_t sparseOffset(uint32_t index);  // titles.sparse[index] -> titles.bin byte offset

  char dir_[32] = {0};
  Meta meta_;
  FsFile titlesF_;
  FsFile sparseF_;
  FsFile articlesF_;
  uint32_t sparseCount_ = 0;

  // A fixed-size workspace for zstd's *static* DStream API (ZSTD_estimateDStreamSize
  // + ZSTD_initStaticDStream), sized once in begin() for the full MAX_WINDOW_LOG
  // and malloc'd exactly once, early (right after boot, before other allocations
  // fragment the heap). Every loadArticleToCache() call re-inits a DStream inside
  // this same block -- no heap traffic at all after the one-time allocation, so
  // there's nothing to grow or fragment. See loadArticleToCache() for why a
  // reused *heap-allocated* ZSTD_DStream wasn't enough: a small early article's
  // window fit fine, but the compressor sizes each frame's actual window to its
  // own content (up to the cap), and a later, larger article's window couldn't
  // grow into whatever the first one had allocated.
  uint8_t* dstreamWorkspace_ = nullptr;
  size_t dstreamWorkspaceSize_ = 0;
};

// ASCII-only query normalization: lowercases, folds anything that isn't
// a-z/0-9 to a space, collapses runs, and drops leading/trailing space.
// Matches guidec.py's norm_key() for the character set the on-device typing
// UI can even produce (space + A-Z + 0-9 + a little punctuation -- PLAN.md
// #1). Full Unicode NFKD/diacritic folding only matters for titles.bin's
// *stored* keys (already folded once, on the desktop, at build time) --
// never for what gets typed here, so this deliberately doesn't attempt it.
void normQuery(const char* in, char* out, size_t outCap);
