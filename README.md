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

## OpenGL (`Xcb/GL`)

- [ ] GLFW
- [ ] GLU
- [ ] GLUT 