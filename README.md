# Mali Termux WSI

Window System Integration (XCB) for Mali GPUs on Termux (Termux:X11).

## Vulkan (`Xcb/Vulkan`)

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
    make clean

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

### Limitations

AArch64 only. One instance and one device per process.

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

### Next?

Adding on future supports for Wayland and Win32 (separated)

## OpenGL (`Xcb/GL`)

## GLFW (`Xcb/GL/GLFW`)

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
    make clean

### Usage

    LD_LIBRARY_PATH=$PWD/build ./your_app

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

### Third-party code

- `src/glfw` – GLFW headers, zlib/libpng license (see `src/glfw/LICENSE.md`).

## GLU (`Xcb/GL/GLU`)

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
    make clean

### Usage

    LD_LIBRARY_PATH=$PWD/build ./your_app

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

### Third-party code

- `src/libtess2` – libtess2, SGI Free Software License B (see
  `src/libtess2/LICENSE.txt`).

## GLUT (`Xcb/GL/GLUT`)

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
    make clean

### Usage

    LD_LIBRARY_PATH=$PWD/build ./your_app

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

### Work in progress

EGL and GL is coming after fixed some issues found
