/* opensles.h -- minimal OpenSL ES shim for Infinity Blade's OpenAL backend
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * libopenal.so dynamically loads the Android OpenSL ES system library, which
 * has no devkitPro equivalent. We implement just enough of the object model
 * (Engine -> OutputMix / AudioPlayer with an Android simple buffer queue, plus
 * the Play and Volume interfaces) and back the buffer-queue players with a
 * single SDL2 audio device that software-mixes them.
 */

#ifndef __OPENSLES_H__
#define __OPENSLES_H__

#include <stdint.h>

uint32_t slCreateEngine(void **pEngine, uint32_t numOptions, const void *pEngineOptions,
                        uint32_t numInterfaces, const void *pInterfaceIds,
                        const void *pInterfaceRequired);

void opensles_shutdown(void);


int opensles_movie_begin(int requested_rate);
int opensles_movie_queue(const int16_t *pcm, int frames);
void opensles_movie_set_paused(int paused);
uint64_t opensles_movie_samples_queued(void);
uint64_t opensles_movie_samples_played(void);
int opensles_movie_buffered_frames(void);
void opensles_movie_end(void);

/* Shared SDL device used by OpenSL players, movie music, and the AAudio
 * side-bus (iOS AUGraph path). Callers must not open a second device. */
int opensles_ensure_shared_device(void);
int opensles_device_rate(void);
int opensles_device_ready(void);

/* AAudio / AUGraph side-bus. Every open AAudio stream owns one ring; all rings
 * are mixed on the shared SDL device. Writes accept S16 or float PCM with any
 * channel count (converted to stereo S16) and return the frames accepted,
 * which is less than requested when the ring is full -- they never block. */
typedef struct AARing AARing;
AARing *opensles_aaudio_open(void);
void opensles_aaudio_close(AARing *ring);
void opensles_aaudio_set_active(AARing *ring, int active, int flush);
/* Sample rate of the stream's PCM. The mixer resamples to the device rate when
 * they differ. 0 (or an out-of-range value) means "same as the device". */
void opensles_aaudio_set_rate(AARing *ring, int rate);
int opensles_aaudio_write_pcm(AARing *ring, const void *data, int frames,
                              int channels, int is_float);
/* As above, but accepts frames only while fewer than `max_queued` are queued. */
int opensles_aaudio_write_pcm_ex(AARing *ring, const void *data, int frames,
                                 int channels, int is_float, int max_queued);
int opensles_aaudio_queued_frames(AARing *ring);
int opensles_aaudio_free_frames(AARing *ring);
/* Underruns since the ring opened (a playing ring that ran dry). */
uint32_t opensles_aaudio_xruns(AARing *ring);
void opensles_aaudio_stats(AARing *ring, uint64_t *written, int32_t *peak_in);
/* Extended stats: also returns max |float| sample seen (before clamp). Useful
 * when peak_in is 0 but the guest may be sending non-zero floats that clamp
 * to silence or are outside the AAudio [-1,1] range. */
void opensles_aaudio_stats_ex(AARing *ring, uint64_t *written, int32_t *peak_in,
                              float *float_peak);

// Interface-id tokens returned through dlsym. Each is a unique non-NULL
// sentinel (self-addressed); OpenAL passes the value to GetInterface and we
// compare pointers.
extern void *SL_IID_3DCOMMIT, *SL_IID_3DDOPPLER, *SL_IID_3DGROUPING, *SL_IID_3DLOCATION;
extern void *SL_IID_3DMACROSCOPIC, *SL_IID_3DSOURCE, *SL_IID_ANDROIDCONFIGURATION;
extern void *SL_IID_ANDROIDEFFECT, *SL_IID_ANDROIDEFFECTCAPABILITIES, *SL_IID_ANDROIDEFFECTSEND;
extern void *SL_IID_ANDROIDSIMPLEBUFFERQUEUE, *SL_IID_AUDIODECODERCAPABILITIES, *SL_IID_AUDIOENCODER;
extern void *SL_IID_AUDIOENCODERCAPABILITIES, *SL_IID_AUDIOIODEVICECAPABILITIES, *SL_IID_BASSBOOST;
extern void *SL_IID_BUFFERQUEUE, *SL_IID_DEVICEVOLUME, *SL_IID_DYNAMICINTERFACEMANAGEMENT;
extern void *SL_IID_DYNAMICSOURCE, *SL_IID_EFFECTSEND, *SL_IID_ENGINE, *SL_IID_ENGINECAPABILITIES;
extern void *SL_IID_ENVIRONMENTALREVERB, *SL_IID_EQUALIZER, *SL_IID_LED, *SL_IID_METADATAEXTRACTION;
extern void *SL_IID_METADATATRAVERSAL, *SL_IID_MIDIMESSAGE, *SL_IID_MIDIMUTESOLO, *SL_IID_MIDITEMPO;
extern void *SL_IID_MIDITIME, *SL_IID_MUTESOLO, *SL_IID_NULL, *SL_IID_OBJECT, *SL_IID_OUTPUTMIX;
extern void *SL_IID_PITCH, *SL_IID_PLAY, *SL_IID_PLAYBACKRATE, *SL_IID_PREFETCHSTATUS;
extern void *SL_IID_PRESETREVERB, *SL_IID_RATEPITCH, *SL_IID_RECORD, *SL_IID_SEEK, *SL_IID_THREADSYNC;
extern void *SL_IID_VIBRA, *SL_IID_VIRTUALIZER, *SL_IID_VISUALIZATION, *SL_IID_VOLUME;

/* Audio liveness. cbs = total mixer callbacks; peak = loudest
 * sample since the last call (read-and-reset). Callbacks flat = the device
 * stopped pulling; callbacks rising with peak 0 = the mixer is silent. */
void nx_audio_stats(unsigned *cbs, unsigned *peak);

#endif
