#
# PS Vita specific build rules
#

# Toolchain
CROSS_COMPILE := arm-vita-eabi-
CC := $(CROSS_COMPILE)gcc
CXX := $(CROSS_COMPILE)g++
AS := $(CROSS_COMPILE)as
LD := $(CROSS_COMPILE)ld
AR := $(CROSS_COMPILE)ar
STRIP := $(CROSS_COMPILE)strip

# Vita SDK includes
VITASDK_INCLUDES := -I/home/zyb/.local/vitasdk/arm-vita-eabi/include
VITASDK_INCLUDES += -I/home/zyb/.local/vitasdk/arm-vita-eabi/include/c++

# Vita SDK libraries
VITASDK_LIBS := -L/home/zyb/.local/vitasdk/arm-vita-eabi/lib
VITASDK_LIBS += -lvita2d -lSceDisplay_stub -lSceCtrl_stub -lSceGxm_stub
VITASDK_LIBS += -lSceAudioOut_stub -lSceKernel_stub -lSceSysmodule_stub
VITASDK_LIBS += -lSceCommonDialog_stub -lSceAppMgr_stub -lSceIo_stub
VITASDK_LIBS += -lScePgf_stub -lSceJpeg_stub -lScePng_stub
VITASDK_LIBS += -lSceNet_stub -lSceNetCtl_stub -lSceHttp_stub -lSceSsl_stub

# Compiler flags
CFLAGS += -march=armv7-a -mtune=cortex-a9 -mfloat-abi=hard -mfpu=vfpv3
CFLAGS += -DARM -DVITA -D__PSP2__ -D__VITA__
CFLAGS += -DSUPPORTS_MEMORY_MAPPED_FILES=0
CFLAGS += -DSUPPORTS_ADJUSTABLE_MEMORY_CHUNK=0
CFLAGS += -DSUPPORTS_TIMER_THREAD=0
CFLAGS += -DUSE_VM_EXCEPTIONS=0
CFLAGS += -DALWAYS_LSW_FIRST_FOR_DOUBLE=1
CFLAGS += $(VITASDK_INCLUDES)

# Linker flags
LDFLAGS += 
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -Wl,-z,max-page-size=0x1000
LDFLAGS += $(VITASDK_LIBS)
LDFLAGS += -lpthread -lm

# Source files for Vita OS layer
VITA_OS_SRCS := $(WorkSpace)/src/vm/os/vita/OS_vita.cpp
VITA_OS_SRCS += $(WorkSpace)/src/vm/os/vita/OsMemory_vita.cpp
VITA_OS_SRCS += $(WorkSpace)/src/vm/os/vita/OsMisc_vita.cpp
VITA_OS_SRCS += $(WorkSpace)/src/vm/os/vita/OsFile_vita.cpp
VITA_OS_SRCS += $(WorkSpace)/src/vm/os/vita/OsSocket_vita.cpp
VITA_OS_SRCS += $(WorkSpace)/src/vm/os/vita/JVM_vita.cpp
VITA_OS_SRCS += $(WorkSpace)/src/vm/os/vita/Main_vita.cpp

# Override OS layer source selection
OS_SRCS := $(VITA_OS_SRCS)

# Disable thumb mode
ENABLE_THUMB_VM := false
ENABLE_THUMB_COMPILER := false

# Enable ARM VFP
ENABLE_FLOAT := true
ENABLE_SOFT_FLOAT := false
ENABLE_ARM_VFP := true

# Disable features not supported on Vita
ENABLE_TIMER_THREAD := false
