# Mali Termux WSI

Window System Integration (XCB) for Mali GPUs on Termux (Termux:X11).

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

## Overview

A zero-copy Window System Integration (WSI) layer that lets desktop Linux
applications (X11, KDE, Minecraft, Firefox) run on Android’s proprietary
ARM Mali GPU, **without root** and **without Mesa/Zink**.

### Local device

| | |
|--|--|
| **Hardware** | Xiaomi 24040RN64Y (MediaTek) |
| **GPU** | Mali-G52 MC2 |
| **CPU** | MediaTek Helio G91 Ultra (MT6769G) |
| **OS** | Android 14 (stock, no root, locked bootloader) |
| **Environment** | Termux (Bionic linker, no proot) |

### Also tested on

| GPU | Testers (Discord) |
|-----|-------------------|
| Mali-G72, Mali-G78 | @saicharankandukuri (Zman) |
| Mali-G72 | @nonameforidea |
| Mali-G77 MC9 | @bullymaguire0024 (BullyMaguire) |

## Contents

- [Overview](#overview)
- [Building](#building)
- [Vulkan](#vulkan)
- [OpenGL](#opengl)
- [GLFW](#glfw)
- [GLU](#glu)
- [GLUT](#glut)
- [GBM](#gbm)
- [GLES](#gles)
- [EGL](#egl)
- [DRM](#drm)
- [VA-API driver](#va-api-driver)
- [Roadmap](#roadmap)
- [Notes from Arm engineers](#notes-from-arm-engineers)
- [Third-party code](#third-party-code)

| Library | Path | Output | Summary |
|---|---|---|---|
| Vulkan | `API/Vulkan` | `libvulkan.so.1` | Mali driver loader plus XCB WSI |
| OpenGL | `API/GL` | `libGL.so.1` | GLX 1.4 over EGL, desktop GL 1.x–4.6 emulated on GLES, SPIR-V via SPIRV-Cross |
| GLFW | `API/GL/GLFW` | `libglfw.so.3` | GLFW 3.6 on xcb + EGL |
| GLU | `API/GL/GLU` | `libGLU.so.1` | GLU 1.3, self-contained |
| GLUT | `API/GL/GLUT` | `libglut.so.3` | Small GLUT on Xlib + GLX |
| GBM | `API/EGL/GBM` | `libgbm.so.1` | GBM over AHardwareBuffer |
| DRM | `API/drm` | `libdrm.so.2` | Kernel-less DRM/KMS emulation |
| VA-API | `API/dri` | `ahb_drv_video.so` | Video decode via AMediaCodec |
| GLES | `API/GLES` | `libGLESv2.so.2` | GLES 1.x/2.0/3.x over the Mali blob, zero-copy AHB + DRI3 |
| EGL | `API/EGL` | `libEGL.so.1` | EGL over the Mali blob: X11, XCB, Wayland, GBM, surfaceless; zero-copy AHB + DRI3 / linux-dmabuf |


Repository layout:

    API/Vulkan/        Vulkan loader, WSI, and shims
    API/GL/            Desktop OpenGL, GLX, and EGL shims
    API/GLES/          GLES 1.x/2.x/3.x shim (zero-copy WSI)
    API/EGL/           EGL shim (X11/XCB/Wayland/GBM platforms)
    API/GL/GLFW/       Custom GLFW 3.6 implementation
    API/GL/GLU/        Custom GLU 1.3 implementation
    API/GL/GLUT/       Custom GLUT implementation
    API/EGL/GBM/       GBM over AHardwareBuffer
    API/drm/           Kernel-less DRM/KMS emulation
    API/dri/           VA-API driver (MediaCodec backend)
    src/               Bundled third-party code (libtess2, GLFW headers, bcdec)
    makefile           Build orchestration

## Building

    make               # build everything into build/
    make vulkan        # or: glfw, glu, glut, gles, drm, dri, egl, gl
    make clean

Run an application against the freshly built libraries:

    LD_LIBRARY_PATH=$PWD/build ./your_app

Each library section below lists its own dependencies and options.

## Vulkan

Path: `API/Vulkan`

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

## OpenGL

Path: `API/GL`

A `libGL.so.1` that provides GLX 1.4 on top of the EGL shim and emulates
desktop OpenGL (1.x up to 4.6 core and 3.2 compat) over the GLES that Mali offers.
It also exports all `gl*` functions (it embeds `libGLESv2_shim.c`).
SPIR-V shaders (`GL_ARB_gl_spirv`, core in 4.6) are converted to GLSL ES with
SPIRV-Cross.

### Layout

- `Scripts/glx_shim.c` – GLX API, contexts, drawables; the main C translation
  unit, it `#include`s all the other `.c` files
- `Scripts/gl_compat.c` – GL 1.x over GLES 1.1: `glBegin/glEnd`, display
  lists, `glRasterPos`/`glDrawPixels`/`glBitmap`
- `Scripts/gl_es_emul.c` – version/extension strings, GLSL → GLSL ES
  translation, S3TC/RGTC/BPTC upload handling (GPU decode with CPU fallback),
  1D textures, misc. missing calls
- `Scripts/gl_spirv.c` – `GL_ARB_gl_spirv`: `glShaderBinary` (SPIR-V format)
  and `glSpecializeShader`, SPIR-V → GLSL ES 3.20 through the SPIRV-Cross C API
- `Scripts/gl_dsa.c` – `ARB_direct_state_access` (GL 4.5): `glCreate*`,
  `glNamed*`, `glTexture*`, `glVertexArray*` ... as bind → call → restore over
  ES 3.2, plus `glGetQueryBufferObject*`
- `Scripts/gl_gl46.c` – the rest of GL 4.5/4.6 that is not DSA:
  `glMultiDraw{Arrays,Elements}IndirectCount`, `glPolygonOffsetClamp`,
  `glTextureBarrier`, `glGetTextureSubImage`, `glGetnTexImage`,
  `glGetnUniformdv`, and the `glBindBuffer` wrapper that tracks
  `GL_PARAMETER_BUFFER`
- `Scripts/gl_dt_bridge.cpp` – C++ bridge to `Include/gl_desktop_texture.h`:
  compute-shader decoding of BC1/BC2/BC3/BC6H/BC7 for compressed texture
  uploads. Compiled separately and linked into `libGL.so.1`
- `Include/gl_desktop_texture.h` – single-header GPU encoder/decoder for
  BC1–BC7 (BC6H included); also usable standalone
- `Scripts/gl_es_alias.c`, `Include/gl_alias.h` – ARB/EXT/OES/KHR aliases, DSA wrappers
- `Scripts/gl_desktop_ext.c` – `glPolygonMode`, multi-draw, BGRA uploads
- `Scripts/gl_dsb.c` – dual-source blending via `GL_ARM_shader_framebuffer_fetch`
- `Scripts/gl_clipdist.c` – `gl_ClipDistance` emulation
- `Scripts/gl_litfix.c` – int→float literal repair for GLSL ES
- `Scripts/gl_drawlog.c`, `gl_atlasdump.c` – debugging tools
- `Include/glx_autoes.h` – picks the ES version from the app's imports
- `Scripts/gl_tex1d.c` – obsolete (superseded by `gl_es_emul.c`), not built

### Dependencies

    pkg install clang make libxcb libx11 libglvnd spirv-cross

Also needs `src/bcdec` (bundled, CPU decoder), `API/GLES/Include/gles_list.h`
and the EGL shim at runtime. The GPU BC decoder needs a C++17 compiler
(`clang++`) and `libc++`. `libX11` and `libX11-xcb` are loaded with `dlopen`.

SPIRV-Cross must provide its C API (`spirv_cross/spirv_cross_c.h`) and either
the static archives (`libspirv-cross-*.a`) or `libspirv-cross-c-shared.so`.
It is linked into `libGL.so.1` and its symbols are not exported.

### Build

    make gl                # build/libGL.so.1 (also builds build/gl/gl_dt_bridge.o)

SPIRV-Cross is linked statically by default (`SPV_LIBS` in the makefile). To
link the shared library instead:

    make gl SPV_LIBS=-lspirv-cross-c-shared

### Usage

    LD_LIBRARY_PATH=$PWD/build ./your_app

### ES version selection

| Source (in priority order) | Result |
|---|---|
| `GLX_ES=1\|2\|3` | forced |
| `GLX_GL` (e.g. `21`, `33`, `46`) | `<20` → ES1, `>=30` → ES3 |
| `glXCreateContextAttribsARB` | ES profile, or major 1 → ES1, major ≥3 → ES3 |
| Imported symbols of the app | fixed-function only → ES1, shaders → ES2/3 |
| Fallback | ES2 (`GLX_DEFAULT_ES=3` for ES3) |

Fixed-function applications run on ES1; shader applications on ES3, with
the reported version up to GL 4.6.

### Environment variables

| Variable | Effect |
|---|---|
| `GLX_ES`, `GLX_GL`, `GLX_DEFAULT_ES` | ES version / reported GL version (see above) |
| `GLX_ARGB_VISUAL=0x..` | Visual id for 32-bit windows |
| `GLX_FORCE_DEPTH24` | Always report 24-bit visuals |
| `GLX_HIDE_EXT="A,B"` | Hide extensions from the application |
| `GLX_NO_GPU_BC=1` | Disable GPU BC decoding, use the CPU decoder (bcdec) for all formats |
| `GLX_NO_LITFIX=1` | Disable the int→float literal repair |
| `GL_DEBUG=1` | Verbose logging (`MALI_DEBUG`, `EGL_DEBUG` also work); shows `GPU BC decode: on/off`, and every 5 s the `GLX_EXT_texture_from_pixmap` bind statistics (binds, sub/full uploads, MB, average ms) |
| `vblank_mode=0` | Mesa-compatible: start windows with swap interval 0 (no vsync, tearing possible). The application can still change it with `glXSwapInterval*` |
| `GLX_DEBUG=1` | Log the auto ES detection |
| `GLX_TRACE=1` | Log `glGetIntegerv`/`glGetStringi` queries on stdout |
| `GLX_SHADER_DUMP=1` | Print shaders sent to Mali (also the GLSL ES generated from SPIR-V) |
| `GLX_DUMP_TEXT=1` | Save WebRender text shaders to `$TMPDIR` |
| `GLX_DSB_DEBUG=1`, `GLX_DSB_TEST=1..5` | Dual-source blending debug / test modes |
| `GLX_DRAWLOG=<n>\|text\|swap\|fb0` | Log unique draw-state signatures |
| `GLX_SWAPDUMP=x,y,w,h`, `GLX_SWAPDUMP_CH=r\|g\|b` | ASCII dump of the framebuffer at swaps 6/20/60 |
| `GLX_ATLAS_DUMP=<n>`, `GLX_ATLAS_TEX`, `GLX_ATLAS_WH` | ASCII dump of uploaded atlas glyphs |
| `GLX_ERRLOG=1` | Log GL errors in texture uploads |
| `GLX_FS_TEST`, `GLX_FS_DBG` | Fragment shader debug injection (WebRender text) |

### How it works

- **Contexts:** GLX contexts map to EGL ES contexts; drawables are windows
  (EGL window surfaces), pbuffers, or pixmaps (pbuffer + `xcb_put_image`
  on flush/swap).
- **Fixed function:** `glBegin/glEnd` become client-side vertex arrays
  (QUADS → triangles); display lists record and replay commands; `glDrawPixels`
  and `glBitmap` are drawn with textured/solid quads.
- **GLSL:** GLSL 1.10–4.40 is translated to GLSL ES 1.00 / 3.20; `sampler1D`,
  `sampler2DRect`, `sampler1DArray`, `gl_ClipDistance` and dual-source
  outputs are rewritten. Shaders that fail on int→float conversions are
  recompiled with promoted literals.
- **SPIR-V (`GL_ARB_gl_spirv`):** `glShaderBinary` with
  `GL_SHADER_BINARY_FORMAT_SPIR_V` stores the module; `glSpecializeShader`
  converts it to GLSL ES 3.20 with SPIRV-Cross (separate textures and samplers
  are combined, descriptor sets are stripped, specialization constants are
  applied) and compiles it on Mali. The stage comes from the shader object.
  Other binary formats go to the driver. If the conversion fails the shader
  simply fails to compile and the reason is printed on stderr.
- **GL 4.5 / 4.6:** `ARB_direct_state_access` is emulated by binding the
  object, calling the ES function and restoring the binding; texture targets
  are tracked from `glCreateTextures` and `glBindTexture`.
  `glMultiDraw*IndirectCount` reads the count from the parameter buffer on the
  CPU and loops over `glDraw*Indirect`; `GL_PARAMETER_BUFFER` is tracked by the
  shim because ES cannot bind it.
- **Textures:** uploads of BC1 with alpha, BC2, BC3 (S3TC) and BC6H, BC7
  (BPTC) are decoded on the GPU by compute shaders (ES 3.1+ contexts):
  the blocks go to an SSBO, are decoded into a temporary RGBA8 / SRGB8_A8 /
  RGBA16F texture and copied into the application's texture with
  `glCopyImageSubData`. The destination's `MIN_FILTER` is relaxed during the
  copy, so textures that are still incomplete (only level 0 defined) work.
  Everything else uses the CPU decoder (bcdec) and uploads uncompressed
  data: BC1 RGB (opaque, `0x83F0` / `0x8C4C`), RGTC (BC4/BC5), non-array
  cubemaps, allocations without data, and any GPU failure. ETC/EAC pass
  through; `TEXTURE_1D` uses a height-1 2D texture.
- **Queries:** `GL_VERSION`, extensions and many limits are spoofed;
  `GLX_MESA_query_renderer` is probed from the driver.

### Limitations

- Needs an ES context underneath; fixed-function calls require ES1
  (use `GLX_ES=1` if auto-detection fails).
- Not recorded in display lists: `glDrawPixels`, `glBitmap`, vertex arrays.
- No pixel transfer (scale/bias/map), no `GL_UNPACK_ROW_LENGTH` on ES1.
- `glXUseXFont`, `glClampColor`, `glProvokingVertex(FIRST)`, secondary
  color, fog coord, double attributes and conditional render are stubs.
- Clip distances are always applied (ignores `glEnable(GL_CLIP_DISTANCEn)`).
- `glPolygonMode` LINE/POINT only for triangles in emulated ES2/3 contexts.
- GPU BC decoding: BC6H textures end up as RGBA16F (not RGB16F), and the
  GPU path creates a temporary texture and buffer per upload. The 2D upload
  path is covered by tests; sub-image, 3D/array and PBO uploads use the same
  code but are less tested. Use `GLX_NO_GPU_BC=1` to compare with the CPU path.
- SPIR-V: specialization constants are passed as raw 32-bit values, no
  SPIR-V extensions are reported (`GL_NUM_SPIR_V_EXTENSIONS` is 0), and
  features that GLSL ES 3.20 lacks (for example `double`) cannot be converted.
- GL 4.5/4.6 gaps: `glPolygonOffsetClamp` ignores the clamp value; indirect
  draws need `baseInstance` = 0 in the commands (an ES limit); 
  `glGetTextureSubImage` only reads RGBA with `UNSIGNED_BYTE` or `FLOAT`;
  `glGetCompressedTexImage`, `glGetCompressedTextureImage` and
  `glGetCompressedTextureSubImage` do nothing (ES cannot read back compressed
  data); `glTextureBarrier` is a memory barrier; the `GL_QUERY_BUFFER` binding
  is not emulated.
- Advertised only: `GL_ARB_cull_distance`, `GL_ARB_derivative_control`,
  `GL_ARB_shader_group_vote`, `GL_ARB_shader_atomic_counter_ops`,
  `GL_ARB_pipeline_statistics_query`, `GL_ARB_transform_feedback_overflow_query`,
  `GL_ARB_conditional_render_inverted` and `GL_KHR_no_error` are listed so that
  GL 4.5/4.6 feature checks (for example `glewinfo`) pass, but they have no
  real implementation. Hide them with `GLX_HIDE_EXT` if an application
  misbehaves.
- Do not load this together with `libGLESv2.so.2` in the same process.
- `GLX_EXT_texture_from_pixmap`: Mali has no `EGL_KHR_image_pixmap`, so
  `glXBindTexImageEXT` reads the X pixmap back with `xcb_get_image` and
  uploads it with `glTexImage2D` (a copy on every bind, not zero-copy).
  Only `GL_TEXTURE_2D` is supported: no `GL_TEXTURE_RECTANGLE` or 1D targets,
  no mipmapped pixmaps (`GLX_BIND_TO_MIPMAP_TEXTURE_EXT` is 0) and no
  `GLX_FRONT_RIGHT/BACK/AUX` buffers. Rendering to a GLX pixmap while it is
  bound is not reflected until the next bind.
- GLX pixmaps are EGL pbuffers copied back to the X pixmap with `xcb_put_image`
  on `glFlush`/`glFinish`/`glXWaitGL`/`glXSwapBuffers`/context switch.
  This is slow for large pixmaps and not suited to per-frame use.
- `GLX_EXT_buffer_age` is reported only on the dmabuf present path (age 1,
  since the swap acts as a copy). The legacy WSI fallback, pixmaps and
  pbuffers report 0, so applications redraw the whole frame. Querying a
  drawable that is not current returns 0 instead of raising `GLXBadDrawable`.
- `GLX_EXT_swap_control_tear` is advertised but adaptive vsync does not exist:
  a negative interval behaves as plain vsync. The swap interval applies to the
  current surface, not to the drawable passed to `glXSwapIntervalEXT`, so call
  it with the context current. `GLX_MAX_SWAP_INTERVAL_EXT` is a nominal value;
  intervals above 1 behave as 1.
- `GLX_SGIX_fbconfig` entry points are aliases of the GLX 1.3 ones.
  `GLX_ARB_create_context_no_error` and `GLX_ARB_context_flush_control` are
  advertised only; their attributes are ignored.
- GLX always exposes one visual (the root visual, or a 32-bit ARGB one) and
  one screen. `glXCopyContext` and `glXSelectEvent` are no-ops, and
  `GLX_DRAWABLE_TYPE` always reports window, pixmap and pbuffer, even for
  configs that lack `EGL_PBUFFER_BIT`.
- Mailbox / no-vsync presenting (`SHIM_MAILBOX=1`, or swap interval 0 with
  `EGL_SWAP_TEARING_HINT_VSYNC_EXT`) does not limit the frame rate by itself.
  The application keeps rendering as fast as the GPU allows, and every frame
  still costs a GPU blit and a fence wait. Frames are only dropped when all
  present buffers are still busy, so if the display or the X server cannot
  consume frames that fast, most of the work is wasted and CPU/GPU load,
  heat and battery use stay high. The FPS shown by the application counts
  `glXSwapBuffers` calls, including dropped frames, so it can be much higher
  than what reaches the screen. Use `SHIM_STATS=1` to see the real
  presented/dropped counts. Cap the frame rate in the application, or keep
  vsync on, for anything heavy. Swap interval 0 without mailbox presents
  asynchronously and can tear.
- If a swap or fence wait takes longer than `SHIM_STALL_MS` (default 250 ms),
  the shim forces vsync on that surface to recover, so a very slow frame can
  silently turn a no-vsync setup back into a vsynced one.
- Running without vsync can overload the whole device, not just the
  application. Mailbox mode (`SHIM_MAILBOX=1`) with glmark2 at 1920x1080 and
  the default sync mode froze the Termux:X11 UI until the process was
  stopped with Ctrl+C. At 800x600 it only caused some stutter. With
  `SHIM_SYNC=implicit` the same 1920x1080 test showed some stutter but the UI
  stayed stable, so the sync mode matters as well as the resolution. The
  exact bottleneck (GPU, X server or compositor) was not identified, and
  other devices or drivers may behave differently.
  The shim bounds the number of frames in flight (`SHIM_BUFFERS`, 4 by default,
  8 at most), but the bound is not absolute: if the server stops sending
  `IDLE_NOTIFY` events, the shim stops waiting after 300 ms and presents
  without waiting for free buffers. Keep vsync on for heavy or untested
  applications, especially at high resolutions, try `SHIM_SYNC=implicit` if
  no-vsync freezes the UI, and check the real presented/dropped rates with
  `SHIM_STATS=1`. If the UI freezes, stop the application from a terminal
  (Ctrl+C or `kill`).
  
## GLFW

Path: `API/GL/GLFW`

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

Path: `API/GL/GLU`

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

Path: `API/GL/GLUT`

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

- Adding supports display for Wayland and Win32.
- fixing issues on blender crashing without clue
- real implementations for the OpenGL 4.6 extensions that are only advertised
  today (see the OpenGL limitations)
- and much more!

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

Path: `API/EGL/GBM`

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

## GLES

Path: `API/GLES`

A GLES 1.x / 2.0 / 3.x shim over the Mali blob. All GLES entry points are
forwarded to the vendor driver; on top of that, a direct WSI presents the
frames through DRI3/Present with no CPU copy.

### Layout

- `Scripts/libGLESv2_shim.c` – trampolines, WSI, swap logic
- `Include/gles_list.h` – list of forwarded GLES functions
- `API/EGL/Include/mali_sphal.h` – Mali driver loader (shared with EGL)

### Dependencies

    pkg install clang make libxcb

### Build

    make gles              # build/libGLESv2.so.2

One library is built; `libGLESv2.so`, `libGLESv3.so`, `libGLESv1_CM.so.1`
and `libGLESv1_CM.so` are copy to it.

### Usage

    LD_LIBRARY_PATH=$PWD/build ./your_app

### Environment variables

| Variable | Effect |
|---|---|
| `GL_DEBUG=1` | Verbose logging (`MALI_DEBUG` and `EGL_DEBUG` also work) |
| `SHIM_DMABUF=0` | Disable the zero-copy WSI, use the original path |
| `SHIM_SYNC=finish\|flush\|implicit` | Sync before Present (default: EGL fence; `implicit` uses `EGL_ARM_implicit_external_sync`) |
| `SHIM_PRESENT_COPY=1` | Present with the `COPY` option |
| `SHIM_FORMAT=1\|5` | Force AHB format: 5 = BGRA8888, 1 = RGBA8888 + swizzle |
| `SHIM_W`, `SHIM_H` | Size of the window and buffers (default 1280x720) |
| `SHIM_REAL_SWAP=1` | Also call the driver's real `eglSwapBuffers` |

### How it works

- **Forwarding:** every function in `gles_list.h` is an aarch64 trampoline
  that jumps to the pointer resolved from the Mali driver at load time.
  The driver is opened through SPHAL by a constructor.
- **Offscreen rendering:** on first use the shim creates an offscreen FBO
  (RGBA8 texture + depth/stencil). `glBindFramebuffer(0)` is redirected to
  it, so the application renders there instead of to a window surface.
- **Swap:** `eglSwapBuffers` runs a GPU pass that flips Y, swizzles if
  needed and forces alpha to 1, writing into one of 3 buffers.
- **Buffers:** each is an `AHardwareBuffer` with CPU_READ usage (linear,
  no AFBC), imported twice: as an X pixmap (DRI3 `PixmapFromBuffer`) and as
  an `EGLImage` bound to a texture. BGRA is tried first (native for X);
  if it fails, RGBA with a swizzle in the blit shader.
- **Present:** a worker thread waits on an EGL fence, then calls
  `xcb_present_pixmap`. `PresentIdleNotify` marks a buffer free again; if
  no event arrives within 300 ms, waiting is disabled automatically.
- **Lazy init:** the WSI starts on the first `glBindFramebuffer(0)` or
  `eglSwapBuffers` with a GLES 3 context. It saves and restores the GL
  state it touches.
- **EGL shim:** if the EGL shim from this project is loaded
  (`shim_x11_default_fbo` found), the WSI in this library is disabled and
  the EGL shim handles presentation.

### Limitations

- aarch64 only.
- Zero-copy applies only to GLES 3 contexts; GLES 1/2 contexts use the
  original driver path.
- The shim creates its own X window of `SHIM_W` x `SHIM_H`; there is no
  resize handling.
- One context and one window per process.
- The default framebuffer is emulated: its contents are the application's
  FBO, not a real window surface.

## EGL

Path: `API/EGL`

A `libEGL.so.1` that wraps the Mali blob and adds X11 window surfaces.
Applications can create windows with Xlib or xcb, or render through GBM,
without an EGL X11 platform in the driver.

### Layout

- `Scripts/egl_x11_shim.c` – EGL API, platforms, surfaces, zero-copy WSI
- `Scripts/wsi_wayland.c`, `Include/wsi_wayland.h` – Wayland transport (linux-dmabuf, `wl_buffer.release`, frame callbacks)
- `Include/shim_wayland.inc` – Wayland display/surface glue, included by `egl_x11_shim.c`
- `Scripts/wl_server.c`, `Include/wl_server.h` – `EGL_WL_bind_wayland_display` (`wl_drm` server side)
- `Include/wsi_egl_x11.h`, `mali_sphal.h`, `mali_native_fence.h` – fallback WSI, driver loader, native fence helper
- `src/mesa/src/egl/wayland/wayland-drm/wayland-drm.xml` – `wl_drm` protocol (from Mesa)
- `build/wayland/` – generated by `wayland-scanner` (not in git): `linux-dmabuf-client.h`, `linux-dmabuf-protocol.c`, `wayland-drm-server-protocol.h`, `wayland-drm-protocol.c`

### Dependencies

      pkg install clang make libxcb vulkan-headers libglvnd libwayland wayland-protocols wayland-scanner

### Build

    make egl               # build/libEGL.so.1
    make protocols         # only regenerate the Wayland protocol files
    
### Usage

    LD_LIBRARY_PATH=$PWD/build ./your_app

### Environment variables

| Variable | Effect |
|---|---|
| `EGL_DEBUG=1` | Verbose logging (`MALI_DEBUG`, `GL_DEBUG` also work) |
| `SHIM_DMABUF=0` | Skip zero-copy, use the fallback WSI |
| `SHIM_BUFFERS=<n>` | Number of presented buffers (default 4, max 8) |
| `SHIM_MAILBOX=1` | Drop the frame when no buffer is free instead of waiting |
| `SHIM_SYNC=finish\|flush\|implicit\|native` | Sync before Present (default: EGL fence) |
| `SHIM_FORMAT=1\|5` | AHB format: 5 = BGRA, 1 = RGBA + swizzle |
| `SHIM_IMPORT=native\|dmabuf` | Restrict the EGLImage import method |
| `SHIM_AFBC=1` | Try an AFBC-compressed application texture |
| `SHIM_PRESENT_COPY=1` | Present with the `COPY` option |
| `SHIM_EVENT_THREAD=0` | No event thread (this also disables resize detection) |
| `SHIM_OPAQUE=1` | Force alpha to 1 on 32-bit windows |
| `SHIM_STATS=1` | Print frame statistics (with `EGL_DEBUG=1`) |
| `SHIM_GBM=1` | `eglGetDisplay` returns the GBM platform display |
| `SHIM_ES1=1` | Desktop GL requests create ES 1 contexts |
| `WSI_NO_UPGRADE=1` | Do not upgrade ES2 contexts to ES3 |
| `WSI_FPS`, `WSI_VSYNC`, `WSI_STATS` | Fallback WSI: frame cap, vsync gating, diagnostics |
| `SHIM_DRM_PRIMARY`, `SHIM_DRM_RENDER`, `SHIM_LIBDRM` | DRM node names for the device extensions |
| `SHIM_STALL_MS=<ms>` | Force VSYNC pacing once a swap or fence wait takes this long (default 250, `0` disables) |
| `SHIM_WAYLAND=1` | `eglGetDisplay` returns the Wayland platform display |
| `SHIM_WL_ALPHA=1` | Wayland surfaces use ARGB8888 instead of XRGB8888 |
| `SHIM_NO_CONFIGURE=1` | Do not send the synthetic ConfigureNotify (X11) |
| `EGL_API=GL\|GL_ES` | Restrict the client API exposed by configs and `eglQueryString` |
| `SHIM_LIBGL` | Full path of the `libGL` used for desktop GL emulation |
| `SHIM_WL_THREAD=1` | Wayland: read the socket on a background thread (default: the app thread reads it inside `eglSwapBuffers`) |

### How it works

- **Platforms:** `EGL_PLATFORM_X11_EXT`, `EGL_PLATFORM_XCB_EXT`,
  `EGL_PLATFORM_GBM_KHR`, `EGL_PLATFORM_WAYLAND_KHR`, surfaceless and device. The Mali driver is
  loaded lazily through SPHAL and every other call is forwarded to it.
- **Window surfaces:** the application renders into an offscreen FBO.
  On swap, a GPU pass flips Y (and swizzles if needed) into one of N
  `AHardwareBuffer`s, which are shared as DRI3 pixmaps and EGLImages.
  A worker thread waits for the fence and calls `xcb_present_pixmap`.
  `PresentIdleNotify`, read on a separate xcb connection, frees buffers.
- **Resize:** `ConfigureNotify` is handled on the event thread and the
  buffers are rebuilt on the next swap.
- **Fallback:** if zero-copy fails (or the context is not ES3), the
  legacy WSI in `wsi_egl_x11.h` renders into a back FBO and flips it.
- **Contexts:** ES2 contexts are upgraded to ES3 when possible.
  `eglBindAPI(EGL_OPENGL_API)` is mapped to OpenGL ES.
- **GBM:** surfaces created from `libgbm.so.1` (this project) are
  rendered into the GBM buffer pool directly.
- **Extensions:** adds or emulates `EGL_KHR_debug`, `EGL_EXT_sync_reuse`,
  `EGL_MESA_drm_image`, `EGL_MESA_image_dma_buf_export`, `EGL_EXT_device_*`,
  `EGL_KHR_swap_buffers_with_damage`, `EGL_EXT_swap_tearing_hint`,
  `EGL_WL_create_wayland_buffer_from_image` and `EGL_WL_bind_wayland_display`.
- **GLES shim:** `libGLESv2` detects `shim_x11_default_fbo` here and
  switches its own WSI off.
- **Swap pacing:** `eglSwapInterval` and `EGL_SWAP_TEARING_HINT_EXT`
  (set with `eglSurfaceAttrib` on the current surface) are handled
  separately. With interval 0, `DEFAULT` and `ASYNC` present with the
  Present `ASYNC` option; `VSYNC` presents without it and drops frames
  when no buffer is free (mailbox) instead of blocking. With interval
  >= 1 the hint is ignored. If a swap or fence wait takes longer than
  `SHIM_STALL_MS`, the surface switches to `VSYNC` behaviour until it
  is recreated. The hint is write-only: querying it returns
  `EGL_BAD_ATTRIBUTE`.
- **Wayland:** window surfaces (`wl_egl_window`) use the same offscreen FBO and
  GPU pass as X11. The AHardwareBuffers are shared as `wl_buffer`s through
  `zwp_linux_dmabuf_v1` and presented with `wl_surface.attach/commit`.
  `wl_buffer.release` frees slots and `wl_egl_window_resize` triggers a rebuild
  on the next swap. There is no legacy WSI fallback: if the dmabuf path fails,
  `eglMakeCurrent` fails.

### Limitations

- Pixmap surfaces are not supported.
- One X connection and display per process.
- Zero-copy requires GLES 3; GLES 1/2 contexts use the fallback WSI.
- Up to 64 contexts and 4 initialized displays.
- Swap tearing hint: stored but unused on GBM surfaces; the fallback WSI
  only toggles `ASYNC` (no mailbox).
- Stall fallback to `VSYNC` is per surface and is not reverted
  automatically.
- Wayland: zero-copy only, GLES 3 required, no fallback WSI.
- Wayland: needs `zwp_linux_dmabuf_v1` version >= 2 in the compositor and
  accepts only ARGB8888/XRGB8888.
- `eglCreateWaylandBufferFromImageWL` uses `create_immed`: a rejected format or
  modifier is a protocol error and closes the connection.
- AHardwareBuffers are exported with the INVALID modifier. AFBC buffers will not
  display correctly in the compositor.
- `AHardwareBuffer_to_ANativeWindowBuffer` called directly by the app bypasses
  the AHB mapping, so `eglCreateWaylandBufferFromImageWL` cannot export those images.
- `eglBindWaylandDisplayWL` must be called from the compositor's event-loop thread.

### Wayland extensions

- `eglCreateWaylandBufferFromImageWL` creates a client-side `wl_buffer` from an
  EGLImage on a Wayland display. It works for images created with
  `EGL_LINUX_DMA_BUF_EXT`, and for images created from an AHardwareBuffer
  obtained through `eglGetNativeClientBufferANDROID` (the shim keeps a
  dup of the dma-buf fd). Anything else returns `EGL_BAD_MATCH`.
- `EGL_WL_bind_wayland_display` (optional) exposes `wl_drm` on a compositor
  display. Only `create_prime_buffer` with one ARGB8888/XRGB8888 plane is
  supported.

## DRM

Path: `API/drm`

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

Path: `API/dri`

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
- `src/bcdec` – [bcdec](https://github.com/iOrange/bcdec) by iOrange, BC1-BC7
  texture decoder, used by the OpenGL shim for S3TC/RGTC/BPTC decoding.
  Dual-licensed: MIT or Public Domain/Unlicense (see `src/bcdec/LICENSE`).
- [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) – not bundled;
  linked from the system package by the OpenGL shim for SPIR-V → GLSL ES
  conversion. See its repository for the license.
