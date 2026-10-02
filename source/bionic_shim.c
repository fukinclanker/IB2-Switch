#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <switch.h>

#include "bionic_shim.h"
#include "blocktrace.h"
#include "imports.h"
#include "opensles.h"
#include "so_util.h"
#include "util.h"

extern so_module game_mod;
static int opensles_handle_token;
static _Thread_local const char *last_dlerror;

int android_log_print_fake(int priority, const char *tag, const char *format,
                           ...) {
  (void)priority;
  char message[0x1000];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  debugPrintf("%s: %s\n", tag ? tag : "Android", message);
  return 0;
}

void assert2_fake(const char *file, int line, const char *function,
                  const char *expression) {
  debugPrintf("assertion failed: %s:%d (%s): %s\n", file ? file : "?", line,
              function ? function : "?", expression ? expression : "?");
  abort();
}

int cxa_atexit_fake(void (*destructor)(void *), void *object, void *dso) {
  (void)destructor;
  (void)object;
  (void)dso;
  return 0;
}

void cxa_finalize_fake(void *dso) { (void)dso; }

int *errno_fake(void) { return &errno; }

void exit_fake(int status) { exit(status); }

void *dlopen_fake(const char *filename, int flags) {
  (void)flags;
  if (!filename)
    return &game_mod;
  if (strstr(filename, "ib3") || strstr(filename, "IB3"))
    return &game_mod;
  if (strstr(filename, "libOpenSLES.so")) {
    debugPrintf("dlopen: OpenSL ES compatibility library ready\n");
    return &opensles_handle_token;
  }
  /* System Android libraries are represented by the static import table. */
  if (strstr(filename, "libc.so") || strstr(filename, "libm.so") ||
      strstr(filename, "libdl.so") || strstr(filename, "liblog.so") ||
      strstr(filename, "libandroid.so") || strstr(filename, "libEGL.so") ||
      strstr(filename, "libaaudio.so") || strstr(filename, "libmediandk.so") ||
      strstr(filename, "libGLESv2.so") || strstr(filename, "libz.so"))
    return (void *)1;
  debugPrintf("dlopen: unsupported library %s\n", filename);
  last_dlerror = "unsupported library in compatibility wrapper";
  return NULL;
}

void *dlsym_fake(void *handle, const char *symbol) {
  if (!symbol)
    return NULL;
  if (handle == &opensles_handle_token) {
    if (!strcmp(symbol, "slCreateEngine")) {
      debugPrintf("dlsym: OpenSL slCreateEngine -> %p\n", &slCreateEngine);
      return (void *)&slCreateEngine;
    }
#define RETURN_OPENSL_IID(name)                                                \
    if (!strcmp(symbol, "SL_IID_" #name)) {                                  \
      debugPrintf("dlsym: OpenSL SL_IID_" #name " -> %p\n",                 \
                  &SL_IID_##name);                                             \
      return (void *)&SL_IID_##name;                                           \
    }
    RETURN_OPENSL_IID(ENGINE);
    RETURN_OPENSL_IID(PLAY);
    RETURN_OPENSL_IID(RECORD);
    RETURN_OPENSL_IID(ANDROIDSIMPLEBUFFERQUEUE);
    RETURN_OPENSL_IID(ANDROIDCONFIGURATION);
    RETURN_OPENSL_IID(BUFFERQUEUE);
    RETURN_OPENSL_IID(VOLUME);
    RETURN_OPENSL_IID(OBJECT);
    RETURN_OPENSL_IID(OUTPUTMIX);
    RETURN_OPENSL_IID(PLAYBACKRATE);
#undef RETURN_OPENSL_IID
  } else if (handle == &game_mod) {
    uintptr_t address = so_try_find_addr_rx(&game_mod, symbol);
    if (address)
      return (void *)address;
  } else {
    uintptr_t address = so_try_find_addr_rx(&game_mod, symbol);
    if (address)
      return (void *)address;
  }
  /* The guest runtime looks up EGL/GL entry points by name at run time
   * (dlsym). Serve them from the same shim table used for load-time imports,
   * then fall back to the driver for any other gl* function. */
  {
    DynLibFunction *imp = so_find_import(dynlib_functions,
                                         (int)dynlib_numfunctions, symbol);
    if (imp && imp->func) {
      debugPrintf("dlsym: %s -> import table %p\n", symbol,
                  (void *)imp->func);
      return (void *)imp->func;
    }
  }
  if (symbol[0] == 'g' && symbol[1] == 'l') {
    void *proc = (void *)eglGetProcAddress(symbol);
    if (proc) {
      debugPrintf("dlsym: %s -> eglGetProcAddress %p\n", symbol, proc);
      return proc;
    }
  }
  debugPrintf("dlsym: unresolved %s\n", symbol);
  last_dlerror = "symbol unavailable in compatibility wrapper";
  return NULL;
}

int dlclose_fake(void *handle) {
  (void)handle;
  return 0;
}

char *dlerror_fake(void) {
  const char *error = last_dlerror;
  last_dlerror = NULL;
  return (char *)error;
}

/* Guest code uses zero-initialised (static) mutexes/conditions, so the native
 * object is created lazily on first use. Two threads can reach a fresh object
 * at the same moment; without serialisation each gets its own copy and wakeups
 * are lost. The guest's lock slots are only 4-byte aligned, so 64-bit atomic
 * instructions (ldar/cas) fault on them: use a global lock for creation and
 * 32-bit halves (lo published first, hi released last) for the lock-free
 * fast path. */
static pthread_mutex_t g_lazy_init_lock = PTHREAD_MUTEX_INITIALIZER;

static inline uintptr_t slot_read(void *slot) {
  volatile uint32_t *w = (volatile uint32_t *)slot;
  const uint32_t hi = __atomic_load_n(&w[1], __ATOMIC_ACQUIRE);
  const uint32_t lo = __atomic_load_n(&w[0], __ATOMIC_RELAXED);
  return ((uintptr_t)hi << 32) | lo;
}

static inline void slot_write(void *slot, uintptr_t v) {
  volatile uint32_t *w = (volatile uint32_t *)slot;
  __atomic_store_n(&w[0], (uint32_t)v, __ATOMIC_RELAXED);
  __atomic_store_n(&w[1], (uint32_t)(v >> 32), __ATOMIC_RELEASE);
}

static int ensure_mutex(pthread_mutex_t **mutex) {
  if (!mutex)
    return EINVAL;
  uintptr_t cur = slot_read(mutex);
  if ((cur >> 32) != 0 && cur > 0x8000)
    return 0;
  pthread_mutex_lock(&g_lazy_init_lock);
  cur = slot_read(mutex);
  if (cur > 0x8000) {
    pthread_mutex_unlock(&g_lazy_init_lock);
    return 0;
  }
  pthread_mutex_t *native = calloc(1, sizeof(*native));
  if (!native) {
    pthread_mutex_unlock(&g_lazy_init_lock);
    return ENOMEM;
  }
  *native = (pthread_mutex_t)PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
  slot_write(mutex, (uintptr_t)native);
  pthread_mutex_unlock(&g_lazy_init_lock);
  return 0;
}

int pthread_mutex_init_fake(pthread_mutex_t **mutex, const void *attributes) {
  (void)attributes;
  if (mutex)
    *mutex = NULL;
  return ensure_mutex(mutex);
}

int pthread_mutex_destroy_fake(pthread_mutex_t **mutex) {
  if (mutex && *mutex && (uintptr_t)*mutex > 0x8000) {
    pthread_mutex_destroy(*mutex);
    free(*mutex);
    *mutex = NULL;
  }
  return 0;
}

int pthread_mutex_lock_fake(pthread_mutex_t **mutex) {
  int result = ensure_mutex(mutex);
  if (result)
    return result;
  /* Uncontended fast path is not traced; only real waits are. */
  if (pthread_mutex_trylock(*mutex) == 0)
    return 0;
  bt_enter("mutex_lock", *mutex, __builtin_return_address(0), -1);
  result = pthread_mutex_lock(*mutex);
  bt_leave();
  return result;
}

int pthread_mutex_unlock_fake(pthread_mutex_t **mutex) {
  int result = ensure_mutex(mutex);
  return result ? result : pthread_mutex_unlock(*mutex);
}

static int ensure_condition(pthread_cond_t **condition) {
  if (!condition)
    return EINVAL;
  uintptr_t cur = slot_read(condition);
  if ((cur >> 32) != 0)
    return 0;
  pthread_mutex_lock(&g_lazy_init_lock);
  cur = slot_read(condition);
  if (cur != 0) {
    pthread_mutex_unlock(&g_lazy_init_lock);
    return 0;
  }
  pthread_cond_t *native = calloc(1, sizeof(*native));
  if (!native) {
    pthread_mutex_unlock(&g_lazy_init_lock);
    return ENOMEM;
  }
  int result = pthread_cond_init(native, NULL);
  if (result) {
    free(native);
    pthread_mutex_unlock(&g_lazy_init_lock);
    return result;
  }
  slot_write(condition, (uintptr_t)native);
  pthread_mutex_unlock(&g_lazy_init_lock);
  return 0;
}

int pthread_cond_init_fake(pthread_cond_t **condition, const void *attributes) {
  (void)attributes;
  if (condition)
    *condition = NULL;
  return ensure_condition(condition);
}

int pthread_cond_destroy_fake(pthread_cond_t **condition) {
  if (condition && *condition) {
    pthread_cond_destroy(*condition);
    free(*condition);
    *condition = NULL;
  }
  return 0;
}

int pthread_cond_broadcast_fake(pthread_cond_t **condition) {
  int result = ensure_condition(condition);
  return result ? result : pthread_cond_broadcast(*condition);
}

int pthread_cond_signal_fake(pthread_cond_t **condition) {
  int result = ensure_condition(condition);
  return result ? result : pthread_cond_signal(*condition);
}

int pthread_cond_wait_fake(pthread_cond_t **condition,
                           pthread_mutex_t **mutex) {
  int result = ensure_condition(condition);
  if (!result)
    result = ensure_mutex(mutex);
  {
    static volatile unsigned cond_logs;
    if (__atomic_fetch_add(&cond_logs, 1, __ATOMIC_RELAXED) < 48)
      debugPrintf("wait: pthread_cond_wait thread=%lx caller=%p\n",
                  (unsigned long)pthread_self(), __builtin_return_address(0));
  }
  if (result)
    return result;
  bt_enter("cond_wait", *condition, __builtin_return_address(0), -1);
  result = pthread_cond_wait(*condition, *mutex);
  bt_leave();
  return result;
}

int pthread_cond_timedwait_fake(pthread_cond_t **condition,
                                pthread_mutex_t **mutex,
                                const struct timespec *deadline) {
  int result = ensure_condition(condition);
  if (!result)
    result = ensure_mutex(mutex);
  if (result)
    return result;
  /* Horizon's libc numbers ETIMEDOUT as 116; Android/Linux (which the guest's
   * libc++ was built for) uses 110. If the guest sees 116 it does not
   * recognise a normal timeout and std::condition_variable throws
   * system_error, aborting the game. */
  {
    /* Log the requested wait so a bogus (far-future) deadline is visible. */
    long long remaining_ns = -1;
    struct timespec now;
    if (deadline && clock_gettime(CLOCK_REALTIME, &now) == 0)
      remaining_ns = ((long long)deadline->tv_sec - now.tv_sec) * 1000000000LL +
                     (deadline->tv_nsec - now.tv_nsec);
    bt_enter("cond_timedwait", *condition, __builtin_return_address(0),
             remaining_ns);
    result = pthread_cond_timedwait(*condition, *mutex, deadline);
    bt_leave();
  }
  return result == ETIMEDOUT ? 110 : result;
}

int pthread_once_fake(volatile int *control, void (*initializer)(void)) {
  if (!control || !initializer)
    return EINVAL;
  int state = __atomic_load_n(control, __ATOMIC_ACQUIRE);
  if (state == 2)
    return 0;
  if (state == 0 && __sync_bool_compare_and_swap(control, 0, 1)) {
    initializer();
    __atomic_store_n(control, 2, __ATOMIC_RELEASE);
    return 0;
  }
  while (__atomic_load_n(control, __ATOMIC_ACQUIRE) != 2)
    svcSleepThread(100000);
  return 0;
}

#define FAKE_KEY_COUNT 1024
#define FAKE_THREAD_COUNT 64
static volatile unsigned int next_key = 1;
static unsigned char active_keys[FAKE_KEY_COUNT];
static void *thread_ids[FAKE_THREAD_COUNT];
static void *thread_values[FAKE_THREAD_COUNT][FAKE_KEY_COUNT];

static void *thread_identity(void) {
  void *identity;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(identity));
  return identity ? identity : (void *)(uintptr_t)threadGetCurHandle();
}

static int thread_slot(int create) {
  void *identity = thread_identity();
  for (int index = 0; index < FAKE_THREAD_COUNT; index++)
    if (__atomic_load_n(&thread_ids[index], __ATOMIC_ACQUIRE) == identity)
      return index;
  if (!create)
    return -1;
  for (int index = 0; index < FAKE_THREAD_COUNT; index++) {
    void *empty = NULL;
    if (__atomic_compare_exchange_n(&thread_ids[index], &empty, identity, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      return index;
  }
  return -1;
}

int pthread_key_create_fake(pthread_key_t *key, void (*destructor)(void *)) {
  (void)destructor;
  if (!key)
    return EINVAL;
  unsigned int value = __sync_fetch_and_add(&next_key, 1);
  if (value >= FAKE_KEY_COUNT)
    return EAGAIN;
  active_keys[value] = 1;
  *key = (pthread_key_t)value;
  return 0;
}

int pthread_key_delete_fake(pthread_key_t key) {
  unsigned int value = (unsigned int)key;
  if (!value || value >= FAKE_KEY_COUNT || !active_keys[value])
    return EINVAL;
  active_keys[value] = 0;
  for (int index = 0; index < FAKE_THREAD_COUNT; index++)
    thread_values[index][value] = NULL;
  return 0;
}

void *pthread_getspecific_fake(pthread_key_t key) {
  unsigned int value = (unsigned int)key;
  int slot = thread_slot(0);
  if (!value || value >= FAKE_KEY_COUNT || !active_keys[value] || slot < 0)
    return NULL;
  return thread_values[slot][value];
}

int pthread_setspecific_fake(pthread_key_t key, const void *value) {
  unsigned int key_value = (unsigned int)key;
  int slot = thread_slot(1);
  if (!key_value || key_value >= FAKE_KEY_COUNT || !active_keys[key_value] ||
      slot < 0)
    return EINVAL;
  thread_values[slot][key_value] = (void *)value;
  return 0;
}

typedef struct {
  void *(*entry)(void *);
  void *argument;
  uint8_t tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
} ThreadStart;

static void *thread_trampoline(void *opaque) {
  ThreadStart *start = opaque;
  void *(*entry)(void *) = start->entry;
  void *argument = start->argument;
  thread_registry_add();
  install_bionic_tls(start->tls);
  return entry(argument);
}

int pthread_create_fake(pthread_t *thread, const void *attributes,
                        void *entry, void *argument) {
  (void)attributes;
  ThreadStart *start = calloc(1, sizeof(*start));
  if (!start)
    return ENOMEM;
  start->entry = (void *(*)(void *))entry;
  start->argument = argument;
  pthread_attr_t native_attributes;
  int result = pthread_attr_init(&native_attributes);
  const int attributes_initialized = result == 0;
  if (attributes_initialized)
    result = pthread_attr_setstacksize(&native_attributes, 2 * 1024 * 1024);
  if (!result)
    result = pthread_create(thread, &native_attributes, thread_trampoline, start);
  if (attributes_initialized)
    pthread_attr_destroy(&native_attributes);
  if (result)
    free(start);
  return result;
}

/* Horizon's libc reports ENOSYS for pthread_detach, which makes libc++'s
 * std::thread::detach() throw and abort the game. A thread that is never
 * joined simply ends on its own, so treat "not implemented" as success. */
int pthread_detach_fake(pthread_t thread) {
  const int result = pthread_detach(thread);
  return result == ENOSYS ? 0 : result;
}

int pthread_setname_np_fake(pthread_t thread, const char *name) {
  (void)thread;
  (void)name;
  return 0;
}

int pthread_setschedparam_fake(pthread_t thread, int policy,
                               const struct sched_param *parameter) {
  (void)thread;
  (void)policy;
  (void)parameter;
  return 0;
}

int sched_yield_fake(void) {
  svcSleepThread(0);
  return 0;
}

void sincos_fake(double value, double *sine, double *cosine) {
  if (sine)
    *sine = sin(value);
  if (cosine)
    *cosine = cos(value);
}

int pthread_mutex_trylock_fake(pthread_mutex_t **mutex) {
  int result = ensure_mutex(mutex);
  return result ? result : pthread_mutex_trylock(*mutex);
}

/* bionic's clockwait takes an explicit clock id; newlib's timedwait has no
 * such parameter. The deadline is interpreted on the condition's default
 * clock, which is what the shimmed condvars use. */
int pthread_cond_clockwait_fake(pthread_cond_t **condition,
                                pthread_mutex_t **mutex, int clock_id,
                                const struct timespec *deadline) {
  /* Linux clock ids: CLOCK_REALTIME=0, CLOCK_MONOTONIC=1. The underlying wait
   * is on the realtime clock, so re-base a monotonic deadline onto it. */
  if (clock_id == 1 && deadline) {
    struct timespec mono, real, adjusted = *deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &mono) == 0 &&
        clock_gettime(CLOCK_REALTIME, &real) == 0) {
      adjusted.tv_sec += real.tv_sec - mono.tv_sec;
      adjusted.tv_nsec += real.tv_nsec - mono.tv_nsec;
      while (adjusted.tv_nsec < 0) {
        adjusted.tv_nsec += 1000000000L;
        adjusted.tv_sec--;
      }
      while (adjusted.tv_nsec >= 1000000000L) {
        adjusted.tv_nsec -= 1000000000L;
        adjusted.tv_sec++;
      }
      return pthread_cond_timedwait_fake(condition, mutex, &adjusted);
    }
  }
  return pthread_cond_timedwait_fake(condition, mutex, deadline);
}

/* ---- Android <-> Horizon clock ids -------------------------------------
 * The guest passes Linux clock ids (REALTIME=0, MONOTONIC=1, PROCESS_CPUTIME=2,
 * THREAD_CPUTIME=3, MONOTONIC_RAW=4, REALTIME_COARSE=5, MONOTONIC_COARSE=6,
 * BOOTTIME=7). newlib on Horizon numbers them differently (REALTIME=1,
 * MONOTONIC=4), so forwarding the raw id made the guest's "monotonic" clock the
 * wall clock and its "realtime" clock invalid (EINVAL, timespec untouched).
 * pthread_cond_clockwait_fake then re-based a wall-clock "monotonic" deadline by
 * (realtime - monotonic), producing a deadline decades away: a timed wait that
 * never returns. */
int clock_gettime_fake(int linux_clock_id, struct timespec *ts) {
  clockid_t host;
  switch (linux_clock_id) {
    case 0: /* CLOCK_REALTIME */
    case 5: /* CLOCK_REALTIME_COARSE */
      host = CLOCK_REALTIME;
      break;
    case 1: /* CLOCK_MONOTONIC */
    case 2: /* CLOCK_PROCESS_CPUTIME_ID: not defined by devkitA64 newlib */
    case 3: /* CLOCK_THREAD_CPUTIME_ID: not defined by devkitA64 newlib */
    case 4: /* CLOCK_MONOTONIC_RAW */
    case 6: /* CLOCK_MONOTONIC_COARSE */
    case 7: /* CLOCK_BOOTTIME */
      host = CLOCK_MONOTONIC;
      break;
    default:
      errno = EINVAL;
      return -1;
  }
  return clock_gettime(host, ts);
}

/* Sleeps are traced so polling loops ("while (!flag) usleep(n)") show up. */
int nanosleep_fake(const struct timespec *request, struct timespec *remain) {
  const long long ns = request ? (long long)request->tv_sec * 1000000000LL +
                                     request->tv_nsec
                               : 0;
  bt_enter("nanosleep", NULL, __builtin_return_address(0), ns);
  int result = nanosleep(request, remain);
  bt_leave();
  return result;
}

int usleep_fake(unsigned int microseconds) {
  bt_enter("usleep", NULL, __builtin_return_address(0),
           (long long)microseconds * 1000LL);
  int result = usleep(microseconds);
  bt_leave();
  return result;
}

unsigned int sleep_fake(unsigned int seconds) {
  bt_enter("sleep", NULL, __builtin_return_address(0),
           (long long)seconds * 1000000000LL);
  unsigned int result = sleep(seconds);
  bt_leave();
  return result;
}
