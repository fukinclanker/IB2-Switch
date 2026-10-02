#ifndef INFINITY_BLADE_NX_JNI_FAKE_H
#define INFINITY_BLADE_NX_JNI_FAKE_H

extern void *fake_vm;
extern void *fake_env;

void jni_init(void);
void *jni_make_object(const char *label);
void *jni_make_string(const char *text);

#endif

