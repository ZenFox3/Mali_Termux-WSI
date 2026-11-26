# Android API 
TARGET  ?= aarch64-linux-android29

ifeq ($(origin CC),default)
CC := clang
endif
ifeq ($(origin CXX),default)
CXX := clang++
endif
CFLAGS   ?= -O2 -Wall -fPIC
CFLAGS   += --target=$(TARGET)
CXXFLAGS ?= -O2 -Wall -fPIC
CXXFLAGS += --target=$(TARGET)
BUILD    := build
LDFLAGS  += --target=$(TARGET) -L$(BUILD)

# ---- Vulkan ----
VK_DIR  := API/Vulkan
VK_SRC  := $(VK_DIR)/Scripts/libvulkan.c
VK_DEPS := $(wildcard $(VK_DIR)/Include/*)
VK_OUT  := $(BUILD)/libvulkan.so.1
VK_WL_SRC := $(BUILD)/wayland/linux-dmabuf-protocol.c
VK_WL_HDR := $(BUILD)/wayland/linux-dmabuf-client.h
VK_CFLAGS := -I$(BUILD)/wayland
VK_LIBS := -lxcb -lxcb-dri3 -lxcb-present -ldl -lpthread -lxcb-shm -lwayland-client

# make XLIB=1  -> Xlib support
ifeq ($(XLIB),1)
VK_FLAGS += -DWSI_XLIB
VK_LIBS  += -lX11 -lX11-xcb
endif

# make GLIBC=1 TARGET=aarch64-linux-gnu -> glibc guest (libhybris)
ifeq ($(GLIBC),1)
VK_FLAGS += -DSHIM_GLIBC
endif

# ---- GLU ----
GLU_DIR    := API/GL/GLU
TESS_DIR   := src/libtess2
GLU_SRC    := $(wildcard $(GLU_DIR)/Scripts/*.c)
TESS_SRC   := $(wildcard $(TESS_DIR)/Source/*.c)
GLU_OBJ    := $(patsubst %.c,$(BUILD)/glu/%.o,$(GLU_SRC) $(TESS_SRC))
GLU_OUT    := $(BUILD)/libGLU.so.1
GLU_CFLAGS := -I$(TESS_DIR)/Include -I$(TESS_DIR)/Source

# ---- GLUT ----
GLUT_DIR    := API/GL/GLUT
GLUT_SRC    := $(wildcard $(GLUT_DIR)/Scripts/*.c)
GLUT_OBJ    := $(patsubst %.c,$(BUILD)/glut/%.o,$(GLUT_SRC))
GLUT_OUT    := $(BUILD)/libglut.so.3
GLUT_CFLAGS := -I$(GLUT_DIR)/Include

# ---- GLFW ----
GLFW_DIR    := API/GL/GLFW
GLFW_SRC    := $(wildcard $(GLFW_DIR)/Scripts/*.c)
GLFW_OUT    := $(BUILD)/libglfw.so.3
GLFW_CFLAGS := -Isrc/glfw/include
GLFW_LIBS   := -lxcb -lxcb-randr -lxcb-xkb -lxkbcommon -lxkbcommon-x11 -lEGL -ldl -lm -lpthread

# ---- GBM ----
GBM_DIR  := API/EGL/GBM
GBM_SRC  := $(wildcard $(GBM_DIR)/Scripts/*.c)
GBM_OUT  := $(BUILD)/libgbm.so.1
GBM_DEFS := 

# ---- DRM ----
PREFIX     ?= /data/data/com.termux/files/usr
DRM_DIR    := API/drm
DRM_SRC    := $(wildcard $(DRM_DIR)/Scripts/*.c)
DRM_OUT    := $(BUILD)/libdrm.so.2
DRM_CFLAGS := -D_GNU_SOURCE -I$(PREFIX)/include -I$(PREFIX)/include/libdrm

# ---- DRI (VA-API) ----
DRI_DIR    := API/dri
DRI_SRC    := $(wildcard $(DRI_DIR)/Scripts/*.c)
DRI_DEPS   := $(wildcard $(DRI_DIR)/Include/*)
DRI_OUT    := $(BUILD)/ahb_drv_video.so
DRI_CFLAGS := -D_GNU_SOURCE -I$(DRI_DIR)/Include -I$(PREFIX)/include

# ---- GLES ----
GLES_DIR    := API/GLES
GLES_SRC    := $(GLES_DIR)/Scripts/libGLESv2_shim.c
GLES_DEPS   := $(wildcard $(GLES_DIR)/Include/*) $(wildcard API/EGL/Include/*)
GLES_OUT    := $(BUILD)/libGLESv2.so.2
GLES_CFLAGS := -I$(GLES_DIR)/Include -IAPI/EGL/Include
GLES_LIBS   := -lxcb -lxcb-dri3 -lxcb-present -ldl -lpthread

# ---- EGL ----
EGL_DIR    := API/EGL
WL_GEN     := $(BUILD)/wayland
SCANNER    ?= wayland-scanner
WP := $(PREFIX)/share/wayland-protocols

# name=xml  ->  $(WL_GEN)/<name>-client.h + <name>-protocol.c
WL_CLIENT_PROTOS := \
  linux-dmabuf=$(WP)/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
#  tearing-control=$(WP)/staging/tearing-control/tearing-control-v1.xml

WL_CLIENT_NAMES := $(foreach p,$(WL_CLIENT_PROTOS),$(word 1,$(subst =, ,$(p))))

EGL_GEN_C := $(foreach n,$(WL_CLIENT_NAMES),$(WL_GEN)/$(n)-protocol.c) \
             $(WL_GEN)/wayland-drm-protocol.c
EGL_GEN_H := $(foreach n,$(WL_CLIENT_NAMES),$(WL_GEN)/$(n)-client.h) \
             $(WL_GEN)/wayland-drm-server-protocol.h
DRM_XML    ?= src/mesa/src/egl/wayland/wayland-drm/wayland-drm.xml
EGL_SRC    := $(EGL_DIR)/Scripts/egl_x11_shim.c \
              $(EGL_DIR)/Scripts/wsi_wayland.c \
              $(EGL_DIR)/Scripts/wl_server.c
EGL_OBJ    := $(patsubst $(EGL_DIR)/Scripts/%.c,$(BUILD)/egl/%.o,$(EGL_SRC)) \
              $(patsubst $(WL_GEN)/%.c,$(BUILD)/egl/%.o,$(EGL_GEN_C))
EGL_DEPS   := $(wildcard $(EGL_DIR)/Include/*)
EGL_OUT    := $(BUILD)/libEGL.so.1
EGL_CFLAGS := -I$(EGL_DIR)/Include -I$(WL_GEN) -I$(PREFIX)/include
EGL_LIBS   := -lxcb -lxcb-dri3 -lxcb-present -lwayland-client -lwayland-server -ldl -lpthread

# ---- SPIRV-Cross (GL_ARB_gl_spirv, API/GL/Scripts/gl_spirv.c) ----
# Static archives from the system package; the order does not matter (start-group)
# and --exclude-libs keeps the SPIRV-Cross symbols out of libGL.so.1's exports.
# Dynamic alternative:  make gl SPV_LIBS=-lspirv-cross-c-shared
SPV_LIBS ?= -Wl,--start-group \
            -lspirv-cross-c -lspirv-cross-glsl -lspirv-cross-hlsl -lspirv-cross-msl \
            -lspirv-cross-cpp -lspirv-cross-reflect -lspirv-cross-core \
            -Wl,--end-group -Wl,--exclude-libs,ALL

# ---- GL ----
GL_DIR        := API/GL
GL_SRC        := $(GL_DIR)/Scripts/glx_shim.c
GL_SHIM_OBJ   := $(BUILD)/gl/glx_shim.o
GL_BRIDGE_SRC := $(GL_DIR)/Scripts/gl_dt_bridge.cpp
GL_BRIDGE_OBJ := $(BUILD)/gl/gl_dt_bridge.o
GL_OUT        := $(BUILD)/libGL.so.1
GL_DEPS       := $(wildcard $(GL_DIR)/Scripts/*) $(wildcard $(GL_DIR)/Include/*) \
                 $(GLES_DIR)/Scripts/libGLESv2_shim.c $(GLES_DEPS) src/bcdec/bcdec.h
GL_CFLAGS     := -I$(GL_DIR)/Include -I$(GL_DIR)/Scripts \
                 -I$(GLES_DIR)/Scripts -I$(GLES_DIR)/Include -IAPI/EGL/Include \
                 -Isrc/bcdec -I$(PREFIX)/include \
                 -Wno-unused-function -Wno-misleading-indentation
GL_CXXFLAGS   := -std=c++17 -fvisibility=hidden -I$(GL_DIR)/Include \
                 -Wno-unused-function -Wno-unused-variable
# SPV_LIBS comes before -lc++: the SPIRV-Cross archives need libc++ symbols
GL_LIBS       := -lxcb -lxcb-dri3 -lxcb-present -lEGL $(SPV_LIBS) -ldl -lpthread -lm -lc++

.PHONY: all vulkan glu glut glfw gbm drm dri gles egl gl protocols clean

all: vulkan gbm drm dri gles egl gl glu glut glfw protocols

vulkan: $(VK_OUT)
glu:    $(GLU_OUT)
glut:   $(GLUT_OUT)
glfw:   $(GLFW_OUT)
gbm:    $(GBM_OUT)
drm:    $(DRM_OUT)
dri:    $(DRI_OUT)
gles:   $(GLES_OUT)
egl:    $(EGL_OUT)
gl:     $(GL_OUT)
protocols: $(EGL_GEN_H) $(EGL_GEN_C)

$(VK_OUT): $(VK_SRC) $(VK_DEPS) $(VK_WL_SRC) $(VK_WL_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(VK_CFLAGS) $(VK_FLAGS) -shared -Wl,-soname,libvulkan.so.1 \
		$(VK_SRC) $(VK_WL_SRC) -o $@ $(LDFLAGS) $(VK_LIBS)
	cp $(VK_OUT) $(BUILD)/libvulkan.so

$(BUILD)/glu/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(GLU_CFLAGS) -MMD -MP -c $< -o $@

$(GLU_OUT): $(GLU_OBJ) | $(GL_OUT)
	$(CC) $(LDFLAGS) -shared -Wl,-soname,libGLU.so.1 $^ -o $@ -lGL -lm
	cp $(GLU_OUT) $(BUILD)/libGLU.so

$(BUILD)/glut/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(GLUT_CFLAGS) -MMD -MP -c $< -o $@

$(GLUT_OUT): $(GLUT_OBJ) | $(GL_OUT)
	$(CC) $(LDFLAGS) -shared -Wl,-soname,libglut.so.3 $^ -o $@ -lGL -lX11 -lm -ldl
	cp $(GLUT_OUT) $(BUILD)/libglut.so

$(GLFW_OUT): $(GLFW_SRC) $(wildcard src/glfw/include/GLFW/*.h) | $(EGL_OUT)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(GLFW_CFLAGS) -shared -Wl,-soname,libglfw.so.3 $(GLFW_SRC) -o $@ $(LDFLAGS) $(GLFW_LIBS)
	cp $(GLFW_OUT) $(BUILD)/libglfw.so

$(GBM_OUT): $(GBM_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(GBM_DEFS) -shared -Wl,-soname,libgbm.so.1 $^ -o $@ $(LDFLAGS) -ldl -lnativewindow
	cp $(GBM_OUT) $(BUILD)/libgbm.so

$(DRM_OUT): $(DRM_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(DRM_CFLAGS) -shared -Wl,-soname,libdrm.so.2 $^ -o $@ $(LDFLAGS) -ldl -lnativewindow
	cp $(DRM_OUT) $(BUILD)/libdrm.so

$(DRI_OUT): $(DRI_SRC) $(DRI_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(DRI_CFLAGS) -shared $(DRI_SRC) -o $@ $(LDFLAGS) -ldl -lpthread -lnativewindow

$(GLES_OUT): $(GLES_SRC) $(GLES_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(GLES_CFLAGS) -shared -Wl,-soname,libGLESv2.so.2 $< -o $@ $(LDFLAGS) $(GLES_LIBS)
	cp $(GLES_OUT) $(BUILD)/libGLESv2.so
	cp $(GLES_OUT) $(BUILD)/libGLESv3.so
	cp $(GLES_OUT) $(BUILD)/libGLESv1_CM.so.1
	cp $(GLES_OUT) $(BUILD)/libGLESv1_CM.so

# ---- wayland-scanner: client protocols ----
define WL_CLIENT_RULE
$$(WL_GEN)/$(1)-client.h: $(2)
	@mkdir -p $$(@D)
	$$(SCANNER) client-header $$< $$@

$$(WL_GEN)/$(1)-protocol.c: $(2)
	@mkdir -p $$(@D)
	$$(SCANNER) private-code $$< $$@
endef

$(foreach p,$(WL_CLIENT_PROTOS),\
  $(eval $(call WL_CLIENT_RULE,$(word 1,$(subst =, ,$(p))),$(word 2,$(subst =, ,$(p))))))

# ---- wayland-scanner: wl_drm (server) ----
$(WL_GEN)/wayland-drm-server-protocol.h: $(DRM_XML)
	@mkdir -p $(@D)
	$(SCANNER) server-header $< $@

$(WL_GEN)/wayland-drm-protocol.c: $(DRM_XML)
	@mkdir -p $(@D)
	$(SCANNER) private-code $< $@

$(BUILD)/egl/%.o: $(EGL_DIR)/Scripts/%.c $(EGL_DEPS) | $(EGL_GEN_H)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(EGL_CFLAGS) -c $< -o $@

$(BUILD)/egl/%.o: $(WL_GEN)/%.c | $(EGL_GEN_H)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(EGL_CFLAGS) -c $< -o $@

$(EGL_OUT): $(EGL_OBJ)
	@mkdir -p $(BUILD)
	$(CC) $(LDFLAGS) -shared -Wl,-soname,libEGL.so.1 $^ -o $@ $(EGL_LIBS)
	cp $(EGL_OUT) $(BUILD)/libEGL.so

$(GL_SHIM_OBJ): $(GL_SRC) $(GL_DEPS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(GL_CFLAGS) -c $< -o $@

$(GL_BRIDGE_OBJ): $(GL_BRIDGE_SRC) $(GL_DIR)/Include/gl_desktop_texture.h
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(GL_CXXFLAGS) -c $< -o $@

$(GL_OUT): $(GL_SHIM_OBJ) $(GL_BRIDGE_OBJ) | $(EGL_OUT)
	@mkdir -p $(BUILD)
	$(CXX) $(LDFLAGS) -shared -Wl,-soname,libGL.so.1 $^ -o $@ $(GL_LIBS)
	cp $(GL_OUT) $(BUILD)/libGL.so
	cp $(GL_OUT) $(BUILD)/libGLX.so
	cp $(GL_OUT) $(BUILD)/libGLX.so.0

clean:
	rm -rf $(BUILD)

-include $(GLU_OBJ:.o=.d) $(GLUT_OBJ:.o=.d)
