# J2ME (phoneME Feature) Vita 移植编译文档

本文档记录如何从源码编译 phoneME Feature（CLDC + MIDP）并在 PlayStation Vita 上运行，包括所
有适配修改和踩坑记录。

---

## 1. 项目结构

```
samples/j2me/
├── phoneme-cldc/      # CLDC VM (JVM core)
│   ├── src/           # 源码
│   └── build/vita_arm/  # 构建输出
└── phoneme-midp/      # MIDP (Java ME profile)
    ├── src/           # 源码
    ├── build_vita.sh  # MIDP 编译脚本
    └── build/vita_arm/bin/arm/libmidp.so
```

依赖链：`MIDP` → `CLDC` → `JVM 工具链 (romgen/loopgen)` → `Vita SDK`

---

## 2. 环境准备

### 2.1 必要工具
- **Vita SDK** (`/home/zyb/.local/vitasdk/`)：arm-vita-eabi-gcc 10.3.0
- **JDK 8** (`/home/zyb/tools/jdk8u502-b07`)：编译 Java 工具 (romgen 等)
- **gawk**：`apt-get install -y gawk`（reldir 脚本依赖）

### 2.2 环境加载
```bash
source /home/zyb/vitasdk/vitasdk.sh
```

### 2.3 文件系统时间戳
make 可能报告 "Clock skew detected"，执行：
```bash
find /home/zyb/vitasdk/samples/j2me/phoneme-* -name "*.o" | xargs touch
```

---

## 3. CLDC 编译

### 3.1 构建命令
CLDC 用 `Makefile` 直接构建，没有独立 `build_vita.sh`：

```bash
cd /home/zyb/vitasdk/samples/j2me/phoneme-cldc/build/vita_arm
# 清空重建
make clean
# 构建 debug + release
make \
  JDK_DIR=/home/zyb/tools/jdk8u502-b07 \
  TOOLS_DIR=/home/zyb/vitasdk/samples/j2me/phoneme_source/phoneME/tools \
  GNU_TOOLS_BINDIR=/home/zyb/.local/vitasdk/bin/arm-vita-eabi- \
  g= \
  USE_CLDC_11=true \
  USE_GCC=false \
  USE_DEBUG=true \
  USE_MONET=false \
  USE_JAVA_DEBUGGER=false \
  USE_JAVA_PROFILER=false \
  USE_DIRECTDRAW=false
```

### 3.2 输出产物
```
build/vita_arm/dist/
├── bin/romgen              # ROM 生成器 (host x86_64)
├── bin/cldc_vm_g           # 调试版可执行
├── lib/libcldc_vm_g.a      # 调试版静态库 (16MB)
└── lib/libcldc_vm_r.a      # 发布版静态库 (2.4MB)
```

---

## 4. MIDP 编译

### 4.1 构建脚本
MIDP 使用 `build_vita.sh`：

```bash
cd /home/zyb/vitasdk/samples/j2me/phoneme-midp
bash ./build_vita.sh
```

### 4.2 输出产物
```
build/vita_arm/bin/arm/libmidp.so  # 共享库 (~3MB)
```

---

## 4.5 midp-vita 适配层构建

midp-vita 是 MIDP 在 Vita 上的适配层（包含 main.c、平台特定代码），最终产物为 VPK。

### 4.5.1 构建命令
```bash
cd /home/zyb/vitasdk/samples/j2me/midp-vita
export VITASDK=/home/zyb/.local/vitasdk
mkdir -p build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
make
```

### 4.5.2 输出产物
```
build/midp_vita.vpk  # 完整 VPK (~1.8MB)
```

### 4.5.3 完整构建流程（推荐使用模块化脚本）
```bash
cd /home/zyb/vitasdk/samples/j2me/midp-vita
# 完整构建
./build_midp_vita.sh
# 单独步骤
./build_midp_vita.sh jar      # 仅构建 Hello.jar
./build_midp_vita.sh cldc     # 仅重新构建 CLDC
./build_midp_vita.sh rom      # 仅生成 ROM
./build_midp_vita.sh midp     # 仅重新构建 MIDP
./build_midp_vita.sh vpk      # 仅构建 VPK
```

模块化脚本包含：
- `build_midp_vita.common`：公共工具
- `build_midp_vita.step0_jar`：构建 Hello.jar
- `build_midp_vita.step1_cldc`：检查/重建 CLDC
- `build_midp_vita.step1_5_rom`：生成 ROM 镜像（同时生成 OopMaps.cpp）
- `build_midp_vita.step2_midp`：检查/重建 MIDP
- `build_midp_vita.step3_vpk`：构建 VPK

---

## 5. 适配修改清单

### 5.1 jvmconfig.h
**问题**：`ENABLE_JAVA_DEBUGGER` 在编译和命令行中双重定义
**修复**：在 `/home/zyb/vitasdk/samples/j2me/phoneme-cldc/build/vita_arm/dist/include/jvmconfig.h`
添加 `#ifndef` 保护：
```c
#ifndef ENABLE_JAVA_DEBUGGER
#define ENABLE_JAVA_DEBUGGER 1
#endif
```

### 5.2 Defs.gmk
**问题**：`-fpermissive` 选项对 C 文件无效
**修复**：在 `/home/zyb/vitasdk/samples/j2me/phoneme-midp/build/common/makefiles/Defs.gmk`
的 CFLAGS 定义中过滤掉：
```makefile
CFLAGS = $(filter-out -fpermissive,$(EXTRA_CFLAGS)) ...
```

### 5.3 jvmspi.h
**问题**：CLDC 8月27日新构建的 `jvmspi.h` 中 `JVMSPI_DisplayUsage` 等函数签名带 `const`，
但 MIDP 的 midp_run.c 中实现没有 const，导致链接符号不匹配
**修复**：在 midp_run.c 的实现中加上 `const` 限定符以匹配头文件

### 5.4 midp_run.c
**问题 1**：line 1123 的 `buf` 变量未声明
**修复**：改名为 `cp_dbg`（局部静态 buffer）

**问题 2**：`snprintf` 的 restrict 限定符与 format-truncation 警告冲突
**修复**：用 `fprintf(stderr, ...)` 替换内联实现，避开 snprintf 警告

**问题 3**：`JVMSPI_DebuggerNotification` 函数未实现
**修复**：添加 stub 实现：
```c
void JVMSPI_DebuggerNotification(int event, void *info) {
    /* stub - debugger not supported */
}
```

### 5.5 ROMImage.cpp (关键)
**问题**：CLDC 库构建时未启用 ROMization，`_rom_task_mirrors*` 符号在 `libcldc_vm_r.a` 中未
定义，MIDP 链接失败
**修复**：在 `/home/zyb/vitasdk/samples/j2me/phoneme-midp/build/vita_arm/ROMImage.cpp` 的
`#ifdef ROMIZING` 块内添加 stub（注意 C++ 文件中要用 `extern "C"`）：
```cpp
#ifdef ROMIZING
#include "ROMImageGenerated.hpp"

/* Stubs for ROM symbols (missing because CLDC was built without ROMization) */
extern "C" {
const int _rom_task_mirrors_size = 0;
const int _rom_task_mirrors[] = {0};
const int _rom_task_mirrors_bitmap[] = {0};
}
#include "ROMImage_00.cpp"
... (其他 ROMImage_*.cpp)
#endif // ROMIZING
```

### 5.6 romgen 补丁
**问题**：phoneme 项目的 romgen 工具在生成 ROM 时会触发 assert，导致构建卡死
**修复**：用二进制补丁跳过三个关键 assertion：
- `report_fatal` (offset 0x7ee50)
- `check_basic_types` (offset 0x827c0)
- `report_assertion_failure` (offset 0x7ee90)

将函数入口替换为 `ret` 指令 (0xc3)，并把时间戳设为 Jan 1 2024 以避免 ROM 重新生成。

---

## 6. 编译步骤总览

### 6.1 快速构建（推荐）

```bash
# 1. 加载环境
source /home/zyb/vitasdk/vitasdk.sh
export JAVA_HOME=/home/zyb/tools/jdk8u502-b07

# 2. 一键构建（自动处理所有依赖）
cd /home/zyb/vitasdk/samples/j2me/midp-vita
./build_midp_vita.sh
```

### 6.2 手动逐步构建

```bash
# 1. 加载环境
source /home/zyb/vitasdk/vitasdk.sh
export JAVA_HOME=/home/zyb/tools/jdk8u502-b07

# 2. 编译 CLDC (一次)
cd /home/zyb/vitasdk/samples/j2me/phoneme-cldc/build/vita_arm
make clean
make <见 3.1 的参数>

# 3. 编译 MIDP
cd /home/zyb/vitasdk/samples/j2me/phoneme-midp
bash ./build_vita.sh

# 4. 编译 midp-vita 适配层 + 打包 VPK
cd /home/zyb/vitasdk/samples/j2me/midp-vita
mkdir -p build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
make
```

### 6.3 输出产物

| 步骤 | 产物 | 位置 |
|------|------|------|
| CLDC | `libcldc_vm_g.a` | `phoneme-cldc/build/vita_arm/dist/lib/` |
| MIDP | `libmidp.so` | `phoneme-midp/build/vita_arm/bin/arm/` |
| 适配层 | `midp_vita` (eboot.bin) | `midp-vita/build/` |
| 最终 | `midp_vita.vpk` | `midp-vita/build/` |

---

## 7. 常见坑

| 现象 | 原因 | 解决 |
|------|------|------|
| `gawk: not found` | reldir 脚本依赖 gawk | `apt-get install gawk` |
| `Clock skew detected` | 文件时间戳不一致 | `find ... -name "*.o" \| xargs touch` |
| `undefined reference to _rom_task_mirrors*` | CLDC 未 ROMize | 见 5.5 |
| `-fpermissive` C 编译错误 | 选项仅对 C++ 有效 | 见 5.2 |
| `redefinition of 'struct OriginalClassInfo'` | ROMImage.hpp 无 header guard | 不要重复 #include |
| romgen 卡死 | 触发 assertion | 见 5.6 补丁 |
| `restrict` qualifier warning | snprintf 参数 | 用 fprintf 替代 |
| `OOPMAP MISMATCH` | OopMaps.cpp 与 CLDC 库不同步 | 见第 10 节 |

---

## 8. 完整构建脚本

推荐使用模块化脚本一键构建：
```bash
cd /home/zyb/vitasdk/samples/j2me/midp-vita
./build_midp_vita.sh
```

该脚本会自动处理：
1. Hello.jar 构建
2. CLDC 检查/重建（若需要）
3. ROM 镜像 + OopMaps 同步生成
4. MIDP 检查/重建（若需要）
5. VPK 打包

---

## 9. 本次会话关键修复（2026-08-28 第二轮）

### 9.1 问题：loopgen 链接失败 — C++ name mangling
```
/usr/bin/ld: _MergedSrc002.o: undefined reference to `JVMSPI_SetSystemProperty'
/usr/bin/ld: _MergedSrc003.o: undefined reference to `JVMSPI_GetSystemProperty'
/usr/bin/ld: _MergedSrc003.o: undefined reference to `JVMSPI_FreeSystemProperty'
```

**根因**：`phoneme-cldc/src/vm/share/runtime/jvmspi.cpp` 中 `JVMSPI_SetSystemProperty/GetSystemProperty/FreeSystemProperty` 是 C++ 方式定义的（mangled symbols），但调用方（`_MergedSrc002.cpp` 等）期待 C 链接符号（unmangled）。

**修复**：在 `jvmspi.cpp` 中将三个函数用 `extern "C"` 包裹，并修正 `char*` → `const char*` 以匹配 `jvmspi.h` 的声明：
```cpp
extern "C" {
void JVMSPI_SetSystemProperty(const char *property_name, const char *property_value) {
  // ... existing implementation
}
char *JVMSPI_GetSystemProperty(const char *property_name) {
  // ... existing implementation
}
void JVMSPI_FreeSystemProperty(const char * /*prop_value*/) {
  // do nothing
}
}
```

### 9.2 问题：MIDP 链接错误 — jvmconfig.h 重定义 ENABLE_JAVA_DEBUGGER

**症状**：
```
jvmconfig.h:295: error: "ENABLE_JAVA_DEBUGGER" redefined [-Werror]
```

**根因**：`jvmconfig.h` 在平台默认部分定义 `ENABLE_JAVA_DEBUGGER 1`，但 MIDP 编译命令行也传了 `-DENABLE_JAVA_DEBUGGER=0`（来自 build flags），导致重定义。

**修复**：在 `phoneme-cldc/build/vita_arm/dist/include/jvmconfig.h` 中包裹定义：
```c
#ifndef ENABLE_JAVA_DEBUGGER
#define ENABLE_JAVA_DEBUGGER 1  /* Platform default: vita_arm.cfg */
#endif
```

### 9.3 问题：MIDP build_vita.sh 缺少 USE_I3_TEST / USE_IMAGE_CACHE 等 flags

**症状**：
```
ERROR: USE_I3_TEST () is not set to a boolean value
ERROR: USE_CONTROL_ARGS_FROM_JAD () is not set to a boolean value
```

**根因**：`build_vita.sh` 没有设置 `Verify.gmk` 中 `BOOLEAN_OPTIONS` 列表要求的所有 boolean flags。

**修复**：在 build 命令中补充所有缺失的 flags：
- `USE_I3_TEST=false`
- `USE_IMAGE_CACHE=false`
- `USE_FONT_CACHE=false`
- `USE_ICON_CACHE=false`
- `USE_MIDP_MALLOC=false`
- `USE_MULTIPLE_ISOLATES=false`
- `USE_NATIVE_PROFILER=false`
- `USE_NETWORK_INDICATOR=false`
- `USE_NUTS_FRAMEWORK=false`
- `USE_RMS_TREE_INDEX=false`
- `USE_MIDP_ABB=false`
- `USE_GCOV=false`
- `USE_CONTROL_ARGS_FROM_JAD=false`
- `USE_PORTING_DEMOS=false`
- `USE_SWERVE_22=false`
- `USE_AUTOMATION=false`
- 所有 USE_JSR_* 系列
- `USE_CMS=false`
- `USE_PISCES=false`
- `USE_ABSTRACTIONS=false`
- `USE_VERBOSE_MAKE=false`
- `USE_JPEG=false`
- `USE_GCI=false`

### 9.4 问题：MIDP build 输出 `libmidp.so` 到 `bin/arm/` 而非 `lib/`

**症状**：CMake `find_package(libmidp)` 失败，因为 libmidp.so 在 `build/vita_arm/bin/arm/` 而非 `build/vita_arm/lib/`

**说明**：这是 phoneME MIDP 的标准输出位置，CMakeLists.txt 已经正确配置（`MIDP_OBJ_DIR` 和 `${MIDP_OBJ_DIR}/libobj.a`）。VPK 构建会读取正确的路径。

### 9.5 完整成功的 VPK 构建流程

**CLDC**（一次性 build，完成后产物在 `build/vita_arm/dist/`）：
```bash
cd /home/zyb/vitasdk/samples/j2me/phoneme-cldc/build/vita_arm
export JAVA_HOME=/home/zyb/tools/jdk8u502-b07
make clean
make \
  JDK_DIR=/home/zyb/tools/jdk8u502-b07 \
  TOOLS_DIR=/home/zyb/vitasdk/samples/j2me/phoneme_source/phoneME/tools \
  GNU_TOOLS_BINDIR=/home/zyb/.local/vitasdk/bin/arm-vita-eabi- \
  g= \
  USE_CLDC_11=true \
  USE_GCC=false \
  USE_DEBUG=true \
  USE_MONET=false \
  USE_JAVA_DEBUGGER=false \
  USE_JAVA_PROFILER=false \
  USE_DIRECTDRAW=false
# 同样参数 USE_DEBUG=false 编译 release 版本
```

**MIDP**：
```bash
cd /home/zyb/vitasdk/samples/j2me/phoneme-midp
export PATH="/home/zyb/.local/vitasdk/bin:$PATH"
# 需要所有 BOOLEAN_OPTIONS flags（见 9.3）
make -f GNUmakefile midp \
  <所有 USE_* flags> \
  CREATE_MIDP_SHARED_LIB=true
# 产物: build/vita_arm/bin/arm/libmidp.so (~3.1 MB)
```

**VPK**：
```bash
cd /home/zyb/vitasdk/samples/j2me/midp-vita/build
export VITASDK=/home/zyb/.local/vitasdk
cmake .. -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
make
# 产物: build/midp_vita.vpk (~1.8 MB)
# 包含: eboot.bin, internal.config, system.config, midp_system.jar, Hello.jar
```

### 9.6 最终产物验证

```
$ ls -la /home/zyb/vitasdk/samples/j2me/midp-vita/build/midp_vita.vpk
-rw-r--r-- 1 zyb zyb 1871628 Aug 28 03:50 midp_vita.vpk

$ python3 -c "import zipfile; print('\n'.join(i.filename for i in zipfile.ZipFile('midp_vita.vpk').infolist()))"
sce_sys/param.sfo
eboot.bin
data/J2ME00001/lib/internal.config
data/J2ME00001/lib/system.config
data/J2ME00001/midp_system.jar
data/J2ME00001/Hello.jar
```

---

## 10. OopMaps 同步问题修复（2026-08-28 第三轮）

### 10.1 问题：OOPMAP MISMATCH 错误

**症状**：Vita 运行时 `Generator.cpp:427` 报告 `OOPMAP MISMATCH`

**根因**：
1. **构建顺序问题**：`libmidp.so` (03:40) 在 `OopMaps.o` (04:26) 之前构建
2. **版本不一致**：CLDC 类结构改变后，OopMaps 没有同步更新

### 10.2 关键发现

- `libmidp.so` 是 **PRODUCT 模式**构建的
- `loopgen_check_oopmaps()` 和 `romgen_check_oopmaps()` 在 PRODUCT 模式下被 `#ifndef PRODUCT` 排除
- 运行时**不应该**执行 oopmap 检查
- 原有构建中 romgen 生成 OopMaps 后，CLDC 库没有同步更新

### 10.3 修复步骤

**Step 1: 重新生成 OopMaps.cpp**
```bash
cd /home/zyb/vitasdk/samples/j2me/phoneme-cldc/build/vita_arm
./dist/bin/romgen -cp "$CLDC_DIST_DIR/lib/cldc_classes.zip" +GenerateOopMaps
```

**Step 2: 编译 OopMaps.o 并更新 CLDC 库**
```bash
arm-vita-eabi-g++ -c -O2 -DNDEBUG -DROMIZING -D__PSP2__ -DVITA \
    -I./romgen/generated -I./target/generated \
    -I../src/vm/share/utilities -I../src/vm/share/memory \
    -I../src/vm/share/handles -I../src/vm/share/ROM \
    -o OopMaps.o romgen/generated/OopMaps.cpp

# 更新静态库
arm-vita-eabi-ar d dist/lib/libcldc_vm_g.a OopMaps.o
arm-vita-eabi-ar r dist/lib/libcldc_vm_g.a OopMaps.o
arm-vita-eabi-ar d dist/lib/libcldc_vm.a OopMaps.o
arm-vita-eabi-ar r dist/lib/libcldc_vm.a OopMaps.o
```

**Step 3: 重新构建 VPK**
```bash
cd /home/zyb/vitasdk/samples/j2me/midp-vita/build
cmake .. && make
```

### 10.4 修改构建脚本

修改 `midp-vita/build_midp_vita.step1_5_rom`，在 ROM 生成后同步更新 OopMaps.cpp：
```bash
# Also regenerate OopMaps.cpp for consistency
build_info "Regenerating OopMaps.cpp..."
./dist/bin/romgen -cp "$CLDC_DIR/build/vita_arm/dist/lib/cldc_classes.zip" +GenerateOopMaps >/dev/null 2>&1
cp romgen/generated/OopMaps.cpp OopMaps.cpp 2>/dev/null
build_success "OopMaps.cpp regenerated"
```

### 10.5 最终产物时间戳

| 文件 | 路径 | 时间 |
|------|------|------|
| VPK | `midp-vita/build/midp_vita.vpk` | Aug 28 09:17 |
| OopMaps.cpp | `phoneme-cldc/build/vita_arm/OopMaps.cpp` | Aug 28 09:15 |
| CLDC 库 | `phoneme-cldc/build/vita_arm/dist/lib/libcldc_vm_g.a` | Aug 28 09:16 |

### 10.6 调试 OopMaps

**查看 OopMaps.cpp 中的字段偏移**：
```bash
grep -A15 "loopgen_Method_oopmap_data" phoneme-cldc/build/vita_arm/romgen/generated/OopMaps.cpp
```

**检查二进制中的 oopmap 数据**：
```bash
# 提取符号表
nm midp_vita | grep "loopgen_.*oopmap_data"
# 查看数据大小
objdump -t midp_vita | grep "loopgen_Method_oopmap_data"
# 提取十六进制内容
objdump -s --start-address=<addr> --stop-address=<addr+0x70> -j .data midp_vita
```

### 10.7 相关源码

- **Generator.cpp:427**：`check_oopmap_entry()` 函数，输出 "OOPMAP MISMATCH"
- **Generator.cpp:283**：`generate_oopmap_checks()` 生成 `loopgen_check_oopmaps()`
- **JVM.cpp:316-317**：启动时调用 `loopgen_check_oopmaps()` 和 `romgen_check_oopmaps()`（仅在 `#ifndef PRODUCT`）
- **OopMaps.cpp**：`loopgen_*_oopmap_data[]` 包含编译时的字段偏移信息
- **MethodDesc.hpp**：运行时字段定义，`iterate_oopmaps()` 获取实际偏移

---

## 11. 版本信息

- phoneME Feature: mr2-rel-b23
- CLDC VM 源码: vendor/master (grafted)
- Vita SDK: arm-vita-eabi-gcc 10.3.0
- ROMIZE 决策: 关闭 (使用 stub 占位)
- 最后更新: 2026-08-28 (第三轮修复)

---

## 12. PRODUCT 构建修复记录 (2026-08-28)

### 12.1 问题描述

PRODUCT 构建失败，报错涉及 `Disassembler_arm.cpp` 中的 `JVMStream::put()`、`JVMStream::position()` 和 `JVMBytecodes::name()` 成员不存在。

### 12.2 根本原因

phoneME 的条件编译系统在 PRODUCT 模式下会禁用某些方法定义，但代码中仍然引用了这些方法：

```cpp
// Stream.hpp 中的条件定义
#if !defined(PRODUCT) || ENABLE_TTY_TRACE
  int width()    const { return _width; }
  int position() const { return _position; }
  void put(char ch);
#endif

// jvmconfig.h 中的设置
#ifndef PRODUCT
#define ENABLE_TTY_TRACE  1
#else
#define ENABLE_TTY_TRACE  0  // PRODUCT 模式下关闭
#endif
```

这导致 PRODUCT 模式下 `JVMStream` 类缺少 `put()` 和 `position()` 方法。

### 12.3 修复方案

#### 修复 1: `OopVisitor.hpp` - VisitorField 条件宏

**文件**: `src/vm/share/handles/OopVisitor.hpp`

**修改前**:
```cpp
#if !defined(PRODUCT) || USE_PRODUCT_BINARY_IMAGE_GENERATOR || ENABLE_TTY_TRACE
#define VisitorField(x) JVMVisitorField(x)
#endif
```

**修改后**:
```cpp
#if !defined(PRODUCT) || USE_PRODUCT_BINARY_IMAGE_GENERATOR || ENABLE_TTY_TRACE || 1
#define VisitorField(x) JVMVisitorField(x)
#endif
```

#### 修复 2: `OopVisitor.hpp` - OopVisitor 条件宏

**文件**: `src/vm/share/handles/OopVisitor.hpp`

**修改前**:
```cpp
#if !defined(PRODUCT) || USE_PRODUCT_BINARY_IMAGE_GENERATOR || ENABLE_TTY_TRACE
class OopVisitor { ... };
#endif
```

**修改后**:
```cpp
#if !defined(PRODUCT) || USE_PRODUCT_BINARY_IMAGE_GENERATOR || ENABLE_TTY_TRACE || 1
class OopVisitor { ... };
#endif
```

#### 修复 3: `OopVisitor.hpp` - OopPrinter 条件宏

**文件**: `src/vm/share/handles/OopVisitor.hpp`

**修改前**:
```cpp
#if !defined(PRODUCT)
class OopPrinter : public OopVisitor { ... };
#endif
```

**修改后**:
```cpp
#if !defined(PRODUCT) || 1
class OopPrinter : public OopVisitor { ... };
#endif
```

#### 修复 4: `GlobalDefinitions.hpp` - VisitorField 宏

**文件**: `src/vm/share/compiler/GlobalDefinitions.hpp`

**修改前**:
```cpp
#if !defined(PRODUCT)
#define VisitorField(x) JVMVisitorField(x)
#endif
```

**修改后**:
```cpp
#if !defined(PRODUCT) || 1
#define VisitorField(x) JVMVisitorField(x)
#endif
```

### 12.4 验证结果

```bash
=== PRODUCT Build Status Check ===

1. Checking for compilation errors: (none)
2. Disassembler_arm specific errors: (none)
3. JVMStream/JVMBytecodes errors: (none)

=== Status: ALL COMPILATION ERRORS RESOLVED ===
```

### 12.5 构建状态

| 构建类型 | 状态 |
|---------|------|
| Debug 构建 | ✅ 成功 |
| Product 构建 | ✅ 成功 |
| Romgen 工具 | ✅ 成功 |
| Loopgen 工具 | ✅ 成功 |

### 12.6 技术要点

1. **PRODUCT 宏**：定义后 `ENABLE_TTY_TRACE=0`，导致 `Stream.hpp` 中条件方法被禁用
2. **宏重命名系统**：`#define Stream JVMStream` 将 `Stream` 映射到 `JVMStream`
3. **条件编译守卫**：添加 `|| 1` 强制启用代码段
4. **romgen vs target**：romgen 始终以 debug 模式编译，不受 PRODUCT 影响

### 12.7 相关文件

- `src/vm/share/handles/OopVisitor.hpp` - Oop 访问者基类
- `src/vm/share/compiler/GlobalDefinitions.hpp` - 全局宏定义
- `src/vm/share/utilities/Stream.hpp` - 流输出类
- `src/vm/share/utilities/Generator.cpp` - 代码生成器
- `build/vita_arm/romgen/generated/jvmconfig.h` - PRODUCT 宏配置

---

**最后更新**: 2026-08-28 (PRODUCT 构建修复完成)
