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

- [ ] GLFW
- [ ] GLU
- [ ] GLUT 