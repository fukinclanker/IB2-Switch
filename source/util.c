/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"
#include "so_util.h"

static FILE *s_debug_file;
static int s_debug_fd = -1;

#ifdef DEBUG_LOG

static int s_nxlinkSock = -1;

static void initNxLink(void) {
  if (R_FAILED(socketInitializeDefault()))
    return;
  s_nxlinkSock = nxlinkStdio();
  if (s_nxlinkSock < 0)
    socketExit();
}

static void deinitNxLink(void) {
  if (s_nxlinkSock >= 0) {
    close(s_nxlinkSock);
    socketExit();
    s_nxlinkSock = -1;
  }
}

void userAppInit(void) {
  initNxLink();
}

void userAppExit(void) {
  deinitNxLink();
}

#endif

// the game's `printf` import points here; a no-op with DEBUG_LOG off. The log
// file is kept open for the run (reopening per line on FAT is slow) and flushed
// each line to survive an abrupt exit.
int debugPrintf(char *text, ...) {
#ifdef DEBUG_LOG
  va_list list;
  if (!s_debug_file) {
    s_debug_file = fopen(LOG_NAME, "w"); // fresh log each boot
    if (s_debug_file)
      s_debug_fd = fileno(s_debug_file);
  }
  if (s_debug_file) {
    va_start(list, text);
    vfprintf(s_debug_file, text, list);
    va_end(list);
    fflush(s_debug_file);
  }
  /* Do not mirror through stdout. The graphical console is released before
   * NativeActivity startup; newlib's console stream still retains that stale
   * device and a later vprintf faults after its file-log write succeeds. */
#endif
  return 0;
}

void debugEmergencyWrite(const char *text, size_t length) {
#ifdef DEBUG_LOG
  if (!text || !length)
    return;
  if (s_debug_fd >= 0) {
    (void)write(s_debug_fd, text, length);
    return;
  }
  const int descriptor = open(LOG_NAME, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (descriptor >= 0) {
    (void)write(descriptor, text, length);
    close(descriptor);
  }
#else
  (void)text;
  (void)length;
#endif
}

void install_bionic_tls(void *buffer) {
  if (!buffer)
    return;
  memset(buffer, 0, BIONIC_TLS_SIZE);
  // TP must satisfy the module's TLS alignment (typically <= 64)
  uint8_t *tp = (uint8_t *)(((uintptr_t)buffer + 63) & ~(uintptr_t)63);
  so_tls_init_block(tp + BIONIC_TLS_HDR);
  armSetTlsRw(tp);
}

// boost the CPU to 1785MHz while loading
void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

// Set the GPU clock. Firmware < 8.0 uses pcv, 8.0+ uses clkrst. The previous
// rate is saved so it can be put back on exit.
void gpu_clock(int on) {
#if defined(GPU_CLOCK_HZ)
  static u32 s_prev_hz = 0;
  static int s_applied = 0;
  if (GPU_CLOCK_HZ == 0 || (on && s_applied) || (!on && !s_applied))
    return;
  const u32 target = on ? (u32)GPU_CLOCK_HZ : s_prev_hz;
  Result rc;
  if (hosversionBefore(8, 0, 0)) {
    rc = pcvInitialize();
    if (R_SUCCEEDED(rc)) {
      if (on)
        pcvGetClockRate(PcvModule_GPU, &s_prev_hz);
      rc = pcvSetClockRate(PcvModule_GPU, target ? target : 0);
      pcvExit();
    }
  } else {
    rc = clkrstInitialize();
    if (R_SUCCEEDED(rc)) {
      ClkrstSession session;
      rc = clkrstOpenSession(&session, PcvModuleId_GPU, 3);
      if (R_SUCCEEDED(rc)) {
        if (on)
          clkrstGetClockRate(&session, &s_prev_hz);
        rc = clkrstSetClockRate(&session, target);
        clkrstCloseSession(&session);
      }
      clkrstExit();
    }
  }
  if (R_FAILED(rc))
    debugPrintf("gpu_clock: failed to set %u Hz: %08x\n", target, rc);
  else {
    s_applied = on;
    debugPrintf("gpu_clock: GPU set to %u Hz\n", target);
  }
#else
  (void)on;
#endif
}

// pin the calling thread to a single core. Only pins to cores actually granted
// to this process (cores 0..2 for an application; core 3 is the system core),
// so an out-of-range request just leaves the thread on its default core.
void set_thread_core(int core) {
  static u64 mask = 0;
  if (mask == 0)
    svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
  if (core < 0 || !(mask & (1ull << core)))
    return;
  Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1ull << core);
  if (R_FAILED(rc))
    debugPrintf("affinity: pin to core %d failed: %08x\n", core, rc);
}

/* ---- audio thread scheduling ------------------------------------------- */

static int g_main_core = -1;

static u64 app_core_mask(void) {
  static u64 mask = 0;
  if (mask == 0 && R_FAILED(svcGetInfo(&mask, InfoType_CoreMask,
                                       CUR_PROCESS_HANDLE, 0)))
    mask = 1; /* unknown: behave as if only core 0 exists */
  return mask & 0x7; /* core 3 belongs to the system */
}

void util_record_main_core(void) {
  s32 ideal = -1;
  u64 affinity = 0;
  if (R_SUCCEEDED(svcGetThreadCoreMask(&ideal, &affinity, CUR_THREAD_HANDLE)))
    g_main_core = ideal;
  else
    g_main_core = (int)svcGetCurrentProcessorNumber();
  debugPrintf("sched: main thread core=%d process core mask=0x%llx\n",
              g_main_core, (unsigned long long)app_core_mask());
}

/* The core audio threads live on: the highest application core that is not
 * the default core every libnx pthread (and so every guest thread) starts on. */
int audio_core(void) {
  const u64 mask = app_core_mask();
  for (int core = 2; core >= 0; core--)
    if ((mask & (1ull << core)) && core != g_main_core)
      return core;
  return -1;
}

void audio_thread_boost(const char *who, int priority) {
  s32 old_prio = -1;
  svcGetThreadPriority(&old_prio, CUR_THREAD_HANDLE);

  /* Try the requested priority, then step down toward the 0x2C main-thread
   * level in case this title's NPDM does not allow it. */
  Result prc = 1;
  int applied = -1;
  for (int p = priority; p <= 0x2C; p++) {
    prc = svcSetThreadPriority(CUR_THREAD_HANDLE, (u32)p);
    if (R_SUCCEEDED(prc)) {
      applied = p;
      break;
    }
  }

  const int core = audio_core();
  Result crc = 0;
  if (core >= 0)
    crc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);

  debugPrintf("sched: %s thread prio 0x%x -> %s0x%x (rc=%08x), core %s%d (rc=%08x)\n",
              who, (unsigned)old_prio, applied < 0 ? "UNCHANGED " : "",
              (unsigned)(applied < 0 ? old_prio : applied), prc,
              core < 0 ? "UNCHANGED " : "", core, crc);
}

/* Optional: allow a guest thread on every application core. The ideal core is
 * rotated so CPU-bound interpreter threads start out spread instead of all
 * queuing on the default core. */
void spread_guest_thread(void) {
  static volatile unsigned next;
  const u64 mask = app_core_mask();
  if (!(mask & (mask - 1)))
    return; /* single core available: nothing to spread */
  int ideal = -1;
  for (unsigned tries = 0; tries < 3 && ideal < 0; tries++) {
    const int c = (int)(__atomic_fetch_add(&next, 1u, __ATOMIC_RELAXED) % 3u);
    if (mask & (1ull << c))
      ideal = c;
  }
  if (ideal < 0)
    return;
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, ideal, (u32)mask);
}

// --- thread registry (see util.h) -----------------------------------------
#define MAX_TRACKED_THREADS 64
static Handle g_thread_handles[MAX_TRACKED_THREADS];
static char g_thread_names[MAX_TRACKED_THREADS][32];
static int g_thread_count; // grow-only; index reserved with an atomic add
static Thread g_dump_watchdog_thread;
static volatile int g_dump_watchdog_started;

extern so_module game_mod;

void thread_registry_add(void) {
  int i = __atomic_fetch_add(&g_thread_count, 1, __ATOMIC_RELAXED);
  if (i < MAX_TRACKED_THREADS)
    g_thread_handles[i] = threadGetCurHandle();
}

void thread_registry_set_name(const char *name) {
  const Handle self = threadGetCurHandle();
  int n = g_thread_count;
  if (n > MAX_TRACKED_THREADS)
    n = MAX_TRACKED_THREADS;
  for (int i = 0; i < n; i++) {
    if (g_thread_handles[i] == self) {
      strlcpy(g_thread_names[i], name ? name : "(unnamed)",
              sizeof(g_thread_names[i]));
      return;
    }
  }
}

static void thread_registry_dump_watchdog(void *arg) {
  (void)arg;
  // Startup reaches its first swap after roughly a minute in the emulator.
  // Capture the engine after that point, when the current black-screen stall
  // is active rather than while worker threads are still being created.
  svcSleepThread(75000000000LL);
  const uintptr_t game_base = (uintptr_t)game_mod.load_virtbase;
  const uintptr_t game_end = game_base + game_mod.load_size;
  int n = g_thread_count;
  if (n > MAX_TRACKED_THREADS)
    n = MAX_TRACKED_THREADS;
  debugPrintf("thread dump: begin tracked=%d game=%p..%p\n", n,
              (void *)game_base, (void *)game_end);

  for (int i = 0; i < n; i++) {
    const Handle handle = g_thread_handles[i];
    if (!handle)
      continue;
    Result rc = svcSetThreadActivity(handle, ThreadActivity_Paused);
    if (R_FAILED(rc)) {
      debugPrintf("thread dump[%d] %s: pause failed %08x\n", i,
                  g_thread_names[i][0] ? g_thread_names[i] : "(unnamed)", rc);
      continue;
    }

    ThreadContext context;
    memset(&context, 0, sizeof(context));
    rc = svcGetThreadContext3(&context, handle);
    if (R_SUCCEEDED(rc)) {
      const uintptr_t pc = (uintptr_t)context.pc.x;
      const uintptr_t lr = (uintptr_t)context.lr;
      debugPrintf("thread dump[%d] %s: PC=%p", i,
                  g_thread_names[i][0] ? g_thread_names[i] : "(unnamed)",
                  (void *)pc);
      if (pc >= game_base && pc < game_end)
        debugPrintf(" UE4+0x%lx", pc - game_base);
      debugPrintf(" LR=%p", (void *)lr);
      if (lr >= game_base && lr < game_end)
        debugPrintf(" UE4+0x%lx", lr - game_base);
      debugPrintf(" SP=%p FP=%p\n", (void *)context.sp, (void *)context.fp);

      uintptr_t frame = (uintptr_t)context.fp;
      const uintptr_t stack_start = (uintptr_t)context.sp;
      for (int depth = 0; depth < 8; depth++) {
        if ((frame & 0xf) || frame < stack_start ||
            frame - stack_start > 0x200000)
          break;
        const uintptr_t previous = *(const uintptr_t *)frame;
        const uintptr_t return_address = *(const uintptr_t *)(frame + 8);
        debugPrintf("thread dump[%d] frame[%d]=%p", i, depth,
                    (void *)return_address);
        if (return_address >= game_base && return_address < game_end)
          debugPrintf(" UE4+0x%lx", return_address - game_base);
        debugPrintf("\n");
        if (previous <= frame || previous - frame > 0x100000)
          break;
        frame = previous;
      }
    } else {
      debugPrintf("thread dump[%d] %s: context failed %08x\n", i,
                  g_thread_names[i][0] ? g_thread_names[i] : "(unnamed)", rc);
    }
    svcSetThreadActivity(handle, ThreadActivity_Runnable);
  }
  debugPrintf("thread dump: end\n");
}

/* Snapshot every tracked thread (pc/lr/sp + a short frame-pointer walk) WITHOUT
 * printing while any thread is paused (a paused thread may hold the log file's
 * lock). All printing happens after every thread is running again. */
#define GUEST_IMG_BASE 0x100000000ULL
#define GUEST_IMG_SIZE 0x1300000ULL

typedef struct {
  int used;
  u64 tid;
  Result rc;
  uintptr_t pc, lr, sp, fp;
  uintptr_t frames[10];
  int nframes;
} DumpEntry;
static DumpEntry g_dump[MAX_TRACKED_THREADS];

static void dump_print_addr(uintptr_t a) {
  const uintptr_t lib = (uintptr_t)game_mod.load_virtbase;
  if (a >= lib && a < lib + game_mod.load_size)
    debugPrintf(" libib3+0x%lx", (unsigned long)(a - lib));
  else if (a >= GUEST_IMG_BASE && a < GUEST_IMG_BASE + GUEST_IMG_SIZE)
    debugPrintf(" SwordGame+0x%lx", (unsigned long)(a - GUEST_IMG_BASE));
}

void thread_registry_dump_now(const char *reason) {
  const Handle self = threadGetCurHandle();
  int n = g_thread_count;
  if (n > MAX_TRACKED_THREADS)
    n = MAX_TRACKED_THREADS;
  memset(g_dump, 0, sizeof(g_dump));

  for (int i = 0; i < n; i++) {
    const Handle handle = g_thread_handles[i];
    if (!handle || handle == self)
      continue;
    DumpEntry *e = &g_dump[i];
    e->used = 1;
    svcGetThreadId(&e->tid, handle);
    e->rc = svcSetThreadActivity(handle, ThreadActivity_Paused);
    if (R_FAILED(e->rc))
      continue;
    ThreadContext context;
    memset(&context, 0, sizeof(context));
    e->rc = svcGetThreadContext3(&context, handle);
    if (R_SUCCEEDED(e->rc)) {
      e->pc = (uintptr_t)context.pc.x;
      e->lr = (uintptr_t)context.lr;
      e->sp = (uintptr_t)context.sp;
      e->fp = (uintptr_t)context.fp;
      uintptr_t frame = e->fp;
      for (int d = 0; d < 10; d++) {
        if ((frame & 0xf) || frame < e->sp || frame - e->sp > 0x400000)
          break;
        const uintptr_t previous = *(const uintptr_t *)frame;
        e->frames[e->nframes++] = *(const uintptr_t *)(frame + 8);
        if (previous <= frame || previous - frame > 0x100000)
          break;
        frame = previous;
      }
    }
    svcSetThreadActivity(handle, ThreadActivity_Runnable);
  }

  debugPrintf("thread dump: begin (%s) tracked=%d\n", reason ? reason : "-", n);
  for (int i = 0; i < n; i++) {
    const DumpEntry *e = &g_dump[i];
    if (!e->used)
      continue;
    if (R_FAILED(e->rc)) {
      debugPrintf("thread dump[%d] tid=%llu: failed %08x\n", i,
                  (unsigned long long)e->tid, e->rc);
      continue;
    }
    debugPrintf("thread dump[%d] tid=%llu: PC=%p", i,
                (unsigned long long)e->tid, (void *)e->pc);
    dump_print_addr(e->pc);
    debugPrintf(" LR=%p", (void *)e->lr);
    dump_print_addr(e->lr);
    debugPrintf(" SP=%p\n", (void *)e->sp);
    for (int d = 0; d < e->nframes; d++) {
      debugPrintf("thread dump[%d]   frame[%d]=%p", i, d, (void *)e->frames[d]);
      dump_print_addr(e->frames[d]);
      debugPrintf("\n");
    }
  }
  debugPrintf("thread dump: end\n");
}

void thread_registry_start_dump_watchdog(void) {
  if (__sync_lock_test_and_set(&g_dump_watchdog_started, 1))
    return;
  Result rc = threadCreate(&g_dump_watchdog_thread,
                           thread_registry_dump_watchdog, NULL, NULL, 0x8000,
                           0x2d, 2);
  if (R_SUCCEEDED(rc))
    rc = threadStart(&g_dump_watchdog_thread);
  if (R_FAILED(rc))
    debugPrintf("thread dump: watchdog start failed %08x\n", rc);
}

void thread_registry_pause_others(void) {
  Handle self = threadGetCurHandle();
  int n = g_thread_count;
  if (n > MAX_TRACKED_THREADS)
    n = MAX_TRACKED_THREADS;
  int paused = 0;
  for (int i = 0; i < n; i++) {
    Handle h = g_thread_handles[i];
    if (h && h != self && R_SUCCEEDED(svcSetThreadActivity(h, ThreadActivity_Paused)))
      paused++;
  }
  debugPrintf("EXIT: paused %d/%d engine threads\n", paused, n);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }
