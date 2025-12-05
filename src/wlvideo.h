/*
 * wlvideo.h — Zero-copy video wallpaper for Wayland
 *
 * Architecture:
 *   - VA-API decode (Intel/AMD/NVIDIA via nvidia-vaapi-driver)
 *   - DMA-BUF export for zero-copy on Intel/AMD
 *   - Software fallback when DMA-BUF import fails
 *   - Fixed memory: preallocated ring buffer, no per-frame malloc
 */

#ifndef WLVIDEO_H
#define WLVIDEO_H

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wayland-client.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

/* Ring buffer slots for software decode. Two slots = double buffering. */
#define SW_RING_SIZE 2

/* EGL image cache size. VA-API typically uses 4-8 surfaces. */
#define EGL_CACHE_SIZE 8

typedef enum {
    GPU_VENDOR_UNKNOWN,
    GPU_VENDOR_INTEL,
    GPU_VENDOR_AMD,
    GPU_VENDOR_NVIDIA,
} GpuVendor;

typedef enum {
    CS_BT601,
    CS_BT709,
    CS_BT2020,
} ColorSpace;

typedef enum {
    CR_LIMITED,
    CR_FULL,
} ColorRange;

typedef enum {
    SCALE_FIT,
    SCALE_FILL,
    SCALE_STRETCH,
} ScaleMode;

typedef struct {
    int fd[4];
    uint32_t offset[4];
    uint32_t stride[4];
    uint32_t fourcc;
    uint64_t modifier[4];
    int width, height;
    int num_planes;
} DmaBuf;

typedef struct {
    enum { FRAME_HW, FRAME_SW } type;

    double pts;
    int width, height;
    ColorSpace colorspace;
    ColorRange color_range;

    struct {
        uintptr_t surface_id;
        uint64_t generation;
        DmaBuf dmabuf;
    } hw;

    struct {
        int ring_slot;
        int pixel_format;
        bool available;
    } sw;
} Frame;

typedef struct {
    uint8_t *data;
    size_t slot_size;
    int width, height;
    int y_stride;
    int uv_stride;
} SoftwareRing;

typedef struct {
    uintptr_t surface_id;
    EGLImage image;
    uint64_t last_use;
} EglCacheEntry;

/*
 * Output state machine:
 *
 *   OUT_UNCONFIGURED ──[first configure]──► OUT_READY
 *         ▲                                    │
 *         │                          [request_frame]
 *         │                                    ▼
 *   [create_surface]               OUT_WAITING_CALLBACK
 *         │                                    │
 *         │                           [frame_done]
 *         │                                    │
 *   OUT_PENDING_RECREATE ◄────────────────────┘
 *         ▲                                    │
 *         │                          [layer_closed]
 *         │                                    ▼
 *   [cleanup done]◄───────────── OUT_PENDING_DESTROY
 *
 *   OUT_DEFUNCT: Output permanently failed, will not be recreated
 */
typedef enum {
    OUT_UNCONFIGURED,        /* Initial state, waiting for first configure */
    OUT_READY,               /* Configured and ready to render */
    OUT_WAITING_CALLBACK,    /* Waiting for frame callback */
    OUT_PENDING_DESTROY,     /* layer_closed received, waiting for cleanup */
    OUT_PENDING_RECREATE,    /* Cleanup done, waiting to recreate */
    OUT_DEFUNCT,             /* Permanently failed, skip this output */
} OutputState;

typedef struct Output {
    struct wl_list link;

    struct wl_output *wl_output;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_callback *frame_callback;
    uint32_t wl_name;

    char name[64];
    int width, height;
    int scale;

    struct wl_egl_window *egl_window;
    EGLSurface egl_surface;

    OutputState state;
    uint64_t frames_rendered;

    /* Track configured dimensions to detect actual changes */
    int configured_width, configured_height;

    /* Recreation backoff state */
    double last_recreation_attempt;
    int recreation_failures;
} Output;

typedef struct Decoder Decoder;
typedef struct Renderer Renderer;

typedef struct {
    const char *video_path;
    const char *output_name;
    const char *gpu_device;
    ScaleMode scale_mode;
    bool loop;
    bool hw_accel;
    bool verbose;
} Config;

typedef struct App {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    struct wl_list outputs;

    Decoder *decoder;
    Renderer *renderer;
    SoftwareRing sw_ring;

    Config config;

    bool renderer_needs_reset;

    bool running;
    bool clock_started;
    double start_time;
    double frame_duration;
    uint64_t frame_counter;

    bool render_path_determined;
    bool use_dmabuf_path;

    /* Compositor restart recovery state */
    double last_output_ready_time;    /* When we last had a ready output */
    int no_output_iterations;         /* Consecutive iterations with no ready outputs */
} App;

extern App *g_app;

/* ================================
 * Section: Logging
 * ================================
 * All log macros include monotonic timestamp for debugging timing issues.
 * Format: [LEVEL T+seconds.milliseconds] message
 */

static inline double log_timestamp(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Start time for relative timestamps (set in main) */
extern double g_log_start_time;

#define LOG_ERROR(fmt, ...) \
    fprintf(stderr, "\033[31m[ERROR T+%.3f]\033[0m " fmt "\n", \
            log_timestamp() - g_log_start_time, ##__VA_ARGS__)

#define LOG_WARN(fmt, ...) \
    fprintf(stderr, "\033[33m[WARN  T+%.3f]\033[0m " fmt "\n", \
            log_timestamp() - g_log_start_time, ##__VA_ARGS__)

#define LOG_INFO(fmt, ...) \
    do { \
        if (g_app && g_app->config.verbose) \
            fprintf(stderr, "\033[32m[INFO  T+%.3f]\033[0m " fmt "\n", \
                    log_timestamp() - g_log_start_time, ##__VA_ARGS__); \
    } while(0)

#define LOG_DEBUG(fmt, ...) \
    do { \
        if (g_app && g_app->config.verbose) \
            fprintf(stderr, "\033[34m[DEBUG T+%.3f]\033[0m " fmt "\n", \
                    log_timestamp() - g_log_start_time, ##__VA_ARGS__); \
    } while(0)

/* Utilities */
const char *fourcc_to_str(uint32_t fourcc);
const char *output_state_name(OutputState state);

/* Decoder */
int decoder_init(Decoder **dec, const char *path, bool hw_accel, const char *gpu);
void decoder_destroy(Decoder *dec);
bool decoder_get_frame(Decoder *dec, Frame *frame, SoftwareRing *ring, bool need_sw);
int decoder_seek_start(Decoder *dec);
void decoder_get_info(Decoder *dec, int *w, int *h, double *fps, bool *hw);
void decoder_close_dmabuf(DmaBuf *dmabuf);
GpuVendor decoder_get_gpu_vendor(Decoder *dec);
bool decoder_dmabuf_export_supported(Decoder *dec);
void decoder_set_dmabuf_export_result(Decoder *dec, bool works);
void decoder_increment_generation(Decoder *dec);

/* Renderer */
int renderer_init(Renderer **r, struct wl_display *display);
void renderer_destroy(Renderer *r);
int renderer_create_output(Renderer *r, Output *out);
void renderer_destroy_output(Renderer *r, Output *out);
bool renderer_draw(Renderer *r, Output *out, Frame *frame, SoftwareRing *ring, ScaleMode scale, bool try_dmabuf);
void renderer_clear_cache(Renderer *r);
void renderer_reset_dmabuf_state(Renderer *r);
void renderer_reset_texture_state(Renderer *r);
GpuVendor renderer_get_gpu_vendor(Renderer *r);
const char *renderer_get_gl_renderer(Renderer *r);
void renderer_log_stats(Renderer *r);

/* Wayland */
int wayland_init(App *app);
void wayland_destroy(App *app);
int wayland_create_surface(Output *out, App *app);
void wayland_destroy_surface(Output *out);
void wayland_request_frame(Output *out);

/* Ring buffer */
int sw_ring_init(SoftwareRing *ring, int width, int height);
void sw_ring_destroy(SoftwareRing *ring);
uint8_t *sw_ring_get_y(SoftwareRing *ring, int slot);
uint8_t *sw_ring_get_uv(SoftwareRing *ring, int slot);

#endif
