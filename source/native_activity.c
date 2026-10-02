#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "config.h"
#include "jni_fake.h"
#include "native_activity.h"
#include "so_util.h"
#include "util.h"

typedef struct ANativeActivity ANativeActivity;

typedef struct {
  void (*onStart)(ANativeActivity *activity);
  void (*onResume)(ANativeActivity *activity);
  void *(*onSaveInstanceState)(ANativeActivity *activity, size_t *out_size);
  void (*onPause)(ANativeActivity *activity);
  void (*onStop)(ANativeActivity *activity);
  void (*onDestroy)(ANativeActivity *activity);
  void (*onWindowFocusChanged)(ANativeActivity *activity, int focused);
  void (*onNativeWindowCreated)(ANativeActivity *activity, void *window);
  void (*onNativeWindowResized)(ANativeActivity *activity, void *window);
  void (*onNativeWindowRedrawNeeded)(ANativeActivity *activity, void *window);
  void (*onNativeWindowDestroyed)(ANativeActivity *activity, void *window);
  void (*onInputQueueCreated)(ANativeActivity *activity, void *queue);
  void (*onInputQueueDestroyed)(ANativeActivity *activity, void *queue);
  void (*onContentRectChanged)(ANativeActivity *activity, const void *rect);
  void (*onConfigurationChanged)(ANativeActivity *activity);
  void (*onLowMemory)(ANativeActivity *activity);
} ANativeActivityCallbacks;

struct ANativeActivity {
  ANativeActivityCallbacks *callbacks;
  void *vm;
  void *env;
  void *clazz;
  const char *internalDataPath;
  const char *externalDataPath;
  int32_t sdkVersion;
  void *instance;
  void *assetManager;
  const char *obbPath;
};

static ANativeActivity activity;
static ANativeActivityCallbacks callbacks;
static uint64_t input_queue_token;
static NWindow *native_window;
static int activity_started;

int native_activity_bootstrap(so_module *game_module) {
  typedef void (*OnCreate)(ANativeActivity *, void *, size_t);
  OnCreate on_create =
      (OnCreate)so_try_find_addr_rx(game_module, "ANativeActivity_onCreate");
  if (!on_create) {
    debugPrintf("NativeActivity: ANativeActivity_onCreate missing\n");
    return -1;
  }

  jni_init();
  debugPrintf("NativeActivity: clearing activity state\n");
  memset(&activity, 0, sizeof(activity));
  memset(&callbacks, 0, sizeof(callbacks));
  activity.callbacks = &callbacks;
  activity.vm = fake_vm;
  activity.env = fake_env;
  debugPrintf("NativeActivity: creating activity object\n");
  activity.clazz = jni_make_object("com/ib3port/game/GameActivity");
  /* libib3.so looks for its extracted IPA at "<data path>/game/Payload/
   * SwordGame.app" (the Android launcher installs it into getFilesDir()/game),
   * so both data paths point at the runtime directory that contains game/.
   * Unverified on hardware: if the log shows the runtime searching elsewhere,
   * this is the value to change. */
  activity.internalDataPath = ".";
  activity.externalDataPath = ".";
  activity.sdkVersion = 30;
  activity.assetManager = (void *)1;
  activity.obbPath = ".";

  debugPrintf("NativeActivity: requesting default window\n");
  native_window = nwindowGetDefault();
  debugPrintf("NativeActivity: default window=%p\n", native_window);
  if (!native_window) {
    debugPrintf("NativeActivity: no default window\n");
    return -1;
  }
  /* Match docked vs handheld before the game queries surface size. */
  {
    const AppletOperationMode mode = appletGetOperationMode();
    if (mode == AppletOperationMode_Console) {
      screen_width = 1920;
      screen_height = 1080;
    } else {
      screen_width = 1280;
      screen_height = 720;
    }
    debugPrintf("NativeActivity: operation mode %s -> %dx%d\n",
                mode == AppletOperationMode_Console ? "docked" : "handheld",
                screen_width, screen_height);
  }
  const Result window_result =
      nwindowSetDimensions(native_window, screen_width, screen_height);
  const Result crop_result =
      nwindowSetCrop(native_window, 0, 0, screen_width, screen_height);
  const Result transform_result = nwindowSetTransform(native_window, 0);
  u32 actual_width = 0;
  u32 actual_height = 0;
  const Result dimensions_result =
      nwindowGetDimensions(native_window, &actual_width, &actual_height);
  debugPrintf("NativeActivity: window requested=%dx%d actual=%ux%u set=%08x get=%08x crop=%08x transform=%08x\n",
              screen_width, screen_height, actual_width, actual_height,
              window_result, dimensions_result, crop_result, transform_result);

  debugPrintf("NativeActivity: ANativeActivity_onCreate begin\n");
  on_create(&activity, NULL, 0);
  debugPrintf("NativeActivity: ANativeActivity_onCreate returned instance=%p\n",
              activity.instance);

  if (!callbacks.onStart || !callbacks.onResume ||
      !callbacks.onNativeWindowCreated) {
    debugPrintf("NativeActivity: required lifecycle callbacks were not installed\n");
    return -1;
  }

  callbacks.onStart(&activity);
  callbacks.onResume(&activity);
  callbacks.onNativeWindowCreated(&activity, native_window);
  if (callbacks.onInputQueueCreated)
    callbacks.onInputQueueCreated(&activity, &input_queue_token);
  if (callbacks.onWindowFocusChanged)
    callbacks.onWindowFocusChanged(&activity, 1);
  activity_started = 1;
  debugPrintf("NativeActivity: lifecycle handoff complete window=%p queue=%p\n",
              native_window, &input_queue_token);
  return 0;
}

void native_activity_shutdown(void) {
  if (!activity_started)
    return;
  if (callbacks.onWindowFocusChanged)
    callbacks.onWindowFocusChanged(&activity, 0);
  if (callbacks.onInputQueueDestroyed)
    callbacks.onInputQueueDestroyed(&activity, &input_queue_token);
  if (callbacks.onNativeWindowDestroyed)
    callbacks.onNativeWindowDestroyed(&activity, native_window);
  if (callbacks.onPause)
    callbacks.onPause(&activity);
  if (callbacks.onStop)
    callbacks.onStop(&activity);
  if (callbacks.onDestroy)
    callbacks.onDestroy(&activity);
  activity_started = 0;
}
