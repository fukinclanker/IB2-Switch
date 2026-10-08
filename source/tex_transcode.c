/* tex_transcode.c -- ETC2/EAC -> S3TC (DXT1/DXT5) upload transcoder + texture
 * memory accounting.
 *
 * Why this exists (1.1.0)
 * -----------------------
 * The iOS game ships PVRTC textures; libib3.so re-encodes them to ETC2 and
 * uploads them with glCompressedTexImage2D. On Switch, libdrm_nouveau reports
 * chipset 0x120 (GM200) and Mesa 20.1's nvc0_screen_is_format_supported() only
 * allows ETC/ASTC when chipset == 0x12b or the 3D class is GK20A, so every
 * ETC2 upload falls back to st/mesa's software path. That path keeps a
 * malloc'd copy of the compressed data (st_texture_image::compressed_data)
 * AND decompresses into an uncompressed RGBA8 GPU texture: an ETC2 RGB
 * texture costs 4.5 bytes per texel instead of 0.5 (9x), ETC2 RGBA 5 instead
 * of 1 (5x). Long sessions ran the heap out (std::bad_alloc while streaming
 * CharTextures.tfc) and late-streamed textures could fail to allocate.
 *
 * Maxwell decodes S3TC natively and Mesa exposes it without restriction, so
 * each ETC2 upload is decoded here and re-encoded as DXT1 (RGB, same 4 bpp as
 * ETC2 RGB) or DXT5 (alpha, same 8 bpp as ETC2 RGBA), and Mesa never sees ETC.
 *
 * Safety nets: PBO uploads and unknown formats pass through untouched; if the
 * first transcoded upload raises a GL error, transcoding is switched off for
 * the session and that upload is redone in its original format.
 *
 * The ETC2 decoding rules follow the OpenGL ES 3.0 spec (Annex C) and were
 * cross-checked block-for-block against Mesa's texcompress_etc.c.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tex_transcode.h"

#ifdef __SWITCH__
#include <switch.h>
#include <GLES2/gl2.h>
#include "util.h"
#define TT_HAVE_GL 1
#else
#define TT_HAVE_GL 0
#endif

/* ------------------------------------------------------------------------ *
 * ETC2 / EAC decoding. Output: rgba[p*4], p = y*4 + x (row-major).
 * ------------------------------------------------------------------------ */

static const int etc1_mod[8][4] = {
    {2, 8, -2, -8},     {5, 17, -5, -17},   {9, 29, -9, -29},
    {13, 42, -13, -42}, {18, 60, -18, -60}, {24, 80, -24, -80},
    {33, 106, -33, -106}, {47, 183, -47, -183},
};
static const int etc2_dist[8] = {3, 6, 11, 16, 23, 32, 41, 64};
static const int eac_mod[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14},   {-3, -7, -10, -13, 2, 6, 9, 12},
    {-2, -5, -8, -13, 1, 4, 7, 12},   {-2, -4, -6, -13, 1, 3, 5, 12},
    {-3, -6, -8, -12, 2, 5, 7, 11},   {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10},   {-3, -5, -8, -11, 2, 4, 7, 10},
    {-2, -6, -8, -10, 1, 5, 7, 9},    {-2, -5, -8, -10, 1, 4, 7, 9},
    {-2, -4, -8, -10, 1, 3, 7, 9},    {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},    {-1, -2, -3, -10, 0, 1, 2, 9},
    {-4, -6, -8, -9, 3, 5, 7, 8},     {-3, -5, -7, -9, 2, 4, 6, 8},
};

static inline uint8_t clamp255(int v) {
  return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}
static inline int ext4(int x) { return (x << 4) | x; }
static inline int ext5(int x) { return (x << 3) | (x >> 2); }
static inline int ext6(int x) { return (x << 2) | (x >> 4); }
static inline int ext7(int x) { return (x << 1) | (x >> 6); }

/* punch: 0 = ETC1/ETC2 RGB8, 1 = RGB8 punchthrough alpha1. Writes RGB and A
 * (A = 255 unless a punchthrough texel is transparent). */
void tt_decode_etc2_rgb(const uint8_t *s, uint8_t *rgba, int punch) {
  static const int lut3[8] = {0, 1, 2, 3, -4, -3, -2, -1};
  const uint32_t idxbits = ((uint32_t)s[4] << 24) | ((uint32_t)s[5] << 16) |
                           ((uint32_t)s[6] << 8) | (uint32_t)s[7];
  const int bit33 = (s[3] >> 1) & 1; /* diff bit, or "opaque" for punchthrough */
  const int opaque = punch ? bit33 : 1;
  const int diff = punch ? 1 : bit33;

  int mode; /* 0 ind, 1 diff, 2 T, 3 H, 4 planar */
  if (!diff) {
    mode = 0;
  } else {
    const int r = (s[0] >> 3) + lut3[s[0] & 7];
    const int g = (s[1] >> 3) + lut3[s[1] & 7];
    const int b = (s[2] >> 3) + lut3[s[2] & 7];
    if (r < 0 || r > 31)
      mode = 2;
    else if (g < 0 || g > 31)
      mode = 3;
    else if (b < 0 || b > 31)
      mode = 4;
    else
      mode = 1;
  }

  if (mode == 0 || mode == 1) {
    int base[2][3];
    for (int c = 0; c < 3; c++) {
      if (mode == 0) {
        base[0][c] = ext4(s[c] >> 4);
        base[1][c] = ext4(s[c] & 15);
      } else {
        const int hi = s[c] >> 3;
        base[0][c] = ext5(hi);
        base[1][c] = ext5((hi + lut3[s[c] & 7]) & 31);
      }
    }
    const int tab[2] = {(s[3] >> 5) & 7, (s[3] >> 2) & 7};
    const int flip = s[3] & 1;
    for (int x = 0; x < 4; x++) {
      for (int y = 0; y < 4; y++) {
        const int bit = y + x * 4;
        const int idx = (int)(((idxbits >> (15 + bit)) & 2) | ((idxbits >> bit) & 1));
        uint8_t *d = rgba + (y * 4 + x) * 4;
        if (!opaque && idx == 2) {
          d[0] = d[1] = d[2] = d[3] = 0;
          continue;
        }
        const int blk = flip ? (y >= 2) : (x >= 2);
        int m = etc1_mod[tab[blk]][idx];
        if (!opaque && (idx == 0)) /* non-opaque table: idx 0 and 2 are 0 */
          m = 0;
        d[0] = clamp255(base[blk][0] + m);
        d[1] = clamp255(base[blk][1] + m);
        d[2] = clamp255(base[blk][2] + m);
        d[3] = 255;
      }
    }
    return;
  }

  if (mode == 2 || mode == 3) {
    int c1[3], c2[3], dist;
    if (mode == 2) { /* T */
      c1[0] = ext4((((s[0] >> 3) & 3) << 2) | (s[0] & 3));
      c1[1] = ext4(s[1] >> 4);
      c1[2] = ext4(s[1] & 15);
      c2[0] = ext4(s[2] >> 4);
      c2[1] = ext4(s[2] & 15);
      c2[2] = ext4(s[3] >> 4);
      dist = etc2_dist[(((s[3] >> 2) & 3) << 1) | (s[3] & 1)];
    } else { /* H */
      c1[0] = ext4((s[0] >> 3) & 15);
      c1[1] = ext4(((s[0] & 7) << 1) | ((s[1] >> 4) & 1));
      c1[2] = ext4((s[1] & 8) | ((s[1] & 3) << 1) | (s[2] >> 7));
      c2[0] = ext4((s[2] >> 3) & 15);
      c2[1] = ext4(((s[2] & 7) << 1) | (s[3] >> 7));
      c2[2] = ext4((s[3] >> 3) & 15);
      const int v1 = (c1[0] << 16) | (c1[1] << 8) | c1[2];
      const int v2 = (c2[0] << 16) | (c2[1] << 8) | c2[2];
      dist = etc2_dist[(s[3] & 4) | ((s[3] & 1) << 1) | (v1 >= v2)];
    }
    uint8_t paint[4][3];
    for (int c = 0; c < 3; c++) {
      if (mode == 2) {
        paint[0][c] = clamp255(c1[c]);
        paint[1][c] = clamp255(c2[c] + dist);
        paint[2][c] = clamp255(c2[c]);
        paint[3][c] = clamp255(c2[c] - dist);
      } else {
        paint[0][c] = clamp255(c1[c] + dist);
        paint[1][c] = clamp255(c1[c] - dist);
        paint[2][c] = clamp255(c2[c] + dist);
        paint[3][c] = clamp255(c2[c] - dist);
      }
    }
    for (int x = 0; x < 4; x++) {
      for (int y = 0; y < 4; y++) {
        const int bit = y + x * 4;
        const int idx = (int)(((idxbits >> (15 + bit)) & 2) | ((idxbits >> bit) & 1));
        uint8_t *d = rgba + (y * 4 + x) * 4;
        if (!opaque && idx == 2) {
          d[0] = d[1] = d[2] = d[3] = 0;
          continue;
        }
        d[0] = paint[idx][0];
        d[1] = paint[idx][1];
        d[2] = paint[idx][2];
        d[3] = 255;
      }
    }
    return;
  }

  /* planar (always opaque) */
  const int ro = ext6((s[0] >> 1) & 0x3f);
  const int go = ext7(((s[0] & 1) << 6) | ((s[1] >> 1) & 0x3f));
  const int bo = ext6(((s[1] & 1) << 5) | (s[2] & 0x18) | ((s[2] & 3) << 1) |
                      (s[3] >> 7));
  const int rh = ext6(((s[3] & 0x7c) >> 1) | (s[3] & 1));
  const int gh = ext7((s[4] >> 1) & 0x7f);
  const int bh = ext6(((s[4] & 1) << 5) | ((s[5] >> 3) & 0x1f));
  const int rv = ext6(((s[5] & 7) << 3) | ((s[6] >> 5) & 7));
  const int gv = ext7(((s[6] & 0x1f) << 2) | ((s[7] >> 6) & 3));
  const int bv = ext6(s[7] & 0x3f);
  for (int y = 0; y < 4; y++) {
    for (int x = 0; x < 4; x++) {
      uint8_t *d = rgba + (y * 4 + x) * 4;
      d[0] = clamp255((x * (rh - ro) + y * (rv - ro) + 4 * ro + 2) >> 2);
      d[1] = clamp255((x * (gh - go) + y * (gv - go) + 4 * go + 2) >> 2);
      d[2] = clamp255((x * (bh - bo) + y * (bv - bo) + 4 * bo + 2) >> 2);
      d[3] = 255;
    }
  }
}

/* EAC alpha (8 bytes) -> rgba[p*4+3]. */
void tt_decode_eac_alpha(const uint8_t *s, uint8_t *rgba) {
  const int base = s[0];
  const int mult = s[1] >> 4;
  const int *tab = eac_mod[s[1] & 15];
  const uint64_t bits = ((uint64_t)s[2] << 40) | ((uint64_t)s[3] << 32) |
                        ((uint64_t)s[4] << 24) | ((uint64_t)s[5] << 16) |
                        ((uint64_t)s[6] << 8) | (uint64_t)s[7];
  for (int x = 0; x < 4; x++) {
    for (int y = 0; y < 4; y++) {
      const int shift = ((3 - y) + (3 - x) * 4) * 3;
      const int idx = (int)((bits >> shift) & 7);
      rgba[(y * 4 + x) * 4 + 3] = clamp255(base + tab[idx] * mult);
    }
  }
}

/* ------------------------------------------------------------------------ *
 * S3TC encoding: stb_dxt v1.12 (public domain / MIT, see NOTICE.md). Its
 * tables are built without global state since v1.11, so it is safe to call
 * from the worker threads below.
 * ------------------------------------------------------------------------ */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define STB_DXT_STATIC
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"
#pragma GCC diagnostic pop

void tt_encode_bc1(const uint8_t *rgba, uint8_t *out8) {
  stb_compress_dxt_block(out8, rgba, 0, STB_DXT_NORMAL);
}
void tt_encode_bc3(const uint8_t *rgba, uint8_t *out16) {
  stb_compress_dxt_block(out16, rgba, 1, STB_DXT_NORMAL);
}

/* ------------------------------------------------------------------------ *
 * Whole-image transcoding
 * ------------------------------------------------------------------------ */

#define GLE_ETC1_RGB8 0x8D64
#define GLE_R11_EAC 0x9270
#define GLE_RGB8_ETC2 0x9274
#define GLE_SRGB8_ETC2 0x9275
#define GLE_RGB8_PT_ETC2 0x9276
#define GLE_SRGB8_PT_ETC2 0x9277
#define GLE_RGBA8_ETC2_EAC 0x9278
#define GLE_SRGB8_A8_ETC2_EAC 0x9279
#define GLE_DXT1_RGB 0x83F0
#define GLE_DXT5_RGBA 0x83F3
#define GLE_SRGB_DXT1 0x8C4C
#define GLE_SRGB_A_DXT5 0x8C4F

int tt_plan(unsigned src_format, unsigned *dst_format, int *src_block_bytes,
            int *dst_block_bytes, int *kind) {
  switch (src_format) {
  case GLE_ETC1_RGB8:
  case GLE_RGB8_ETC2:
    *dst_format = GLE_DXT1_RGB; *src_block_bytes = 8; *dst_block_bytes = 8;
    *kind = TT_KIND_RGB;
    return 1;
  case GLE_SRGB8_ETC2:
    *dst_format = GLE_SRGB_DXT1; *src_block_bytes = 8; *dst_block_bytes = 8;
    *kind = TT_KIND_RGB;
    return 1;
  case GLE_RGB8_PT_ETC2:
    /* DXT5 rather than DXT1's 1-bit mode: exact 0/255 alpha, simple encoder */
    *dst_format = GLE_DXT5_RGBA; *src_block_bytes = 8; *dst_block_bytes = 16;
    *kind = TT_KIND_PUNCH;
    return 1;
  case GLE_SRGB8_PT_ETC2:
    *dst_format = GLE_SRGB_A_DXT5; *src_block_bytes = 8; *dst_block_bytes = 16;
    *kind = TT_KIND_PUNCH;
    return 1;
  case GLE_RGBA8_ETC2_EAC:
    *dst_format = GLE_DXT5_RGBA; *src_block_bytes = 16; *dst_block_bytes = 16;
    *kind = TT_KIND_RGBA;
    return 1;
  case GLE_SRGB8_A8_ETC2_EAC:
    *dst_format = GLE_SRGB_A_DXT5; *src_block_bytes = 16; *dst_block_bytes = 16;
    *kind = TT_KIND_RGBA;
    return 1;
  default:
    return 0;
  }
}

static unsigned transcode_range(const uint8_t *src, uint8_t *dst,
                                size_t first, size_t count, int kind) {
  const size_t sbb = kind == TT_KIND_RGBA ? 16 : 8;
  const size_t dbb = kind == TT_KIND_RGB ? 8 : 16;
  src += first * sbb;
  dst += first * dbb;
  uint8_t px[64];
  unsigned black = 0;
  for (size_t i = 0; i < count; i++) {
    if (kind == TT_KIND_RGBA) {
      tt_decode_etc2_rgb(src + 8, px, 0);
      tt_decode_eac_alpha(src, px);
      tt_encode_bc3(px, dst);
    } else if (kind == TT_KIND_PUNCH) {
      tt_decode_etc2_rgb(src, px, 1);
      tt_encode_bc3(px, dst);
    } else {
      tt_decode_etc2_rgb(src, px, 0);
      tt_encode_bc1(px, dst);
    }
    src += sbb;
    dst += dbb;
    int dark = 1;
    for (int p = 0; p < 16 && dark; p++)
      if (px[p * 4] > 6 || px[p * 4 + 1] > 6 || px[p * 4 + 2] > 6 ||
          px[p * 4 + 3] < 250)
        dark = 0;
    black += (unsigned)dark;
  }
  return black;
}

#ifdef __SWITCH__
/* ---- worker pool -------------------------------------------------------
 * Transcoding runs inside the game's glCompressedTexImage2D call on its
 * render thread. Large uploads are split across two helper threads on the
 * application cores the guest leaves idle (guest pthreads all start on the
 * default core), so a 1024x1024 texture costs ~1/3 of the wall time. */
#define TT_WORKERS 2
#define TT_CHUNK 256 /* blocks per work item */
#define TT_PARALLEL_MIN 4096 /* below this, threading costs more than it saves */

typedef struct {
  const uint8_t *src;
  uint8_t *dst;
  size_t nblocks;
  int kind;
  volatile size_t next;
  volatile unsigned black;
  volatile int active;
} TTJob;

static Mutex g_pool_lock;     /* one parallel job at a time */
static Mutex g_job_lock;
static CondVar g_job_cv, g_done_cv;
static TTJob *g_job;
static unsigned g_job_gen;
static Thread g_workers[TT_WORKERS];
static int g_pool_state; /* 0 untried, 1 running, -1 unavailable */

static void job_run(TTJob *j) {
  for (;;) {
    const size_t first = __atomic_fetch_add(&j->next, TT_CHUNK, __ATOMIC_RELAXED);
    if (first >= j->nblocks)
      break;
    size_t count = j->nblocks - first;
    if (count > TT_CHUNK)
      count = TT_CHUNK;
    const unsigned b = transcode_range(j->src, j->dst, first, count, j->kind);
    if (b)
      __atomic_fetch_add(&j->black, b, __ATOMIC_RELAXED);
  }
}

static void worker_main(void *arg) {
  (void)arg;
  unsigned seen = 0;
  for (;;) {
    mutexLock(&g_job_lock);
    while (g_job_gen == seen)
      condvarWait(&g_job_cv, &g_job_lock);
    seen = g_job_gen;
    TTJob *j = g_job;
    mutexUnlock(&g_job_lock);
    if (j)
      job_run(j);
    mutexLock(&g_job_lock);
    if (j && --j->active == 0)
      condvarWakeAll(&g_done_cv);
    mutexUnlock(&g_job_lock);
  }
}

static void pool_start(void) {
  mutexInit(&g_pool_lock);
  mutexInit(&g_job_lock);
  condvarInit(&g_job_cv);
  condvarInit(&g_done_cv);
  u64 mask = 0;
  svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
  int started = 0;
  for (int i = 0; i < TT_WORKERS; i++) {
    const int core = 1 + i; /* cores 1 and 2; the guest lives on core 0 */
    if (!(mask & (1ull << core)))
      continue;
    if (R_FAILED(threadCreate(&g_workers[started], worker_main, NULL, NULL,
                              0x8000, 0x2C, core)))
      continue;
    if (R_FAILED(threadStart(&g_workers[started]))) {
      threadClose(&g_workers[started]);
      continue;
    }
    started++;
  }
  g_pool_state = started ? started : -1;
  debugPrintf("tex: transcode worker pool: %d helper thread(s)\n", started);
}

static unsigned transcode_all(const uint8_t *src, uint8_t *dst, size_t nblocks,
                              int kind) {
  if (nblocks < TT_PARALLEL_MIN)
    return transcode_range(src, dst, 0, nblocks, kind);
  if (g_pool_state == 0) {
    static volatile int once;
    if (!__atomic_exchange_n(&once, 1, __ATOMIC_ACQ_REL))
      pool_start();
    else
      while (g_pool_state == 0)
        svcSleepThread(100000);
  }
  if (g_pool_state < 0 || !mutexTryLock(&g_pool_lock))
    return transcode_range(src, dst, 0, nblocks, kind);
  TTJob job = {src, dst, nblocks, kind, 0, 0, g_pool_state};
  mutexLock(&g_job_lock);
  g_job = &job;
  g_job_gen++;
  condvarWakeAll(&g_job_cv);
  mutexUnlock(&g_job_lock);
  job_run(&job); /* the calling thread works too */
  mutexLock(&g_job_lock);
  while (job.active > 0)
    condvarWait(&g_done_cv, &g_job_lock);
  g_job = NULL;
  mutexUnlock(&g_job_lock);
  mutexUnlock(&g_pool_lock);
  return job.black;
}
#else
static unsigned transcode_all(const uint8_t *src, uint8_t *dst, size_t nblocks,
                              int kind) {
  return transcode_range(src, dst, 0, nblocks, kind);
}
#endif

/* Transcode nblocks blocks. Returns the number of blocks that decoded to
 * (near) solid black with full opacity -- the "black texture" signature. */
unsigned tt_transcode_blocks(const uint8_t *src, uint8_t *dst, size_t nblocks,
                             int kind) {
  return transcode_all(src, dst, nblocks, kind);
}

/* ------------------------------------------------------------------------ *
 * GL interposition + texture memory accounting (Switch build only)
 * ------------------------------------------------------------------------ */
#if TT_HAVE_GL

#ifndef GL_PIXEL_UNPACK_BUFFER_BINDING
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#endif

/* name -> bytes per (level, face) */
#define TT_LEVELS 16
#define TT_FACES 6
typedef struct {
  GLuint name;   /* 0 = empty, ~0u = tombstone */
  uint32_t *bytes; /* TT_LEVELS*TT_FACES */
  uint32_t total;
} TexEntry;

static Mutex g_tt_lock;
static TexEntry *g_tex;
static size_t g_tex_cap; /* power of two */
static size_t g_tex_used; /* live + tombstones */
static uint64_t g_live_bytes, g_live_count, g_peak_bytes;
static uint64_t g_xc_count, g_xc_in, g_xc_out, g_xc_ticks, g_pass_count,
    g_black_uploads, g_gl_errors, g_unmanaged_bytes;
static int g_xc_disabled, g_black_logs, g_pbo_logged;
/* Per destination format: 0 = not yet probed, 1 = driver accepted it,
 * -1 = rejected (that ETC format then goes to Mesa unchanged). */
static int g_fmt_state[4];
static int fmt_slot(unsigned f) {
  switch (f) {
  case GLE_DXT1_RGB: return 0;
  case GLE_DXT5_RGBA: return 1;
  case GLE_SRGB_DXT1: return 2;
  default: return 3; /* GLE_SRGB_A_DXT5 */
  }
}

static size_t tex_hash(GLuint n) { return (size_t)(n * 2654435761u); }

static TexEntry *tex_find(GLuint name, int create);

static void tex_grow(void) {
  const size_t ncap = g_tex_cap ? g_tex_cap * 2 : 4096;
  TexEntry *old = g_tex;
  const size_t ocap = g_tex_cap;
  TexEntry *fresh = calloc(ncap, sizeof(*fresh));
  if (!fresh)
    return;
  g_tex = fresh;
  g_tex_cap = ncap;
  g_tex_used = 0;
  for (size_t i = 0; i < ocap; i++) {
    if (old[i].name && old[i].name != ~0u) {
      TexEntry *e = tex_find(old[i].name, 1);
      if (e) {
        e->bytes = old[i].bytes;
        e->total = old[i].total;
      }
    }
  }
  free(old);
}

static TexEntry *tex_find(GLuint name, int create) {
  if (!name || name == ~0u)
    return NULL;
  if (create && (g_tex_used + 1) * 4 >= g_tex_cap * 3)
    tex_grow();
  if (!g_tex_cap)
    return NULL;
  const size_t mask = g_tex_cap - 1;
  TexEntry *tomb = NULL;
  for (size_t i = tex_hash(name) & mask, n = 0; n < g_tex_cap;
       i = (i + 1) & mask, n++) {
    TexEntry *e = &g_tex[i];
    if (e->name == name)
      return e;
    if (e->name == ~0u) {
      if (!tomb) tomb = e;
      continue;
    }
    if (e->name == 0) {
      if (!create)
        return NULL;
      TexEntry *slot = tomb ? tomb : e;
      if (!tomb)
        g_tex_used++;
      slot->name = name;
      slot->bytes = NULL;
      slot->total = 0;
      return slot;
    }
  }
  return NULL;
}

static void tex_account(GLenum target, GLint level, uint32_t bytes) {
  GLint bound = 0;
  GLenum query = GL_TEXTURE_BINDING_2D;
  int face = 0;
  if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X &&
      target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) {
    query = GL_TEXTURE_BINDING_CUBE_MAP;
    face = (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
  } else if (target != GL_TEXTURE_2D) {
    mutexLock(&g_tt_lock);
    g_unmanaged_bytes += bytes;
    mutexUnlock(&g_tt_lock);
    return;
  }
  glGetIntegerv(query, &bound);
  if (level < 0 || level >= TT_LEVELS || bound <= 0)
    return;
  mutexLock(&g_tt_lock);
  TexEntry *e = tex_find((GLuint)bound, 1);
  if (e) {
    if (!e->bytes)
      e->bytes = calloc(TT_LEVELS * TT_FACES, sizeof(uint32_t));
    if (e->bytes) {
      uint32_t *slot = &e->bytes[level * TT_FACES + face];
      if (e->total == 0)
        g_live_count++;
      g_live_bytes -= *slot;
      e->total -= *slot;
      *slot = bytes;
      e->total += bytes;
      g_live_bytes += bytes;
      if (g_live_bytes > g_peak_bytes)
        g_peak_bytes = g_live_bytes;
    }
  }
  mutexUnlock(&g_tt_lock);
}

void glDeleteTextures_hook(GLsizei n, const GLuint *names) {
  if (names && n > 0) {
    mutexLock(&g_tt_lock);
    for (GLsizei i = 0; i < n; i++) {
      TexEntry *e = tex_find(names[i], 0);
      if (!e)
        continue;
      if (e->total) {
        g_live_bytes -= e->total;
        g_live_count--;
      }
      free(e->bytes);
      e->bytes = NULL;
      e->total = 0;
      e->name = ~0u; /* tombstone */
    }
    mutexUnlock(&g_tt_lock);
  }
  glDeleteTextures(n, names);
}

static uint32_t teximage_bytes(GLsizei w, GLsizei h, GLenum format, GLenum type) {
  uint32_t bpp = 4;
  if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 ||
      type == GL_UNSIGNED_SHORT_5_5_5_1)
    bpp = 2;
  else if (type == GL_UNSIGNED_BYTE) {
    if (format == GL_ALPHA || format == GL_LUMINANCE)
      bpp = 1;
    else if (format == GL_LUMINANCE_ALPHA)
      bpp = 2;
    else
      bpp = 4; /* RGB8 is padded to RGBX on the GPU */
  } else if (type == GL_FLOAT)
    bpp = 16;
  return (uint32_t)w * (uint32_t)h * bpp;
}

void glTexImage2D_hook(GLenum target, GLint level, GLint internalformat,
                       GLsizei width, GLsizei height, GLint border,
                       GLenum format, GLenum type, const void *pixels) {
  glTexImage2D(target, level, internalformat, width, height, border, format,
               type, pixels);
  if (width > 0 && height > 0)
    tex_account(target, level, teximage_bytes(width, height, format, type));
}

void glCompressedTexImage2D_hook(GLenum target, GLint level,
                                 GLenum internalformat, GLsizei width,
                                 GLsizei height, GLint border,
                                 GLsizei image_size, const void *data) {
  unsigned dst_format;
  int sbb, dbb, kind;
  int plan = !g_xc_disabled && width > 0 && height > 0 &&
             tt_plan(internalformat, &dst_format, &sbb, &dbb, &kind);
  if (plan && g_fmt_state[fmt_slot(dst_format)] < 0)
    plan = 0;
  if (!plan) {
    glCompressedTexImage2D(target, level, internalformat, width, height,
                           border, image_size, data);
    __atomic_fetch_add(&g_pass_count, 1, __ATOMIC_RELAXED);
    /* An ETC format reaching Mesa costs the compressed copy plus RGBA8. */
    uint32_t cost = (uint32_t)image_size;
    if (internalformat == GLE_ETC1_RGB8 ||
        (internalformat >= GLE_R11_EAC && internalformat <= GLE_SRGB8_A8_ETC2_EAC))
      cost += (uint32_t)width * (uint32_t)height * 4u;
    tex_account(target, level, cost);
    return;
  }

  GLint pbo = 0;
  glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
  if (pbo) {
    if (!g_pbo_logged) {
      g_pbo_logged = 1;
      debugPrintf("tex: ETC upload from a pixel-unpack buffer; passed through\n");
    }
    glCompressedTexImage2D(target, level, internalformat, width, height,
                           border, image_size, data);
    tex_account(target, level, (uint32_t)image_size +
                                   (uint32_t)width * (uint32_t)height * 4u);
    return;
  }

  const size_t bw = ((size_t)width + 3) / 4, bh = ((size_t)height + 3) / 4;
  const size_t nblocks = bw * bh;
  const size_t need = nblocks * (size_t)sbb;
  const size_t out_size = nblocks * (size_t)dbb;
  if (data && (size_t)image_size < need) {
    /* malformed: let the driver report the error exactly as before */
    glCompressedTexImage2D(target, level, internalformat, width, height,
                           border, image_size, data);
    return;
  }

  uint8_t *out = NULL;
  unsigned black = 0;
  const uint64_t t0 = armGetSystemTick();
  if (data) {
    out = malloc(out_size);
    if (!out) {
      glCompressedTexImage2D(target, level, internalformat, width, height,
                             border, image_size, data);
      return;
    }
    black = tt_transcode_blocks(data, out, nblocks, kind);
  }
  const uint64_t t1 = armGetSystemTick();

  const int slot = fmt_slot(dst_format);
  const int probing = g_fmt_state[slot] == 0;
  if (probing)
    while (glGetError() != GL_NO_ERROR) {
    } /* clear stale flags so the probe below is ours */
  glCompressedTexImage2D(target, level, dst_format, width, height, border,
                         (GLsizei)out_size, out);
  if (probing) {
    const GLenum err = glGetError();
    if (err == GL_INVALID_ENUM || err == GL_INVALID_VALUE ||
        err == GL_INVALID_OPERATION) {
      g_fmt_state[slot] = -1;
      g_gl_errors++;
      debugPrintf("tex: driver rejected S3TC format 0x%x (GL error 0x%x); "
                  "ETC format 0x%x will be uploaded unchanged\n",
                  dst_format, err, internalformat);
      glCompressedTexImage2D(target, level, internalformat, width, height,
                             border, image_size, data);
      free(out);
      return;
    }
    g_fmt_state[slot] = 1;
    debugPrintf("tex: ETC->S3TC transcoding active for 0x%x -> 0x%x "
                "(first upload %dx%d ok)\n", internalformat, dst_format, width,
                height);
  }
  free(out);

  __atomic_fetch_add(&g_xc_count, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_xc_in, (uint64_t)need, __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_xc_out, (uint64_t)out_size, __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_xc_ticks, t1 - t0, __ATOMIC_RELAXED);
  tex_account(target, level, (uint32_t)out_size);

  if (data && nblocks >= 4 && black == nblocks) {
    __atomic_fetch_add(&g_black_uploads, 1, __ATOMIC_RELAXED);
    if (g_black_logs < 40) {
      g_black_logs++;
      GLint bound = 0;
      glGetIntegerv(target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D
                                            : GL_TEXTURE_BINDING_CUBE_MAP,
                    &bound);
      debugPrintf("tex: all-black upload: texture %d level %d %dx%d fmt 0x%x "
                  "(the game's PVRTC->ETC2 data is already black)\n",
                  bound, level, width, height, internalformat);
    }
  }
}

void tt_stats(TexStats *st) {
  mutexLock(&g_tt_lock);
  st->live_count = g_live_count;
  st->live_bytes = g_live_bytes;
  st->peak_bytes = g_peak_bytes;
  st->unmanaged_bytes = g_unmanaged_bytes;
  mutexUnlock(&g_tt_lock);
  st->transcoded = __atomic_load_n(&g_xc_count, __ATOMIC_RELAXED);
  st->transcoded_in = __atomic_load_n(&g_xc_in, __ATOMIC_RELAXED);
  st->transcoded_out = __atomic_load_n(&g_xc_out, __ATOMIC_RELAXED);
  st->transcode_ms = armTicksToNs(__atomic_load_n(&g_xc_ticks, __ATOMIC_RELAXED)) /
                     1000000ull;
  st->passthrough = __atomic_load_n(&g_pass_count, __ATOMIC_RELAXED);
  st->black_uploads = __atomic_load_n(&g_black_uploads, __ATOMIC_RELAXED);
  st->gl_errors = g_gl_errors;
  st->disabled = g_xc_disabled;
}

#endif /* TT_HAVE_GL */
