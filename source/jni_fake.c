#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jni_fake.h"
#include "music_player.h"
#include "util.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t JniWord;

enum {
  TAG_OBJECT = 0x4f424a31,
  TAG_STRING = 0x53545231,
  TAG_ID = 0x4d494431,
  TAG_ARRAY = 0x41525231,
};

typedef struct {
  uint32_t tag;
  char label[96];
} FakeObject;

typedef struct {
  uint32_t tag;
  char *text;
} FakeString;

typedef struct {
  uint32_t tag;
  int length;
  int element_size;
  void *data;
} FakeArray;

typedef struct {
  uint32_t tag;
  char class_name[96];
  char name[96];
  char signature[96];
} FakeID;

#define MAX_IDS 128
static FakeID ids[MAX_IDS];
static int id_count;

void *jni_make_object(const char *label) {
  FakeObject *object = calloc(1, sizeof(*object));
  if (!object)
    return NULL;
  object->tag = TAG_OBJECT;
  strlcpy(object->label, label ? label : "object", sizeof(object->label));
  return object;
}

void *jni_make_string(const char *text) {
  FakeString *string = calloc(1, sizeof(*string));
  if (!string)
    return NULL;
  string->tag = TAG_STRING;
  string->text = strdup(text ? text : "");
  return string;
}

static const char *object_label(void *object) {
  FakeObject *value = object;
  return value && value->tag == TAG_OBJECT ? value->label : "object";
}

static const char *string_text(void *object) {
  FakeString *value = object;
  return value && value->tag == TAG_STRING ? value->text : "";
}

static FakeID *method_id(void *class_object, const char *name,
                         const char *signature) {
  const char *class_name = object_label(class_object);
  for (int index = 0; index < id_count; index++)
    if (!strcmp(ids[index].class_name, class_name) &&
        !strcmp(ids[index].name, name) &&
        !strcmp(ids[index].signature, signature))
      return &ids[index];
  if (id_count >= MAX_IDS)
    return &ids[MAX_IDS - 1];
  FakeID *id = &ids[id_count++];
  id->tag = TAG_ID;
  strlcpy(id->class_name, class_name, sizeof(id->class_name));
  strlcpy(id->name, name ? name : "", sizeof(id->name));
  strlcpy(id->signature, signature ? signature : "", sizeof(id->signature));
  debugPrintf("JNI: method %s.%s %s\n", id->class_name, id->name,
              id->signature);
  return id;
}

static void dispatch_void(FakeID *id, va_list arguments) {
  if (!id)
    return;
  if (!strcmp(id->name, "showToast")) {
    debugPrintf("JNI toast: %s\n", string_text(va_arg(arguments, void *)));
    return;
  }
  if (!strcmp(id->name, "setSongVolume")) {
    float volume = (float)va_arg(arguments, double);
    music_player_set_volume(volume);
    return;
  }
  if (!strcmp(id->name, "playSong")) {
    const char *song = string_text(va_arg(arguments, void *));
    debugPrintf("JNI: playSong(%s) -> %d\n", song, music_player_play(song));
    return;
  }
  if (!strcmp(id->name, "stopSong")) {
    music_player_stop();
    return;
  }
  if (!strcmp(id->name, "exportSave") || !strcmp(id->name, "importSave")) {
    debugPrintf("JNI: %s requested; saves are exposed under SaveData/\n",
                id->name);
    return;
  }
  if (!strcmp(id->name, "finish") || !strcmp(id->name, "finishAffinity")) {
    debugPrintf("JNI: activity finish requested\n");
    return;
  }
  debugPrintf("JNI: ignored void call %s.%s %s\n", id->class_name, id->name,
              id->signature);
}

static float dispatch_float(FakeID *id) {
  if (id && !strcmp(id->name, "getDisplayXDpi"))
    return 237.0f;
  if (id && !strcmp(id->name, "getDisplayRefreshRate"))
    return 60.0f;
  debugPrintf("JNI: default float for %s.%s -> 0\n",
              id ? id->class_name : "?", id ? id->name : "?");
  return 0.0f;
}

static JniWord get_version(void *environment) {
  (void)environment;
  return JNI_VERSION_1_6;
}

static void *find_class(void *environment, const char *name) {
  (void)environment;
  return jni_make_object(name);
}

static void *get_object_class(void *environment, void *object) {
  (void)environment;
  return jni_make_object(object_label(object));
}

static void *get_method_id(void *environment, void *class_object,
                           const char *name, const char *signature) {
  (void)environment;
  return method_id(class_object, name, signature);
}

static void call_void_method(void *environment, void *object, FakeID *id, ...) {
  (void)environment;
  (void)object;
  va_list arguments;
  va_start(arguments, id);
  dispatch_void(id, arguments);
  va_end(arguments);
}

static void call_void_method_v(void *environment, void *object, FakeID *id,
                               va_list arguments) {
  (void)environment;
  (void)object;
  dispatch_void(id, arguments);
}

static float call_float_method(void *environment, void *object, FakeID *id,
                               ...) {
  (void)environment;
  (void)object;
  return dispatch_float(id);
}

static float call_float_method_v(void *environment, void *object, FakeID *id,
                                 va_list arguments) {
  (void)environment;
  (void)object;
  (void)arguments;
  return dispatch_float(id);
}

static JniWord call_word_method(void *environment, void *object, FakeID *id,
                                ...) {
  (void)environment;
  (void)object;
  debugPrintf("JNI: default scalar for %s.%s -> 0\n",
              id ? id->class_name : "?", id ? id->name : "?");
  return 0;
}

static JniWord call_word_method_v(void *environment, void *object, FakeID *id,
                                  va_list arguments) {
  (void)arguments;
  return call_word_method(environment, object, id);
}

static void *call_object_method(void *environment, void *object, FakeID *id,
                                ...) {
  (void)environment;
  (void)object;
  debugPrintf("JNI: default object for %s.%s\n", id ? id->class_name : "?",
              id ? id->name : "?");
  return NULL;
}

static void *call_object_method_v(void *environment, void *object, FakeID *id,
                                  va_list arguments) {
  (void)arguments;
  return call_object_method(environment, object, id);
}

static void *new_object(void *environment, void *class_object, FakeID *id, ...) {
  (void)environment;
  (void)id;
  return jni_make_object(object_label(class_object));
}

static void *new_object_v(void *environment, void *class_object, FakeID *id,
                          va_list arguments) {
  (void)arguments;
  return new_object(environment, class_object, id);
}

static void *new_string_utf(void *environment, const char *text) {
  (void)environment;
  return jni_make_string(text);
}

static const char *get_string_utf_chars(void *environment, void *string,
                                        uint8_t *copy) {
  (void)environment;
  if (copy && (uintptr_t)copy > 0x1000)
    *copy = 0;
  return string_text(string);
}

static void release_string_utf_chars(void *environment, void *string,
                                     const char *characters) {
  (void)environment;
  (void)string;
  (void)characters;
}

static JniWord get_string_utf_length(void *environment, void *string) {
  (void)environment;
  return strlen(string_text(string));
}

static void *new_ref(void *environment, void *object) {
  (void)environment;
  return object;
}

static void delete_ref(void *environment, void *object) {
  (void)environment;
  (void)object;
}

static JniWord return_zero(void) { return 0; }
static void *return_null(void) { return NULL; }

static void *new_array(int length, int element_size) {
  FakeArray *array = calloc(1, sizeof(*array));
  if (!array)
    return NULL;
  array->tag = TAG_ARRAY;
  array->length = length > 0 ? length : 0;
  array->element_size = element_size;
  array->data = calloc(array->length ? (size_t)array->length : 1,
                       (size_t)element_size);
  return array;
}

static void *new_byte_array(void *environment, int length) {
  (void)environment;
  return new_array(length, 1);
}

static void *new_int_array(void *environment, int length) {
  (void)environment;
  return new_array(length, 4);
}

static JniWord get_array_length(void *environment, FakeArray *array) {
  (void)environment;
  return array && array->tag == TAG_ARRAY ? (JniWord)array->length : 0;
}

static void *get_array_elements(void *environment, FakeArray *array,
                                uint8_t *copy) {
  (void)environment;
  if (copy && (uintptr_t)copy > 0x1000)
    *copy = 0;
  return array && array->tag == TAG_ARRAY ? array->data : NULL;
}

static void release_array_elements(void *environment, void *array,
                                   void *elements, int mode) {
  (void)environment;
  (void)array;
  (void)elements;
  (void)mode;
}

static JniWord register_natives(void *environment, void *class_object,
                                void *methods, int count) {
  (void)environment;
  (void)class_object;
  (void)methods;
  debugPrintf("JNI: RegisterNatives(%d) ignored\n", count);
  return JNI_OK;
}

static JniWord get_java_vm(void *environment, void **vm) {
  (void)environment;
  if (vm)
    *vm = fake_vm;
  return JNI_OK;
}

static void *environment_table[233];
static void **environment_table_pointer = environment_table;
void *fake_env = &environment_table_pointer;

static JniWord vm_destroy(void *vm) {
  (void)vm;
  return JNI_OK;
}

static JniWord vm_attach(void *vm, void **environment, void *arguments) {
  (void)vm;
  (void)arguments;
  if (environment)
    *environment = fake_env;
  return JNI_OK;
}

static JniWord vm_detach(void *vm) {
  (void)vm;
  return JNI_OK;
}

static JniWord vm_get_environment(void *vm, void **environment, int version) {
  (void)vm;
  (void)version;
  if (environment)
    *environment = fake_env;
  return JNI_OK;
}

static void *vm_table[8];
static void **vm_table_pointer = vm_table;
void *fake_vm = &vm_table_pointer;

void jni_init(void) {
  for (int index = 0; index < 233; index++)
    environment_table[index] = (void *)return_zero;
  environment_table[4] = (void *)get_version;
  environment_table[6] = (void *)find_class;
  environment_table[15] = (void *)return_null;
  environment_table[16] = (void *)return_zero;
  environment_table[17] = (void *)return_zero;
  environment_table[21] = (void *)new_ref;
  environment_table[22] = (void *)delete_ref;
  environment_table[23] = (void *)delete_ref;
  environment_table[25] = (void *)new_ref;
  environment_table[28] = (void *)new_object;
  environment_table[29] = (void *)new_object_v;
  environment_table[31] = (void *)get_object_class;
  environment_table[33] = (void *)get_method_id;
  environment_table[34] = (void *)call_object_method;
  environment_table[35] = (void *)call_object_method_v;
  environment_table[37] = (void *)call_word_method;
  environment_table[38] = (void *)call_word_method_v;
  environment_table[49] = (void *)call_word_method;
  environment_table[50] = (void *)call_word_method_v;
  environment_table[52] = (void *)call_word_method;
  environment_table[53] = (void *)call_word_method_v;
  environment_table[55] = (void *)call_float_method;
  environment_table[56] = (void *)call_float_method_v;
  environment_table[61] = (void *)call_void_method;
  environment_table[62] = (void *)call_void_method_v;
  environment_table[94] = (void *)get_method_id;
  environment_table[113] = (void *)get_method_id;
  environment_table[114] = (void *)call_object_method;
  environment_table[115] = (void *)call_object_method_v;
  environment_table[117] = (void *)call_word_method;
  environment_table[118] = (void *)call_word_method_v;
  environment_table[129] = (void *)call_word_method;
  environment_table[130] = (void *)call_word_method_v;
  environment_table[135] = (void *)call_float_method;
  environment_table[136] = (void *)call_float_method_v;
  environment_table[141] = (void *)call_void_method;
  environment_table[142] = (void *)call_void_method_v;
  environment_table[144] = (void *)get_method_id;
  environment_table[167] = (void *)new_string_utf;
  environment_table[168] = (void *)get_string_utf_length;
  environment_table[169] = (void *)get_string_utf_chars;
  environment_table[170] = (void *)release_string_utf_chars;
  environment_table[171] = (void *)get_array_length;
  environment_table[176] = (void *)new_byte_array;
  environment_table[179] = (void *)new_int_array;
  for (int index = 183; index <= 190; index++)
    environment_table[index] = (void *)get_array_elements;
  for (int index = 191; index <= 198; index++)
    environment_table[index] = (void *)release_array_elements;
  environment_table[215] = (void *)register_natives;
  environment_table[219] = (void *)get_java_vm;
  environment_table[228] = (void *)return_zero;

  vm_table[3] = (void *)vm_destroy;
  vm_table[4] = (void *)vm_attach;
  vm_table[5] = (void *)vm_detach;
  vm_table[6] = (void *)vm_get_environment;
  vm_table[7] = (void *)vm_attach;
  debugPrintf("JNI: Infinity Blade III environment initialized env=%p vm=%p\n",
              fake_env, fake_vm);
}
