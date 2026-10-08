#ifndef INFINITY_BLADE2_NX_CONFIG_H
#define INFINITY_BLADE2_NX_CONFIG_H

#define LOG_NAME "infinityblade2_nx.log"
#define APPSTATE_NAME "SaveData/appstate.txt"

/* GPU clock the game runs at, in Hz. 0 = leave the system default alone. */
#define GPU_CLOCK_HZ 900000000u

/* ---- Audio output (1.0.8/1.0.9, see STABLE.md) ----------------------------
 * Frames per hardware wave buffer. The native audren output keeps 4 of them
 * queued, so the cushion is 4 * AUDIO_DEVICE_SAMPLES (512 -> ~43 ms) with a
 * refill every ~10.7 ms. (Only used as "samples" by the SDL fallback path,
 * which has just 2 buffers.) */
#define AUDIO_DEVICE_SAMPLES 512

/* Horizon priorities: lower number = higher priority. libnx creates every
 * pthread (including SDL's audio thread and all guest threads) at 0x3B, the
 * LOWEST application priority, on the process' default core. SDL maps
 * TIME_CRITICAL to 0x3B too. The mixer thread mostly sleeps in
 * audrenWaitFrame(), so running it above everything costs almost nothing. */
#define AUDIO_MIXER_PRIORITY   0x2A /* SDL device callback thread */
#define AUDIO_PRODUCER_PRIORITY 0x2B /* guest thread calling AAudioStream_write */

/* Jitter buffer for AAudio rings: after an underrun (or at stream start) a
 * ring waits until it holds this many frames before it plays again, so a late
 * producer causes one short gap instead of continuous crackle. */
#define AUDIO_PRIME_FRAMES 1536

/* 1 = let guest threads run on every application core (0..2) instead of all
 * sharing the default core. Big CPU win, but it changes timing for the whole
 * runtime, so it is off until tested on hardware. Audio threads are moved off
 * the default core regardless of this setting. */
#define SPREAD_GUEST_THREADS 0

extern int screen_width;
extern int screen_height;

#endif
