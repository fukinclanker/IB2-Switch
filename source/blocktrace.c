#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "blocktrace.h"
#include "so_util.h"
#include "util.h"

#define BT_SLOTS 64
#define BT_INTERVAL_NS 2000000000LL

typedef struct {
  volatile uint32_t tid;          /* 0 = free */
  volatile int blocked;
  const char *volatile kind;
  const void *volatile addr;
  const void *volatile caller;
  volatile long long timeout_ns;  /* -1 = infinite */
  volatile uint64_t since_ns;
  volatile uint32_t calls;        /* total enters */
  uint32_t calls_seen;            /* watchdog-private */
  const char *volatile last_kind;
  const void *volatile last_caller;
} BtSlot;

extern so_module game_mod;

static BtSlot g_slots[BT_SLOTS];
static Thread g_watchdog_thread;
static volatile int g_watchdog_started;

static uint64_t now_ns(void) { return armTicksToNs(armGetSystemTick()); }

static uint32_t self_tid(void) {
  u64 id = 0;
  if (R_FAILED(svcGetThreadId(&id, CUR_THREAD_HANDLE)) || !id)
    return 1;
  return (uint32_t)(id & 0x7fffffff);
}

static BtSlot *slot_for(uint32_t tid) {
  for (int i = 0; i < BT_SLOTS; i++) {
    uint32_t cur = __atomic_load_n(&g_slots[i].tid, __ATOMIC_ACQUIRE);
    if (cur == tid)
      return &g_slots[i];
    if (cur == 0) {
      uint32_t expected = 0;
      if (__atomic_compare_exchange_n(&g_slots[i].tid, &expected, tid, 0,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return &g_slots[i];
      if (expected == tid)
        return &g_slots[i];
    }
  }
  return NULL;
}

void bt_enter(const char *kind, const void *addr, const void *caller,
              long long timeout_ns) {
  BtSlot *s = slot_for(self_tid());
  if (!s)
    return;
  s->kind = kind;
  s->addr = addr;
  s->caller = caller;
  s->timeout_ns = timeout_ns;
  s->since_ns = now_ns();
  s->last_kind = kind;
  s->last_caller = caller;
  __atomic_fetch_add(&s->calls, 1, __ATOMIC_RELAXED);
  __atomic_store_n(&s->blocked, 1, __ATOMIC_RELEASE);
}

void bt_leave(void) {
  BtSlot *s = slot_for(self_tid());
  if (s)
    __atomic_store_n(&s->blocked, 0, __ATOMIC_RELEASE);
}

static void print_caller(const void *caller) {
  const uintptr_t pc = (uintptr_t)caller;
  const uintptr_t base = (uintptr_t)game_mod.load_virtbase;
  if (pc >= base && pc < base + game_mod.load_size)
    debugPrintf(" caller=libib3+0x%lx", (unsigned long)(pc - base));
  else
    debugPrintf(" caller=%p", caller);
}

static void watchdog_main(void *arg) {
  (void)arg;
  unsigned cycle = 0;
  debugPrintf("block: watchdog thread running\n");
  for (;;) {
    svcSleepThread(BT_INTERVAL_NS);
    cycle++;
    if (cycle == 20 || cycle == 40 || cycle == 90) {
      char why[32];
      snprintf(why, sizeof(why), "cycle %u", cycle);
      thread_registry_dump_now(why);
    }
    /* Full detail for the first minute, then every 10th cycle. */
    const int verbose = cycle <= 30 || (cycle % 10) == 0;
    const uint64_t now = now_ns();
    int printed_header = 0;
    for (int i = 0; i < BT_SLOTS; i++) {
      BtSlot *s = &g_slots[i];
      const uint32_t tid = __atomic_load_n(&s->tid, __ATOMIC_ACQUIRE);
      if (!tid)
        continue;
      const uint32_t calls = __atomic_load_n(&s->calls, __ATOMIC_RELAXED);
      const uint32_t delta = calls - s->calls_seen;
      s->calls_seen = calls;
      const int blocked = __atomic_load_n(&s->blocked, __ATOMIC_ACQUIRE);
      const uint64_t age_ms = blocked ? (now - s->since_ns) / 1000000ULL : 0;
      if (!verbose && !(blocked && age_ms >= 4000))
        continue;
      if (!printed_header) {
        debugPrintf("block: --- cycle %u ---\n", cycle);
        printed_header = 1;
      }
      if (blocked && age_ms >= 500) {
        debugPrintf("block: tid=%u BLOCKED %s addr=%p age=%llums timeout=%lldns "
                    "calls+%u",
                    tid, s->kind ? s->kind : "?", s->addr,
                    (unsigned long long)age_ms, s->timeout_ns, delta);
        print_caller(s->caller);
        debugPrintf("\n");
      } else {
        debugPrintf("block: tid=%u running calls+%u last=%s", tid, delta,
                    s->last_kind ? s->last_kind : "-");
        if (s->last_caller)
          print_caller(s->last_caller);
        debugPrintf("\n");
      }
    }
  }
}

void bt_start_watchdog(void) {
  if (__sync_lock_test_and_set(&g_watchdog_started, 1))
    return;
  Result rc = threadCreate(&g_watchdog_thread, watchdog_main, NULL, NULL,
                           0x8000, 0x1c, -2);
  if (R_SUCCEEDED(rc))
    rc = threadStart(&g_watchdog_thread);
  if (R_FAILED(rc))
    debugPrintf("block: watchdog start failed %08x\n", rc);
  else
    debugPrintf("block: watchdog started\n");
}
