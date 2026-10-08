/* opensles.c -- minimal OpenSL ES shim backed by SDL2 audio
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * Implements the slice of OpenSL ES 1.0.1 the bundled OpenAL backend uses:
 * the Object interface (Realize/GetInterface/Destroy), the Engine interface
 * (CreateOutputMix/CreateAudioPlayer), and on each player the Play, Volume and
 * AndroidSimpleBufferQueue interfaces. Players are software-mixed into one SDL2
 * audio device; the buffer-queue completion callback is fired from the SDL
 * audio thread, exactly like Android's fast-track callback.
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#include <SDL2/SDL.h>
#ifdef __SWITCH__
#include <malloc.h>
#include <switch.h>
#endif

#include "opensles.h"
#include "util.h"
#include "config.h"

// --- OpenSL ES constants ----------------------------------------------------

/* Values from OpenSLES.h. PARAMETER_INVALID was 0x0D (that is INTERNAL_ERROR);
 * the real value is 0x02. A full buffer queue is BUFFER_INSUFFICIENT, not a
 * parameter error. */
#define SL_RESULT_SUCCESS              0
#define SL_RESULT_PARAMETER_INVALID    0x02
#define SL_RESULT_MEMORY_FAILURE       0x03
#define SL_RESULT_RESOURCE_ERROR       0x04
#define SL_RESULT_BUFFER_INSUFFICIENT  0x07
#define SL_RESULT_FEATURE_UNSUPPORTED  0x0C

#define SL_BOOLEAN_FALSE 0
#define SL_BOOLEAN_TRUE  1

#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED  2
#define SL_PLAYSTATE_PLAYING 3

#define SL_OBJECT_STATE_REALIZED 2

typedef uint32_t SLuint32;
typedef int32_t  SLint32;
typedef uint16_t SLuint16;
typedef int16_t  SLint16;
typedef uint8_t  SLuint8;
typedef uint32_t SLresult;
typedef uint32_t SLboolean;
typedef int32_t  SLmillibel;

// PCM data format (samplesPerSec is in milliHz per the spec)
typedef struct {
  SLuint32 formatType;
  SLuint32 numChannels;
  SLuint32 samplesPerSec;
  SLuint32 bitsPerSample;
  SLuint32 containerSize;
  SLuint32 channelMask;
  SLuint32 endianness;
} SLDataFormat_PCM;

typedef struct {
  SLuint32 locatorType;
  SLuint32 numBuffers;
} SLDataLocator_BufferQueue;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSource;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSink;

typedef void *SLObjectItf;       // -> &obj->obj_vt
typedef void *SLInterfaceID;

// callback: (SLAndroidSimpleBufferQueueItf caller, void *pContext)
typedef void (*slBufferQueueCallback)(void *caller, void *context);

// --- interface-id sentinels -------------------------------------------------

#define DEF_IID(n) void *SL_IID_##n = &SL_IID_##n
DEF_IID(3DCOMMIT); DEF_IID(3DDOPPLER); DEF_IID(3DGROUPING); DEF_IID(3DLOCATION);
DEF_IID(3DMACROSCOPIC); DEF_IID(3DSOURCE); DEF_IID(ANDROIDCONFIGURATION);
DEF_IID(ANDROIDEFFECT); DEF_IID(ANDROIDEFFECTCAPABILITIES); DEF_IID(ANDROIDEFFECTSEND);
DEF_IID(ANDROIDSIMPLEBUFFERQUEUE); DEF_IID(AUDIODECODERCAPABILITIES); DEF_IID(AUDIOENCODER);
DEF_IID(AUDIOENCODERCAPABILITIES); DEF_IID(AUDIOIODEVICECAPABILITIES); DEF_IID(BASSBOOST);
DEF_IID(BUFFERQUEUE); DEF_IID(DEVICEVOLUME); DEF_IID(DYNAMICINTERFACEMANAGEMENT);
DEF_IID(DYNAMICSOURCE); DEF_IID(EFFECTSEND); DEF_IID(ENGINE); DEF_IID(ENGINECAPABILITIES);
DEF_IID(ENVIRONMENTALREVERB); DEF_IID(EQUALIZER); DEF_IID(LED); DEF_IID(METADATAEXTRACTION);
DEF_IID(METADATATRAVERSAL); DEF_IID(MIDIMESSAGE); DEF_IID(MIDIMUTESOLO); DEF_IID(MIDITEMPO);
DEF_IID(MIDITIME); DEF_IID(MUTESOLO); DEF_IID(NULL); DEF_IID(OBJECT); DEF_IID(OUTPUTMIX);
DEF_IID(PITCH); DEF_IID(PLAY); DEF_IID(PLAYBACKRATE); DEF_IID(PREFETCHSTATUS);
DEF_IID(PRESETREVERB); DEF_IID(RATEPITCH); DEF_IID(RECORD); DEF_IID(SEEK); DEF_IID(THREADSYNC);
DEF_IID(VIBRA); DEF_IID(VIRTUALIZER); DEF_IID(VISUALIZATION); DEF_IID(VOLUME);
#undef DEF_IID

// --- vtable structs (method order matches the OpenSL ES 1.0.1 spec) ---------

typedef struct {
  SLresult (*Realize)(void *self, SLboolean async);
  SLresult (*Resume)(void *self, SLboolean async);
  SLresult (*GetState)(void *self, SLuint32 *pState);
  SLresult (*GetInterface)(void *self, const SLInterfaceID iid, void *pInterface);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  SLresult (*AbortAsyncOperation)(void *self);
  void     (*Destroy)(void *self);
  SLresult (*SetPriority)(void *self, SLint32 priority, SLboolean preemptable);
  SLresult (*GetPriority)(void *self, SLint32 *pPriority);
  SLresult (*SetLossOfControlInterfaces)(void *self, SLint32 n, SLInterfaceID *ids, SLboolean enabled);
} SLObjectItf_;

// only CreateAudioPlayer (slot 2) and CreateOutputMix (slot 7) are used; the
// rest keep the correct layout but are generic so a shared stub assigns
// cleanly. The engine calls each slot with its own typed vtable.
typedef struct {
  void *CreateLEDDevice;
  void *CreateVibraDevice;
  SLresult (*CreateAudioPlayer)(void *self, SLObjectItf *pPlayer, SLDataSource *src, SLDataSink *snk,
                                SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req);
  void *CreateAudioRecorder;
  void *CreateMidiPlayer;
  void *CreateListener;
  void *Create3DGroup;
  SLresult (*CreateOutputMix)(void *self, SLObjectItf *pMix, SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req);
  void *CreateMetadataExtractor;
  void *CreateExtensionObject;
  void *QueryNumSupportedInterfaces;
  void *QuerySupportedInterfaces;
  void *QueryNumSupportedExtensions;
  void *QuerySupportedExtension;
  void *IsExtensionSupported;
} SLEngineItf_;

typedef struct {
  SLresult (*SetPlayState)(void *self, SLuint32 state);
  SLresult (*GetPlayState)(void *self, SLuint32 *pState);
  SLresult (*GetDuration)(void *self, SLuint32 *pMsec);
  SLresult (*GetPosition)(void *self, SLuint32 *pMsec);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  SLresult (*SetCallbackEventsMask)(void *self, SLuint32 mask);
  SLresult (*GetCallbackEventsMask)(void *self, SLuint32 *pMask);
  SLresult (*SetMarkerPosition)(void *self, SLuint32 m);
  SLresult (*ClearMarkerPosition)(void *self);
  SLresult (*GetMarkerPosition)(void *self, SLuint32 *p);
  SLresult (*SetPositionUpdatePeriod)(void *self, SLuint32 m);
  SLresult (*GetPositionUpdatePeriod)(void *self, SLuint32 *p);
} SLPlayItf_;

typedef struct {
  SLresult (*Enqueue)(void *self, const void *pBuffer, SLuint32 size);
  SLresult (*Clear)(void *self);
  SLresult (*GetState)(void *self, void *pState);
  SLresult (*RegisterCallback)(void *self, slBufferQueueCallback cb, void *ctx);
} SLBufferQueueItf_;

typedef struct {
  SLresult (*SetVolumeLevel)(void *self, SLmillibel level);
  SLresult (*GetVolumeLevel)(void *self, SLmillibel *p);
  SLresult (*GetMaxVolumeLevel)(void *self, SLmillibel *p);
  SLresult (*SetMute)(void *self, SLboolean mute);
  SLresult (*GetMute)(void *self, SLboolean *p);
  SLresult (*EnableStereoPosition)(void *self, SLboolean enable);
  SLresult (*IsEnabledStereoPosition)(void *self, SLboolean *p);
  SLresult (*SetStereoPosition)(void *self, SLint32 perMille);
  SLresult (*GetStereoPosition)(void *self, SLint32 *p);
} SLVolumeItf_;

// Some OpenSL clients request playback-rate control. We don't resample through
// this interface, so SetRate is accepted but ignored; the method order matters.
typedef struct {
  SLresult (*SetRate)(void *self, SLint16 rate);
  SLresult (*GetRate)(void *self, SLint16 *p);
  SLresult (*SetPropertyConstraints)(void *self, SLuint32 c);
  SLresult (*GetProperties)(void *self, SLuint32 *p);
  SLresult (*GetCapabilitiesOfRate)(void *self, SLuint32 *p);
  SLresult (*GetRateRange)(void *self, SLuint8 i, SLint16 *min, SLint16 *max, SLint16 *step, SLuint32 *prop);
} SLPlaybackRateItf_;

// OpenAL requests this interface before Realize to set the Android stream type.
// It must be obtainable even though the stream-type hint itself can be ignored.
typedef struct {
  SLresult (*SetConfiguration)(void *self, const void *key, const void *value, SLuint32 valueSize);
  SLresult (*GetConfiguration)(void *self, const void *key, SLuint32 *pValueSize, void *value);
  SLresult (*AcquireJavaProxy)(void *self, SLuint32 proxyType, void *pProxyObj);
  SLresult (*ReleaseJavaProxy)(void *self, SLuint32 proxyType);
} SLAndroidConfigurationItf_;

// --- objects ----------------------------------------------------------------

#define MAX_PLAYERS 64
// Keep enough queue space for an audio backend to enqueue several periods before
// the first callback drains one. OpenAL currently uses a small queue, but a
// larger ring is cheap and prevents an avoidable initialization failure.
#define BQ_SLOTS 256

typedef struct {
  const void *data;
  SLuint32 size;
} BQBuffer;

typedef struct Player {
  const SLObjectItf_ *obj_vt;
  const SLPlayItf_   *play_vt;
  const SLBufferQueueItf_ *bq_vt;
  const SLVolumeItf_ *vol_vt;
  const SLPlaybackRateItf_ *rate_vt;
  const SLAndroidConfigurationItf_ *config_vt;

  int in_use;
  int channels;
  int rate;
  int sbytes;    // bytes per sample in the enqueued buffers (2=16-bit, 4=32-bit)
  int is_float;  // 1 if samples are 32-bit float, 0 if signed integer
  int playing;
  int drained;   // consecutive callbacks this playing player produced no audio
  float gain; // linear, from SetVolumeLevel (millibels)
  int muted;  // SetMute state; kept separate so un-muting restores `gain`
  SLmillibel level_mb; // last SetVolumeLevel value, returned by GetVolumeLevel

  slBufferQueueCallback cb;
  void *cb_ctx;

  // FIFO of enqueued buffers
  BQBuffer q[BQ_SLOTS];
  int q_head, q_tail; // count = (tail - head + N) % N
  // currently draining buffer
  const uint8_t *cur;
  SLuint32 cur_size, cur_pos;
  double cur_fpos; // fractional sample index into cur (for rate conversion)

  SDL_mutex *lock;
} Player;

typedef struct {
  const SLObjectItf_ *obj_vt;
} OutputMix;

typedef struct {
  const SLObjectItf_ *obj_vt;
  const SLEngineItf_ *eng_vt;
} Engine;

#define CONTAINER(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))

// --- global SDL device + player registry ------------------------------------

static SDL_AudioDeviceID g_dev = 0;
static volatile int g_dev_ready = 0; /* set last, after g_dev_rate is known and the device is unpaused */
static int g_dev_rate = 48000;
static Player *g_players[MAX_PLAYERS];
static int g_player_count = 0;
static SDL_mutex *g_reg_lock = NULL;

#define MOVIE_RING_FRAMES 65536
static SDL_mutex *g_movie_lock = NULL;
static int16_t *g_movie_pcm = NULL;
static int g_movie_active = 0;
static int g_movie_paused = 0;
static int g_movie_head = 0;
static int g_movie_count = 0;
static int g_movie_rate = 44100;
static double g_movie_fpos = 0.0;
static uint64_t g_movie_samples_queued = 0;
static uint64_t g_movie_samples_played = 0;

/* AAudio / AUGraph side-bus (iOS Core Audio path). Every open AAudio stream
 * owns its own S16-stereo ring; all rings are mixed on the single shared SDL
 * device so we never open two competing audio outputs on Horizon. Per-stream
 * rings matter: one shared ring interleaved the PCM of two streams (garbled /
 * double-speed audio) and closing one stream silenced the others. */
#define AAUDIO_RING_FRAMES 16384
#define AAUDIO_MAX_RINGS 8
struct AARing {
  int16_t *pcm;      /* AAUDIO_RING_FRAMES * 2 interleaved S16 */
  int head, count;
  int active;
  int in_use;
  int rate;          /* the stream's own sample rate; 0 = device rate */
  double fpos;       /* fractional read position (source frames) when resampling */
  uint64_t written;  /* frames accepted over the ring's lifetime */
  int32_t peak_in;   /* loudest input sample (S16 scale) since the last stats read */
  float float_peak;  /* max |float| sample seen since last stats (diagnose guests
                      * that send non-zero floats outside [-1,1] or near-zero) */
  /* Underrun handling (1.0.8). A ring that runs dry used to be played again
   * the moment any frames arrived, so a producer that was slightly late every
   * period produced continuous crackle (play 512, gap, play 512, gap...), and
   * every gap started with a hard cut to zero (a click). Now it ramps out,
   * waits until AUDIO_PRIME_FRAMES are buffered, then ramps back in. */
  int priming;       /* 1 = holding output until the ring has refilled */
  int prime_last;    /* ring->count seen on the previous callback while priming */
  int prime_stalls;  /* callbacks the count did not move while priming */
  int playing;       /* produced audio on the previous callback */
  int fade_in;       /* frames of fade-in remaining */
  int32_t last_l, last_r; /* last frame actually output (fade-out start) */
  uint32_t xruns;    /* underruns while playing (AAudioStream_getXRunCount) */
};
#define AAUDIO_DECLICK_FRAMES 64
static SDL_mutex *g_aaudio_lock = NULL;
static AARing g_aaudio_rings[AAUDIO_MAX_RINGS];

static float mb_to_linear(SLmillibel mb) {
  if (mb <= -9600) return 0.0f;
  return powf(10.0f, (float)mb / 2000.0f); // 100 mB = 1 dB
}

// Read one sample (at sample-index k within the buffer) and return it scaled to
// signed-16-bit range, regardless of the source format. The shim's accumulator
// and SDL device are S16; an OpenSL output player can be 16-bit int, 32-bit
// int, or 32-bit float, so normalise here.
static inline int32_t read_sample_s16(const void *buf, long k, int sbytes, int is_float) {
  if (is_float) {
    float f = ((const float *)buf)[k];
    if (f != f) f = 0.0f;  // NaN -> silence (the cast below is UB for NaN)
    if (f > 1.0f) f = 1.0f; else if (f < -1.0f) f = -1.0f;
    return (int32_t)(f * 32767.0f);
  }
  if (sbytes == 4)
    return ((const int32_t *)buf)[k] >> 16;   // S32 -> S16 range
  return (int32_t)((const int16_t *)buf)[k];  // S16
}

// mix one playing player into the S16 stereo accumulator (int32 to avoid clip).
// cur/cur_pos are touched only by this (audio) thread, so they need no lock;
// only the buffer queue is shared with Enqueue. Critically, the engine's
// completion callback is fired WITHOUT our lock held -- Android's contract --
// otherwise the engine's mixer thread (holding its own mutex, calling Enqueue
// which wants our lock) deadlocks against us.
static void mix_player(Player *p, int32_t *acc, int frames) {
  if (!p->playing)
    return;

  // A playing player with nothing queued is a finished one-shot SE the engine
  // fired and never Destroy'd; count the dry callbacks so alloc can recycle it.
  SDL_LockMutex(p->lock);
  const int dry = (!p->cur) && (p->q_head == p->q_tail);
  SDL_UnlockMutex(p->lock);
  if (dry) { if (p->drained < (1 << 20)) p->drained++; return; }
  p->drained = 0;

  const float g = p->muted ? 0.0f : p->gain;
  const int stereo = (p->channels >= 2);
  const int sbytes = p->sbytes > 0 ? p->sbytes : 2;    // bytes per sample
  const int is_float = p->is_float;
  const int bps = stereo ? sbytes * 2 : sbytes;        // bytes per input frame
  // resample the player's own rate to the device rate (players come in at 22050
  // AND 44100; without this, off-rate voices play at the wrong speed/pitch).
  const double ratio = g_dev_rate > 0 ? (double)p->rate / (double)g_dev_rate : 1.0;

  for (int i = 0; i < frames; i++) {
    // ensure cur holds a buffer whose integer sample index covers cur_fpos,
    // carrying the fractional remainder across buffer boundaries.
    for (;;) {
      if (!p->cur) {
        SDL_LockMutex(p->lock);
        const int have = (p->q_head != p->q_tail);
        BQBuffer b = { NULL, 0 };
        if (have) {
          b = p->q[p->q_head];
          p->q_head = (p->q_head + 1) % BQ_SLOTS;
        }
        SDL_UnlockMutex(p->lock);
        if (!have)
          return; // underrun: rest of the block stays silent
        p->cur = b.data;
        p->cur_size = b.size;
      }
      const long n = (long)(p->cur_size / (SLuint32)bps);
      if (n > 0 && (long)p->cur_fpos < n)
        break; // position is inside the current buffer
      // buffer consumed (or empty): carry remainder, notify engine, fetch next
      p->cur_fpos -= (double)n;
      if (p->cur_fpos < 0.0) p->cur_fpos = 0.0;
      p->cur = NULL;
      if (p->cb) {
        static int once = 0;
        if (!once) { once = 1; debugPrintf("opensles: buffer-queue callback firing; OpenAL is producing PCM\n"); }
        p->cb(&p->bq_vt, p->cb_ctx);
      }
    }

    const long n = (long)(p->cur_size / (SLuint32)bps);
    const long idx = (long)p->cur_fpos;
    const double frac = p->cur_fpos - (double)idx;
    const void *s = p->cur;
    int32_t l, r;
    if (stereo) {
      const long j0 = idx * 2, j1 = (idx + 1 < n ? idx + 1 : idx) * 2;
      const int32_t l0 = read_sample_s16(s, j0,     sbytes, is_float);
      const int32_t l1 = read_sample_s16(s, j1,     sbytes, is_float);
      const int32_t r0 = read_sample_s16(s, j0 + 1, sbytes, is_float);
      const int32_t r1 = read_sample_s16(s, j1 + 1, sbytes, is_float);
      l = (int32_t)(l0 * (1.0 - frac) + l1 * frac);
      r = (int32_t)(r0 * (1.0 - frac) + r1 * frac);
    } else {
      const int32_t a  = read_sample_s16(s, idx, sbytes, is_float);
      const int32_t b2 = (idx + 1 < n) ? read_sample_s16(s, idx + 1, sbytes, is_float) : a;
      l = r = (int32_t)(a * (1.0 - frac) + b2 * frac);
    }
    acc[i * 2 + 0] += (int32_t)(l * g);
    acc[i * 2 + 1] += (int32_t)(r * g);
    p->cur_fpos += ratio;
  }
}

/* Audio liveness, read by the heartbeat (round 164). */
static volatile unsigned g_cb_count, g_cb_peak;

static void mix_movie(int32_t *acc, int frames) {
  if (!g_movie_lock)
    return;

  SDL_LockMutex(g_movie_lock);
  if (!g_movie_active || g_movie_paused || !g_movie_pcm) {
    SDL_UnlockMutex(g_movie_lock);
    return;
  }

  // mpg123 normally produces the soundtrack at 44.1 kHz while the Switch SDL
  // device runs at 48 kHz. Interpolate in source-frame space so music retains
  // its intended speed/pitch and does not drift against 48 kHz game audio.
  const double ratio = g_dev_rate > 0
                           ? (double)g_movie_rate / (double)g_dev_rate
                           : 1.0;
  int mixed = 0;
  while (mixed < frames) {
    const int source_frame = (int)g_movie_fpos;
    if (source_frame >= g_movie_count)
      break;

    const double fraction = g_movie_fpos - (double)source_frame;
    const int next_frame = source_frame + 1 < g_movie_count
                               ? source_frame + 1
                               : source_frame;
    const int index0 = (g_movie_head + source_frame) % MOVIE_RING_FRAMES;
    const int index1 = (g_movie_head + next_frame) % MOVIE_RING_FRAMES;
    for (int channel = 0; channel < 2; channel++) {
      const int32_t sample0 = g_movie_pcm[index0 * 2 + channel];
      const int32_t sample1 = g_movie_pcm[index1 * 2 + channel];
      acc[mixed * 2 + channel] +=
          (int32_t)(sample0 * (1.0 - fraction) + sample1 * fraction);
    }
    g_movie_fpos += ratio;
    mixed++;
  }

  int consumed = (int)g_movie_fpos;
  if (consumed > g_movie_count)
    consumed = g_movie_count;
  g_movie_head = (g_movie_head + consumed) % MOVIE_RING_FRAMES;
  g_movie_count -= consumed;
  g_movie_fpos -= (double)consumed;
  g_movie_samples_played += (uint64_t)consumed;
  SDL_UnlockMutex(g_movie_lock);
}

/* Drain every active AAudio ring into the mix accumulator. Each ring holds
 * S16 stereo at the stream's own rate (ring->rate; 0 means the device rate)
 * and is linearly resampled to the device rate (ratio 1.0 = plain copy).
 *
 * Underruns (1.0.8): when a ring runs dry mid-block the output ramps from the
 * last frame to zero over AAUDIO_DECLICK_FRAMES instead of cutting, the xrun is
 * counted, and the ring re-primes: it stays silent until AUDIO_PRIME_FRAMES are
 * buffered, then fades back in. If the producer stops feeding while a ring is
 * priming (end of a sound, a stream being torn down), whatever is left is
 * played after a few callbacks so short tails are never swallowed. */
static void mix_aaudio(int32_t *acc, int frames) {
  if (!g_aaudio_lock)
    return;
  const int dev_rate = g_dev_rate > 0 ? g_dev_rate : 48000;
  SDL_LockMutex(g_aaudio_lock);
  for (int r = 0; r < AAUDIO_MAX_RINGS; r++) {
    AARing *ring = &g_aaudio_rings[r];
    if (!ring->in_use || !ring->active || !ring->pcm)
      continue;

    if (ring->priming) {
      int prime = AUDIO_PRIME_FRAMES;
      if (prime > AAUDIO_RING_FRAMES / 2)
        prime = AAUDIO_RING_FRAMES / 2;
      if (ring->count >= prime) {
        ring->priming = 0;
      } else if (ring->count > 0 && ring->count == ring->prime_last) {
        if (++ring->prime_stalls >= 3)
          ring->priming = 0; /* producer went quiet: play the remainder */
      } else {
        ring->prime_stalls = 0;
      }
      ring->prime_last = ring->count;
      if (ring->priming)
        continue;
      ring->prime_stalls = 0;
      ring->fade_in = AAUDIO_DECLICK_FRAMES;
    }

    const double ratio =
        (ring->rate <= 0 || ring->rate == dev_rate)
            ? 1.0
            : (double)ring->rate / (double)dev_rate;
    int mixed = 0;
    while (mixed < frames) {
      const int s0 = (int)ring->fpos;
      if (s0 >= ring->count)
        break;
      const int i0 = (ring->head + s0) % AAUDIO_RING_FRAMES;
      int32_t l = ring->pcm[i0 * 2 + 0];
      int32_t rr = ring->pcm[i0 * 2 + 1];
      const double frac = ring->fpos - (double)s0;
      if (frac > 0.0 && s0 + 1 < ring->count) {
        const int i1 = (ring->head + s0 + 1) % AAUDIO_RING_FRAMES;
        l = (int32_t)(l * (1.0 - frac) + ring->pcm[i1 * 2 + 0] * frac);
        rr = (int32_t)(rr * (1.0 - frac) + ring->pcm[i1 * 2 + 1] * frac);
      }
      if (ring->fade_in > 0) {
        const int32_t k = AAUDIO_DECLICK_FRAMES - ring->fade_in + 1;
        l = l * k / AAUDIO_DECLICK_FRAMES;
        rr = rr * k / AAUDIO_DECLICK_FRAMES;
        ring->fade_in--;
      }
      acc[mixed * 2 + 0] += l;
      acc[mixed * 2 + 1] += rr;
      ring->last_l = l;
      ring->last_r = rr;
      ring->fpos += ratio;
      mixed++;
    }
    int consumed = (int)ring->fpos;
    if (consumed > ring->count)
      consumed = ring->count;
    ring->head = (ring->head + consumed) % AAUDIO_RING_FRAMES;
    ring->count -= consumed;
    ring->fpos -= (double)consumed;
    if (ring->fpos < 0.0)
      ring->fpos = 0.0;

    if (mixed > 0)
      ring->playing = 1;
    if (mixed < frames) {
      /* Ran dry: fade the last frame out rather than stepping to zero. */
      const int left = frames - mixed;
      const int ramp = left < AAUDIO_DECLICK_FRAMES ? left : AAUDIO_DECLICK_FRAMES;
      for (int i = 0; i < ramp; i++) {
        const int32_t k = ramp - i - 1;
        acc[(mixed + i) * 2 + 0] += ring->last_l * k / ramp;
        acc[(mixed + i) * 2 + 1] += ring->last_r * k / ramp;
      }
      ring->last_l = ring->last_r = 0;
      if (ring->playing) {
        ring->xruns++;
        ring->playing = 0;
      }
      ring->priming = 1;
      ring->prime_last = ring->count;
      ring->prime_stalls = 0;
      ring->fpos = 0.0;
    }
  }
  SDL_UnlockMutex(g_aaudio_lock);
}

static void SDLCALL audio_callback(void *ud, Uint8 *stream, int len) {
  (void)ud;

  // The engine's completion callback (fired from here via mix_player) reads its
  // stack-guard from tpidr_el0+0x28; SDL's audio thread never set that up. Give
  // this thread its OWN bionic TLS block (single audio thread, so static is fine
  // and persists for its lifetime).
  static uint8_t audio_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  static int tls_ready = 0;
  if (!tls_ready) {
    install_bionic_tls(audio_tls);
    tls_ready = 1;
    /* SDL creates this thread through libnx pthreads (priority 0x3B, the
     * lowest, on the default core) and its Switch port maps TIME_CRITICAL to
     * 0x3B as well. It then round-robins in 10 ms slices with every guest
     * interpreter thread while the hardware holds only two buffers. Raise it
     * and move it off the default core; it sleeps in audrenWaitFrame() most
     * of the time, so this costs the game almost nothing. */
    audio_thread_boost("mixer", AUDIO_MIXER_PRIORITY);
  }

  const int frames = len / 4; // S16 stereo
  static int32_t acc[8192 * 2];
  if (frames > 8192) { memset(stream, 0, len); return; }
  memset(acc, 0, frames * 2 * sizeof(int32_t));

  SDL_LockMutex(g_reg_lock);
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] && g_players[i]->in_use)
      mix_player(g_players[i], acc, frames);
  SDL_UnlockMutex(g_reg_lock);

  // Measure the OpenSL/OpenAL contribution before movie music is added. The
  // final-device peak alone cannot tell whether only the separately decoded
  // soundtrack is audible.
  int32_t openal_peak = 0;
  for (int i = 0; i < frames * 2; i++) {
    int32_t a = acc[i] < 0 ? -acc[i] : acc[i];
    if (a > openal_peak) openal_peak = a;
  }
  if (openal_peak > 64) {
    static int once = 0;
    if (!once) {
      once = 1;
      debugPrintf("opensles: first non-silent OpenAL PCM (peak=%d)\n", openal_peak);
    }
  }

  mix_movie(acc, frames);
  mix_aaudio(acc, frames);

  int16_t *out = (int16_t *)stream;
  int32_t peak = 0;
  for (int i = 0; i < frames * 2; i++) {
    int32_t v = acc[i];
    if (v > 32767) v = 32767;
    else if (v < -32768) v = -32768;
    out[i] = (int16_t)v;
    int32_t a = v < 0 ? -v : v;
    if (a > peak) peak = a;
  }
  if (peak > 64) {
    static int once = 0;
    if (!once) { once = 1; debugPrintf("opensles: first non-silent mixed audio to device (peak=%d)\n", peak); }
  }
  /* Round 164. "Sound died after backing out of a game" was reported twice and
   * the log had NOTHING to say about it: audio came up cleanly and then simply
   * stopped, with no error and no further audio line all session. The
   * first-non-silent message is one-shot, so silence and death look identical.
   *
   * Count callbacks and track the running peak so the heartbeat can tell them
   * apart. Three states become distinguishable:
   *   callbacks rising, peak > 64   -- audio is fine
   *   callbacks rising, peak == 0   -- the mixer is running but producing
   *                                    silence: OpenAL stopped producing sound
   *   callbacks flat                -- the device stopped pulling: our OpenSL /
   *                                    SDL path died, not OpenAL
   * That distinction decides whether the next fix belongs in the audio shim or
   * in whatever unloaded the clips, and there is no way to guess it from here. */
  __atomic_fetch_add(&g_cb_count, 1u, __ATOMIC_RELAXED);
  if (peak > (int32_t)__atomic_load_n(&g_cb_peak, __ATOMIC_RELAXED))
    __atomic_store_n(&g_cb_peak, (unsigned)peak, __ATOMIC_RELAXED);
}

void nx_audio_stats(unsigned *cbs, unsigned *peak) {
  if (cbs)  *cbs  = __atomic_load_n(&g_cb_count, __ATOMIC_RELAXED);
  if (peak) *peak = __atomic_exchange_n(&g_cb_peak, 0u, __ATOMIC_RELAXED);
}

/* The guest's audio threads and the main thread can all reach the first
 * ensure_device()/slCreateEngine() together. The unlocked "if (!lock) create"
 * and "if (!g_dev) open" sequences let two threads each create a mutex (one
 * leaked, the other overwritten while in use) or open two SDL devices that
 * then both pulled from the same mixer. Serialise them. */
static SDL_SpinLock g_init_spin;
static SDL_SpinLock g_dev_spin;

static void ensure_locks(void) {
  SDL_AtomicLock(&g_init_spin);
  if (!g_reg_lock)
    g_reg_lock = SDL_CreateMutex();
  if (!g_aaudio_lock)
    g_aaudio_lock = SDL_CreateMutex();
  if (!g_movie_lock)
    g_movie_lock = SDL_CreateMutex();
  SDL_AtomicUnlock(&g_init_spin);
}

#ifdef __SWITCH__
/* ---- native Horizon output (1.0.9) -------------------------------------
 * SDL's Switch backend can wedge for good. Its PlayDevice() queues a wave
 * buffer and then loops
 *     while (buffer[current].state != AudioDriverWaveBufState_Playing)
 *         { audrvUpdate(); audrenWaitFrame(); }
 * but libnx's audrv moves a buffer straight from Queued to Done when the
 * voice finishes it between two updates. If the SDL thread is held off for
 * one buffer length (10.7 ms at 512 frames) the state it waits for never
 * appears and it spins forever: the mixer stops being called and audio is dead
 * for the rest of the session. An infinitesimal-looking hitch during loading
 * was enough (log: "mixer callbacks" frozen at 3294 from ~35 s on, ring stuck
 * at queued=4096).
 *
 * This drives audren directly with NX_OUT_BUFS wave buffers. The loop only
 * ever refills buffers that are Free or Done and never waits for a particular
 * state, so a late wake-up costs at most a gap, never the device. */
#define NX_OUT_BUFS 4

static const AudioRendererConfig g_nx_cfg = {
    .output_rate = AudioRendererOutputRate_48kHz,
    .num_voices = 2,
    .num_effects = 0,
    .num_sinks = 1,
    .num_mix_objs = 1,
    .num_mix_buffers = 2,
};

static AudioDriver g_nx_drv;
static void *g_nx_pool;
static AudioDriverWaveBuf g_nx_wb[NX_OUT_BUFS];
static Thread g_nx_thread;
static volatile int g_nx_quit;
static int g_nx_frames;
static int g_nx_stage; /* 1 audren, 2 driver, 3 thread */

static void nx_out_thread(void *arg) {
  (void)arg;
  const int bytes = g_nx_frames * 4; /* S16 stereo */
  u32 last_drops = 0;
  unsigned loops = 0;
  while (!__atomic_load_n(&g_nx_quit, __ATOMIC_ACQUIRE)) {
    for (int i = 0; i < NX_OUT_BUFS; i++) {
      AudioDriverWaveBuf *wb = &g_nx_wb[i];
      if (wb->state != AudioDriverWaveBufState_Free &&
          wb->state != AudioDriverWaveBufState_Done)
        continue;
      uint8_t *pcm = (uint8_t *)g_nx_pool + (size_t)i * (size_t)bytes;
      audio_callback(NULL, pcm, bytes);
      armDCacheFlush(pcm, (size_t)bytes);
      audrvVoiceAddWaveBuf(&g_nx_drv, 0, wb);
    }
    /* Idempotent; keeps the voice running after it has drained completely. */
    audrvVoiceStart(&g_nx_drv, 0);
    audrvUpdate(&g_nx_drv);
    if ((++loops & 1023u) == 0) {
      const u32 drops = audrvVoiceGetVoiceDropsCount(&g_nx_drv, 0);
      if (drops != last_drops) {
        debugPrintf("opensles: audren reported %u voice drop(s) (total %u)\n",
                    drops - last_drops, drops);
        last_drops = drops;
      }
    }
    audrenWaitFrame();
  }
}

static void nx_close_device(void) {
  if (g_nx_stage >= 3) {
    __atomic_store_n(&g_nx_quit, 1, __ATOMIC_RELEASE);
    threadWaitForExit(&g_nx_thread);
    threadClose(&g_nx_thread);
  }
  if (g_nx_stage >= 2)
    audrvClose(&g_nx_drv);
  if (g_nx_stage >= 1)
    audrenExit();
  free(g_nx_pool);
  g_nx_pool = NULL;
  g_nx_stage = 0;
}

static int open_device_nx_locked(void) {
  Result rc = audrenInitialize(&g_nx_cfg);
  if (R_FAILED(rc)) {
    debugPrintf("opensles: audrenInitialize failed: %08x\n", rc);
    return 0;
  }
  g_nx_stage = 1;
  rc = audrvCreate(&g_nx_drv, &g_nx_cfg, 2);
  if (R_FAILED(rc)) {
    debugPrintf("opensles: audrvCreate failed: %08x\n", rc);
    nx_close_device();
    return 0;
  }
  g_nx_stage = 2;

  g_nx_frames = AUDIO_DEVICE_SAMPLES;
  if (g_nx_frames < 128 || g_nx_frames > 4096)
    g_nx_frames = 512;
  const size_t bytes = (size_t)g_nx_frames * 4;
  const size_t pool_size =
      (bytes * NX_OUT_BUFS + (AUDREN_MEMPOOL_ALIGNMENT - 1)) &
      ~(size_t)(AUDREN_MEMPOOL_ALIGNMENT - 1);
  g_nx_pool = memalign(AUDREN_MEMPOOL_ALIGNMENT, pool_size);
  if (!g_nx_pool) {
    debugPrintf("opensles: no memory for audio pool\n");
    nx_close_device();
    return 0;
  }
  memset(g_nx_pool, 0, pool_size);
  armDCacheFlush(g_nx_pool, pool_size);
  for (int i = 0; i < NX_OUT_BUFS; i++) {
    memset(&g_nx_wb[i], 0, sizeof(g_nx_wb[i]));
    g_nx_wb[i].data_raw = g_nx_pool;
    g_nx_wb[i].size = pool_size;
    g_nx_wb[i].start_sample_offset = i * g_nx_frames;
    g_nx_wb[i].end_sample_offset = (i + 1) * g_nx_frames;
  }

  const int mpid = audrvMemPoolAdd(&g_nx_drv, g_nx_pool, pool_size);
  if (mpid < 0 || !audrvMemPoolAttach(&g_nx_drv, mpid)) {
    debugPrintf("opensles: audio mempool setup failed (%d)\n", mpid);
    nx_close_device();
    return 0;
  }
  static const u8 sink_channels[] = {0, 1};
  if (audrvDeviceSinkAdd(&g_nx_drv, AUDREN_DEFAULT_DEVICE_NAME, 2,
                         sink_channels) < 0) {
    debugPrintf("opensles: audio sink setup failed\n");
    nx_close_device();
    return 0;
  }
  rc = audrvUpdate(&g_nx_drv);
  if (R_SUCCEEDED(rc))
    rc = audrenStartAudioRenderer();
  if (R_FAILED(rc)) {
    debugPrintf("opensles: audio renderer start failed: %08x\n", rc);
    nx_close_device();
    return 0;
  }
  if (!audrvVoiceInit(&g_nx_drv, 0, 2, PcmFormat_Int16, 48000)) {
    debugPrintf("opensles: audio voice init failed\n");
    nx_close_device();
    return 0;
  }
  audrvVoiceSetDestinationMix(&g_nx_drv, 0, AUDREN_FINAL_MIX_ID);
  audrvVoiceSetMixFactor(&g_nx_drv, 0, 1.0f, 0, 0);
  audrvVoiceSetMixFactor(&g_nx_drv, 0, 0.0f, 0, 1);
  audrvVoiceSetMixFactor(&g_nx_drv, 0, 0.0f, 1, 0);
  audrvVoiceSetMixFactor(&g_nx_drv, 0, 1.0f, 1, 1);
  audrvVoiceStart(&g_nx_drv, 0);
  audrvUpdate(&g_nx_drv);

  g_dev_rate = 48000;
  __atomic_store_n(&g_nx_quit, 0, __ATOMIC_RELEASE);
  /* Create the thread at the audio priority on the audio core directly; step
   * the priority down if this title's NPDM refuses it. */
  const int core = audio_core();
  rc = 1;
  int prio = AUDIO_MIXER_PRIORITY;
  for (; prio <= 0x2C && R_FAILED(rc); prio++)
    rc = threadCreate(&g_nx_thread, nx_out_thread, NULL, NULL, 0x10000, prio,
                      core >= 0 ? core : -2);
  if (R_FAILED(rc)) {
    debugPrintf("opensles: audio thread create failed: %08x\n", rc);
    nx_close_device();
    return 0;
  }
  rc = threadStart(&g_nx_thread);
  if (R_FAILED(rc)) {
    debugPrintf("opensles: audio thread start failed: %08x\n", rc);
    threadClose(&g_nx_thread);
    nx_close_device();
    return 0;
  }
  g_nx_stage = 3;
  debugPrintf("opensles: native audren device opened: 48000 Hz S16 stereo, "
              "%d x %d frames (%.1f ms cushion), thread prio 0x%x core %d "
              "(shared OpenSL+AAudio)\n",
              NX_OUT_BUFS, g_nx_frames,
              NX_OUT_BUFS * g_nx_frames * 1000.0 / 48000.0, prio - 1,
              core >= 0 ? core : -2);
  return 1;
}
#endif /* __SWITCH__ */

static void open_device_sdl_locked(void) {
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
    debugPrintf("opensles: SDL audio init failed: %s\n", SDL_GetError());
    return;
  }
  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = 48000;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  /* SDL's Switch backend (SDL_switchaudio.c) has exactly two wave buffers of
   * `samples` frames, so the hardware only ever holds 2 * samples. At 512 that
   * was ~21 ms: any time the mixer thread was not scheduled for ~10 ms (it ran
   * at libnx's lowest pthread priority, round-robin with every interpreter
   * thread) the voice ran dry and audio crackled or dropped out. */
  want.samples = AUDIO_DEVICE_SAMPLES;
  want.callback = audio_callback;
  g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (!g_dev) {
    debugPrintf("opensles: SDL_OpenAudioDevice(%d) failed: %s; retrying\n",
                AUDIO_DEVICE_SAMPLES, SDL_GetError());
    /* Retry with larger buffers rather than leaving the game with no audio
     * device at all. */
    static const int fallback_samples[] = {2048, 4096};
    for (unsigned i = 0; i < sizeof(fallback_samples) / sizeof(fallback_samples[0]) && !g_dev; i++) {
      want.samples = (Uint16)fallback_samples[i];
      g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
      if (!g_dev)
        debugPrintf("opensles: SDL_OpenAudioDevice(%d) failed: %s\n",
                    fallback_samples[i], SDL_GetError());
    }
  }
  if (!g_dev) {
    debugPrintf("opensles: no SDL audio device could be opened (driver=%s)\n",
                SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "none");
    return;
  }
  g_dev_rate = have.freq;
  debugPrintf("opensles: SDL device opened: freq=%d channels=%d format=0x%04x samples=%d (shared OpenSL+AAudio)\n",
              have.freq, have.channels, have.format, have.samples);
  if (have.format != AUDIO_S16SYS || have.channels != 2)
    debugPrintf("opensles: WARNING device is not S16 stereo (format=0x%04x ch=%d); "
                "the mixer assumes S16 stereo\n", have.format, have.channels);
  SDL_PauseAudioDevice(g_dev, 0);
  __atomic_store_n(&g_dev_ready, 1, __ATOMIC_RELEASE);
}

static void open_device_locked(void) {
#ifdef __SWITCH__
  if (open_device_nx_locked()) {
    g_dev = 1;
    __atomic_store_n(&g_dev_ready, 1, __ATOMIC_RELEASE);
    return;
  }
  debugPrintf("opensles: native audio output failed; falling back to SDL\n");
#endif
  open_device_sdl_locked();
}

static void ensure_device(int rate) {
  (void)rate; // players run at mixed rates (22050/44100); open at the Switch's
              // native 48000 and resample each player in mix_player instead of
              // letting the first player pin the device rate.
  ensure_locks();
  if (__atomic_load_n(&g_dev_ready, __ATOMIC_ACQUIRE))
    return;
  SDL_AtomicLock(&g_dev_spin);
  if (!__atomic_load_n(&g_dev_ready, __ATOMIC_ACQUIRE))
    open_device_locked();
  SDL_AtomicUnlock(&g_dev_spin);
}

int opensles_ensure_shared_device(void) {
  ensure_device(48000);
  return __atomic_load_n(&g_dev_ready, __ATOMIC_ACQUIRE) ? 1 : 0;
}

int opensles_device_ready(void) {
  return __atomic_load_n(&g_dev_ready, __ATOMIC_ACQUIRE) ? 1 : 0;
}

int opensles_device_rate(void) {
  return g_dev_rate > 0 ? g_dev_rate : 48000;
}

AARing *opensles_aaudio_open(void) {
  if (!opensles_ensure_shared_device() || !g_aaudio_lock)
    return NULL;
  AARing *ring = NULL;
  SDL_LockMutex(g_aaudio_lock);
  for (int i = 0; i < AAUDIO_MAX_RINGS; i++) {
    if (!g_aaudio_rings[i].in_use) {
      int16_t *pcm = calloc((size_t)AAUDIO_RING_FRAMES * 2, sizeof(int16_t));
      if (pcm) {
        ring = &g_aaudio_rings[i];
        memset(ring, 0, sizeof(*ring));
        ring->pcm = pcm;
        ring->in_use = 1;
        ring->priming = 1; /* build a cushion before the first frame plays */
      }
      break;
    }
  }
  SDL_UnlockMutex(g_aaudio_lock);
  if (!ring)
    debugPrintf("opensles: no free AAudio ring (max %d streams)\n",
                AAUDIO_MAX_RINGS);
  return ring;
}

void opensles_aaudio_close(AARing *ring) {
  if (!ring || !g_aaudio_lock)
    return;
  SDL_LockMutex(g_aaudio_lock);
  if (ring->in_use) {
    free(ring->pcm);
    memset(ring, 0, sizeof(*ring));
  }
  SDL_UnlockMutex(g_aaudio_lock);
}

void opensles_aaudio_set_rate(AARing *ring, int rate) {
  if (!ring || !g_aaudio_lock)
    return;
  SDL_LockMutex(g_aaudio_lock);
  if (ring->in_use) {
    /* 0 = follow the device rate (no resampling). */
    ring->rate = (rate >= 8000 && rate <= 192000) ? rate : 0;
    ring->fpos = 0.0;
  }
  SDL_UnlockMutex(g_aaudio_lock);
}

void opensles_aaudio_set_active(AARing *ring, int active, int flush) {
  if (!ring || !g_aaudio_lock)
    return;
  SDL_LockMutex(g_aaudio_lock);
  if (ring->in_use) {
    ring->active = active != 0;
    if (flush) {
      ring->head = 0;
      ring->count = 0;
      ring->fpos = 0.0;
      ring->priming = 1;
      ring->prime_last = 0;
      ring->prime_stalls = 0;
      ring->playing = 0;
      ring->fade_in = 0;
      ring->last_l = ring->last_r = 0;
    }
  }
  SDL_UnlockMutex(g_aaudio_lock);
  debugPrintf("opensles: AAudio ring %s%s (ring=%d frames @ %d Hz)\n",
              active ? "started" : "stopped", flush ? " + flushed" : "",
              AAUDIO_RING_FRAMES, g_dev_rate);
}

int opensles_aaudio_queued_frames(AARing *ring) {
  int n = 0;
  if (!ring || !g_aaudio_lock)
    return 0;
  SDL_LockMutex(g_aaudio_lock);
  if (ring->in_use)
    n = ring->count;
  SDL_UnlockMutex(g_aaudio_lock);
  return n;
}

uint32_t opensles_aaudio_xruns(AARing *ring) {
  uint32_t n = 0;
  if (!ring || !g_aaudio_lock)
    return 0;
  SDL_LockMutex(g_aaudio_lock);
  if (ring->in_use)
    n = ring->xruns;
  SDL_UnlockMutex(g_aaudio_lock);
  return n;
}

int opensles_aaudio_free_frames(AARing *ring) {
  return ring ? AAUDIO_RING_FRAMES - opensles_aaudio_queued_frames(ring) : 0;
}

void opensles_aaudio_stats(AARing *ring, uint64_t *written, int32_t *peak_in) {
  opensles_aaudio_stats_ex(ring, written, peak_in, NULL);
}

void opensles_aaudio_stats_ex(AARing *ring, uint64_t *written, int32_t *peak_in,
                              float *float_peak) {
  if (written)
    *written = 0;
  if (peak_in)
    *peak_in = 0;
  if (float_peak)
    *float_peak = 0.0f;
  if (!ring || !g_aaudio_lock)
    return;
  SDL_LockMutex(g_aaudio_lock);
  if (ring->in_use) {
    if (written)
      *written = ring->written;
    if (peak_in) {
      *peak_in = ring->peak_in;
      ring->peak_in = 0; /* read-and-reset; callers passing NULL leave it */
    }
    if (float_peak) {
      *float_peak = ring->float_peak;
      ring->float_peak = 0.0f;
    }
  }
  SDL_UnlockMutex(g_aaudio_lock);
}

/* Convert `frames` frames of interleaved PCM (S16 or float, any channel count)
 * to stereo S16 and append them. Mono is duplicated; channels beyond the first
 * two are dropped. Returns frames accepted (short when the ring is full). */
int opensles_aaudio_write_pcm(AARing *ring, const void *data, int frames,
                              int channels, int is_float) {
  return opensles_aaudio_write_pcm_ex(ring, data, frames, channels, is_float,
                                      AAUDIO_RING_FRAMES);
}

/* Same as above but never lets more than `max_queued` frames sit in the ring.
 * The ring is 16384 frames (~340 ms at 48 kHz); a producer paced only by
 * "write until full" parked ~340 ms of audio in it, so every sound effect
 * played that much late. The stream's buffer size is the real bound. */
int opensles_aaudio_write_pcm_ex(AARing *ring, const void *data, int frames,
                                 int channels, int is_float, int max_queued) {
  if (!ring || !data || frames <= 0 || !g_aaudio_lock)
    return 0;
  if (channels < 1)
    channels = 1;
  if (max_queued > AAUDIO_RING_FRAMES || max_queued <= 0)
    max_queued = AAUDIO_RING_FRAMES;
  SDL_LockMutex(g_aaudio_lock);
  if (!ring->in_use || !ring->pcm) {
    SDL_UnlockMutex(g_aaudio_lock);
    return 0;
  }
  const int space = max_queued - ring->count;
  if (space <= 0) {
    SDL_UnlockMutex(g_aaudio_lock);
    return 0;
  }
  const int n = frames < space ? frames : space;
  int32_t peak = ring->peak_in;
  float fpeak = ring->float_peak;
  for (int i = 0; i < n; i++) {
    int32_t l, r;
    if (is_float) {
      const float *f = (const float *)data + (size_t)i * (size_t)channels;
      float lf = f[0], rf = channels > 1 ? f[1] : f[0];
      if (lf != lf) lf = 0.0f;               /* NaN guard */
      if (rf != rf) rf = 0.0f;
      /* Track raw magnitude BEFORE clamp so we can see if the guest is sending
       * values outside the AAudio [-1,1] contract (or near-zero noise). */
      float alf = lf < 0.0f ? -lf : lf;
      float arf = rf < 0.0f ? -rf : rf;
      if (alf > fpeak) fpeak = alf;
      if (arf > fpeak) fpeak = arf;
      if (lf > 1.0f) lf = 1.0f; else if (lf < -1.0f) lf = -1.0f;
      if (rf > 1.0f) rf = 1.0f; else if (rf < -1.0f) rf = -1.0f;
      /* ~1 dB headroom so full-scale float does not hard-clip to ±32767. */
      l = (int32_t)(lf * 30000.0f);
      r = (int32_t)(rf * 30000.0f);
    } else {
      const int16_t *s16 = (const int16_t *)data + (size_t)i * (size_t)channels;
      l = s16[0];
      r = channels > 1 ? s16[1] : s16[0];
    }
    const int idx = (ring->head + ring->count + i) % AAUDIO_RING_FRAMES;
    ring->pcm[idx * 2 + 0] = (int16_t)l;
    ring->pcm[idx * 2 + 1] = (int16_t)r;
    const int32_t al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
    if (al > peak) peak = al;
    if (ar > peak) peak = ar;
  }
  ring->count += n;
  ring->written += (uint64_t)n;
  ring->peak_in = peak;
  ring->float_peak = fpeak;
  SDL_UnlockMutex(g_aaudio_lock);
  return n;
}

int opensles_movie_begin(int requested_rate) {
  ensure_locks();
  if (!g_movie_pcm)
    g_movie_pcm = calloc(MOVIE_RING_FRAMES * 2, sizeof(int16_t));
  if (!g_movie_lock || !g_movie_pcm)
    return 0;

  ensure_device(requested_rate > 0 ? requested_rate : 44100);
  if (!g_dev)
    return 0;

  SDL_LockMutex(g_movie_lock);
  g_movie_active = 1;
  g_movie_paused = 1;
  g_movie_head = 0;
  g_movie_count = 0;
  g_movie_rate = requested_rate > 0 ? requested_rate : 44100;
  g_movie_fpos = 0.0;
  g_movie_samples_queued = 0;
  g_movie_samples_played = 0;
  SDL_UnlockMutex(g_movie_lock);
  debugPrintf("opensles: music resampler source=%dHz device=%dHz\n",
              g_movie_rate, g_dev_rate);
  return g_dev_rate;
}

int opensles_movie_queue(const int16_t *pcm, int frames) {
  int done = 0;
  while (done < frames) {
    if (!g_movie_lock)
      return done;

    SDL_LockMutex(g_movie_lock);
    if (!g_movie_active || !g_movie_pcm) {
      SDL_UnlockMutex(g_movie_lock);
      return done;
    }

    const int space = MOVIE_RING_FRAMES - g_movie_count;
    int n = frames - done;
    if (n > space)
      n = space;
    for (int i = 0; i < n; i++) {
      const int idx = (g_movie_head + g_movie_count + i) % MOVIE_RING_FRAMES;
      g_movie_pcm[idx * 2 + 0] = pcm[(done + i) * 2 + 0];
      g_movie_pcm[idx * 2 + 1] = pcm[(done + i) * 2 + 1];
    }
    g_movie_count += n;
    g_movie_samples_queued += (uint64_t)n;
    SDL_UnlockMutex(g_movie_lock);

    done += n;
    if (done < frames)
      SDL_Delay(2);
  }
  return done;
}

void opensles_movie_set_paused(int paused) {
  if (!g_movie_lock)
    return;
  SDL_LockMutex(g_movie_lock);
  if (g_movie_active)
    g_movie_paused = paused != 0;
  SDL_UnlockMutex(g_movie_lock);
}

uint64_t opensles_movie_samples_queued(void) {
  uint64_t ret = 0;
  if (!g_movie_lock)
    return 0;
  SDL_LockMutex(g_movie_lock);
  ret = g_movie_samples_queued;
  SDL_UnlockMutex(g_movie_lock);
  return ret;
}

uint64_t opensles_movie_samples_played(void) {
  uint64_t ret = 0;
  if (!g_movie_lock)
    return 0;
  SDL_LockMutex(g_movie_lock);
  ret = g_movie_samples_played;
  SDL_UnlockMutex(g_movie_lock);
  return ret;
}

int opensles_movie_buffered_frames(void) {
  int ret = 0;
  if (!g_movie_lock)
    return 0;
  SDL_LockMutex(g_movie_lock);
  ret = g_movie_count;
  SDL_UnlockMutex(g_movie_lock);
  return ret;
}

void opensles_movie_end(void) {
  if (!g_movie_lock)
    return;
  SDL_LockMutex(g_movie_lock);
  g_movie_active = 0;
  g_movie_paused = 0;
  g_movie_head = 0;
  g_movie_count = 0;
  g_movie_rate = 44100;
  g_movie_fpos = 0.0;
  SDL_UnlockMutex(g_movie_lock);
}

// --- buffer queue interface -------------------------------------------------

static SLresult bq_Enqueue(void *self, const void *pBuffer, SLuint32 size) {
  Player *p = CONTAINER(self, Player, bq_vt);
  SDL_LockMutex(p->lock);
  const int next = (p->q_tail + 1) % BQ_SLOTS;
  if (next == p->q_head) { // full
    SDL_UnlockMutex(p->lock);
    return SL_RESULT_BUFFER_INSUFFICIENT;
  }
  p->q[p->q_tail].data = pBuffer;
  p->q[p->q_tail].size = size;
  p->q_tail = next;
  const int depth = (p->q_tail - p->q_head + BQ_SLOTS) % BQ_SLOTS +
                    (p->cur ? 1 : 0);
  SDL_UnlockMutex(p->lock);
  static unsigned enqueue_logs = 0;
  const unsigned log_index = __atomic_fetch_add(&enqueue_logs, 1u, __ATOMIC_RELAXED);
  if (log_index < 4)
    debugPrintf("opensles: enqueue #%u size=%u depth=%d\n",
                log_index + 1, size, depth);
  return SL_RESULT_SUCCESS;
}

static SLresult bq_Clear(void *self) {
  Player *p = CONTAINER(self, Player, bq_vt);
  SDL_LockMutex(p->lock);
  p->q_head = p->q_tail = 0;
  p->cur = NULL;
  p->cur_pos = p->cur_size = 0;
  p->cur_fpos = 0.0;
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}

typedef struct { SLuint32 count; SLuint32 index; } SLBufferQueueState;

static SLresult bq_GetState(void *self, void *pState) {
  Player *p = CONTAINER(self, Player, bq_vt);
  if (pState) {
    SLBufferQueueState *st = pState;
    SDL_LockMutex(p->lock);
    st->count = (p->q_tail - p->q_head + BQ_SLOTS) % BQ_SLOTS + (p->cur ? 1 : 0);
    st->index = 0;
    SDL_UnlockMutex(p->lock);
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_RegisterCallback(void *self, slBufferQueueCallback cb, void *ctx) {
  Player *p = CONTAINER(self, Player, bq_vt);
  p->cb = cb;
  p->cb_ctx = ctx;
  return SL_RESULT_SUCCESS;
}

static const SLBufferQueueItf_ bq_vtable = {
  bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback,
};

// --- play interface ---------------------------------------------------------

static SLresult play_SetPlayState(void *self, SLuint32 state) {
  Player *p = CONTAINER(self, Player, play_vt);
  SDL_LockMutex(p->lock);
  p->playing = (state == SL_PLAYSTATE_PLAYING);
  SDL_UnlockMutex(p->lock);
  if (p->playing) {
    static int once = 0;
    if (!once) { once = 1; debugPrintf("opensles: SetPlayState(PLAYING); output running\n"); }
  }
  return SL_RESULT_SUCCESS;
}
static SLresult play_GetPlayState(void *self, SLuint32 *pState) {
  Player *p = CONTAINER(self, Player, play_vt);
  if (pState) *pState = p->playing ? SL_PLAYSTATE_PLAYING : SL_PLAYSTATE_STOPPED;
  return SL_RESULT_SUCCESS;
}
static SLresult play_ret0_u32(void *self, SLuint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult play_ok_u32(void *self, SLuint32 v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult play_ok(void *self) { (void)self; return SL_RESULT_SUCCESS; }
static SLresult play_RegisterCallback(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }

static const SLPlayItf_ play_vtable = {
  play_SetPlayState, play_GetPlayState, play_ret0_u32, play_ret0_u32,
  play_RegisterCallback, play_ok_u32, play_ret0_u32, play_ok_u32,
  play_ok, play_ret0_u32, play_ok_u32, play_ret0_u32,
};

// --- volume interface -------------------------------------------------------

static SLresult vol_SetVolumeLevel(void *self, SLmillibel level) {
  Player *p = CONTAINER(self, Player, vol_vt);
  // Clamp to the valid OpenSL volume range [-9600, 0] mB before converting.
  int mb = (int)level;
  if (mb > 0) mb = 0;
  if (mb < -9600) mb = -9600;
  p->level_mb = (SLmillibel)mb;
  p->gain = mb_to_linear(mb);
  return SL_RESULT_SUCCESS;
}
static SLresult vol_GetVolumeLevel(void *self, SLmillibel *out) {
  Player *p = CONTAINER(self, Player, vol_vt);
  if (out) *out = p->level_mb;
  return SL_RESULT_SUCCESS;
}
static SLresult vol_GetMaxVolumeLevel(void *self, SLmillibel *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
/* Mute is its own flag. It used to zero `gain` and nothing ever restored it, so
 * a voice that was muted once stayed silent forever, even after SetMute(false)
 * or a fresh SetVolumeLevel. */
static SLresult vol_SetMute(void *self, SLboolean m) {
  Player *p = CONTAINER(self, Player, vol_vt);
  p->muted = m != 0;
  return SL_RESULT_SUCCESS;
}
static SLresult vol_GetMute(void *self, SLboolean *out) {
  Player *p = CONTAINER(self, Player, vol_vt);
  if (out) *out = p->muted ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
  return SL_RESULT_SUCCESS;
}
static SLresult vol_enable(void *self, SLboolean e) { (void)self; (void)e; return SL_RESULT_SUCCESS; }
static SLresult vol_isenabled(void *self, SLboolean *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_setpos(void *self, SLint32 v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult vol_getpos(void *self, SLint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }

static const SLVolumeItf_ vol_vtable = {
  vol_SetVolumeLevel, vol_GetVolumeLevel, vol_GetMaxVolumeLevel, vol_SetMute,
  vol_GetMute, vol_enable, vol_isenabled, vol_setpos, vol_getpos,
};

// --- playback rate interface (accepted but not resampled) -------------------

static SLresult rate_SetRate(void *self, SLint16 r) { (void)self; (void)r; return SL_RESULT_SUCCESS; }
static SLresult rate_GetRate(void *self, SLint16 *p) { (void)self; if (p) *p = 1000; return SL_RESULT_SUCCESS; }
static SLresult rate_SetProps(void *self, SLuint32 c) { (void)self; (void)c; return SL_RESULT_SUCCESS; }
static SLresult rate_GetProps(void *self, SLuint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult rate_GetCaps(void *self, SLuint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult rate_GetRange(void *self, SLuint8 i, SLint16 *min, SLint16 *max, SLint16 *step, SLuint32 *prop) {
  (void)self; (void)i;
  if (min)
    *min = 500;
  if (max)
    *max = 2000;
  if (step)
    *step = 1;
  if (prop)
    *prop = 0;
  return SL_RESULT_SUCCESS;
}
static const SLPlaybackRateItf_ rate_vtable = {
  rate_SetRate, rate_GetRate, rate_SetProps, rate_GetProps, rate_GetCaps, rate_GetRange,
};

// --- android configuration interface (accepted, ignored) --------------------

static SLresult cfg_SetConfiguration(void *self, const void *key, const void *value, SLuint32 sz) {
  (void)self; (void)key; (void)value; (void)sz; return SL_RESULT_SUCCESS;
}
static SLresult cfg_GetConfiguration(void *self, const void *key, SLuint32 *psz, void *value) {
  (void)self; (void)key; (void)value; if (psz) *psz = 0; return SL_RESULT_SUCCESS;
}
static SLresult cfg_AcquireJavaProxy(void *self, SLuint32 t, void *p) {
  (void)self; (void)t; if (p) *(void **)p = NULL; return SL_RESULT_FEATURE_UNSUPPORTED;
}
static SLresult cfg_ReleaseJavaProxy(void *self, SLuint32 t) { (void)self; (void)t; return SL_RESULT_SUCCESS; }

static const SLAndroidConfigurationItf_ cfg_vtable = {
  cfg_SetConfiguration, cfg_GetConfiguration, cfg_AcquireJavaProxy, cfg_ReleaseJavaProxy,
};

// --- player object ----------------------------------------------------------

static SLresult player_GetInterface(void *self, const SLInterfaceID iid, void *pInterface);
static void player_Destroy(void *self);

static SLresult obj_Realize(void *self, SLboolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_Resume(void *self, SLboolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_GetState(void *self, SLuint32 *pState) { (void)self; if (pState) *pState = SL_OBJECT_STATE_REALIZED; return SL_RESULT_SUCCESS; }
static SLresult obj_RegisterCallback(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }
static SLresult obj_Abort(void *self) { (void)self; return SL_RESULT_SUCCESS; }
static SLresult obj_SetPriority(void *self, SLint32 a, SLboolean b) { (void)self; (void)a; (void)b; return SL_RESULT_SUCCESS; }
static SLresult obj_GetPriority(void *self, SLint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult obj_SetLOC(void *self, SLint32 a, SLInterfaceID *b, SLboolean c) { (void)self; (void)a; (void)b; (void)c; return SL_RESULT_SUCCESS; }

static SLresult mix_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  (void)self; (void)iid;
  if (pInterface) *(void **)pInterface = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
static void simple_Destroy(void *self) { free(self); }

static const SLObjectItf_ player_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, player_GetInterface, obj_RegisterCallback,
  obj_Abort, player_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};
static const SLObjectItf_ mix_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, mix_GetInterface, obj_RegisterCallback,
  obj_Abort, simple_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};

static SLresult player_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  Player *p = CONTAINER(self, Player, obj_vt);
  if (!pInterface)
    return SL_RESULT_PARAMETER_INVALID;
  if (iid == SL_IID_PLAY) {
    *(void **)pInterface = &p->play_vt;
  } else if (iid == SL_IID_BUFFERQUEUE || iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE) {
    *(void **)pInterface = &p->bq_vt;
  } else if (iid == SL_IID_VOLUME) {
    *(void **)pInterface = &p->vol_vt;
  } else if (iid == SL_IID_PLAYBACKRATE) {
    *(void **)pInterface = &p->rate_vt;
  } else if (iid == SL_IID_ANDROIDCONFIGURATION) {
    *(void **)pInterface = &p->config_vt;
  } else {
    *(void **)pInterface = NULL;
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }
  return SL_RESULT_SUCCESS;
}

static void player_Destroy(void *self) {
  Player *p = CONTAINER(self, Player, obj_vt);
  SDL_LockMutex(g_reg_lock);
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] == p) g_players[i] = NULL;
  SDL_UnlockMutex(g_reg_lock);
  if (p->lock) SDL_DestroyMutex(p->lock);
  free(p);
}

// --- engine interface -------------------------------------------------------

static SLresult eng_CreateAudioPlayer(void *self, SLObjectItf *pPlayer, SLDataSource *src, SLDataSink *snk,
                                      SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req) {
  (void)self; (void)snk; (void)numIfaces; (void)ids; (void)req;
  if (!pPlayer)
    return SL_RESULT_PARAMETER_INVALID;

  Player *p = calloc(1, sizeof(*p));
  if (!p)
    return SL_RESULT_MEMORY_FAILURE;
  p->obj_vt = &player_obj_vtable;
  p->play_vt = &play_vtable;
  p->bq_vt = &bq_vtable;
  p->vol_vt = &vol_vtable;
  p->rate_vt = &rate_vtable;
  p->config_vt = &cfg_vtable;
  p->in_use = 1;
  p->gain = 1.0f;
  p->channels = 2;
  p->rate = 44100;
  p->sbytes = 2;     // assume 16-bit signed PCM unless the format says otherwise
  p->is_float = 0;
  p->lock = SDL_CreateMutex();
  if (!p->lock) {
    free(p);
    return SL_RESULT_MEMORY_FAILURE;
  }

  if (src && src->pFormat) {
    const SLDataFormat_PCM *fmt = src->pFormat;
    // formatType 2 = SL_DATAFORMAT_PCM (integer); 4 = SL_ANDROID_DATAFORMAT_PCM_EX
    // (adds a trailing representation field: 1=signed int, 2=unsigned int,
    // 3=float, per OpenSLES_Android.h).
    if (fmt->formatType == 2 || fmt->formatType == 4) {
      p->channels = fmt->numChannels ? (int)fmt->numChannels : 2;
      p->rate = fmt->samplesPerSec ? (int)(fmt->samplesPerSec / 1000) : 44100;
      // stride is the container size (bits) when given, else the sample width.
      uint32_t stride_bits = fmt->containerSize ? fmt->containerSize : fmt->bitsPerSample;
      p->sbytes = stride_bits >= 32 ? 4 : 2;
      if (fmt->formatType == 4) {
        uint32_t representation = ((const uint32_t *)fmt)[7]; // field after endianness
        p->is_float = (representation == 3);
      }
    }
    debugPrintf("opensles: format type=%u ch=%u rate=%uHz bits=%u container=%u -> %d-byte %s\n",
                fmt->formatType, fmt->numChannels, p->rate, fmt->bitsPerSample,
                fmt->containerSize, p->sbytes, p->is_float ? "float" : "int");
  }
  debugPrintf("opensles: CreateAudioPlayer: %d Hz, %d ch (OpenAL output)\n",
              p->rate, p->channels);

  ensure_device(p->rate);

  SDL_LockMutex(g_reg_lock);
  int slot = -1;
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] == NULL) { slot = i; break; }
  if (slot < 0 && g_player_count < MAX_PLAYERS)
    slot = g_player_count++;
  if (slot < 0) {
    // pool full: the engine never Destroys finished SEs, so reclaim one that has
    // been playing-but-silent for >~0.8s (a live BGM re-enqueues far sooner, so
    // it never becomes a victim). Safe to free here -- the mixer holds g_reg_lock
    // while mixing, so it can't touch the victim concurrently.
    for (int i = 0; i < g_player_count; i++) {
      Player *q = g_players[i];
      /* Only recycle one-shots with no buffer-queue callback. Voices that
       * registered a callback (OpenAL/BGM) re-enqueue through it — freeing
       * those left the guest holding a dead player and audio never returned. */
      if (q && q->playing && !q->cb && q->drained > 250) {
        g_players[i] = NULL;
        if (q->lock) SDL_DestroyMutex(q->lock);
        free(q);
        slot = i;
        break;
      }
    }
  }
  if (slot >= 0)
    g_players[slot] = p;
  SDL_UnlockMutex(g_reg_lock);

  if (slot < 0) {
    /* Pool exhausted and no reclaimable voice. The player used to be handed back
     * as a success while never being registered with the mixer: the guest then
     * believed it was playing and heard nothing. Fail the create instead. */
    debugPrintf("opensles: player pool exhausted (%d voices); CreateAudioPlayer fails\n",
                MAX_PLAYERS);
    SDL_DestroyMutex(p->lock);
    free(p);
    *pPlayer = NULL;
    return SL_RESULT_RESOURCE_ERROR;
  }

  *pPlayer = &p->obj_vt;
  return SL_RESULT_SUCCESS;
}

static SLresult eng_CreateOutputMix(void *self, SLObjectItf *pMix, SLuint32 numIfaces,
                                    const SLInterfaceID *ids, const SLboolean *req) {
  (void)self; (void)numIfaces; (void)ids; (void)req;
  OutputMix *m = calloc(1, sizeof(*m));
  if (!m)
    return SL_RESULT_MEMORY_FAILURE;
  m->obj_vt = &mix_obj_vtable;
  if (pMix) *pMix = &m->obj_vt;
  return SL_RESULT_SUCCESS;
}

static SLresult eng_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

static const SLEngineItf_ engine_vtable = {
  .CreateLEDDevice = (void *)eng_unsupported,
  .CreateVibraDevice = (void *)eng_unsupported,
  .CreateAudioPlayer = eng_CreateAudioPlayer,
  .CreateAudioRecorder = (void *)eng_unsupported,
  .CreateMidiPlayer = (void *)eng_unsupported,
  .CreateListener = (void *)eng_unsupported,
  .Create3DGroup = (void *)eng_unsupported,
  .CreateOutputMix = eng_CreateOutputMix,
  .CreateMetadataExtractor = (void *)eng_unsupported,
  .CreateExtensionObject = (void *)eng_unsupported,
  .QueryNumSupportedInterfaces = (void *)eng_unsupported,
  .QuerySupportedInterfaces = (void *)eng_unsupported,
  .QueryNumSupportedExtensions = (void *)eng_unsupported,
  .QuerySupportedExtension = (void *)eng_unsupported,
  .IsExtensionSupported = (void *)eng_unsupported,
};

static SLresult engine_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  Engine *e = CONTAINER(self, Engine, obj_vt);
  if (!pInterface)
    return SL_RESULT_PARAMETER_INVALID;
  if (iid == SL_IID_ENGINE) {
    *(void **)pInterface = &e->eng_vt;
    return SL_RESULT_SUCCESS;
  }
  *(void **)pInterface = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static const SLObjectItf_ engine_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, engine_GetInterface, obj_RegisterCallback,
  obj_Abort, simple_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};

// --- entry point ------------------------------------------------------------

uint32_t slCreateEngine(void **pEngine, uint32_t numOptions, const void *pEngineOptions,
                        uint32_t numInterfaces, const void *pInterfaceIds,
                        const void *pInterfaceRequired) {
  (void)numOptions; (void)pEngineOptions; (void)numInterfaces;
  (void)pInterfaceIds; (void)pInterfaceRequired;
  debugPrintf("opensles: slCreateEngine called; OpenSL output selected\n");
  ensure_locks();
  if (!pEngine)
    return SL_RESULT_PARAMETER_INVALID;
  Engine *e = calloc(1, sizeof(*e));
  if (!e)
    return SL_RESULT_MEMORY_FAILURE;
  e->obj_vt = &engine_obj_vtable;
  e->eng_vt = &engine_vtable;
  *pEngine = &e->obj_vt;
  return SL_RESULT_SUCCESS;
}

void opensles_shutdown(void) {
  opensles_movie_end();
  if (g_dev) {
    __atomic_store_n(&g_dev_ready, 0, __ATOMIC_RELEASE);
#ifdef __SWITCH__
    if (g_nx_stage)
      nx_close_device();
    else
#endif
      SDL_CloseAudioDevice(g_dev);
    g_dev = 0;
  }
  free(g_movie_pcm);
  g_movie_pcm = NULL;
  /* Free every OpenSL player so a later restart does not mix freed objects or
   * leak the pool. */
  if (g_reg_lock)
    SDL_LockMutex(g_reg_lock);
  for (int i = 0; i < g_player_count; i++) {
    Player *p = g_players[i];
    g_players[i] = NULL;
    if (p) {
      if (p->lock)
        SDL_DestroyMutex(p->lock);
      free(p);
    }
  }
  g_player_count = 0;
  if (g_reg_lock)
    SDL_UnlockMutex(g_reg_lock);
  if (g_aaudio_lock) {
    SDL_LockMutex(g_aaudio_lock);
    for (int i = 0; i < AAUDIO_MAX_RINGS; i++) {
      free(g_aaudio_rings[i].pcm);
      memset(&g_aaudio_rings[i], 0, sizeof(g_aaudio_rings[i]));
    }
    SDL_UnlockMutex(g_aaudio_lock);
  }
  /* Keep the mutexes alive so a later ensure_device()/open can reuse them
   * without racing a destroyed lock. */
}
