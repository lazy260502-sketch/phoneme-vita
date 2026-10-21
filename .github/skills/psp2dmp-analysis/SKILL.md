---
name: psp2dmp-analysis
description: "Use when: 分析 PS Vita psp2core coredump（*.psp2dmp）或 vita3k/真机崩溃冻结日志；判读 THREAD_REG_INFO 寄存器、探测模块加载基址（基址会漂移，不能假设 0x81000000）、内核 PC/r12 syscall 槽位解读；phoneME/J2ME Vita 移植（midp_vita）崩溃取证、卡死退出、白屏、livelock 调试。工具在 debug/tools/psp2dmp.py。"
---

# psp2core coredump 判读（J2ME Vita 移植专用）

## 一句话方法

psp2dmp = gzip → ELF32 LE ARM core note 文件。判读三步：
1. `threads` 看寄存器 + 自动基址探测 + 符号化
2. 内核 PC（0xe0xxxxxx）的线程 → r12 是 syscall 槽号，r0-r3 是参数
3. 参数里的 0x81xxxxxx 指针 → `elfstr`/`mem` 解引用

## 工具用法

```bash
cd <本仓库根>
python3 debug/tools/psp2dmp.py list    <dump.psp2dmp>            # notes+页清单
python3 debug/tools/psp2dmp.py threads <dump.psp2dmp>            # 寄存器+符号化
python3 debug/tools/psp2dmp.py base    <dump.psp2dmp>            # 基址判决
python3 debug/tools/psp2dmp.py mem     <dump.psp2dmp> 815d9558 64   # 读 dump 页
python3 debug/tools/psp2dmp.py elfstr  <dump.psp2dmp> 812e43b8      # 读 ELF 串
```

默认 ELF 为 `vita-port/build/cmake/midp_vita`，`--elf` 可覆盖。

## 已实证的机制（照抄即可）

- **格式**：`gunzip` 后 ELF32 LE；e_phoff@0x1c、e_phnum@0x2a；PT_NOTE(type=4)
按序走（nsz,dsz,ntype @p，name 在 p+12，desc=p+12+align4(nsz)，next=desc+align4(dsz)）；PT_LOAD(type=1) 是 psp2core 追加的故障证据页。
- **寄存器**：THREAD_REG_INFO 头 2×u32 (ver=17, n)，每条目 size+tid+17 个 u32（r0-r12,sp,lr,pc,cpsr），size=0x178=376B，条目间距 376B。
- **线程名**：THREAD_INFO 按 THREAD_REG_INFO 相同顺序排列（名字串拼在一起）。
- **基址会漂移**：log70=0x8101a000、log71=0x81024000。**addr2line 前必须减 delta**。
- **基址探测（锚点法）**：睡眠线程特征 pc∈0xe0xxxxxx + r12=0x9da（DelayThread 槽）+ lr 落 0x81xxxxxx；delta = lr − 0x81011b4b（__wrap_sceKernelDelayThread 内 R_ARM_THM_CALL@0x81011b46 的返回址）。多个睡眠线程应给出同一 delta（互验）。
- **内核 PC 解读**：pc/lr∈0xe0xxxxxx + r12=槽号 → 线程阻塞在系统调用内（cpsr 显示 USR 模式是正常的，dump 快照如此）。已知槽位：0x9da=sceKernelDelayThread、0xb2a=sceIo* 家族、0x9d7=EventFlag 等待。
- **syscall 参数**：r0-r3 即参数。r0 是 0x81xxxxxx 指针时，先减 delta 映射回 ELF vaddr 再读（`elfstr`）。
- **Vita flags**：SCE_O_WRONLY=2、O_CREAT=0x200、O_TRUNC=0x400 → 0x602；mode 0x1ff=0777。
- **psp2core dump 的页**：只 dump 线程栈 + 故障周围页，不含应用 .text/.data 全量。SYSTEM_INFO2 note 很大(~4MB)但不含应用段，别被它吓到。
- **TTY 输出**：TTY_INFO note 或 raw log；我们的 jstack/probe 消息走 `tty->print*` → stdout 链路。
- **cpsr**：掩码 0x1f → 0x10=ARM/0x11=user(错位，正常)。0x070 = 关中断（i+f+a），内核上下文常见。
- **stub 编码**：DelayThread stub 4 字 = [0x00000000, 0x859a24b1, 0x4b675d05, 0xe320f000(nop)]；r12 槽号由内核侧装入，不是 stub 指令产生的——反查 syscall 身份只能靠参数语义。
- **陷阱清单**：
  - PC 落"不可自陷指令"（orr/nop）或落 .rodata → 基址算错了，重跑 `base`
  - FPU/扩展区 0xdeadbeef 填充 → 忽略，别当真值
  - 第 5 个寄存器条目以后是扩展区（75B）——我们只解 17 u32 头部
  - `ar r` 重打包要在目标目录内用 basename（历史坑，与 dump 无关但同源）
  - ring trap / jstack 消息被截断 → 先看 TTY_INFO 是否还有未消费的尾部

## 两个已定案的历史案例（判读模板）

### log70（v02.45 movt 回归）
- 基址 0x8101a000（锚点法，双锚点吻合）
- 主线程 pc = vita_landing_pad+0x148，r2=0x9790 → 每次"线程出生"写 0x9790（movt 丢失，只写低16位）
- 修复：v02.47 补回 `movt r2,#:upper16:vita_v244_ct`

### log71（v02.47 ring trap 首次触发，死在 sceIoOpen）
- 基址 0x81024000
- 主线程 tid=40010003：pc=0xe0007d54（SceLibKernel）、r12=0xb2a（sceIo* 槽）、r0→ELF 串 `"ux0:/data/J2ME00001/ring_trap.log"`、r1=0x602（WRONLY|CREAT|TRUNC）、r2=0x1ff(0777)
- 判决：洪水陷阱真实触发（256 连续 pe==NULL 弹跳环），但 `vita_ring_trap` 的 sceIoOpen 冻死在内核 → "卡死退出"。ring_trap.log 未写出（I/O 未完成）。
- 结论：轻量线程上下文做同步文件 I/O 不安全 → v02.48 方向：trap 只写 BSS 快照+原子标志，watchdog 真线程代写 I/O。

> 更多案例（log72 起的 ring_trap / watchdog / 线程审计演进）见 `PROJECT_MEMORY.md`；
> 各次会话日志在 `debug/log/`（log85..log100 等目录）与 `PROJECT_MEMORY.md` 对照。
