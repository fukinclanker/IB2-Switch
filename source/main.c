#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <switch.h>

#include "ib3_shim.h"
#include "imports.h"
#include "native_activity.h"
#include "music_player.h"
#include "opensles.h"
#include "blocktrace.h"
#include "so_util.h"
#include "util.h"
#include "memwatch.h"

#define APP_VERSION "1.1.0-ib2"
/* Shrink the SO arena slightly so more of the Horizon heap stays available
 * for UE3 texture streaming (bad_alloc after boss/island loads). libib3.so
 * itself is only ~4 MiB; 24 MiB still leaves ample room for code caches. */
#define SO_ARENA_SIZE (24u * 1024u * 1024u)

typedef struct {
  const char *path;
  int64_t expected_size;
  int directory;
} RuntimeFile;

/* IB2 Android runtime libib3.so is 4444616 bytes (InfinityBladeII-Android-1.6.1).
 * Size 0 disables the strict size check so either the IB2 or older IB3 library
 * can be dropped in during development. */
static const RuntimeFile runtime_files[] = {
    {"libib3.so", 4444616, 0},
    {"cursor.png", 0, 0},
    /* Extracted IPA: game/Payload/SwordGame.app/... (tools/prepare_runtime.py) */
    {"game/Payload/SwordGame.app/CookedIPhone/Engine.xxx", 0, 0},
    {"SaveData", 0, 1},
};

static void *so_arena_base;
static size_t so_arena_size;
static size_t heap_total_size;
static uint8_t main_android_tls[BIONIC_TLS_SIZE] __attribute__((aligned(64)));

/* The single native module: libib3.so, the iOS-compatibility runtime. */
so_module game_mod;
volatile int g_hide_saves = 0;
int screen_width = 1280;
int screen_height = 720;

void __libnx_initheap(void) {
  void *heap_base = NULL;
  size_t heap_size = 0;

  if (envHasHeapOverride()) {
    heap_base = envGetHeapOverrideAddr();
    heap_size = envGetHeapOverrideSize();
  } else {
    size_t total = 0;
    size_t used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    heap_size = total > used + 0x400000 ? total - used - 0x400000 : 0x20000000;
    heap_size &= ~(size_t)0x1fffff;
    Result rc = svcSetHeapSize(&heap_base, heap_size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t arena = heap_size > SO_ARENA_SIZE + 0x10000000
                           ? SO_ARENA_SIZE
                           : (heap_size / 4) & ~(size_t)0xfff;
  const size_t newlib_size = heap_size - arena;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = heap_base;
  fake_heap_end = (char *)heap_base + newlib_size;

  so_arena_base = (void *)ALIGN_MEM((uintptr_t)fake_heap_end, 0x1000);
  so_arena_size = (char *)heap_base + heap_size - (char *)so_arena_base;
  heap_total_size = heap_size;
}

static int validate_runtime(void) {
  int failures = 0;

  puts("Runtime inventory:");
  for (size_t index = 0;
       index < sizeof(runtime_files) / sizeof(runtime_files[0]); index++) {
    const RuntimeFile *file = &runtime_files[index];
    struct stat status;
    if (stat(file->path, &status) != 0) {
      printf("  [--] %-24s MISSING\n", file->path);
      failures++;
      continue;
    }

    if ((file->directory && !S_ISDIR(status.st_mode)) ||
        (!file->directory && !S_ISREG(status.st_mode))) {
      printf("  [!!] %-24s wrong file type\n", file->path);
      failures++;
      continue;
    }

    if (file->expected_size > 0 &&
        (int64_t)status.st_size != file->expected_size) {
      printf("  [!!] %-24s size=%" PRId64 " expected=%" PRId64 "\n",
             file->path, (int64_t)status.st_size, file->expected_size);
      failures++;
      continue;
    }
    printf("  [OK] %-24s %s\n", file->path,
           file->directory ? "directory" : "fingerprint size");
  }
  return failures;
}

static int code_memory_syscalls_available(void) {
  const int available = envIsSyscallHinted(0x73) && envIsSyscallHinted(0x77) &&
                        envIsSyscallHinted(0x78);
  printf("Code-memory syscall hints: %s\n", available ? "present" : "missing");
  return available;
}

static int load_one(so_module *module, const char *path, void **cursor) {
  const uintptr_t used = (uintptr_t)*cursor - (uintptr_t)so_arena_base;
  if (used >= so_arena_size)
    return -3;
  const int result = so_load(module, path, *cursor, so_arena_size - used);
  if (result == 0)
    *cursor = (void *)ALIGN_MEM((uintptr_t)*cursor + module->load_size, 0x1000);
  return result;
}

static int run_loader_audit(void) {
  void *cursor = so_arena_base;
  printf("\nELF arena: %zu MiB of %zu MiB total heap\n", so_arena_size >> 20,
         heap_total_size >> 20);

  puts("Loading libib3.so ...");
  int result = load_one(&game_mod, "libib3.so", &cursor);
  if (result != 0) {
    printf("libib3.so load failed: %d\n", result);
    return 1;
  }
  printf("  mapped image requirement: %zu KiB\n", game_mod.load_size >> 10);

  puts("Applying AArch64 relocations ...");
  if (so_relocate(&game_mod) != 0) {
    puts("Relocation failed.");
    return 1;
  }

  puts("Resolving shim imports ...");
  const int game_missing =
      so_resolve(&game_mod, dynlib_functions, (int)dynlib_numfunctions, 0);

  const uintptr_t native_activity =
      so_try_find_addr_rx(&game_mod, "ANativeActivity_onCreate");
  const uintptr_t android_main = so_try_find_addr_rx(&game_mod, "android_main");
  printf("Entry ANativeActivity_onCreate: %s\n",
         native_activity ? "present" : "MISSING");
  printf("Entry android_main: %s\n", android_main ? "present" : "MISSING");
  printf("Unresolved relocation records: %d\n", game_missing);

  if (game_missing) {
    puts("Constructor execution intentionally blocked until these shims exist.");
    puts("See infinityblade2_nx.log for symbol-level diagnostics.");
    return 2;
  }

  puts("All imports resolved. Finalizing executable mappings ...");
  so_finalize(&game_mod);
  so_flush_caches(&game_mod);
  thread_registry_add();
  bt_start_watchdog();

  /* Keep each ELF's temporary metadata image alive. Constructor traversal,
   * symbol lookup during NativeActivity bootstrap, and runtime dlsym all use
   * its section headers, symbol table, and string tables. The first test build
   * freed these here; OpenAL allocations reused the block and the traversal
   * subsequently called strcmp with an overwritten section-name pointer. */

  install_bionic_tls(main_android_tls);
  puts("Executing libib3.so constructors ...");
  debugPrintf("stage: libib3.so constructors begin\n");
  so_execute_init_array(&game_mod);
  debugPrintf("stage: libib3.so constructors done\n");

  /* Open the shared SDL audio device as early as possible so the first
   * AAudioStreamBuilder_openStream / slCreateEngine from the guest never
   * races a cold SDL_InitSubSystem. Matches the Android host mixer which
   * brings AAudio up before any AUGraph/AVAudioPlayer work. */
  if (opensles_ensure_shared_device())
    debugPrintf("stage: shared audio device ready early\n");
  else
    debugPrintf("stage: shared audio device open failed (will retry on first use)\n");

  puts("Constructors completed; NativeActivity handoff is ready.");
  return 0;
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  /* Before anything else maps memory: keep the guest's fixed window free. */
  const int guest_window = ib3_reserve_guest_window();
  /* Remember the default core so audio threads can be moved off it. */
  util_record_main_core();

  consoleInit(NULL);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  printf("Infinity Blade II %s\n", APP_VERSION);
  puts("ARM64 Android compatibility-loader (iOS runtime, IB2)\n");
  int game_started = 0;
  int console_active = 1;
  const int failures = validate_runtime();
  if (failures) {
    printf("\nRuntime is incomplete (%d required check%s failed).\n", failures,
           failures == 1 ? "" : "s");
    puts("Use tools/prepare_runtime.py with your own IB2 APK and IPA.");
  } else if (guest_window != 0) {
    puts("\nThis launch's random memory layout put a system region inside the");
    puts("guest's fixed address window. Close the app and launch it again.");
  } else if (!code_memory_syscalls_available()) {
    puts("\nLaunch through full-memory title override, not Album applet mode.");
  } else {
    const int audit = run_loader_audit();
    printf("\nLoader audit result: %s\n",
           audit == 0 ? "ready" : audit == 2 ? "shim work remains" : "failed");
    if (audit == 0) {
      puts("Handing the default window to the IB2 runtime; further detail is in the log.");
      consoleUpdate(NULL);
      consoleExit(NULL);
      console_active = 0;
      gpu_clock(1);
      memwatch_start(); /* 10 s memory / texture telemetry in the log */
      game_started = native_activity_bootstrap(&game_mod) == 0;
    }
  }
  if (!game_started)
    puts("\nPress + to exit.");
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (!game_started &&
        (padGetButtonsDown(&pad) & HidNpadButton_Plus))
      break;
    if (game_started) {
      /* appletMainLoop() and padUpdate() never block and the console is gone,
       * so this loop used to spin flat out at the main thread's priority
       * (0x2C) on the default core -- the same core every guest pthread,
       * including the game's audio thread and SDL's mixer thread (all at
       * 0x3B), is created on. Only applet messages need servicing here. */
      svcSleepThread(16000000LL);
      continue;
    }
    consoleUpdate(NULL);
  }

  if (game_started)
    native_activity_shutdown();
  else if (console_active)
    consoleExit(NULL);
  music_player_stop();
  opensles_shutdown();
  gpu_clock(0);
  return 0;
}
