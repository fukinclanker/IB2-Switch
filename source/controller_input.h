#ifndef INFINITY_BLADE_NX_CONTROLLER_INPUT_H
#define INFINITY_BLADE_NX_CONTROLLER_INPUT_H

#include <stdint.h>

typedef struct {
  int action;
  int64_t event_time_ns;
  float x;
  float y;
} ControllerTouchEvent;

// Poll the controller and emit at most one Android motion event. allow_touch is
// false while a real touchscreen finger owns pointer id 0.
int controller_input_poll(ControllerTouchEvent *event, int allow_touch);
int controller_input_touch_active(void);

// Draw the optional right-stick cursor over the completed game frame.
void controller_input_draw_cursor(void);

#endif
