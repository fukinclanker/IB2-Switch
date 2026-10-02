# IB3 Switch — Stability Pass (1.0.2-ib3-stable)

This tree includes targeted fixes for unstable audio, display tearing,
`std::bad_alloc` during heavy texture loads, and an early-touch null deref
during bootstrap.

## 1.0.2 — early touch crash

**Symptom:** Log ends with `touch: DOWN ...` then
`exception-raw ... far=0x24` / `crash: ec=0x24` (data abort on a near-null
object; registers show `x0=0x20`). Happens if the screen is touched before
the first frame is presented.

**Fix:** `poll_touchscreen` drops all input until `ib3_present_count() > 0`
(first successful `eglSwapBuffers`). Expected log line:
`touch: gated until first present (engine not ready)`.

## Changes (1.0.1)

### 1. Unified audio (single SDL device)

**Problem:** The iOS AUGraph path opened an AAudio→SDL queue device while
OpenSL/OpenAL used a second callback device. Two consumers on Horizon’s
audio output caused random underruns and “audio randomly bugs out”.

**Fix:**
- AAudio no longer opens its own `SDL_OpenAudioDevice`.
- Writes go into a shared S16 stereo ring (`opensles_aaudio_*`) mixed on the
  same OpenSLES callback thread as OpenAL players and movie music.
- `AAudioStream_write` is non-blocking (no `svcSleepThread` on the game
  thread). Full ring → short write; producer can retry.
- Device buffer reduced to 512 frames (~10.7 ms @ 48 kHz); AAudio ring is
  16 384 frames to absorb bursts.

**Log markers you should see:**
```
opensles: SDL device opened: ... (shared OpenSL+AAudio)
AAudio: opened shared mixer 48000 Hz, 2 ch, ... (no 2nd device)
opensles: AAudio side-bus started (ring=16384 frames @ 48000 Hz)
```

### 2. Forced vsync + docked/handheld resolution

**Problem:** Surface was fixed at 1280×720 while docked presents often ran
at 1920×1080; `eglSwapInterval(0)` was allowed → tearing.

**Fix:**
- `eglSwapInterval` always forces interval ≥ 1.
- First `eglSwapBuffers` also forces vsync if the game never called it.
- On bootstrap and every ~60 presents, `ib3_update_display_mode()` sets
  1280×720 (handheld) or 1920×1080 (docked) on `screen_*` and the NWindow.

**Log markers:**
```
NativeActivity: operation mode docked -> 1920x1080
EGL: eglSwapInterval(... interval=1) -> 1 (forced vsync)
display: mode=handheld -> 1280x720
```

### 3. Memory headroom for texture streaming

**Problem:** After long sessions / boss fights the guest hit
`std::bad_alloc` while streaming `CharTextures.tfc` and package files.
Sparse code-memory commits also failed with `0xd401` under pressure.

**Fix:**
- SO arena reduced 32 → **24 MiB** so more Horizon heap stays with newlib /
  the guest allocator.
- Sparse commit window reduced 16 → **8 MiB**.
- `commit_range` retries with 4 MiB then 2 MiB on failure before giving up.

## Rebuild

```bash
cd IB3-Switch-main
make clean
make -j
```

Deploy the new `.nro` over your existing runtime folder on the SD card
(`sd:/switch/infinityblade3_nx/`). Assets and `libib3.so` are unchanged.

## What this does *not* claim

- Startup video (`Startup.m4v`) is still missing / not decoded; that path
  was already non-fatal.
- NDK media decode remains unimplemented.
- Extreme docked 1080p scenes may still hitch if the guest over-allocates;
  the retry path and extra heap reduce the abort rate but do not invent RAM.

If audio still glitches, check the log for rising `opensles` callback counts
via any heartbeat that calls `nx_audio_stats()`. Flat callbacks mean the
device stopped; rising callbacks with peak 0 mean the mixer is silent
(game-side), not a driver underrun.


## 1.0.1-ib2 — AAudio rework (audio fix pass)

**Symptom:** no audio at all.

**Changes (source/ib3_shim.c, source/opensles.c):**
- The AAudio surface is now complete (builder setters, state/format/channel
  getters, requestStop/Pause/Flush, waitForStateChange, getTimestamp, result
  and state text, ...). The runtime also resolves AAudio through `dlsym`;
  before, only 13 entry points existed, and a guest that finds one optional
  entry point missing can quietly disable its audio backend.
- Data-callback mode (`setDataCallback`) is supported through a pump thread
  that runs the guest callback with bionic TLS.
- Error codes fixed: "unavailable" was -899 (that is DISCONNECTED, which makes
  callers reopen the stream); it is now -889, with the other codes from the NDK.
- One ring per stream (was one shared ring): closing a stream no longer
  silences the others and two streams no longer interleave their PCM.
- Float and mono/multichannel PCM are converted correctly (the float path
  assumed stereo and NaNs were not guarded).
- Diagnostics: the log now prints each stream's format/channels/mode, and for
  the first three writes and every ~5 s afterwards:
  `AAudio[write]: written=.. input peak=.. queued=.. mixer callbacks=.. mixer peak=..`
    - input peak 0            -> the guest is sending silence
    - input peak > 0, mixer callbacks flat -> SDL device not pulling
    - input peak > 0, mixer peak 0, callbacks rising -> bug in our mixing
  If a guest `dlsym`s an AAudio function that is still not provided you will
  see `dlsym: unresolved <name>` in the log.

## 1.0.1-ib2-audio — early device + float default (no-audio follow-up)

**Symptom:** still no audio on some boots / first-run after the AAudio surface
was completed.

**Changes:**
- `main.c` / `run_loader_audit`: call `opensles_ensure_shared_device()`
  immediately after libib3.so constructors. Removes the race where the guest's
  first `AAudioStreamBuilder_openStream` (or `slCreateEngine`) hit a cold
  `SDL_InitSubSystem` and received `AAUDIO_ERROR_UNAVAILABLE`, after which the
  runtime permanently disables its audio backend.
- `AAudio_createStreamBuilder`: default format is now `AAUDIO_FORMAT_PCM_FLOAT`
  (matches the Android host mixer in `src/audio/mixer.cpp`). A guest that
  forgets `setFormat` still gets the format the runtime was built against.
- `AAudioStream_write`: timeout capped at the same 100 ms the Android host
  uses; if the SDL device is not yet ready, a short 20 ms retry window is
  given instead of returning 0 immediately (which also caused the guest to
  give up on audio).

Rebuild, deploy the new `.nro`, and check the log for:
```
stage: shared audio device ready early
opensles: SDL device opened: ... (shared OpenSL+AAudio)
AAudio: opened stream fmt=float ...
AAudio[write]: ... input peak=...
```

## 1.0.2-ib2-audio — soft-null for audio thread crash

**Log evidence (infinityblade2_nx.log):**
- Shared SDL device opens correctly; AAudio stream opens as float stereo.
- Early AAudio[write] lines show `input peak=0` (guest sending silence while
  0 mixer inputs).
- ~34 s in, game configures real AUGraph (RemoteIO + 3D Mixer, 64 inputs,
  sampleRate 44100). Immediately the audio thread dies:
  `CRASH: signal 11 at ... (address 0xffffffff)` with FAR=0xffffffff.
- After that, no further non-silent audio is possible.

**Fix (exception_dump.c):**
- On data abort with FAR == 0 / 0xffffffff / high canonical null, decode a
  simple LDR and zero the destination register, then advance PC by 4
  ("soft-null"). This keeps the guest audio thread alive past the bad
  pointer instead of letting its SIGSEGV handler kill the thread.
- Expected new log line: `soft-null: far=0xffffffff skipped load -> xN`

Rebuild and re-test. If peak stays 0 after the soft-null, the next place to
look is why the 3D Mixer bus callbacks produce silence (TLS / rate / gain).

## 1.0.3-ib2-audio — unconditional soft-null

Previous soft-null required a matching LDR encoding and missed the actual
faulting instruction. The audio thread still died with FAR=0xffffffff and
no soft-null log line.

Now any data abort with FAR in {0, 0xffffffff, high-null} unconditionally:
- zeros x0 (the bad pointer in the observed crash)
- advances PC by 4
- logs `soft-null: far=... skip pc+4 x0=0 instr=0x...`

This keeps the audio thread alive past the AUGraph sample-rate setup crash.

## 1.0.4-ib2-audio — FAR match fix

Soft-null never fired: the fault address is `0x00000000ffffffff`
(32-bit style -1), but the check only tested `(uint64_t)-1`
(`0xffffffffffffffff`). Added an explicit match for `0xffffffffull`.

## 1.0.5-ib2-audio — retarget InfinityBladeII-Android-1.6.1

- Expected libib3.so size updated 4406368 → **4444616** (1.6.1 APK).
- prepare_runtime.py accepts the 1.6.1 size/hash (1.5 still accepted).
- Added fortify wrappers `__pread_chk` / `__pwrite_chk` required by the
  1.6.1 binary (were the only two missing imports).
- Soft-null FAR match for 0xffffffff (from 1.0.4) retained.

## 1.0.6-ib2-audio — auto-enable mixer buses

**Problem:** AAudio wrote continuously with `input peak=0`. Inside libib3.so,
`mix_buses` skips any bus that is not `enabled`. Buses default to disabled
and are only enabled via `AudioUnitSetParameter(param=5)`. Callbacks were
registered (`AUGraphStart (64 mixer inputs)`) but enable never stuck, so the
mixer mixed pure silence.

**Fix:** `prepare_runtime.py` patches 1.6.1 `libib3.so` so `mixer_set_callback`
also sets `enabled=1` (byte at Bus+0x10). Re-run prepare_runtime on the 1.6.1
APK to get a patched libib3.so in the runtime folder.
