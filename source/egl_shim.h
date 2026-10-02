#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>

EGLBoolean eglChooseConfig_fake(EGLDisplay display, const EGLint *attribs,
                                EGLConfig *configs, EGLint config_size,
                                EGLint *num_config);
EGLContext eglCreateContext_fake(EGLDisplay display, EGLConfig config,
                                 EGLContext share_context,
                                 const EGLint *attribs);
EGLSurface eglCreateWindowSurface_fake(EGLDisplay display, EGLConfig config,
                                       EGLNativeWindowType window,
                                       const EGLint *attribs);
EGLDisplay eglGetDisplay_fake(EGLNativeDisplayType display_id);
EGLBoolean eglInitialize_fake(EGLDisplay display, EGLint *major, EGLint *minor);
EGLBoolean eglMakeCurrent_fake(EGLDisplay display, EGLSurface draw,
                               EGLSurface read, EGLContext context);
EGLBoolean eglQuerySurface_fake(EGLDisplay display, EGLSurface surface,
                                EGLint attribute, EGLint *value);
EGLBoolean eglSwapBuffers_fake(EGLDisplay display, EGLSurface surface);
EGLBoolean eglSwapInterval_fake(EGLDisplay display, EGLint interval);

/* Sync screen_width/height + NWindow to current applet operation mode. */
void ib3_update_display_mode(void);

/* Successful eglSwapBuffers count. Touch input is gated until this is > 0. */
uint64_t ib3_present_count(void);

void glDrawArrays_fake(GLenum mode, GLint first, GLsizei count);
void glDrawElements_fake(GLenum mode, GLsizei count, GLenum type,
                         const void *indices);
void glViewport_fake(GLint x, GLint y, GLsizei width, GLsizei height);
