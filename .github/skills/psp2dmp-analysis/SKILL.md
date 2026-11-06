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

**基址优先读 `MODULE_INFO`（每段各自的 delta），锚点法只作后备** —— 见下
"基址判定顺序"。log174 就是这样定案的（锚点法失败但 MODULE_INFO 成功）。

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

## 基址判定顺序（log174 后修订）

0. **先看 dump 文件名与 APP_INFO 的版本号**，确认是不是你想要的那个版本
   （`python3 -c` 解 `APP_INFO` note；`BUILD_VER_INFO` 里那串
   `release_branches/release_03.650` 是 **SceLibc 的版本，不是我们的**）。
1. **`MODULE_INFO` note — 最可靠。** 里面按模块名（如 `midp_vita`）列出段表，
   结构是"每段一条 {type, addr, size, align}"：
   ```
   +0x734: type=5  addr=0x81005000  size=0x3e356c   ← 对应 ELF 段0（RE）
   +0x748: type=6  addr=0x81500000  size=0x6d18e4   ← 对应 ELF 段1（RW）
   +0x6f0: pc0 = 0x810078b5
   ```
   **交叉验证**：`pc0 − <ELF 的 e_entry>` = 段0 的 delta。
   log174：`0x810078b5 − 0x810028b5 = 0x5000` ✓
   **注意各段 delta 可以不同**（log174 段0 = +0x5000、段1 = +0x10000），
   因为 vita-elf-create 对每段独立按 0x1000 对齐。
2. 锚点法（`base` 命令）：需要恰好有线程睡在 `sceKernelDelayThread` stub 里。
   **没有睡眠线程时会 FAIL，这不等于无解** —— 回到第 1 步。
3. 再不行：用 dump 页内容与 ELF 对比（**只对未重定位的 .rodata 有效**，.data
   被 R_ARM_RELATIVE 改写过，比值一定不匹配，别据此否定）。

**判据自检**：符号化后如果 pc/lr 落在不可自洽的位置（函数中间、.rodata、
`_GLOBAL__sub_I_*+0x3f` 这种奇怪偏移），先怀疑 delta。


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
  - **psp2core 里没有"异常类型"字段**（log187 实测）：`SUMMARY_INFO` 是线程状态表
    `{status,size,size}`，`EVENT_LOG_INFO` 是内核模块事件日志，`kernel_forensics.log` 只有模块表。
    `THREAD_REG_INFO +0x170/+0x174` 只在 log174 那种 permission fault 下可读成
    `0x0d` + `FAR==pc`；另一种值（如 log187 的 `0xf5` / 内核段地址）**没有文档可依，不要硬解读**。
  - **故障页里可能没有"出问题的那段代码"**：psp2core 只 dump 线程栈 + 故障周边页。JIT 生成的代码
    在 Java 堆块里（如 `0x85d00000`），若 `list` 的 PT_LOAD 里没有该块，就**不可能**反汇编出
    出错指令 —— 这时候要靠"pc 落在哪个块 + 哪个阶段没往下走"来定案。
  - **分析某次 dump 前先确认本地 ELF 还是那个版本**。`vita-port/build/cmake/midp_vita` 会被下一次
    构建覆盖；版本不符时用 dump 页内容在 ELF 里搜会得到**假匹配**（log187 复盘：曾算出
    `delta=0x10f80`，不是 0x1000 的倍数 ⇒ 假的）。判据：`cmd` 的 `boot_log.txt` 的 `version:` 行。
  - **`midp_vita_v0x.xx.sym` 是 vita-elf-create 的二进制格式**，不是文本 `nm` 输出 ——
    grep/awk/bisect 都用不了，别在它上面浪费时间。
  - **`.psp2dmp.tmp` 说明 dump 没写完**：解压后长度小于 note 解析所需长度时 `psp2dmp.py` 会直接
    `struct.error`，换另一个 dump 或放弃。

## 三个已定案的历史案例（判读模板）

### log174（v04.03 真机启动即崩）：`pc = 0` = 调用未解析的 vitalink import stub

判读路径（**全程不需要日志，只有 dump**）：
- 只有 **2 个线程**（主线程 + SceCommonDialogWorker）、sp 距栈顶仅 0xE0
  ⇒ 崩在 `_start` 里，应用代码一行没跑。
- 两次 dump 的 lr 差恒定 0x22000 ⇒ 确定性 bug + ASLR。
- `base` 失败 → 改读 `MODULE_INFO`：段0 delta = +0x5000、段1 = +0x10000。
- `lr 0x8100766b − 0x5000 = 0x8100266b` = `_GLOBAL__sub_I_...freeres + 0x3f`，
  即 `.init_array[6]`（C++ 静态初始化）。
- 反汇编该处：`movw/movt r0, .LC4` 然后 `blx getenv`；**dump 里的
  r0 = 0x811e32cc，减 delta = 0x811de2cc = `.LC4`** ⇒ lr 就是这条 blx 的返回址。
- `getenv` @ 0x811c3510 是 16 字节 stub（`00000008 / hash / hash / nop`），
  位于 `.vitalink.fstubs.SceLibMonoBridge` ⇒ 无 Sce 模块导出 ⇒ 目标解析为 0
  ⇒ 执行地址 0 ⇒ prefetch abort。

**可复用的判据**：
- `pc == 0` 且只有极少数线程 ⇒ 优先查 `.init_array` / `_start` 里的未解析 import。
- 用 `debug/tools/check_vitalink_stubs.py` 一次列出所有"混进 Sce stub 段的非 Sce
  符号"（白名单 `sce/_sce/pss_/tai`）。
- **修这类问题不要顺手引用另一个未验证符号**：第一版修法遍历 `environ`，
  加了 `extern char **environ;` 之后 `lib_a-environ.o` 不再被拉入，`environ`
  自己变成 `T`（stub）而不是 `D` —— 等于把崩溃往下挪一层。

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
