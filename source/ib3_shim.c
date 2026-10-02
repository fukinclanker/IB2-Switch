/* ib3_shim.c -- imports required by libib3.so (Infinity Blade III runtime)
 *
 * libib3.so is not the UE3 game compiled for Android: it is an iOS
 * compatibility runtime that executes the original iOS release of
 * Infinity Blade III from a user-supplied IPA. It therefore needs a few
 * services the Infinity Blade 1 native build never asked for: anonymous and
 * file-backed memory mappings, AAudio output, and the NDK media codec API.
 *
 * NOTE: everything here is written from the import table and public NDK/POSIX
 * semantics. It has not been run on hardware. See docs/PORTING.md.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL.h>
#include <switch.h>

#include "ib3_shim.h"
#include "android_shim.h"
#include "libc_shim.h"
#include "opensles.h"
#include "util.h"
#include "so_util.h"

extern so_module game_mod;

/* ------------------------------------------------------------------------ *
 * Memory mapping
 *
 * Horizon exposes no mmap(). Mappings are carved out of the process heap and
 * tracked so munmap/mremap can find them again. Consequences the runtime must
 * live with:
 *   - MAP_FIXED to an address we did not hand out cannot be honoured.
 *   - PROT_EXEC mappings are code-memory aliases that flip between RW and RX
 *     on demand (see the exec section below); plain mappings stay non-exec.
 *   - Anonymous mappings are zero-filled eagerly, so a huge lazily-committed
 *     reservation costs real memory here.
 * Each of those is logged the first time it happens.
 * ------------------------------------------------------------------------ */

#define MAP_FAILED_PTR ((void *)-1)
#define BIONIC_MAP_FIXED 0x10
#define BIONIC_MAP_ANONYMOUS 0x20
#define BIONIC_MAP_FIXED_NOREPLACE 0x100000
#define MIN_HINT_ADDR 0x100000u
#define BIONIC_PROT_WRITE 0x2
#define BIONIC_PROT_EXEC 0x4
#define BIONIC_MREMAP_MAYMOVE 1
#define MAX_MAPPINGS 8192
#define PAGE 0x1000u
#define ALIGN_UP(x, a) (((x) + ((size_t)(a) - 1)) & ~((size_t)(a) - 1))

typedef struct {
  uintptr_t base;
  size_t size;
  void *backing; // non-NULL: base is a code-memory alias of this heap block
                 // NULL + reservation: pure VA claim (PROT_NONE, no pages yet)
  void *reservation; // VirtmemReservation *, optional
} Mapping;

static Mapping g_maps[MAX_MAPPINGS];
static int g_map_count;
static Mutex g_map_lock;
static int g_warned_fixed, g_hint_logs, g_region_logged;

/* Chunk size for MapProcessCodeMemory. 2 MiB keeps kernel memory-block
 * slab pressure low (1 GiB → 512 blocks instead of 16k at 64 KiB). */
#define MAP_CHUNK 0x200000u
/* On-demand commit window: never materialise more than this at once for a
 * pure VA reservation. Guest address spaces are often 1 GiB PROT_NONE.
 * 8 MiB (was 16) reduces peak heap pressure during sparse commits so the
 * newlib/guest heap has more headroom for texture streaming. */
#define COMMIT_WINDOW 0x800000u /* 8 MiB */

static Mapping *find_mapping(uintptr_t address) {
  /* Prefer the tightest match, and prefer already-committed entries over
   * pure VA reservations that span the same range. */
  Mapping *best = NULL;
  for (int i = 0; i < g_map_count; i++) {
    if (address < g_maps[i].base ||
        address >= g_maps[i].base + g_maps[i].size)
      continue;
    if (!best) {
      best = &g_maps[i];
      continue;
    }
    const int cur_committed = g_maps[i].backing != NULL;
    const int best_committed = best->backing != NULL;
    if (cur_committed && !best_committed) {
      best = &g_maps[i];
    } else if (cur_committed == best_committed &&
               g_maps[i].size < best->size) {
      best = &g_maps[i];
    }
  }
  return best;
}

static void drop_mapping(Mapping *mapping) {
  if (mapping->reservation) {
    virtmemLock();
    virtmemRemoveReservation(mapping->reservation);
    virtmemUnlock();
    mapping->reservation = NULL;
  }
  *mapping = g_maps[--g_map_count];
}

static Handle proc_handle(void) {
  const Handle h = envGetOwnProcessHandle();
  return h == INVALID_HANDLE ? CUR_PROCESS_HANDLE : h;
}

/* The IB3 Mach-O image expects to live at a fixed virtual address, and the
 * runtime then asks for a large fixed reservation around it. Horizon places
 * libnx's own mappings (libib3.so's image, code pools, framebuffers, ...) at
 * random addresses in the same 39-bit space, so every so often one of them used
 * to land inside the window and the guest mapping failed ("address 0x100000000
 * is taken"). Reserve the window with libnx's virtmem before anything else is
 * mapped so virtmemFind*() never hands it out.
 *
 * Returns 0 when the window is free, -1 when something (a kernel-placed region
 * such as the heap) already occupies it; that is a bad random layout that a
 * relaunch fixes. */
#define GUEST_WINDOW_BASE 0x100000000ull
#define GUEST_WINDOW_SIZE 0x80000000ull /* 2 GiB */

static void *g_guest_window;

int ib3_reserve_guest_window(void) {
  virtmemLock();
  g_guest_window = virtmemAddReservation((void *)(uintptr_t)GUEST_WINDOW_BASE,
                                         (size_t)GUEST_WINDOW_SIZE);
  virtmemUnlock();

  uint64_t cursor = GUEST_WINDOW_BASE;
  const uint64_t end = GUEST_WINDOW_BASE + GUEST_WINDOW_SIZE;
  while (cursor < end) {
    MemoryInfo info;
    u32 page_info = 0;
    if (R_FAILED(svcQueryMemory(&info, &page_info, cursor)))
      break;
    if (info.type != MemType_Unmapped) {
      debugPrintf("guest window %llx+%llx is already occupied: block %llx+%llx type=%x\n",
                  (unsigned long long)GUEST_WINDOW_BASE,
                  (unsigned long long)GUEST_WINDOW_SIZE,
                  (unsigned long long)info.addr, (unsigned long long)info.size,
                  info.type);
      return -1;
    }
    const uint64_t next = (uint64_t)info.addr + (uint64_t)info.size;
    if (next <= cursor)
      break;
    cursor = next;
  }
  debugPrintf("guest window %llx+%llx reserved (reservation=%p)\n",
              (unsigned long long)GUEST_WINDOW_BASE,
              (unsigned long long)GUEST_WINDOW_SIZE, g_guest_window);
  return 0;
}

static void log_address_space_once(void) {
  if (g_region_logged)
    return;
  g_region_logged = 1;
  u64 v[8] = {0};
  svcGetInfo(&v[0], InfoType_AslrRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&v[1], InfoType_AslrRegionSize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&v[2], InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&v[3], InfoType_AliasRegionSize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&v[4], InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&v[5], InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&v[6], InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&v[7], InfoType_StackRegionSize, CUR_PROCESS_HANDLE, 0);
  debugPrintf("aspace: aslr %llx+%llx alias %llx+%llx heap %llx+%llx stack %llx+%llx\n",
              (unsigned long long)v[0], (unsigned long long)v[1],
              (unsigned long long)v[2], (unsigned long long)v[3],
              (unsigned long long)v[4], (unsigned long long)v[5],
              (unsigned long long)v[6], (unsigned long long)v[7]);
}

/* True when [want, want+size) is fully free (Unmapped). Logs the first block. */
static int range_is_unmapped(uintptr_t want, size_t size) {
  uintptr_t cursor = want;
  const uintptr_t end = want + size;
  while (cursor < end) {
    MemoryInfo info;
    u32 pageinfo = 0;
    if (R_FAILED(svcQueryMemory(&info, &pageinfo, cursor))) {
      debugPrintf("mmap: QueryMemory(%p) failed\n", (void *)cursor);
      return 0;
    }
    if (info.type != MemType_Unmapped) {
      debugPrintf("mmap: %p not free (type=%x attr=%x perm=%x block=%llx+%llx)\n",
                  (void *)cursor, info.type, info.attr, info.perm,
                  (unsigned long long)info.addr, (unsigned long long)info.size);
      return 0;
    }
    const uintptr_t block_end = (uintptr_t)info.addr + (uintptr_t)info.size;
    if (block_end <= cursor)
      return 0;
    cursor = block_end;
  }
  return 1;
}

/* Map [va, va+size) as RW code-memory aliases of `backing`, MAP_CHUNK at a
 * time. On failure unmaps what was mapped. Returns 0 on success. */
static Result map_chunks_rw(uintptr_t va, void *backing, size_t size) {
  const Handle h = proc_handle();
  size_t done = 0;
  Result rc = 0;
  while (done < size) {
    const size_t len = (size - done) < MAP_CHUNK ? (size - done) : MAP_CHUNK;
    rc = svcMapProcessCodeMemory(h, (u64)(va + done),
                                 (u64)((uint8_t *)backing + done), len);
    if (R_SUCCEEDED(rc)) {
      rc = svcSetProcessMemoryPermission(h, (u64)(va + done), len, Perm_Rw);
      if (R_FAILED(rc))
        svcUnmapProcessCodeMemory(h, (u64)(va + done),
                                  (u64)((uint8_t *)backing + done), len);
    }
    if (R_FAILED(rc))
      break;
    done += len;
  }
  if (R_FAILED(rc)) {
    for (size_t off = 0; off < done; off += MAP_CHUNK) {
      const size_t len = (done - off) < MAP_CHUNK ? (done - off) : MAP_CHUNK;
      svcUnmapProcessCodeMemory(h, (u64)(va + off),
                                (u64)((uint8_t *)backing + off), len);
    }
  }
  return rc;
}

static int commit_range(Mapping *owner, uintptr_t addr, size_t length);

/* Claim [want, want+size) as a pure VA reservation (no physical pages).
 * Used for large PROT_NONE / MAP_FIXED_NOREPLACE so the guest owns the
 * range without spending 1 GiB of heap. Later FIXED sub-maps or mprotect
 * commit real pages. Caller holds g_map_lock. */
static int reserve_at_address(uintptr_t want, size_t size) {
  if (g_map_count >= MAX_MAPPINGS)
    return -1;
  if (!range_is_unmapped(want, size))
    return -1;
  virtmemLock();
  void *res = virtmemAddReservation((void *)want, size);
  virtmemUnlock();
  if (!res) {
    debugPrintf("mmap: virtmemAddReservation(%p, %zu) failed\n", (void *)want, size);
    return -1;
  }
  g_maps[g_map_count].base = want;
  g_maps[g_map_count].size = size;
  g_maps[g_map_count].backing = NULL;
  g_maps[g_map_count].reservation = res;
  g_map_count++;
  debugPrintf("mmap: reserved %zu KiB at %p (VA only, no pages yet)\n",
              size >> 10, (void *)want);
  /* Eagerly commit the first window so the guest's initial writes at the
   * base of the reservation do not take a fault on the hot path. */
  (void)commit_range(&g_maps[g_map_count - 1], want, COMMIT_WINDOW);
  return 0;
}

/* Commit a subrange of a pure VA reservation. Only materialises up to
 * COMMIT_WINDOW bytes aligned around the request — never the whole 1 GiB.
 * Adds a new Mapping entry for the committed window; the parent pure
 * reservation stays so FIXED ownership checks still succeed.
 * Caller holds g_map_lock. Returns 0 on success. */
static int commit_range(Mapping *owner, uintptr_t addr, size_t length) {
  if (!owner)
    return -1;
  /* Already have real pages covering the request? */
  Mapping *existing = find_mapping(addr);
  if (existing && existing->backing &&
      addr >= existing->base &&
      addr + length <= existing->base + existing->size)
    return 0;

  /* Window: align down to MAP_CHUNK, cover the request, cap at COMMIT_WINDOW. */
  uintptr_t win_base = addr & ~(uintptr_t)(MAP_CHUNK - 1);
  if (win_base < owner->base)
    win_base = owner->base;
  size_t win_size = ALIGN_UP(addr + length - win_base, MAP_CHUNK);
  if (win_size < MAP_CHUNK)
    win_size = MAP_CHUNK;
  if (win_size > COMMIT_WINDOW)
    win_size = COMMIT_WINDOW;
  if (win_base + win_size > owner->base + owner->size)
    win_size = owner->base + owner->size - win_base;
  if (win_size == 0)
    return -1;

  /* Skip parts already committed; map only the first unmapped gap in window. */
  MemoryInfo info;
  u32 pageinfo = 0;
  if (R_SUCCEEDED(svcQueryMemory(&info, &pageinfo, win_base)) &&
      info.type != MemType_Unmapped) {
    /* Find next unmapped page inside the window. */
    uintptr_t cursor = (uintptr_t)info.addr + (uintptr_t)info.size;
    if (cursor >= win_base + win_size)
      return 0; /* fully mapped already */
    win_size = win_base + win_size - cursor;
    win_base = cursor;
    win_size = ALIGN_UP(win_size, MAP_CHUNK);
    if (win_size > COMMIT_WINDOW)
      win_size = COMMIT_WINDOW;
    if (win_base + win_size > owner->base + owner->size)
      win_size = owner->base + owner->size - win_base;
  }

  if (g_map_count >= MAX_MAPPINGS)
    return -1;

  /* Retry with a smaller window if the first commit fails (0xd401 = out of
   * memory for the mapping). Common under texture-stream pressure. */
  for (int attempt = 0; attempt < 3; attempt++) {
    size_t try_size = win_size;
    if (attempt == 1)
      try_size = MAP_CHUNK * 2;
    else if (attempt == 2)
      try_size = MAP_CHUNK;
    if (try_size > win_size)
      try_size = win_size;
    if (try_size == 0)
      break;
    if (win_base + try_size > owner->base + owner->size)
      try_size = owner->base + owner->size - win_base;

    void *backing = memalign(PAGE, try_size);
    if (!backing) {
      debugPrintf("mmap: no heap to commit %zu bytes at %p (attempt %d)\n",
                  try_size, (void *)win_base, attempt);
      continue;
    }
    memset(backing, 0, try_size);

    Result rc = map_chunks_rw(win_base, backing, try_size);
    if (R_FAILED(rc)) {
      debugPrintf("mmap: commit MapProcessCodeMemory(%p, %zu) failed: %08x (attempt %d)\n",
                  (void *)win_base, try_size, rc, attempt);
      free(backing);
      continue;
    }
    win_size = try_size;
    g_maps[g_map_count].base = win_base;
    g_maps[g_map_count].size = win_size;
    g_maps[g_map_count].backing = backing;
    g_maps[g_map_count].reservation = NULL;
    g_map_count++;
    debugPrintf("mmap: committed %zu KiB at %p (sparse, parent %p+%zu)\n",
                win_size >> 10, (void *)win_base,
                (void *)owner->base, owner->size);
    return 0;
  }
  return -1;
}

/* Commit whole mapping — only for small pure reservations. Large ones must
 * use commit_range. */
static int commit_mapping(Mapping *m) {
  if (!m)
    return -1;
  if (m->backing)
    return 0;
  if (m->size > COMMIT_WINDOW)
    return commit_range(m, m->base, COMMIT_WINDOW);
  return commit_range(m, m->base, m->size);
}

/* Give the guest real, zeroed RW memory at exactly `want`. Same mechanism as
 * so_finalize(): allocate heap backing, alias it at `want` as code memory,
 * then make the alias RW. The backing pages become the alias (no second copy)
 * and are inaccessible through their old address until unmapped.
 *
 * Large requests are mapped in MAP_CHUNK pieces. If the kernel rejects the
 * destination (e.g. Alias-region state → 0xD401), returns -1 so the caller
 * can fall back to a pure VA reservation for PROT_NONE.
 * Caller holds g_map_lock. Returns 0 on success. */
static int map_at_address(uintptr_t want, size_t size) {
  if (g_map_count >= MAX_MAPPINGS)
    return -1;

  /* Huge FIXED maps (guest address-space reservations) must not try to
   * materialise 1 GiB of code-memory blocks — that exhausts the kernel slab
   * (0xCE01). Force the pure-VA path instead. */
  if (size > COMMIT_WINDOW) {
    debugPrintf("mmap: MapProcessCodeMemory(%p, %zu) skipped (>%u MiB; use VA reserve)\n",
                (void *)want, size, (unsigned)(COMMIT_WINDOW >> 20));
    return -1;
  }

  if (!range_is_unmapped(want, size))
    return -1;

  void *backing = memalign(PAGE, size);
  if (!backing) {
    debugPrintf("mmap: no heap for %zu bytes to back %p\n", size, (void *)want);
    return -1;
  }
  memset(backing, 0, size);

  Result rc = map_chunks_rw(want, backing, size);
  if (R_FAILED(rc)) {
    debugPrintf("mmap: MapProcessCodeMemory(%p, %zu) failed: %08x\n",
                (void *)want, size, rc);
    free(backing);
    return -1;
  }

  virtmemLock();
  void *res = virtmemAddReservation((void *)want, size);
  virtmemUnlock();

  g_maps[g_map_count].base = want;
  g_maps[g_map_count].size = size;
  g_maps[g_map_count].backing = backing;
  g_maps[g_map_count].reservation = res;
  g_map_count++;
  debugPrintf("mmap: mapped %zu KiB at requested address %p (%zu chunks)\n",
              size >> 10, (void *)want,
              (size + MAP_CHUNK - 1) / MAP_CHUNK);
  return 0;
}

static void fill_from_file(void *memory, size_t length, int fd, int64_t offset) {
  size_t done = 0;
  while (done < length) {
    const ssize_t got =
        pread_fake(fd, (uint8_t *)memory + done, length - done,
                   (off_t)offset + (off_t)done);
    if (got <= 0)
      break;
    done += (size_t)got;
  }
}


/* ------------------------------------------------------------------------ *
 * Executable mappings
 *
 * Heap memory can never be executed on Horizon. Any mmap() that asks for
 * PROT_EXEC is therefore backed by heap pages aliased as code memory at a
 * fixed virtual address, in EXEC_CHUNK-sized pieces that are mapped
 * independently. Code memory cannot go RW -> RX in place, so flipping one
 * chunk is unmap + map + set-permission over the same backing pages
 * (contents are kept). A chunk is never W and X at once.
 *
 * Flips happen when the guest calls mprotect() and, lazily, from the fault
 * handler in exception_dump.c: an instruction fetch from an RW chunk flips
 * it to RX, a store into an RX chunk flips it to RW. Only the chunk that
 * was touched changes, so code being executed elsewhere in the cache stays
 * executable while new code is written.
 *
 * A store fault cannot be retried in place (see exception_dump.c), so the
 * faulting store is re-executed from a tiny per-site stub placed within
 * branch range of libib3.so; ib3_store_stub() builds those stubs.
 * ------------------------------------------------------------------------ */

#define MAX_EXEC 256
#define EXEC_CHUNK 0x10000u
#define STUB_SLOT 16u

typedef struct {
  uintptr_t base;
  size_t size;
  void *backing;
  void *reservation; // VirtmemReservation *
  uint8_t *rx;       // per chunk: bit0 = RX (else RW), bit1 = guest asked for EXEC
  size_t nchunks;
  int promoted;      // 1: pre-existing address-pinned mapping registered later
} ExecMap;

#define CHUNK_RX 1u
#define CHUNK_EXEC_INTENT 2u

static ExecMap g_exec[MAX_EXEC];
static volatile int g_exec_count;
static volatile int g_exec_lock;
static volatile unsigned g_exec_flips;
static int g_exec_logs, g_exec_plain_logs;

/* store-fault stubs */
static uintptr_t g_stub_base;
static size_t g_stub_size;
static uint64_t *g_stub_pc;
static size_t g_stub_slots, g_stub_count;

static void exec_lock(void) {
  while (__atomic_exchange_n(&g_exec_lock, 1, __ATOMIC_ACQUIRE))
    ;
}
static void exec_unlock(void) { __atomic_store_n(&g_exec_lock, 0, __ATOMIC_RELEASE); }

/* caller holds g_exec_lock */
static ExecMap *exec_find(uintptr_t address) {
  for (int i = 0; i < g_exec_count; i++)
    if (address >= g_exec[i].base && address < g_exec[i].base + g_exec[i].size)
      return &g_exec[i];
  return NULL;
}

static size_t chunk_len(const ExecMap *e, size_t c) {
  const size_t rest = e->size - c * EXEC_CHUNK;
  return rest < EXEC_CHUNK ? rest : EXEC_CHUNK;
}

/* caller holds g_exec_lock. Returns 0 on success. */
static int exec_flip_chunk(ExecMap *e, size_t c, int want_rx) {
  if ((e->rx[c] & CHUNK_RX) == (unsigned)want_rx)
    return 0;
  const size_t off = c * EXEC_CHUNK;
  const size_t len = chunk_len(e, c);
  void *dst = (void *)(e->base + off);
  void *src = (uint8_t *)e->backing + off;
  if (!(e->rx[c] & CHUNK_RX))
    armDCacheFlush(dst, len); // push guest writes out before X
  const Handle h = proc_handle();
  if (R_FAILED(svcUnmapProcessCodeMemory(h, (u64)dst, (u64)src, len)))
    return -1;
  if (R_FAILED(svcMapProcessCodeMemory(h, (u64)dst, (u64)src, len)))
    return -2; // chunk is now unmapped; nothing sane left to do
  if (R_FAILED(svcSetProcessMemoryPermission(h, (u64)dst, len,
                                            want_rx ? Perm_Rx : Perm_Rw)))
    return -3;
  if (want_rx)
    armICacheInvalidate(dst, len);
  e->rx[c] = (uint8_t)((e->rx[c] & ~CHUNK_RX) | (unsigned)want_rx);
  return 0;
}

/* Map [va, va+size) chunk by chunk as RW code-memory aliases of `backing`.
 * On failure everything mapped so far is undone. Returns 0 on success. */
static Result exec_map_chunks(uintptr_t va, void *backing, size_t size) {
  const Handle h = proc_handle();
  size_t done = 0;
  Result rc = 0;
  while (done < size) {
    const size_t len = (size - done) < EXEC_CHUNK ? (size - done) : EXEC_CHUNK;
    rc = svcMapProcessCodeMemory(h, (u64)(va + done), (u64)((uint8_t *)backing + done), len);
    if (R_SUCCEEDED(rc)) {
      rc = svcSetProcessMemoryPermission(h, (u64)(va + done), len, Perm_Rw);
      if (R_FAILED(rc))
        svcUnmapProcessCodeMemory(h, (u64)(va + done), (u64)((uint8_t *)backing + done), len);
    }
    if (R_FAILED(rc))
      break;
    done += len;
  }
  if (R_FAILED(rc)) {
    for (size_t off = 0; off < done; off += EXEC_CHUNK) {
      const size_t len = (done - off) < EXEC_CHUNK ? (done - off) : EXEC_CHUNK;
      svcUnmapProcessCodeMemory(h, (u64)(va + off), (u64)((uint8_t *)backing + off), len);
    }
  }
  return rc;
}

static char *fmt_text(char *o, const char *t) {
  while (*t)
    *o++ = *t++;
  return o;
}
static char *fmt_hex(char *o, uint64_t v) {
  static const char hex[] = "0123456789abcdef";
  for (int shift = 60; shift >= 0; shift -= 4)
    *o++ = hex[(v >> shift) & 0xf];
  return o;
}

/* Return codes for the user exception handler:
 *   0 — not handled
 *   1 — exec-chunk flip (store → re-exec via stub at pc+4; fetch → resume at pc)
 *   2 — sparse VA commit (must RETRY the same instruction at the same pc) */
int ib3_handle_exec_fault(int is_instruction_fetch, uint64_t address) {
  /* Data abort into a pure VA reservation: sparse-commit a window and retry
   * the SAME instruction (pages were unmapped; the store never completed). */
  if (!is_instruction_fetch) {
    mutexLock(&g_map_lock);
    Mapping *owner = NULL;
    for (int i = 0; i < g_map_count; i++) {
      if (!g_maps[i].backing && address >= g_maps[i].base &&
          address < g_maps[i].base + g_maps[i].size) {
        owner = &g_maps[i];
        break;
      }
    }
    if (owner) {
      Mapping *cur = find_mapping((uintptr_t)address);
      if (cur && cur->backing) {
        mutexUnlock(&g_map_lock);
        return 2; /* already mapped; retry same pc */
      }
      const int ok = commit_range(owner, (uintptr_t)address, PAGE) == 0;
      mutexUnlock(&g_map_lock);
      if (ok) {
        static const char prefix[] = "mmap: fault-commit ok (retry-same-pc)\n";
        debugEmergencyWrite(prefix, sizeof(prefix) - 1);
        return 2; /* MUST retry at the same pc, not pc+4 */
      }
      return 0;
    }
    mutexUnlock(&g_map_lock);
  }

  exec_lock();
  ExecMap *e = exec_find((uintptr_t)address);
  if (!e) {
    exec_unlock();
    return 0;
  }
  const size_t c = ((uintptr_t)address - e->base) / EXEC_CHUNK;
  const int want_rx = is_instruction_fetch ? 1 : 0;
  if ((e->rx[c] & CHUNK_RX) == (unsigned)want_rx) {
    /* Already in the wanted state: another thread flipped it after this fault
     * was raised, so one retry is fine. The same address twice in a row means
     * this is a different kind of fault. */
    static uint64_t last_stale;
    const int repeat = last_stale == address;
    last_stale = address;
    exec_unlock();
    return !repeat;
  }
  const int rc = exec_flip_chunk(e, c, want_rx);
  if (rc == 0 && want_rx && (e->rx[c] & CHUNK_EXEC_INTENT)) {
    /* Guest code: a fetch fault can only be resumed when a register holds the
     * target address, which is not the case when straight-line code runs into
     * the next chunk. So make the whole neighbouring run of chunks the guest
     * marked executable RX in one go. */
    for (size_t k = c + 1; k < e->nchunks && (e->rx[k] & CHUNK_EXEC_INTENT); k++)
      if (exec_flip_chunk(e, k, 1) != 0)
        break;
    for (size_t k = c; k > 0 && (e->rx[k - 1] & CHUNK_EXEC_INTENT); k--)
      if (exec_flip_chunk(e, k - 1, 1) != 0)
        break;
  }
  const unsigned n = __atomic_add_fetch(&g_exec_flips, 1, __ATOMIC_RELAXED);
  const uintptr_t base = e->base;
  exec_unlock();
  if (n <= 64 || (n & (n - 1)) == 0 || rc != 0) {
    char line[192];
    char *o = fmt_text(line, "exec-flip #");
    o = fmt_hex(o, n);
    o = fmt_text(o, is_instruction_fetch ? " fetch->RX at 0x" : " store->RW at 0x");
    o = fmt_hex(o, address);
    o = fmt_text(o, " base=0x");
    o = fmt_hex(o, base);
    o = fmt_text(o, rc ? " FAILED\n" : " ok\n");
    debugEmergencyWrite(line, (size_t)(o - line));
  }
  return rc == 0;
}

/* Reserve the stub area next to libib3.so (mapped lazily, before the first
 * store fault, from normal context). Safe to call more than once. */
static void ib3_stub_init(void) {
  if (g_stub_base || !game_mod.stub_virtbase)
    return;
  const size_t size = game_mod.stub_size;
  void *backing = memalign(PAGE, size);
  uint64_t *slots = calloc(size / STUB_SLOT, sizeof(uint64_t));
  if (!backing || !slots) {
    free(backing);
    free(slots);
    return;
  }
  memset(backing, 0, size);
  const uintptr_t va = (uintptr_t)game_mod.stub_virtbase;
  const Result rc = exec_map_chunks(va, backing, size);
  if (R_FAILED(rc)) {
    debugPrintf("stub: cannot map %zu bytes at %p: %08x\n", size, (void *)va, rc);
    free(backing);
    free(slots);
    return;
  }
  ExecMap *e;
  exec_lock();
  if (g_exec_count >= MAX_EXEC) {
    exec_unlock();
    return;
  }
  e = &g_exec[g_exec_count];
  e->base = va;
  e->size = size;
  e->backing = backing;
  e->reservation = NULL; // reserved together with libib3.so
  e->promoted = 0;
  e->nchunks = (size + EXEC_CHUNK - 1) / EXEC_CHUNK;
  e->rx = calloc(e->nchunks, 1);
  if (!e->rx) {
    exec_unlock();
    return;
  }
  g_exec_count++;
  for (size_t c = 0; c < e->nchunks; c++)
    exec_flip_chunk(e, c, 1);
  g_stub_pc = slots;
  g_stub_slots = size / STUB_SLOT;
  g_stub_base = va;
  g_stub_size = size;
  exec_unlock();
  debugPrintf("stub: %zu KiB at %p (within branch range of libib3.so)\n", size >> 10, (void *)va);
}

/* Returns the address of a stub that re-executes the store at `pc` and then
 * continues at pc+4, or 0. Stub layout (16 bytes):
 *   ldur x17, [sp, #-24]   restores x17, which the resume code clobbered
 *   <the store instruction, copied>
 *   b     pc+4
 *   nop
 * Runs in the exception handler: no libc, no allocation. */
uint64_t ib3_store_stub(uint64_t pc) {
  if (!g_stub_base)
    return 0;
  const uintptr_t lo = (uintptr_t)game_mod.load_virtbase;
  if (pc < lo || pc >= lo + game_mod.load_size || (pc & 3))
    return 0;

  exec_lock();
  ExecMap *e = exec_find(g_stub_base);
  if (!e) {
    exec_unlock();
    return 0;
  }
  for (size_t i = 0; i < g_stub_count; i++) {
    if (g_stub_pc[i] == pc) {
      exec_unlock();
      return g_stub_base + i * STUB_SLOT;
    }
  }
  if (g_stub_count >= g_stub_slots) {
    exec_unlock();
    return 0;
  }
  const size_t idx = g_stub_count;
  const size_t off = idx * STUB_SLOT;
  const size_t c = off / EXEC_CHUNK;
  const uintptr_t slot = g_stub_base + off;

  const int64_t delta = (int64_t)(pc + 4) - (int64_t)(slot + 8);
  if ((delta & 3) || delta < -(1 << 27) || delta >= (1 << 27)) {
    exec_unlock();
    return 0;
  }
  const uint32_t instr = *(const volatile uint32_t *)pc;

  if (exec_flip_chunk(e, c, 0) != 0) {
    exec_unlock();
    return 0;
  }
  volatile uint32_t *w = (volatile uint32_t *)slot;
  w[0] = 0xF85E83F1u; // ldur x17, [sp, #-24]
  w[1] = instr;
  w[2] = 0x14000000u | (((uint32_t)(delta >> 2)) & 0x03FFFFFFu); // b pc+4
  w[3] = 0xD503201Fu; // nop
  int rc = exec_flip_chunk(e, c, 1);
  if (rc == 0) {
    g_stub_pc[idx] = pc;
    g_stub_count++;
  }
  exec_unlock();
  if (rc != 0)
    return 0;

  char line[160];
  char *o = fmt_text(line, "store-stub: pc=0x");
  o = fmt_hex(o, pc);
  o = fmt_text(o, " instr=0x");
  o = fmt_hex(o, instr);
  o = fmt_text(o, " slot=0x");
  o = fmt_hex(o, slot);
  o = fmt_text(o, "\n");
  debugEmergencyWrite(line, (size_t)(o - line));
  return slot;
}

/* Horizon traps EL0 reads of CNTVCT_EL0 (only CNTPCT_EL0 is allowed). libib3
 * emits `mrs xN, cntvct_el0` into its code cache for mach_absolute_time().
 * Rewrite such an instruction in place to read CNTPCT_EL0 instead, so the trap
 * happens once per call site rather than on every call. Runs in the exception
 * handler. Returns 1 if the instruction at pc was patched (retry the same pc),
 * 0 if it is not a patchable instruction in one of our executable pools. */
int ib3_patch_counter_read(uint64_t pc) {
  if (pc & 3)
    return 0;
  exec_lock();
  ExecMap *e = exec_find((uintptr_t)pc);
  if (!e) {
    exec_unlock();
    return 0;
  }
  volatile uint32_t *word = (volatile uint32_t *)(uintptr_t)pc;
  const uint32_t instr = *word;
  if ((instr & 0xFFFFFFE0u) != 0xD53BE040u) { /* mrs xN, cntvct_el0 */
    exec_unlock();
    return 0;
  }
  const size_t c = ((uintptr_t)pc - e->base) / EXEC_CHUNK;
  const int was_rx = (e->rx[c] & CHUNK_RX) != 0;
  if (was_rx && exec_flip_chunk(e, c, 0) != 0) {
    exec_unlock();
    return 0;
  }
  *word = (instr & ~0xE0u) | 0x20u; /* op2 2 -> 1: cntvct_el0 -> cntpct_el0 */
  int rc = 0;
  if (was_rx) {
    rc = exec_flip_chunk(e, c, 1); /* flushes D-cache, invalidates I-cache */
  } else {
    armDCacheFlush((void *)(uintptr_t)(pc & ~(uint64_t)63), 64);
  }
  exec_unlock();
  if (rc == 0) {
    static unsigned logged;
    if (logged++ < 8) {
      char line[96];
      char *o = fmt_text(line, "counter: patched cntvct->cntpct at 0x");
      o = fmt_hex(o, pc);
      o = fmt_text(o, "\n");
      debugEmergencyWrite(line, (size_t)(o - line));
    }
  }
  return rc == 0;
}

/* Returns the mapped address, or NULL if the request cannot be served as an
 * executable mapping (caller then falls back to the plain path). */
static void *mmap_exec(void *addr, size_t size, size_t length, int prot,
                       int flags, int fd, int64_t offset, void *caller) {
  const uintptr_t hint = (uintptr_t)addr;
  const int wants_fixed = (flags & (BIONIC_MAP_FIXED | BIONIC_MAP_FIXED_NOREPLACE)) != 0;

  if (g_exec_logs < 32) {
    g_exec_logs++;
    debugPrintf("mmap-exec: addr=%p len=%zu prot=%x flags=%x fd=%d off=%lld caller=libib3+0x%llx\n",
                addr, size, prot, flags, fd, (long long)offset,
                (unsigned long long)((uintptr_t)caller - (uintptr_t)game_mod.load_virtbase));
  }

  ib3_stub_init();

  mutexLock(&g_map_lock);
  if (g_map_count >= MAX_MAPPINGS || g_exec_count >= MAX_EXEC) {
    mutexUnlock(&g_map_lock);
    errno = ENOMEM;
    return MAP_FAILED_PTR;
  }
  if (wants_fixed && find_mapping(hint)) {
    // carving executable pages out of a plain mapping is not possible
    mutexUnlock(&g_map_lock);
    if (g_exec_plain_logs++ < 8)
      debugPrintf("mmap-exec: MAP_FIXED|EXEC at %p inside a plain mapping; cannot make it executable\n", addr);
    return NULL;
  }

  void *backing = memalign(PAGE, size);
  uint8_t *chunk_state = calloc((size + EXEC_CHUNK - 1) / EXEC_CHUNK, 1);
  if (!backing || !chunk_state) {
    mutexUnlock(&g_map_lock);
    debugPrintf("mmap-exec: no heap for %zu bytes\n", size);
    free(backing);
    free(chunk_state);
    errno = ENOMEM;
    return MAP_FAILED_PTR;
  }
  memset(backing, 0, size);

  uintptr_t va = 0;
  void *reservation = NULL;
  Result rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);

  virtmemLock();
  if (hint >= MIN_HINT_ADDR && (hint & (PAGE - 1)) == 0) {
    rc = exec_map_chunks(hint, backing, size);
    if (R_SUCCEEDED(rc))
      va = hint;
  }
  if (!va && !wants_fixed) {
    void *found = virtmemFindCodeMemory(size, PAGE);
    if (found) {
      rc = exec_map_chunks((uintptr_t)found, backing, size);
      if (R_SUCCEEDED(rc))
        va = (uintptr_t)found;
    }
  }
  if (va)
    reservation = virtmemAddReservation((void *)va, size);
  virtmemUnlock();

  if (!va) {
    mutexUnlock(&g_map_lock);
    debugPrintf("mmap-exec: MapProcessCodeMemory failed: %08x\n", rc);
    free(backing);
    free(chunk_state);
    errno = ENOMEM;
    return MAP_FAILED_PTR;
  }

  g_maps[g_map_count].base = va;
  g_maps[g_map_count].size = size;
  g_maps[g_map_count].backing = backing;
  g_maps[g_map_count].reservation = NULL; /* owned by ExecMap */
  g_map_count++;
  mutexUnlock(&g_map_lock);

  if (!(flags & BIONIC_MAP_ANONYMOUS) && fd >= 0)
    fill_from_file((void *)va, length, fd, offset);

  exec_lock();
  ExecMap *e = &g_exec[g_exec_count];
  e->base = va;
  e->size = size;
  e->backing = backing;
  e->reservation = reservation;
  e->rx = chunk_state; // all chunks start RW
  e->promoted = 0;
  e->nchunks = (size + EXEC_CHUNK - 1) / EXEC_CHUNK;
  g_exec_count++;
  exec_unlock();

  debugPrintf("mmap-exec: %zu KiB at %p (%zu chunks of %u KiB, start RW; flip on demand)\n",
              size >> 10, (void *)va, e->nchunks, EXEC_CHUNK >> 10);
  return (void *)va;
}

/* munmap() of an executable mapping: unmap every chunk. Returns 1 if `base`
 * was executable (and *backing / *size are filled in and the mapping is
 * gone), 0 if it was not, -1 if it is executable but could not be unmapped. */
static int exec_release(uintptr_t base, void **backing_out, size_t *size_out) {
  void *reservation = NULL;
  exec_lock();
  int index = -1;
  for (int i = 0; i < g_exec_count; i++)
    if (g_exec[i].base == base)
      index = i;
  if (index < 0) {
    exec_unlock();
    return 0;
  }
  ExecMap *e = &g_exec[index];
  const Handle h = proc_handle();
  for (size_t c = 0; c < e->nchunks; c++) {
    const size_t off = c * EXEC_CHUNK;
    if (R_FAILED(svcUnmapProcessCodeMemory(h, (u64)(e->base + off),
                                           (u64)((uint8_t *)e->backing + off),
                                           chunk_len(e, c)))) {
      exec_unlock();
      return -1;
    }
  }
  *backing_out = e->backing;
  *size_out = e->size;
  reservation = e->reservation;
  free(e->rx);
  *e = g_exec[--g_exec_count];
  exec_unlock();
  if (reservation) {
    virtmemLock();
    virtmemRemoveReservation(reservation);
    virtmemUnlock();
  }
  return 1;
}

void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd,
                int64_t offset) {
  if (length == 0) {
    errno = EINVAL;
    return MAP_FAILED_PTR;
  }
  const size_t size = ALIGN_UP(length, PAGE);

  if (prot & BIONIC_PROT_EXEC) {
    void *exec = mmap_exec(addr, size, length, prot, flags, fd, offset,
                           __builtin_return_address(0));
    if (exec)
      return exec;
    // NULL: not servable as executable; continue as a plain mapping
  }

  mutexLock(&g_map_lock);

  const uintptr_t hint = (uintptr_t)addr;
  const int wants_fixed = (flags & (BIONIC_MAP_FIXED | BIONIC_MAP_FIXED_NOREPLACE)) != 0;

  if (hint >= MIN_HINT_ADDR && (hint & (PAGE - 1)) == 0) {
    if (g_hint_logs < 32) {
      g_hint_logs++;
      log_address_space_once();
      debugPrintf("mmap: request addr=%p len=%zu prot=%x flags=%x fd=%d\n",
                  addr, size, prot, flags, fd);
    }
    Mapping *owner = find_mapping(hint);
    if (wants_fixed && owner &&
        hint + size <= owner->base + owner->size) {
      /* Carve into an existing mapping. Sparse-commit only the subrange
       * being touched, never the whole parent (may be 1 GiB PROT_NONE). */
      if (commit_range(owner, hint, size) != 0) {
        mutexUnlock(&g_map_lock);
        errno = ENOMEM;
        return MAP_FAILED_PTR;
      }
      if ((flags & BIONIC_MAP_ANONYMOUS) && (prot & BIONIC_PROT_WRITE))
        memset(addr, 0, size < length ? size : length);
      mutexUnlock(&g_map_lock);
      return addr;
    }
    if (!owner && map_at_address(hint, size) == 0) {
      mutexUnlock(&g_map_lock);
      if (!(flags & BIONIC_MAP_ANONYMOUS) && fd >= 0)
        fill_from_file(addr, length, fd, offset);
      return addr;
    }
    /* Kernel rejected a real mapping (0xD401 invalid state / 0xCE01 resource
     * exhaustion on huge FIXED). For PROT_NONE-style reservations, claim the
     * VA only; pages are sparse-committed on first use. */
    if (!owner && wants_fixed &&
        !(prot & (BIONIC_PROT_WRITE | BIONIC_PROT_EXEC)) &&
        reserve_at_address(hint, size) == 0) {
      mutexUnlock(&g_map_lock);
      return addr;
    }
    if (wants_fixed) {
      mutexUnlock(&g_map_lock);
      errno = (flags & BIONIC_MAP_FIXED_NOREPLACE) ? EEXIST : ENOMEM;
      return MAP_FAILED_PTR;
    }
    // plain hint that could not be honoured: fall through to any address
  }

  if (wants_fixed) {
    Mapping *owner = find_mapping((uintptr_t)addr);
    if (owner && (uintptr_t)addr + size <= owner->base + owner->size) {
      if (commit_range(owner, (uintptr_t)addr, size) != 0) {
        mutexUnlock(&g_map_lock);
        errno = ENOMEM;
        return MAP_FAILED_PTR;
      }
      if ((flags & BIONIC_MAP_ANONYMOUS) && (prot & BIONIC_PROT_WRITE))
        memset(addr, 0, size < length ? size : length);
      mutexUnlock(&g_map_lock);
      return addr;
    }
    if (!g_warned_fixed) {
      g_warned_fixed = 1;
      debugPrintf("mmap: MAP_FIXED at %p (%zu bytes) is outside memory we own\n",
                  addr, size);
    }
    mutexUnlock(&g_map_lock);
    errno = ENOMEM;
    return MAP_FAILED_PTR;
  }

  if (g_map_count >= MAX_MAPPINGS) {
    mutexUnlock(&g_map_lock);
    errno = ENOMEM;
    return MAP_FAILED_PTR;
  }

  void *memory = memalign(PAGE, size);
  if (!memory) {
    mutexUnlock(&g_map_lock);
    debugPrintf("mmap: out of memory for %zu bytes\n", size);
    errno = ENOMEM;
    return MAP_FAILED_PTR;
  }
  g_maps[g_map_count].base = (uintptr_t)memory;
  g_maps[g_map_count].size = size;
  g_maps[g_map_count].backing = NULL;
  g_maps[g_map_count].reservation = NULL;
  g_map_count++;
  mutexUnlock(&g_map_lock);

  memset(memory, 0, size);
  if (!(flags & BIONIC_MAP_ANONYMOUS) && fd >= 0)
    fill_from_file(memory, length, fd, offset); /* private copy of the window */
  return memory;
}

int munmap_fake(void *addr, size_t length) {
  (void)length;
  mutexLock(&g_map_lock);
  Mapping *mapping = find_mapping((uintptr_t)addr);
  /* Only whole-mapping unmaps release memory; partial unmaps are accepted
   * and leave the tail resident. */
  if (mapping && mapping->base == (uintptr_t)addr &&
      ALIGN_UP(length, PAGE) >= mapping->size) {
    void *base = (void *)mapping->base;
    if (mapping->backing) {
      void *backing = mapping->backing;
      void *exec_backing = NULL;
      size_t exec_size = 0;
      const int exec = exec_release(mapping->base, &exec_backing, &exec_size);
      if (exec < 0) {
        debugPrintf("munmap: could not unmap executable mapping %p; leaving it mapped\n", base);
        mutexUnlock(&g_map_lock);
        return 0;
      }
      if (exec == 0) {
        /* Chunked unmap for large address-pinned plain mappings. */
        Result urc = 0;
        for (size_t off = 0; off < mapping->size; off += MAP_CHUNK) {
          const size_t len =
              (mapping->size - off) < MAP_CHUNK ? (mapping->size - off) : MAP_CHUNK;
          urc = svcUnmapProcessCodeMemory(proc_handle(), (u64)((uintptr_t)base + off),
                                          (u64)((uint8_t *)backing + off), len);
          if (R_FAILED(urc))
            break;
        }
        if (R_FAILED(urc)) {
          debugPrintf("munmap: could not unmap %p; leaving it mapped\n", base);
          mutexUnlock(&g_map_lock);
          return 0;
        }
      }
      drop_mapping(mapping);
      mutexUnlock(&g_map_lock);
      free(backing);
      return 0;
    }
    /* Pure VA reservation (backing == NULL, reservation set): just drop it.
     * Heap-backed mapping (backing == NULL, base is memalign result): free. */
    const int pure_va = mapping->reservation != NULL;
    drop_mapping(mapping);
    mutexUnlock(&g_map_lock);
    if (!pure_va)
      free(base);
    return 0;
  }
  mutexUnlock(&g_map_lock);
  return 0;
}

/* The guest image sits inside the address-pinned RW mapping made for the
 * runtime's reservation (a plain code-memory alias). Those pages can already
 * be flipped one chunk at a time, so the first mprotect(PROT_EXEC) simply
 * registers the whole mapping as chunk-tracked; nothing is remapped. */
static void exec_promote(uintptr_t address) {
  mutexLock(&g_map_lock);
  Mapping *m = find_mapping(address);
  if (!m || (m->base & (EXEC_CHUNK - 1))) {
    mutexUnlock(&g_map_lock);
    return;
  }
  if (!m->backing && commit_mapping(m) != 0) {
    mutexUnlock(&g_map_lock);
    return;
  }
  const uintptr_t base = m->base;
  const size_t size = m->size;
  void *backing = m->backing;
  mutexUnlock(&g_map_lock);

  const size_t n = (size + EXEC_CHUNK - 1) / EXEC_CHUNK;
  uint8_t *state = calloc(n, 1);
  if (!state)
    return;
  exec_lock();
  if (exec_find(address) || g_exec_count >= MAX_EXEC) {
    exec_unlock();
    free(state);
    return;
  }
  ExecMap *e = &g_exec[g_exec_count++];
  e->base = base;
  e->size = size;
  e->backing = backing;
  e->reservation = NULL;
  e->rx = state; // all chunks are RW right now
  e->nchunks = n;
  e->promoted = 1;
  exec_unlock();
  debugPrintf("mprotect: registered pinned mapping %p (%zu KiB, %zu chunks) as executable-capable\n",
              (void *)base, size >> 10, n);
}

int mprotect_fake(void *addr, size_t length, int prot) {
  /* Sparse-commit only the touched window of a pure VA reservation. */
  if (prot & (BIONIC_PROT_WRITE | BIONIC_PROT_EXEC)) {
    mutexLock(&g_map_lock);
    Mapping *m = find_mapping((uintptr_t)addr);
    if (m)
      (void)commit_range(m, (uintptr_t)addr, length ? length : PAGE);
    mutexUnlock(&g_map_lock);
  }

  exec_lock();
  ExecMap *e = exec_find((uintptr_t)addr);
  if (!e && (prot & BIONIC_PROT_EXEC)) {
    exec_unlock();
    exec_promote((uintptr_t)addr);
    exec_lock();
    e = exec_find((uintptr_t)addr);
  }
  if (e) {
    int rc = 0;
    const uintptr_t first = (uintptr_t)addr - e->base;
    const uintptr_t last = first + (length ? length : 1) - 1;
    const size_t c0 = first / EXEC_CHUNK;
    size_t c1 = last / EXEC_CHUNK;
    if (c1 >= e->nchunks)
      c1 = e->nchunks - 1;
    // remember which chunks of a promoted mapping the guest wants as code
    if (e->promoted && (prot & BIONIC_PROT_EXEC))
      for (size_t c = c0; c <= c1; c++)
        e->rx[c] |= CHUNK_EXEC_INTENT;
    // chunk granularity; RWX resolves to RW and flips to RX on fetch
    for (size_t c = c0; c <= c1 && rc == 0; c++) {
      if (prot & BIONIC_PROT_WRITE)
        rc = exec_flip_chunk(e, c, 0);
      else if (prot & BIONIC_PROT_EXEC)
        rc = exec_flip_chunk(e, c, 1);
    }
    exec_unlock();
    if (g_exec_logs < 64) {
      g_exec_logs++;
      debugPrintf("mprotect-exec: %p len=%zu prot=%x -> %s caller=libib3+0x%llx\n", addr,
                  length, prot, rc ? "FAILED" : "ok",
                  (unsigned long long)((uintptr_t)__builtin_return_address(0) -
                                       (uintptr_t)game_mod.load_virtbase));
    }
    return rc ? -1 : 0;
  }
  exec_unlock();
  if ((prot & BIONIC_PROT_EXEC) && g_exec_plain_logs++ < 8)
    debugPrintf("mprotect: EXEC on plain (non-exec) mapping %p len=%zu prot=%x caller=libib3+0x%llx; cannot honour\n",
                addr, length, prot,
                (unsigned long long)((uintptr_t)__builtin_return_address(0) -
                                     (uintptr_t)game_mod.load_virtbase));
  return 0;
}

void *mremap_fake(void *old_addr, size_t old_size, size_t new_size, int flags,
                  ...) {
  (void)flags;
  void *fresh = mmap_fake(NULL, new_size, 3, BIONIC_MAP_ANONYMOUS, -1, 0);
  if (fresh == MAP_FAILED_PTR)
    return fresh;
  memcpy(fresh, old_addr, old_size < new_size ? old_size : new_size);
  munmap_fake(old_addr, old_size);
  return fresh;
}

int madvise_fake(void *addr, size_t length, int advice) {
  (void)addr;
  (void)length;
  (void)advice;
  return 0;
}

/* ------------------------------------------------------------------------ *
 * AAudio over the shared OpenSLES SDL mixer (single device on Horizon)
 *
 * The iOS AUGraph path writes PCM via AAudioStream_write (or, in callback
 * mode, supplies a data callback). Opening a second SDL device raced the
 * OpenSL mixer and caused random underruns, so every stream owns a ring inside
 * opensles.c and all rings are mixed on the one SDL callback thread.
 *
 * The whole public AAudio surface is provided, not just the calls the IB3
 * runtime happened to import: the runtime also resolves AAudio by dlsym, and
 * a guest that finds one optional entry point missing may silently disable
 * its audio backend (-> no sound, no error).
 * ------------------------------------------------------------------------ */

#define AAUDIO_OK 0
/* Values from the NDK's aaudio_result_t. (The earlier shim used -899 for
 * "unavailable"; -899 is AAUDIO_ERROR_DISCONNECTED, which makes callers try to
 * reopen the stream.) */
#define AAUDIO_ERROR_DISCONNECTED (-899)
#define AAUDIO_ERROR_ILLEGAL_ARGUMENT (-898)
#define AAUDIO_ERROR_INVALID_STATE (-895)
#define AAUDIO_ERROR_INVALID_HANDLE (-892)
#define AAUDIO_ERROR_UNIMPLEMENTED (-890)
#define AAUDIO_ERROR_UNAVAILABLE (-889)
#define AAUDIO_ERROR_NO_MEMORY (-887)
#define AAUDIO_ERROR_NULL (-886)
#define AAUDIO_ERROR_TIMEOUT (-885)
#define AAUDIO_FORMAT_PCM_I16 1
#define AAUDIO_FORMAT_PCM_FLOAT 2
#define AAUDIO_DIRECTION_INPUT 1
#define AAUDIO_BURST_FRAMES 512
#define AAUDIO_BUFFER_CAPACITY 4096

enum {
  AAUDIO_STATE_UNINITIALIZED = 0,
  AAUDIO_STATE_UNKNOWN = 1,
  AAUDIO_STATE_OPEN = 2,
  AAUDIO_STATE_STARTING = 3,
  AAUDIO_STATE_STARTED = 4,
  AAUDIO_STATE_PAUSING = 5,
  AAUDIO_STATE_PAUSED = 6,
  AAUDIO_STATE_FLUSHING = 7,
  AAUDIO_STATE_FLUSHED = 8,
  AAUDIO_STATE_STOPPING = 9,
  AAUDIO_STATE_STOPPED = 10,
  AAUDIO_STATE_CLOSING = 11,
  AAUDIO_STATE_CLOSED = 12,
  AAUDIO_STATE_DISCONNECTED = 13,
};

typedef int32_t (*aaudio_data_cb)(void *stream, void *user, void *audio,
                                  int32_t frames);
typedef void (*aaudio_error_cb)(void *stream, void *user, int32_t error);

typedef struct {
  int32_t format;
  int32_t channels;
  int32_t rate;
  int32_t direction;
  int32_t usage;
  int32_t perf_mode;
  int32_t sharing_mode;
  int32_t device_id;
  int32_t frames_per_callback;
  aaudio_data_cb data_cb;
  void *data_user;
  aaudio_error_cb error_cb;
  void *error_user;
} AAudioBuilder;

typedef struct {
  int32_t format;
  int32_t rate;
  int32_t channels;
  int32_t bytes_per_frame;
  int32_t usage;
  int32_t perf_mode;
  int32_t sharing_mode;
  int32_t buffer_size;
  int32_t frames_per_callback;
  int state;
  int started;
  AARing *ring;
  aaudio_data_cb data_cb;
  void *data_user;
  aaudio_error_cb error_cb;
  void *error_user;
  /* data-callback pump thread (only when a data callback was registered) */
  pthread_t pump;
  int pump_running;
  volatile int pump_quit;
  uint64_t write_calls;
  uint8_t tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
} AAudioStream;

static void aaudio_log_stats(AAudioStream *stream, const char *where) {
  uint64_t written = 0;
  int32_t peak_in = 0;
  float float_peak = 0.0f;
  unsigned cbs = 0, mixer_peak = 0;
  opensles_aaudio_stats_ex(stream->ring, &written, &peak_in, &float_peak);
  nx_audio_stats(&cbs, &mixer_peak);
  debugPrintf("AAudio[%s]: written=%llu frames, input peak=%d, float|peak|=%.6f, "
              "queued=%d, mixer callbacks=%u, mixer peak=%u\n",
              where, (unsigned long long)written, (int)peak_in, float_peak,
              opensles_aaudio_queued_frames(stream->ring), cbs, mixer_peak);
}

int AAudio_createStreamBuilder_fake(void **builder) {
  if (!builder)
    return AAUDIO_ERROR_NULL;
  AAudioBuilder *state = calloc(1, sizeof(*state));
  if (!state)
    return AAUDIO_ERROR_NO_MEMORY;
  /* Match the Android host mixer (src/audio/mixer.cpp): it always opens
   * PCM_FLOAT stereo @ 48 kHz. Defaulting here means a guest that forgets
   * setFormat still gets the format the runtime was built against. */
  state->format = AAUDIO_FORMAT_PCM_FLOAT;
  state->channels = 2;
  state->rate = 48000;
  state->frames_per_callback = AAUDIO_BURST_FRAMES;
  *builder = state;
  return AAUDIO_OK;
}

#define AAUDIO_BUILDER_SETTER(name, field)                                     \
  void AAudioStreamBuilder_##name##_fake(void *builder, int32_t value) {       \
    if (builder)                                                               \
      ((AAudioBuilder *)builder)->field = value;                               \
  }
AAUDIO_BUILDER_SETTER(setFormat, format)
AAUDIO_BUILDER_SETTER(setChannelCount, channels)
AAUDIO_BUILDER_SETTER(setSamplesPerFrame, channels)
AAUDIO_BUILDER_SETTER(setSampleRate, rate)
AAUDIO_BUILDER_SETTER(setPerformanceMode, perf_mode)
AAUDIO_BUILDER_SETTER(setUsage, usage)
AAUDIO_BUILDER_SETTER(setDirection, direction)
AAUDIO_BUILDER_SETTER(setSharingMode, sharing_mode)
AAUDIO_BUILDER_SETTER(setDeviceId, device_id)
AAUDIO_BUILDER_SETTER(setFramesPerDataCallback, frames_per_callback)
#undef AAUDIO_BUILDER_SETTER

#define AAUDIO_BUILDER_IGNORED(name)                                           \
  void AAudioStreamBuilder_##name##_fake(void *builder, int32_t value) {       \
    (void)builder;                                                             \
    (void)value;                                                               \
  }
AAUDIO_BUILDER_IGNORED(setBufferCapacityInFrames)
AAUDIO_BUILDER_IGNORED(setContentType)
AAUDIO_BUILDER_IGNORED(setSessionId)
AAUDIO_BUILDER_IGNORED(setInputPreset)
AAUDIO_BUILDER_IGNORED(setAllowedCapturePolicy)
AAUDIO_BUILDER_IGNORED(setPrivacySensitive)
AAUDIO_BUILDER_IGNORED(setIsContentSpatialized)
AAUDIO_BUILDER_IGNORED(setSpatializationBehavior)
#undef AAUDIO_BUILDER_IGNORED

void AAudioStreamBuilder_setDataCallback_fake(void *builder, void *callback,
                                              void *user) {
  AAudioBuilder *state = builder;
  if (!state)
    return;
  state->data_cb = (aaudio_data_cb)callback;
  state->data_user = user;
}

void AAudioStreamBuilder_setErrorCallback_fake(void *builder, void *callback,
                                               void *user) {
  AAudioBuilder *state = builder;
  if (!state)
    return;
  state->error_cb = (aaudio_error_cb)callback;
  state->error_user = user;
}

int AAudioStreamBuilder_delete_fake(void *builder) {
  free(builder);
  return AAUDIO_OK;
}

int AAudioStreamBuilder_openStream_fake(void *builder, void **out) {
  const AAudioBuilder *state = builder;
  if (!state || !out)
    return AAUDIO_ERROR_NULL;
  if (state->direction == AAUDIO_DIRECTION_INPUT) {
    debugPrintf("AAudio: input (capture) stream requested; unavailable\n");
    return AAUDIO_ERROR_UNAVAILABLE;
  }
  if (!opensles_ensure_shared_device()) {
    debugPrintf("AAudio: shared OpenSLES device unavailable\n");
    return AAUDIO_ERROR_UNAVAILABLE;
  }
  AAudioStream *stream = calloc(1, sizeof(*stream));
  if (!stream)
    return AAUDIO_ERROR_NO_MEMORY;
  stream->ring = opensles_aaudio_open();
  if (!stream->ring) {
    free(stream);
    return AAUDIO_ERROR_UNAVAILABLE;
  }
  stream->format =
      state->format == AAUDIO_FORMAT_PCM_FLOAT ? AAUDIO_FORMAT_PCM_FLOAT
                                               : AAUDIO_FORMAT_PCM_I16;
  /* The device runs at a fixed rate; report it so the guest renders at it. */
  stream->rate = opensles_device_rate();
  stream->channels = state->channels > 0 ? state->channels : 2;
  stream->bytes_per_frame =
      (stream->format == AAUDIO_FORMAT_PCM_FLOAT ? 4 : 2) * stream->channels;
  stream->usage = state->usage;
  stream->perf_mode = state->perf_mode;
  stream->sharing_mode = state->sharing_mode;
  stream->buffer_size = AAUDIO_BURST_FRAMES * 2;
  stream->frames_per_callback = state->frames_per_callback > 0 &&
                                        state->frames_per_callback <= 4096
                                    ? state->frames_per_callback
                                    : AAUDIO_BURST_FRAMES;
  stream->data_cb = state->data_cb;
  stream->data_user = state->data_user;
  stream->error_cb = state->error_cb;
  stream->error_user = state->error_user;
  stream->state = AAUDIO_STATE_OPEN;
  debugPrintf("AAudio: opened stream fmt=%s ch=%d (asked %d Hz, mixer runs at "
              "%d Hz) %d B/frame mode=%s\n",
              stream->format == AAUDIO_FORMAT_PCM_FLOAT ? "float" : "s16",
              stream->channels, (int)state->rate, stream->rate,
              stream->bytes_per_frame,
              stream->data_cb ? "data-callback" : "blocking-write");
  if (state->rate > 0 && state->rate != stream->rate)
    debugPrintf("AAudio: NOTE requested %d Hz but stream reports %d Hz; the "
                "guest must render at the reported rate\n",
                (int)state->rate, stream->rate);
  *out = stream;
  return AAUDIO_OK;
}

/* Data-callback mode: the guest supplies a callback that fills a buffer. Run it
 * on a pump thread with bionic TLS (it executes guest code), keeping the ring
 * topped up. */
static void *aaudio_pump(void *opaque) {
  AAudioStream *stream = opaque;
  thread_registry_add();
  install_bionic_tls(stream->tls);
  const int frames = stream->frames_per_callback;
  const size_t bytes = (size_t)frames * (size_t)stream->bytes_per_frame;
  void *buffer = calloc(1, bytes);
  while (buffer && !__atomic_load_n(&stream->pump_quit, __ATOMIC_ACQUIRE)) {
    if (__atomic_load_n(&stream->state, __ATOMIC_ACQUIRE) !=
            AAUDIO_STATE_STARTED ||
        opensles_aaudio_free_frames(stream->ring) < frames) {
      svcSleepThread(2000000LL);
      continue;
    }
    memset(buffer, 0, bytes);
    const int32_t result = stream->data_cb(stream, stream->data_user, buffer,
                                           frames);
    opensles_aaudio_write_pcm(stream->ring, buffer, frames, stream->channels,
                              stream->format == AAUDIO_FORMAT_PCM_FLOAT);
    if (((++stream->write_calls) & 0x1ff) == 1)
      aaudio_log_stats(stream, "callback");
    if (result != 0) /* AAUDIO_CALLBACK_RESULT_STOP */
      __atomic_store_n(&stream->state, AAUDIO_STATE_STOPPED, __ATOMIC_RELEASE);
  }
  free(buffer);
  return NULL;
}

int AAudioStream_requestStart_fake(void *stream_ptr) {
  AAudioStream *stream = stream_ptr;
  if (!stream)
    return AAUDIO_ERROR_NULL;
  opensles_aaudio_set_active(stream->ring, 1, 0);
  stream->started = 1;
  __atomic_store_n(&stream->state, AAUDIO_STATE_STARTED, __ATOMIC_RELEASE);
  if (stream->data_cb && !stream->pump_running) {
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    const int attr_ready = rc == 0;
    if (!rc)
      rc = pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    if (!rc)
      rc = pthread_create(&stream->pump, &attr, aaudio_pump, stream);
    if (!rc)
      stream->pump_running = 1;
    else
      debugPrintf("AAudio: could not start data-callback thread (%d)\n", rc);
    if (attr_ready)
      pthread_attr_destroy(&attr);
  }
  return AAUDIO_OK;
}

int AAudioStream_requestPause_fake(void *stream_ptr) {
  AAudioStream *stream = stream_ptr;
  if (!stream)
    return AAUDIO_ERROR_NULL;
  opensles_aaudio_set_active(stream->ring, 0, 0);
  stream->started = 0;
  __atomic_store_n(&stream->state, AAUDIO_STATE_PAUSED, __ATOMIC_RELEASE);
  return AAUDIO_OK;
}

int AAudioStream_requestStop_fake(void *stream_ptr) {
  AAudioStream *stream = stream_ptr;
  if (!stream)
    return AAUDIO_ERROR_NULL;
  opensles_aaudio_set_active(stream->ring, 0, 1);
  stream->started = 0;
  __atomic_store_n(&stream->state, AAUDIO_STATE_STOPPED, __ATOMIC_RELEASE);
  return AAUDIO_OK;
}

int AAudioStream_requestFlush_fake(void *stream_ptr) {
  AAudioStream *stream = stream_ptr;
  if (!stream)
    return AAUDIO_ERROR_NULL;
  opensles_aaudio_set_active(stream->ring, stream->started, 1);
  if (!stream->started)
    __atomic_store_n(&stream->state, AAUDIO_STATE_FLUSHED, __ATOMIC_RELEASE);
  return AAUDIO_OK;
}

int AAudioStream_waitForStateChange_fake(void *stream_ptr, int32_t input_state,
                                         int32_t *next_state,
                                         int64_t timeout_ns) {
  (void)input_state;
  (void)timeout_ns;
  AAudioStream *stream = stream_ptr;
  if (!stream)
    return AAUDIO_ERROR_NULL;
  /* State changes are applied synchronously, so there is never a transition
   * to wait for. */
  if (next_state)
    *next_state = __atomic_load_n(&stream->state, __ATOMIC_ACQUIRE);
  return AAUDIO_OK;
}

int32_t AAudioStream_getState_fake(void *stream_ptr) {
  const AAudioStream *stream = stream_ptr;
  return stream ? __atomic_load_n(&stream->state, __ATOMIC_ACQUIRE)
                : AAUDIO_STATE_UNINITIALIZED;
}

int32_t AAudioStream_getSampleRate_fake(void *stream_ptr) {
  const AAudioStream *stream = stream_ptr;
  return stream ? stream->rate : opensles_device_rate();
}

int32_t AAudioStream_getChannelCount_fake(void *stream_ptr) {
  const AAudioStream *stream = stream_ptr;
  return stream ? stream->channels : 2;
}

int32_t AAudioStream_getFormat_fake(void *stream_ptr) {
  const AAudioStream *stream = stream_ptr;
  return stream ? stream->format : AAUDIO_FORMAT_PCM_I16;
}

int32_t AAudioStream_getFramesPerBurst_fake(void *stream_ptr) {
  (void)stream_ptr;
  return AAUDIO_BURST_FRAMES;
}

int32_t AAudioStream_getFramesPerDataCallback_fake(void *stream_ptr) {
  const AAudioStream *stream = stream_ptr;
  return stream ? stream->frames_per_callback : AAUDIO_BURST_FRAMES;
}

int32_t AAudioStream_getBufferSizeInFrames_fake(void *stream_ptr) {
  const AAudioStream *stream = stream_ptr;
  return stream ? stream->buffer_size : AAUDIO_BURST_FRAMES * 2;
}

int32_t AAudioStream_setBufferSizeInFrames_fake(void *stream_ptr,
                                                int32_t frames) {
  AAudioStream *stream = stream_ptr;
  if (!stream)
    return AAUDIO_ERROR_NULL;
  if (frames < AAUDIO_BURST_FRAMES)
    frames = AAUDIO_BURST_FRAMES;
  if (frames > AAUDIO_BUFFER_CAPACITY)
    frames = AAUDIO_BUFFER_CAPACITY;
  stream->buffer_size = frames;
  return frames;
}

int32_t AAudioStream_getBufferCapacityInFrames_fake(void *stream_ptr) {
  (void)stream_ptr;
  return AAUDIO_BUFFER_CAPACITY;
}

int32_t AAudioStream_getXRunCount_fake(void *stream_ptr) {
  (void)stream_ptr;
  return 0;
}

int32_t AAudioStream_getPerformanceMode_fake(void *stream_ptr) {
  const AAudioStream *stream = stream_ptr;
  return stream ? stream->perf_mode : 10 /* AAUDIO_PERFORMANCE_MODE_NONE */;
}

int32_t AAudioStream_getUsage_fake(void *stream_ptr) {
  const AAudioStream *stream = stream_ptr;
  return stream ? stream->usage : 0;
}

int32_t AAudioStream_getSharingMode_fake(void *stream_ptr) {
  const AAudioStream *stream = stream_ptr;
  return stream ? stream->sharing_mode : 0;
}

int32_t AAudioStream_getDirection_fake(void *stream_ptr) {
  (void)stream_ptr;
  return 0; /* AAUDIO_DIRECTION_OUTPUT */
}

int32_t AAudioStream_getDeviceId_fake(void *stream_ptr) {
  (void)stream_ptr;
  return 0;
}

int32_t AAudioStream_getSessionId_fake(void *stream_ptr) {
  (void)stream_ptr;
  return -1; /* AAUDIO_SESSION_ID_NONE */
}

int AAudioStream_isMMapUsed_fake(void *stream_ptr) {
  (void)stream_ptr;
  return 0;
}

int64_t AAudioStream_getFramesWritten_fake(void *stream_ptr) {
  const AAudioStream *stream = stream_ptr;
  uint64_t written = 0;
  if (stream && stream->ring)
    opensles_aaudio_stats(stream->ring, &written, NULL);
  return (int64_t)written;
}

int64_t AAudioStream_getFramesRead_fake(void *stream_ptr) {
  const AAudioStream *stream = stream_ptr;
  if (!stream || !stream->ring)
    return 0;
  const int64_t written = AAudioStream_getFramesWritten_fake(stream_ptr);
  const int64_t queued = opensles_aaudio_queued_frames(stream->ring);
  return written > queued ? written - queued : 0;
}

int AAudioStream_getTimestamp_fake(void *stream_ptr, int32_t clock_id,
                                   int64_t *position, int64_t *time_ns) {
  (void)stream_ptr;
  (void)clock_id;
  (void)position;
  (void)time_ns;
  return AAUDIO_ERROR_UNIMPLEMENTED;
}

int32_t AAudioStream_read_fake(void *stream_ptr, void *buffer, int32_t frames,
                               int64_t timeout_ns) {
  (void)stream_ptr;
  (void)buffer;
  (void)frames;
  (void)timeout_ns;
  return AAUDIO_ERROR_UNIMPLEMENTED; /* no capture on this port */
}

const char *AAudio_convertResultToText_fake(int32_t result) {
  switch (result) {
  case AAUDIO_OK: return "AAUDIO_OK";
  case AAUDIO_ERROR_DISCONNECTED: return "AAUDIO_ERROR_DISCONNECTED";
  case AAUDIO_ERROR_ILLEGAL_ARGUMENT: return "AAUDIO_ERROR_ILLEGAL_ARGUMENT";
  case AAUDIO_ERROR_INVALID_STATE: return "AAUDIO_ERROR_INVALID_STATE";
  case AAUDIO_ERROR_INVALID_HANDLE: return "AAUDIO_ERROR_INVALID_HANDLE";
  case AAUDIO_ERROR_UNIMPLEMENTED: return "AAUDIO_ERROR_UNIMPLEMENTED";
  case AAUDIO_ERROR_UNAVAILABLE: return "AAUDIO_ERROR_UNAVAILABLE";
  case AAUDIO_ERROR_NO_MEMORY: return "AAUDIO_ERROR_NO_MEMORY";
  case AAUDIO_ERROR_NULL: return "AAUDIO_ERROR_NULL";
  case AAUDIO_ERROR_TIMEOUT: return "AAUDIO_ERROR_TIMEOUT";
  default: return "AAUDIO_ERROR_UNKNOWN";
  }
}

const char *AAudio_convertStreamStateToText_fake(int32_t state) {
  static const char *const names[] = {
      "UNINITIALIZED", "UNKNOWN", "OPEN",    "STARTING", "STARTED",
      "PAUSING",       "PAUSED",  "FLUSHING", "FLUSHED", "STOPPING",
      "STOPPED",       "CLOSING", "CLOSED",  "DISCONNECTED"};
  return state >= 0 && state < (int32_t)(sizeof(names) / sizeof(names[0]))
             ? names[state]
             : "UNKNOWN";
}

int32_t AAudioStream_write_fake(void *stream_ptr, const void *buffer,
                                int32_t frames, int64_t timeout_ns) {
  AAudioStream *stream = stream_ptr;
  if (!stream || !buffer)
    return AAUDIO_ERROR_NULL;
  if (frames <= 0)
    return 0;
  if (stream->data_cb)
    return AAUDIO_ERROR_INVALID_STATE; /* callback streams cannot be written */
  if (!stream->started) {
    /* Real AAudio would just fill its buffer until started; starting here is
     * the lenient equivalent and keeps a guest that forgets requestStart
     * audible. */
    AAudioStream_requestStart_fake(stream);
  }

  /* Real AAudio blocks until the frames are queued or the timeout expires; the
   * guest's audio thread relies on that to pace itself. Honour it (capped to the
   * 100 ms the Android host mixer uses, and only while a device is actually
   * draining the ring) instead of returning a short write immediately, which
   * made the producer outrun the device and the guest give up on audio. */
  const int64_t max_wait_ns = 100000000; /* match Android host 100 ms timeout */
  int64_t wait_ns = timeout_ns;
  if (wait_ns < 0 || wait_ns > max_wait_ns)
    wait_ns = max_wait_ns;
  if (!opensles_device_ready()) {
    /* Device may still be coming up; give it a short window instead of
     * immediately returning 0 (which the guest treats as failure and may
     * permanently disable its audio backend). */
    wait_ns = 20000000; /* 20 ms */
  }

  const int is_float = stream->format == AAUDIO_FORMAT_PCM_FLOAT;
  const uint8_t *cursor = (const uint8_t *)buffer;
  int total = 0;
  int64_t waited_ns = 0;
  for (;;) {
    const int n = opensles_aaudio_write_pcm(stream->ring, cursor,
                                            frames - total, stream->channels,
                                            is_float);
    if (n > 0) {
      total += n;
      cursor += (size_t)n * (size_t)stream->bytes_per_frame;
    }
    if (total >= frames || waited_ns >= wait_ns)
      break;
    svcSleepThread(2000000LL); /* 2 ms; the mixer drains ~512 frames / 10 ms */
    waited_ns += 2000000LL;
  }

  /* First few writes, then roughly every 5 s: lets the log distinguish "the
   * guest only sends silence" (input peak 0) from "we drop it" (queued stuck
   * at 0 / mixer peak 0 with input peak > 0). */
  const uint64_t call = ++stream->write_calls;
  if (call <= 3 || (call & 0x1ff) == 0)
    aaudio_log_stats(stream, "write");
  return total;
}

int AAudioStream_close_fake(void *stream_ptr) {
  AAudioStream *stream = stream_ptr;
  if (!stream)
    return AAUDIO_OK;
  __atomic_store_n(&stream->state, AAUDIO_STATE_CLOSING, __ATOMIC_RELEASE);
  if (stream->pump_running) {
    __atomic_store_n(&stream->pump_quit, 1, __ATOMIC_RELEASE);
    pthread_join(stream->pump, NULL);
    stream->pump_running = 0;
  }
  /* Only this stream's ring goes away; other open streams keep playing. */
  opensles_aaudio_close(stream->ring);
  free(stream);
  return AAUDIO_OK;
}

/* ------------------------------------------------------------------------ *
 * NDK media codec / extractor
 *
 * Hardware video decode is not wired up. Everything reports failure or a
 * NULL handle so the runtime's own error path decides what to do with movie
 * playback. Each entry point logs once.
 * ------------------------------------------------------------------------ */

#define AMEDIA_ERROR_UNKNOWN (-10000)
static int g_media_warned;

static void media_unavailable(const char *what) {
  if (!g_media_warned) {
    g_media_warned = 1;
    debugPrintf("media: %s requested; video/audio decode via NDK media is not implemented\n",
                what);
  }
}

const char *ib3_amediaformat_key_width = "width";
const char *ib3_amediaformat_key_height = "height";
const char *ib3_amediaformat_key_mime = "mime";
const char *ib3_amediaformat_key_duration = "durationUs";
const char *ib3_amediaformat_key_stride = "stride";
const char *ib3_amediaformat_key_slice_height = "slice-height";
const char *ib3_amediaformat_key_color_format = "color-format";
const char *ib3_amediaformat_key_display_crop = "crop";
const char *ib3_amediaformat_key_sample_rate = "sample-rate";
const char *ib3_amediaformat_key_channel_count = "channel-count";
const char *ib3_amediaformat_key_pcm_encoding = "pcm-encoding";

void *AMediaExtractor_new_fake(void) {
  media_unavailable("AMediaExtractor_new");
  return NULL;
}
int AMediaExtractor_delete_fake(void *e) { (void)e; return 0; }
int AMediaExtractor_setDataSourceFd_fake(void *e, int fd, off_t o, off_t l) {
  (void)e; (void)fd; (void)o; (void)l;
  return AMEDIA_ERROR_UNKNOWN;
}
size_t AMediaExtractor_getTrackCount_fake(void *e) { (void)e; return 0; }
void *AMediaExtractor_getTrackFormat_fake(void *e, size_t i) {
  (void)e; (void)i;
  return NULL;
}
int AMediaExtractor_selectTrack_fake(void *e, size_t i) {
  (void)e; (void)i;
  return AMEDIA_ERROR_UNKNOWN;
}
int AMediaExtractor_seekTo_fake(void *e, int64_t us, int mode) {
  (void)e; (void)us; (void)mode;
  return AMEDIA_ERROR_UNKNOWN;
}
ssize_t AMediaExtractor_readSampleData_fake(void *e, uint8_t *b, size_t c) {
  (void)e; (void)b; (void)c;
  return -1;
}
int64_t AMediaExtractor_getSampleTime_fake(void *e) { (void)e; return -1; }
int AMediaExtractor_advance_fake(void *e) { (void)e; return 0; }
int AMediaFormat_delete_fake(void *f) { (void)f; return 0; }
int AMediaFormat_getInt32_fake(void *f, const char *n, int32_t *o) {
  (void)f; (void)n; (void)o;
  return 0;
}
int AMediaFormat_getInt64_fake(void *f, const char *n, int64_t *o) {
  (void)f; (void)n; (void)o;
  return 0;
}
int AMediaFormat_getString_fake(void *f, const char *n, const char **o) {
  (void)f; (void)n; (void)o;
  return 0;
}
int AMediaFormat_getBuffer_fake(void *f, const char *n, void **d, size_t *s) {
  (void)f; (void)n; (void)d; (void)s;
  return 0;
}
int AMediaFormat_getRect_fake(void *f, const char *n, int32_t *l, int32_t *t,
                              int32_t *r, int32_t *b) {
  (void)f; (void)n; (void)l; (void)t; (void)r; (void)b;
  return 0;
}
void AMediaFormat_setInt32_fake(void *f, const char *n, int32_t v) {
  (void)f; (void)n; (void)v;
}
void *AMediaCodec_createDecoderByType_fake(const char *mime) {
  media_unavailable(mime ? mime : "AMediaCodec_createDecoderByType");
  return NULL;
}
int AMediaCodec_configure_fake(void *c, const void *f, void *s, void *cr,
                               uint32_t fl) {
  (void)c; (void)f; (void)s; (void)cr; (void)fl;
  return AMEDIA_ERROR_UNKNOWN;
}
int AMediaCodec_start_fake(void *c) { (void)c; return AMEDIA_ERROR_UNKNOWN; }
int AMediaCodec_stop_fake(void *c) { (void)c; return 0; }
int AMediaCodec_flush_fake(void *c) { (void)c; return 0; }
int AMediaCodec_delete_fake(void *c) { (void)c; return 0; }
ssize_t AMediaCodec_dequeueInputBuffer_fake(void *c, int64_t t) {
  (void)c; (void)t;
  return -1;
}
uint8_t *AMediaCodec_getInputBuffer_fake(void *c, size_t i, size_t *o) {
  (void)c; (void)i;
  if (o)
    *o = 0;
  return NULL;
}
int AMediaCodec_queueInputBuffer_fake(void *c, size_t i, off_t o, size_t s,
                                      uint64_t t, uint32_t f) {
  (void)c; (void)i; (void)o; (void)s; (void)t; (void)f;
  return AMEDIA_ERROR_UNKNOWN;
}
ssize_t AMediaCodec_dequeueOutputBuffer_fake(void *c, void *i, int64_t t) {
  (void)c; (void)i; (void)t;
  return -1;
}
uint8_t *AMediaCodec_getOutputBuffer_fake(void *c, size_t i, size_t *o) {
  (void)c; (void)i;
  if (o)
    *o = 0;
  return NULL;
}
void *AMediaCodec_getOutputFormat_fake(void *c) { (void)c; return NULL; }
int AMediaCodec_releaseOutputBuffer_fake(void *c, size_t i, int r) {
  (void)c; (void)i; (void)r;
  return 0;
}

/* ------------------------------------------------------------------------ *
 * Input
 *
 * The existing input queue only produces touch/motion events. libib3.so also
 * imports the key-event accessors, so answer them coherently (no key events
 * are ever queued yet).
 * ------------------------------------------------------------------------ */

#define AINPUT_SOURCE_TOUCHSCREEN 0x00001002

int AInputEvent_getSource_fake(void *event) {
  return event ? AINPUT_SOURCE_TOUCHSCREEN : 0;
}
int AKeyEvent_getAction_fake(void *event) { (void)event; return 0; }
int AKeyEvent_getKeyCode_fake(void *event) { (void)event; return 0; }
int AKeyEvent_getRepeatCount_fake(void *event) { (void)event; return 0; }

/* ------------------------------------------------------------------------ *
 * Misc bionic / POSIX
 * ------------------------------------------------------------------------ */

int android_log_write_fake(int priority, const char *tag, const char *text) {
  (void)priority;
  debugPrintf("[%s] %s\n", tag ? tag : "?", text ? text : "");
  return 1;
}

int __open_2_fake(const char *path, int flags) {
  return open_fake(path, flags, 0);
}

ssize_t __read_chk_fake(int fd, void *buf, size_t count, size_t buf_size) {
  if (count > buf_size)
    abort();
  return read_dispatch_fake(fd, buf, count);
}

ssize_t __write_chk_fake(int fd, const void *buf, size_t count,
                         size_t buf_size) {
  if (count > buf_size)
    abort();
  return write_dispatch_fake(fd, buf, count);
}

ssize_t __readlink_chk_fake(const char *path, char *buf, size_t size,
                            size_t buf_size) {
  (void)path; (void)buf; (void)size; (void)buf_size;
  errno = ENOENT; /* /proc/self/exe and friends do not exist here */
  return -1;
}

static Mutex g_pio_lock;

ssize_t pread_fake(int fd, void *buf, size_t count, off_t offset) {
  mutexLock(&g_pio_lock);
  const off_t saved = lseek(fd, 0, SEEK_CUR);
  ssize_t result = -1;
  if (saved >= 0 && lseek(fd, offset, SEEK_SET) >= 0) {
    result = read(fd, buf, count);
    lseek(fd, saved, SEEK_SET);
  }
  mutexUnlock(&g_pio_lock);
  return result;
}

ssize_t pwrite_fake(int fd, const void *buf, size_t count, off_t offset) {
  mutexLock(&g_pio_lock);
  const off_t saved = lseek(fd, 0, SEEK_CUR);
  ssize_t result = -1;
  if (saved >= 0 && lseek(fd, offset, SEEK_SET) >= 0) {
    result = write(fd, buf, count);
    lseek(fd, saved, SEEK_SET);
  }
  mutexUnlock(&g_pio_lock);
  return result;
}

/* Fortify-source wrappers (libib3 1.6.1). The extra buflen argument is only a
 * bounds check; ignore it and call the normal path. */
ssize_t __pread_chk_fake(int fd, void *buf, size_t count, off_t offset,
                         size_t buflen) {
  (void)buflen;
  return pread_fake(fd, buf, count, offset);
}

ssize_t __pwrite_chk_fake(int fd, const void *buf, size_t count, off_t offset,
                          size_t buflen) {
  (void)buflen;
  return pwrite_fake(fd, buf, count, offset);
}

int fsync_fake(int fd) {
  (void)fd;
  return 0;
}

int ftruncate_fake(int fd, off_t length) { return ftruncate(fd, length); }

int truncate_fake(const char *path, off_t length) {
  const int fd = open_fake(path, O_WRONLY, 0);
  if (fd < 0)
    return -1;
  const int result = ftruncate(fd, length);
  close(fd);
  return result;
}

int fchmod_fake(int fd, mode_t mode) { (void)fd; (void)mode; return 0; }
int fchmodat_fake(int d, const char *p, mode_t m, int f) {
  (void)d; (void)p; (void)m; (void)f;
  return 0;
}
int fchown_fake(int fd, uid_t o, gid_t g) { (void)fd; (void)o; (void)g; return 0; }
void *fdopendir_fake(int fd) { (void)fd; errno = ENOSYS; return NULL; }
int link_fake(const char *a, const char *b) {
  (void)a; (void)b;
  errno = ENOSYS;
  return -1;
}
int symlink_fake(const char *a, const char *b) {
  (void)a; (void)b;
  errno = ENOSYS;
  return -1;
}

ssize_t sendfile_fake(int out_fd, int in_fd, off_t *offset, size_t count) {
  uint8_t chunk[0x4000];
  size_t total = 0;
  while (total < count) {
    const size_t want = count - total < sizeof(chunk) ? count - total : sizeof(chunk);
    const ssize_t got = offset ? pread_fake(in_fd, chunk, want, *offset + (off_t)total)
                               : read(in_fd, chunk, want);
    if (got <= 0)
      break;
    if (write(out_fd, chunk, (size_t)got) != got)
      return -1;
    total += (size_t)got;
  }
  if (offset)
    *offset += (off_t)total;
  return (ssize_t)total;
}

ssize_t getrandom_fake(void *buf, size_t length, unsigned flags) {
  (void)flags;
  randomGet(buf, length);
  return (ssize_t)length;
}

ssize_t process_vm_readv_fake(int pid, const void *l, unsigned long lc,
                              const void *r, unsigned long rc,
                              unsigned long flags) {
  (void)pid; (void)l; (void)lc; (void)r; (void)rc; (void)flags;
  errno = ENOSYS;
  return -1;
}

int utimensat_fake(int d, const char *p, const void *t, int f) {
  (void)d; (void)p; (void)t; (void)f;
  return 0;
}
int utimes_fake(const char *p, const void *t) { (void)p; (void)t; return 0; }

/* bionic's struct sysinfo (LP64). */
struct bionic_sysinfo {
  long uptime;
  unsigned long loads[3];
  unsigned long totalram, freeram, sharedram, bufferram;
  unsigned long totalswap, freeswap;
  unsigned short procs, pad;
  unsigned long totalhigh, freehigh;
  unsigned int mem_unit;
  char reserved[4];
};

int sysinfo_fake(void *out) {
  struct bionic_sysinfo *info = out;
  uint64_t total = 0, used = 0;
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  memset(info, 0, sizeof(*info));
  info->uptime = (long)(armTicksToNs(armGetSystemTick()) / 1000000000ULL);
  info->totalram = total;
  info->freeram = total > used ? total - used : 0;
  info->procs = 1;
  info->mem_unit = 1;
  return 0;
}

/* pthread_getattr_np / pthread_attr_getstack: report the real bounds of the
 * calling thread's stack by asking the kernel about a local's address. */
typedef struct {
  void *stack_addr;
  size_t stack_size;
} StackAttr;

int pthread_getattr_np_fake(unsigned long thread, void *attr) {
  (void)thread;
  StackAttr *out = attr;
  MemoryInfo info;
  u32 page_info;
  volatile int marker = 0;
  if (R_FAILED(svcQueryMemory(&info, &page_info, (u64)(uintptr_t)&marker)))
    return EINVAL;
  out->stack_addr = (void *)(uintptr_t)info.addr;
  out->stack_size = (size_t)info.size;
  return 0;
}

int pthread_attr_getstack_fake(const void *attr, void **addr, size_t *size) {
  const StackAttr *in = attr;
  *addr = in->stack_addr;
  *size = in->stack_size;
  return 0;
}

/* Horizon has no POSIX signals. We store handlers and synthesise delivery
 * from the user exception handler for SIGILL/SIGSEGV/SIGBUS/SIGFPE so the
 * guest JIT's zero/UDF trampolines and fastmem traps can run. */
#define IB3_SIG_MAX 32
#define BIONIC_SA_SIGINFO 4
#define BIONIC_SIG_DFL ((void *)0)
#define BIONIC_SIG_IGN ((void *)1)

typedef void (*ib3_sighandler_t)(int);
typedef void (*ib3_sigaction_t)(int, void * /*siginfo*/, void * /*ucontext*/);

typedef struct {
  void *handler; /* sa_handler / sa_sigaction union */
  int flags;
  int set;
} Ib3SigReg;

static Ib3SigReg g_sigreg[IB3_SIG_MAX];
static int g_signal_warned;
static int g_signal_logs;
static int g_signal_deliver_logs;

/* `act` is bionic's arm64 struct sigaction: int sa_flags @0, handler @8. */
static void note_signal(int signum, const void *act) {
  if (!g_signal_warned) {
    g_signal_warned = 1;
    debugPrintf("signal: handlers stored; SIGILL/SEGV/BUS/FPE synthesised on fault\n");
  }
  if (g_signal_logs < 48) {
    g_signal_logs++;
    if (act)
      debugPrintf("signal: sigaction(%d) flags=%x handler=%p (libib3+0x%llx)\n", signum,
                  *(const int *)act, *(void *const *)((const char *)act + 8),
                  (unsigned long long)(*(const uintptr_t *)((const char *)act + 8) -
                                       (uintptr_t)game_mod.load_virtbase));
    else
      debugPrintf("signal: signal(%d) registered\n", signum);
  }
}

int sigaction_fake(int signum, const void *act, void *oldact) {
  if (oldact) {
    memset(oldact, 0, 32);
    if (signum >= 0 && signum < IB3_SIG_MAX && g_sigreg[signum].set) {
      *(int *)oldact = g_sigreg[signum].flags;
      *(void **)((char *)oldact + 8) = g_sigreg[signum].handler;
    }
  }
  if (act && signum >= 0 && signum < IB3_SIG_MAX) {
    g_sigreg[signum].flags = *(const int *)act;
    g_sigreg[signum].handler = *(void *const *)((const char *)act + 8);
    g_sigreg[signum].set = 1;
    note_signal(signum, act);
  }
  return 0;
}

void *signal_fake(int signum, void *handler) {
  void *prev = BIONIC_SIG_DFL;
  if (signum >= 0 && signum < IB3_SIG_MAX) {
    prev = g_sigreg[signum].set ? g_sigreg[signum].handler : BIONIC_SIG_DFL;
    g_sigreg[signum].handler = handler;
    g_sigreg[signum].flags = 0;
    g_sigreg[signum].set = 1;
  }
  note_signal(signum, NULL);
  if (g_signal_logs <= 48)
    debugPrintf("signal: signal(%d) handler=%p\n", signum, handler);
  return prev;
}

int sigaltstack_fake(const void *ss, void *old_ss) {
  (void)ss;
  if (old_ss)
    memset(old_ss, 0, 24);
  return 0;
}

/* Minimal bionic-compatible siginfo for synthesised delivery. */
typedef struct {
  int si_signo;
  int si_errno;
  int si_code;
  int si_pad;
  void *si_addr;
  char si_rest[64];
} Ib3SigInfo;

/*
 * Bionic/Linux aarch64 ucontext layout (matches glibc so uc_mcontext.pc sits
 * at offset 0x1B8). The v7 log's fault=0x1b8 was a NULL ucontext dereference
 * of exactly that field.
 *
 *   uc_flags(8) + uc_link(8) + stack_t(24) + sigmask(8) + pad(128) = 0xB0
 *   mcontext: fault_address, regs[31], sp, pc, pstate
 */
typedef struct {
  uint64_t uc_flags;
  void *uc_link;
  void *ss_sp;
  int ss_flags;
  int ss_pad;
  uint64_t ss_size;
  uint64_t uc_sigmask;
  char __padding[128];
  /* mcontext_t (sigcontext) */
  uint64_t fault_address;
  uint64_t regs[31];
  uint64_t sp;
  uint64_t pc;
  uint64_t pstate;
  /* FPSIMD / reserved — keep writable so handlers that poke further don't SEGV */
  uint64_t reserved[64];
} Ib3UContext;

static volatile int g_in_signal_delivery;

/*
 * Deliver a synthesised POSIX signal to a registered handler.
 * Returns 1 if a handler was invoked (caller should resume, possibly at a
 * new PC from *out_pc), 0 if no handler / ignored / default.
 *
 * regs may be NULL; otherwise it is x0..x30, sp, pc, pstate (34 values).
 * After SA_SIGINFO handlers return, *out_pc receives uc_mcontext.pc (so a
 * handler that patches an instruction and advances PC is honoured).
 *
 * Re-entrancy is refused: if the handler itself faults we must not loop.
 */
int ib3_deliver_signal(int signum, uint64_t fault_addr, uint64_t pc,
                       const uint64_t *regs, uint64_t *out_pc) {
  if (signum < 0 || signum >= IB3_SIG_MAX || !g_sigreg[signum].set)
    return 0;
  void *h = g_sigreg[signum].handler;
  if (h == BIONIC_SIG_DFL || h == BIONIC_SIG_IGN || h == NULL)
    return 0;

  if (g_in_signal_delivery) {
    if (g_signal_deliver_logs < 40) {
      g_signal_deliver_logs++;
      debugPrintf("signal: nested deliver blocked sig=%d fault=%p pc=%p\n",
                  signum, (void *)(uintptr_t)fault_addr,
                  (void *)(uintptr_t)pc);
    }
    return 0;
  }

  if (g_signal_deliver_logs < 32) {
    g_signal_deliver_logs++;
    debugPrintf("signal: deliver sig=%d handler=%p fault=%p pc=%p\n", signum, h,
                (void *)(uintptr_t)fault_addr, (void *)(uintptr_t)pc);
  }

  Ib3SigInfo info;
  memset(&info, 0, sizeof(info));
  info.si_signo = signum;
  info.si_code = (signum == 4) ? 2 /* ILL_ILLOPC */ : 1 /* SEGV_MAPERR-ish */;
  info.si_addr = (void *)(uintptr_t)(fault_addr ? fault_addr : pc);

  Ib3UContext uctx;
  memset(&uctx, 0, sizeof(uctx));
  uctx.fault_address = fault_addr ? fault_addr : pc;
  uctx.pc = pc;
  uctx.pstate = 0;
  if (regs) {
    for (int i = 0; i < 31; i++)
      uctx.regs[i] = regs[i];
    uctx.sp = regs[31];
    uctx.pc = regs[32];
    uctx.pstate = regs[33];
  }

  g_in_signal_delivery = 1;
  if (g_sigreg[signum].flags & BIONIC_SA_SIGINFO) {
    ib3_sigaction_t sa = (ib3_sigaction_t)h;
    sa(signum, &info, &uctx);
  } else {
    ib3_sighandler_t sh = (ib3_sighandler_t)h;
    sh(signum);
  }
  g_in_signal_delivery = 0;

  if (out_pc)
    *out_pc = uctx.pc;
  return 1;
}
