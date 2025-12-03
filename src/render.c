/*
 * render.c — EGL/OpenGL ES 2.0 renderer
 *
 * Two rendering paths:
 * 1. DMA-BUF import: create EGLImage from DMA-BUF, bind as external texture.
 *    Driver handles YUV->RGB conversion. Used when zero-copy is available.
 * 2. Software upload: upload Y and UV planes separately, convert in shader.
 *    Used when DMA-BUF import fails.
 *
 * EGLImage cache avoids repeated eglCreateImageKHR calls for the same surface.
 * Cache entries are keyed by (surface_id, generation) to handle surface reuse.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wayland-egl.h>
#include <drm_fourcc.h>

#include "wlvideo.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;

extern const char *fourcc_to_str(uint32_t f);

static const char *egl_error_name(EGLint err) {
    switch (err) {
    case EGL_SUCCESS: return "SUCCESS";
    case EGL_BAD_ACCESS: return "BAD_ACCESS";
    case EGL_BAD_ALLOC: return "BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE: return "BAD_ATTRIBUTE";
    case EGL_BAD_CONFIG: return "BAD_CONFIG";
    case EGL_BAD_CONTEXT: return "BAD_CONTEXT";
    case EGL_BAD_DISPLAY: return "BAD_DISPLAY";
    case EGL_BAD_MATCH: return "BAD_MATCH";
    case EGL_BAD_PARAMETER: return "BAD_PARAMETER";
    case EGL_BAD_SURFACE: return "BAD_SURFACE";
    default: return "UNKNOWN";
    }
}

static GpuVendor vendor_from_gl_renderer(const char *renderer) {
    if (!renderer) return GPU_VENDOR_UNKNOWN;

    char lower[256];
    size_t len = strlen(renderer);
    if (len >= sizeof(lower)) len = sizeof(lower) - 1;
    for (size_t i = 0; i < len; i++)
        lower[i] = tolower((unsigned char)renderer[i]);
    lower[len] = '\0';

    if (strstr(lower, "nvidia") || strstr(lower, "geforce")) return GPU_VENDOR_NVIDIA;
    if (strstr(lower, "intel")) return GPU_VENDOR_INTEL;
    if (strstr(lower, "amd") || strstr(lower, "radeon")) return GPU_VENDOR_AMD;
    return GPU_VENDOR_UNKNOWN;
}

/* Vertex shader: simple transform with scale/offset */
static const char *vert_src =
    "#version 100\n"
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "uniform vec4 u_transform;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_pos * u_transform.xy + u_transform.zw, 0.0, 1.0);\n"
    "    v_uv = a_uv;\n"
    "}\n";

/* NV12 fragment shader with colorspace/range conversion */
static const char *frag_nv12_src =
    "#version 100\n"
    "precision highp float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex_y;\n"
    "uniform sampler2D u_tex_uv;\n"
    "uniform int u_colorspace;\n"
    "uniform int u_range;\n"
    "\n"
    "vec3 yuv_to_rgb_601(float y, float u, float v) {\n"
    "    return vec3(y + 1.402*v, y - 0.344*u - 0.714*v, y + 1.772*u);\n"
    "}\n"
    "vec3 yuv_to_rgb_709(float y, float u, float v) {\n"
    "    return vec3(y + 1.575*v, y - 0.187*u - 0.468*v, y + 1.856*u);\n"
    "}\n"
    "vec3 yuv_to_rgb_2020(float y, float u, float v) {\n"
    "    return vec3(y + 1.475*v, y - 0.165*u - 0.571*v, y + 1.881*u);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    float y_raw = texture2D(u_tex_y, v_uv).r;\n"
    "    vec2 uv_raw = texture2D(u_tex_uv, v_uv).rg;\n"
    "    float y, u, v;\n"
    "    if (u_range == 0) {\n"
    "        y = (y_raw - 0.0627) * 1.164;\n"
    "        u = (uv_raw.r - 0.502) * 1.138;\n"
    "        v = (uv_raw.g - 0.502) * 1.138;\n"
    "    } else {\n"
    "        y = y_raw;\n"
    "        u = uv_raw.r - 0.5;\n"
    "        v = uv_raw.g - 0.5;\n"
    "    }\n"
    "    vec3 rgb;\n"
    "    if (u_colorspace == 0) rgb = yuv_to_rgb_601(y, u, v);\n"
    "    else if (u_colorspace == 2) rgb = yuv_to_rgb_2020(y, u, v);\n"
    "    else rgb = yuv_to_rgb_709(y, u, v);\n"
    "    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n"
    "}\n";

/* External texture shader for DMA-BUF path */
static const char *frag_external_src =
    "#version 100\n"
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform samplerExternalOES u_tex;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_tex, v_uv);\n"
    "}\n";

typedef struct {
    uintptr_t surface_id;
    uint64_t generation;
    EGLImage image;
    uint64_t last_use;
} CacheEntry;

struct Renderer {
    EGLDisplay dpy;
    EGLContext ctx;
    EGLConfig cfg;

    GLuint prog_nv12, prog_ext;
    GLint u_transform_nv12, u_tex_y, u_tex_uv, u_colorspace, u_range;
    GLint u_transform_ext, u_tex_ext;

    GLuint vbo;
    GLuint tex_y, tex_uv, tex_dmabuf;
    int tex_w, tex_h;
    bool tex_allocated;

    CacheEntry cache[EGL_CACHE_SIZE];
    uint64_t frame_count;

    bool has_dmabuf;
    bool has_modifiers;
    bool has_yuv_hint;
    bool has_rg_texture;
    bool dmabuf_tested;
    bool dmabuf_works;

    char gl_renderer[128];
    GpuVendor gpu_vendor;
};

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);

    GLint ok;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        LOG_ERROR("Shader error: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint link_program(const char *vert, const char *frag) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vert);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag);
    if (!vs || !fs) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_pos");
    glBindAttribLocation(prog, 1, "a_uv");
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        LOG_ERROR("Link error: %s", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static bool has_egl_extension(EGLDisplay dpy, const char *ext) {
    const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
    if (!exts) return false;

    size_t len = strlen(ext);
    const char *p = exts;
    while ((p = strstr(p, ext))) {
        if ((p == exts || p[-1] == ' ') && (p[len] == 0 || p[len] == ' '))
            return true;
        p += len;
    }
    return false;
}

int renderer_init(Renderer **out, struct wl_display *display) {
    Renderer *r = calloc(1, sizeof(Renderer));
    if (!r) return -1;

    r->dpy = eglGetDisplay((EGLNativeDisplayType)display);
    if (r->dpy == EGL_NO_DISPLAY) {
        LOG_ERROR("eglGetDisplay failed");
        goto fail;
    }

    EGLint major, minor;
    if (!eglInitialize(r->dpy, &major, &minor)) {
        LOG_ERROR("eglInitialize failed");
        goto fail;
    }
    LOG_INFO("EGL %d.%d", major, minor);

    /* Check extensions */
    r->has_dmabuf = has_egl_extension(r->dpy, "EGL_EXT_image_dma_buf_import");
    r->has_modifiers = r->has_dmabuf && has_egl_extension(r->dpy, "EGL_EXT_image_dma_buf_import_modifiers");
    r->has_yuv_hint = has_egl_extension(r->dpy, "EGL_EXT_yuv_surface");

    if (r->has_dmabuf) {
        eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
        if (!eglCreateImageKHR || !eglDestroyImageKHR || !glEGLImageTargetTexture2DOES)
            r->has_dmabuf = false;
    }

    LOG_INFO("DMA-BUF import: %s", r->has_dmabuf ? "yes" : "no");
    LOG_INFO("DMA-BUF modifiers: %s", r->has_modifiers ? "yes" : "no");

    /* Choose config */
    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLint num_cfg;
    if (!eglChooseConfig(r->dpy, cfg_attr, &r->cfg, 1, &num_cfg) || num_cfg == 0) {
        LOG_ERROR("eglChooseConfig failed");
        goto fail;
    }

    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    r->ctx = eglCreateContext(r->dpy, r->cfg, EGL_NO_CONTEXT, ctx_attr);
    if (r->ctx == EGL_NO_CONTEXT) {
        LOG_ERROR("eglCreateContext failed");
        goto fail;
    }

    eglMakeCurrent(r->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, r->ctx);

    const char *gl_renderer = (const char *)glGetString(GL_RENDERER);
    if (gl_renderer) {
        strncpy(r->gl_renderer, gl_renderer, sizeof(r->gl_renderer) - 1);
        r->gpu_vendor = vendor_from_gl_renderer(gl_renderer);
        LOG_INFO("GL: %s", r->gl_renderer);
    }

    const char *gl_exts = (const char *)glGetString(GL_EXTENSIONS);
    r->has_rg_texture = gl_exts && strstr(gl_exts, "GL_EXT_texture_rg");

    /* Compile shaders */
    r->prog_nv12 = link_program(vert_src, frag_nv12_src);
    if (!r->prog_nv12) goto fail;

    r->u_transform_nv12 = glGetUniformLocation(r->prog_nv12, "u_transform");
    r->u_tex_y = glGetUniformLocation(r->prog_nv12, "u_tex_y");
    r->u_tex_uv = glGetUniformLocation(r->prog_nv12, "u_tex_uv");
    r->u_colorspace = glGetUniformLocation(r->prog_nv12, "u_colorspace");
    r->u_range = glGetUniformLocation(r->prog_nv12, "u_range");

    r->prog_ext = link_program(vert_src, frag_external_src);
    if (r->prog_ext) {
        r->u_transform_ext = glGetUniformLocation(r->prog_ext, "u_transform");
        r->u_tex_ext = glGetUniformLocation(r->prog_ext, "u_tex");
    }

    /* Fullscreen quad geometry */
    static const float verts[] = {
        -1, -1,  0, 1,
         1, -1,  1, 1,
        -1,  1,  0, 0,
         1,  1,  1, 0,
    };
    glGenBuffers(1, &r->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    glGenTextures(1, &r->tex_y);
    glGenTextures(1, &r->tex_uv);
    glGenTextures(1, &r->tex_dmabuf);

    for (int i = 0; i < EGL_CACHE_SIZE; i++)
        r->cache[i].image = EGL_NO_IMAGE;

    *out = r;
    return 0;

fail:
    renderer_destroy(r);
    return -1;
}

void renderer_destroy(Renderer *r) {
    if (!r) return;

    eglMakeCurrent(r->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, r->ctx);

    for (int i = 0; i < EGL_CACHE_SIZE; i++)
        if (r->cache[i].image != EGL_NO_IMAGE)
            eglDestroyImageKHR(r->dpy, r->cache[i].image);

    glDeleteTextures(1, &r->tex_y);
    glDeleteTextures(1, &r->tex_uv);
    glDeleteTextures(1, &r->tex_dmabuf);
    glDeleteBuffers(1, &r->vbo);
    glDeleteProgram(r->prog_nv12);
    glDeleteProgram(r->prog_ext);

    if (r->ctx != EGL_NO_CONTEXT) eglDestroyContext(r->dpy, r->ctx);
    if (r->dpy != EGL_NO_DISPLAY) eglTerminate(r->dpy);

    free(r);
}

GpuVendor renderer_get_gpu_vendor(Renderer *r) {
    return r ? r->gpu_vendor : GPU_VENDOR_UNKNOWN;
}

const char *renderer_get_gl_renderer(Renderer *r) {
    return r ? r->gl_renderer : NULL;
}

int renderer_create_output(Renderer *r, Output *out) {
    out->egl_window = wl_egl_window_create(out->surface, out->width, out->height);
    if (!out->egl_window) return -1;

    out->egl_surface = eglCreateWindowSurface(r->dpy, r->cfg, (EGLNativeWindowType)out->egl_window, NULL);
    if (out->egl_surface == EGL_NO_SURFACE) {
        wl_egl_window_destroy(out->egl_window);
        out->egl_window = NULL;
        return -1;
    }
    return 0;
}

void renderer_destroy_output(Renderer *r, Output *out) {
    if (out->egl_surface) {
        eglDestroySurface(r->dpy, out->egl_surface);
        out->egl_surface = EGL_NO_SURFACE;
    }
    if (out->egl_window) {
        wl_egl_window_destroy(out->egl_window);
        out->egl_window = NULL;
    }
}

void renderer_clear_cache(Renderer *r) {
    if (!r) return;

    eglMakeCurrent(r->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, r->ctx);

    for (int i = 0; i < EGL_CACHE_SIZE; i++) {
        if (r->cache[i].image != EGL_NO_IMAGE) {
            eglDestroyImageKHR(r->dpy, r->cache[i].image);
            r->cache[i].image = EGL_NO_IMAGE;
        }
        r->cache[i].surface_id = 0;
    }
    LOG_DEBUG("EGL cache cleared");
}

/* Compute scale transform for aspect ratio */
static void compute_transform(float *out, int vid_w, int vid_h, int out_w, int out_h, ScaleMode mode) {
    float sx = 1.0f, sy = 1.0f;
    float vid_aspect = (float)vid_w / vid_h;
    float out_aspect = (float)out_w / out_h;

    switch (mode) {
    case SCALE_FIT:
        if (vid_aspect > out_aspect) sy = out_aspect / vid_aspect;
        else sx = vid_aspect / out_aspect;
        break;
    case SCALE_FILL:
        if (vid_aspect > out_aspect) sx = vid_aspect / out_aspect;
        else sy = out_aspect / vid_aspect;
        break;
    case SCALE_STRETCH:
        break;
    }

    out[0] = sx; out[1] = sy; out[2] = 0.0f; out[3] = 0.0f;
}

/* Find or allocate cache entry for surface */
static CacheEntry *cache_get(Renderer *r, uintptr_t surface_id, uint64_t generation) {
    /* Look for existing entry */
    for (int i = 0; i < EGL_CACHE_SIZE; i++)
        if (r->cache[i].surface_id == surface_id && r->cache[i].generation == generation)
            return &r->cache[i];

    /* Find empty or LRU slot */
    int best = 0;
    uint64_t oldest = r->cache[0].last_use;
    for (int i = 0; i < EGL_CACHE_SIZE; i++) {
        if (r->cache[i].surface_id == 0) { best = i; break; }
        if (r->cache[i].last_use < oldest) { oldest = r->cache[i].last_use; best = i; }
    }

    CacheEntry *e = &r->cache[best];
    if (e->image != EGL_NO_IMAGE) {
        eglDestroyImageKHR(r->dpy, e->image);
        e->image = EGL_NO_IMAGE;
    }
    e->surface_id = surface_id;
    e->generation = generation;
    return e;
}

/* Render frame via DMA-BUF import (zero-copy path) */
static bool render_dmabuf(Renderer *r, Output *out, Frame *frame, ScaleMode scale) {
    if (!r->has_dmabuf || !r->prog_ext) return false;
    if (r->dmabuf_tested && !r->dmabuf_works) return false;

    DmaBuf *dmabuf = &frame->hw.dmabuf;

    /* Build EGL attributes */
    CacheEntry *ce = cache_get(r, frame->hw.surface_id, frame->hw.generation);
    if (ce->image == EGL_NO_IMAGE) {
        uint64_t mod[4];
        for (int i = 0; i < 4; i++)
            mod[i] = (dmabuf->modifier[i] == DRM_FORMAT_MOD_INVALID) ? DRM_FORMAT_MOD_LINEAR : dmabuf->modifier[i];

        /* Check modifier support */
        for (int p = 0; p < dmabuf->num_planes; p++) {
            if (!r->has_modifiers && mod[p] != DRM_FORMAT_MOD_LINEAR) {
                if (!r->dmabuf_tested)
                    LOG_WARN("EGL doesn't support modifier 0x%llx", (unsigned long long)mod[p]);
                r->dmabuf_tested = true;
                r->dmabuf_works = false;
                ce->surface_id = 0;
                return false;
            }
        }

        int w = dmabuf->width > 0 ? dmabuf->width : frame->width;
        int h = dmabuf->height > 0 ? dmabuf->height : frame->height;

        EGLint attr[64];
        int i = 0;

        attr[i++] = EGL_WIDTH; attr[i++] = w;
        attr[i++] = EGL_HEIGHT; attr[i++] = h;
        attr[i++] = EGL_LINUX_DRM_FOURCC_EXT; attr[i++] = dmabuf->fourcc;

        /* Plane 0 */
        attr[i++] = EGL_DMA_BUF_PLANE0_FD_EXT; attr[i++] = dmabuf->fd[0];
        attr[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attr[i++] = dmabuf->offset[0];
        attr[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT; attr[i++] = dmabuf->stride[0];
        if (r->has_modifiers) {
            attr[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT; attr[i++] = mod[0] & 0xffffffff;
            attr[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT; attr[i++] = mod[0] >> 32;
        }

        /* Plane 1 */
        if (dmabuf->num_planes > 1 && dmabuf->fd[1] >= 0) {
            attr[i++] = EGL_DMA_BUF_PLANE1_FD_EXT; attr[i++] = dmabuf->fd[1];
            attr[i++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT; attr[i++] = dmabuf->offset[1];
            attr[i++] = EGL_DMA_BUF_PLANE1_PITCH_EXT; attr[i++] = dmabuf->stride[1];
            if (r->has_modifiers) {
                attr[i++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT; attr[i++] = mod[1] & 0xffffffff;
                attr[i++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT; attr[i++] = mod[1] >> 32;
            }
        }

        /* Colorspace hints */
        #define EGL_YUV_COLOR_SPACE_HINT_EXT 0x327B
        #define EGL_ITU_REC601_EXT 0x327F
        #define EGL_ITU_REC709_EXT 0x3280
        #define EGL_ITU_REC2020_EXT 0x3281
        #define EGL_SAMPLE_RANGE_HINT_EXT 0x327C
        #define EGL_YUV_FULL_RANGE_EXT 0x3282
        #define EGL_YUV_NARROW_RANGE_EXT 0x3283

        if (r->has_yuv_hint) {
            attr[i++] = EGL_YUV_COLOR_SPACE_HINT_EXT;
            attr[i++] = (frame->colorspace == CS_BT601) ? EGL_ITU_REC601_EXT :
                        (frame->colorspace == CS_BT2020) ? EGL_ITU_REC2020_EXT : EGL_ITU_REC709_EXT;
            attr[i++] = EGL_SAMPLE_RANGE_HINT_EXT;
            attr[i++] = (frame->color_range == CR_FULL) ? EGL_YUV_FULL_RANGE_EXT : EGL_YUV_NARROW_RANGE_EXT;
        }

        attr[i++] = EGL_NONE;

        ce->image = eglCreateImageKHR(r->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attr);

        if (ce->image == EGL_NO_IMAGE) {
            EGLint err = eglGetError();
            if (!r->dmabuf_tested) {
                LOG_WARN("DMA-BUF import failed: %s (0x%x)", egl_error_name(err), err);
                LOG_WARN("  fourcc=%s %dx%d mod=0x%llx", fourcc_to_str(dmabuf->fourcc), w, h, (unsigned long long)mod[0]);
            }
            r->dmabuf_tested = true;
            r->dmabuf_works = false;
            ce->surface_id = 0;
            return false;
        }

        if (!r->dmabuf_tested) {
            LOG_INFO("DMA-BUF import OK, using zero-copy path");
            r->dmabuf_tested = true;
            r->dmabuf_works = true;
        }
    }

    ce->last_use = r->frame_count;

    /* Bind texture and draw */
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, r->tex_dmabuf);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, ce->image);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glUseProgram(r->prog_ext);

    float transform[4];
    compute_transform(transform, frame->width, frame->height, out->width, out->height, scale);
    glUniform4fv(r->u_transform_ext, 1, transform);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, r->tex_dmabuf);
    glUniform1i(r->u_tex_ext, 0);

    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    return true;
}

/* Render frame via software upload */
static void render_software(Renderer *r, Output *out, Frame *frame, SoftwareRing *ring, ScaleMode scale) {
    int slot = frame->sw.ring_slot;
    const uint8_t *y_data = sw_ring_get_y(ring, slot);
    const uint8_t *uv_data = sw_ring_get_uv(ring, slot);
    int w = frame->width, h = frame->height;

    GLenum y_fmt = r->has_rg_texture ? GL_RED_EXT : GL_LUMINANCE;
    GLenum uv_fmt = r->has_rg_texture ? GL_RG_EXT : GL_LUMINANCE_ALPHA;

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    /* Y texture */
    glBindTexture(GL_TEXTURE_2D, r->tex_y);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (!r->tex_allocated || r->tex_w != w || r->tex_h != h) {
        glTexImage2D(GL_TEXTURE_2D, 0, y_fmt, w, h, 0, y_fmt, GL_UNSIGNED_BYTE, NULL);
        r->tex_w = w; r->tex_h = h; r->tex_allocated = true;
    }

    if (ring->y_stride == w)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, y_fmt, GL_UNSIGNED_BYTE, y_data);
    else
        for (int row = 0; row < h; row++)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, w, 1, y_fmt, GL_UNSIGNED_BYTE, y_data + row * ring->y_stride);

    /* UV texture */
    glBindTexture(GL_TEXTURE_2D, r->tex_uv);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    int uv_w = w / 2, uv_h = h / 2;
    static int prev_uv_w = 0, prev_uv_h = 0;
    if (prev_uv_w != uv_w || prev_uv_h != uv_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, uv_fmt, uv_w, uv_h, 0, uv_fmt, GL_UNSIGNED_BYTE, NULL);
        prev_uv_w = uv_w; prev_uv_h = uv_h;
    }

    if (ring->uv_stride == w)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_w, uv_h, uv_fmt, GL_UNSIGNED_BYTE, uv_data);
    else
        for (int row = 0; row < uv_h; row++)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, uv_w, 1, uv_fmt, GL_UNSIGNED_BYTE, uv_data + row * ring->uv_stride);

    /* Draw */
    glUseProgram(r->prog_nv12);

    float transform[4];
    compute_transform(transform, w, h, out->width, out->height, scale);
    glUniform4fv(r->u_transform_nv12, 1, transform);

    glUniform1i(r->u_colorspace, (frame->colorspace == CS_BT601) ? 0 : (frame->colorspace == CS_BT2020) ? 2 : 1);
    glUniform1i(r->u_range, (frame->color_range == CR_FULL) ? 1 : 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r->tex_y);
    glUniform1i(r->u_tex_y, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, r->tex_uv);
    glUniform1i(r->u_tex_uv, 1);

    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

bool renderer_draw(Renderer *r, Output *out, Frame *frame, SoftwareRing *ring, ScaleMode scale, bool try_dmabuf) {
    eglMakeCurrent(r->dpy, out->egl_surface, out->egl_surface, r->ctx);
    glViewport(0, 0, out->width, out->height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    r->frame_count++;

    bool dmabuf_ok = false;
    if (try_dmabuf && frame->type == FRAME_HW)
        dmabuf_ok = render_dmabuf(r, out, frame, scale);

    if (!dmabuf_ok && frame->sw.available)
        render_software(r, out, frame, ring, scale);

    eglSwapBuffers(r->dpy, out->egl_surface);
    return dmabuf_ok;
}