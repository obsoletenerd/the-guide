#include "pack.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include <cstdlib>
#include <cstring>

// zstddeclib.c is the vendored Meta/Facebook single-file zstd decompressor
// (same file as the RLCD build -- see PLAN.md #4), compiled in by PlatformIO
// as a plain .c translation unit. It carries the full streaming API, not
// just one-shot ZSTD_decompress; declare only what we call.
extern "C" {
typedef struct ZSTD_DCtx_s ZSTD_DStream;
size_t ZSTD_initDStream(ZSTD_DStream* zds);
// Static-allocation API: sizes and creates a DStream *inside* a caller-owned
// fixed buffer, with no heap traffic of its own. See pack.h's dstreamWorkspace_
// comment for why this replaced ZSTD_createDStream()/ZSTD_freeDStream().
size_t ZSTD_estimateDStreamSize(size_t maxWindowSize);
ZSTD_DStream* ZSTD_initStaticDStream(void* workspace, size_t workspaceSize);
typedef struct {
  const void* src;
  size_t size;
  size_t pos;
} ZSTD_inBuffer;
typedef struct {
  void* dst;
  size_t size;
  size_t pos;
} ZSTD_outBuffer;
size_t ZSTD_decompressStream(ZSTD_DStream* zds, ZSTD_outBuffer* output, ZSTD_inBuffer* input);
unsigned ZSTD_isError(size_t code);
const char* ZSTD_getErrorName(size_t code);
// zstddeclib.c is built with ZSTD_STRIP_ERROR_STRINGS (size optimization for
// embedded targets) -- ZSTD_getErrorName() always returns "Error strings
// stripped" as a result, useless for diagnosis. The numeric code survives
// stripping; log that instead.
int ZSTD_getErrorCode(size_t functionResult);
}

namespace {

// Fixed once-at-boot buffers (PLAN.md #8 risk: allocate the decompress
// scratch once and keep it, rather than repeated alloc/free fragmenting the
// C3's one small heap). Input matches the SD card's natural read chunk;
// output is bigger than one line of GuideText so most flushes are one write.
constexpr size_t kInBufSize = 4096;
constexpr size_t kOutBufSize = 8192;
uint8_t g_inBuf[kInBufSize];
uint8_t g_outBuf[kOutBufSize];

// Reads one NUL-terminated string from `f`, byte at a time (Stream::read()
// is the one File API guaranteed present across SdFat versions -- no
// assumption about readBytesUntil()'s exact behavior). Returns the string
// length, or -1 on EOF before a terminator.
int readCString(FsFile& f, char* buf, int cap) {
  int n = 0;
  while (n < cap - 1) {
    const int c = f.read();
    if (c < 0) return -1;
    if (c == 0) break;
    buf[n++] = static_cast<char>(c);
  }
  buf[n] = 0;
  return n;
}

// Minimal reader for our own flat, single-level pack.json -- not a general
// JSON parser (deliberately: this format is ours on both ends, guidec.py and
// here, so a real parser dependency buys nothing).
bool findJsonUInt(const char* json, const char* key, uint32_t* out) {
  char pat[24];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char* p = strstr(json, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':');
  if (!p) return false;
  *out = static_cast<uint32_t>(strtoul(p + 1, nullptr, 10));
  return true;
}

}  // namespace

void normQuery(const char* in, char* out, size_t outCap) {
  size_t o = 0;
  bool pendingSpace = false;
  for (const char* p = in; *p && o + 1 < outCap; ++p) {
    char c = *p;
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!alnum) {
      if (o > 0) pendingSpace = true;
      continue;
    }
    if (pendingSpace) {
      out[o++] = ' ';
      pendingSpace = false;
      if (o + 1 >= outCap) break;
    }
    out[o++] = c;
  }
  out[o] = 0;
}

Pack::~Pack() {}

bool Pack::begin(const char* dir) {
  strncpy(dir_, dir, sizeof(dir_) - 1);
  dir_[sizeof(dir_) - 1] = 0;

  char path[64];
  snprintf(path, sizeof path, "%s/pack.json", dir_);
  char json[512];
  const size_t n = SdMan.readFileToBuffer(path, json, sizeof json);
  if (n == 0) {
    Serial.printf("[pack] can't read %s\n", path);
    return false;
  }

  findJsonUInt(json, "format", &meta_.format);
  findJsonUInt(json, "articles", &meta_.articles);
  findJsonUInt(json, "titles", &meta_.titles);
  findJsonUInt(json, "shards", &meta_.shards);
  findJsonUInt(json, "stride", &meta_.stride);
  meta_.windowLog = 0;
  findJsonUInt(json, "window_log", &meta_.windowLog);

  if (meta_.windowLog == 0 || meta_.windowLog > MAX_WINDOW_LOG) {
    Serial.printf(
        "[pack] refusing %s: window_log=%u (need 1..%u) -- rebuild with "
        "`guidec build --window-log %u`\n",
        dir_, meta_.windowLog, MAX_WINDOW_LOG, MAX_WINDOW_LOG);
    return false;
  }

  snprintf(path, sizeof path, "%s/titles.bin", dir_);
  titlesF_ = SdMan.open(path, O_RDONLY);
  snprintf(path, sizeof path, "%s/titles.sparse", dir_);
  sparseF_ = SdMan.open(path, O_RDONLY);
  snprintf(path, sizeof path, "%s/articles.bin", dir_);
  articlesF_ = SdMan.open(path, O_RDONLY);
  if (!titlesF_ || !sparseF_ || !articlesF_) {
    Serial.printf("[pack] missing titles.bin/titles.sparse/articles.bin under %s\n", dir_);
    return false;
  }
  sparseCount_ = static_cast<uint32_t>(sparseF_.fileSize() / 4);

  SdMan.ensureDirectoryExists("/.guide");

  // One-time zstd workspace, sized for the full MAX_WINDOW_LOG cap and
  // allocated now -- early, while the heap is least fragmented -- so no
  // article's decompression ever needs a fresh heap allocation. See
  // pack.h's dstreamWorkspace_ comment.
  //
  // ZSTD_initStaticDCtx() requires the workspace to start 8-byte aligned
  // (it returns NULL otherwise -- confirmed on hardware: malloc()'s return
  // here isn't guaranteed 8-aligned by this platform's heap allocator the
  // way desktop libc's is). Over-allocate by up to 7 bytes and align up
  // manually; the size passed to zstd shrinks by however much padding that
  // consumed.
  const size_t needed = ZSTD_estimateDStreamSize(static_cast<size_t>(1) << MAX_WINDOW_LOG);
  uint8_t* raw = static_cast<uint8_t*>(malloc(needed + 8));
  if (!raw) {
    Serial.printf("[pack] can't allocate %u-byte zstd workspace (OOM)\n", static_cast<unsigned>(needed + 8));
    return false;
  }
  const uintptr_t misalignment = reinterpret_cast<uintptr_t>(raw) & 7;
  const size_t padding = misalignment ? (8 - misalignment) : 0;
  dstreamWorkspace_ = raw + padding;
  dstreamWorkspaceSize_ = needed + 8 - padding;

  Serial.printf("[pack] %s: %u articles, %u titles, %u shard(s), window_log=%u, zstd workspace=%uB\n", dir_,
                meta_.articles, meta_.titles, meta_.shards, meta_.windowLog,
                static_cast<unsigned>(dstreamWorkspaceSize_));
  return true;
}

uint32_t Pack::sparseOffset(uint32_t index) {
  sparseF_.seekSet(static_cast<uint64_t>(index) * 4);
  uint8_t b[4] = {0, 0, 0, 0};
  sparseF_.read(b, 4);
  return b[0] | (b[1] << 8) | (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

bool Pack::recordAt(uint32_t off, char* key, char* display, uint32_t* aid, uint32_t* consumed) {
  if (!titlesF_.seekSet(off)) return false;
  const int kn = readCString(titlesF_, key, MAX_KEY);
  if (kn < 0) return false;
  const int dn = readCString(titlesF_, display, MAX_KEY);
  if (dn < 0) return false;
  uint8_t idBuf[4];
  if (titlesF_.read(idBuf, 4) != 4) return false;
  *aid = idBuf[0] | (idBuf[1] << 8) | (static_cast<uint32_t>(idBuf[2]) << 16) |
         (static_cast<uint32_t>(idBuf[3]) << 24);
  *consumed = static_cast<uint32_t>(kn) + 1 + static_cast<uint32_t>(dn) + 1 + 4;
  return true;
}

int Pack::search(const char* prefixRaw, Result* out, int limit) {
  if (sparseCount_ == 0) return 0;

  char key[MAX_KEY];
  normQuery(prefixRaw, key, sizeof key);
  const size_t keyLen = strlen(key);

  char k[MAX_KEY], disp[MAX_KEY];
  uint32_t aid = 0, consumed = 0;

  // Binary search: last sparse entry whose key <= the query (mirrors
  // Pack.search() in guidec.py exactly).
  int32_t lo = 0, hi = static_cast<int32_t>(sparseCount_) - 1;
  while (lo < hi) {
    const int32_t mid = (lo + hi + 1) / 2;
    recordAt(sparseOffset(static_cast<uint32_t>(mid)), k, disp, &aid, &consumed);
    if (strcmp(k, key) <= 0) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }

  uint32_t off = sparseOffset(static_cast<uint32_t>(lo));
  const uint64_t size = titlesF_.fileSize();
  int count = 0;
  while (off < size && count < limit) {
    if (!recordAt(off, k, disp, &aid, &consumed)) break;
    if (strncmp(k, key, keyLen) == 0) {
      strncpy(out[count].key, k, MAX_KEY - 1);
      out[count].key[MAX_KEY - 1] = 0;
      strncpy(out[count].display, disp, MAX_KEY - 1);
      out[count].display[MAX_KEY - 1] = 0;
      out[count].aid = aid;
      count++;
    } else if (strcmp(k, key) > 0) {
      break;
    }
    off += consumed;
  }
  return count;
}

int32_t Pack::loadArticleToCache(uint32_t aid) {
  if (aid >= meta_.articles) {
    Serial.printf("[pack] bad article id %u (have %u)\n", aid, meta_.articles);
    return -1;
  }
  articlesF_.seekSet(static_cast<uint64_t>(aid) * 10);
  uint8_t rec[10];
  if (articlesF_.read(rec, 10) != 10) {
    Serial.println("[pack] short read in articles.bin");
    return -1;
  }
  const uint16_t shard = rec[0] | (rec[1] << 8);
  const uint32_t off = rec[2] | (rec[3] << 8) | (static_cast<uint32_t>(rec[4]) << 16) |
                        (static_cast<uint32_t>(rec[5]) << 24);
  const uint32_t len = rec[6] | (rec[7] << 8) | (static_cast<uint32_t>(rec[8]) << 16) |
                        (static_cast<uint32_t>(rec[9]) << 24);

  char shardPath[48];
  snprintf(shardPath, sizeof shardPath, "%s/shard_%04u.dat", dir_, shard);
  FsFile shardF = SdMan.open(shardPath, O_RDONLY);
  if (!shardF) {
    Serial.printf("[pack] can't open %s\n", shardPath);
    return -1;
  }
  if (!shardF.seekSet(off)) {
    Serial.printf("[pack] seek failed in %s\n", shardPath);
    shardF.close();
    return -1;
  }

  FsFile outF = SdMan.open(kCachePath, O_RDWR | O_CREAT | O_TRUNC);
  if (!outF) {
    Serial.printf("[pack] can't open cache file %s\n", kCachePath);
    shardF.close();
    return -1;
  }

  // A fresh DStream every call, carved out of dstreamWorkspace_ (allocated
  // once, in begin(), sized for the full window cap) -- no heap traffic at
  // all here. Two things were tried and rejected before this: (1) one
  // heap-allocated DStream reused via ZSTD_initDStream() for the whole
  // session -- broke because the compressor sizes each frame's window to
  // its actual content (up to the cap), so a later article needing a
  // *bigger* window than an earlier one used couldn't grow into the same
  // buffer (reproducible "memory_allocation" error on hardware); (2) a
  // fresh heap-allocated DStream per call -- broke even worse, because by
  // the time any article loads, SD/display/async-input activity has
  // already fragmented the heap enough that a fresh ~64 KB allocation
  // reliably fails, where it would have succeeded if requested at boot.
  ZSTD_DStream* ds = ZSTD_initStaticDStream(dstreamWorkspace_, dstreamWorkspaceSize_);
  if (!ds) {
    Serial.println("[pack] ZSTD_initStaticDStream failed (workspace too small?)");
    outF.close();
    shardF.close();
    return -1;
  }
  ZSTD_initDStream(ds);

  // Bounded to exactly `len` compressed bytes -- shards are frames
  // concatenated with no separator, so an unbounded read would decode
  // straight into the *next* article's frame once this one ends. (Caught
  // the desktop equivalent of this bug in guidec.py's --stream self-check
  // before writing this loop -- see the-guide/converter/guidec.py.)
  uint32_t remaining = len;
  int32_t totalOut = 0;
  bool ok = true;
  while (remaining > 0) {
    const size_t want = remaining < kInBufSize ? remaining : kInBufSize;
    const int got = shardF.read(g_inBuf, want);
    if (got <= 0) {
      Serial.println("[pack] short read in shard file");
      ok = false;
      break;
    }
    remaining -= static_cast<uint32_t>(got);

    ZSTD_inBuffer in{g_inBuf, static_cast<size_t>(got), 0};
    while (in.pos < in.size) {
      ZSTD_outBuffer outb{g_outBuf, kOutBufSize, 0};
      const size_t ret = ZSTD_decompressStream(ds, &outb, &in);
      if (ZSTD_isError(ret)) {
        Serial.printf("[pack] zstd error code %d (%s) at comp-byte %u/%u, remaining=%u\n",
                      ZSTD_getErrorCode(ret), ZSTD_getErrorName(ret), len - remaining, len, remaining);
        ok = false;
        break;
      }
      if (outb.pos > 0) {
        outF.write(g_outBuf, outb.pos);
        totalOut += static_cast<int32_t>(outb.pos);
      }
    }
    if (!ok) break;
  }

  outF.close();
  shardF.close();
  if (!ok) return -1;
  Serial.printf("[pack] article %u: %u B comp -> %d B text\n", aid, len, totalOut);
  return totalOut;
}
