#ifndef ANDROID_NATIVE_WINDOW_H
#define ANDROID_NATIVE_WINDOW_H
typedef struct ANativeWindow ANativeWindow;
int ANativeWindow_getWidth(ANativeWindow *window);
int ANativeWindow_getHeight(ANativeWindow *window);
void ANativeWindow_acquire(ANativeWindow *window);
void ANativeWindow_release(ANativeWindow *window);
#endif
