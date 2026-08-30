# J2ME/MIDP on PS Vita - 构建历程完整记录

> **日期**：2026-08-27
> **最终成功产物**：`phoneme_midp_rebuilt.vpk` (5.8MB)

---

## 1. 三套构建系统的关系

当前 `/home/zyb/vitasdk/samples/j2me/` 下有三套构建系统，分别承担不同角色：

| 系统 | 位置 | 输入 | 输出 | 角色 |
|------|------|------|------|------|
| **phoneme-cldc make** | `phoneme-cldc/build/vita_arm/` | C++ 源 + Java 类 | CLDC 静态库 + cldc_vm.velf | CLDC VM 编译器 |
| **phoneme-midp make** | `phoneme-midp/build/vita_arm/` | MIDP 源 + CLDC 库 | libobj.a + eboot.velf | MIDP 主程序 |
| **midp-vita cmake** | `midp-vita/build/` | midp-vita/src + MIDP 库 | midp_vita.vpk | J2ME 模拟器（失败） |

**正确构建流程**：
```
phoneme-cldc (make)  →  libcldc_vm_midp_g.a
                            ↓
phoneme-midp (make)  →  eboot.velf (17MB) - 包含 CLDC + MIDP
                            ↓
vita-make-fself      →  eboot.self (17MB)
                            ↓
vita-pack-vpk        →  *.vpk (5.8MB)
```

---

## 2. 关键发现（本次会话的突破）

### 2.1 完整 CLDC 库
**`/home/zyb/vitasdk/samples/j2me/phoneme-cldc/build/vita_arm/libcldc_vm_midp_g.a`** (8.2MB, 2026-08-25 13:11)

- ✅ 包含 `jvm_fast_globals` (地址 0x811a3d0c)
- ✅ 包含所有 `JVMSPI_*` 符号
- ✅ **0 个 undefined 符号** - 直接可用
- 内含 17324 个已定义符号

### 2.2 完整 MIDP 主程序
**`/home/zyb/vitasdk/samples/j2me/phoneme-midp/vita_test/eboot.velf`** (17MB, 2026-08-25 17:44)

- 已成功链接 CLDC + MIDP + 全部 native stubs
- 共 23380 个符号
- `vita-make-fself` 直接转换 → `eboot_test.self`

### 2.3 旧 VPK 作为模板
**`/home/zyb/vitasdk/samples/j2me/phoneme-midp/vita_test/phoneme_midp_test.vpk`** (5.5MB, 2026-08-25 20:11)

- 包含正确的 `sce_sys/param.sfo` 和 `midp_classes.zip`
- 用于本次重建的 metadata 模板

---

## 3. 构建问题的根本原因

### 3.1 `libcldc_vm_g.a` 被破坏
**当前状态** (`dist/lib/libcldc_vm_g.a`, 653KB, 2026-08-27 11:11):
- 这是个 **thin archive**，路径引用 `/home/zyb/...` 的旧构建位置
- 缺失 `jvm_fast_globals` 等所有 VM 核心符号
- 原因是之前的 session 把它清空了

**原状态** (应该是 8.2MB `libcldc_vm_midp_g.a`):
- 由 `phoneme-cldc` make 完整生成
- 包含所有 MergedSrc + 中间产物

### 3.2 ARM Linker Segfault (GNU ld 2.34)
- `arm-vita-eabi-ld -r` 在处理 7-8MB 的 relocatable ELF 时崩溃
- 不论是 `cldc_vm_g_rel.o` (7.8MB) 还是 `libcldc_vm_midp_g.a` (8.2MB) 单独使用都触发 SIGSEGV
- 这是工具链已知 bug

### 3.3 中间产物不可用
之前 session 试图通过 `gcc -r` / `ld -r` 合并 6 个 `_MergedSrc*.o` 来重建符号：
- `gcc -r` 输出 2.7MB 但符号数量不对
- `ld -r` 输出 0 字节（segfault 后无错误信息）
- 最终被证明完全走错路 - 直接用 `libcldc_vm_midp_g.a` 即可

---

## 4. 最终成功方案

### 4.1 不重新构建 - 直接打包
**核心洞察**：`phoneme-midp/vita_test/eboot.velf` 已经是完整的可执行文件
(CLDC + MIDP + stubs)，不需要用 `midp-vita/cmake` 重新链接。

```bash
# 步骤 1: velf → self (Vita 专用 ELF 格式)
vita-make-fself eboot.velf eboot.self

# 步骤 2: 用 Python 直接构建 VPK（vita-pack-vpk 有 .sfo 命名 bug）
python3 << 'PY'
import zipfile, os
vpk = 'phoneme_midp_rebuilt.vpk'
with zipfile.ZipFile(vpk, 'w', zipfile.ZIP_STORED) as z:
    z.write('eboot.self', 'eboot.bin')
    z.write('param.sfo', 'sce_sys/param.sfo')
    z.write('midp_classes.zip', 'midp_classes.zip')
PY
```

### 4.2 避坑要点
- ❌ **不要**用 `arm-vita-eabi-ar r` 重新打包 `libcldc_vm_g.a` (会破坏 thin archive)
- ❌ **不要**用 `gcc -r` 合并 `_MergedSrc*.o` (会丢失符号)
- ❌ **不要**在 midp-vita cmake 中链接大库 (会触发 ld segfault)
- ❌ **不要**相信 `vita-pack-vpk` 的 `.sfo file missing` 错误
  (它实际指的是 `sce_sys/param.sfo` 格式)
- ✅ **使用** `python3 zipfile` 绕过 `vita-pack-vpk` 的 bug
- ✅ **复用** 之前成功的 VPK 中的 `param.sfo` 和 `midp_classes.zip`

---

## 5. 三套构建脚本对比

### 5.1 `phoneme-midp/build_vita.sh` (单文件，201 行)
**优点**：
- 完整、独立的端到端构建
- 已成功生成 `eboot.velf` (17MB)
- 包含所有 native stubs

**缺点**：
- 命令行巨长 (200+ 行 make flags)
- 一次性，修改任何源都要全部重跑
- 不能单独更新某个组件

### 5.2 `midp-vita/build_midp_vita.sh` (模块化，当前尝试)
**优点**：
- 分 5 个 step：jar / rom / cldc / midp / vpk
- md5 增量检查
- 彩色输出

**致命问题**：
- 依赖被破坏的 `libcldc_vm_g.a`
- 试图用 `gcc -r` 重新合并 MergedSrc，但失败
- CMake 链接 8MB+ 库触发 ld segfault
- Step1 编译 `_MergedSrc004.cpp` 但 `jvm_fast_globals` 不在那

### 5.3 不使用 midp-vita/cmake 的原因

`midp-vita` 的设计意图是从头开始构建完整的 MIDP 模拟器，包括：
1. 用 cmake 重新编译 MIDP 源文件
2. 链接到 CLDC 库
3. 打包 VPK

但它有以下根本性错误：
1. **依赖被破坏的 libcldc_vm_g.a** - 不可恢复
2. **CMakeLists.txt 假设 `cldc_vm` 是完整库** - 实际只有 653KB 残留
3. **target_link_libraries 加 8.2MB 库就 segfault** - 工具链问题
4. **从 main.c → runMidlet.o → libobj.a 的链接链太长** - 任一环节挂掉

**实际可行的方案**（本次成功）:
- 跳过 midp-vita/cmake 整个 build 流程
- 直接用 `phoneme-midp/vita_test/eboot.velf`（已存在）
- 只执行 `vita-make-fself` + Python zip 打包

---

## 6. 未来重新构建的正确流程

```bash
# 完整重新构建（如果 eboot.velf 失效）
cd /home/zyb/vitasdk/samples/j2me/phoneme-midp
bash build_vita.sh  # 重新生成 eboot.velf

# 重新打包 VPK
cd vita_test
vita-make-fself eboot.velf eboot.self

# Python 打包
python3 << 'PY'
import zipfile
with zipfile.ZipFile('phoneme_midp_new.vpk', 'w', zipfile.ZIP_STORED) as z:
    z.write('eboot.self', 'eboot.bin')
    z.write('param.sfo', 'sce_sys/param.sfo')
    z.write('midp_classes.zip', 'midp_classes.zip')
PY
```

---

## 7. 重要文件清单

### 7.1 不要删除
- `phoneme-cldc/build/vita_arm/libcldc_vm_midp_g.a` (8.2MB) - 完整 CLDC
- `phoneme-midp/vita_test/eboot.velf` (17MB) - 完整 MIDP+CLDC
- `phoneme-midp/vita_test/phoneme_midp_test.vpk` (5.5MB) - 旧 VPK 模板
- `phoneme-midp/vita_test/param.sfo` (57B) - VPK metadata
- `phoneme-midp/vita_test/midp_classes.zip` (88KB) - Java 类

### 7.2 已损坏（避免使用）
- `phoneme-cldc/build/vita_arm/dist/lib/libcldc_vm_g.a` (653KB) - thin archive
- `phoneme-cldc/build/vita_arm/dist/lib/libcldc_vm.a` (3.4MB) - 缺关键符号
- `phoneme-cldc/build/vita_arm/cldc_vm_g_rel.o` (7.8MB) - 单独 ld 会 segfault
- `phoneme-cldc/build/vita_arm/cldc_vm_g.velf` (8.8MB) - 同上

### 7.3 当前 VPK 产物
- `midp-vita/phoneme_midp_rebuilt.vpk` (5.8MB) - **可装入的最终产物**
- `phoneme-midp/vita_test/phoneme_midp_rebuilt.vpk` (5.8MB) - 副本
- `phoneme-midp/vita_test/eboot_test.self` (17MB) - 原始 self
