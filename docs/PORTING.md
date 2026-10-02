# Porting notes: Infinity Blade III

## Target contract

Input: `InfinityBladeIII-Android-1_2_1.apk`, package `com.ib3port.game`
(`LauncherActivity` + `GameActivity`, a `NativeActivity`).

| Library | Size | SHA-256 |
|---|---:|---|
| `libib3.so` (arm64-v8a) | 4,133,456 | `94ca83a3713bd65bef20545e5b72c2fac12fd747425eae9c24fda5aef6cc9340` |

The APK contains no game data and no OpenAL. Its Java layer asks the user for
an iOS IPA (Infinity Blade III 1.4.4), copies `Payload/SwordGame.app/` into
`<filesDir>/game/`, and starts `GameActivity`.

## What `libib3.so` is

Not a UE3 build. Its symbols show a guest-CPU interpreter/emulator
(`cpu::Thread`), high-level emulation of iOS frameworks (`hle::wrap<...>`,
`objc::*`, CoreGraphics/UIKit/AudioSession stubs), an ObjC runtime and a GLES
translation layer (`gles::SavedState`). Exports of interest:
`ANativeActivity_onCreate`, `android_main`, `ib3_hle_entry`,
`ib3_hle_dispatch`, `ib3_guest_call`. It has no `JNI_OnLoad`.

It links `libaaudio`, `libmediandk`, `libandroid`, `liblog`, `libdl`, `libm`,
`libc` (345 undefined imports).

## What changed relative to the Infinity Blade 1 wrapper

* Single module (`game_mod`); `libopenal.so`, its constructor pass and the
  cross-module resolution were removed from `main.c`/`bionic_shim.c`.
* Runtime layout: `libib3.so`, `game/Payload/SwordGame.app/`, `SaveData/`.
  `tools/prepare_runtime.py` builds it from the APK + your IPA.
* `native_activity.c`: activity class is `com/ib3port/game/GameActivity`; both
  data paths are `"."` so `<path>/game/...` resolves.
* `imports.c`: 29 existing shims that were not wired up are now mapped;
  everything else new lives in `source/ib3_shim.c`:
  * `mmap`/`munmap`/`mprotect`/`mremap`/`madvise` over the process heap
  * AAudio over SDL2 queue-mode audio (blocking `write` paces the producer)
  * NDK media codec/extractor: stubbed as unavailable
  * key-event accessors, `getrandom`, `pread/pwrite`, `sysinfo`,
    `pthread_getattr_np`/`pthread_attr_getstack`, no-op signal calls, etc.
* `bionic_shim.c`: `pthread_mutex_trylock`, `pthread_cond_clockwait`.
* Tooling (`audit_target.py`, `check_import_coverage.py`) retargeted.
* The prebuilt IB1 `.nro` was removed; it does not apply.

Verified (statically): `tools/audit_target.py` reports a known fingerprint and
`tools/check_import_coverage.py` reports `required=345 table=345 missing=0`.
`ib3_shim.c` passes `gcc -fsyntax-only` against stub headers. Nothing else has
been compiled: no devkitPro was available.

## Risks that static analysis cannot settle

1. **Guest memory model.** If the runtime reserves a large fixed-address or
   very large `MAP_NORESERVE` region for the guest, the heap-backed `mmap`
   cannot satisfy it (the log prints the first `MAP_FIXED` miss). The fix is
   probably a virtmem-based reservation with on-demand backing.
2. **Executable mappings.** `PROT_EXEC` is ignored. If the guest CPU is a
   JIT rather than an interpreter, code memory must go through
   `svcMapProcessCodeMemory`-style handling instead.
3. **Signals.** `sigaction`/`sigaltstack` are no-ops. Fastmem-by-fault designs
   need libnx's exception handler wired to the runtime's handler.
4. **Data path.** The runtime is assumed to look for `<internalDataPath>/game`.
   The log shows what it opens; adjust `activity.internalDataPath` if not.
5. **JNI.** The native code calls at least two `GameActivity` methods
   (signatures `(ILjava/lang/String;Ljava/lang/String;[Ljava/lang/String;IILjava/lang/String;)V`
   and `(IILjava/lang/String;)V`, apparently alert/text-input dialogs, with a
   `nativeAlertResult` callback on the Java side). The fake JNI layer logs
   every method by class/name/signature and returns defaults; it does not
   answer dialogs yet.
6. **Movies.** IB3 uses NDK media for video. Whatever the runtime does when the
   extractor is unavailable is unknown.
7. **Memory.** Anonymous mappings are zero-filled eagerly; the ELF arena was
   reduced to 32 MiB to leave more heap for the guest.
8. **Input.** `AKeyEvent_*` return zeros; no key events are queued. The
   inherited controller code synthesizes IB1-style touches.

Leftover IB1-only code (`opensles.c`, `music_player.c`) is inert for IB3 and
was left in place to avoid unverified build changes.

## Log finding (first hardware run)

The run reached `ANativeActivity_onCreate` and the runtime's own start-up, then
faulted with `esr=0x8200000e` (instruction abort, permission fault) at a
page-aligned address inside the process heap, right after the runtime asked
for a `PROT_EXEC` mapping. Cause: `mmap` was served from the heap and
`PROT_EXEC` was ignored, so the first jump into that block faulted.

## Executable mappings (v3)

`mmap(PROT_EXEC)` is now a code-memory alias (see `mmap_exec` in
`source/ib3_shim.c`). It starts RW. Code memory cannot change RW -> RX in
place, so a flip is unmap + map + set-permission over the same backing pages.
Flips are driven by `mprotect()` or, lazily, by the fault handler in
`exception_dump.c` (instruction fetch -> RX, store -> RW). Every executable
`mmap`/`mprotect` is logged as `mmap-exec:` / `mprotect-exec:` with the caller as
an offset into `libib3.so`, and the first 16 flips are logged as `exec-flip:`.

Unverified: nothing here has been built or run. If the runtime toggles
permissions on every compiled block, flips will be slow. A `MAP_FIXED|EXEC`
request inside a plain mapping, or `mprotect(EXEC)` on a plain mapping, cannot
be honoured and is logged.

## Manual exception resume (v4)

Log 2 showed the first executable fault being serviced (`exec-flip ... ok`) and
then a bad-SVC exception (`desc=0x301`, `esr=0x56000028`, i.e. `svc #0x28`,
ReturnFromException). `exception_dump.c` now:

1. Saves the exception dump and calls `svc #0x28` from its own stub
   (`ib3_return_from_exception`).
2. If that comes back as a bad-SVC exception at that stub, logs
   `kernel-resume: svc #0x28 rejected`, and from then on resumes by itself:
   the general registers are copied below the guest stack pointer, reloaded,
   and the thread branches back through the one register that already equals
   the resume address (`manual-resume: pc=... via xN`). SIMD registers are
   never touched.

Limits: only instruction-fetch faults can be resumed this way. A store into a
mapping that has been made executable logs `manual-resume: store fault cannot
be retried` with the instruction word at the faulting pc, then panics. That
line is the next thing to read. The assembly is generated for x0..x30 and has
not been assembled anywhere (no AArch64 toolchain was available).

## Store faults and chunked code cache (v5)

Log 3: kernel resume rejected (`svc #0x28` bad-SVC), manual resume of the first
instruction fetch worked (`via x20`), then a 4-byte store into the freshly
flipped cache faulted (`instr 0xb9000269` = `str w9, [x19]`, inside
`libib3.so`). Changes:

* The code cache is mapped and flipped in 64 KiB chunks, so writing new code
  no longer makes the whole 8 MiB cache non-executable and vice versa.
* `so_load` reserves 16 KiB of address space right after `libib3.so`
  (`stub_virtbase`). `ib3_store_stub()` (ib3_shim.c) puts one 16-byte stub per
  faulting store site there: `ldur x17,[sp,#-24]` / copy of the store /
  `b pc+4` / `nop`. The exception handler restores every register from the
  dump, jumps to the stub through x17, and the stub finishes the job. New log
  lines: `stub:`, `store-stub:`, `exec-flip #n` (first 16, then powers of two).
* Stores from outside `libib3.so` (or beyond +/-128 MiB of the stub area)
  cannot be stubbed and log `no stub for store at pc=...`.

## Log 4 and diagnostics (v7)

Log 4 (v6): the guest image is now executable and native guest code runs
(`exec-flip ... base=0x100000000`, manual resume via x20/x16 works, objc setup
and the analytics-class stubbing complete). It then dies with `desc=0x100
esr=0x02000000`: EC=0 is an *undefined instruction* at `pc` inside the runtime's
own code cache (chunk 0, offset 0x1108, called from guest text). Open
questions, answered by the v7 log lines:

* `words pc=...` / `words lr=...`: the actual instruction words at the fault and
  at the caller. All zero = an unwritten thunk; a small `0x0000xxxx` value = a
  deliberate `udf #n` trap that the runtime expects a signal handler to catch;
  anything else = an instruction the Cortex-A57 lacks (e.g. LSE atomics).
* `signal: sigaction(n) flags=... handler=...`: every registration is logged now.
  If SIGILL (4) / SIGTRAP (5) / SIGSEGV (11) handlers are registered, the runtime
  probably depends on signal delivery, which Horizon does not have.
* `regs ...`: x0..x30 at the crash.

## Signal delivery ucontext (v7.3 fix for black-screen loop)

Hardware log after analytics stubbing:

```
signal: deliver sig=4 handler=0x1c8963450 fault=0x81e0a1108 pc=0x81e0a1108
signal: deliver sig=11 handler=0x1c8963450 fault=0x1b8 pc=0x1c89634a0
(… infinite SIGSEGV cascade, black screen, no panic)
```

Root cause: `ib3_deliver_signal` called SA_SIGINFO handlers with a NULL
`ucontext`. The libib3 SIGILL handler reads `uc_mcontext.pc` (bionic aarch64
offset `0x1B8`), faults, and the exception path re-delivers SIGSEGV forever.

Fix in `source/ib3_shim.c` / `exception_dump.c`:

1. Build a full bionic/Linux aarch64 `ucontext` (pc at `0x1B8`) from the
   `ThreadExceptionDump` GPRs and pass it as the third argument.
2. After the handler returns, honour any PC write-back via `*out_pc`.
3. Block nested signal delivery (`g_in_signal_delivery`) so a handler fault
   cannot recurse.

Also ship a default `settings.ini` next to the NRO; the runtime repeatedly
opens `sdmc:/switch/infinityblade3_nx/settings.ini` (non-fatal if missing).
