#ifndef INFINITY_BLADE_NX_ANDROID_SHIM_H
#define INFINITY_BLADE_NX_ANDROID_SHIM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

void *AConfiguration_new_fake(void);
void AConfiguration_delete_fake(void *config);
void AConfiguration_fromAssetManager_fake(void *config, void *manager);
void AConfiguration_getLanguage_fake(void *config, char out[2]);
void AConfiguration_getCountry_fake(void *config, char out[2]);
int AConfiguration_getDensity_fake(void *config);

void *AAssetManager_openDir_fake(void *manager, const char *path);
const char *AAssetDir_getNextFileName_fake(void *directory);
void AAssetDir_close_fake(void *directory);

void *ALooper_prepare_fake(int options);
int ALooper_addFd_fake(void *looper, int fd, int ident, int events,
                       int (*callback)(int, int, void *), void *data);
int ALooper_pollOnce_fake(int timeout_ms, int *out_fd, int *out_events,
                          void **out_data);

int AInputQueue_attachLooper_fake(void *queue, void *looper, int ident,
                                  int (*callback)(int, int, void *), void *data);
void AInputQueue_detachLooper_fake(void *queue);
int AInputQueue_getEvent_fake(void *queue, void **event);
int AInputQueue_preDispatchEvent_fake(void *queue, void *event);
void AInputQueue_finishEvent_fake(void *queue, void *event, int handled);

int AInputEvent_getType_fake(void *event);
int AMotionEvent_getAction_fake(void *event);
int64_t AMotionEvent_getEventTime_fake(void *event);
size_t AMotionEvent_getPointerCount_fake(void *event);
int AMotionEvent_getPointerId_fake(void *event, size_t pointer_index);
float AMotionEvent_getX_fake(void *event, size_t pointer_index);
float AMotionEvent_getY_fake(void *event, size_t pointer_index);
float AMotionEvent_getAxisValue_fake(void *event, int32_t axis,
                                     size_t pointer_index);

void ANativeActivity_setWindowFlags_fake(void *activity, uint32_t add_flags,
                                         uint32_t remove_flags);

int pipe_fake(int descriptors[2]);
ssize_t read_dispatch_fake(int descriptor, void *buffer, size_t size);
ssize_t write_dispatch_fake(int descriptor, const void *buffer, size_t size);
int close_dispatch_fake(int descriptor);

#endif
