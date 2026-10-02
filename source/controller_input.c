#include "controller_input.h"

#include <GLES2/gl2.h>
#include <math.h>
#include <png.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "util.h"

#define TAP_NS 45000000ULL
#define SWIPE_STEP_NS 16000000ULL
#define CURSOR_SPEED 900.0f
#define STICK_DEADZONE 0.18f
#define SWIPE_START 0.58f
#define SWIPE_REARM 0.25f
#define LEFT_STICK_SWIPE_REACH 0.23f
#define RIGHT_STICK_SWIPE_REACH 0.23f
#define CURSOR_CAMERA_SWIPE_REACH 0.18f

enum GestureKind {
  GESTURE_NONE,
  GESTURE_TAP,
  GESTURE_HOLD,
  GESTURE_SWIPE,
};

typedef struct {
  enum GestureKind kind;
  int stage;
  u64 button;
  uint64_t next_ns;
  const char *name;
  float start_x;
  float start_y;
  float end_x;
  float end_y;
} ControllerGesture;

static PadState controller_pad;
static int controller_ready;
static uint64_t last_update_ns;
static int left_swipe_armed = 1;
static int right_swipe_armed = 1;
static ControllerGesture gesture;

static pthread_mutex_t cursor_mutex = PTHREAD_MUTEX_INITIALIZER;
static int cursor_visible;
static float cursor_x;
static float cursor_y;

static uint64_t controller_now_ns(void) {
  return armTicksToNs(armGetSystemTick());
}

static float clampf(float value, float low, float high) {
  if (value < low)
    return low;
  if (value > high)
    return high;
  return value;
}

static float stick_axis(int value) {
  float axis = (float)value / 32767.0f;
  float magnitude = fabsf(axis);
  if (magnitude <= STICK_DEADZONE)
    return 0.0f;
  magnitude = (magnitude - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
  return copysignf(magnitude, axis);
}

static void controller_init(void) {
  if (controller_ready)
    return;
  padInitializeDefault(&controller_pad);
  cursor_x = (float)screen_width * 0.5f;
  cursor_y = (float)screen_height * 0.5f;
  last_update_ns = controller_now_ns();
  controller_ready = 1;
  debugPrintf("controller: initialized; R toggles dual-stick cursor mode\n");
}

static void set_event(ControllerTouchEvent *event, int action, float x,
                      float y, uint64_t now) {
  event->action = action;
  event->event_time_ns = (int64_t)now;
  event->x = x;
  event->y = y;
}

static int start_tap(ControllerTouchEvent *event, float x, float y,
                     uint64_t now, const char *name) {
  memset(&gesture, 0, sizeof(gesture));
  gesture.kind = GESTURE_TAP;
  gesture.stage = 1;
  gesture.next_ns = now + TAP_NS;
  gesture.start_x = x;
  gesture.start_y = y;
  set_event(event, 0, x, y, now);
  debugPrintf("controller: %s tap DOWN x=%.1f y=%.1f\n", name, x, y);
  return 1;
}

static int start_hold(ControllerTouchEvent *event, u64 button, float x,
                      float y, uint64_t now, const char *name) {
  memset(&gesture, 0, sizeof(gesture));
  gesture.kind = GESTURE_HOLD;
  gesture.button = button;
  gesture.start_x = x;
  gesture.start_y = y;
  set_event(event, 0, x, y, now);
  debugPrintf("controller: %s hold DOWN x=%.1f y=%.1f\n", name, x, y);
  return 1;
}

static int start_swipe(ControllerTouchEvent *event, float direction_x,
                       float direction_y, float reach, const char *name,
                       uint64_t now) {
  const float length = sqrtf(direction_x * direction_x +
                             direction_y * direction_y);
  if (length <= 0.0f)
    return 0;
  const float center_x = (float)screen_width * 0.5f;
  const float center_y = (float)screen_height * 0.5f;
  memset(&gesture, 0, sizeof(gesture));
  gesture.kind = GESTURE_SWIPE;
  gesture.stage = 1;
  gesture.next_ns = now + SWIPE_STEP_NS;
  gesture.name = name;
  gesture.start_x = center_x;
  gesture.start_y = center_y;
  gesture.end_x = clampf(center_x + direction_x / length * reach, 1.0f,
                         (float)screen_width - 2.0f);
  // Switch stick +Y is up while screen-space +Y is down.
  gesture.end_y = clampf(center_y - direction_y / length * reach, 1.0f,
                         (float)screen_height - 2.0f);
  set_event(event, 0, center_x, center_y, now);
  debugPrintf("controller: %s swipe DOWN center=%.1f,%.1f end=%.1f,%.1f\n",
              name, center_x, center_y, gesture.end_x, gesture.end_y);
  return 1;
}

static int continue_gesture(ControllerTouchEvent *event, u64 held,
                            uint64_t now) {
  if (gesture.kind == GESTURE_NONE)
    return 0;

  if (gesture.kind == GESTURE_HOLD) {
    if (held & gesture.button)
      return 0;
    set_event(event, 1, gesture.start_x, gesture.start_y, now);
    debugPrintf("controller: hold UP x=%.1f y=%.1f\n", gesture.start_x,
                gesture.start_y);
    memset(&gesture, 0, sizeof(gesture));
    return 1;
  }

  if (now < gesture.next_ns)
    return 0;

  if (gesture.kind == GESTURE_TAP) {
    set_event(event, 1, gesture.start_x, gesture.start_y, now);
    debugPrintf("controller: tap UP x=%.1f y=%.1f\n", gesture.start_x,
                gesture.start_y);
    memset(&gesture, 0, sizeof(gesture));
    return 1;
  }

  if (gesture.kind == GESTURE_SWIPE) {
    const float t = (float)gesture.stage / 3.0f;
    const float x = gesture.start_x + (gesture.end_x - gesture.start_x) * t;
    const float y = gesture.start_y + (gesture.end_y - gesture.start_y) * t;
    if (gesture.stage < 3) {
      set_event(event, 2, x, y, now);
      gesture.stage++;
      gesture.next_ns = now + SWIPE_STEP_NS;
    } else {
      set_event(event, 1, gesture.end_x, gesture.end_y, now);
      debugPrintf("controller: %s swipe UP x=%.1f y=%.1f\n",
                  gesture.name ? gesture.name : "unknown", gesture.end_x,
                  gesture.end_y);
      memset(&gesture, 0, sizeof(gesture));
    }
    return 1;
  }

  return 0;
}

static void update_cursor(HidAnalogStickState left, HidAnalogStickState right,
                          uint64_t now) {
  (void)right;
  float dt = (float)(now - last_update_ns) / 1000000000.0f;
  if (dt < 0.0f)
    dt = 0.0f;
  if (dt > 0.05f)
    dt = 0.05f;
  last_update_ns = now;

  pthread_mutex_lock(&cursor_mutex);
  if (cursor_visible) {
    // Cursor mode reserves the left stick for the pointer. The right stick is
    // handled separately as a short camera swipe.
    cursor_x += stick_axis(left.x) * CURSOR_SPEED * dt;
    cursor_y -= stick_axis(left.y) * CURSOR_SPEED * dt;
    cursor_x = clampf(cursor_x, 1.0f, (float)screen_width - 2.0f);
    cursor_y = clampf(cursor_y, 1.0f, (float)screen_height - 2.0f);
  }
  pthread_mutex_unlock(&cursor_mutex);
}

static int start_directional_button_swipe(ControllerTouchEvent *event,
                                          u64 down, uint64_t now) {
  float direction_x = 0.0f;
  float direction_y = 0.0f;
  const char *name = NULL;
  if (down & HidNpadButton_Up) {
    direction_y = 1.0f;
    name = "D-pad Up";
  } else if (down & HidNpadButton_Down) {
    direction_y = -1.0f;
    name = "D-pad Down";
  } else if (down & HidNpadButton_Left) {
    direction_x = -1.0f;
    name = "D-pad Left";
  } else if (down & HidNpadButton_Right) {
    direction_x = 1.0f;
    name = "D-pad Right";
  }

  if (!name)
    return 0;
  return start_swipe(event, direction_x, direction_y,
                     (float)screen_height * 0.26f, name, now);
}

static void toggle_cursor(void) {
  pthread_mutex_lock(&cursor_mutex);
  cursor_visible = !cursor_visible;
  if (cursor_visible) {
    cursor_x = (float)screen_width * 0.5f;
    cursor_y = (float)screen_height * 0.5f;
  }
  const int visible = cursor_visible;
  pthread_mutex_unlock(&cursor_mutex);
  debugPrintf("controller: cursor %s\n", visible ? "ON" : "OFF");
}

static void recenter_cursor(void) {
  pthread_mutex_lock(&cursor_mutex);
  if (cursor_visible) {
    cursor_x = (float)screen_width * 0.5f;
    cursor_y = (float)screen_height * 0.5f;
    debugPrintf("controller: cursor recentered by L\n");
  }
  pthread_mutex_unlock(&cursor_mutex);
}

static void cursor_snapshot(int *visible, float *x, float *y) {
  pthread_mutex_lock(&cursor_mutex);
  if (visible)
    *visible = cursor_visible;
  if (x)
    *x = cursor_x;
  if (y)
    *y = cursor_y;
  pthread_mutex_unlock(&cursor_mutex);
}

int controller_input_poll(ControllerTouchEvent *event, int allow_touch) {
  if (!event)
    return 0;
  controller_init();
  padUpdate(&controller_pad);
  const u64 held = padGetButtons(&controller_pad);
  const u64 down = padGetButtonsDown(&controller_pad);
  const uint64_t now = controller_now_ns();
  const HidAnalogStickState left = padGetStickPos(&controller_pad, 0);
  const HidAnalogStickState right = padGetStickPos(&controller_pad, 1);

  if (down & HidNpadButton_R)
    toggle_cursor();
  if (down & HidNpadButton_L)
    recenter_cursor();
  update_cursor(left, right, now);

  const float left_x = (float)left.x / 32767.0f;
  const float left_y = (float)left.y / 32767.0f;
  const float left_length = sqrtf(left_x * left_x + left_y * left_y);
  const float right_x = (float)right.x / 32767.0f;
  const float right_y = (float)right.y / 32767.0f;
  const float right_length = sqrtf(right_x * right_x + right_y * right_y);
  if (left_length < SWIPE_REARM)
    left_swipe_armed = 1;
  if (right_length < SWIPE_REARM)
    right_swipe_armed = 1;

  if (continue_gesture(event, held, now))
    return 1;
  // A timed tap or swipe may be between frames. Do not allow another control
  // to overwrite it while it is waiting to emit its next MOVE/UP event.
  if (gesture.kind != GESTURE_NONE)
    return 0;
  if (!allow_touch)
    return 0;

  int visible = 0;
  float current_cursor_x = 0.0f;
  float current_cursor_y = 0.0f;
  cursor_snapshot(&visible, &current_cursor_x, &current_cursor_y);

  const float width = (float)screen_width;
  const float height = (float)screen_height;

  // Hold-style combat controls preserve the touch until the physical button is
  // released, matching the game's on-screen button behaviour.
  if (down & HidNpadButton_B)
    return start_hold(event, HidNpadButton_B,
                      visible ? width * 0.965f : width * 0.50f,
                      height * 0.90f, now,
                      visible ? "B cursor-mode bottom-right" : "B shield");
  if (!visible && (down & HidNpadButton_ZL))
    return start_hold(event, HidNpadButton_ZL, width * 0.035f, height * 0.90f,
                      now, "ZL dodge-left");
  if (!visible && (down & HidNpadButton_ZR))
    return start_hold(event, HidNpadButton_ZR, width * 0.965f, height * 0.90f,
                      now, "ZR dodge-right");

  if (visible && (down & HidNpadButton_ZL))
    return start_hold(event, HidNpadButton_ZL, current_cursor_x,
                      current_cursor_y, now, "ZL cursor-select");
  if (visible && (down & HidNpadButton_ZR))
    return start_hold(event, HidNpadButton_ZR, current_cursor_x,
                      current_cursor_y, now, "ZR cursor-select");

  if (down & HidNpadButton_A) {
    if (visible)
      return start_hold(event, HidNpadButton_A, current_cursor_x,
                        current_cursor_y, now, "A cursor-select");
    return start_tap(event, width * 0.50f, height * 0.50f, now, "A center");
  }
  if (down & HidNpadButton_Y)
    return start_tap(event, width * 0.055f, height * 0.14f, now,
                     "Y sword");
  if (down & HidNpadButton_X)
    return start_tap(event, width * 0.945f, height * 0.14f, now,
                     "X magic");
  if ((down & HidNpadButton_Plus) && !(held & HidNpadButton_Minus))
    return start_tap(event, width * 0.50f, height * 0.055f, now,
                     "+ pause");
  if ((down & HidNpadButton_Minus) && !(held & HidNpadButton_Plus))
    return start_tap(event, width * 0.50f, height * 0.30f, now,
                     "- unpause");

  if (start_directional_button_swipe(event, down, now))
    return 1;

  // With the cursor hidden, either stick behaves as a flick gesture. Keep the
  // travel shorter than the original mapping so combat swipes are quick and
  // do not cross as much of the HUD. Each stick must return near center before
  // it can begin another swipe.
  if (!visible && left_swipe_armed && left_length >= SWIPE_START) {
    left_swipe_armed = 0;
    return start_swipe(event, left_x, left_y,
                       (float)screen_height * LEFT_STICK_SWIPE_REACH,
                       "left-stick", now);
  }
  if (!visible && right_swipe_armed && right_length >= SWIPE_START) {
    right_swipe_armed = 0;
    return start_swipe(event, right_x, right_y,
                       (float)screen_height * RIGHT_STICK_SWIPE_REACH,
                       "right-stick-camera", now);
  }
  if (visible && right_swipe_armed && right_length >= SWIPE_START) {
    right_swipe_armed = 0;
    return start_swipe(event, right_x, right_y,
                       (float)screen_height * CURSOR_CAMERA_SWIPE_REACH,
                       "right-stick-camera-cursor-mode", now);
  }

  return 0;
}

int controller_input_touch_active(void) {
  return gesture.kind != GESTURE_NONE;
}

// ---------------------------------------------------------------------------
// Cursor overlay, adapted from the state-preserving nx_pointer implementation
// used by the provided Fruit Ninja and Subway Surfers Switch ports.
// ---------------------------------------------------------------------------

static GLuint cursor_program;
static GLuint cursor_texture;
static GLint cursor_u_screen;
static GLint cursor_u_origin;
static GLint cursor_u_size;
static GLint cursor_u_texture;
static int cursor_gl_failed;

// Position then UV. Cursor x/y is the Android tap coordinate; the textured
// quad is offset around it so the hotspot sits inward from the arrow tip.
static const GLfloat cursor_quad[] = {
    0.0f, 0.0f, 0.0f, 0.0f,
    1.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f,
};

typedef struct {
  GLint enabled;
  GLint size;
  GLint type;
  GLint normalized;
  GLint stride;
  GLint buffer;
  void *pointer;
} CursorAttribState;

static GLuint compile_cursor_shader(GLenum type, const char *source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static int load_cursor_texture(void) {
  png_image image;
  memset(&image, 0, sizeof(image));
  image.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_file(&image, "cursor.png")) {
    debugPrintf("controller: cursor.png open failed: %s\n", image.message);
    return 0;
  }
  image.format = PNG_FORMAT_RGBA;
  void *pixels = malloc(PNG_IMAGE_SIZE(image));
  if (!pixels || !png_image_finish_read(&image, NULL, pixels, 0, NULL)) {
    debugPrintf("controller: cursor.png decode failed: %s\n", image.message);
    free(pixels);
    png_image_free(&image);
    return 0;
  }

  GLint old_active_texture = 0;
  GLint old_texture = 0;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active_texture);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
  glGenTextures(1, &cursor_texture);
  glBindTexture(GL_TEXTURE_2D, cursor_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)image.width,
               (GLsizei)image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  glBindTexture(GL_TEXTURE_2D, (GLuint)old_texture);
  glActiveTexture((GLenum)old_active_texture);
  free(pixels);
  debugPrintf("controller: cursor texture loaded %ux%u\n", image.width,
              image.height);
  png_image_free(&image);
  return cursor_texture != 0;
}

static int init_cursor_gl(void) {
  if (cursor_program)
    return 1;
  if (cursor_gl_failed)
    return 0;

  static const char *vertex_source =
      "attribute vec2 aPos;\n"
      "attribute vec2 aUv;\n"
      "uniform vec2 uScreen;\n"
      "uniform vec2 uOrigin;\n"
      "uniform vec2 uSize;\n"
      "varying vec2 vUv;\n"
      "void main(){\n"
      " vec2 p=uOrigin+aPos*uSize;\n"
      " gl_Position=vec4((p.x/uScreen.x)*2.0-1.0,"
      "1.0-(p.y/uScreen.y)*2.0,0.0,1.0);\n"
      " vUv=aUv;\n"
      "}\n";
  static const char *fragment_source =
      "precision mediump float;\n"
      "uniform sampler2D uTexture;\n"
      "varying vec2 vUv;\n"
      "void main(){ gl_FragColor=texture2D(uTexture,vUv); }\n";

  GLuint vertex = compile_cursor_shader(GL_VERTEX_SHADER, vertex_source);
  GLuint fragment = compile_cursor_shader(GL_FRAGMENT_SHADER, fragment_source);
  if (!vertex || !fragment) {
    if (vertex)
      glDeleteShader(vertex);
    if (fragment)
      glDeleteShader(fragment);
    cursor_gl_failed = 1;
    debugPrintf("controller: cursor shader compile failed\n");
    return 0;
  }

  cursor_program = glCreateProgram();
  glAttachShader(cursor_program, vertex);
  glAttachShader(cursor_program, fragment);
  glBindAttribLocation(cursor_program, 0, "aPos");
  glBindAttribLocation(cursor_program, 1, "aUv");
  glLinkProgram(cursor_program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint ok = 0;
  glGetProgramiv(cursor_program, GL_LINK_STATUS, &ok);
  if (!ok) {
    glDeleteProgram(cursor_program);
    cursor_program = 0;
    cursor_gl_failed = 1;
    debugPrintf("controller: cursor shader link failed\n");
    return 0;
  }

  cursor_u_screen = glGetUniformLocation(cursor_program, "uScreen");
  cursor_u_origin = glGetUniformLocation(cursor_program, "uOrigin");
  cursor_u_size = glGetUniformLocation(cursor_program, "uSize");
  cursor_u_texture = glGetUniformLocation(cursor_program, "uTexture");
  if (!load_cursor_texture()) {
    glDeleteProgram(cursor_program);
    cursor_program = 0;
    cursor_gl_failed = 1;
    return 0;
  }
  debugPrintf("controller: cursor GL overlay ready\n");
  return 1;
}

static void save_cursor_attrib(GLuint index, CursorAttribState *state) {
  glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &state->enabled);
  glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &state->size);
  glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &state->type);
  glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                      &state->normalized);
  glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &state->stride);
  glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                      &state->buffer);
  glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                            &state->pointer);
}

static void restore_cursor_attrib(GLuint index,
                                  const CursorAttribState *state) {
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)state->buffer);
  if (state->size > 0)
    glVertexAttribPointer(index, state->size, (GLenum)state->type,
                          state->normalized ? GL_TRUE : GL_FALSE,
                          state->stride, state->pointer);
  if (state->enabled)
    glEnableVertexAttribArray(index);
  else
    glDisableVertexAttribArray(index);
}

void controller_input_draw_cursor(void) {
  int visible = 0;
  float x = 0.0f;
  float y = 0.0f;
  cursor_snapshot(&visible, &x, &y);
  if (!controller_ready || !visible || !init_cursor_gl())
    return;

  CursorAttribState attributes[2];
  save_cursor_attrib(0, &attributes[0]);
  save_cursor_attrib(1, &attributes[1]);

  GLint old_framebuffer = 0;
  GLint old_program = 0;
  GLint old_buffer = 0;
  GLint old_active_texture = 0;
  GLint old_texture = 0;
  GLint old_viewport[4] = {0, 0, 0, 0};
  GLint blend_src_rgb = 0;
  GLint blend_dst_rgb = 0;
  GLint blend_src_alpha = 0;
  GLint blend_dst_alpha = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_framebuffer);
  glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_buffer);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active_texture);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
  glGetIntegerv(GL_VIEWPORT, old_viewport);
  glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_alpha);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_alpha);
  const GLboolean blend_enabled = glIsEnabled(GL_BLEND);
  const GLboolean depth_enabled = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean cull_enabled = glIsEnabled(GL_CULL_FACE);
  const GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, screen_width, screen_height);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glUseProgram(cursor_program);
  glUniform2f(cursor_u_screen, (GLfloat)screen_width,
              (GLfloat)screen_height);
  // The previous procedural cursor occupied about 40x44 pixels including its
  // outline at 720p. Preserve that footprint while placing the tap hotspot
  // 42% into the texture instead of at its upper-left tip.
  const GLfloat cursor_height = 44.0f * ((GLfloat)screen_height / 720.0f);
  const GLfloat cursor_width = cursor_height * (96.0f / 105.0f);
  glUniform2f(cursor_u_origin, x - cursor_width * 0.42f,
              y - cursor_height * 0.42f);
  glUniform2f(cursor_u_size, cursor_width, cursor_height);
  glUniform1i(cursor_u_texture, 0);
  glBindTexture(GL_TEXTURE_2D, cursor_texture);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                        cursor_quad);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                        cursor_quad + 2);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  glViewport(old_viewport[0], old_viewport[1], old_viewport[2],
             old_viewport[3]);
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_framebuffer);
  glBindTexture(GL_TEXTURE_2D, (GLuint)old_texture);
  glActiveTexture((GLenum)old_active_texture);
  restore_cursor_attrib(0, &attributes[0]);
  restore_cursor_attrib(1, &attributes[1]);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)old_buffer);
  glUseProgram((GLuint)old_program);
  glBlendFuncSeparate((GLenum)blend_src_rgb, (GLenum)blend_dst_rgb,
                      (GLenum)blend_src_alpha, (GLenum)blend_dst_alpha);
  if (!blend_enabled)
    glDisable(GL_BLEND);
  else
    glEnable(GL_BLEND);
  if (depth_enabled)
    glEnable(GL_DEPTH_TEST);
  if (cull_enabled)
    glEnable(GL_CULL_FACE);
  if (scissor_enabled)
    glEnable(GL_SCISSOR_TEST);
}
