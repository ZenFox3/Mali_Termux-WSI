# Android API level. 31 = Android 12+ (portable), 34 = Android 14
TARGET  ?= aarch64-linux-android31

ifeq ($(origin CC),default)
CC := clang
endif
CFLAGS  ?= -O2 -Wall -fPIC
CFLAGS  += --target=$(TARGET)
LDFLAGS += --target=$(TARGET)
BUILD   := build

# ---- Vulkan ----
VK_DIR  := Xcb/Vulkan
VK_SRC  := $(VK_DIR)/Scripts/libvulkan.c
VK_DEPS := $(wildcard $(VK_DIR)/Include/*)
VK_OUT  := $(BUILD)/libvulkan.so.1
VK_LIBS := -lxcb -lxcb-dri3 -lxcb-present -ldl -lpthread

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
GLU_DIR    := Xcb/GL/GLU
TESS_DIR   := src/libtess2
GLU_SRC    := $(wildcard $(GLU_DIR)/Scripts/*.c)
TESS_SRC   := $(wildcard $(TESS_DIR)/Source/*.c)
GLU_OBJ    := $(patsubst %.c,$(BUILD)/glu/%.o,$(GLU_SRC) $(TESS_SRC))
GLU_OUT    := $(BUILD)/libGLU.so.1
GLU_CFLAGS := -I$(TESS_DIR)/Include -I$(TESS_DIR)/Source

# ---- GLUT ----
GLUT_DIR    := Xcb/GL/GLUT
GLUT_SRC    := $(wildcard $(GLUT_DIR)/Scripts/*.c)
GLUT_OBJ    := $(patsubst %.c,$(BUILD)/glut/%.o,$(GLUT_SRC))
GLUT_OUT    := $(BUILD)/libglut.so.3
GLUT_CFLAGS := -I$(GLUT_DIR)/Include

# ---- GLFW ----
GLFW_DIR    := Xcb/GL/GLFW
GLFW_SRC    := $(wildcard $(GLFW_DIR)/Scripts/*.c)
GLFW_OUT    := $(BUILD)/libglfw.so.3
GLFW_CFLAGS := -Isrc/glfw/include
GLFW_LIBS   := -lxcb -lxcb-randr -lxcb-xkb -lxkbcommon -lxkbcommon-x11 -lEGL -ldl -lm -lpthread

# ---- GBM ----
GBM_DIR  := Xcb/EGL/GBM
GBM_SRC  := $(wildcard $(GBM_DIR)/Scripts/*.c)
GBM_OUT  := $(BUILD)/libgbm.so.1
GBM_DEFS := -D_GNU_SOURCE

# ---- DRM ----
PREFIX     ?= /data/data/com.termux/files/usr
DRM_DIR    := Xcb/drm
DRM_SRC    := $(wildcard $(DRM_DIR)/Scripts/*.c)
DRM_OUT    := $(BUILD)/libdrm.so.2
DRM_CFLAGS := -D_GNU_SOURCE -I$(PREFIX)/include -I$(PREFIX)/include/libdrm

# ---- DRI (VA-API) ----
DRI_DIR    := Xcb/dri
DRI_SRC    := $(wildcard $(DRI_DIR)/Scripts/*.c)
DRI_DEPS   := $(wildcard $(DRI_DIR)/Include/*)
DRI_OUT    := $(BUILD)/ahb_drv_video.so
DRI_CFLAGS := -D_GNU_SOURCE -I$(DRI_DIR)/Include -I$(PREFIX)/include

# ---- GLES ----
GLES_DIR    := Xcb/GLES
GLES_SRC    := $(GLES_DIR)/Scripts/libGLESv2_shim.c
GLES_DEPS   := $(wildcard $(GLES_DIR)/Include/*) $(wildcard Xcb/EGL/Include/*)
GLES_OUT    := $(BUILD)/libGLESv2.so.2
GLES_CFLAGS := -I$(GLES_DIR)/Include -IXcb/EGL/Include
GLES_LIBS   := -lxcb -lxcb-dri3 -lxcb-present -ldl -lpthread

# ---- EGL ----
EGL_DIR    := Xcb/EGL
EGL_SRC    := $(EGL_DIR)/Scripts/egl_x11_shim.c
EGL_DEPS   := $(wildcard $(EGL_DIR)/Include/*)
EGL_OUT    := $(BUILD)/libEGL.so.1
EGL_CFLAGS := -I$(EGL_DIR)/Include
EGL_LIBS   := -lxcb -lxcb-dri3 -lxcb-present -ldl -lpthread

# ---- GL ----
GL_DIR    := Xcb/GL
GL_SRC    := $(GL_DIR)/Scripts/glx_shim.c
GL_DEPS   := $(wildcard $(GL_DIR)/Scripts/*) $(wildcard $(GL_DIR)/Include/*) \
             $(GLES_DIR)/Scripts/libGLESv2_shim.c $(GLES_DEPS) src/bcdec/bcdec.h
GL_OUT    := $(BUILD)/libGL.so.1
GL_CFLAGS := -D_GNU_SOURCE -I$(GL_DIR)/Include -I$(GL_DIR)/Scripts \
             -I$(GLES_DIR)/Scripts -I$(GLES_DIR)/Include -IXcb/EGL/Include \
             -Isrc/bcdec -Wno-unused-function -Wno-misleading-indentation
GL_LIBS   := -lxcb -lxcb-dri3 -lxcb-present -lEGL -ldl -lpthread -lm

.PHONY: all vulkan glu glut glfw gbm drm dri gles egl gl clean

all: vulkan glu glut glfw gbm drm dri gles egl gl

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


$(VK_OUT): $(VK_SRC) $(VK_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(VK_FLAGS) -shared -Wl,-soname,libvulkan.so.1 $< -o $@ $(LDFLAGS) $(VK_LIBS)
	ln -sf libvulkan.so.1 $(BUILD)/libvulkan.so

$(BUILD)/glu/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(GLU_CFLAGS) -MMD -MP -c $< -o $@

$(GLU_OUT): $(GLU_OBJ)
	$(CC) $(LDFLAGS) -shared -Wl,-soname,libGLU.so.1 $^ -o $@ -lGL -lm
	ln -sf libGLU.so.1 $(BUILD)/libGLU.so

$(BUILD)/glut/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(GLUT_CFLAGS) -MMD -MP -c $< -o $@

$(GLUT_OUT): $(GLUT_OBJ)
	$(CC) $(LDFLAGS) -shared -Wl,-soname,libglut.so.3 $^ -o $@ -lGL -lX11 -lm -ldl
	ln -sf libglut.so.3 $(BUILD)/libglut.so

$(GLFW_OUT): $(GLFW_SRC) $(wildcard src/glfw/include/GLFW/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(GLFW_CFLAGS) -shared -Wl,-soname,libglfw.so.3 $(GLFW_SRC) -o $@ $(LDFLAGS) $(GLFW_LIBS)
	ln -sf libglfw.so.3 $(BUILD)/libglfw.so

$(GBM_OUT): $(GBM_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(GBM_DEFS) -shared -Wl,-soname,libgbm.so.1 $^ -o $@ $(LDFLAGS) -ldl -lnativewindow
	ln -sf libgbm.so.1 $(BUILD)/libgbm.so

$(DRM_OUT): $(DRM_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(DRM_CFLAGS) -shared -Wl,-soname,libdrm.so.2 $^ -o $@ $(LDFLAGS) -ldl -lnativewindow

$(DRI_OUT): $(DRI_SRC) $(DRI_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(DRI_CFLAGS) -shared $(DRI_SRC) -o $@ $(LDFLAGS) -ldl -lpthread -lnativewindow

$(GLES_OUT): $(GLES_SRC) $(GLES_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(GLES_CFLAGS) -shared -Wl,-soname,libGLESv2.so.2 $< -o $@ $(LDFLAGS) $(GLES_LIBS)
	ln -sf libGLESv2.so.2 $(BUILD)/libGLESv2.so
	ln -sf libGLESv2.so.2 $(BUILD)/libGLESv3.so
	ln -sf libGLESv2.so.2 $(BUILD)/libGLESv1_CM.so.1
	ln -sf libGLESv2.so.2 $(BUILD)/libGLESv1_CM.so

$(EGL_OUT): $(EGL_SRC) $(EGL_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(EGL_CFLAGS) -shared -Wl,-soname,libEGL.so.1 $< -o $@ $(LDFLAGS) $(EGL_LIBS)
	ln -sf libEGL.so.1 $(BUILD)/libEGL.so

$(GL_OUT): $(GL_SRC) $(GL_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(GL_CFLAGS) -shared -Wl,-soname,libGL.so.1 $< -o $@ $(LDFLAGS) $(GL_LIBS)
	ln -sf libGL.so.1 $(BUILD)/libGL.so
  
clean:
	rm -rf $(BUILD)

-include $(GLU_OBJ:.o=.d) $(GLUT_OBJ:.o=.d)