#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "android_shim.h"
#include "config.h"
#include "controller_input.h"
#include "egl_shim.h"
#include "util.h"

#define ALOOPER_POLL_TIMEOUT (-3)

typedef struct {
  DIR *handle;
  char current[NAME_MAX + 1];
} FakeAssetDir;

typedef struct {
  int type;
  int action;
  int64_t event_time_ns;
  size_t pointer_count;
  struct {
    int id;
    float x;
    float y;
  } pointers[10];
} FakeInputEvent;

#define FAKE_PIPE_BASE 0x70000000
#define FAKE_PIPE_COUNT 32
#define FAKE_PIPE_CAPACITY 4096

typedef struct {
  int descriptor;
  int ident;
  int events;
  int (*callback)(int, int, void *);
  void *data;
} FakeLooperEntry;

typedef struct {
  FakeLooperEntry entries[8];
  int count;
  int input_ident;
  int input_events;
  int (*input_callback)(int, int, void *);
  void *input_data;
} FakeLooper;

typedef struct {
  int used;
  int read_open;
  int write_open;
  unsigned char bytes[FAKE_PIPE_CAPACITY];
  size_t head;
  size_t count;
  pthread_mutex_t mutex;
} FakePipe;

static _Thread_local FakeLooper fake_looper;
static FakePipe fake_pipes[FAKE_PIPE_COUNT];
static pthread_mutex_t pipe_table_mutex = PTHREAD_MUTEX_INITIALIZER;
static FakeInputEvent *pending_input_event;
static int touch_initialized;
static int touch_active;
static float touch_x;
static float touch_y;

static int publish_controller_event(const ControllerTouchEvent *source) {
  FakeInputEvent *event = calloc(1, sizeof(*event));
  if (!event)
    return 0;
  event->type = 2;
  event->action = source->action;
  event->event_time_ns = source->event_time_ns;
  event->pointer_count = 1;
  event->pointers[0].id = 0;
  event->pointers[0].x = source->x;
  event->pointers[0].y = source->y;
  pending_input_event = event;
  return 1;
}

static int poll_touchscreen(void) {
  if (pending_input_event)
    return 1;

  if (!touch_initialized) {
    hidInitializeTouchScreen();
    touch_initialized = 1;
  }

  /* Engine is not safe for input until it has presented once. While gated we
   * still sample HID so our local touch_active/x/y stay in sync; we only
   * suppress delivery into the guest. That way a finger held through boot
   * does not produce a stale DOWN the moment the gate opens, and the user
   * does not need to avoid the screen during load. */
  const int engine_ready = ib3_present_count() > 0;
  if (!engine_ready) {
    static int gated_logged;
    if (!gated_logged) {
      gated_logged = 1;
      debugPrintf("touch: input held until first present (auto; no user action needed)\n");
    }
    HidTouchScreenState warm;
    memset(&warm, 0, sizeof(warm));
    if (hidGetTouchScreenStates(&warm, 1) > 0) {
      const HidTouchState *t =
          warm.count > 0 ? &warm.touches[0] : NULL;
      const int ended = t && (t->attributes & HidTouchAttribute_End);
      const int active = t != NULL && !ended;
      if (active) {
        touch_active = 1;
        touch_x = (float)t->x * (float)screen_width / 1280.0f;
        touch_y = (float)t->y * (float)screen_height / 720.0f;
      } else {
        touch_active = 0;
      }
    }
    return 0;
  }

  ControllerTouchEvent controller_event;
  if (controller_input_touch_active()) {
    if (controller_input_poll(&controller_event, 1))
      return publish_controller_event(&controller_event);
    return 0;
  }
  HidTouchScreenState state;
  memset(&state, 0, sizeof(state));
  const size_t states_read = hidGetTouchScreenStates(&state, 1);
  // No returned HID snapshot means that there is no new state to process. It
  // does not mean that an existing finger was released.
  if (states_read == 0) {
    if (controller_input_poll(&controller_event, !touch_active))
      return publish_controller_event(&controller_event);
    return 0;
  }
  const HidTouchState *touch =
      state.count > 0 ? &state.touches[0] : NULL;
  const int ended = touch && (touch->attributes & HidTouchAttribute_End);
  int active = touch != NULL && !ended;
  float next_x = touch ? (float)touch->x * (float)screen_width / 1280.0f
                       : touch_x;
  float next_y = touch ? (float)touch->y * (float)screen_height / 720.0f
                       : touch_y;

  int action = -1;
  if (active && !touch_active)
    action = 0;
  else if (active && touch_active &&
           (next_x != touch_x || next_y != touch_y))
    action = 2;
  else if (!active && touch_active)
    action = 1;
  if (action < 0) {
    if (controller_input_poll(&controller_event, !touch_active))
      return publish_controller_event(&controller_event);
    return 0;
  }

  FakeInputEvent *event = calloc(1, sizeof(*event));
  if (!event)
    return 0;
  event->type = 2;
  event->action = action;
  event->event_time_ns = (int64_t)armTicksToNs(armGetSystemTick());
  event->pointer_count = 1;
  event->pointers[0].id = 0;
  event->pointers[0].x = next_x;
  event->pointers[0].y = next_y;
  pending_input_event = event;
  touch_active = active;
  touch_x = next_x;
  touch_y = next_y;
  if (action != 2)
    debugPrintf("touch: %s x=%.1f y=%.1f sample=%llu attributes=0x%x\n",
                action == 0 ? "DOWN" : "UP", next_x, next_y,
                (unsigned long long)state.sampling_number,
                touch ? touch->attributes : 0);
  return 1;
}

static FakePipe *pipe_for_descriptor(int descriptor, int *write_end) {
  if (descriptor < FAKE_PIPE_BASE)
    return NULL;
  int index = (descriptor - FAKE_PIPE_BASE) / 2;
  if (index < 0 || index >= FAKE_PIPE_COUNT || !fake_pipes[index].used)
    return NULL;
  if (write_end)
    *write_end = (descriptor - FAKE_PIPE_BASE) & 1;
  return &fake_pipes[index];
}

int pipe_fake(int descriptors[2]) {
  if (!descriptors) {
    errno = EFAULT;
    return -1;
  }
  pthread_mutex_lock(&pipe_table_mutex);
  for (int index = 0; index < FAKE_PIPE_COUNT; index++) {
    FakePipe *pipe = &fake_pipes[index];
    if (pipe->used)
      continue;
    memset(pipe, 0, sizeof(*pipe));
    pthread_mutex_init(&pipe->mutex, NULL);
    pipe->used = 1;
    pipe->read_open = 1;
    pipe->write_open = 1;
    descriptors[0] = FAKE_PIPE_BASE + index * 2;
    descriptors[1] = descriptors[0] + 1;
    pthread_mutex_unlock(&pipe_table_mutex);
    return 0;
  }
  pthread_mutex_unlock(&pipe_table_mutex);
  errno = EMFILE;
  return -1;
}

ssize_t read_dispatch_fake(int descriptor, void *buffer, size_t size) {
  int write_end = 0;
  FakePipe *pipe = pipe_for_descriptor(descriptor, &write_end);
  if (!pipe)
    return read(descriptor, buffer, size);
  if (write_end || !pipe->read_open) {
    errno = EBADF;
    return -1;
  }
  pthread_mutex_lock(&pipe->mutex);
  if (!pipe->count) {
    int writer_closed = !pipe->write_open;
    pthread_mutex_unlock(&pipe->mutex);
    if (writer_closed)
      return 0;
    errno = EAGAIN;
    return -1;
  }
  if (size > pipe->count)
    size = pipe->count;
  for (size_t index = 0; index < size; index++)
    ((unsigned char *)buffer)[index] =
        pipe->bytes[(pipe->head + index) % FAKE_PIPE_CAPACITY];
  pipe->head = (pipe->head + size) % FAKE_PIPE_CAPACITY;
  pipe->count -= size;
  pthread_mutex_unlock(&pipe->mutex);
  return (ssize_t)size;
}

ssize_t write_dispatch_fake(int descriptor, const void *buffer, size_t size) {
  int write_end = 0;
  FakePipe *pipe = pipe_for_descriptor(descriptor, &write_end);
  if (!pipe)
    return write(descriptor, buffer, size);
  if (!write_end || !pipe->write_open || !pipe->read_open) {
    errno = EPIPE;
    return -1;
  }
  pthread_mutex_lock(&pipe->mutex);
  size_t available = FAKE_PIPE_CAPACITY - pipe->count;
  if (size > available)
    size = available;
  for (size_t index = 0; index < size; index++)
    pipe->bytes[(pipe->head + pipe->count + index) % FAKE_PIPE_CAPACITY] =
        ((const unsigned char *)buffer)[index];
  pipe->count += size;
  pthread_mutex_unlock(&pipe->mutex);
  if (!size) {
    errno = EAGAIN;
    return -1;
  }
  return (ssize_t)size;
}

int close_dispatch_fake(int descriptor) {
  int write_end = 0;
  FakePipe *pipe = pipe_for_descriptor(descriptor, &write_end);
  if (!pipe)
    return close(descriptor);
  pthread_mutex_lock(&pipe->mutex);
  if (write_end)
    pipe->write_open = 0;
  else
    pipe->read_open = 0;
  int release = !pipe->read_open && !pipe->write_open;
  pthread_mutex_unlock(&pipe->mutex);
  if (release) {
    pthread_mutex_lock(&pipe_table_mutex);
    pthread_mutex_destroy(&pipe->mutex);
    memset(pipe, 0, sizeof(*pipe));
    pthread_mutex_unlock(&pipe_table_mutex);
  }
  return 0;
}

void *AConfiguration_new_fake(void) { return calloc(1, 8); }

void AConfiguration_delete_fake(void *config) { free(config); }

void AConfiguration_fromAssetManager_fake(void *config, void *manager) {
  (void)config;
  (void)manager;
}

void AConfiguration_getLanguage_fake(void *config, char out[2]) {
  (void)config;
  if (out) {
    out[0] = 'e';
    out[1] = 'n';
  }
}

void AConfiguration_getCountry_fake(void *config, char out[2]) {
  (void)config;
  if (out) {
    out[0] = 'U';
    out[1] = 'S';
  }
}

int AConfiguration_getDensity_fake(void *config) {
  (void)config;
  return 160;
}

void *AAssetManager_openDir_fake(void *manager, const char *path) {
  (void)manager;
  char resolved[0x500];
  if (path && path[0])
    snprintf(resolved, sizeof(resolved), "assets/%s", path);
  else
    strlcpy(resolved, "assets", sizeof(resolved));

  DIR *handle = opendir(resolved);
  if (!handle)
    return NULL;
  FakeAssetDir *directory = calloc(1, sizeof(*directory));
  if (!directory) {
    closedir(handle);
    return NULL;
  }
  directory->handle = handle;
  return directory;
}

const char *AAssetDir_getNextFileName_fake(void *directory_ptr) {
  FakeAssetDir *directory = directory_ptr;
  if (!directory || !directory->handle)
    return NULL;
  for (;;) {
    struct dirent *entry = readdir(directory->handle);
    if (!entry)
      return NULL;
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
        !strcmp(entry->d_name, ".DS_Store"))
      continue;
    strlcpy(directory->current, entry->d_name, sizeof(directory->current));
    return directory->current;
  }
}

void AAssetDir_close_fake(void *directory_ptr) {
  FakeAssetDir *directory = directory_ptr;
  if (!directory)
    return;
  if (directory->handle)
    closedir(directory->handle);
  free(directory);
}

void *ALooper_prepare_fake(int options) {
  (void)options;
  return &fake_looper;
}

int ALooper_addFd_fake(void *looper, int fd, int ident, int events,
                       int (*callback)(int, int, void *), void *data) {
  FakeLooper *instance = looper ? looper : &fake_looper;
  if (instance->count >= (int)(sizeof(instance->entries) /
                              sizeof(instance->entries[0])))
    return -1;
  FakeLooperEntry *entry = &instance->entries[instance->count++];
  entry->descriptor = fd;
  entry->ident = ident;
  entry->events = events;
  entry->callback = callback;
  entry->data = data;
  return 1;
}

int ALooper_pollOnce_fake(int timeout_ms, int *out_fd, int *out_events,
                          void **out_data) {
  if (out_fd)
    *out_fd = -1;
  if (out_events)
    *out_events = 0;
  if (out_data)
    *out_data = NULL;
  for (;;) {
    if (fake_looper.input_ident && poll_touchscreen()) {
      if (fake_looper.input_callback)
        return fake_looper.input_callback(-1, fake_looper.input_events,
                                          fake_looper.input_data)
                   ? -2
                   : ALOOPER_POLL_TIMEOUT;
      if (out_events)
        *out_events = fake_looper.input_events;
      if (out_data)
        *out_data = fake_looper.input_data;
      return fake_looper.input_ident;
    }
    for (int index = 0; index < fake_looper.count; index++) {
      FakeLooperEntry *entry = &fake_looper.entries[index];
      FakePipe *pipe = pipe_for_descriptor(entry->descriptor, NULL);
      if (!pipe || !pipe->count)
        continue;
      if (entry->callback)
        return entry->callback(entry->descriptor, entry->events, entry->data)
                   ? -2
                   : ALOOPER_POLL_TIMEOUT;
      if (out_fd)
        *out_fd = entry->descriptor;
      if (out_events)
        *out_events = entry->events;
      if (out_data)
        *out_data = entry->data;
      return entry->ident;
    }
    if (timeout_ms == 0)
      break;
    int sleep_ms = timeout_ms < 0 ? 1 : timeout_ms;
    svcSleepThread((int64_t)sleep_ms * 1000000LL);
    break;
  }
  return ALOOPER_POLL_TIMEOUT;
}

int AInputQueue_attachLooper_fake(void *queue, void *looper, int ident,
                                  int (*callback)(int, int, void *), void *data) {
  (void)queue;
  FakeLooper *instance = looper ? looper : &fake_looper;
  instance->input_ident = ident;
  instance->input_events = 1;
  instance->input_callback = callback;
  instance->input_data = data;
  return 0;
}

void AInputQueue_detachLooper_fake(void *queue) {
  (void)queue;
  fake_looper.input_ident = 0;
  fake_looper.input_callback = NULL;
  fake_looper.input_data = NULL;
}

int AInputQueue_getEvent_fake(void *queue, void **event) {
  (void)queue;
  if (!pending_input_event)
    poll_touchscreen();
  if (!pending_input_event) {
    if (event)
      *event = NULL;
    errno = EAGAIN;
    return -1;
  }
  if (event)
    *event = pending_input_event;
  pending_input_event = NULL;
  return 0;
}

int AInputQueue_preDispatchEvent_fake(void *queue, void *event) {
  (void)queue;
  (void)event;
  return 0;
}

void AInputQueue_finishEvent_fake(void *queue, void *event, int handled) {
  (void)queue;
  (void)handled;
  free(event);
}

int AInputEvent_getType_fake(void *event_ptr) {
  const FakeInputEvent *event = event_ptr;
  return event ? event->type : 0;
}

int AMotionEvent_getAction_fake(void *event_ptr) {
  const FakeInputEvent *event = event_ptr;
  return event ? event->action : 0;
}

int64_t AMotionEvent_getEventTime_fake(void *event_ptr) {
  const FakeInputEvent *event = event_ptr;
  return event ? event->event_time_ns : 0;
}

size_t AMotionEvent_getPointerCount_fake(void *event_ptr) {
  const FakeInputEvent *event = event_ptr;
  return event ? event->pointer_count : 0;
}

int AMotionEvent_getPointerId_fake(void *event_ptr, size_t pointer_index) {
  const FakeInputEvent *event = event_ptr;
  return event && pointer_index < event->pointer_count
             ? event->pointers[pointer_index].id
             : -1;
}

float AMotionEvent_getX_fake(void *event_ptr, size_t pointer_index) {
  const FakeInputEvent *event = event_ptr;
  return event && pointer_index < event->pointer_count
             ? event->pointers[pointer_index].x
             : 0.0f;
}

float AMotionEvent_getY_fake(void *event_ptr, size_t pointer_index) {
  const FakeInputEvent *event = event_ptr;
  return event && pointer_index < event->pointer_count
             ? event->pointers[pointer_index].y
             : 0.0f;
}

float AMotionEvent_getAxisValue_fake(void *event_ptr, int32_t axis,
                                     size_t pointer_index) {
  /* Touch path only reports X/Y; joystick axes are unused. */
  (void)event_ptr;
  (void)axis;
  (void)pointer_index;
  return 0.0f;
}

void ANativeActivity_setWindowFlags_fake(void *activity, uint32_t add_flags,
                                         uint32_t remove_flags) {
  (void)activity;
  (void)add_flags;
  (void)remove_flags;
}
