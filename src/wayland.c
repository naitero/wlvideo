/*
 * wayland.c — Layer shell surface management
 *
 * Creates a background layer surface that covers the entire output.
 * Frame callbacks throttle rendering to display refresh rate.
 *
 * Lifecycle: When the compositor sends layer_surface::closed (e.g., during
 * compositor restart), we must destroy resources and can recreate them
 * when the output becomes available again.
 *
 * State machine ensures clean transitions and prevents duplicate operations.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "wlvideo.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

/* Human-readable state names for logging */
const char *output_state_name(OutputState state) {
    switch (state) {
    case OUT_UNCONFIGURED:     return "UNCONFIGURED";
    case OUT_READY:            return "READY";
    case OUT_WAITING_CALLBACK: return "WAITING_CALLBACK";
    case OUT_PENDING_DESTROY:  return "PENDING_DESTROY";
    case OUT_PENDING_RECREATE: return "PENDING_RECREATE";
    case OUT_DEFUNCT:          return "DEFUNCT";
    default:                   return "UNKNOWN";
    }
}

/* ================================
 * Section: Layer surface events
 * ================================ */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *surf,
                            uint32_t serial, uint32_t w, uint32_t h) {
    Output *out = data;

    /* Always ack configure first, per protocol requirements */
    zwlr_layer_surface_v1_ack_configure(surf, serial);

    /* Ignore configures if we're in a destruction or defunct state */
    if (out->state == OUT_PENDING_DESTROY || out->state == OUT_PENDING_RECREATE ||
        out->state == OUT_DEFUNCT) {
        LOG_DEBUG("Output %s: ignoring configure in state %s",
                  out->name, output_state_name(out->state));
        return;
    }

    /* Check if this is a meaningful change */
    bool size_changed = (out->configured_width != (int)w ||
                         out->configured_height != (int)h);
    bool first_configure = (out->state == OUT_UNCONFIGURED);

    if (!size_changed && !first_configure) {
        /* Duplicate configure with same dimensions - ignore */
        LOG_DEBUG("Output %s: ignoring duplicate configure %dx%d",
                  out->name, w, h);
        return;
    }

    /* Store new dimensions */
    out->configured_width = w;
    out->configured_height = h;
    out->width = w;
    out->height = h;

    /* Resize EGL window if it exists */
    if (out->egl_window) {
        wl_egl_window_resize(out->egl_window, w, h, 0, 0);
        LOG_DEBUG("Output %s: resized EGL window to %dx%d", out->name, w, h);
    }

    /* State transition */
    if (first_configure) {
        out->state = OUT_READY;
        LOG_INFO("Output %s: configured %dx%d", out->name, w, h);
    } else {
        LOG_INFO("Output %s: resized to %dx%d", out->name, w, h);
    }
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *surf) {
    (void)surf;
    Output *out = data;

    LOG_INFO("Output %s: layer surface closed by compositor (was %s)",
             out->name, output_state_name(out->state));

    /* Prevent duplicate handling */
    if (out->state == OUT_PENDING_DESTROY || out->state == OUT_PENDING_RECREATE ||
        out->state == OUT_DEFUNCT) {
        LOG_DEBUG("Output %s: already in destruction/defunct state, ignoring", out->name);
        return;
    }

    /*
     * Per wlr-layer-shell protocol: "The client should destroy the resource
     * after receiving this event, and create a new surface if they so choose."
     *
     * We can't destroy the layer_surface synchronously here (we're in its
     * callback), so we transition to PENDING_DESTROY state for deferred
     * cleanup in the main loop.
     */

    /* Destroy EGL resources immediately - safe since we're not in EGL callback */
    if (g_app && g_app->renderer) {
        renderer_destroy_output(g_app->renderer, out);
    }

    /* Destroy frame callback if pending - set to NULL first to prevent race */
    struct wl_callback *cb = out->frame_callback;
    out->frame_callback = NULL;
    if (cb) {
        wl_callback_destroy(cb);
    }

    /* Transition to pending destroy state */
    out->state = OUT_PENDING_DESTROY;

    /* Signal renderer reset needed due to compositor restart */
    if (g_app) {
        g_app->renderer_needs_reset = true;
    }
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

/* ================================
 * Section: Frame callback
 * ================================ */

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    (void)time;
    Output *out = data;

    /*
     * Check if this callback is still the active one. layer_closed() may have
     * already destroyed and NULLed out->frame_callback, in which case this
     * callback is orphaned and we should just destroy it without side effects.
     */
    if (out->frame_callback == cb) {
        out->frame_callback = NULL;
        wl_callback_destroy(cb);

        /* Only transition if we were actually waiting */
        if (out->state == OUT_WAITING_CALLBACK) {
            out->state = OUT_READY;
        }
    } else {
        /*
         * Orphaned callback - layer_closed already handled cleanup.
         * The callback object was already destroyed in layer_closed,
         * so we must NOT destroy it again here.
         */
        LOG_DEBUG("Output %s: orphaned frame callback (already cleaned up)", out->name);
    }
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

void wayland_request_frame(Output *out) {
    if (!out->surface) return;

    /* Don't request frames in destruction or defunct states */
    if (out->state == OUT_PENDING_DESTROY || out->state == OUT_PENDING_RECREATE ||
        out->state == OUT_DEFUNCT) {
        return;
    }

    /* Clean up any existing callback first */
    if (out->frame_callback) {
        wl_callback_destroy(out->frame_callback);
        out->frame_callback = NULL;
    }

    out->frame_callback = wl_surface_frame(out->surface);
    if (!out->frame_callback) {
        LOG_WARN("Output %s: wl_surface_frame failed", out->name);
        return;
    }
    wl_callback_add_listener(out->frame_callback, &frame_listener, out);
    out->state = OUT_WAITING_CALLBACK;
}

/* ================================
 * Section: Output events
 * ================================ */

static void output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
                            int32_t pw, int32_t ph, int32_t subpx, const char *make,
                            const char *model, int32_t transform) {
    (void)data; (void)o; (void)x; (void)y; (void)pw; (void)ph;
    (void)subpx; (void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w,
                        int32_t h, int32_t refresh) {
    (void)o; (void)refresh;
    Output *out = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        out->width = w;
        out->height = h;
    }
}

static void output_done(void *data, struct wl_output *o) {
    (void)o;
    Output *out = data;

    LOG_DEBUG("Output %s: wl_output.done (state=%s, %dx%d)",
              out->name, output_state_name(out->state), out->width, out->height);

    /*
     * output::done indicates the output info is complete.
     * If this output needs a surface and is in recreate state, it's ready.
     */
    if (out->state == OUT_PENDING_RECREATE &&
        out->width > 0 && out->height > 0 && out->name[0]) {
        LOG_DEBUG("Output %s: ready for surface recreation", out->name);
        /* State will be handled by process_output_lifecycle in main loop */
    }
}

static void output_scale(void *data, struct wl_output *o, int32_t scale) {
    (void)o;
    Output *out = data;
    out->scale = scale;
}

static void output_name(void *data, struct wl_output *o, const char *name) {
    (void)o;
    Output *out = data;
    strncpy(out->name, name, sizeof(out->name) - 1);
    LOG_INFO("Found output: %s", name);
}

static void output_desc(void *data, struct wl_output *o, const char *desc) {
    (void)data; (void)o; (void)desc;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_desc,
};

/* ================================
 * Section: Registry
 * ================================ */

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t ver) {
    App *app = data;

    if (!strcmp(iface, wl_compositor_interface.name)) {
        app->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
        app->layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) {
        app->dmabuf = wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, ver < 3 ? ver : 3);
    } else if (!strcmp(iface, wl_output_interface.name)) {
        Output *out = calloc(1, sizeof(Output));
        if (!out) return;

        out->wl_name = name;
        out->scale = 1;
        out->state = OUT_UNCONFIGURED;
        out->configured_width = 0;
        out->configured_height = 0;
        out->wl_output = wl_registry_bind(reg, name, &wl_output_interface, ver < 4 ? ver : 4);

        wl_output_add_listener(out->wl_output, &output_listener, out);
        wl_list_insert(&app->outputs, &out->link);

        LOG_DEBUG("Registered output wl_name=%u", name);
    }
}

static void registry_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)reg;
    App *app = data;

    Output *out, *tmp;
    wl_list_for_each_safe(out, tmp, &app->outputs, link) {
        if (out->wl_name == name) {
            LOG_INFO("Output removed: %s (wl_name=%u)", out->name, name);
            wl_list_remove(&out->link);

            /* Clean up all resources */
            if (g_app && g_app->renderer) {
                renderer_destroy_output(g_app->renderer, out);
            }
            if (out->frame_callback) wl_callback_destroy(out->frame_callback);
            if (out->layer_surface) zwlr_layer_surface_v1_destroy(out->layer_surface);
            if (out->surface) wl_surface_destroy(out->surface);
            if (out->egl_window) wl_egl_window_destroy(out->egl_window);
            if (out->wl_output) wl_output_destroy(out->wl_output);

            free(out);
            return;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

/* ================================
 * Section: Public API
 * ================================ */

int wayland_init(App *app) {
    app->display = wl_display_connect(NULL);
    if (!app->display) {
        LOG_ERROR("Cannot connect to Wayland");
        return -1;
    }

    app->registry = wl_display_get_registry(app->display);
    wl_registry_add_listener(app->registry, &registry_listener, app);

    wl_display_roundtrip(app->display);

    if (!app->compositor) {
        LOG_ERROR("No wl_compositor");
        return -1;
    }
    if (!app->layer_shell) {
        LOG_ERROR("No wlr-layer-shell (is this a wlroots compositor?)");
        return -1;
    }

    wl_display_roundtrip(app->display);

    if (wl_list_empty(&app->outputs)) {
        LOG_ERROR("No outputs found");
        return -1;
    }

    LOG_INFO("Wayland initialized");
    return 0;
}

void wayland_destroy(App *app) {
    Output *out, *tmp;
    wl_list_for_each_safe(out, tmp, &app->outputs, link) {
        wl_list_remove(&out->link);

        if (out->frame_callback) wl_callback_destroy(out->frame_callback);
        if (out->layer_surface) zwlr_layer_surface_v1_destroy(out->layer_surface);
        if (out->surface) wl_surface_destroy(out->surface);
        if (out->egl_window) wl_egl_window_destroy(out->egl_window);
        if (out->wl_output) wl_output_destroy(out->wl_output);

        free(out);
    }

    if (app->dmabuf) zwp_linux_dmabuf_v1_destroy(app->dmabuf);
    if (app->layer_shell) zwlr_layer_shell_v1_destroy(app->layer_shell);
    if (app->compositor) wl_compositor_destroy(app->compositor);
    if (app->registry) wl_registry_destroy(app->registry);
    if (app->display) wl_display_disconnect(app->display);
}

int wayland_create_surface(Output *out, App *app) {
    /* Validate state - only create from UNCONFIGURED or PENDING_RECREATE */
    if (out->state != OUT_UNCONFIGURED && out->state != OUT_PENDING_RECREATE) {
        LOG_WARN("Output %s: cannot create surface in state %s",
                 out->name, output_state_name(out->state));
        return -1;
    }

    out->surface = wl_compositor_create_surface(app->compositor);
    if (!out->surface) {
        LOG_ERROR("Output %s: failed to create wl_surface", out->name);
        return -1;
    }

    out->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        app->layer_shell, out->surface, out->wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "wlvideo");

    if (!out->layer_surface) {
        wl_surface_destroy(out->surface);
        out->surface = NULL;
        LOG_ERROR("Output %s: failed to create layer surface", out->name);
        return -1;
    }

    /* Fullscreen, behind everything, no input */
    zwlr_layer_surface_v1_set_size(out->layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(out->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(out->layer_surface, -1);

    zwlr_layer_surface_v1_add_listener(out->layer_surface, &layer_listener, out);
    wl_surface_commit(out->surface);

    /* Reset configured dimensions to detect first configure */
    out->configured_width = 0;
    out->configured_height = 0;
    out->state = OUT_UNCONFIGURED;

    LOG_DEBUG("Output %s: Wayland surface created, waiting for configure", out->name);
    return 0;
}

void wayland_destroy_surface(Output *out) {
    LOG_DEBUG("Output %s: destroying Wayland surface (state=%s)",
              out->name, output_state_name(out->state));

    if (out->frame_callback) {
        wl_callback_destroy(out->frame_callback);
        out->frame_callback = NULL;
    }
    if (out->layer_surface) {
        zwlr_layer_surface_v1_destroy(out->layer_surface);
        out->layer_surface = NULL;
    }
    if (out->surface) {
        wl_surface_destroy(out->surface);
        out->surface = NULL;
    }
    if (out->egl_window) {
        wl_egl_window_destroy(out->egl_window);
        out->egl_window = NULL;
    }

    out->egl_surface = EGL_NO_SURFACE;
    out->configured_width = 0;
    out->configured_height = 0;

    /* Transition to PENDING_RECREATE so main loop knows to recreate */
    out->state = OUT_PENDING_RECREATE;
}
