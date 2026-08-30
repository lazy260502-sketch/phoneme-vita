# Phase 2: phoneme-midp 移植计划

## 概述
本文档详细描述将 phoneme-midp 移植到 PS Vita 的完整计划，包括架构分析、组件依赖关系、移植步骤和实现细节。

---

## 1. phoneme-midp 架构分析

### 1.1 目录结构
```
phoneme-midp/
├── src/
│   ├── ams/                          # 应用管理系统 (Application Management System)
│   │   ├── ams_api/                  # MIDP API 实现
│   │   │   └── reference/classes/javax/microedition/midlet/
│   │   │       ├── MIDlet.java              # MIDlet 基类
│   │   │   └── MIDletStateChangeException.java
│   │   │
│   │   ├── ams_base/                # AMS 基础实现
│   │   │   └── reference/classes/com/sun/midp/main/
│   │   │       └── AbstractMIDletSuiteLoader.java
│   │   │
│   │   ├── nams/                     # Native AMS
│   │   │   └── reference/native/
│   │   │       ├── midpNativeAppManagerPeer.c
│   │   │   └── midpNativeDisplayControllerPeer.c
│   │   │
│   │   └── mvm/                      # MIDP VM 集成
│   │
│   ├── core/                         # 核心服务
│   │   ├── memory/                  # 内存管理
│   │   ├── storage/                 # 存储管理
│   │   ├── log/                     # 日志系统
│   │   └── timer_queue/             # 定时器队列
│   │
│   ├── highlevelui/                 # 高级 UI
│   │   └── lcdui/                   # LCD UI
│   │       ├── reference/classes/javax/microedition/lcdui/
│   │       │   ├── Canvas.java              # 画布
│   │       │   ├── Display.java             # 显示管理
│   │       │   ├── Displayable.java          # 可显示对象
│   │       │   ├── Form.java                # 表单
│   │       │   ├── Graphics.java            # 图形绘制
│   │       │   ├── Image.java               # 图像
│   │       │   ├── Screen.java              # 屏幕
│   │       │   └── game/                    # 游戏相关
│   │       │       ├── GameCanvas.java      # 游戏画布
│   │       │       ├── Layer.java           # 图层
│   │       │       ├── LayerManager.java    # 图层管理器
│   │       │       ├── Sprite.java          # 精灵
│   │       │       └── TiledLayer.java      # 平铺图层
│   │       │
│   │       └── reference/native/
│   │           ├── lcdui_display.c          # 显示本地接口
│   │           ├── lcdui_game.c             # 游戏本地接口
│   │           ├── lcdui_input.c            # 输入本地接口
│   │           └── lcdui_audio.c            # 音频本地接口
│   │
│   └── lowlevelui/                  # 低级 UI
│       ├── graphics/                # 图形
│       │   ├── gx_platform/         # 平台图形
│       │   │   └── native/
│       │   │       ├── gxp_graphics.c      # 平台图形实现
│       │   │       ├── gxp_font.c           # 字体
│       │   │       └── gxp_image.c          # 图像
│       │   │
│       │   └── gx_putpixel/          # 像素级图形
│       │       └── native/
│       │           ├── gxj_graphics.c       # 基础图形操作
│       │           ├── gxj_font_bitmap.c   # 位图字体
│       │           ├── gxj_image.c          # 图像处理
│       │           └── gxj_putpixel.c       # 像素绘制
│       │
│       └── image/                   # 图像处理
│           ├── image_api/           # 图像 API
│           └── image_decode/        # 图像解码
│
└── build/
    └── linux_fb_gcc/                # Linux Framebuffer 构建配置
        ├── Platform.gmk
        ├── Options.gmk
        ├── config.gmk
        └── GNUmakefile
```

### 1.2 组件依赖关系

```
┌─────────────────────────────────────────────────────────────┐
│                    phoneme-midp 组件依赖图                        │
├─────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌─────────────┐     ┌─────────────┐     ┌─────────────┐    │
│  │   MIDlet    │────▶│    AMS      │────▶│   CLDC VM   │    │
│  │   (API)     │     │   (Core)    │     │  (phoneme-   │    │
│  └─────────────┘     └─────────────┘     │   cldc) ✅   │    │
│                                          └──────────┬────┘    │
│                                             │            │    │
│  ┌─────────────┐     ┌─────────────┐     ┌─────▼─────┐    │
│  │   LCUI      │────▶│  HighLevel  │     │  Native   │    │
│  │   (API)     │     │   UI Core   │────▶│  AMS Peer │    │
│  └─────────────┘     └─────────────┘     └────────────┘    │
│           │                       │                          │    │
│           ▼                       ▼                          │    │
│  ┌─────────────┐     ┌─────────────┐                    │    │
│  │   Graphics  │◀────│   LowLevel  │◀───────────────────┘    │
│  │   (Java)    │     │    UI       │                         │    │
│  └─────────────┘     └─────────────┘                         │    │
│           │                       │                              │    │
│           ▼                       ▼                              │    │
│  ┌─────────────────────────────────────────────────────┐    │    │
│  │              Platform Graphics Port                   │    │    │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐    │    │    │
│  │  │  gx_platform │  │ gx_putpixel  │  │   image     │    │    │    │
│  │  │  (SceGxm)   │  │  (Software)  │  │  (Decode)    │    │    │    │
│  │  └─────────────┘  └─────────────┘  └─────────────┘    │    │    │
│  └─────────────────────────────────────────────────────┘    │    │
│                                                                  │
└─────────────────────────────────────────────────────────────┘
```

### 1.3 关键接口分析

#### AMS (Application Management System)
- **作用**: 管理 MIDlet 生命周期
- **核心类**: `AbstractMIDletSuiteLoader`, `MIDletPeer`
- **本地接口**: `midpNativeAppManagerPeer.c`

#### LCUI (LCD User Interface)
- **作用**: 提供 UI 组件和图形绘制
- **核心类**: `Display`, `Canvas`, `Graphics`, `GameCanvas`
- **本地接口**: `lcdui_display.c`, `lcdui_game.c`, `lcdui_input.c`

#### LowLevelUI
- **作用**: 底层图形实现
- **核心文件**: `gxj_graphics.c`, `gxj_putpixel.c`
- **平台接口**: `gxp_graphics.c` (需要实现 Vita 版本)

---

## 2. 移植策略

### 2.1 总体策略

#### 方法 1: 集成到 phoneme-cldc (推荐 ⭐)
将 MIDP 作为 phoneme-cldc 的一个模块构建，生成一个包含 MIDP 支持的 JVM。

**优点**:
- 单一二进制文件，易于分发
- 直接使用 phoneme-cldc 的 JVM 核心
- 无需额外的 JNI 调用开销

**缺点**:
- 二进制文件较大
- 需要修改 phoneme-cldc 的构建系统

#### 方法 2: 独立构建 MIDP 库
将 MIDP 构建为独立的共享库，与 phoneme-cldc 分开。

**优点**:
- 模块化设计
- 可以选择性加载 MIDP

**缺点**:
- 需要 JNI 调用
- 多个文件管理

**决定**: 采用 **方法 1**，集成到 phoneme-cldc

### 2.2 移植步骤

#### 阶段 2A: 基础设施 (1-2 周)
1. **创建 Vita 平台配置**
   - 复制 `linux_fb_gcc` 为 `vita_arm`
   - 修改 `Platform.gmk`, `Options.gmk`, `config.gmk`
   - 创建 `GNUmakefile`

2. **集成到 phoneme-cldc 构建系统**
   - 修改 phoneme-cldc 的构建配置
   - 添加 MIDP 模块
   - 确保依赖关系正确

3. **平台抽象层**
   - 创建 `platform/vita` 目录
   - 实现基本的平台接口

#### 阶段 2B: AMS 移植 (1 周)
1. **MIDlet 生命周期管理**
   - 移植 `ams_api` 模块
   - 实现 `MIDlet.java` 支持
   - 创建 Vita 版本的 `midpNativeAppManagerPeer.c`

2. **应用管理**
   - 移植 `ams_base` 模块
   - 实现 `AbstractMIDletSuiteLoader`
   - 处理 MIDlet 套件加载

#### 阶段 2C: LCUI 移植 (2-3 周)
1. **图形系统**
   - 移植 `lowlevelui/graphics` 模块
   - 实现 `gxp_graphics.c` (SceGxm 后端)
   - 实现 `gxj_graphics.c` (软件渲染后端)

2. **显示管理**
   - 移植 `lcdui_display.c`
   - 实现 `Display` 类
   - 处理屏幕刷新

3. **输入系统**
   - 移植 `lcdui_input.c`
   - 实现按键映射 (Vita 控制器 → J2ME 按键)
   - 处理触摸屏 (如果需要)

4. **游戏支持**
   - 移植 `lcdui_game.c`
   - 实现 `GameCanvas` 类
   - 优化游戏循环

#### 阶段 2D: I/O 移植 (1 周)
1. **HTTP 连接**
   - 实现 `HttpConnection`
   - 使用 Vita 的网络 API

2. **Socket 连接**
   - 实现 `SocketConnection`
   - 基于 phoneme-cldc 的 BSD socket 支持

3. **文件连接**
   - 实现 `FileConnection`
   - 使用 Vita 的文件系统 API

#### 阶段 2E: 测试和优化 (2-3 周)
1. **基本测试**
   - Hello World MIDlet
   - Canvas 测试
   - 输入测试
   - 图形测试

2. **游戏测试**
   - 简单 2D 游戏
   - 精灵动画
   - 碰撞检测
   - 滚动背景

3. **性能优化**
   - 图形渲染优化
   - 内存管理优化
   - 输入响应优化

---

## 3. 详细实现计划

### 3.1 Vita 平台配置

#### 文件: `phoneme-midp/build/vita_arm/Platform.gmk`
```makefile
# Platform definition for PS Vita
HOST_PLATFORM    = linux
HOST_OS          = linux
HOST_CPU         = i386
HOST_COMPILER    = gcc

TARGET_PLATFORM  = vita_arm
TARGET_OS        = vita
TARGET_CPU       = arm
TARGET_COMPILER  = gcc
TARGET_VM        = cldc_vm
TARGET_DEVICE    = vita

FN_SEP           = /
```

#### 文件: `phoneme-midp/build/vita_arm/Options.gmk`
```makefile
# Build options for PS Vita
USE_CLDC_11             = true
USE_MONET               = false
USE_VM_PROFILES         = false

# JSRs (Java Specification Requests)
USE_JSR_75              = false  # FileConnection
USE_JSR_82              = false  # Bluetooth
USE_JSR_120             = false  # Wireless Messaging
USE_JSR_135             = false  # Mobile Media API
USE_JSR_172             = false  # Web Services
USE_JSR_177             = false  # Security and Trust
USE_JSR_179             = false  # Location API
USE_JSR_180             = false  # SIP API
USE_JSR_184             = false  # 3D Graphics
USE_JSR_205             = false  # Wireless Messaging 2.0
USE_JSR_211             = false  # Content Handler API
USE_JSR_226             = false  # Scalable 2D Vector Graphics
USE_JSR_229             = false  # Payment API
USE_JSR_230             = false  # VIA (Video on Demand)
USE_JSR_234             = false  # Advanced Multimedia Supplements
USE_JSR_238             = false  # Internationalization API
USE_JSR_239             = false  # Java Binding for OpenGL ES
USE_JSR_280             = false  # XML API for J2ME

# MIDP options
USE_MULTIPLE_ISOLATES   = false
USE_STATIC_PROPERTIES   = true
USE_IMAGE_CACHE         = true
USE_FONT_CACHE          = false
USE_ICON_CACHE          = true
USE_RMS_TREE_INDEX      = false
USE_NETWORK_INDICATOR   = true
USE_CLDC_RELEASE        = false
USE_NATIVE_APP_MANAGER  = false
USE_NATIVE_INSTALLER    = false
```

#### 文件: `phoneme-midp/build/vita_arm/config.gmk`
```makefile
# Component configuration for PS Vita
core/javautil           = reference
core/string             = reference
core/memory             = reference
core/storage            = reference
core/log                = reference
core/global_status      = reference
core/resource_manager   = reference
rms/rms_api             = reference
rms/rms_exc             = reference
rms/rms_base            = reference
rms/record_store        = file_based
rms/record_index        = linear_index
vm_services             = cldc_vm
ams/ams_api             = reference
ams/ams_base            = reference
ams/nams                = reference
ams/mvm                 = reference
ams/platform_request    = vita
ams/ota                 = reference
ams/ota_control         = reference
ams/ams_jsr_interface   = reference
ams/appmanager_ui       = reference
ams/ams_util            = mvm
ams/app_image_gen       = reference
ams/app_image_gen_base  = mvm
ams/autotester          = mvm
ams/autotester_base     = reference
ams/installer           = reference
ams/jams                = mvm
ams/midlet_suite_info   = reference
```

### 3.2 平台抽象层

#### 目录结构: `phoneme-midp/src/porting_demos/vita/`
```
porting_demos/vita/
├── Makefile
├── include/
│   └── platform_vita.h
├── native/
│   ├── platform_graphics.c
│   ├── platform_input.c
│   ├── platform_audio.c
│   └── platform_display.c
└── lib.gmk
```

#### 文件: `platform_vita.h`
```c
#ifndef PLATFORM_VITA_H
#define PLATFORM_VITA_H

#include <psp2/gxm.h>
#include <psp2/ctrl.h>
#include <psp2/audioout.h>
#include <psp2/display.h>

// Display constants
#define VITA_SCREEN_WIDTH  960
#define VITA_SCREEN_HEIGHT 544
#define VITA_SCREEN_BPP    32

// Graphics context
extern SceGxmContext* gxmContext;
extern SceGxmRenderTarget* renderTarget;
extern SceGxmShaderPatched* shader;
extern SceGxmVertexProgram* vertexProgram;
extern SceGxmFragmentProgram* fragmentProgram;

// Input state
extern SceCtrlData ctrlData;

// Initialize platform
int platform_vita_init();
void platform_vita_shutdown();

// Graphics functions
void platform_vita_clear_screen(uint32_t color);
void platform_vita_swap_buffers();
void platform_vita_draw_pixel(int x, int y, uint32_t color);
void platform_vita_draw_rect(int x, int y, int w, int h, uint32_t color);

// Input functions
void platform_vita_poll_input();
int platform_vita_get_key_state(int keyCode);

// Display functions
void platform_vita_create_display(int width, int height);
void platform_vita_destroy_display();

#endif // PLATFORM_VITA_H
```

### 3.3 AMS 移植

#### 文件: `phoneme-midp/src/ams/nams/reference/native/midpNativeAppManagerPeer_vita.c`
```c
#include <kni.h>
#include <midp_logging.h>
#include <midpMalloc.h>

#include "midpNativeAppManagerPeer.h"
#include "platform_vita.h"

/**
 * Initialize the native application manager peer
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_main_NativeAppManagerPeer_init) {
    // Initialize Vita platform
    if (platform_vita_init() != 0) {
        midpMalloc_setOutOfMemory();
        KNI_ThrowNew(KNI_GetClassPointer(env, "java/lang/OutOfMemoryError"),
                    "Failed to initialize Vita platform");
        return;
    }
    KNI_ReturnVoid();
}

/**
 * Shutdown the native application manager peer
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_main_NativeAppManagerPeer_shutdown) {
    platform_vita_shutdown();
    KNI_ReturnVoid();
}

/**
 * Process events
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_main_NativeAppManagerPeer_processEvents) {
    platform_vita_poll_input();
    KNI_ReturnVoid();
}
```

### 3.4 LCUI 移植

#### 文件: `phoneme-midp/src/highlevelui/lcdui/reference/native/lcdui_display_vita.c`
```c
#include <kni.h>
#include <sni.h>
#include <jvm.h>
#include <commonKNIMacros.h>

#include <lcdlf_export.h>
#include <midpEventUtil.h>
#include <gxapi_graphics.h>

#include "platform_vita.h"

/**
 * Refresh display
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_lcdui_DisplayDevice_refresh0) {
    int y2 = KNI_GetParameterAsInt(6);
    int x2 = KNI_GetParameterAsInt(5);
    int y1 = KNI_GetParameterAsInt(4);
    int x1 = KNI_GetParameterAsInt(3);
    jint displayId = KNI_GetParameterAsInt(2);
    jint hardwareId = KNI_GetParameterAsInt(1);

    // For now, just swap buffers
    platform_vita_swap_buffers();
    KNI_ReturnVoid();
}

/**
 * Get display dimensions
 */
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_lcdui_DisplayDevice_getDisplayWidth) {
    KNI_ReturnInt(VITA_SCREEN_WIDTH);
}

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_lcdui_DisplayDevice_getDisplayHeight) {
    KNI_ReturnInt(VITA_SCREEN_HEIGHT);
}
```

#### 文件: `phoneme-midp/src/highlevelui/lcdui/reference/native/lcdui_input_vita.c`
```c
#include <kni.h>
#include <sni.h>

#include "platform_vita.h"

// J2ME key codes
#define KEY_NUM0       48
#define KEY_NUM1       49
#define KEY_NUM2       50
#define KEY_NUM3       51
#define KEY_UP         1
#define KEY_DOWN       2
#define KEY_LEFT       3
#define KEY_RIGHT      4
#define KEY_FIRE       5

// Vita button mapping
static int vita_to_j2me_key[] = {
    [SCE_CTRL_CROSS]     = KEY_NUM0,    // A button
    [SCE_CTRL_CIRCLE]    = KEY_NUM1,    // B button
    [SCE_CTRL_SQUARE]    = KEY_NUM2,    // X button
    [SCE_CTRL_TRIANGLE]  = KEY_NUM3,    // Y button
    [SCE_CTRL_DPAD_UP]   = KEY_UP,
    [SCE_CTRL_DPAD_DOWN] = KEY_DOWN,
    [SCE_CTRL_DPAD_LEFT] = KEY_LEFT,
    [SCE_CTRL_DPAD_RIGHT]= KEY_RIGHT,
    [SCE_CTRL_LTRIGGER]  = KEY_FIRE,
    [SCE_CTRL_RTRIGGER]  = KEY_FIRE,
};

/**
 * Get key state
 */
KNIEXPORT KNI_RETURNTYPE_BOOLEAN
KNIDECL(com_sun_midp_lcdui_EventHandler_getKeyState) {
    int j2meKeyCode = KNI_GetParameterAsInt(1);
    
    // Poll input
    platform_vita_poll_input();
    
    // Check all Vita buttons
    for (int i = 0; i < SCE_CTRL_MAX_BUTTONS; i++) {
        if (vita_to_j2me_key[i] == j2meKeyCode) {
            if (ctrlData.buttons & (1 << i)) {
                KNI_ReturnBoolean(KNI_TRUE);
            }
        }
    }
    
    KNI_ReturnBoolean(KNI_FALSE);
}
```

### 3.5 图形系统移植

#### 文件: `phoneme-midp/src/lowlevelui/graphics/gx_platform/native/gxp_graphics_vita.c`
```c
#include <kni.h>
#include <gxapi_graphics.h>

#include "platform_vita.h"
#include "gxj_intern_graphics.h"

/**
 * Initialize graphics
 */
void gxp_graphics_init() {
    // Initialize SceGxm
    SceGxmInitializeParams gxmParams;
    memset(&gxmParams, 0, sizeof(gxmParams));
    gxmParams.flags = SCE_GXM_INITIALIZE_FLAG_NONE;
    
    sceGxmInitialize(&gxmParams);
    
    // Create context
    SceGxmContextParams contextParams;
    memset(&contextParams, 0, sizeof(contextParams));
    contextParams.hostMem = SCE_GXM_HOST_MEM_SIZE;
    contextParams.hostMemCallback = NULL;
    contextParams.parameterBufferMem = SCE_GXM_PARAMETER_BUFFER_MEM_SIZE;
    
    gxmContext = sceGxmCreateContext(&contextParams);
    
    // Create render target
    SceGxmRenderTargetParams renderTargetParams;
    memset(&renderTargetParams, 0, sizeof(renderTargetParams));
    renderTargetParams.width = VITA_SCREEN_WIDTH;
    renderTargetParams.height = VITA_SCREEN_HEIGHT;
    renderTargetParams.samples = SCE_GXM_MULTISAMPLE_NONE;
    renderTargetParams.driverMemBlock = -1;
    
    renderTarget = sceGxmCreateRenderTarget(&renderTargetParams);
}

/**
 * Draw pixel
 */
void gxp_draw_pixel(jint x, jint y, jint color) {
    // Convert color from Java ARGB to Vita ABGR
    uint32_t vitaColor = ((color & 0xFF00FF00) | \
                        ((color & 0x00FF0000) >> 16) | \
                        ((color & 0x000000FF) << 16));
    
    // For now, use software rendering
    // TODO: Implement hardware-accelerated pixel drawing
    platform_vita_draw_pixel(x, y, vitaColor);
}

/**
 * Fill rectangle
 */
void gxp_fill_rect(jint x, jint y, jint width, jint height, jint color) {
    uint32_t vitaColor = ((color & 0xFF00FF00) | \
                        ((color & 0x00FF0000) >> 16) | \
                        ((color & 0x000000FF) << 16));
    
    platform_vita_draw_rect(x, y, width, height, vitaColor);
}
```

---

## 4. 构建系统集成

### 4.1 修改 phoneme-cldc 构建配置

#### 文件: `phoneme-cldc/build/vita_arm/vita_arm.cfg`
添加 MIDP 支持：
```makefile
# MIDP support
ifndef ENABLE_MIDP
ENABLE_MIDP := true
export ENABLE_MIDP__BY := vita_arm.cfg
endif

# MIDP directory
MIDP_DIR := $(JWC_WORK_SPACE)/../phoneme-midp
```

### 4.2 创建 MIDP 构建规则

#### 文件: `phoneme-cldc/build/vita_arm/target/MIDP.gmk`
```makefile
# MIDP build rules for Vita

ifdef ENABLE_MIDP

MIDP_BUILD_DIR := $(MIDP_DIR)/build/vita_arm
MIDP_OUTPUT_DIR := $(MIDP_BUILD_DIR)/target/debug

# Include MIDP build
include $(MIDP_BUILD_DIR)/GNUmakefile

# Add MIDP libraries to linking
LIBRARIES += $(MIDP_OUTPUT_DIR)/lib/libmidp_vita.a

# Add MIDP include paths
EXTRA_INCLUDES += $(MIDP_DIR)/src/ams/ams_api/reference 
                   $(MIDP_DIR)/src/ams/ams_base/reference 
                   $(MIDP_DIR)/src/highlevelui/lcdui/reference 
                   $(MIDP_DIR)/src/lowlevelui/graphics/gx_platform/include

endif
```

---

## 5. 按键映射详细方案

### 5.1 J2ME 按键码

```
┌─────────────────────────────────────────────────────────┐
│ J2ME Canvas Key Codes (from Canvas.java)                  │
├─────────────────────────────────────────────────────────┤
│                                                             │
│  数字键 (0-9):                                              │
│    KEY_NUM0 = 48    KEY_NUM1 = 49    KEY_NUM2 = 50         │
│    KEY_NUM3 = 51    KEY_NUM4 = 52    KEY_NUM5 = 53         │
│    KEY_NUM6 = 54    KEY_NUM7 = 55    KEY_NUM8 = 56         │
│    KEY_NUM9 = 57                                                   │
│                                                             │
│  星号和井号:                                                 │
│    KEY_STAR = 42    KEY_POUND = 35                           │
│                                                             │
│  方向键:                                                    │
│    KEY_UP = 1       KEY_DOWN = 2                            │
│    KEY_LEFT = 3     KEY_RIGHT = 4                           │
│                                                             │
│  功能键:                                                    │
│    KEY_FIRE = 5           # 确认/选择                       │
│    KEY_SOFT1 = -1         # 左软键 (通常映射到 L触发器)      │
│    KEY_SOFT2 = -2         # 右软键 (通常映射到 R触发器)      │
│                                                             │
│  游戏键 (GameCanvas):                                       │
│    GAME_A = -3            # 游戏A键                        │
│    GAME_B = -4            # 游戏B键                        │
│    GAME_C = -5            # 游戏C键                        │
│    GAME_D = -6            # 游戏D键                        │
│                                                             │
└─────────────────────────────────────────────────────────┘
```

### 5.2 Vita 控制器映射

```
┌─────────────────────────────────────────────────────────┐
│ Vita Controller Button Mapping                           │
├─────────────────────────────────────────────────────────┤
│                                                             │
│  面板按键:                                                  │
│    ┌─────────┐                                              │
│    │  △ O    │  △ = KEY_NUM3 (51)  O = KEY_NUM1 (49)        │
│    │  □ X    │  □ = KEY_NUM2 (50)  X = KEY_NUM0 (48)        │
│    │  L1 R1  │  L1 = KEY_SOFT1    R1 = KEY_SOFT2           │
│    └─────────┘                                              │
│    ┌─────────┐                                              │
│    │  ↑ ↓ ← →│  ↑ = KEY_UP       ↓ = KEY_DOWN               │
│    │         │  ← = KEY_LEFT     → = KEY_RIGHT              │
│    └─────────┘                                              │
│                                                             │
│  触摸板:                                                    │
│    左触摸板 = 鼠标移动 (如果游戏支持)                         │
│    右触摸板 = 鼠标移动 (如果游戏支持)                         │
│                                                             │
│  模拟按键:                                                  │
│    L2/R2 = 无映射 (可选映射到 GAME_C/GAME_D)                  │
│    START = 无映射 (可选映射到 暂停)                         │
│    SELECT = 无映射 (可选映射到 菜单)                         │
│                                                             │
└─────────────────────────────────────────────────────────┘
```

### 5.3 实现代码

#### 文件: `platform_input.c`
```c
#include "platform_vita.h"

// J2ME key codes
#define KEY_NUM0       48
#define KEY_NUM1       49
#define KEY_NUM2       50
#define KEY_NUM3       51
#define KEY_UP         1
#define KEY_DOWN       2
#define KEY_LEFT       3
#define KEY_RIGHT      4
#define KEY_FIRE       5
#define KEY_SOFT1      -1
#define KEY_SOFT2      -2
#define GAME_A         -3
#define GAME_B         -4
#define GAME_C         -5
#define GAME_D         -6

// Current key states
static int keyStates[256] = {0};

// Vita button to J2ME key mapping
static struct {
    uint32_t vitaButton;
    int j2meKeyCode;
} buttonMap[] = {
    {SCE_CTRL_CROSS,     KEY_NUM0},    // A button (X on Vita)
    {SCE_CTRL_CIRCLE,    KEY_NUM1},    // B button (O on Vita)
    {SCE_CTRL_SQUARE,    KEY_NUM2},    // X button (□ on Vita)
    {SCE_CTRL_TRIANGLE,  KEY_NUM3},    // Y button (△ on Vita)
    {SCE_CTRL_DPAD_UP,   KEY_UP},
    {SCE_CTRL_DPAD_DOWN, KEY_DOWN},
    {SCE_CTRL_DPAD_LEFT, KEY_LEFT},
    {SCE_CTRL_DPAD_RIGHT, KEY_RIGHT},
    {SCE_CTRL_LTRIGGER,  KEY_SOFT1},   // L1
    {SCE_CTRL_RTRIGGER,  KEY_SOFT2},   // R1
    {SCE_CTRL_L2,       GAME_C},       // L2
    {SCE_CTRL_R2,       GAME_D},       // R2
    {SCE_CTRL_START,    -7},           // START (custom)
    {SCE_CTRL_SELECT,   -8},           // SELECT (custom)
};

SceCtrlData ctrlData;

/**
 * Initialize input system
 */
int platform_vita_input_init() {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceCtrlSetSamplingRate(120); // 120Hz sampling
    return 0;
}

/**
 * Poll input
 */
void platform_vita_poll_input() {
    sceCtrlPeekBufferPositive(0, &ctrlData, 1);
    
    // Update key states
    for (int i = 0; i < sizeof(buttonMap)/sizeof(buttonMap[0]); i++) {
        if (ctrlData.buttons & buttonMap[i].vitaButton) {
            keyStates[buttonMap[i].j2meKeyCode & 0xFF] = 1;
        } else {
            keyStates[buttonMap[i].j2meKeyCode & 0xFF] = 0;
        }
    }
}

/**
 * Get key state
 */
int platform_vita_get_key_state(int keyCode) {
    if (keyCode < 0 || keyCode > 255) {
        return 0;
    }
    return keyStates[keyCode];
}

/**
 * Get analog stick position (for touch emulation)
 */
void platform_vita_get_analog(int* x, int* y) {
    *x = ctrlData.lx - 128;  // Left stick X (-128 to 127)
    *y = ctrlData.ly - 128;  // Left stick Y (-128 to 127)
}
```

---

## 6. 图形渲染详细方案

### 6.1 渲染架构

```
┌─────────────────────────────────────────────────────────┐
│ MIDP Graphics Rendering Pipeline                           │
├─────────────────────────────────────────────────────────┤
│                                                             │
│  Java Layer (MIDP)                                         │
│  ┌─────────────────────┐                                  │
│  │  Graphics.java       │  Graphics g = canvas.getGraphics();│
│  │  Canvas.java         │  g.setColor(0xFF0000);           │
│  │  GameCanvas.java      │  g.fillRect(10, 10, 100, 100);     │
│  └─────────────────────┘                                  │
│              │                                              │
│              ▼                                              │
│  ┌─────────────────────┐                                  │
│  │  JNI Bridge          │  Java → Native calls             │
│  │  lcdui_graphics.c    │                                  │
│  └─────────────────────┘                                  │
│              │                                              │
│              ▼                                              │
│  ┌─────────────────────┐                                  │
│  │  LowLevelUI          │  gxj_graphics.c                 │
│  │  (Software Render)   │  - Draw pixel, line, rect        │
│  └─────────────────────┘                                  │
│              │                                              │
│              ▼                                              │
│  ┌─────────────────────┐                                  │
│  │  Platform Graphics    │  gxp_graphics_vita.c            │
│  │  (Hardware Accel)    │  - SceGxm backend                │
│  └─────────────────────┘                                  │
│              │                                              │
│              ▼                                              │
│  ┌─────────────────────┐                                  │
│  │  SceGxm API          │  - Vertex buffers                │
│  │  (Vita GPU)          │  - Shaders                       │
│  └─────────────────────┘                                  │
│                                                             │
└─────────────────────────────────────────────────────────┘
```

### 6.2 渲染模式

#### 模式 1: 软件渲染 (Software Rendering)
- **优点**: 简单，易于实现，兼容性好
- **缺点**: 性能较低
- **适用场景**: 简单游戏，原型开发

#### 模式 2: 硬件加速渲染 (Hardware Acceleration)
- **优点**: 性能高，支持复杂效果
- **缺点**: 实现复杂，需要深入理解 SceGxm
- **适用场景**: 商业游戏，性能要求高的应用

**决定**: 先实现软件渲染，后续再优化为硬件加速

### 6.3 软件渲染实现

#### 文件: `platform_graphics.c`
```c
#include "platform_vita.h"

// Screen buffer
static uint32_t* screenBuffer = NULL;
static int screenWidth = VITA_SCREEN_WIDTH;
static int screenHeight = VITA_SCREEN_HEIGHT;
static int screenPitch = VITA_SCREEN_WIDTH;

// SceGxm display queue
static SceGxmDisplayQueue* displayQueue = NULL;

/**
 * Initialize graphics
 */
int platform_vita_graphics_init() {
    // Allocate screen buffer
    screenBuffer = (uint32_t*)malloc(screenWidth * screenHeight * sizeof(uint32_t));
    if (!screenBuffer) {
        return -1;
    }
    
    // Initialize SceGxm display queue
    SceGxmDisplayQueueParams displayQueueParams;
    memset(&displayQueueParams, 0, sizeof(displayQueueParams));
    displayQueueParams.priority = SCE_KERNEL_PRIO_DEFAULT;
    displayQueueParams.poolSize = SCE_GXM_DEFAULT_POOL_SIZE;
    displayQueueParams.stackSize = SCE_GXM_DEFAULT_STACK_SIZE;
    displayQueueParams.cbFunc = NULL;
    displayQueueParams.cbArg = NULL;
    
    if (sceGxmDisplayQueueCreate(&displayQueueParams) < 0) {
        free(screenBuffer);
        return -1;
    }
    
    return 0;
}

/**
 * Clear screen
 */
void platform_vita_clear_screen(uint32_t color) {
    for (int i = 0; i < screenWidth * screenHeight; i++) {
        screenBuffer[i] = color;
    }
}

/**
 * Draw pixel
 */
void platform_vita_draw_pixel(int x, int y, uint32_t color) {
    if (x < 0 || x >= screenWidth || y < 0 || y >= screenHeight) {
        return;
    }
    screenBuffer[y * screenPitch + x] = color;
}

/**
 * Draw rectangle
 */
void platform_vita_draw_rect(int x, int y, int w, int h, uint32_t color) {
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            int px = x + dx;
            int py = y + dy;
            if (px >= 0 && px < screenWidth && py >= 0 && py < screenHeight) {
                screenBuffer[py * screenPitch + px] = color;
            }
        }
    }
}

/**
 * Swap buffers
 */
void platform_vita_swap_buffers() {
    // Create SceGxm sync object
    SceGxmSyncObject* syncObject = sceGxmCreateSyncObject();
    
    // Create render target
    SceGxmRenderTarget* renderTarget;
    SceGxmRenderTargetParams renderTargetParams;
    memset(&renderTargetParams, 0, sizeof(renderTargetParams));
    renderTargetParams.width = screenWidth;
    renderTargetParams.height = screenHeight;
    renderTargetParams.samples = SCE_GXM_MULTISAMPLE_NONE;
    renderTargetParams.driverMemBlock = -1;
    
    renderTarget = sceGxmCreateRenderTarget(&renderTargetParams);
    
    // Begin scene
    sceGxmBeginScene(gxmContext, 0, renderTarget, NULL, NULL, syncObject, NULL, NULL);
    
    // Copy screen buffer to render target
    SceGxmTexture texture;
    SceGxmTextureParams textureParams;
    memset(&textureParams, 0, sizeof(textureParams));
    textureParams.width = screenWidth;
    textureParams.height = screenHeight;
    textureParams.format = SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR;
    textureParams.mipFilter = SCE_GXM_TEXTURE_MIP_FILTER_NONE;
    textureParams.minFilter = SCE_GXM_TEXTURE_FILTER_NEAREST;
    textureParams.magFilter = SCE_GXM_TEXTURE_FILTER_NEAREST;
    
    sceGxmTextureInitLinear(&texture, &textureParams, screenBuffer);
    
    // Draw texture
    // ... (shader setup and drawing code)
    
    // End scene
    sceGxmEndScene(gxmContext, NULL, NULL);
    
    // Display
    sceGxmDisplayQueueAddEntry(displayQueue, syncObject, NULL);
    sceGxmSyncObjectDestroy(syncObject);
    sceGxmRenderTargetDestroy(renderTarget);
}

/**
 * Shutdown graphics
 */
void platform_vita_graphics_shutdown() {
    if (screenBuffer) {
        free(screenBuffer);
        screenBuffer = NULL;
    }
    if (displayQueue) {
        sceGxmDisplayQueueDestroy(displayQueue);
        displayQueue = NULL;
    }
}
```

---

## 7. 测试计划

### 7.1 测试环境

#### 硬件要求
- PS Vita (任何型号)
- 自定义固件 (支持 homebrew)
- 至少 1GB 空闲存储空间

#### 软件要求
- Vita SDK (已安装)
- VPK 包管理器
- 调试工具 (可选)

### 7.2 测试用例

#### 基础测试 (Phase 2A)
1. **Hello World MIDlet**
   ```java
   public class HelloMIDlet extends MIDlet {
       public void startApp() {
           Display.getDisplay(this).setCurrent(new Form("Hello"));
       }
       public void pauseApp() {}
       public void destroyApp(boolean unconditional) {}
   }
   ```

2. **Canvas 测试**
   ```java
   public class CanvasTest extends MIDlet {
       public void startApp() {
           Display.getDisplay(this).setCurrent(new TestCanvas());
       }
       
       class TestCanvas extends Canvas {
           public void paint(Graphics g) {
               g.setColor(0xFF0000); // Red
               g.fillRect(0, 0, getWidth(), getHeight());
           }
       }
   }
   ```

#### 输入测试 (Phase 2B)
3. **按键测试**
   ```java
   public class InputTest extends MIDlet {
       public void startApp() {
           Display.getDisplay(this).setCurrent(new InputCanvas());
       }
       
       class InputCanvas extends Canvas {
           private String message = "Press buttons";
           
           public void paint(Graphics g) {
               g.drawString(message, 10, 10, Graphics.TOP | Graphics.LEFT);
           }
           
           public void keyPressed(int keyCode) {
               message = "Key: " + keyCode;
               repaint();
           }
       }
   }
   ```

#### 图形测试 (Phase 2C)
4. **图形绘制测试**
   ```java
   public class GraphicsTest extends MIDlet {
       public void startApp() {
           Display.getDisplay(this).setCurrent(new GraphicsCanvas());
       }
       
       class GraphicsCanvas extends Canvas {
           public void paint(Graphics g) {
               // Clear
               g.setColor(0xFFFFFF);
               g.fillRect(0, 0, getWidth(), getHeight());
               
               // Draw shapes
               g.setColor(0xFF0000);
               g.drawRect(10, 10, 100, 100);
               
               g.setColor(0x00FF00);
               g.fillRect(120, 10, 100, 100);
               
               g.setColor(0x0000FF);
               g.drawLine(10, 120, 220, 120);
               
               g.setColor(0xFF00FF);
               g.drawArc(10, 130, 100, 100, 0, 360);
           }
       }
   }
   ```

5. **精灵测试**
   ```java
   public class SpriteTest extends MIDlet {
       public void startApp() {
           Display.getDisplay(this).setCurrent(new GameCanvas() {
               private Sprite sprite;
               private int x = 100, y = 100;
               
               public void start() {
                   try {
                       Image img = Image.createImage("/sprite.png");
                       sprite = Sprite.createSprite(img);
                   } catch (Exception e) {
                       e.printStackTrace();
                   }
               }
               
               public void paint(Graphics g) {
                   g.setColor(0xFFFFFF);
                   g.fillRect(0, 0, getWidth(), getHeight());
                   if (sprite != null) {
                       sprite.paint(g, x, y);
                   }
               }
               
               public void keyPressed(int keyCode) {
                   switch (keyCode) {
                       case KEY_UP:    y -= 5; break;
                       case KEY_DOWN:  y += 5; break;
                       case KEY_LEFT:  x -= 5; break;
                       case KEY_RIGHT: x += 5; break;
                   }
                   repaint();
               }
           });
       }
   }
   ```

#### 游戏测试 (Phase 2D)
6. **简单游戏测试**
   - 2D 平台游戏
   - 射击游戏
   - 迷宫游戏
   - 卡片游戏

### 7.3 性能测试

#### FPS 测试
```java
public class FPSTest extends MIDlet {
    private long lastTime;
    private int frameCount;
    private int fps;
    
    public void startApp() {
        Display.getDisplay(this).setCurrent(new FPSCanvas());
        lastTime = System.currentTimeMillis();
    }
    
    class FPSCanvas extends GameCanvas {
        public void paint(Graphics g) {
            // Draw FPS
            g.setColor(0xFFFFFF);
            g.fillRect(0, 0, getWidth(), getHeight());
            g.setColor(0x000000);
            g.drawString("FPS: " + fps, 10, 10, Graphics.TOP | Graphics.LEFT);
            
            // Update FPS
            frameCount++;
            long currentTime = System.currentTimeMillis();
            if (currentTime - lastTime >= 1000) {
                fps = frameCount;
                frameCount = 0;
                lastTime = currentTime;
            }
        }
        
        public void run() {
            while (true) {
                paint(getGraphics());
                flushGraphics();
                try { Thread.sleep(16); } catch (Exception e) {}
            }
        }
    }
}
```

---

## 8. 优化策略

### 8.1 图形优化

#### 1. 双缓冲
- 使用两个缓冲区交替渲染
- 减少屏幕闪烁

#### 2. 裁剪优化
- 实现视口裁剪
- 只渲染可见区域

#### 3. 批处理
- 批量处理绘制操作
- 减少状态切换

#### 4. 纹理缓存
- 缓存常用图像
- 减少内存分配

### 8.2 内存优化

#### 1. 对象池
- 重用对象
- 减少 GC 压力

#### 2. 图像压缩
- 使用适合的图像格式
- 支持透明度

#### 3. 内存管理
- 监控内存使用
- 及时释放资源

### 8.3 输入优化

#### 1. 输入缓冲
- 缓冲输入事件
- 减少轮询次数

#### 2. 重复键处理
- 处理按键重复
- 支持长按

---

## 9. 问题排查

### 9.1 常见问题

#### 问题 1: 黑屏
- **可能原因**: 图形初始化失败
- **解决方案**: 检查 SceGxm 初始化，确保显示队列创建成功

#### 问题 2: 按键无响应
- **可能原因**: 输入映射错误，轮询频率不足
- **解决方案**: 检查按键映射表，增加轮询频率

#### 问题 3: 图形错误
- **可能原因**: 颜色格式不匹配，坐标超出范围
- **解决方案**: 检查颜色转换，添加边界检查

#### 问题 4: 内存不足
- **可能原因**: 图像缓存过大，对象泄漏
- **解决方案**: 优化内存使用，检查对象生命周期

### 9.2 调试工具

#### 日志输出
```c
#include <midp_logging.h>

MIDP_LOG("Debug message: %d", value);
MIDP_LOG_ERROR("Error: %s", errorMessage);
```

#### 断言
```c
#include <assert.h>

assert(condition != NULL);
```

#### 性能计数器
```c
static uint64_t frameCount = 0;
static uint64_t startTime = 0;

void startPerformanceCounter() {
    startTime = sceKernelGetProcessTimeWide();
}

void logFPS() {
    uint64_t endTime = sceKernelGetProcessTimeWide();
    uint64_t elapsed = endTime - startTime;
    float seconds = (float)elapsed / 1000000.0f; // Convert to seconds
    float fps = frameCount / seconds;
    MIDP_LOG("FPS: %.2f", fps);
}
```

---

## 10. 文件清单

### 10.1 需要创建的文件

#### 构建配置
- [ ] `phoneme-midp/build/vita_arm/Platform.gmk`
- [ ] `phoneme-midp/build/vita_arm/Options.gmk`
- [ ] `phoneme-midp/build/vita_arm/config.gmk`
- [ ] `phoneme-midp/build/vita_arm/GNUmakefile`

#### 平台抽象层
- [ ] `phoneme-midp/src/porting_demos/vita/include/platform_vita.h`
- [ ] `phoneme-midp/src/porting_demos/vita/native/platform_graphics.c`
- [ ] `phoneme-midp/src/porting_demos/vita/native/platform_input.c`
- [ ] `phoneme-midp/src/porting_demos/vita/native/platform_audio.c`
- [ ] `phoneme-midp/src/porting_demos/vita/native/platform_display.c`
- [ ] `phoneme-midp/src/porting_demos/vita/Makefile`
- [ ] `phoneme-midp/src/porting_demos/vita/lib.gmk`

#### AMS 移植
- [ ] `phoneme-midp/src/ams/nams/reference/native/midpNativeAppManagerPeer_vita.c`
- [ ] `phoneme-midp/src/ams/nams/reference/native/midpNativeDisplayControllerPeer_vita.c`

#### LCUI 移植
- [ ] `phoneme-midp/src/highlevelui/lcdui/reference/native/lcdui_display_vita.c`
- [ ] `phoneme-midp/src/highlevelui/lcdui/reference/native/lcdui_game_vita.c`
- [ ] `phoneme-midp/src/highlevelui/lcdui/reference/native/lcdui_input_vita.c`
- [ ] `phoneme-midp/src/highlevelui/lcdui/reference/native/lcdui_audio_vita.c`

#### 图形系统移植
- [ ] `phoneme-midp/src/lowlevelui/graphics/gx_platform/native/gxp_graphics_vita.c`
- [ ] `phoneme-midp/src/lowlevelui/graphics/gx_putpixel/native/gxj_graphics_vita.c`

#### 集成文件
- [ ] `phoneme-cldc/build/vita_arm/target/MIDP.gmk`

### 10.2 需要修改的文件

#### phoneme-cldc
- [ ] `build/vita_arm/vita_arm.cfg` (添加 MIDP 支持)

#### phoneme-midp
- [ ] `src/ams/ams_base/reference/classes/com/sun/midp/main/AbstractMIDletSuiteLoader.java` (Vita 适配)
- [ ] `src/highlevelui/lcdui/reference/classes/javax/microedition/lcdui/Display.java` (Vita 适配)
- [ ] `src/highlevelui/lcdui/reference/classes/javax/microedition/lcdui/Canvas.java` (Vita 适配)
- [ ] `src/highlevelui/lcdui/reference/classes/javax/microedition/lcdui/Graphics.java` (Vita 适配)

---

## 11. 里程碑

### 里程碑 1: 基础设施完成 (1-2 周)
- [ ] Vita 平台配置创建
- [ ] 构建系统集成
- [ ] 平台抽象层实现
- [ ] 基础测试通过

### 里程碑 2: AMS 完成 (1 周)
- [ ] MIDlet 生命周期管理
- [ ] 应用管理系统
- [ ] Hello World 测试通过

### 里程碑 3: LCUI 基础完成 (2 周)
- [ ] 显示管理
- [ ] 输入系统
- [ ] 基础图形绘制
- [ ] Canvas 测试通过

### 里程碑 4: 图形系统完成 (1-2 周)
- [ ] 软件渲染
- [ ] 硬件加速 (可选)
- [ ] 图形测试通过

### 里程碑 5: I/O 完成 (1 周)
- [ ] HTTP 连接
- [ ] Socket 连接
- [ ] 文件连接

### 里程碑 6: 游戏测试 (2-3 周)
- [ ] 简单游戏测试
- [ ] 复杂游戏测试
- [ ] 性能优化
- [ ] 兼容性测试

---

## 12. 资源链接

### 文档
- [PS Vita SDK Documentation](https://vitasdk.org)
- [phoneME Project Documentation](https://phoneme.dev.java.net)
- [J2ME CLDC 1.1 Specification](https://jcp.org/en/jsr/detail?id=139)
- [J2ME MIDP 2.0 Specification](https://jcp.org/en/jsr/detail?id=118)

### API 参考
- [SceGxm API](https://vitasdk.org/site/vitasdk/docs/libgxm.html)
- [SceCtrl API](https://vitasdk.org/site/vitasdk/docs/libctrl.html)
- [SceAudio API](https://vitasdk.org/site/vitasdk/docs/libaudio.html)

### 社区
- [Vita Homebrew Discord](https://discord.gg/vitasdk)
- [GBAtemp Vita Forum](https://gbatemp.net/forums/ps-vita.272/)

---

## 13. 附录: J2ME 游戏兼容性

### 13.1 支持的游戏类型

| 游戏类型 | 支持程度 | 备注 |
|----------|----------|------|
| 2D 平台游戏 | ⭐⭐⭐⭐⭐ | 完全支持 |
| 2D 射击游戏 | ⭐⭐⭐⭐⭐ | 完全支持 |
| 2D 迷宫游戏 | ⭐⭐⭐⭐⭐ | 完全支持 |
| 2D 卡片游戏 | ⭐⭐⭐⭐⭐ | 完全支持 |
| 2D RPG | ⭐⭐⭐⭐ | 基本支持 |
| 2D 策略游戏 | ⭐⭐⭐⭐ | 基本支持 |
| 2D 益智游戏 | ⭐⭐⭐⭐⭐ | 完全支持 |
| 3D 游戏 | ⭐ | 不支持 (MIDP 2.0 才支持) |
| 在线游戏 | ⭐⭐ | 部分支持 (需要网络) |

### 13.2 已知兼容性问题

| 问题 | 影响 | 解决方案 |
|------|------|----------|
| 触摸屏支持 | 部分游戏 | 模拟触摸屏为方向键 |
| 多点触控 | 不支持 | 使用单点触控模拟 |
| 加速度计 | 不支持 | 使用方向键模拟 |
| GPS | 不支持 | 返回固定坐标 |
| 摄像头 | 不支持 | 返回空图像 |
| 语音 | 不支持 | 返回空数据 |

---

**文档版本**: 1.0  
**创建日期**: 2026-08-25  
**最后更新**: 2026-08-25  
**状态**: 计划阶段  
**下一步**: 开始实施 Phase 2A (基础设施)