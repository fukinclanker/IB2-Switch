/* memwatch.c -- periodic memory / texture telemetry (1.1.0)
 *
 * The 1.0.9 log ended after ~25 minutes in "std::bad_alloc" with nothing in
 * the log about memory, so there was no way to tell a leak from a slow
 * creep. This thread prints one line every MEMWATCH_PERIOD_S seconds:
 *
 *   mem: heap 1234/2900 MB used (free 1666 MB) | maps 42 (heap 3 MB, guest
 *        610 MB) | tex live 812 = 140 MB (peak 151) | xcoded 3000 ... | black 0
 *
 * and the same line on demand when an allocation fails or the game aborts.
 * Everything here reads counters; nothing allocates on the report path. */

#include <malloc.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <switch.h>

#include "ib3_shim.h"
#include "memwatch.h"
#include "tex_transcode.h"
#include "util.h"

#define MEMWATCH_PERIOD_S 10
#define MB(x) ((unsigned)((x) >> 20))

extern char *fake_heap_start;
extern char *fake_heap_end;

static Thread g_mw_thread;
static int g_mw_started;
static volatile uint64_t g_alloc_failures;
static volatile uint64_t g_alloc_fail_last;
static volatile int g_low_warned;

void memwatch_report(const char *why) {
  const char *top = (const char *)sbrk(0);
  const uint64_t heap_total = (uint64_t)(fake_heap_end - fake_heap_start);
  uint64_t untouched = 0;
  if (top && top != (const char *)-1 && top >= fake_heap_start &&
      top <= fake_heap_end)
    untouched = (uint64_t)(fake_heap_end - top);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  struct mallinfo mi = mallinfo();
#pragma GCC diagnostic pop
  const uint64_t in_use = (uint64_t)mi.uordblks;
  const uint64_t free_total = untouched + (uint64_t)mi.fordblks;

  int maps = 0;
  size_t map_heap = 0, map_guest = 0;
  ib3_mapping_stats(&maps, &map_heap, &map_guest);

  TexStats ts;
  memset(&ts, 0, sizeof(ts));
  tt_stats(&ts);

  debugPrintf("mem%s%s: heap %u/%u MB in use (free %u MB, %u MB never touched) "
              "| maps %d (heap %u MB, guest %u MB) | tex live %llu = %u MB "
              "(peak %u MB, other %u MB) | xcoded %llu (%u->%u MB, %llu ms) "
              "passthru %llu | black %llu | allocfail %llu (last %llu B)%s\n",
              why ? " " : "", why ? why : "", MB(in_use), MB(heap_total),
              MB(free_total), MB(untouched), maps, MB((uint64_t)map_heap),
              MB((uint64_t)map_guest), (unsigned long long)ts.live_count,
              MB(ts.live_bytes), MB(ts.peak_bytes), MB(ts.unmanaged_bytes),
              (unsigned long long)ts.transcoded, MB(ts.transcoded_in),
              MB(ts.transcoded_out), (unsigned long long)ts.transcode_ms,
              (unsigned long long)ts.passthrough,
              (unsigned long long)ts.black_uploads,
              (unsigned long long)g_alloc_failures,
              (unsigned long long)g_alloc_fail_last,
              ts.disabled ? " | ETC->S3TC DISABLED" : "");

  if (free_total < (96ull << 20) && !g_low_warned) {
    g_low_warned = 1;
    debugPrintf("mem: WARNING less than 96 MB of heap left\n");
  }
}

void memwatch_alloc_failed(size_t size, const char *what) {
  static volatile int in_report;
  __atomic_fetch_add(&g_alloc_failures, 1, __ATOMIC_RELAXED);
  g_alloc_fail_last = size;
  /* Logging itself may need memory; never recurse. Report the first few. */
  if (__atomic_exchange_n(&in_report, 1, __ATOMIC_ACQ_REL))
    return;
  if (g_alloc_failures <= 8) {
    debugPrintf("mem: %s(%zu) FAILED\n", what, size);
    memwatch_report("at allocation failure");
  }
  __atomic_store_n(&in_report, 0, __ATOMIC_RELEASE);
}

static void memwatch_main(void *arg) {
  (void)arg;
  for (;;) {
    svcSleepThread((int64_t)MEMWATCH_PERIOD_S * 1000000000LL);
    memwatch_report(NULL);
  }
}

void memwatch_start(void) {
  if (g_mw_started)
    return;
  u64 mask = 0;
  svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
  const int core = (mask & 2) ? 1 : -2;
  if (R_FAILED(threadCreate(&g_mw_thread, memwatch_main, NULL, NULL, 0x10000,
                            0x2C, core)) ||
      R_FAILED(threadStart(&g_mw_thread))) {
    debugPrintf("mem: telemetry thread failed to start\n");
    return;
  }
  g_mw_started = 1;
  memwatch_report("start");
}
