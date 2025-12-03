project('wlvideo', 'c',
  version: '0.0.1',
  license: 'MIT',
  default_options: ['c_std=c11', 'warning_level=2', 'b_lto=true'],
)

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments(['-Wno-unused-parameter', '-D_POSIX_C_SOURCE=200809L']), language: 'c')

# Dependencies
wayland_client = dependency('wayland-client', version: '>=1.20')
wayland_egl = dependency('wayland-egl')
wayland_protocols = dependency('wayland-protocols', version: '>=1.25')
egl = dependency('egl')
glesv2 = dependency('glesv2')
libavcodec = dependency('libavcodec', version: '>=58.0')
libavformat = dependency('libavformat', version: '>=58.0')
libavutil = dependency('libavutil', version: '>=56.0')
libdrm = dependency('libdrm')
libm = cc.find_library('m', required: false)

libva = dependency('libva', required: false)
libva_drm = dependency('libva-drm', required: false)

cuda_available = cc.has_header('libavutil/hwcontext_cuda.h') and cc.has_header('cuda.h')

conf = configuration_data()
if libva.found() and libva_drm.found()
  conf.set('HAVE_VAAPI', true)
  message('VA-API: enabled')
else
  message('VA-API: disabled')
endif

if cuda_available
  conf.set('HAVE_CUDA', true)
  message('CUDA/NVDEC: enabled')
else
  message('CUDA/NVDEC: disabled')
endif

configure_file(output: 'config.h', configuration: conf)

# Protocol generation
wayland_scanner = find_program('wayland-scanner')
protocols_dir = wayland_protocols.get_variable('pkgdatadir')

xdg_shell_xml = protocols_dir / 'stable/xdg-shell/xdg-shell.xml'
xdg_shell_src = custom_target('xdg-shell-src', input: xdg_shell_xml, output: 'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
xdg_shell_hdr = custom_target('xdg-shell-hdr', input: xdg_shell_xml, output: 'xdg-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

layer_shell_xml = files('protocol/wlr-layer-shell-unstable-v1.xml')
layer_shell_src = custom_target('layer-shell-src', input: layer_shell_xml, output: 'wlr-layer-shell-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
layer_shell_hdr = custom_target('layer-shell-hdr', input: layer_shell_xml, output: 'wlr-layer-shell-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

dmabuf_xml = protocols_dir / 'unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml'
dmabuf_src = custom_target('dmabuf-src', input: dmabuf_xml, output: 'linux-dmabuf-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
dmabuf_hdr = custom_target('dmabuf-hdr', input: dmabuf_xml, output: 'linux-dmabuf-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

proto_src = [xdg_shell_src, xdg_shell_hdr, layer_shell_src, layer_shell_hdr, dmabuf_src, dmabuf_hdr]

sources = ['src/main.c', 'src/wayland.c', 'src/decode.c', 'src/render.c', 'src/wlvideo.h']

deps = [wayland_client, wayland_egl, egl, glesv2, libavcodec, libavformat, libavutil, libdrm]
if libva.found() and libva_drm.found()
  deps += [libva, libva_drm]
endif
if libm.found()
  deps += libm
endif

executable('wlvideo', sources + proto_src,
  dependencies: deps,
  include_directories: include_directories('.'),
  install: true)
