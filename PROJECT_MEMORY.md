# J2ME/MIDP on PS Vita - Project Memory
> Last Updated: 2026-08-30

## 2026-08-30 缺口分析与 vita-port 重建（重要！）

### 功能缺口分析结论（当天完成，详见对话记录）
游戏三链路断裂的根因全部定位：
- **渲染链唯一断点**: `libobj.a` 的 `lfjport_fb_export.o` 里 `lfjport_refresh()` 是空 TODO——
  整条 gxj 渲染管线画进 `gxj_system_screen_buffer` 后无人上屏。
  链路: Graphics native → gxj_system_screen_buffer → DisplayDevice.refresh0 → lcdlf_refresh → lfjport_refresh(空)。
- **输入链唯一断点**: `libobj.a` 的 `mastermode_export.o` 里 `checkForSystemSignal()` 是空函数——
  VM 周期回调 `midp_check_events()` 轮询到它什么都不做。
  正规链路: checkForSystemSignal 填 MIDP_KEY_EVENT+UI_SIGNAL → midpStoreEventAndSignalForeground → Java EventQueue。
  键码用 `keymap_input.h` 的 KEYMAP_KEY_* 值（'0'-'9'、UP=-1..SELECT=-5、SOFT1/2=-6/-7、GAMEA-D=-13..-16）。
- **旧自研代码是死代码**: `phoneme-midp/src/porting/vita/*` 的 KNI 名与 Java 侧 native 声明不匹配，
  且双 framebuffer（vita_fb vs gxj_system_screen_buffer）从未汇合。已放弃，不再演进。
- 其余缺口: 无音频（JSR-135 关+无 SceAudio 代码）、仅 5x7 ASCII 字体、网络全 stub、
  internal.config 的 display 尺寸配置（240x320 虚拟屏需同步三处：lfjport、config、gxj buffer）。

### vita-port/（新的权威移植层目录）
**所有新代码只写 `samples/j2me/vita-port/`，phoneme 两树零改动**。构建方式：链接前
`ar d libobj_no_main.a lfjport_fb_export.o mastermode_export.o runMidlet_md.o`，
再链入本目录实现（`--allow-multiple-definition` 下先到的 .o 赢）。
- `src/vita_display.c`: lfjport 22 符号全覆盖；240x320 虚拟屏 → 960x544 最近邻缩放居中 blit（RGB565→A8B8G8R8）
- `src/vita_input.c`: 真 checkForSystemSignal + 16 槽 SPSC 事件环 + SceCtrl 边沿检测
- `src/vita_main.c`: 启动器，读 `ux0:/data/J2ME00001/launch.cfg`（首行 jar 路径、次行 MIDlet 类名）运行任意游戏
- `src/vita_pcsl.c`: 复制自旧 vita_stubs.c（sceIo 文件 I/O 真实现，socket 仍 stub）
- `config/internal.config`: 改为 display 240x320 后随 VPK 打包
- `midlets/`: HelloMIDlet + CanvasTest（渲染验证）+ InputTest（按键验证）
- 构建: `./build.sh`（`clean`/`midp` 参数）；产物 `build/cmake/midp_vita.vpk`
- 构建已验证: 链接成功、覆盖符号已确认在最终 ELF（checkForSystemSignal=0x160 新实现、
  lfjport_refresh 尾调用 flip_to_display）、VPK 含 240x320 config；**Vita3K 运行验证待做**
- **Vita3K 首次运行反馈 (8-30)**: 能启动，屏幕只显示 Form 的 softbar "Exit"，标题/正文空白。
  根因: `midp_system.jar` 里没有任何 PNG——chameleon 皮肤资源
  (`src/highlevelui/lcdlf/lfjava/resource/`，178 张 skin PNG + indicator) 没进 classes/ 目录，
  CMake 的 `jar cf -C classes .` 自然打不进去。皮肤加载失败 → 布局字段未初始化 → Form 主体不可见。
  修复: vita-port/CMakeLists.txt 的 jar 命令追加
  `-C ${MIDP_DIR}/src/highlevelui/lcdlf/lfjava/resource .`（jar 878KB→1.09MB，183 张 PNG）。
  渲染链已被此次运行间接验证（softbar 上屏=字体+blit+refresh 都工作）。

### 当前构建管线事实（2026-08-30 起，取代早前 ROM 化描述）
- 现行链路是**非 ROM 化**的：CLDC 库 `dist/lib/libcldc_vm*.a`（8-29 20:01）+ MIDP `obj/arm/libobj.a`
  （145 成员）→ midp-vita 式 CMake 链接 → 类从 VPK 内 `midp_system.jar` 文件系统加载
- 旧 8.2MB `libcldc_vm_midp_g.a` 已不存在；`USE_ROM=true`/ROMImage 路线当前未启用
  （`build/vita_arm/ROMImage_*.cpp` 8-30 00:02 生成过，但未链入现行库）

---

## 项目概述

**目标**: 在 PS Vita 上运行 J2ME/MIDP 应用（Java ME）

**核心组件**:
- phoneME CLDC - Java VM 实现
- phoneME MIDP - MIDP 2.0 实现  
- Vita SDK 交叉编译工具链

**工作目录**: `/home/zyb/vitasdk/samples/j2me/`

---

## 项目结构

```
j2me/
├── phoneme-cldc/          # Java VM (CLDC 1.1)
│   ├── src/vm/
│   │   ├── share/handles/Universe.cpp    # HiddenPackage bypass
│   │   ├── share/ROM/                    # ROM 生成相关
│   │   └── os/vita/                       # Vita 原生代码
│   └── build/vita_arm/
│       ├── dist/lib/libcldc_vm_g.a         # 编译好的 VM 库
│       ├── dist/lib/cldc_classes.zip      # CLDC 类
│       └── romgen/generated/              # ROMImage_*.cpp
├── phoneme-midp/          # MIDP 2.0 实现
│   └── build/vita_arm/
│       └── classes/       # 编译好的 MIDP 类
│           ├── com/
│           ├── java/
│           └── javax/
└── midp-vita/             # Vita 启动器
    ├── src/main.c         # 入口点
    ├── build/             # VPK 输出
    └── build_midp_vita.sh # 构建脚本
```

---

## 技术细节

### 工具链
- 编译器: `arm-vita-eabi-gcc`
- 构建系统: CMake + 自定义 Makefile
- ROM 生成: `romgen` (32-bit x86 Linux binary)
- 环境: `/home/zyb/.local/vitasdk`

### ROM 生成流程
```
cldc_classes.zip + MIDP classes (com/, java/, javax/) + midp_system.jar + Hello.jar
    → min_rom.jar (合并，包含所有系统类)
    → dist/bin/romgen
    → ROMImage_*.cpp (19,567 objects, ~1.2MB)
    → libcldc_vm_g.a (链接，UseROM=true)
```

### UseROM 标志
- **关键**: `UseROM = true` 从 ROMImage 加载系统类，不再需要文件系统上的 midp_system.jar
- 配置位置: `phoneme-cldc/build/vita_arm/vita_arm.cfg`
- 启用条件: `ENABLE_ROM_GENERATOR = true`, `ROMIZING = 1`

### VPK 打包路径
```
data/J2ME00001/
├── midp_system.jar      # (嵌入 ROM，不再需要)
├── Hello.jar            # 用户 MIDlet
└── lib/
    ├── internal.config  # MIDP 内部配置
    └── system.config    # MIDP 系统配置
```

### 类加载路径
- ROM 包含: 所有 CLDC + MIDP + midp_system.jar + Hello.jar 类
- VM 启动: 从 ROM 加载 `com.sun.midp.main.MIDletSuiteLoader`
- 不再需要从文件系统加载

---

## 已解决问题 ✅

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| HiddenPackage error | JVM class 未标记 hidden | 修改 `Universe.cpp` 添加 VITA bypass |
| NoClassDefFoundError | midp_system.jar 未打包 | 嵌入 ROM |
| 多 JAR classpath | `:` 分隔符问题 | 修改 `getClassPathPlus()` |
| ROM skeleton | 未生成真实 ROM | 实现 romgen 集成 |
| 构建脚本混乱 | 单文件 | 拆分为模块化 |
| `app0:/u/` 路径错误 | `UseROM = false` 导致从文件系统加载 | 启用 `UseROM = true`，ROM 包含所有类 |
| ClassFormatError:154 | 用户 jar 未做 CLDC preverify | `build_jar.sh` 加 preverify 步骤（javac → preverify → jar） |
| IAE "does not support title" | `lfjport_get_display_capabilities` 返回 0；`lfjport_get_display_device_ids` 返回 NULL 但 KNI 会解引用 | `lfjport_fb_export.c` 返回 255（全能力位）；ids 返回静态数组 `{0}` |
| **VPK 内 ELF 陈旧（本次白屏反复的元凶）** | `libobj_no_main.a` 只经 `target_link_options` 传入，CMake 未注册文件级链接依赖，归档更新后 make 判定无需重链；`vpk` 目标链只依赖 `.self/.sfo` 也探测不到 | CMakeLists 给 midp_vita 加 `LINK_DEPENDS "${LIBOBJ_NO_MAIN}"`；**每次构建后必须 `arm-vita-eabi-objdump` 验证 ELF 内实际代码**。教训：改完源码后 ELF mtime 不变 = 链接没发生，产物不可信 |

---

## 当前状态 🔄

### 构建成功
- VPK: `midp-vita/build/midp_vita.vpk` (2.6MB)
- ROM: 19,567 objects, ~1.2MB (包含 midp_system.jar + Hello.jar)
- UseROM: enabled

### 待解决问题
1. **运行时测试** - 需要在 Vita3K 上验证

### 遗留事项
- [ ] 在 Vita3K 上验证运行时
- [ ] 验证 VPK 在真机上运行
- [ ] 实现更多 MIDlet 测试
- [ ] 优化 ROM 大小

---

## 构建命令

```bash
cd /home/zyb/vitasdk/samples/j2me/midp-vita

# 完整构建
./build_midp_vita.sh

# 清理后重建
./build_midp_vita.sh clean

# 仅构建 Hello.jar
bash build_jar.sh
```

---

## 关键文件修改记录

### Universe.cpp
```c
#if defined(VITA) || defined(VITA_SDK)
    if (!jvm_class()->access_flags().is_hidden()) {
        fprintf(stderr, "[VITA] Warning: JVM class not marked as hidden, skipping check\n");
    }
#else
    if (!jvm_class()->access_flags().is_hidden()) {
        JVM_FATAL(jvm_class_must_be_hidden);
        return false;
    }
#endif
```

### main.c
```c
char *midp_args[] = {
    "runMidlet",
    "-classpathext",
    "ux0:/data/J2ME00001/midp_system.jar:ux0:/data/J2ME00001/Hello.jar",
    "ux0:/data/J2ME00001/Hello.jar",
    "HelloMIDlet"
};
```

### midp_run.c (getClassPathPlus)
支持 `:` 分隔的多个 JAR 路径

### vita_arm.cfg (启用 UseROM)
```makefile
# Enable ROM (loads system classes from ROMImage.o, embedded in VPK)
# This eliminates the need for midp_system.jar on filesystem
export ENABLE_ROM_GENERATOR := true
export ENABLE_ROM_GENERATOR__BY := vita_arm.cfg
export ROMIZING := 1
export USE_ROM := 1
```

### build_midp_vita.step1_5_rom
ROM jar 现在包含:
- CLDC classes
- MIDP classes (com/, java/, javax/)
- midp_system.jar 内容
- Hello.jar 内容

---

## 调试日志

Vita3K 日志位置:
- `C:/Users/zyb/AppData/Roaming/Vita3K/Vita3K/ux0/data/midp_debug.log`
- `C:/Users/zyb/AppData/Roaming/Vita3K/Vita3K/ux0/data/debug_log.txt`

---

## 参考资料

- phoneME CLDC: `src/vm/share/ROM/` 目录
- MIDP 构建: `phoneme-midp/build/vita_arm/GNUmakefile`
- romgen 配置: `dist/lib/cldc_rom.cfg`
- UseROM 配置: `src/vm/share/utilities/Globals.hpp`
- ROM 初始化: `src/vm/share/ROM/ROM.cpp`
- **8-30 第二轮修复（用户反馈"只有 Exit"+ 截图分析）**:
  1) 缩放公式反了：旧代码把 240x320 拉伸成 720x544（截图白区宽~720 证实），改为等比
     fit: scale=min(PW/VW,PH/VH)，竖屏 407x543 / 横屏 726x544 居中。
  2) **Java 堆仅 1.25MB（Form 空白根因）**: `getInternalPropertyInt("JAVA_HEAP_SIZE")`
     在复制来的 vita_pcsl.c stub 里返回 0 → 默认 1280KB，装载 183 张皮肤图 OOM
     （大图失败→标题/正文不画，softbar 小图幸存；GC 风暴→3FPS）。stub 现返回 32MB。
  3) **方向**: Vita 横持，默认虚拟屏改为 320x240 landscape；launch.cfg 第三行
     portrait|landscape 可切（vita_display_set_orientation 须在 VM 启动前调）。
     internal.config 与 internal.landscape.config 两份随 VPK 打包，启动器按方向拷贝。
  待确认: 用户重测 Form 是否显示、FPS 是否恢复。
- **8-30 第三轮修复（闪退）**: JAVA_HEAP_SIZE=32MB 后 VM 报 "Could not allocate VM heap"
  直接退出。根因: CLDC 堆走 jvm_malloc→newlib，vitaSDK newlib malloc 池默认仅 32MB
  （sbrk.c 的 `_newlib_heap_size`），32MB Java 堆塞不进。修复: vita_main.c 定义
  `unsigned int _newlib_heap_size_user = 64MB`（覆盖弱符号，app 分区 MEMSIZE=128MB 内）。
  链接验证: nm 显示 _newlib_heap_size_user 已从 w 变为 D 81251f48。
  经验: Vita3K 日志 tty 输出（vm_output.log: "Could not allocate VM heap"）定位此类问题最快。
- **8-30 第四轮（用户要求）**: 默认方向改回竖屏 240x320（J2ME 游戏主流为竖屏设计，
  320x240 默认会让竖屏游戏布局错乱）。vita_main.c 默认 orient="portrait"，
  VITA_DEFAULT_LANDSCAPE=0。landscape 仍可经 launch.cfg 第三行选择。
- **8-30 第五轮（日志确诊，Form 空白真根因）**: vm_output.log 抛
  `IllegalArgumentException: This display does not support title` @ Display.setCurrent(bci=45)
  → startApp 失败，Form 从未真正显示（白屏+Exit 只是显示系统初始层）。
  根因: 我写的 lfjport_get_display_capabilities() 返回 0 = 什么都不支持。
  修复: 返回 0x3FF（DisplayDevice.java 位: 1=input,2=commands,4=forms,8=ticker,
  16=title,32=alerts,64=lists,128=textboxes,256=tabbedpanes,512=fileselectors）。
  教训: 参考 lfjport_fb_export.c 复制来的"返回 0"stub 不是无害默认值，是硬故障源。
  另: debug_log.txt 显示 SkinResources 会先找 ux0:/data/J2ME00001/lib/*.png 再回退 jar
  （lib 无自定义皮肤属正常回退路径）。
- **🎉 8-30 里程碑: 首次完整运行成功**。Form 标题/正文/Exit 全部正常显示（竖屏 240x320）。
  渲染链+高级UI+皮肤+字体+32MB堆+能力位修复全部验证通过。
  待验证: InputTest（按键链）、CanvasTest（drawImage/PNG）、真实游戏。
