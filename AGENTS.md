# J2ME 移植项目守则（所有 AI 工具必读）

完整规则见 `.github/agents/jvm-dev.agent.md`（JVM/J2ME 开发专家智能体，含仓库结构、
关键机制、已踩过的坑），以下是不可违反的底线：

1. **`phoneme_source/` 只读**。它是 phoneME Archive 上游参考树（git 仓库），用于查阅机制与原始构建流程。禁止修改，禁止把它的文件复制进工作副本再改动。
2. **判断"改过什么"只用 git**：`phoneme_source` 与 `phoneme-cldc` 是不同快照，目录级 diff 不能当改动清单。在 `phoneme-cldc` / `phoneme-midp` 工作副本内用 `git status` / `git diff` 确认真实改动；当前规模：cldc 约 130 文件、midp 约 136 文件——不要显著放大它。
3. **最小 diff**：优先新增文件（`src/vm/os/vita/`、`src/anilib/vita/`、`build/vita_arm/`）；必须改共享文件时用最小补丁 + `#if defined(VITA)` 隔离。禁止重排格式、改尾随空格、"顺手优化"无关代码。构建产物（classes/、tmpclasses/、dist/）和 .bak 不入库。
4. **构建体系**：新平台 = 新增 `cldc/build/<config>/` 下 Makefile + `<config>.cfg` 两个文件（参考上游 `linux_arm/` 模板）；`build/share/` 的 root.make/jvm.make 骨架不改，平台开关在 cfg 里导出。MIDP 侧同理（GNUmakefile + *.gmk）。
   **允许 overlay（覆盖/替换）**：对生成物或上游文件，优先用"新增文件 + 打包/构建阶段替换"，不改原文件、不改 `build/share/`。
   做法：新增文件放 `src/vm/os/vita/`（或 `build/vita_arm/`），由 `rebuild_vm.sh` 的 `pack_lib`（或平台 cfg/mk）在打包时用新成员替换旧成员；
   符号冲突用 `objcopy --redefine-sym` 改名，不靠链接顺序 + `--allow-multiple-definition`。
   要求：①幂等（重复执行结果不变）；②替换后用 `nm` 校验每个符号恰好定义一次；③overlay 的原因、范围、验证方法写进文件头注释和 `PROJECT_MEMORY.md`；
   ④`.gitignore` 若屏蔽了目录（如 `build/vita_arm/*`）要加白名单，否则新文件不入库；⑤不把 overlay 逻辑放进 `vita-port/CMakeLists.txt`（它只管链接最终 ELF）。
   现有例子：`src/vm/os/vita/vita_fp_overlay.S` + `patch_interp.sh`（v03.11，替换 `Interpreter_arm.o` 的 FP 调用约定）。
5. **改前改后留痕**：改前 `git diff -- <file>` 看现状，改后记录到 `PROJECT_MEMORY.md` 的"关键文件修改记录"。
6. **验证**：用 `vita-port/build.sh`（`clean` 参数全量重建）验证；如实报告结果。

## 智能体 / skill 入口

| 文件 | 用途 |
| --- | --- |
| `BUILD_QUICKSTART.md` | **编译速查**：改了什么跑什么、overlay 做法、已踩的坑（动手前先读） |
| `UPSTREAM_SOURCES.md` | **上游源码去哪找**：PhoneJ2ME 模块仓、基线 commit、官方归档现状、易混淆仓库 |
| `.github/agents/jvm-dev.agent.md` | JVM/J2ME 开发专家智能体（完整规则、机制、踩坑清单） |
| `.github/skills/psp2dmp-analysis/SKILL.md` | psp2core coredump / 卡死冻结日志判读流程 |
| `debug/tools/psp2dmp.py` | 上述 skill 使用的解析器（`list`/`threads`/`base`/`mem`/`elfstr`） |
