# 真机日志归档（debug/logs/）

每次真机测试一个子目录，**不混放**（规范见 `../FILE_LAYOUT.md`）。
日志与 psp2core 转储已入库，作为取证证据与代码一起版本化。

## 会话索引

| 会话 | 日期 | 版本（boot_log.txt 的 `version:` 行） | 备注 |
|---|---|---|---|
| `log60` | 2026-10-02 | v02.36 b407 (cac5a7a) | 首次全面日志审计基线段 |
| `log61` | 2026-10-02 | v02.37 b410 (56a4b1e) | |
| `log62` | 2026-10-02 | v02.37 b410 (56a4b1e) | |
| `log64` | 2026-10-03 | v02.38 b415 (2c34bdf) | 全反汇编对照（`../tools/v02.38_full_disasm.txt`） |
| `log65` | 2026-10-03 | v02.38 b415 (2c34bdf) | |
| `log66` | 2026-10-04 | v02.39 b419 (d402aa0) | |
| `log67` | 2026-10-07 | v02.40 b425 (a51f6f4) | livelock 取证 → `../notes/log67_livelock_and_v0241_png_off.md` |
| `log68` | 2026-10-07 | v02.42 b430 (7f71d3d) | qring 取证 → `../notes/log68_*` |
| `log69` | 2026-10-07 | v02.43 b431 (048a30c) | → `../notes/log69_verdict_and_v0244.md` |
| `log70` | 2026-10-08 | v02.46 b438 (c5175f3) | |
| `log71` | 2026-10-08 | v02.47 b441 (788be8a) | |
| `log72` | 2026-10-09 | v02.48 b445 (bb305ee) | |
| `log73` | 2026-10-10 | v02.49 b449 (81d59c9) | → `../notes/v02.49_notes.md` |
| `log74` | 2026-10-11 | v02.50 b452 (4df2961) | → `../notes/v02.50_notes.md` |
| `log75` | 2026-10-11 | v02.51 b455 (5a5b109) | → `../notes/log75_verdict.md`（trip 未跑，door 两次均 GOOD，log74 误判更正） |
| `log76` | 2026-10-12 | v02.52 b459 (505d929) | → `../notes/v02.52_notes.md`（ghost-filter，§8 v02.53 交付） |

> v02.53/v02.53a 的 17 次运行审计见 `../notes/v02.52_notes.md §8` 与仓库
> 提交 `616f89f`/`7204e26`（stdout 在所有运行中均为 0 字节 → `midp_stderr.log`
> 是唯一可信通道）。

## 会话目录内的文件

| 文件 | 来源 | 内容 |
|---|---|---|
| `boot_log.txt` | 真机 `ux0:/data/J2ME00001/` | 启动头：版本/构建号/CPU 亲和/kubridge 预检 |
| `vm_boot.log` | VM | `JVM_Start` 类路径、`preload load_all_in_classpath` 等 |
| `vm_output.log` / `vm_stderr.log` | VM stdout/stderr | tty 输出与 stderr 分流 |
| `midp_stdout.log` / `midp_stderr.log` | MIDP 重定向 | **stderr 为可信通道**（stdout 多次为 0 字节） |
| `watchdog.log` | 看门狗 | `HANG ce=… polls=… tick=…` 判定记录 |
| `ring_trap.log` | ring 取证装置 | 冻结点 `site seq/frozen/self`、slide、地址环 |
| `thread_audit.log` | 线程审计 | Java 线程栈/状态快照 |
| `crumb.log` | 面包屑 | 关键路径打点 |
| `ani_mark.log` / `audio_debug.log` / `input_debug.log` | 子系统 | ANI/音频/输入调试 |
| `kernel_forensics.log` | 内核取证 | 内核态现场 |
| `midp_debug.log` / `runmidlet_debug.log` | MIDP | 调试开关输出 |
| `net_log.txt` | 网络 | socket 层记录 |
| `psp2core-*.psp2dmp` | 真机崩溃转储 | gzip 压缩的 ARM ELF core；解析用 `../tools/psp2dmp.py` |

## 解析工具

`../tools/` 下的 `psp2dmp.py`（coredump）、`threads*.py`（线程）、`slide*.py`
（基址漂移）、`stack*.py`（栈回溯）等。**基址会漂移，不要假设 0x81000000**。
