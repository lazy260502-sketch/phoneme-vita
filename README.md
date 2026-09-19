# phoneme-vita — phoneME (CLDC/MIDP) PS Vita 移植

phoneME Feature (CLDC-HI + MIDP 2.0) JVM/J2ME 运行时移植到 Sony PS Vita
(ARM Cortex-A9, VitaSDK / arm-vita-eabi-gcc 10.3.0)。

## 仓库结构

```
├── phoneme-cldc/     # CLDC 1.1 VM 完整源码（含 Vita 移植，直接入库）
├── phoneme-midp/     # MIDP 2.0 完整源码（含 Vita 移植，直接入库）
├── phoneme_source/   # 上游纯净参考树（只读，gitlink → magicus/phoneME）
├── vita-port/        # Vita 启动器/菜单/字体/网络/输入（当前主力，产物 midp_vita.vpk）
├── jsr75/            # JSR-75 (FileConnection) 实现
├── patches/          # ★ 工作副本相对上游基线的完整补丁（见下）
├── tools/            # 手工重编辅助脚本
├── rebuild_vm.sh     # VM 重编脚本（含完整 PRODUCT 旗标配方）
└── *.md              # 文档（AGENTS.md / BUILD.md / PORTING.md / PROJECT_MEMORY.md …）
```

> 注：本机另有 `jamvm/`（备选 JVM，未集成）、`cldc-vita/`（遗留实验）、
> `docker/`（构建镜像）、`midp-vita/`（旧版启动器，仅作参考）四个目录，
> 均不在构建链路上，未入库。

## phoneme-cldc / phoneme-midp 的来源（源码已直接入库）

`phoneme-cldc/` 与 `phoneme-midp/` 以**完整源码**形式提交在本仓库中，克隆即用，
无需子模块或补丁操作。它们的来源：

- `phoneme-cldc` ← https://github.com/tberthel/phoneme-components-cldc
  基线提交 `aae56e14a7e3`（phoneME Feature mr2-rel-b23）+ Vita 移植改动
  （129 文件，+3470/−419，见 `patches/phoneme-cldc.patch`）
- `phoneme-midp` ← https://github.com/tberthel/phoneme-components-midp
  基线提交 `8d44713992fd` + Vita 移植改动
  （136 文件，+12833/−58，见 `patches/phoneme-midp.patch`）

`patches/` 下的补丁是工作副本相对上游基线的完整 diff，供对照与向上游回提。

`phoneme_source/phoneME`（magicus/phoneME @ 5a13f65）的 gitlink 目标在公开
远端，可直接 `git submodule update --init` 获取。

## 构建

- Vita 启动器（VPK）：`vita-port/build.sh`（产物 `vita-port/build/cmake/midp_vita.vpk`）
- VM：`rebuild_vm.sh`（必须使用其中的完整 PRODUCT 旗标，避免 ABI 混装链接失败）
- MIDP：`phoneme-midp/build_vita.sh`（`USE_JSR_75=true`，JSR_75_DIR 指向本仓库 `jsr75/`）

详细文档：`BUILD.md`（构建）、`PORTING.md`（移植总览）、`VM_BUILD.md`（VM 细节）、
`FILE_LAYOUT.md`（产物/日志位置）、`PROJECT_MEMORY.md`（开发记忆/踩坑记录）。

## 日志与产物（不入库）

- VPK 交付：`out/vpk/`
- 设备日志：`debug/logs/<session>/`（含 psp2core 转储）
- 以上均被 `.gitignore` 排除，不随仓库分发。
