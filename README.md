# Mali Termux WSI

Window System Integration (XCB) for Mali GPUs on Termux (Termux:X11).

## Contents

- [Overview](#overview)
- [Building](#building)
- [Vulkan](#vulkan)
- [GLFW](#glfw)
- [GLU](#glu)
- [GLUT](#glut)
- [Roadmap](#roadmap)
- [Notes from Arm engineers](#notes-from-arm-engineers)
- [Third-party code](#third-party-code)

## Overview

| Library | Path | Output | Summary |
|---|---|---|---|
| Vulkan | `Xcb/Vulkan` | `libvulkan.so.1` | Mali driver loader plus XCB WSI |
| GLFW | `Xcb/GL/GLFW` | `libglfw.so.3` | GLFW 3.6 on xcb + EGL |
| GLU | `Xcb/GL/GLU` | `libGLU.so.1` | GLU 1.3, self-contained |
| GLUT | `Xcb/GL/GLUT` | `libglut.so.3` | Small GLUT on Xlib + GLX |

Repository layout:

    Xcb/Vulkan/        Vulkan shim and WSI
    Xcb/GL/GLFW/       GLFW
    Xcb/GL/GLU/        GLU
    Xcb/GL/GLUT/       GLUT
    src/               bundled third-party code (libtess2, GLFW headers)
    makefile

## Building

    make               # build everything into build/
    make vulkan        # or: glfw, glu, glut
    make clean

Run an application against the freshly built libraries:

    LD_LIBRARY_PATH=$PWD/build ./your_app

Each library section below lists its own dependencies and options.

## Vulkan

Path: `Xcb/Vulkan`

A drop-in replacement for `libvulkan.so.1`. It loads the Mali driver through
the Android HAL (SPHAL) and adds an XCB WSI layer on top (swapchain,
DRI3/Present).

### Layout

- `Scripts/libvulkan.c` – aarch64 stubs and jump table
- `Include/mali_loader.h` – Mali HAL/driver loader
- `Include/vulkan_wsi_x11.h` – XCB WSI, swapchain, layer chain
- `Include/shim_dl.h` – dlopen indirection (bionic or libhybris for glibc)
- `Include/vulkan_calls.inc` – list of Vulkan functions (generated)

### Dependencies

    pkg install clang make libxcb vulkan-headers

`libx11` is only needed for `XLIB=1`.

### Build

    make vulkan            # build/libvulkan.so.1
    make vulkan XLIB=1     # with Xlib surface support
    make vulkan GLIBC=1    # glibc guest (libhybris)

### Usage

    LD_LIBRARY_PATH=$PWD/build vulkaninfo --summary

### Environment variables

| Variable | Effect |
|---|---|
| `MALI_ICD_LIB` | Path to the Mali driver (default `/vendor/lib64/egl/libGLES_mali.so`) |
| `VULKAN_DEBUG=1` | Verbose logging |
| `VULKAN_CPU=1` | Force CPU copy mode |
| `VULKAN_SYNC=1` | Synchronous zero-copy (no presentation thread) |
| `VULKAN_NOVSYNC=1` | Ignore requested present mode, always present async (like IMMEDIATE) |
| `VULKAN_DMABUF=1\|name` | Zero-copy via dma-buf heap, without AHB |
| `VULKAN_AHB_LINEAR=vendor3\|none` | AHB allocation usage flags |
| `VK_LAYER_PATH`, `VK_INSTANCE_LAYERS` | Layer search path / layers to enable |
| `VK_SHIM_TRACE=1` | Call trace and SIGSEGV diagnostics handler |

### How it works

**1. Stub library.** `libvulkan.c` exports every Vulkan entry point listed
in `vulkan_calls.inc`. Each one is a tiny aarch64 stub that loads a pointer
from a jump table (`g_tab[index]`) and branches to it. Only the scratch
registers x16/x17 are used, so arguments pass through untouched and the
stubs work for any function signature. If a slot is empty, the stub calls
`shim_unresolved`, which prints the missing function name and returns
`VK_ERROR_EXTENSION_NOT_PRESENT`.

**2. Loading the Mali driver.** A constructor runs at load time:

- The vendor driver (`MALI_ICD_LIB`) is opened from the `sphal` linker
  namespace (`android_dlopen_ext`, with `android_load_sphal_library` as a
  fallback). In a glibc guest, libhybris' `hybris_dlopen` is used instead.
- The `HMI` symbol is read and the `hwvulkan` device `vk0` is opened.
- `vkGetInstanceProcAddr` is found by probing slots 17-19 of the HAL device
  and checking via `/proc/self/maps` that the pointers lie inside the
  driver's executable mapping.

**3. Filling the jump table.** Global functions are resolved at startup.
After `vkCreateInstance` the table is refilled with instance-level
pointers, and after `vkCreateDevice` with device-level ones. Both lookups
go through the WSI layer first, so its functions override the driver's.

**4. WSI layer (`vulkan_wsi_x11.h`).** The Mali HAL has no X11 support, so
this layer provides it:

- Surface: `VK_KHR_surface`, `VK_KHR_xcb_surface` (and optionally
  `VK_KHR_xlib_surface`).
- Swapchain: `VK_KHR_swapchain`, `swapchain_maintenance1`, mutable format.
- These extensions are advertised to the application but removed from the
  create info before it reaches the driver.
- Extra device extensions needed for zero-copy (AHB, dma-buf, external
  semaphore/memory fd) are enabled automatically when supported.
- Requested features the GPU does not support are disabled instead of
  failing device creation.
- The API version is capped at Vulkan 1.1.

**5. Presentation paths.** The swapchain tries them in this order:

1. **Zero-copy, async (default).** Images are backed by an
   `AHardwareBuffer` (or a dma-buf heap with `VULKAN_DMABUF`). The buffer
   fd is wrapped into an X pixmap with DRI3 `PixmapFromBuffer` and the same
   memory is imported into Vulkan. On present, a sync_fd is exported from a
   semaphore; a worker thread waits on it and calls `xcb_present_pixmap`.
   For FIFO the worker waits for `PresentCompleteNotify` (vsync gating,
   disabled automatically if no event arrives within 250 ms).
2. **Zero-copy, sync** (`VULKAN_SYNC=1`). Waits on a fence in
   `vkQueuePresentKHR`, then `xcb_copy_area`.
3. **CPU copy** (`VULKAN_CPU=1`, or fallback if zero-copy fails). The image
   is copied to a host-visible buffer and sent with `xcb_put_image` in
   chunks that fit the X request size.

**6. Present modes.** FIFO is vsynced. MAILBOX drops older queued frames
and keeps the newest. IMMEDIATE (or `VULKAN_NOVSYNC=1`) presents with the
Present `ASYNC` option.

**7. Resize.** The window geometry is polled; when it differs from the
swapchain size, `vkAcquireNextImageKHR` returns `VK_ERROR_OUT_OF_DATE_KHR`
and `vkQueuePresentKHR` returns `VK_SUBOPTIMAL_KHR`, so the application
recreates the swapchain.

**8. Layers.** Layers (`libVkLayer*.so` / `libvk*.so`) are loaded from
`VK_LAYER_PATH` and chained using the loader-layer interface, with the
driver as the last link.

### Limitations

- AArch64 only.
- One instance and one device per process.

## GLFW

Path: `Xcb/GL/GLFW`

A custom GLFW 3.6 implementation for Termux:X11 on Mali GPUs. It uses pure
xcb (no Xlib), xkbcommon for keyboard input and EGL for contexts.

### Layout

- `Scripts/libglfw.c` – the whole library in one file
- `src/glfw/include/` (repo root) – original GLFW 3.6 headers

### Dependencies

    pkg install clang make libxcb libxkbcommon libglvnd vulkan-headers

The Termux GLFW package (older version) is not used: its header lacks
constants this build needs, so the headers are bundled in `src/glfw`.

### Build

    make glfw              # build/libglfw.so.3

### Environment variables

| Variable | Effect |
|---|---|
| `GLFW_SCALE=<float>` | Content scale reported to applications (default 1.0) |
| `GLFW_DEBUG=1` | Log X errors |

### How it works

- **Window and input:** xcb windows, EWMH/ICCCM properties for title, size
  hints, fullscreen, maximize and iconify. Key codes are evdev codes
  (X keycode - 8) mapped to `GLFW_KEY_*`; text comes from xkbcommon
  (xkbcommon-x11 when available, default keymap otherwise).
- **Contexts:** EGL only. `GLFW_OPENGL_ES_API` gives ES 1/2/3.
  `GLFW_OPENGL_API` binds the desktop API, which the EGL shim maps to ES.
  The display comes from `eglGetPlatformDisplay(EGL_PLATFORM_XCB_EXT)`.
- **Events:** `glfwWaitEvents` polls the xcb socket plus an internal pipe, so
  `glfwPostEmptyEvent` can wake it from other threads.
- **Cursor:** hidden, disabled (virtual cursor with pointer warp and grab)
  and captured modes; standard cursor shapes from the X cursor font.
- **Clipboard:** UTF-8 selection owner and requestor.
- **Monitors:** RandR outputs and modes, with a single-screen fallback.
- **Vulkan:** the loader is opened at runtime; surfaces use
  `VK_KHR_surface` and `VK_KHR_xcb_surface`.

### Limitations

- Joysticks and gamepads, gamma ramps, custom cursors, drag and drop,
  raw mouse motion and mouse passthrough are stubs that return the proper
  GLFW error.
- Only the X11 platform is available.
- Framebuffer size equals window size (no HiDPI scaling).

## GLU

Path: `Xcb/GL/GLU`

A self-contained implementation of GLU 1.3 for Termux. It has no X11 or
Mali-specific code and only needs an OpenGL library underneath.

### Layout

- `Scripts/glu.c` – matrices, quadrics, mipmaps, image scaling, error strings
- `Scripts/glu_nurbs.c` – NURBS curves and surfaces
- `Scripts/glu_tess.c` – polygon tessellator (wrapper over libtess2)
- `src/libtess2/` (repo root) – bundled copy of libtess2, not available in Termux

### Dependencies

    pkg install clang make libglu

`libglu` is only needed for the headers (`GL/gl.h`, `GL/glu.h`).

### Build

    make glu               # build/libGLU.so.1

### How it works

- **Projection and camera:** `gluPerspective`, `gluLookAt`, `gluProject`,
  `gluUnProject` etc. are computed on the CPU and applied through
  `glMultMatrixd`, so they work with any compatibility-profile GL.
- **Quadrics:** spheres, cylinders and disks are emitted as triangle strips,
  lines or points with `glBegin`/`glEnd`, with optional normals and texture
  coordinates.
- **Images and mipmaps:** only `GL_UNSIGNED_BYTE` is supported. Scaling uses
  a box filter when shrinking and linear interpolation when enlarging.
  1D/2D/3D mipmaps are uploaded level by level (3D uses
  `glXGetProcAddressARB("glTexImage3D")`).
- **NURBS:** B-spline evaluation on a sampled grid. In renderer mode it draws
  directly with GL; in tessellator mode it calls the user callbacks.
- **Tessellation:** contours are collected, passed to libtess2, and the
  result is returned through the usual `GLU_TESS_*` callbacks as
  `GL_TRIANGLES` (or `GL_LINE_LOOP` with `GLU_TESS_BOUNDARY_ONLY`).

### Limitations

- NURBS trimming (`gluBeginTrim`, `gluPwlCurve`) is not implemented.
- NURBS sampling is parametric, not pixel-based, so `gluLoadSamplingMatrices`
  has no effect.
- The tessellator ignores `GLU_TESS_EDGE_FLAG` and always outputs triangles.
- Image functions accept only `GL_UNSIGNED_BYTE`.

## GLUT

Path: `Xcb/GL/GLUT`

A small GLUT implementation ("mglut") on top of Xlib and GLX. It runs on
any X server (Termux:X11) with a GLX-capable `libGL`.

### Layout

- `Scripts/mglut.c` – window, GLX context, event loop, timers, input
- `Scripts/mglut_shapes.c` – solid/wire cube, sphere, cone, torus, polyhedra
- `Scripts/mglut_font.c` – bitmap fonts (`glutBitmapCharacter`, ...)
- `Scripts/mglut_stubs.c` – unimplemented API (menus, overlays, ...)
- `Include/mglut_fontdata.h` – generated glyph data

### Dependencies

    pkg install clang make freeglut libx11

`freeglut` is only needed for the header (`GL/glut.h`).

### Build

    make glut              # build/libglut.so.3

### How it works

- **Window:** `glutCreateWindow` picks a GLX visual, trying progressively
  weaker attributes (no stencil/accum, then no alpha, then minimal depth).
- **Main loop:** pending X events are dispatched to the callbacks, then
  timers, then the idle function, then the display callback. When there is
  nothing to do it sleeps in `select()` until the next event or timer.
- **Input:** key repeat can be filtered, special keys are translated to
  `GLUT_KEY_*`, wheel buttons produce a down/up pair like FreeGLUT.
- **Shapes:** generated with `glBegin`/`glEnd`; polyhedra are built at
  runtime with outward-facing normals.
- **Fonts:** 7 bitmap fonts rendered with `glBitmap` from embedded data.

### Limitations

- One window only; sub-windows are not supported.
- Menus, overlays, color index, stroke fonts, spaceball, tablet, joystick
  and video resize are stubs and do nothing.
- Game mode is a plain fullscreen window.
- Teapot, `glutSolidCylinder` and `glutBitmapString` are not implemented.

## Roadmap

- Separate backends for Wayland and Win32.
- EGL and desktop GL support, once a few remaining driver/WSI issues are
  resolved.

## Notes from Arm engineers

Answers from Arm's Mali engineers (Arm Developers Discord, `#mobile`,
October 2026) that shaped design decisions in this project. They are
informal community answers, not official Arm documentation.

### Framebuffer fetch vs. dual-source blending

For `GL_ARM_shader_framebuffer_fetch` with MRT (e.g. a G-buffer), the cost
depends on the hardware generation. There is no practical difference
between framebuffer fetch and dual-source blending: both use the same
hardware path. Background: the Arm blog post
[Vulkan subpasses: the good, the bad and the ugly](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/vulkan-subpasses-the-good-the-bad-and-the-ugly).

This is why `EXT_blend_func_extended` can be emulated with
`gl_LastFragColorARM` on Mali-G52 (Bifrost).

### Texture compression rate query

For `GL_EXT_texture_storage_compression`:

1. Call `glGetInternalformativ()` with
   `GL_NUM_SURFACE_COMPRESSION_FIXED_RATES_EXT` to get the number of rates.
2. Allocate memory for the rate list.
3. Call `glGetInternalformativ()` with `GL_SURFACE_COMPRESSION_EXT` to get
   the supported rates for the format.

### AFBC and AHardwareBuffer usage flags

- Any surface with `CPU_READ` usage is expected to disable framebuffer
  compression (AFBC). For GPU-only compression, avoid `CPU_READ` and use
  GPU usage flags only.
- Gralloc formats must be the lowest common denominator of all hardware
  that touches the surface (GPU, display, camera ISP, video). Minimize the
  number of hardware blocks involved.
- Whether AFBC survives to display scanout depends on the display
  hardware and the vendor's gralloc, which is outside Arm's control. For a
  specific device, ask the silicon vendor.
- Arm's reference implementation:
  [Mali Android Gralloc Module](https://developer.arm.com/downloads/-/Mali%20Android%20Gralloc%20Module).
  OEMs customize it, so it is a starting point only.

**Observed on MediaTek (Mali-G52):** for `AB24` (BGRA8888) the driver
exposes 13 DMA-BUF modifiers (LINEAR plus several AFBC layouts), while for
`AR24` (RGBA8888) only LINEAR is exposed. In practice AFBC works with
`AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM` and falls back to linear for RGBA.
This is a device observation, not a statement from Arm. The WSI layer
should therefore query `eglQueryDmaBufModifiersEXT` per format at runtime
instead of assuming a layout.

### Subgroup size

On Mali-G52 subgroup operations are always 8 wide. `VK_KHR_shader_quad_control`
is supported from the Mali-G57/G77 generation onward, for Vulkan only;
OpenGL ES has no equivalent.

### Native fence lifetime (open question)

When extracting a native fence with `eglCreateSyncKHR` and
`eglDupNativeFenceFDANDROID`, destroying the `EGLSyncKHR` right after the
dup can cause problems on some MediaTek drivers. Arm did not confirm the
correct lifetime, nor the behavior of `EGL_ARM_implicit_external_sync`.
Until then, keep the sync object alive until the duplicated fd has
signaled.

### Further reading

- [Mobile Application GPU Optimization Guide (MAGOG)](https://arm-software.github.io/magog/)
- [Arm GPU Best Practices](https://support.arm.com/documentation/101897/0400/)

## GBM

Path: `Xcb/EGL/GBM`

A `libgbm.so.1` replacement that allocates buffers as Android
`AHardwareBuffer`s instead of using Mesa. It lets EGL/X11 code that expects
GBM run on the Mali blob.

### Layout

- `Scripts/libgbm_ahb.c` – the whole library in one file

### Dependencies

    pkg install clang make mesa

`mesa` is only needed for the header (`gbm.h`). At runtime
`libnativewindow.so` is loaded from the system.

### Build

    make gbm               # build/libgbm.so.1

### Environment variables

| Variable | Effect |
|---|---|
| `GBM_DEBUG=1` | Log buffer allocation and native handle contents |
| `GBM_FORCE_LINEAR=1` | Always request CPU access, which disables AFBC |
| `GBM_EXTRA_USAGE=0x...` | Extra AHardwareBuffer usage bits (e.g. a vendor "no AFBC" flag) |
| `GBM_FD_INDEX=<n>` | Force which native-handle fd is returned by `gbm_bo_get_fd` |

### How it works

- **Buffers:** `gbm_bo_create` allocates an `AHardwareBuffer` with GPU
  color-output and sampled usage. Formats are mapped to HAL formats
  (ARGB/XRGB8888 to BGRA8888, ABGR8888 to RGBA8888, RGB565, ...).
- **AFBC vs. linear:** without CPU usage, Mali gralloc picks a compressed
  (AFBC) layout. Buffers requested with `SCANOUT`, `LINEAR`, `WRITE` or
  `CURSOR`, or with `GBM_FORCE_LINEAR`, get CPU read/write usage, which makes
  them linear. `gbm_bo_get_modifier` reports `LINEAR` or `INVALID`
  accordingly. See [Notes from Arm engineers](#notes-from-arm-engineers).
- **Handles and fds:** `gbm_bo_get_handle().ptr` returns the
  `ANativeWindowBuffer*` for the EGL shim. `gbm_bo_get_fd` returns a dup of
  the dma-buf fd found in the native handle.
- **Surfaces:** a pool of 3 buffers. Besides the standard
  `lock_front_buffer`/`release_buffer`, extra entry points
  (`gbm_surface_ahb_dequeue`, `gbm_surface_ahb_queue`,
  `gbm_surface_ahb_size`) let the EGL shim drive the pool.
- **DRM hooks:** if a shim `libdrm.so.2` is loaded, buffers are registered
  with it via `drm_ahb_register`/`drm_ahb_unregister`.

### Limitations

- `gbm_bo_import` is not implemented.
- `gbm_bo_map` only works on buffers with CPU access (linear).
- Single plane formats only; no multi-planar or modifier negotiation.
- Surfaces are not thread-safe.

## DRM

Path: `Xcb/drm`

A kernel-less `libdrm.so.2`. Buffers are backed by `AHardwareBuffer`, and
the KMS side (one CRTC, one connector, one plane) is emulated in
userspace. It lets DRM/KMS and GBM applications run without `/dev/dri`.

### Layout

- `Scripts/libdrm_ahb.c` – the whole library in one file

### Dependencies

    pkg install clang make libdrm

`libdrm` is only needed for the headers (`xf86drm.h`, `drm.h`, ...).

### Build

    make drm               # build/libdrm.so.2

### Usage

    LD_PRELOAD=$PWD/build/libdrm.so.2 ./your_app

Preloading is required: the library intercepts `open`, `close`, `dup`,
`fcntl`, `stat` and `mmap` so that `/dev/dri/*` nodes resolve to fake
devices.

### Environment variables

| Variable | Effect |
|---|---|
| `DRM_AHB_MODE=WxH@R` | Display mode (default: detected from X, else `1080x2400@60`) |
| `DRM_AHB_DRIVER=<name>` | Driver name from `drmGetVersion` (default `ahb`) |
| `DRM_AHB_NODE=card9,renderD199` | Only these nodes are fake; others stay real |
| `DRM_AHB_SINK=<name>` | Load `libdrm_ahb_<name>.so`, which displays flipped frames |
| `DRM_AHB_SINK_LIB=<path>` | Full path of the sink library |
| `DRM_AHB_DEBUG=1` | Debug logging |

### How it works

- **Fake devices:** opening a `/dev/dri/card*` or `renderD*` path returns an
  `eventfd` registered as a DRM node. Calls on other fds go to the real libc.
- **Buffers:** dumb buffers are allocated as `AHardwareBuffer`s with CPU and
  GPU usage. `mmap` on a dumb buffer locks the AHB and returns its address.
  Buffers from `libgbm` register via `drm_ahb_register`, keyed by
  `gbm_bo_get_handle()`.
- **PRIME:** handle to fd duplicates the dmabuf fd from the native handle;
  fd to handle matches by inode, so only our own buffers can be imported.
- **KMS:** one CRTC, encoder, connector and primary plane, with legacy
  (`SetCrtc`, `PageFlip`) and atomic (`AtomicCommit`, properties, blobs)
  paths. Flip events are delivered through the eventfd.
- **Presenting:** a flip calls a *sink*, a function that shows the buffer
  (see `DRM_AHB_SINK`).
- **Enumeration:** `drmGetDevices2` and related calls report one platform
  device, so wlroots, Mesa and weston can find it.

### Limitations

- One CRTC, one connector, one plane; no overlay or cursor planes.
- Sync objects return `ENOSYS` and the sync-object capability is off.
- Importing foreign dmabufs is not supported.
- Dumb buffers are RGBA8888, RGB565 or R8 only.

## VA-API driver

Path: `Xcb/dri`

A VA-API driver (`ahb_drv_video.so`) that decodes video with Android's
`AMediaCodec` and stores frames in `AHardwareBuffer` surfaces. It lets
VA-API applications (mpv, ffmpeg, ...) use the phone's hardware decoder.

### Layout

- `Scripts/ahb_drv_video.c` – VA-API entry point, configs, surfaces,
  contexts, buffers, decode pipeline
- `Include/ahb_images.inc` – NV12 images (`vaCreateImage`, `vaGetImage`, `vaPutImage`)
- `Include/ahb_mc.inc` – AMediaCodec backend and bitstream packetizers

### Dependencies

    pkg install clang make libva

`libva` is only needed for the headers. `libmediandk.so` is loaded from
`/system/lib64` at runtime.

### Build

    make dri               # build/ahb_drv_video.so

### Usage

    LIBVA_DRIVER_NAME=ahb LIBVA_DRIVERS_PATH=$PWD/build vainfo

### Environment variables

| Variable | Effect |
|---|---|
| `AHB_VA_PROFILES=h264,vp8,vp9,hevc,av1\|all\|default` | Profiles to expose (default: the ready ones) |
| `AHB_VA_DEBUG=1` | Debug logging |
| `AHB_VA_SYNC_MS=500` | Maximum wait in `vaSyncSurface` |
| `AHB_VA_LAYOUT=i420` | Force planar codec output (default: from the codec color format) |
| `AHB_VA_H264_LEVEL=51` | `level_idc` in the synthesized H.264 SPS |
| `AHB_VA_H264_REORDER=0` | `max_num_reorder_frames` in the SPS (0 = output as soon as possible) |

### How it works

- **Surfaces:** each surface is an NV12 `AHardwareBuffer`. Frames are read
  and written with `AHardwareBuffer_lockPlanes`, using the real pixel and
  row strides, so both NV12 and NV21 layouts work.
- **Decode flow:** `vaCreateContext` starts a codec, `vaEndPicture`
  packetizes the render buffers into one access unit and queues it with the
  target surface id as the timestamp, and `vaSyncSurface` drains the codec
  until that surface has been written.
- **H.264:** VA-API does not carry the SPS and PPS, so they are
  synthesized from the picture parameters and sent as Annex B before
  keyframes and whenever they change. The SPS requests low output delay.
- **VP8 / VP9:** the frame is passed through unchanged. Hidden frames are
  detected so they do not wait for an output.
- **Locking:** `mc_mu` is taken before `mu`, never the reverse.

### Limitations

- Profiles: H.264 (Constrained Baseline, Main, High), VP8, VP9 Profile 0.
  HEVC cannot be reconstructed from VA-API data; AV1 is software-only on
  the test device.
- Codec names are MediaTek-specific (`c2.mtk.*`).
- No `vaDeriveImage` or surface export: frames reach the application
  through `vaGetImage` (CPU copy).
- Custom H.264 scaling lists are not forwarded.
- Fixed limits: 256 surfaces, 16 contexts, up to 8192x8192.

## Third-party code

- `src/libtess2` – libtess2, SGI Free Software License B (see
  `src/libtess2/LICENSE.txt`).
- `src/glfw` – GLFW headers, zlib/libpng license (see
  `src/glfw/LICENSE.md`).
