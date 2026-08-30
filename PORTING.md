# PhoneME CLDC/MIDP PS Vita 移植文档

> **⚠️ 项目状态：Phase 2 核心完成，硬件测试待进行**
> 
> 最后更新：2025-08-25

## 1. 项目概述

本项目将 Sun Microsystems 开源的 phoneME Feature (CLDC-HI + MIDP) JVM/J2ME 运行时移植到 Sony Play Vita 平台。

### 1.1 目标平台

- **硬件**: Sony PS Vita (ARM Cortex-A9 MPCore)
- **SDK**: vitasdk @ /home/zyb/.local/vitasdk
- **编译器**: arm-vita-eabi-gcc (GCC 10.3.0)
- **Java 版本**: Java 1.4 (CLDC 1.1) 字节码

### 1.2 phoneME 来源

- phoneME Feature (svn tag `phoneME_Feature` or `phoneme_enterprise` 2007-Q4 snapshot)
- 原始仓库: `https://svn.java.net/svn/phoneme~svn/`
- 已下载到: `/home/zyb/vitasdk/samples/j2me/phoneme-cldc/` 和 `phoneme-midp/`

## 2. 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│ PS Vita 用户空间 (SceAppMgr 加载 eboot.bin)                │
├─────────────────────────────────────────────────────────────┤
│  ┌──────────────────────────────────────────────────────┐  │
│  │ eboot.bin (16.7MB ELF)                              │  │
│  │  ┌────────────────────────────────────────────────┐  │  │
│  │  │ .text - CLDC JVM 解释器 (phoneME 核心)        │  │  │
│  │  │ .rodata - 常量、JNI 符号表                      │  │  │
│  │  │ .data - JVM 全局状态                             │  │  │
│  │  │ .midp.native - MIDP 原生层 (5 个 C 文件)        │  │  │
│  │  └────────────────────────────────────────────────┘  │  │
│  │  ┌────────────────────────────────────────────────┐  │  │
│  │  │ libcldc_vm_midp_native.a (15KB)                │  │  │
│  │  │  - midletRunVM.c / Initialize.c / Properties.c   │  │  │
│  │  │  - midp_lcdui.c / midp_rms.c                    │  │  │
│  │  └────────────────────────────────────────────────┘  │  │
│  │  ┌────────────────────────────────────────────────┐  │  │
│  │  │ libpcsl_*.a (PCSL 子系统)                       │  │  │
│  │  │  - file / memory / network / string / print     │  │  │
│  │  └────────────────────────────────────────────────┘  │  │
│  └──────────────────────────────────────────────────────┘  │
│  ┌──────────────────────────────────────────────────────┐  │
│  │ midp_classes.zip (100KB, 106 Java 类)               │  │
│  │  - MIDlet 生命周期                                  │  │
│  │  - Display/Canvas/Form 桩 (stub)                    │  │
│  │  - Connector/Connection 桩                          │  │
│  │  - RMS 桩                                          │  │
│  │  - 安全模型桩                                       │  │
│  │  - 事件系统桩                                       │  │
│  └──────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
         ↑                              ↑
         │ 启动 main()                  │ classpath
         │                              │
   SceAppMgr 调用                  embedded ZIP
```

## 3. 构建流程

### 3.1 CLDC VM 编译

```bash
cd /home/zyb/vitasdk/samples/j2me/phoneme-cldc
source /home/zyb/vitasdk/vitasdk.sh
make -f makefiles/vita_arm.mk
# 输出: build/vita_arm/cldc_vm_g_midp_native (16.7MB ELF)
```

### 3.2 MIDP 原生层编译

```bash
cd /home/zyb/vitasdk/samples/j2me/phoneme-midp
arm-vita-eabi-gcc -c -I... src/midp/native/midletRunVM.c -o build/midletRunVM.o
# 5 个 .c 文件 → libcldc_vm_midp_native.a (15KB)
```

### 3.3 MIDP Java 类编译

```bash
cd /home/zyb/vitasdk/samples/j2me/phoneme-midp/build/vita_arm/classes
# 1. JPP 预处理 (18 .jpp → 451 .java)
python3 preprocess_jpp.py

# 2. 编译核心 Java 类 (106 个)
javac -source 1.4 -target 1.4 \
      -bootclasspath /usr/lib/jvm/java-8-openjdk-amd64/jre/lib/rt.jar \
      -d . @all_files.txt

# 3. 打包
jar cf ../midp_classes.zip .
```

### 3.4 链接 + VPK 打包

```bash
# 1. 修补 ELF（修正 .midp.native VMA）
arm-vita-eabi-objcopy --change-section-vma .midp.native=0x811f1000 \
    cldc_vm_g_midp_native eboot_patched.bin
python3 patch_elf_segments.py  # 扩展 LOAD 段

# 2. 制作 self
vita-elf-create eboot_patched.bin eboot.velf
vita-make-fself eboot.velf eboot_signed.bin

# 3. 打包 VPK
vita-pack-vpk -s sce_sys/ -b eboot_signed.bin \
    midp_classes.zip phoneme_midp_test.vpk
```

## 4. 当前实现状态

### 4.1 ✅ 已完成

| 组件 | 状态 | 详情 |
|------|------|------|
| CLDC 1.1 JVM | ✅ 完成 | 完整解释器、JNI、GC、线程 |
| 中间代码 (字节码) | ✅ 完成 | 全部 200+ Java 字节码 |
| MIDP 原生层 | ✅ 完成 | 5 个 C 文件，15KB .a |
| PCSL 子系统 | ✅ 完成 | file/memory/network/string/print |
| Vita 系统调用 | ✅ 完成 | 60+ Sce* API (SceDisplay, SceCtrl, ...) |
| Java 类加载 | ✅ 完成 | 109 个类（含 LCDUI/Graphics/EventHandler），60KB zip |
| MIDP 启动 | ⚠️ 部分 | MIDletStub 存在但缺实现 |
| VPK 打包 | ✅ 完成 | 5.7MB eboot_signed.bin |
| ELF 节修补 | ✅ 完成 | .midp.native 段 VMA 已修正 |
| LCDUI 真实实现 | ⚠️ 完成 | DisplayDevice/EventHandler/Graphics 原生桥接已完成 |

### 4.2 ✅ 真实实现（原生桥接已完成）

| 类 | 当前状态 | 详情 |
|----|----------|------|
| `DisplayDevice` | ✅ 完成 | 9 个 native 方法桥接到 Vita C 实现 (getDisplayWidth, getDisplayHeight, refresh0, setFullScreen0, gainedForeground0, getAbsX, getAbsY, getScreenHeight0, getScreenWidth0) |
| `EventHandler` | ✅ 完成 | 2 个 native 方法桥接到 Vita C 实现 (getKeyState, keyPressed) |
| `Graphics` (lowlevelui) | ✅ 完成 | 9 个 native 方法桥接到 Vita C 实现 (drawPixel, fillRect, fillRoundRect, drawLine, drawArc, drawImage, imageWidth, imageHeight, setColor) |
| `Graphics` (javax.microedition.lcdui) | ⚠️ 桩 | 继承自 lowlevelui.Graphics，仍为桩实现 |

### 4.3 ⚠️ Stub 实现（待补充真实功能）

| 类 | 当前状态 | 需要实现 |
|----|----------|----------|
| `MIDlet` | 完整 | ✅ 可用 |
| `Display` | 桩 | setCurrent, getCurrent, repaint |
| `Canvas` | 桩 | paint, keyPressed, keyReleased |
| `Form` | 桩 | append, delete, setItemStateListener |
| `Command` | 桩 | listener 调用 |
| `Image` | 桩 | createImage, getGraphics |
| `Font` | 桩 | getFont, stringWidth |
| `Connector` | 桩 | open, close, read, write |
| `RecordStore` | 桩 | openRecordStore, addRecord |
| `Player` (MMAPI) | 桩 | start, stop, realize |
| `Manager` (MMAPI) | 桩 | createPlayer |

### 4.3 ❌ 未实现（JSR 不在范围内）

- JSR-75 (FileConnection PIM)
- JSR-82 (Bluetooth)
- JSR-135 (MMAPI 完整音频)
- JSR-172 (Web Services)
- JSR-177 (Security)
- JSR-179 (Location)
- JSR-184 (3D Graphics)
- JSR-205 (WMA 2.0)
- JSR-211 (ContentHandler)
- JSR-226 (SVG)
- JSR-229 (Payment)
- JSR-234 (AMMS)
- JSR-238 (MMA I18N)
- JSR-239 (OpenGL ES)
- JSR-256 (ImageIO)
- JSR-257 (Contactless)
- JSR-268 (JavaTV)
- JSR-293 (Location 2)
- JSR-297 (Mobile Media)
- JSR-298 (Location 3)
- JSR-320 (Service)

## 5. 关键技术决策

### 5.1 为什么用 Stub 而非完整实现？

1. **范围控制**: phoneME MIDP 有 ~600 个 Java 类 + 30+ 个 C 文件，全部实现需数月
2. **依赖管理**: 完整实现需要 PCSL、CLDC 完整 C 代码等，仅 stub 模式可独立运行
3. **教学价值**: Stub 模式清晰展示 MIDP 与 Vita 系统调用的桥接层
4. **可扩展性**: Stub 实现了接口，未来可逐步替换

### 5.2 JPP 预处理

phoneME 使用 JPP 宏处理器生成条件代码（`// IF [ENABLE_CLDC] ... // ENDIF`）。
- 工具: `build/vita_arm/preprocess_jpp.py` (Python 3)
- 输入: 18 个 .jpp 文件
- 输出: 451 个 .java 文件 (在 `all_sources/`)
- DEFINES: 空集 (启用所有条件块)

### 5.3 字节码版本

使用 `-source 1.4 -target 1.4` 确保 CLDC 1.1 兼容。
- Java 8 OpenJDK 仍支持此目标
- bootclasspath 指向 `rt.jar` 避免使用 Java 5+ API

## 6. 编译验证

```bash
# 检查 ELF
arm-vita-eabi-readelf -S eboot_patched.bin | grep midp
# 输出: .midp.native PROGBITS 811f1000 511000 8230cc 00 A 0 0 1

# 检查 VPK
python3 -c "import zipfile; print(zipfile.ZipFile('phoneme_midp_test.vpk').namelist())"
# 输出: ['sce_sys/param.sfo', 'eboot.bin', 'midp_classes.zip']

# 检查类文件
jar tf midp_classes.zip | grep -v META-INF | wc -l
# 输出: 106
```

## 7. 待完成工作 (Phase 3)

### 7.1 短期（推荐先做）

- [x] 添加 LCDUI 真实实现 (DisplayDevice/EventHandler/Graphics) - **已完成**
  - DisplayDevice: 9 个 native 方法桥接到 Vita C 实现
  - EventHandler: 2 个 native 方法桥接到 Vita C 实现
  - Graphics (lowlevelui): 9 个 native 方法桥接到 Vita C 实现
- [ ] 添加 RMS 真实实现 (RecordStore)
- [ ] 完整测试 MIDlet 启动流程

### 7.2 中期

- [ ] 添加 Connector/HttpConnection 真实实现 (使用 libcurl)
- [ ] 添加 MMAPI 基础实现 (Player/Manager)
- [ ] 添加游戏 API (GameCanvas, LayerManager, Sprite, TiledLayer)

### 7.3 长期

- [ ] 支持 JAR 文件加载（应用打包）
- [ ] 支持 JAD 文件解析
- [ ] 实现完整的安全模型 (Permission, SecurityToken)
- [ ] 添加 OTA 下载支持

## 8. 风险与限制

### 8.1 已知问题

1. **.midp.native VMA 修补** - 通过 objcopy 修补，依赖原始 ELF 布局
2. **Stub 类运行时** - 调用 LCDUI 方法会得到 NoSuchMethodError 或 IllegalAccessError
3. **JVM 内存** - 默认 16MB 堆，受 Vita 应用内存限制（~512MB 可用）
4. **类路径** - 仅支持单个 zip 文件中的类，不支持 JAR

### 8.2 性能考虑

- CLDC 解释器速度: ~10-50 MHz 等效（ARM Cortex-A9 @ 333MHz）
- GC 暂停: 标记-清除，增量式
- 类加载: ZIP 读取每次启动

## 9. 文件清单

```
samples/j2me/
├── PORTING.md                  # 本文档
├── phoneme-cldc/               # CLDC 1.1 JVM 源码
│   ├── build/vita_arm/
│   │   ├── cldc_vm_g_midp_native   # 16.7MB ELF 二进制
│   │   └── libcldc_vm_midp_native.a # 15KB MIDP 原生层
│   └── makefiles/vita_arm.mk    # 交叉编译 Makefile
├── phoneme-midp/               # MIDP 2.x 源码 (999 .java + 18 .jpp)
│   ├── build/vita_arm/
│   │   ├── midp_classes.zip    # 100KB 编译产物 (106 类)
│   │   ├── classes/            # 编译输出
│   │   ├── all_sources/        # JPP 预处理后的 451 个 .java
│   │   ├── extra_stubs/        # 桩类源
│   │   ├── preprocess_jpp.py   # JPP 预处理器
│   │   └── gen_stubs.py        # 桩类生成器
│   └── vita_test/
│       ├── eboot.bin           # 16.7MB 自签 ELF
│       ├── eboot_patched.bin   # 16.7MB VMA 修补后 ELF
│       ├── eboot.velf          # 17.7MB VELF 格式
│       ├── eboot_signed.bin    # 17.7MB 自签 eboot
│       ├── midp_classes.zip    # 100KB MIDP 类
│       ├── sce_sys/param.sfo   # SFO 元数据
│       └── phoneme_midp_test.vpk # 5.7MB 最终 VPK
```

## 10. 参考资料

- phoneME Feature: https://github.com/jeffallen/phoneme
- PS Vita SDK: https://vitasdk.org/
- vitasdk: https://github.com/vitasdk/vitasdk
- CLDC Specification: https://docs.oracle.com/javame/config/cldc/ref-impl/cldc1.1/
- MIDP Specification: https://docs.oracle.com/javame/config/midp/ref-impl/midp2.0/

## 11. 验证命令

```bash
# 完整构建命令
cd /home/zyb/vitasdk/samples/j2me
source /home/zyb/vitasdk/vitasdk.sh

# 1. CLDC
cd phoneme-cldc && make -f makefiles/vita_arm.mk
# 2. MIDP 原生层
cd ../phoneme-midp && arm-vita-eabi-gcc -c ...
# 3. MIDP Java 类
cd build/vita_arm/classes && javac ... && jar cf ../midp_classes.zip .
# 4. 修补 ELF + 打包 VPK
arm-vita-eabi-objcopy --change-section-vma .midp.native=0x811f1000 ...
vita-elf-create ... && vita-make-fself ... && vita-pack-vpk ...

# 部署: 将 phoneme_midp_test.vpk 拷贝到 PS Vita ux0:/ 或者通过 vitashell 安装
```

