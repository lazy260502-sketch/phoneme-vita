# phoneme-vita — phoneME (CLDC/MIDP) PS Vita 移植

phoneME Feature (CLDC-HI + MIDP 2.0) JVM/J2ME 运行时移植到 Sony PS Vita
(ARM Cortex-A9, VitaSDK / arm-vita-eabi-gcc 10.3.0)。

## 仓库结构

```
├── phoneme-cldc/     # CLDC 1.1 VM 工作副本（gitlink，见下）
├── phoneme-midp/     # MIDP 2.0 工作副本（gitlink，见下）
├── phoneme_source/   # 上游纯净参考树（只读，gitlink → magicus/phoneME）
├── jamvm/            # 备选 JVM 源码树（gitlink → cfriedt/jamvm）
├── vita-port/        # Vita 启动器/菜单/字体/网络/输入（当前主力，产物 midp_vita.vpk）
├── midp-vita/        # 旧版启动器（保留作参考，见 vita-port/PLAN.md）
├── jsr75/            # JSR-75 (FileConnection) 实现
├── patches/          # ★ 工作副本相对上游基线的完整补丁（见下）
├── tools/            # 手工重编辅助脚本
├── docker/           # VitaSDK 构建镜像
├── rebuild_vm.sh     # VM 重编脚本（含完整 PRODUCT 旗标配方）
└── *.md              # 文档（AGENTS.md / BUILD.md / PORTING.md / PROJECT_MEMORY.md …）
```

## phoneme-cldc / phoneme-midp 的获取方式（重要）

`phoneme-cldc` 与 `phoneme-midp` 是 gitlink（子模块指针），但它们指向的
提交 **只存在于本机，未推送到任何远端**。克隆本仓库后请按以下步骤重建：

1. 克隆上游基线：
   - `phoneme-cldc` ← https://github.com/tberthel/phoneme-components-cldc
     基线提交 `aae56e14a7e3`
   - `phoneme-midp` ← https://github.com/tberthel/phoneme-components-midp
     基线提交 `8d44713992fd`
2. 应用本仓库 `patches/` 下的补丁：
   - `git apply patches/phoneme-cldc.patch`（126 文件，+3261/−419）
   - `git apply patches/phoneme-midp.patch`（136 文件，+12817/−58）
3. 应用后工作副本 HEAD 对应本仓库 gitlink 记录的
   `phoneme-cldc = fd915f7`、`phoneme-midp = 6d216ae`。

`phoneme_source/phoneME`（magicus/phoneME @ 5a13f65）与 `jamvm`
（cfriedt/jamvm @ 236f9d8）的 gitlink 目标均在公开远端，可直接
`git submodule update --init` 获取。

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
