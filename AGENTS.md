# J2ME 移植项目守则（所有 AI 工具必读）

完整规则见上级目录 `../../AGENTS.md`，以下是不可违反的底线：

1. **`phoneme_source/` 只读**。它是 phoneME Archive 上游参考树（git 仓库），用于查阅机制与原始构建流程。禁止修改，禁止把它的文件复制进工作副本再改动。
2. **判断"改过什么"只用 git**：`phoneme_source` 与 `phoneme-cldc` 是不同快照，目录级 diff 不能当改动清单。在 `phoneme-cldc` / `phoneme-midp` 工作副本内用 `git status` / `git diff` 确认真实改动；当前规模：cldc 约 103 文件、midp 约 15 文件——不要显著放大它。
3. **最小 diff**：优先新增文件（`src/vm/os/vita/`、`src/anilib/vita/`、`build/vita_arm/`）；必须改共享文件时用最小补丁 + `#if defined(VITA)` 隔离。禁止重排格式、改尾随空格、"顺手优化"无关代码。构建产物（classes/、tmpclasses/、dist/）和 .bak 不入库。
4. **构建体系**：新平台 = 新增 `cldc/build/<config>/` 下 Makefile + `<config>.cfg` 两个文件（参考上游 `linux_arm/` 模板）；`build/share/` 的 root.make/jvm.make 骨架不改，平台开关在 cfg 里导出。MIDP 侧同理（GNUmakefile + *.gmk）。
5. **改前改后留痕**：改前 `git diff -- <file>` 看现状，改后记录到 `PROJECT_MEMORY.md` 的"关键文件修改记录"。
6. **验证**：用 `midp-vita/build_midp_vita.sh`（`clean` 参数全量重建）验证；如实报告结果。
