CC      ?= clang
CFLAGS  ?= -O2 -Wall -fPIC
BUILD   := build

VK_DIR  := Xcb/Vulkan
VK_SRC  := $(VK_DIR)/Scripts/libvulkan.c
VK_DEPS := $(wildcard $(VK_DIR)/Include/*)
VK_OUT  := $(BUILD)/libvulkan.so.1
VK_LIBS := -lxcb -lxcb-dri3 -lxcb-present -ldl -lpthread

GLU_DIR    := Xcb/GL/GLU
TESS_DIR   := src/libtess2
GLU_SRC    := $(wildcard $(GLU_DIR)/Scripts/*.c)
TESS_SRC   := $(wildcard $(TESS_DIR)/Source/*.c)
GLU_OBJ    := $(patsubst %.c,$(BUILD)/glu/%.o,$(GLU_SRC) $(TESS_SRC))
GLU_OUT    := $(BUILD)/libGLU.so.1
GLU_CFLAGS := -I$(TESS_DIR)/Include -I$(TESS_DIR)/Source

GLUT_DIR    := Xcb/GL/GLUT
GLUT_SRC    := $(wildcard $(GLUT_DIR)/Scripts/*.c)
GLUT_OBJ    := $(patsubst %.c,$(BUILD)/glut/%.o,$(GLUT_SRC))
GLUT_OUT    := $(BUILD)/libglut.so.3
GLUT_CFLAGS := -I$(GLUT_DIR)/Include

GLFW_DIR    := Xcb/GL/GLFW
GLFW_SRC    := $(wildcard $(GLFW_DIR)/Scripts/*.c)
GLFW_OUT    := $(BUILD)/libglfw.so.3
GLFW_CFLAGS := -Isrc/glfw/include
GLFW_LIBS   := -lxcb -lxcb-randr -lxcb-xkb -lxkbcommon -lxkbcommon-x11 -lEGL -ldl -lm -lpthread

# make XLIB=1  -> suport Xlib
ifeq ($(XLIB),1)
VK_FLAGS += -DWSI_XLIB
VK_LIBS  += -lX11 -lX11-xcb
endif

# make GLIBC=1 -> guest glibc (libhybris)
ifeq ($(GLIBC),1)
VK_FLAGS += -DSHIM_GLIBC
endif

.PHONY: all vulkan glu clean
all: vulkan glu
glu: $(GLU_OUT)
glut: $(GLUT_OUT)
glfw: $(GLFW_OUT)

all: vulkan
vulkan: $(VK_OUT)

$(VK_OUT): $(VK_SRC) $(VK_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(VK_FLAGS) -shared -Wl,-soname,libvulkan.so.1 $< -o $@ $(VK_LIBS)
	ln -sf libvulkan.so.1 $(BUILD)/libvulkan.so

$(BUILD)/glu/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(GLU_CFLAGS) -c $< -o $@

$(GLU_OUT): $(GLU_OBJ)
	$(CC) -shared -Wl,-soname,libGLU.so.1 $^ -o $@ -lGL -lm
	ln -sf libGLU.so.1 $(BUILD)/libGLU.so

$(BUILD)/glut/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(GLUT_CFLAGS) -c $< -o $@

$(GLUT_OUT): $(GLUT_OBJ)
	$(CC) -shared -Wl,-soname,libglut.so.3 $^ -o $@ -lGL -lX11 -lm -ldl
	ln -sf libglut.so.3 $(BUILD)/libglut.so

$(GLFW_OUT): $(GLFW_SRC) $(wildcard src/glfw/include/GLFW/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(GLFW_CFLAGS) -shared -Wl,-soname,libglfw.so.3 $(GLFW_SRC) -o $@ $(GLFW_LIBS)
	ln -sf libglfw.so.3 $(BUILD)/libglfw.so
  
clean:
	rm -rf $(BUILD)