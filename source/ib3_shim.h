/* ib3_shim.h -- imports required by libib3.so (Infinity Blade III runtime)
 * that the earlier Infinity Blade / libc shims did not cover.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef INFINITY_BLADE3_NX_IB3_SHIM_H
#define INFINITY_BLADE3_NX_IB3_SHIM_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* --- memory mapping (Horizon has no mmap) ------------------------------- */
void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd,
                int64_t offset);
int munmap_fake(void *addr, size_t length);
int mprotect_fake(void *addr, size_t length, int prot);
void *mremap_fake(void *old_addr, size_t old_size, size_t new_size, int flags,
                  ...);
int madvise_fake(void *addr, size_t length, int advice);

/* --- AAudio over the shared SDL mixer ------------------------------------ */
int AAudio_createStreamBuilder_fake(void **builder);
int AAudioStreamBuilder_delete_fake(void *builder);
int AAudioStreamBuilder_openStream_fake(void *builder, void **stream);
void AAudioStreamBuilder_setFormat_fake(void *builder, int32_t value);
void AAudioStreamBuilder_setChannelCount_fake(void *builder, int32_t value);
void AAudioStreamBuilder_setSamplesPerFrame_fake(void *builder, int32_t value);
void AAudioStreamBuilder_setSampleRate_fake(void *builder, int32_t value);
void AAudioStreamBuilder_setPerformanceMode_fake(void *builder, int32_t value);
void AAudioStreamBuilder_setUsage_fake(void *builder, int32_t value);
void AAudioStreamBuilder_setDirection_fake(void *builder, int32_t value);
void AAudioStreamBuilder_setSharingMode_fake(void *builder, int32_t value);
void AAudioStreamBuilder_setDeviceId_fake(void *builder, int32_t value);
void AAudioStreamBuilder_setFramesPerDataCallback_fake(void *builder,
                                                       int32_t value);
void AAudioStreamBuilder_setBufferCapacityInFrames_fake(void *builder,
                                                        int32_t value);
void AAudioStreamBuilder_setContentType_fake(void *builder, int32_t value);
void AAudioStreamBuilder_setSessionId_fake(void *builder, int32_t value);
void AAudioStreamBuilder_setInputPreset_fake(void *builder, int32_t value);
void AAudioStreamBuilder_setAllowedCapturePolicy_fake(void *builder,
                                                      int32_t value);
void AAudioStreamBuilder_setPrivacySensitive_fake(void *builder, int32_t value);
void AAudioStreamBuilder_setIsContentSpatialized_fake(void *builder,
                                                      int32_t value);
void AAudioStreamBuilder_setSpatializationBehavior_fake(void *builder,
                                                        int32_t value);
void AAudioStreamBuilder_setDataCallback_fake(void *builder, void *callback,
                                              void *user);
void AAudioStreamBuilder_setErrorCallback_fake(void *builder, void *callback,
                                               void *user);
int AAudioStream_requestStart_fake(void *stream);
int AAudioStream_requestPause_fake(void *stream);
int AAudioStream_requestStop_fake(void *stream);
int AAudioStream_requestFlush_fake(void *stream);
int AAudioStream_waitForStateChange_fake(void *stream, int32_t input_state,
                                         int32_t *next_state,
                                         int64_t timeout_ns);
int32_t AAudioStream_getState_fake(void *stream);
int32_t AAudioStream_getSampleRate_fake(void *stream);
int32_t AAudioStream_getChannelCount_fake(void *stream);
int32_t AAudioStream_getFormat_fake(void *stream);
int32_t AAudioStream_getFramesPerBurst_fake(void *stream);
int32_t AAudioStream_getFramesPerDataCallback_fake(void *stream);
int32_t AAudioStream_getBufferSizeInFrames_fake(void *stream);
int32_t AAudioStream_setBufferSizeInFrames_fake(void *stream, int32_t frames);
int32_t AAudioStream_getBufferCapacityInFrames_fake(void *stream);
int32_t AAudioStream_getXRunCount_fake(void *stream);
int32_t AAudioStream_getPerformanceMode_fake(void *stream);
int32_t AAudioStream_getUsage_fake(void *stream);
int32_t AAudioStream_getSharingMode_fake(void *stream);
int32_t AAudioStream_getDirection_fake(void *stream);
int32_t AAudioStream_getDeviceId_fake(void *stream);
int32_t AAudioStream_getSessionId_fake(void *stream);
int AAudioStream_isMMapUsed_fake(void *stream);
int64_t AAudioStream_getFramesWritten_fake(void *stream);
int64_t AAudioStream_getFramesRead_fake(void *stream);
int AAudioStream_getTimestamp_fake(void *stream, int32_t clock_id,
                                   int64_t *position, int64_t *time_ns);
int32_t AAudioStream_read_fake(void *stream, void *buffer, int32_t frames,
                               int64_t timeout_ns);
int32_t AAudioStream_write_fake(void *stream, const void *buffer,
                                int32_t frames, int64_t timeout_ns);
int AAudioStream_close_fake(void *stream);
const char *AAudio_convertResultToText_fake(int32_t result);
const char *AAudio_convertStreamStateToText_fake(int32_t state);

/* --- NDK media (cutscenes): reported as unavailable ---------------------- */
void *AMediaExtractor_new_fake(void);
int AMediaExtractor_delete_fake(void *extractor);
int AMediaExtractor_setDataSourceFd_fake(void *extractor, int fd, off_t offset,
                                         off_t length);
size_t AMediaExtractor_getTrackCount_fake(void *extractor);
void *AMediaExtractor_getTrackFormat_fake(void *extractor, size_t index);
int AMediaExtractor_selectTrack_fake(void *extractor, size_t index);
int AMediaExtractor_seekTo_fake(void *extractor, int64_t us, int mode);
ssize_t AMediaExtractor_readSampleData_fake(void *extractor, uint8_t *buffer,
                                            size_t capacity);
int64_t AMediaExtractor_getSampleTime_fake(void *extractor);
int AMediaExtractor_advance_fake(void *extractor);
int AMediaFormat_delete_fake(void *format);
int AMediaFormat_getInt32_fake(void *format, const char *name, int32_t *out);
int AMediaFormat_getInt64_fake(void *format, const char *name, int64_t *out);
int AMediaFormat_getString_fake(void *format, const char *name,
                                const char **out);
int AMediaFormat_getBuffer_fake(void *format, const char *name, void **data,
                                size_t *size);
int AMediaFormat_getRect_fake(void *format, const char *name, int32_t *left,
                              int32_t *top, int32_t *right, int32_t *bottom);
void AMediaFormat_setInt32_fake(void *format, const char *name, int32_t value);
void *AMediaCodec_createDecoderByType_fake(const char *mime);
int AMediaCodec_configure_fake(void *codec, const void *format, void *surface,
                               void *crypto, uint32_t flags);
int AMediaCodec_start_fake(void *codec);
int AMediaCodec_stop_fake(void *codec);
int AMediaCodec_flush_fake(void *codec);
int AMediaCodec_delete_fake(void *codec);
ssize_t AMediaCodec_dequeueInputBuffer_fake(void *codec, int64_t timeout_us);
uint8_t *AMediaCodec_getInputBuffer_fake(void *codec, size_t index,
                                         size_t *out_size);
int AMediaCodec_queueInputBuffer_fake(void *codec, size_t index, off_t offset,
                                      size_t size, uint64_t time, uint32_t flags);
ssize_t AMediaCodec_dequeueOutputBuffer_fake(void *codec, void *info,
                                             int64_t timeout_us);
uint8_t *AMediaCodec_getOutputBuffer_fake(void *codec, size_t index,
                                          size_t *out_size);
void *AMediaCodec_getOutputFormat_fake(void *codec);
int AMediaCodec_releaseOutputBuffer_fake(void *codec, size_t index,
                                         int render);
extern const char *ib3_amediaformat_key_width;
extern const char *ib3_amediaformat_key_height;
extern const char *ib3_amediaformat_key_mime;
extern const char *ib3_amediaformat_key_duration;
extern const char *ib3_amediaformat_key_stride;
extern const char *ib3_amediaformat_key_slice_height;
extern const char *ib3_amediaformat_key_color_format;
extern const char *ib3_amediaformat_key_display_crop;
extern const char *ib3_amediaformat_key_sample_rate;
extern const char *ib3_amediaformat_key_channel_count;
extern const char *ib3_amediaformat_key_pcm_encoding;

/* --- input --------------------------------------------------------------- */
int AInputEvent_getSource_fake(void *event);
int AKeyEvent_getAction_fake(void *event);
int AKeyEvent_getKeyCode_fake(void *event);
int AKeyEvent_getRepeatCount_fake(void *event);

/* --- misc bionic --------------------------------------------------------- */
int android_log_write_fake(int priority, const char *tag, const char *text);
int __open_2_fake(const char *path, int flags);
ssize_t __read_chk_fake(int fd, void *buf, size_t count, size_t buf_size);
ssize_t __write_chk_fake(int fd, const void *buf, size_t count,
                         size_t buf_size);
ssize_t __readlink_chk_fake(const char *path, char *buf, size_t size,
                            size_t buf_size);
ssize_t pread_fake(int fd, void *buf, size_t count, off_t offset);
ssize_t pwrite_fake(int fd, const void *buf, size_t count, off_t offset);
ssize_t __pread_chk_fake(int fd, void *buf, size_t count, off_t offset,
                         size_t buflen);
ssize_t __pwrite_chk_fake(int fd, const void *buf, size_t count, off_t offset,
                          size_t buflen);
int fsync_fake(int fd);
int ftruncate_fake(int fd, off_t length);
int truncate_fake(const char *path, off_t length);
int fchmod_fake(int fd, mode_t mode);
int fchmodat_fake(int dirfd, const char *path, mode_t mode, int flags);
int fchown_fake(int fd, uid_t owner, gid_t group);
void *fdopendir_fake(int fd);
int link_fake(const char *from, const char *to);
int symlink_fake(const char *target, const char *linkpath);
ssize_t sendfile_fake(int out_fd, int in_fd, off_t *offset, size_t count);
ssize_t getrandom_fake(void *buf, size_t length, unsigned flags);
ssize_t process_vm_readv_fake(int pid, const void *local, unsigned long lcount,
                              const void *remote, unsigned long rcount,
                              unsigned long flags);
int utimensat_fake(int dirfd, const char *path, const void *times, int flags);
int utimes_fake(const char *path, const void *times);
int sysinfo_fake(void *info);
int pthread_getattr_np_fake(unsigned long thread, void *attr);
int pthread_attr_getstack_fake(const void *attr, void **stack_addr,
                               size_t *stack_size);
int sigaction_fake(int signum, const void *act, void *oldact);
void *signal_fake(int signum, void *handler);
int sigaltstack_fake(const void *ss, void *old_ss);
/* Synthesise signal delivery from the exception handler (SIGILL etc.).
 * regs: optional x0..x30,sp,pc,pstate (34 u64s). If non-NULL and the handler
 * mutates uc_mcontext.pc, *out_pc is set so the caller can resume there.
 * Returns 1 if a handler ran, 0 otherwise. */
int ib3_deliver_signal(int signum, uint64_t fault_addr, uint64_t pc,
                       const uint64_t *regs, uint64_t *out_pc);

int ib3_reserve_guest_window(void);

#endif
