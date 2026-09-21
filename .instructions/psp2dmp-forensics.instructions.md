---
name: psp2dmp-forensics
description: PS Vita J2ME 移植（samples/j2me）挂死/崩溃取证。当用户提到 Vita3K 挂死、psp2dmp/coredump 解析、按键卡死、site_ring、watchdog、HANG、冻结、需要符号化 PC/LR/栈回溯、或要分析 debug/logs/logNN 目录时使用。涵盖：psp2dmp 一键解析脚本用法、ASLR slide 求解、寄存器窗口、core 捕获空洞判读、符号源选择（.sym 归档 vs build/midp_vita）、日志链路（stdout vs stderr）、完整收集清单。
---

# PS Vita J2ME 挂死取证（psp2dmp + site ring + 日志链路）

## 一键解析（首选，已固化）

```bash
cd /home/zyb/vitasdk/samples/j2me
source /home/zyb/vitasdk/vitasdk.sh   # nm 在 PATH
python3 debug/tools/parse_psp2dmp.py <psp2dmp 文件> \
    --elf vita-port/out/vpk/midp_vita_v<版本>.sym   # 与 dump 同版本的符号归档
```

脚本自动完成：gzip 检测解压 → ELF32 PT_LOAD/PT_NOTE 解析 → THREAD_REG_INFO
(type=0x1004) 定位 + 寄存器窗口自适应（+0x10 优先，log19/log21 实测；Vita3K
版本相关）→ ASLR slide 自动求解（优先 site_ring.log 首行 `self=` 作 anchor，
按符号名 vita_site_dump/vita_wd_snapshot 就近取页，残差<0x800 容版本漂移；
兜底寄存器+栈区间交）→ nm -n 符号化 PC/LR → 栈回溯（SP 起 512 字，
runtime .text 区间过滤 + 距离拒绝）→ PT_LOAD 覆盖清单（捕获完整性判读）。

参数：`--slide 0xNNN` 手工指定；`--anchor 0xNNN` 手工给 self 地址；
`-v` 看探测细节。

## 判读要点（log21 定案的知识）

1. **core 只含脏匿名页**：PT_LOAD 覆盖清单里没有 .data/.bss 头（0x810xxxxx
   代码段只有零星几页）、Java 堆结构性不可读 → **Thread 字段、调度器计数、
   jvm_fast_globals 一律读不到**，必须靠活进程打点（v01.95 vita_wd_snapshot）。
2. **PC/LR 在 ASM 桩簇**（符号名 L 开头标签，如 L420/L438，或
   invoke_pending_entries/start_lightweight_thread_asm 入口）= 冻结在绿线程
   切换桩，C++ 层 mark 全部打不中——这是 log21 的定案签名。
3. **栈回溯只剩 JVM::start 一帧** = 切到 primordial 栈后没回来，
   全新派发态（fp=0、r1(pending)=NULL 佐证）。
4. slide 每次启动不同（log21=0x44000、log19=0x15000）——绝不能跨 dump 复用。

## 日志链路（排查"日志是空的"先看这个）

- `tty->print*` → JVMSPI_PrintRaw → jvm_printf → **stdout** → midp_stdout.log
- `fprintf(stderr)` → midp_stderr.log；**freopen 前必须 sceIoMkdir 数据目录**
- watchdog 独立线程：HANG 时依次 vita_wd_snapshot（活全局，v01.95 起）→
  vita_site_dump（ring 冻结快照）
- Vita3K 侧数据目录：`C:/Users/zyb/AppData/Roaming/Vita3K/Vita3K/ux0/data/J2ME00001/`

## 完整收集清单（每次复测必须核对）

16 文件：boot_log.txt / crumb.log / watchdog.log / site_ring.log /
ani_mark.log / audio_debug.log / input_debug.log / midp_debug.log /
midp_stderr.log / midp_stdout.log / net_log.txt / runmidlet_debug.log /
vm_boot.log / vm_output.log / vm_stderr.log / *.psp2dmp

## 版本对应（防符号漂移）

每个 VPK 同批归档 `out/vpk/midp_vita_v<版本>.sym`（未 strip 链接产物，
v01.95 起自动生成）。解析旧 dump 必须用同版本 .sym；若旧版本无归档，
用 `--slide` 手工给（从 dump 当时 site_ring.log 的 self−静态 求出）。

## 相关文件

- 解析器：`debug/tools/parse_psp2dmp.py`（旧手工流程参考 parse_core18b.py）
- watchdog：`vita-port/src/vita_watchdog.c`；VM 侧快照：`phoneme-cldc/src/vm/os/vita/OS_vita.cpp`
- 历史定案：`PROJECT_MEMORY.md` 各 v01.xx 章节（log18/19/21 三个指令级定案）
