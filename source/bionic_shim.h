#ifndef INFINITY_BLADE_NX_BIONIC_SHIM_H
#define INFINITY_BLADE_NX_BIONIC_SHIM_H

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

int android_log_print_fake(int priority, const char *tag, const char *format,
                           ...);
void assert2_fake(const char *file, int line, const char *function,
                  const char *expression);
int cxa_atexit_fake(void (*destructor)(void *), void *object, void *dso);
void cxa_finalize_fake(void *dso);
int *errno_fake(void);
void exit_fake(int status) __attribute__((noreturn));

void *dlopen_fake(const char *filename, int flags);
void *dlsym_fake(void *handle, const char *symbol);
int dlclose_fake(void *handle);
char *dlerror_fake(void);

int pthread_mutex_init_fake(pthread_mutex_t **mutex, const void *attributes);
int pthread_mutex_destroy_fake(pthread_mutex_t **mutex);
int pthread_mutex_lock_fake(pthread_mutex_t **mutex);
int pthread_mutex_trylock_fake(pthread_mutex_t **mutex);
int pthread_cond_clockwait_fake(pthread_cond_t **condition,
                                pthread_mutex_t **mutex, int clock_id,
                                const struct timespec *deadline);
int pthread_mutex_unlock_fake(pthread_mutex_t **mutex);
int pthread_cond_init_fake(pthread_cond_t **condition, const void *attributes);
int pthread_cond_destroy_fake(pthread_cond_t **condition);
int pthread_cond_broadcast_fake(pthread_cond_t **condition);
int pthread_cond_signal_fake(pthread_cond_t **condition);
int pthread_cond_wait_fake(pthread_cond_t **condition,
                           pthread_mutex_t **mutex);
int pthread_cond_timedwait_fake(pthread_cond_t **condition,
                                pthread_mutex_t **mutex,
                                const struct timespec *deadline);
int pthread_once_fake(volatile int *control, void (*initializer)(void));
int pthread_key_create_fake(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete_fake(pthread_key_t key);
void *pthread_getspecific_fake(pthread_key_t key);
int pthread_setspecific_fake(pthread_key_t key, const void *value);
int pthread_detach_fake(pthread_t thread);
int pthread_create_fake(pthread_t *thread, const void *attributes,
                        void *entry, void *argument);
int pthread_attr_init_fake(void *attribute);
int pthread_attr_destroy_fake(void *attribute);
int pthread_attr_setdetachstate_fake(void *attribute, int state);
int pthread_attr_setstacksize_fake(void *attribute, size_t size);
int pthread_setname_np_fake(pthread_t thread, const char *name);
int pthread_setschedparam_fake(pthread_t thread, int policy,
                               const struct sched_param *parameter);
int sched_yield_fake(void);
void sincos_fake(double value, double *sine, double *cosine);
int clock_gettime_fake(int linux_clock_id, struct timespec *ts);
int nanosleep_fake(const struct timespec *request, struct timespec *remain);
int usleep_fake(unsigned int microseconds);
unsigned int sleep_fake(unsigned int seconds);

#endif

