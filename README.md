# wlvideo

A high-performance video wallpaper application for wlroots-based Wayland compositors. Designed for minimal resource consumption through hardware-accelerated decoding and zero-copy GPU buffer sharing.

## Supported Compositors

Requires compositors implementing the `wlr-layer-shell-unstable-v1` protocol:

- **Sway** — Reference wlroots compositor
- And etc.

> **Note:** GNOME (Mutter), Weston, and other non-wlroots compositors are not supported as they do not implement the layer-shell protocol.

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                              wlvideo                                    │
├─────────────┬─────────────┬─────────────────────┬──────────────────────┤
│   main.c    │  wayland.c  │      decode.c       │      render.c        │
│             │             │                     │                      │
│ Event loop  │ Layer shell │ FFmpeg demuxer      │ EGL context          │
│ Clock sync  │ Surface mgmt│ VA-API HW decode    │ EGLImage cache       │
│ Path select │ Frame cb    │ DMA-BUF export      │ GLES2 shaders        │
└─────────────┴─────────────┴─────────────────────┴──────────────────────┘
```

### Data Flow

The application implements two rendering paths, automatically selecting based on hardware capability:

```
ZERO-COPY PATH (Intel/AMD)
══════════════════════════

  FFmpeg ──► VA-API ──► vaExportSurfaceHandle() ──► DMA-BUF FDs
                                                        │
                              EGLImage Cache ◄──────────┘
                                    │
                    eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)
                                    │
                                    ▼
                    glEGLImageTargetTexture2DOES()
                                    │
                         GL_TEXTURE_EXTERNAL_OES
                                    │
                              GLES2 Render ──► eglSwapBuffers()

  • Decoded frames remain in GPU VRAM throughout
  • No CPU involvement in pixel data transfer
  • Driver performs YUV→RGB conversion automatically


FALLBACK PATH (NVIDIA, or when DMA-BUF import fails)
════════════════════════════════════════════════════

  FFmpeg ──► VA-API ──► av_hwframe_transfer_data() ──► System RAM
                                                           │
                                Ring Buffer ◄──────────────┘
                                    │
                         glTexSubImage2D() × 2
                              (Y plane, UV plane)
                                    │
                              NV12 Shader
                              (YUV→RGB)
                                    │
                              GLES2 Render ──► eglSwapBuffers()

  • GPU→CPU readback required (decode GPU VRAM → system RAM)
  • CPU→GPU upload required (system RAM → render GPU VRAM)
  • Shader performs colorspace conversion
```

## Principle of Operation

### 1. Wayland Surface Initialization (`wayland.c`)

The application creates a background layer surface following the wlr-layer-shell protocol lifecycle:

1. **Surface Creation**: `wl_compositor_create_surface()` followed by `zwlr_layer_shell_v1_get_layer_surface()` with `ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND`
2. **Configuration**: Size set to `(0, 0)` with all four anchors (`TOP|BOTTOM|LEFT|RIGHT`), instructing the compositor to provide actual output dimensions. Exclusive zone set to `-1` to extend behind panels
3. **Initial Commit**: Per protocol requirements, first commit contains no buffer attachment
4. **Configure Handling**: Compositor sends `configure(serial, width, height)`; client must `ack_configure(serial)` before buffer attachment becomes valid
5. **Frame Callbacks**: `wl_surface_frame()` provides vsync-aligned render timing; callback destroyed and recreated each frame

Output state machine ensures clean lifecycle transitions:
```
OUT_UNCONFIGURED ──[configure]──► OUT_READY ◄──[frame_done]──┐
        ▲                            │                       │
        │                    [request_frame]                 │
        │                            ▼                       │
OUT_PENDING_RECREATE ◄───── OUT_WAITING_CALLBACK ────────────┘
        ▲
        │
OUT_PENDING_DESTROY ◄──[layer_closed]
```

### 2. Video Decoding (`decode.c`)

**Hardware Context Initialization:**
- `av_hwdevice_ctx_create()` with `AV_HWDEVICE_TYPE_VAAPI` and render node path
- Codec context receives `hw_device_ctx` reference
- `get_format` callback selects `AV_PIX_FMT_VAAPI` from offered formats

**DMA-BUF Export (Intel/AMD):**
- `vaExportSurfaceHandle()` with `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2` provides file descriptors and modifiers
- `VADRMPRIMESurfaceDescriptor` contains: fourcc, dimensions, objects (fd, modifier), layers (plane mappings)
- File descriptors transferred to renderer; decoder closes unused descriptors

**Software Fallback:**
- `av_hwframe_transfer_data()` performs GPU→CPU readback into NV12 format
- Data copied to preallocated ring buffer slot
- Ring buffer provides Y and UV plane pointers to renderer

**Surface Generation Tracking:**
- Stable identifier incremented only on seek/loop
- Combined with VA surface ID, forms unique key for EGL cache
- Prevents stale cache entries after position changes

### 3. Rendering (`render.c`)

**EGL Setup:**
- Platform display from `wl_display`
- OpenGL ES 2.0 context with `EGL_WINDOW_BIT` surface type
- Extension probing: `EGL_EXT_image_dma_buf_import`, `EGL_EXT_image_dma_buf_import_modifiers`

**Zero-Copy Import:**
```c
EGLint attribs[] = {
    EGL_WIDTH, width,
    EGL_HEIGHT, height,
    EGL_LINUX_DRM_FOURCC_EXT, fourcc,
    EGL_DMA_BUF_PLANE0_FD_EXT, fd[0],
    EGL_DMA_BUF_PLANE0_OFFSET_EXT, offset[0],
    EGL_DMA_BUF_PLANE0_PITCH_EXT, stride[0],
    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (modifier & 0xFFFFFFFF),
    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (modifier >> 32),
    // ... plane 1 for UV ...
    EGL_NONE
};
EGLImage image = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, 
                                    EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
```

**EGLImage Cache:**
- LRU cache with 8 entries (matching typical VA-API surface pool size)
- Key: `(surface_id, generation)` tuple
- Avoids repeated `eglCreateImageKHR()` calls for reused surfaces
- Cleared on seek, loop, or compositor restart

**Shader Programs:**
- **External texture shader**: Samples `GL_TEXTURE_EXTERNAL_OES`; driver handles YUV→RGB
- **NV12 shader**: Separate Y (`GL_LUMINANCE`/`GL_RED_EXT`) and UV (`GL_LUMINANCE_ALPHA`/`GL_RG_EXT`) textures with colorspace matrices for BT.601/BT.709/BT.2020

### 4. Timing and Synchronization (`main.c`)

**Playback Clock:**
- `CLOCK_MONOTONIC` provides stable time reference
- Frame display time: `start_time + frame_number × frame_duration`
- Frame skipping when decode can't keep up (max 5 frames per iteration)
- Clock reset if falling behind by more than 10 frames

**Event Loop:**
- Single-threaded `poll()` on Wayland display FD
- Timeout computed from next frame deadline
- Wayland events dispatched; render triggered when outputs ready

## Memory Efficiency

The application achieves low memory footprint through several architectural decisions:

### Fixed Allocation Strategy

- **Ring Buffer**: Two slots preallocated at startup, sized for video resolution. No per-frame allocation.
- **EGLImage Cache**: Eight fixed entries. LRU eviction prevents unbounded growth.
- **No Dynamic Buffers**: All working memory allocated during initialization.

### Zero-Copy Elimination of Copies

When the zero-copy path succeeds:
- Decoded frames exist only in GPU VRAM
- No system RAM copy of pixel data
- DMA-BUF file descriptors reference existing GPU allocations

### Minimal State

- Single-threaded: no synchronization overhead
- No frame queue: decode-on-demand model
- Immediate resource cleanup: DMA-BUF FDs closed after import

### Memory Scaling

Memory consumption scales primarily with:
1. **Video resolution**: Ring buffer size = `(width × height × 1.5) × 2` bytes (NV12, 2 slots)
2. **VA-API surface pool**: Driver-managed, typically 8-20 surfaces depending on codec and reference frame requirements
3. **EGL resources**: Texture objects and EGLImages are GPU-resident

The fallback path necessarily increases memory usage as frames must exist in both GPU VRAM (decode) and system RAM (ring buffer) simultaneously.

## Hardware Compatibility

| GPU | Driver | HW Decode | Zero-Copy | Notes |
|-----|--------|-----------|-----------|-------|
| Intel Gen9+ | iHD (intel-media-driver) | ✅ H.264, HEVC, VP9, AV1¹ | ✅ | Full DMA-BUF support with tiled modifiers |
| AMD GCN+ | radeonsi (libva-mesa-driver) | ✅ H.264, HEVC, VP9, AV1² | ✅ | Full DMA-BUF support, DCC compression |
| NVIDIA Maxwell+ | nvidia-vaapi-driver | ✅ H.264, HEVC, VP8/9, AV1³ | ❌ | HW decode works; DMA-BUF modifiers incompatible with EGL import |

¹ AV1 requires Tiger Lake or newer  
² AV1 requires VCN 3.0 (RDNA 2) or newer  
³ AV1 requires Ampere (RTX 30 series) or newer; 10-bit content unsupported by driver

### NVIDIA Limitations

The `nvidia-vaapi-driver` exports DMA-BUFs with proprietary tiled modifiers (0x30000000XXXXXXXX range). These modifiers cannot be imported by:
- Mesa's EGL implementation (used when rendering on Intel/AMD iGPU)
- NVIDIA's own proprietary EGL (architectural limitation)

Consequently, **NVIDIA always requires the software fallback path**, regardless of single-GPU or hybrid configurations.

## Dependencies

### Build-time
```
wayland-client >= 1.20
wayland-egl
wayland-protocols >= 1.25
egl
glesv2
libdrm
libavcodec >= 58.0
libavformat >= 58.0
libavutil >= 56.0
libva (optional, for VA-API)
libva-drm (optional, for VA-API)
```

### Runtime
- **VA-API driver**: `intel-media-driver`, `libva-mesa-driver`, or `nvidia-vaapi-driver`
- **wlroots compositor**: Sway, Hyprland, river, etc.

## Building

```bash
# Fetch wlr-layer-shell protocol XML
./setup-protocols.sh

# Configure
meson setup build

# Compile
ninja -C build

# Install (optional)
sudo ninja -C build install
```

## Usage

```
wlvideo [options] <video>

Options:
  -o, --output <name>   Target specific output (default: all)
  -g, --gpu <path>      VA-API render node (e.g., /dev/dri/renderD129)
  -s, --scale <mode>    fit | fill | stretch (default: fill)
  -l, --no-loop         Play once and exit
  -n, --no-hwaccel      Force software decode
  -v, --verbose         Enable debug logging
  -h, --help            Show help
```

### Examples

```bash
# Basic usage
wlvideo ~/Videos/wallpaper.mp4

# Specific output with letterboxing
wlvideo -o DP-1 --scale fit video.mp4

# NVIDIA hybrid laptop (decode on discrete GPU)
LIBVA_DRIVER_NAME=nvidia NVD_BACKEND=direct \
    prime-run wlvideo --gpu /dev/dri/renderD129 video.mp4

# Debug output
wlvideo -v video.mp4
```

### Environment Variables

| Variable | Purpose |
|----------|---------|
| `LIBVA_DRIVER_NAME=nvidia` | Select nvidia-vaapi-driver |
| `NVD_BACKEND=direct` | Required for nvidia-vaapi-driver on driver 525+ |
| `WLVIDEO_ALLOW_GPU_MISMATCH` | Permit decode/render GPU mismatch (disables zero-copy optimization) |

## Troubleshooting

### "Decode too slow, resetting playback clock"

The decoder cannot maintain real-time playback. Common causes:

1. **Excessive bitrate**: Hardware decoders have throughput limits. Re-encode:
   ```bash
   ffmpeg -i input.mp4 -c:v libx265 -crf 23 -preset medium output.mp4
   ```
2. **Software decode active**: Check verbose output for "HW decode: yes". Verify VA-API with `vainfo`.
3. **Thermal throttling**: Monitor GPU temperature during playback.

### Black screen

1. Verify compositor supports layer-shell: `wayland-info | grep layer_shell`
2. Verify VA-API: `vainfo --display drm --device /dev/dri/renderD128`
3. Test software path: `wlvideo --no-hwaccel video.mp4`

### DMA-BUF import fails (fallback to software)

Expected on NVIDIA. On Intel/AMD, check:
- Kernel DRM driver loaded (`lsmod | grep -E 'i915|amdgpu'`)
- Mesa version supports required modifiers
- No GPU mismatch between decode and render

### High memory usage on NVIDIA

Expected behavior. The fallback path requires:
- Decoded surfaces in GPU VRAM (decode side)
- Ring buffer in system RAM (transfer staging)
- Texture data in GPU VRAM (render side)

## FFmpeg Compatibility

The codebase handles FFmpeg API changes:

| FFmpeg Version | Notes |
|----------------|-------|
| 4.x - 6.x | Uses `FF_PROFILE_*` constants |
| 7.0+ | Uses `AV_PROFILE_*` constants (auto-detected via `LIBAVCODEC_VERSION_MAJOR`) |

## Design Rationale

**Single-threaded**: Video wallpaper does not require sub-frame latency. Complexity of thread synchronization outweighs benefits for this use case.

**Fixed buffers**: Preallocating ring buffer and cache entries eliminates allocation jitter during playback and prevents memory growth over time.

**Graceful degradation**: Zero-copy is attempted first; on failure, the application seamlessly falls back to software upload without user intervention.

**LRU cache**: VA-API surfaces are reused by the decoder. Caching EGLImages for recently-seen surfaces avoids redundant import operations.

## License

MIT

## See Also

- [wlr-protocols](https://gitlab.freedesktop.org/wlroots/wlr-protocols) — Layer shell protocol specification
- [VA-API](https://github.com/intel/libva) — Video Acceleration API
- [nvidia-vaapi-driver](https://github.com/elFarto/nvidia-vaapi-driver) — NVIDIA VA-API implementation