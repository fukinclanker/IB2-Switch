#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <mpg123.h>

#include "music_player.h"
#include "opensles.h"
#include "util.h"

static SDL_Thread *music_thread;
static volatile int music_stop_requested;
static volatile float music_volume = 1.0f;
static char music_path[0x400];
static int mpg123_initialized;

static void scale_s16(void *data, size_t bytes, float volume) {
  int16_t *samples = data;
  size_t count = bytes / sizeof(*samples);
  for (size_t index = 0; index < count; index++)
    samples[index] = (int16_t)((float)samples[index] * volume);
}

static int music_decode_thread(void *opaque) {
  (void)opaque;
  int error = MPG123_OK;
  mpg123_handle *decoder = mpg123_new(NULL, &error);
  if (!decoder) {
    debugPrintf("music: mpg123_new failed: %d\n", error);
    return 1;
  }
  mpg123_format_none(decoder);
  mpg123_format(decoder, 44100, MPG123_STEREO, MPG123_ENC_SIGNED_16);
  if (mpg123_open(decoder, music_path) != MPG123_OK) {
    debugPrintf("music: open failed: %s\n", music_path);
    mpg123_delete(decoder);
    return 1;
  }

  long rate = 0;
  int channels = 0;
  int encoding = 0;
  if (mpg123_getformat(decoder, &rate, &channels, &encoding) != MPG123_OK ||
      encoding != MPG123_ENC_SIGNED_16 || channels < 1 || channels > 2) {
    debugPrintf("music: unsupported stream format rate=%ld channels=%d enc=%x\n",
                rate, channels, encoding);
    mpg123_close(decoder);
    mpg123_delete(decoder);
    return 1;
  }

  if (!opensles_movie_begin((int)rate)) {
    debugPrintf("music: shared audio mixer failed to start\n");
    mpg123_close(decoder);
    mpg123_delete(decoder);
    return 1;
  }
  opensles_movie_set_paused(0);

  size_t buffer_size = mpg123_outblock(decoder);
  unsigned char *buffer = malloc(buffer_size);
  if (!buffer)
    music_stop_requested = 1;

  debugPrintf("music: playing %s rate=%ld channels=%d\n", music_path, rate,
              channels);
  while (!music_stop_requested && buffer) {
    size_t decoded = 0;
    int result = mpg123_read(decoder, buffer, buffer_size, &decoded);
    if (decoded) {
      scale_s16(buffer, decoded, music_volume);
      const int frames = (int)(decoded / ((size_t)channels * sizeof(int16_t)));
      if (channels != 2 || opensles_movie_queue((const int16_t *)buffer,
                                                frames) != frames) {
        debugPrintf("music: shared audio queue stopped\n");
        break;
      }
    }
    if (result == MPG123_DONE) {
      if (mpg123_seek(decoder, 0, SEEK_SET) < 0)
        break;
    } else if (result != MPG123_OK && result != MPG123_NEW_FORMAT) {
      debugPrintf("music: decode failed: %s\n", mpg123_strerror(decoder));
      break;
    }
  }

  free(buffer);
  opensles_movie_end();
  mpg123_close(decoder);
  mpg123_delete(decoder);
  return 0;
}

void music_player_set_volume(float volume) {
  if (volume < 0.0f)
    volume = 0.0f;
  if (volume > 1.0f)
    volume = 1.0f;
  music_volume = volume;
}

void music_player_stop(void) {
  if (!music_thread)
    return;
  music_stop_requested = 1;
  SDL_WaitThread(music_thread, NULL);
  music_thread = NULL;
  music_stop_requested = 0;
}

int music_player_play(const char *song_name) {
  if (!song_name || !song_name[0] || strstr(song_name, "..") ||
      strchr(song_name, '/') || strchr(song_name, '\\'))
    return -1;
  music_player_stop();
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    debugPrintf("music: SDL audio init failed: %s\n", SDL_GetError());
    return -1;
  }
  if (!mpg123_initialized) {
    if (mpg123_init() != MPG123_OK)
      return -1;
    mpg123_initialized = 1;
  }
  snprintf(music_path, sizeof(music_path), "assets/music/%s.mp3", song_name);
  FILE *probe = fopen(music_path, "rb");
  if (!probe) {
    debugPrintf("music: missing %s\n", music_path);
    return -1;
  }
  fclose(probe);
  music_stop_requested = 0;
  music_thread = SDL_CreateThread(music_decode_thread, "IBMusic", NULL);
  return music_thread ? 0 : -1;
}
