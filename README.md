# wlvideo

High-performance video wallpaper for wlroots-based Wayland compositors with zero-copy rendering.

## Features

- **Hardware decode** via VA-API (Intel, AMD, NVIDIA via nvidia-vaapi-driver)
- **Zero-copy rendering** on Intel/AMD through DMA-BUF import
- **Automatic fallback** to software rendering when DMA-BUF import fails
- **Fixed memory footprint** with preallocated ring buffers
- **GPU selection** for hybrid systems (`--gpu /dev/dri/renderD129`)

## Quick Start

```bash
# Build
./setup-protocols.sh
meson setup build
ninja -C build

# Run
./build/wlvideo ~/video.mp4

# Run with verbose logging
./build/wlvideo -v ~/video.mp4
```

## How Zero-Copy Works

Zero-copy means decoded video frames stay in GPU memory without being copied to system RAM:

```
ZERO-COPY PATH (Intel/AMD with Mesa)

  VA-API Decoder ──DMA-BUF──► EGL Import ──EGLImage──► OpenGL ──► Display
       │                          │                      │
       └──────────────────────────┴──────────────────────┘
                    All in VRAM, no CPU copies

  Memory: ~50-80 MiB for 4K (VRAM only)


FALLBACK PATH (NVIDIA, or when DMA-BUF import fails)

  VA-API Decoder ──readback──► Ring Buffer ──upload──► OpenGL ──► Display
       │              │              │           │         │
   (decode GPU)   (GPU→CPU)    (system RAM)  (CPU→GPU)  (render GPU)

  Memory: ~400-500 MiB for 4K (decode VRAM + RAM + render VRAM)
```

### Why Fallback is Needed on NVIDIA

nvidia-vaapi-driver exports DMA-BUFs with tiled memory modifiers. When using Mesa's EGL (on Intel/AMD in hybrid setups, or even NVK), these modifiers can't be imported. The proprietary NVIDIA EGL also doesn't support importing these buffers back.

This means NVIDIA always requires the readback path, regardless of whether you're in hybrid mode or NVIDIA-only mode.

## Hardware Compatibility

| GPU | HW Decode | Zero-Copy | Notes |
|-----|-----------|-----------|-------|
| Intel (iHD) | ✅ H.264, HEVC, VP9, AV1 | ✅ | Full zero-copy via DMA-BUF |
| AMD (radeonsi) | ✅ H.264, HEVC, VP9, AV1 | ✅ | Full zero-copy via DMA-BUF |
| NVIDIA | ✅ H.264, HEVC, VP8/9, AV1* | ❌ | HW decode works, but needs CPU readback |

\* AV1 requires RTX 30+ (Ampere). 10-bit content not supported by nvidia-vaapi-driver.

## Dependencies

### Build-time
```
wayland-client wayland-egl wayland-protocols
egl glesv2
libdrm
libavcodec libavformat libavutil (FFmpeg ≥4.0)
libva libva-drm
```

### Runtime
- VA-API driver: `intel-media-driver`, `libva-mesa-driver`, or `nvidia-vaapi-driver`
- wlroots compositor: sway, Hyprland, river, etc.

## Build

```bash
./setup-protocols.sh
meson setup build
ninja -C build
```

## Usage

```bash
wlvideo [options] <video>

Options:
  -o, --output <name>   Target specific output (default: all)
  -g, --gpu <path>      VA-API render node (e.g., /dev/dri/renderD129)
  -s, --scale <mode>    fit | fill | stretch (default: fill)
  -l, --no-loop         Play once
  -n, --no-hwaccel      Force software decode
  -v, --verbose         Debug logging
```

### Examples

```bash
# Basic usage
wlvideo video.mp4

# Specific output with letterbox scaling
wlvideo -o eDP-1 --scale fit video.mp4

# NVIDIA hybrid laptop
prime-run wlvideo --gpu /dev/dri/renderD129 video.mp4
```

### Environment Variables

```bash
# For nvidia-vaapi-driver
export LIBVA_DRIVER_NAME=nvidia
export NVD_BACKEND=direct

# For debugging
export WLVIDEO_ALLOW_GPU_MISMATCH=1
```

## Memory Usage

| Resolution | Zero-Copy (Intel/AMD) | Fallback (NVIDIA) |
|------------|----------------------|-------------------|
| 1080p | ~50 MiB | ~150 MiB |
| 1440p | ~60 MiB | ~250 MiB |
| 4K | ~80 MiB | ~400-500 MiB |

The fallback path needs memory for: decoded surfaces (VRAM), ring buffer (RAM), and display textures (VRAM).

## Troubleshooting

### "Decode too slow, resetting playback clock"

The decoder can't keep up with playback. Most common causes:

1. **Video bitrate too high** — This is the most likely cause. Hardware decoders have bitrate limits that vary by codec and GPU. For example, a 4K HEVC video at 150+ Mbps may overwhelm an integrated GPU, while the same resolution at 20-30 Mbps plays fine. Re-encode with a reasonable bitrate:
   ```bash
   ffmpeg -i input.mp4 -c:v libx265 -crf 23 -preset medium output.mp4
   ```

2. **Software decode active** — Check verbose output for "HW decode: yes". If hardware decode isn't working, verify VA-API with `vainfo`.

3. **Thermal throttling** — Common on laptops. Monitor GPU temperature during playback.

### High memory usage on NVIDIA

This is expected. NVIDIA's VA-API driver can't export DMA-BUFs that EGL can import, so frames must be copied through system RAM. This applies to both hybrid and NVIDIA-only configurations.

### Black screen

1. Check compositor supports layer-shell: `wayland-info | grep layer_shell`
2. Verify VA-API: `vainfo`
3. Test software path: `wlvideo --no-hwaccel video.mp4`

## Architecture

```
main.c      Event loop, timing, render path selection
wayland.c   Layer shell surface management
decode.c    FFmpeg + VA-API decoder, DMA-BUF export
render.c    EGL/GLES2 renderer, EGLImage cache
```

### Design Choices

**Single-threaded** — Video wallpaper doesn't need sub-frame latency. Simplicity wins.

**Fixed buffers** — Ring buffer allocated once at startup. No per-frame malloc.

**LRU cache** — EGLImage cache avoids repeated eglCreateImageKHR calls.

**Graceful fallback** — Try zero-copy first, fall back to software upload if needed.

## License

MIT