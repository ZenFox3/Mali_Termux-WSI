CC      ?= clang
CFLAGS  ?= -O2 -Wall -fPIC
BUILD   := build

VK_DIR  := Xcb/Vulkan
VK_SRC  := $(VK_DIR)/Scripts/libvulkan.c
VK_DEPS := $(wildcard $(VK_DIR)/Include/*)
VK_OUT  := $(BUILD)/libvulkan.so.1
VK_LIBS := -lxcb -lxcb-dri3 -lxcb-present -ldl -lpthread

# make XLIB=1  -> suport Xlib
ifeq ($(XLIB),1)
VK_FLAGS += -DWSI_XLIB
VK_LIBS  += -lX11 -lX11-xcb
endif

# make GLIBC=1 -> guest glibc (libhybris)
ifeq ($(GLIBC),1)
VK_FLAGS += -DSHIM_GLIBC
endif

.PHONY: all vulkan clean

all: vulkan
vulkan: $(VK_OUT)

$(VK_OUT): $(VK_SRC) $(VK_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(VK_FLAGS) -shared -Wl,-soname,libvulkan.so.1 $< -o $@ $(VK_LIBS)
	ln -sf libvulkan.so.1 $(BUILD)/libvulkan.so

clean:
	rm -rf $(BUILD)