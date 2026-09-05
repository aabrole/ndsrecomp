#if defined(__ANDROID__)
#include "android_second_screen.h"

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace {
std::mutex g_mtx;
ANativeWindow* g_win = nullptr;
int g_geom_w = 0;
int g_geom_h = 0;

// DS bottom-screen touch state forwarded from the second display's SurfaceView.
std::atomic<int> g_touch_x{0};
std::atomic<int> g_touch_y{0};
std::atomic<bool> g_touch_down{false};

// Bottom-screen presentation: false = letterboxed true 4:3 (default),
// true = stretched to fill the panel.
std::atomic<bool> g_stretch{false};
// Content placement within the letterbox canvas, for touch mapping.
std::atomic<int> g_canvas_w{256}, g_canvas_h{192};
std::atomic<int> g_off_x{0}, g_off_y{0};
}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_thor_mph_MyGame_nativeSetSecondScreenStretch(JNIEnv*, jclass,
                                                      jboolean stretch) {
    g_stretch.store(stretch != JNI_FALSE, std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_com_thor_mph_MyGame_nativeSetSecondSurface(JNIEnv* env, jclass,
                                                jobject surface) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_win) {
        ANativeWindow_release(g_win);
        g_win = nullptr;
        g_geom_w = g_geom_h = 0;
    }
    if (surface) {
        g_win = ANativeWindow_fromSurface(env, surface);
        __android_log_write(ANDROID_LOG_INFO, "ThorMPHrun",
                            g_win ? "[second-screen] surface attached"
                                  : "[second-screen] fromSurface failed");
    } else {
        __android_log_write(ANDROID_LOG_INFO, "ThorMPHrun",
                            "[second-screen] surface detached");
    }
}

// Normalized touch (nx,ny in [0,1] across the SurfaceView) -> DS 256x192.
extern "C" JNIEXPORT void JNICALL
Java_com_thor_mph_MyGame_nativeSecondScreenTouch(JNIEnv*, jclass, jfloat nx,
                                                 jfloat ny, jboolean down) {
    // Map through the letterbox: the view spans the canvas; the DS content
    // occupies [off, off+content) inside it.
    const float cw = static_cast<float>(g_canvas_w.load(std::memory_order_relaxed));
    const float ch = static_cast<float>(g_canvas_h.load(std::memory_order_relaxed));
    const float ox = static_cast<float>(g_off_x.load(std::memory_order_relaxed));
    const float oy = static_cast<float>(g_off_y.load(std::memory_order_relaxed));
    if (cw > 0.0f && ch > 0.0f) {
        nx = (nx * cw - ox) / 256.0f;
        ny = (ny * ch - oy) / 192.0f;
    }
    int x = static_cast<int>(nx * 255.0f + 0.5f);
    int y = static_cast<int>(ny * 191.0f + 0.5f);
    if (x < 0) x = 0; else if (x > 255) x = 255;
    if (y < 0) y = 0; else if (y > 191) y = 191;
    g_touch_x.store(x, std::memory_order_relaxed);
    g_touch_y.store(y, std::memory_order_relaxed);
    g_touch_down.store(down != JNI_FALSE, std::memory_order_relaxed);
}

bool android_second_screen_active() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_win != nullptr;
}

bool android_second_screen_touch(int* x, int* y, bool* down) {
    *x = g_touch_x.load(std::memory_order_relaxed);
    *y = g_touch_y.load(std::memory_order_relaxed);
    *down = g_touch_down.load(std::memory_order_relaxed);
    return true;
}

void android_second_screen_present(const uint32_t* pixels, int width,
                                   int height) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_win || !pixels || width <= 0 || height <= 0) return;
    // Canvas: same as the content when stretching; otherwise sized to the
    // panel's aspect so Android's scaler preserves true 4:3 with bars.
    int canvas_w = width, canvas_h = height, off_x = 0, off_y = 0;
    if (!g_stretch.load(std::memory_order_relaxed)) {
        const int win_w = ANativeWindow_getWidth(g_win);
        const int win_h = ANativeWindow_getHeight(g_win);
        if (win_w > 0 && win_h > 0) {
            const float panel = static_cast<float>(win_w) / win_h;
            const float content = static_cast<float>(width) / height;
            if (content > panel) {
                canvas_h = static_cast<int>(width / panel + 0.5f);
                off_y = (canvas_h - height) / 2;
            } else {
                canvas_w = static_cast<int>(height * panel + 0.5f);
                off_x = (canvas_w - width) / 2;
            }
        }
    }
    g_canvas_w.store(canvas_w, std::memory_order_relaxed);
    g_canvas_h.store(canvas_h, std::memory_order_relaxed);
    g_off_x.store(off_x, std::memory_order_relaxed);
    g_off_y.store(off_y, std::memory_order_relaxed);
    if (canvas_w != g_geom_w || canvas_h != g_geom_h) {
        ANativeWindow_setBuffersGeometry(g_win, canvas_w, canvas_h,
                                         WINDOW_FORMAT_RGBA_8888);
        g_geom_w = canvas_w;
        g_geom_h = canvas_h;
    }
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(g_win, &buf, nullptr) != 0) return;
    uint32_t* dst = static_cast<uint32_t*>(buf.bits);
    // Clear the whole canvas (cheap at DS scale) so letterbox bars stay black,
    // then place the content at its offset. Framebuffer is ARGB8888
    // (0xAARRGGBB); Android RGBA_8888 wants [R,G,B,A] = 0xAABBGGRR: swap R/B.
    for (int y = 0; y < buf.height; ++y)
        std::memset(dst + static_cast<size_t>(y) * buf.stride, 0,
                    static_cast<size_t>(buf.width) * 4);
    const int max_h = buf.height - off_y;
    const int max_w = buf.width - off_x;
    const int copy_h = height < max_h ? height : max_h;
    const int copy_w = width < max_w ? width : max_w;
    for (int y = 0; y < copy_h; ++y) {
        const uint32_t* src_row = pixels + static_cast<size_t>(y) * width;
        uint32_t* dst_row = dst +
            static_cast<size_t>(y + off_y) * buf.stride + off_x;
        for (int x = 0; x < copy_w; ++x) {
            const uint32_t p = src_row[x];
            dst_row[x] = (p & 0xFF00FF00u) |
                         ((p & 0x00FF0000u) >> 16) |
                         ((p & 0x000000FFu) << 16);
        }
    }
    ANativeWindow_unlockAndPost(g_win);
}
#endif  // __ANDROID__
