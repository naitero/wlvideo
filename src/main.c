/*
 * main.c — Event loop, timing, and render path selection
 *
 * The playback clock maps video time to wall-clock time:
 *   display_time(n) = start_time + n * frame_duration
 *
 * When decode can't keep up, we skip frames to catch up with the clock.
 * If we fall too far behind, we reset the clock instead of skipping forever.
 *
 * Surface lifecycle: When the compositor restarts, layer surfaces may be
 * closed. We handle this by destroying old resources and recreating surfaces
 * when outputs become available again.
 *
 * Key design decisions:
 * - Separate "cache clear" from "DMA-BUF compatibility reset"
 * - Only reset render_path_determined on actual context loss, not surface recreation
 * - Strict state machine for output lifecycle to prevent duplicate operations
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <errno.h>

#include "wlvideo.h"

App *g_app = NULL;
double g_log_start_time = 0;
static volatile sig_atomic_t quit = 0;

static void handle_signal(int sig) {
    (void)sig;
    quit = 1;
}

static const char *vendor_name(GpuVendor v) {
    switch (v) {
    case GPU_VENDOR_INTEL: return "Intel";
    case GPU_VENDOR_AMD: return "AMD";
    case GPU_VENDOR_NVIDIA: return "NVIDIA";
    default: return "Unknown";
    }
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] <video>\n"
        "\n"
        "Options:\n"
        "  -o, --output <n>   Target output (default: all)\n"
        "  -g, --gpu <path>      VA-API device (e.g., /dev/dri/renderD128)\n"
        "  -s, --scale <mode>    fit, fill, stretch (default: fill)\n"
        "  -l, --no-loop         Don't loop\n"
        "  -n, --no-hwaccel      Software decode\n"
        "  -v, --verbose         Debug output\n"
        "  -h, --help            Show help\n",
        prog);
}

static ScaleMode parse_scale(const char *s) {
    if (!strcmp(s, "fit")) return SCALE_FIT;
    if (!strcmp(s, "fill")) return SCALE_FILL;
    if (!strcmp(s, "stretch")) return SCALE_STRETCH;
    LOG_WARN("Unknown scale '%s', using fill", s);
    return SCALE_FILL;
}

static int parse_args(Config *cfg, int argc, char **argv) {
    static struct option opts[] = {
        {"output", required_argument, 0, 'o'},
        {"gpu", required_argument, 0, 'g'},
        {"scale", required_argument, 0, 's'},
        {"no-loop", no_argument, 0, 'l'},
        {"no-hwaccel", no_argument, 0, 'n'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    cfg->output_name = NULL;
    cfg->gpu_device = NULL;
    cfg->scale_mode = SCALE_FILL;
    cfg->loop = true;
    cfg->hw_accel = true;
    cfg->verbose = false;

    int c;
    while ((c = getopt_long(argc, argv, "o:g:s:lnvh", opts, NULL)) != -1) {
        switch (c) {
        case 'o': cfg->output_name = optarg; break;
        case 'g': cfg->gpu_device = optarg; break;
        case 's': cfg->scale_mode = parse_scale(optarg); break;
        case 'l': cfg->loop = false; break;
        case 'n': cfg->hw_accel = false; break;
        case 'v': cfg->verbose = true; break;
        case 'h': print_usage(argv[0]); exit(0);
        default: return -1;
        }
    }

    if (optind >= argc) {
        LOG_ERROR("No video file specified");
        print_usage(argv[0]);
        return -1;
    }

    cfg->video_path = argv[optind];

    if (access(cfg->video_path, R_OK) != 0) {
        LOG_ERROR("Cannot read: %s", cfg->video_path);
        return -1;
    }

    return 0;
}

static bool any_output_ready(App *app) {
    Output *out;
    wl_list_for_each(out, &app->outputs, link)
        if (out->state == OUT_READY) return true;
    return false;
}

static GpuVendor vendor_from_node(const char *node) {
    if (!node) return GPU_VENDOR_UNKNOWN;

    const char *name = strrchr(node, '/');
    name = name ? name + 1 : node;

    char path[256];
    snprintf(path, sizeof(path), "/sys/class/drm/%s/device/vendor", name);

    FILE *f = fopen(path, "r");
    if (!f) return GPU_VENDOR_UNKNOWN;

    unsigned int vid = 0;
    char buf[32];
    if (fgets(buf, sizeof(buf), f))
        sscanf(buf, "0x%x", &vid);
    fclose(f);

    switch (vid) {
    case 0x8086: return GPU_VENDOR_INTEL;
    case 0x1002: return GPU_VENDOR_AMD;
    case 0x10de: return GPU_VENDOR_NVIDIA;
    default: return GPU_VENDOR_UNKNOWN;
    }
}

/* Check if output matches filter criteria */
static bool output_matches_filter(Output *out, const Config *cfg) {
    if (!cfg->output_name || strcmp(cfg->output_name, "*") == 0)
        return true;
    return strcmp(out->name, cfg->output_name) == 0;
}

/*
 * Reset the EGL renderer after context loss.
 *
 * This is a full reset: destroy and recreate EGL context, clear all caches,
 * and reset DMA-BUF compatibility state. Called when:
 * - Compositor restarts (layer_closed received)
 * - EGL_CONTEXT_LOST error
 */
static bool reset_renderer(App *app) {
    LOG_INFO("Resetting renderer (EGL context) after compositor event");

    if (app->renderer) {
        Output *out;
        wl_list_for_each(out, &app->outputs, link)
            renderer_destroy_output(app->renderer, out);
        renderer_destroy(app->renderer);
        app->renderer = NULL;
    }

    if (renderer_init(&app->renderer, app->display) < 0) {
        LOG_ERROR("Renderer reinit failed");
        return false;
    }

    /*
     * Full reset: clear cache AND reset DMA-BUF compatibility state.
     * This is the only place where we reset dmabuf_tested/dmabuf_works,
     * because a new EGL context might have different capabilities.
     */
    renderer_clear_cache(app->renderer);
    renderer_reset_dmabuf_state(app->renderer);
    renderer_reset_texture_state(app->renderer);

    /* Also increment decoder generation to invalidate any cached surface refs */
    decoder_increment_generation(app->decoder);

    /* Re-create EGL surfaces for outputs that still have Wayland surfaces */
    Output *out;
    wl_list_for_each(out, &app->outputs, link) {
        if (!out->surface)
            continue;

        if (out->state == OUT_READY || out->state == OUT_WAITING_CALLBACK) {
            if (renderer_create_output(app->renderer, out) == 0) {
                LOG_INFO("Output %s: EGL surface recreated after reset", out->name);
            } else {
                LOG_WARN("Output %s: failed to recreate EGL surface, will recreate Wayland surface",
                         out->name);
                wayland_destroy_surface(out);
                /* State is now OUT_PENDING_RECREATE */
            }
        }
    }

    app->render_path_determined = false;
    app->renderer_needs_reset = false;
    return true;
}

/*
 * Process deferred output lifecycle operations.
 *
 * This function handles the state machine transitions for outputs:
 * - OUT_PENDING_DESTROY → destroy Wayland resources → OUT_PENDING_RECREATE
 * - OUT_PENDING_RECREATE → create new surface → OUT_UNCONFIGURED
 *
 * Returns true if any surface was successfully recreated.
 */
static bool process_output_lifecycle(App *app) {
    bool any_recreated = false;
    Output *out;

    wl_list_for_each(out, &app->outputs, link) {
        /* Handle deferred destruction */
        if (out->state == OUT_PENDING_DESTROY) {
            LOG_DEBUG("Output %s: processing deferred destruction", out->name);

            /* EGL resources already destroyed in layer_closed callback */
            /* Destroy Wayland resources - this transitions to OUT_PENDING_RECREATE */
            wayland_destroy_surface(out);

            /* State is now OUT_PENDING_RECREATE */
        }

        /* Recreate surface if needed and output info is complete */
        if (out->state == OUT_PENDING_RECREATE &&
            out->width > 0 && out->height > 0 && out->name[0] &&
            output_matches_filter(out, &app->config)) {

            LOG_INFO("Output %s: recreating surface", out->name);

            if (wayland_create_surface(out, app) == 0) {
                /*
                 * Wait for compositor to send configure event before creating
                 * EGL surface. This is essential - without it, wl_egl_window_create
                 * may fail or produce incorrect results.
                 */
                wl_display_roundtrip(app->display);

                if (out->state == OUT_READY) {
                    if (app->renderer && renderer_create_output(app->renderer, out) == 0) {
                        LOG_INFO("Output %s: surface recreated successfully", out->name);
                        any_recreated = true;
                    } else {
                        LOG_ERROR("Output %s: failed to create EGL surface", out->name);
                        wayland_destroy_surface(out);
                        /* Will retry on next iteration */
                    }
                } else {
                    LOG_WARN("Output %s: surface not configured after roundtrip (state=%s)",
                             out->name, output_state_name(out->state));
                    /* Surface created but not configured yet - this is okay,
                     * will be configured on next compositor event */
                }
            } else {
                LOG_ERROR("Output %s: failed to create Wayland surface", out->name);
                /* Will retry on next iteration */
            }
        }

        /* Handle case where Wayland surface exists but EGL surface is missing */
        if (out->surface && app->renderer &&
            (out->state == OUT_READY || out->state == OUT_WAITING_CALLBACK) &&
            (!out->egl_surface || out->egl_surface == EGL_NO_SURFACE)) {

            LOG_DEBUG("Output %s: reattaching EGL surface", out->name);
            if (renderer_create_output(app->renderer, out) == 0) {
                LOG_INFO("Output %s: EGL surface reattached", out->name);
                any_recreated = true;
            } else {
                LOG_WARN("Output %s: failed to reattach EGL surface, will recreate",
                         out->name);
                wayland_destroy_surface(out);
                /* State is now OUT_PENDING_RECREATE, will be handled on next iteration */
            }
        }
    }

    return any_recreated;
}

/* --- Main --- */

int main(int argc, char **argv) {
    App app = {0};
    g_app = &app;
    g_log_start_time = now();
    wl_list_init(&app.outputs);

    if (parse_args(&app.config, argc, argv) < 0)
        return 1;

    LOG_INFO("wlvideo: %s", app.config.video_path);

    struct sigaction sa = { .sa_handler = handle_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Initialize subsystems */
    if (wayland_init(&app) < 0) {
        LOG_ERROR("Wayland init failed");
        return 1;
    }

    if (renderer_init(&app.renderer, app.display) < 0) {
        LOG_ERROR("Renderer init failed");
        wayland_destroy(&app);
        return 1;
    }

    /* Decide which GPU to use for decoding */
    const char *decode_gpu = app.config.gpu_device;
    GpuVendor render_vendor = renderer_get_gpu_vendor(app.renderer);
    GpuVendor requested_vendor = vendor_from_node(app.config.gpu_device);

    if (decode_gpu && render_vendor != GPU_VENDOR_UNKNOWN &&
        requested_vendor != GPU_VENDOR_UNKNOWN &&
        requested_vendor != render_vendor &&
        !getenv("WLVIDEO_ALLOW_GPU_MISMATCH")) {
        LOG_WARN("Requested GPU (%s) differs from render GPU (%s)",
                 vendor_name(requested_vendor), vendor_name(render_vendor));
        LOG_WARN("Using render GPU for zero-copy. Set WLVIDEO_ALLOW_GPU_MISMATCH=1 to override.");
        decode_gpu = NULL;
    }

    if (decoder_init(&app.decoder, app.config.video_path, app.config.hw_accel, decode_gpu) < 0) {
        LOG_ERROR("Decoder init failed");
        renderer_destroy(app.renderer);
        wayland_destroy(&app);
        return 1;
    }

    int vid_w, vid_h;
    double fps;
    bool hw_active;
    decoder_get_info(app.decoder, &vid_w, &vid_h, &fps, &hw_active);
    app.frame_duration = 1.0 / fps;

    GpuVendor decode_vendor = decoder_get_gpu_vendor(app.decoder);
    LOG_INFO("Video: %dx%d @ %.2f fps, HW: %s, GPU: %s",
             vid_w, vid_h, fps, hw_active ? "yes" : "no", vendor_name(decode_vendor));

    if (sw_ring_init(&app.sw_ring, vid_w, vid_h) < 0) {
        LOG_ERROR("Ring buffer init failed");
        decoder_destroy(app.decoder);
        renderer_destroy(app.renderer);
        wayland_destroy(&app);
        return 1;
    }

    /* Create surfaces on outputs */
    int surface_count = 0;
    Output *out;
    wl_list_for_each(out, &app.outputs, link) {
        if (!output_matches_filter(out, &app.config))
            continue;

        if (wayland_create_surface(out, &app) < 0) {
            LOG_ERROR("Output %s: surface creation failed", out->name);
            continue;
        }

        /* Wait for configure */
        wl_display_roundtrip(app.display);

        if (out->state != OUT_READY) {
            LOG_WARN("Output %s: not configured after roundtrip", out->name);
            wayland_destroy_surface(out);
            continue;
        }

        if (renderer_create_output(app.renderer, out) < 0) {
            LOG_ERROR("Output %s: EGL surface failed", out->name);
            wayland_destroy_surface(out);
            continue;
        }

        surface_count++;
    }

    if (surface_count == 0) {
        LOG_ERROR("No surfaces created");
        sw_ring_destroy(&app.sw_ring);
        decoder_destroy(app.decoder);
        renderer_destroy(app.renderer);
        wayland_destroy(&app);
        return 1;
    }

    wl_display_roundtrip(app.display);

    /* Main loop */
    app.running = true;
    app.use_dmabuf_path = decoder_dmabuf_export_supported(app.decoder);
    app.render_path_determined = !app.use_dmabuf_path;

    int wl_fd = wl_display_get_fd(app.display);
    struct pollfd pfd = { .fd = wl_fd, .events = POLLIN };

    Frame frame = {0};
    /* Initialize DMA-BUF FDs to invalid */
    for (int i = 0; i < 4; i++) {
        frame.hw.dmabuf.fd[i] = -1;
    }

    bool have_frame = false;
    int64_t displayed_frame = -1;

    /* How many frames we can skip per iteration before resetting clock */
    const int max_skip = 5;
    const int reset_threshold = max_skip * 2;

    while (app.running && !quit) {
        /* Reset renderer if requested (e.g., after compositor restart) */
        if (app.renderer_needs_reset) {
            if (have_frame && frame.type == FRAME_HW) {
                decoder_close_dmabuf(&frame.hw.dmabuf);
                have_frame = false;
            }
            if (!reset_renderer(&app)) {
                LOG_ERROR("Renderer reset failed, exiting");
                break;
            }
        }

        /* Process deferred surface lifecycle operations */
        bool surfaces_recreated = process_output_lifecycle(&app);

        /*
         * After surface recreation, invalidate cached frame data.
         * Note: We only clear the EGL cache here, NOT the DMA-BUF compatibility
         * state. The driver's ability to import DMA-BUFs doesn't change just
         * because a surface was recreated.
         */
        if (surfaces_recreated) {
            LOG_INFO("Surfaces recreated, clearing EGL cache");
            renderer_clear_cache(app.renderer);
            if (have_frame && frame.type == FRAME_HW) {
                decoder_close_dmabuf(&frame.hw.dmabuf);
            }
            have_frame = false;
            /*
             * Note: Don't reset render_path_determined here!
             * Surface recreation doesn't change DMA-BUF compatibility.
             * Only reset it on actual context loss (in reset_renderer).
             */
        }

        /* Prepare Wayland events */
        while (wl_display_prepare_read(app.display) != 0)
            wl_display_dispatch_pending(app.display);

        if (wl_display_flush(app.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(app.display);
            LOG_ERROR("Wayland display flush failed: %s", strerror(errno));
            break;
        }

        /* Compute poll timeout */
        double t = now();
        int timeout_ms;

        if (!app.clock_started) {
            timeout_ms = 16;
        } else if (!any_output_ready(&app)) {
            timeout_ms = 100;
        } else {
            double next = app.start_time + (displayed_frame + 1) * app.frame_duration;
            double delta = next - t;
            timeout_ms = delta > 0 ? (int)(delta * 1000 + 0.5) : 0;
            if (timeout_ms > 100) timeout_ms = 100;
        }

        int ret = poll(&pfd, 1, timeout_ms);
        t = now();

        if (ret < 0) {
            wl_display_cancel_read(app.display);
            if (errno == EINTR) continue;
            LOG_ERROR("poll failed: %s", strerror(errno));
            break;
        }

        if (ret > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_read_events(app.display) < 0) {
                LOG_ERROR("Wayland read events failed");
                break;
            }
            wl_display_dispatch_pending(app.display);
        } else {
            wl_display_cancel_read(app.display);
        }

        if (wl_display_get_error(app.display)) {
            LOG_ERROR("Wayland display error: %d", wl_display_get_error(app.display));
            break;
        }

        if (!any_output_ready(&app)) continue;

        /* Start clock on first ready output */
        if (!app.clock_started) {
            app.clock_started = true;
            app.start_time = t;
            displayed_frame = -1;
        }

        /* Figure out which frame should be displayed now */
        double elapsed = t - app.start_time;
        int64_t target = (int64_t)(elapsed / app.frame_duration);

        if (target > displayed_frame) {
            /* Close previous frame's DMA-BUF handles */
            if (have_frame && frame.type == FRAME_HW)
                decoder_close_dmabuf(&frame.hw.dmabuf);

            int decoded = 0;
            while (displayed_frame < target && decoded < max_skip) {
                bool need_sw = !app.render_path_determined || !app.use_dmabuf_path ||
                               decode_vendor == GPU_VENDOR_NVIDIA;

                if (!decoder_get_frame(app.decoder, &frame, &app.sw_ring, need_sw)) {
                    if (app.config.loop) {
                        if (decoder_seek_start(app.decoder) < 0) {
                            app.running = false;
                            break;
                        }
                        renderer_clear_cache(app.renderer);
                        app.start_time = t;
                        displayed_frame = -1;
                        target = 0;
                        continue;
                    }
                    app.running = false;
                    break;
                }

                have_frame = true;
                displayed_frame++;
                decoded++;

                if (displayed_frame >= target) break;

                /* Skip this frame - MUST close DMA-BUF FDs to prevent leak */
                if (frame.type == FRAME_HW) {
                    decoder_close_dmabuf(&frame.hw.dmabuf);
                }
            }

            /* If still far behind, reset clock rather than skip forever */
            if (target - displayed_frame > reset_threshold) {
                LOG_WARN("Decode too slow, resetting clock");
                app.start_time = t - displayed_frame * app.frame_duration;
            }
        }

        /* Render to all ready outputs */
        if (have_frame) {
            wl_list_for_each(out, &app.outputs, link) {
                if (out->state != OUT_READY) continue;

                wayland_request_frame(out);

                bool try_dmabuf = !app.render_path_determined || app.use_dmabuf_path;
                bool ok = renderer_draw(app.renderer, out, &frame, &app.sw_ring,
                                        app.config.scale_mode, try_dmabuf);

                /* Handle render failure (e.g., invalid EGL surface) */
                if (!ok && !frame.sw.available) {
                    LOG_WARN("Output %s: render failed, marking for recreation", out->name);
                    /* Trigger destruction via state machine */
                    if (app.renderer) {
                        renderer_destroy_output(app.renderer, out);
                    }
                    out->state = OUT_PENDING_DESTROY;
                    continue;
                }

                /* Detect render path on first frame */
                if (!app.render_path_determined) {
                    app.render_path_determined = true;
                    if (frame.type == FRAME_HW && try_dmabuf) {
                        app.use_dmabuf_path = ok;
                        decoder_set_dmabuf_export_result(app.decoder, ok);
                        LOG_INFO("Render path: %s", ok ? "zero-copy" : "software");
                    } else {
                        app.use_dmabuf_path = false;
                        LOG_INFO("Render path: software");
                    }
                }

                out->frames_rendered++;
            }
            app.frame_counter++;
        }
    }

    /* Cleanup */
    LOG_INFO("Exiting after %lu frames", (unsigned long)app.frame_counter);

    if (have_frame && frame.type == FRAME_HW)
        decoder_close_dmabuf(&frame.hw.dmabuf);

    /* Log per-output stats and cleanup */
    wl_list_for_each(out, &app.outputs, link) {
        if (out->frames_rendered > 0)
            LOG_INFO("Output %s: %lu frames rendered", out->name, (unsigned long)out->frames_rendered);
        renderer_destroy_output(app.renderer, out);
        wayland_destroy_surface(out);
    }

    sw_ring_destroy(&app.sw_ring);
    decoder_destroy(app.decoder);
    renderer_destroy(app.renderer);
    wayland_destroy(&app);

    return 0;
}