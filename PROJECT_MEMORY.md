# J2ME/MIDP on PS Vita - Project Memory
> Last Updated: 2026-10-09

## 2026-10-09 log72：**v02.48 全链打通 + livelock 环首次完整招供——环不是栈滑动，是 entry-less 派发在 pad↔birth stub 之间打乒乓**

### v02.48 handoff 验证（全按预期）

- `ring_trap.log` 4739B 完整落盘（bounce hist、exit-table、birth-frame、c-stack、调度器 forensics、5 线程符号化 jstack、java-stack-top——一样不缺）；**无 coredump**（watchdog 干净退出，`sceKernelExitProcess(0x45)` 从 OS 线程上下文执行成功）
- watchdog.log 如预期在 `[WD] alive` 处戛然而止——看到 tripped 的那轮 poll 直接跳 dumper 没再记日志（设计如此，不是 watchdog 死了）
- 冻结→落盘延迟 ≤250ms（一轮 poll），远快于 HANG 判定（3s），watchdog 甚至没走到 HANG 分支
- slide=0x12000（三锚点互验：pad 810ad470/exit 810aeefc/uncaught 810ad6d4 全吻合）

### 环的解剖（core 事实）

1. **不是栈滑动**：4 条 hist 全部 `r6=81ddf378 d_r6=+0`——log69 时代的"滑动 gadget"假说就此排除
2. **弹跳边**：lr=810ad470 = **pad 自己**——执行是"解释器返回 → 弹出栈底 sentinel（=pad 地址）→ 落进 pad"的循环；pad 每次 pe==NULL 穿越后尾跳 `start_lightweight_thread_asm`，stub 对无 entry 线程无事可做立即返回 → 又弹 sentinel → **pad↔stub 互跳就是环本体**（~10M/s，256 次在微秒级凑满，调度器根本没再进入——qring 的 SEL 记录停在 trap 前）
3. **被困者 id=4 是新生儿**：jstack `(fp=0, not started)`——一个字节码都没执行过就被派发；r6 指向它栈顶区的真 exit 模板（[r6-4]=pad、[r6+0..12]=exit/uncaught/finish/force_term 完好）
4. **致命共性：q5 三个可运行线程全部 pe=0**——id=2（卡在 `EventQueue.post` bci=0，事件投递者）、id=3（worker，曾在 run bci=8）、id=4（新生儿）。派发 entry-less 线程 = 必然弹跳
5. **冻结时点**：MIDlet 轮刚起步——preload 扫完 21 类（watchdog STORM 行），事件泵 startup 阶段；id=0 主线程在 `MIDletSuiteLoader.startSuite→Object.wait`、id=1 在 `EventQueue.run→Object.wait` 等 id=2 的 post——post 永远完不成，全队饿死
6. 调度序列（qring 尾部倒序）：id=3 SEL×16（id=3 弹跳期，每次回到调度器重选队头）→ id=2 ADD + id=4 ADD（某处唤醒/出生）→ id=2 NRT+SEL → id=4 NRT+SEL → **环闭合**（stub↔pad 紧环，不再回调度器）

### 判决

- **环的充要条件 = 派发了 pe==NULL 的线程**。get_next_runnable_thread 沿优先级链表走，**不检查 entry 就绪性**（Scheduler.cpp:2680-2740 只查 quota/debugger）——链表里有 pe=0 成员就必炸
- 更深的疑点（v02.49 方向）：id=2/id=3 是**有保存上下文的旧线程**（jstack 有帧），被唤醒进 q5 后为何走了"出生路径"而非"恢复路径"？恢复路径不会经过栈底 sentinel。怀疑唤醒者 add_to_active 时没挂 entry、或 NRT 缓存/switch 丢失了 has-saved-context 判定
- v02.49 修复方向（两层）：(a) pad 的 pe==NULL 穿越不该尾跳 birth stub（那等于亲手闭环）——应让出 CPU 回调度器；(b) 根因在唤醒链——找到 add 入队不挂 entry / 误走出生路径的调用点。先做 (b) 的取证（给 ADD 记录加 caller），(a) 是兜底刹车

### 教训入库

- **取证管线至此闭环**：v02.45 设计 trap（抓现场）→ log71 证明轻量线程不能做 I/O → v02.48 handoff → log72 环招供。三轮迭代换来"任何冻结 ≤250ms 出完整现场"
- jstack 的 `.unknown` 类名 = ROM class_from_id 未命中（非崩溃），`??` 是签名原文的 `(`/`[` 被 dumper 过滤
- CYCLE(3) 是**环形双向链表的正常形态**（q5 有 3 个成员），异常的是全员 pe=0——别再把 CYCLE 当链表损坏

## 2026-10-09 v02.48：**trap 去 I/O 化——自旋者停车（wfe），watchdog 代写**（提交 `bb305ee`）

- 动机：log71 证明洪水陷阱能抓住环（256 连续 pe==NULL 弹跳），但 `vita_ring_trap` 第一行 `sceIoOpen` 从轻量线程上下文调用就冻死在内核——轻量线程不是 OS 线程，内核不给它服务 I/O。现场永远落不了盘。
- 设计：trip 路径**纯内存**——现场（r0-r11/sp/lr + parked cookie）存进 `vita_v245_flood[2..13,15]`，`tripped=1` **最后写**（handoff 序），然后 `wfe` 原地停车。watchdog（真 OS 线程，另一核）每轮 poll（250ms）最先检查 `flood[1]`，发现即调用**未改动的** `vita_ring_trap`（写 ring_trap.log + `sceKernelExitProcess(0x45)`）——文件 I/O 与退出都发生在 OS 线程上下文，v01.72 起所有 watchdog.log 已证明安全。
- `flood[15]` = `&vita_ring_parked`（停车点 cookie）：若停车后系统仍整个冻死出 coredump，pc 落 0x8109xxxx 附近且 flood[15] 指向 0x814b7718 即可命名该自旋环。
- 符号（v02.48 build/midp_vita）：`vita_ring_parked` B 0x814b7718、`vita_v245_flood` B 0x814b771c（紧邻，BSS）；`vita_ring_trap_stack_mem`(8KB) 已退役删除。
- 验证：反汇编逐对核查 26 条 movw/movt 成对（log70 教训）、tripped 最后写、自旋环回跳正确；VPK `out/vpk/midp_vita_v02.48.vpk` md5 `db4a7df0915ed0dfe14b40d48a32d908`，版本串 `v02.48 b445 (bb305ee)`。
- 预期下次真机日志形态：ring_trap.log **终于出现**（头部仍是 "RING TRAP v02.45"——dumper 体未改）；watchdog.log 在看到 tripped 的那轮 poll 处戛然而止（直接跳 dumper，不再记日志）。若 ring_trap.log 出现且含 bounce hist + jstack，livelock 环的完整招供（r6 delta 定位滑动 gadget + 各线程 Java 栈）首次落地。
- 遗留风险：(a) wfe 若退化为空转只是更热，不影响正确性（watchdog 在另一核）；(b) 若停车后**整核**调度死亡连 watchdog 都停（log40 的 NEITHER 场景），仍无 ring_trap.log——但那属于方向 B（kubridge 内核侧），本设计已覆盖"轻量线程自旋、内核调度存活"的主分支。

## 2026-10-08 log71：**洪水陷阱首次触发——但 `vita_ring_trap` 死在 trap 自己的 `sceIoOpen` 里**（判读工具化：psp2dmp.py + 技能，提交 `8d3e2a0`）

- 症状：v02.47 正常进入 FileManagerMIDlet、可操作，数分钟后冻结退出，带回 coredump。
- 判决（`psp2dmp.py threads` 一步复现）：基址 0x81024000；主线程 `J2ME00001` pc=0xe0007d54（SceLibKernel 内）、r12=0xb2a（sceIo* 槽位）、**r0 自动解引用 = `"ux0:/data/J2ME00001/ring_trap.log"`**、r1=0x602（WRONLY|CREAT|TRUNC）、r2=0x1ff(0777)、cpsr=0x070（关中断）。其余 4 线程（tone/watchdog/ticker/SceCommonDialogWorker）全部健康睡眠。
- 结论链：v02.45 洪水检测器真实触发了（256 连续 pe==NULL 弹跳环——livelock 复现并被捕获），trap 序列把现场存进 vita_ring_trap 区域后调用 `vita_ring_trap`，但**第一行 sceIoOpen 就冻死在内核**——"卡死退出"的直接原因。ring_trap.log 未写出（I/O 未完成，附件里没有是必然）。
- 意义：(a) v02.47 movt 修复实证有效（能进能玩）；(b) 旧风险"从 JVM 轻量线程上下文调 sceIo 的安全性"得到否定答案——轻量线程非 OS 线程，内核不认它，同步 I/O 即死。
- **v02.48 方向**：trap 去文件 I/O 化——BSS 快照 + 原子 trip 标志（可选 `sceKernelSendSignal` 唤醒 watchdog），由 watchdog 真线程（250ms 节拍）发现标志后代写文件并 `sceKernelExitProcess`。轻量线程侧只做纯内存操作。
- 教训入库：(a) psp2core 的 note name 是**空格填充**不是 \0 填充（"THREAD_INFO" 带 4 个尾随空格，"THREAD_REG_INFO" 恰好 15 字符装下才侥幸匹配）——自写解析器必须 rstrip；(b) 睡眠线程 pc 在**模块内 stub**（pc−delta==0x8110d338）而非内核空间，内核 PC+r12 槽号才是"阻塞在系统调用内"的特征；(c) THREAD_INFO 结构 = ver/count/entsize 头 + 条目（tid u32 + name[32]），顺序与 THREAD_REG_INFO 一致。

## 判读工具与技能（复用入口）

- **工具**：`samples/j2me/debug/tools/psp2dmp.py`（git 已提交 `8d3e2a0`；stdlib-only，双 dump 回归通过）
  - `list <dump>`：notes + PT_LOAD 故障页清单
  - `threads <dump>`：线程名/寄存器/分类/自动基址探测/pc-lr 符号化/内核线程 r0 自动解引用
  - `base <dump>`：只出基址判决（双锚点：lr→0x81011b4b 与 stub pc→0x8110d338 必须一致）
  - `mem <dump> <addr> [len]`：读 dump 内 PT_LOAD 页（栈/故障页窥镜）
  - `elfstr <dump> <addr>`：runtime 地址→ELF vaddr 映射读字符串（.rodata 路径/格式串）
  - `--elf` 可覆盖默认 `vita-port/build/midp_vita`
- **技能**：`.github/skills/psp2dmp-analysis/SKILL.md`（workspace 根，非 git）——判读流程、锚点表、陷阱清单、log70/71 案例模板
- 下一份 dump 到手时：`python3 debug/tools/psp2dmp.py threads <file>` 一条命令出判决主干了。

## 2026-10-08 log70：**v02.45/v02.46 "进去就白屏"定案——不是 debug 太多，是垫 asm 里丢了一条 `movt`**（v02.47 修复，提交 `af2c9bc` + `788be8a`）

- 症状：装 v02.46 后 MIDlet 一启动就白屏，且这次带回 **psp2core coredump**（`psp2core-*.psp2dmp`，gzip 压缩的 ELF32 ARM core 文件）——是**崩溃**不是冻结。boot_log 止于 `runtime Hello.jar size=9203`（jar 部署完、VM 启动早期），无 ring_trap.log。
- coredump 判读方法（复用价值高）：
  1. `gunzip -c *.psp2dmp > /tmp/core`，按 ELF PT_NOTE 遍历：THREAD_INFO（线程名）、THREAD_REG_INFO（5 线程 × 376 字节：size/tid + 17 寄存器 + 扩展区）、STACK_INFO（各线程栈使用量）、TTD/TTY_INFO（内核 TTY 流）。
  2. **模块实际基址 ≠ 0x81000000**：本次装在 `0x8101a000`。锚点法：睡眠线程 pc=0x81127338/r12=0x9da（`sceKernelDelayThread` stub 内 svc），lr=0x8102bb4b 反算后落在 `__wrap_sceKernelDelayThread` 的 R_ARM_THM_CALL（0x81011b46）返回处 → delta=0x1a000。**addr2line 前必须先减 delta**，否则会把 pc 符号化到 `.rodata`（执行数据=基址错位的典型信号）。
  3. 主线程现场：pc=`vita_landing_pad+0x148`（`str r1,[r2]`），**r2=0x00009790**——只有 movw 的低 16 位。
- 根因：v02.45 改注释时把 `movt r2,#:upper16:vita_v244_ct` 挤掉了（v02.44 的 c9fac02^ 里还在）。垫 asm `8:` 标签后的 v244 记录块每次轻量线程出生都执行 → `str` 写未映射的 0x9790 → data abort。洪水检测器从未武装（tripped=0），所以没有 ring_trap.log——**崩在探针自己身上**。
- 判决：用户"debug 加太多导致白屏"方向对了一半——是 v02.45 引入的**回归 bug**，不是探针设计本身；v02.46 的 jstack 无罪（watchdog/ticker 线程整场都在 `sceKernelDelayThread` 里睡觉）。
- 修复：补回 movt（InterpreterRuntime_arm.cpp，`8:` 块），重编 `_MergedSrc004.o` + `OS_vita.o` 链验证 33 objects/29 members，VPK `out/vpk/midp_vita_v02.47.vpk` md5 `00bc489870d7403aaa369a33dc8e13ed`，版本串 `v02.47 b441 (788be8a)`。objdump 复核：`movw r2,#0x9790` 后紧跟 `movt r2,#0x814b`，`str` 落在 `vita_v244_ct`(0x814b9790)。
- 待办：v02.47 上真机复测 log70 场景（FileManagerMIDlet），观察是否回到"冻结+watchdog rd7"老轨道——那样 v02.45 洪水陷阱与 v02.46 jstack 才第一次真正受试。
- 教训入库：(a) **手写 movw/movt 成对出现，注释编辑时必须逐行 diff 核对**；(b) coredump 里 PC 指向"不可自陷指令"（orr/nop/str 之前有更早的载入错位）先怀疑基址，用睡眠线程的 stub PC 反算 delta；(c) psp2core 会把 fault 涉及的额外页 dump 进 PT_LOAD（本次 0x81707000/0x818a5000/0x818b5000 恰是 r10/r0/r6 指向页），可当内存窥镜。

## 2026-09-26 log40：**v02.15 判决 = NEITHER（预注册命中）——cpu1 内核调度器死亡，方向 A 穷尽；core dump 把死点钉死在"第一次切入 MIDlet Java 代码"的第 7 条指令**

> v02.15（b345, `1c2fae4`）真机日志。目录 `debug/logs/log40/`。

### 1. 判决：NEITHER —— 与 log39 §6 预注册的判据矩阵完全一致

```
[WD] VMPROG verdict exec=0 tick=0 -> NEITHER -> cpu1 scheduler DEAD (direction B: kubridge)
[WD] VMPROG-A vm-exec  switch=218008->218008(d=0) fast=131->131(d=0) site=219277->219277(d=0) jsmp=107->107(d=0)
[WD] VMPROG-B tick-ch  tick=1071->1071(d=0) ttk=107->107(d=0) pump=208->208(d=0)
```

- VM 在 500ms 内 **一次 `switch_thread` 检查点都没走**（正常约 21.8 万次/轮）；
- 与 VM **同核同优先级 0xa0** 的 ticker **一次 tick 都没发**；
- cpu1 内核账本三项全冻（`idle 12563428788` / `idleN 25786546` / `switchN 51653795`，全 d=0），
  同时 cpu0 `idleN 8575529->8575576`、`switchN 17608682->17608776`，cpu2/cpu3 同样在涨；
- 同轮 `vm runClocks 10438483->10438483 (d=0)` vs `wd runClocks d=1754`、`tone runClocks d=1930`；
- 内核仍报 `vm status=RUNNING wait=none(0x0) cpu=1 lastcpu=1`（幽灵态）；KICK0..5 全无效，
  KICK5 `susp/res rc=0x80020008` = `ILLEGAL_PERMISSION`。

**⟹ cpu1 的内核调度器整体停摆。方向 A（用户态观测）彻底穷尽。**

### 2. core dump 重建（本轮最重要的新材料）

**解析布局修正**：`THREAD_REG_INFO`(note 0x1004) = 8 字节头 + N×0x178 块，
`+0x00` size_field / `+0x04` SceUID / `+0x08..+0x44` r0..r15 / `+0x48` cpsr。
log39 用的解析器把寄存器当在 `+0x00`，整体错位 2 字 —— 现用解析器 `/tmp/log40/regs.py`。

**5 个线程（`THREAD_INFO` 记录与 entry 反推 slide 交叉验证）**：

| 线程 | uid | entry | 栈 | prio | attr | 冻结 pc |
|---|---|---|---|---|---|---|
| **J2ME00001（vm 主线程）** | 0x40010003 | `_start+1` | 0x81341000 +256K | 0xa0 | 0x80000000 JOINABLE | **`start_lightweight_thread_asm+0x18`** |
| SceCommonDialogWorker | 0x400100c3 | 0xe018f57d | 0xd0001000 +64K | — | 0 | 0xe00073a4 |
| j2me_tone | 0x4001015d | `tone_player_thread+1` | 0x813b5000 +16K | 0xa0 | 0 | `sceKernelDelayThread+0x8` |
| j2me_watchdog | 0x40010161 | `wd_thread_routine+1` | 0x813bd000 +16K | 0xa0 | 0 | `wd_thread_routine+0x884`（Thumb，正在写 sysinfo） |
| cldc_ticker | 0x400101c7 | `ticker_thread_routine+0` | 0x813d1000 +64K | 0xa0 | 0 | `sceKernelDelayThread+0x8`，r0=0x2710 |

⇒ **vm 线程就是主 OS 线程**（与 `vita_main.c` 注释 "this thread becomes the VM's main
interpreter thread inside runMidlet" 一致）；**没有为 MIDlet 新建 OS 线程** ⟹ Java 线程是在
primordial 线程上复用的 green thread。

**vm 线程保存的用户上下文**：

```
pc   = 0x810fd03c  -> start_lightweight_thread_asm+0x18   (bleq invoke_pending_entries)
lr   = 0x810fd024  -> start_lightweight_thread_asm+0x00
sp   = 0x8137ff70
cpsr = 0x40070010  User, ARM, IRQ/FIQ 使能, Z=1
r0 = r3 = 0x81ede328 (当前 Thread oop)   r5 = 0x81401904 (GP)   r6 = 0x81edf390 (jsp)
```

**`lr` 为何等于 stub 入口（机制，非偶然）**：`JVM::start()+0x174` 处 `bl primordial_to_current_thread`
（0x810bd000），该 thunk 只有 9 条指令：

```asm
push {r3-r9,sl,fp} ; push {lr}
ldr  r5, =GP ; ldr r1,[r5,#1036] ( _current_thread )
str  sp,[r5,#1252]                 ; 保存 primordial sp
ldr  r6,[r1,#100]                  ; r6 = jsp
ldr  lr,[r6,#4]                    ; ← 从 Java 栈取出"恢复 PC"
ldr  fp,[r6],#8
mov  pc, lr                        ; ← 跳过去，lr 留在目标地址
```

所以 `lr == 跳转目标 == stub 入口` 是这套"primordial→Java"切换的必然产物，**并证明线程是被
`primordial_to_current_thread` 切进 Java 执行后只跑了 6 条指令就停住**。全程无 `svc`。

**OS 栈把调用链钉死，且每个栈字都可解释**：

```
sp+0x00 (0x8137ff70): 810f068c  JVM::start()+0x178   ← 正是 bl primordial_to_current_thread 的返回地址
sp+0x34:              810f0804  JVM_Start+0xa4
sp+0x54:              810830dc  midp_run_midlet_with_args_cp+0x95c
```

`primordial_to_current_thread` 的 `push {lr}` 把返回地址放在 frame_base，`str sp,[gp+0x4e4]`
保存的正是 frame_base，而 `start_lightweight_thread_asm` 前 7 条指令**没有任何 push**
⟹ `sp == frame_base`、`[sp] == JVM::start()+0x178`。**寄存器与栈完全自洽。**

### 3. 死点语义：冻结发生在"本地原生代码第一次切入 MIDlet 的 Java 代码"那一瞬间

`runmidlet_debug.log` 全部内容以两行收尾：

```
*** runMidlet: midpInitialize OK
*** runMidlet: calling midp_run_midlet_with_args_cp now    ← 此后无任何输出
```

`crumb.log` 末尾：`menu hb: flips=130` → `menu exit: have_sel=1` → `round begin` →
`launch: games/app (1)/game.jar` → `runMidlet enter` → `vm UseCompiler(pre)=1`。
`input_debug.log` 最后一次交互 `buttons=0x00000040`（选择键）。

⇒ **`tick=1071`（10.7s）与 `ce=208`（10.4s）不是"MIDlet 跑了 10 秒"，而是 VM 启动 +
`midpInitialize` + 原生菜单等待的总时长。冻结就在进入 MIDlet 的第一条 Java 代码路径上。**
（此前各轮"进入 MIDlet 后约 11s 冻结"的表述按此修正。）

**指令级**：已执行的 6 条 = 3×load + `mov r2,#0` + `tst` + `strne`（Z=1 → 未执行），
**无系统调用、不写共享内存** ⟹ 线程不可能由此主动进内核。`cpsr` 为 User 且 IRQ/FIQ 使能，
`pc` 停在 `bleq` 本身、`lr` 未被覆盖 ⟹ **`bl` 尚未执行**。

⇒ **线程刚被切换进 Java（第 7 条指令处）就被中断打断，此后 cpu1 内核再未恢复。**
log39 同位置为 `+0x8`（第 3 条）、`lr` 同样是 stub 入口 —— **两次独立运行都停在同一个 stub 的
前 7 条指令内，不可能是巧合的定时器抢占。**

### 4. 五轮复现签名（闭合）

| 轮次 | 版本 | 钉核 | 死点 | 宿主核调度器 | ticker | rt_ticks | KICK |
|---|---|---|---|---|---|---|---|
| log36 | v02.11 | 无 | stub 前沿 | 停摆 | 独立 | — | 无效 |
| log37 | v02.12 | CPU0 | 同上 | 停摆 | 独立 | — | 无效 |
| log38 | v02.13 | 无 | stub+0xc/0x14/0x18 | 停摆 | cpu0 独立 | 1400 | 无效 |
| log39 | v02.14 | CPU1+ticker 同核 | stub+0x8 | 停摆 | 同核饿死 | 0 | 无效 |
| **log40** | **v02.15** | CPU1+ticker 同核 | **stub+0x18** | **停摆（VMPROG 双采样证实）** | **同核饿死** | 0 | 无效 |

### 5. 顺带修正 / 新增事实

- **`site seq=219277 frozen=yes` 本版已是真值**（200ms 窗口 + `VMPROG-A site d=0` 500ms 独立
  复核，三重复核一致）⟹ log39 §2 的 `frozen` 假探针问题已修掉。
- `q5: t0x81ede328(id=4) t0x81edc2ec(id=3) CYCLE(2)` —— 2 元素队列，再次确认 `CYCLE(n)` = 元素数，
  **不是队列损坏**。
- ticker 创建者 = `JVMOs::start_ticks()+0x78`，只在 pthread-init 路径创建一次，**非线程累积**。
- **JIT 已排除**：`vita_jit_policy` 默认 0 ⟹ `runMidlet` 会传 `-int`（`Arguments.cpp:129` 置
  `UseCompiler=false`），本轮是**纯解释器**。crumb 的 `UseCompiler(pre)=1` 是 `Globals.hpp:231`
  默认值，`vita_main.c` 注释已警告其不可信。
- **⚠ 地址校准坑（务必记住）：本版 `text/rodata` slide = `+0x40000`，`data/.bss`（GP 全局区）
  slide = `+0x120000`，两者不等！** 依据：`vita_site_dump` 自打的 `self=0x81094958`（nm
  0x81054958，5 处独立吻合）；而 `[r5+0x40c] = 0x81401d10` = `wd_snapshot` 的 `cur_cell`，
  r5 取自 `.word 0x812e1904`（+0x120000）。**用错 slide 会把数据地址解析成 `g_net_mem+0x…`
  这类假符号。core dump 与 velf 必须分别校准。**

### 6. 局限

- Java 堆段（0x81edf390 等）在 core 里**未保存**（`filesz < memsz`），无法读出 `[jsp+8]`
  （切换后本应执行的目标）与 `[gp+0x4e4]`（保存的 primordial sp）；
- `SYSTEM_INFO2` 有 4.1MB 内核系统信息未挖掘（若需确认"cpu1 上是否还有可运行线程"可再挖）。

### 7. 下一步（按性价比）

1. **零成本 A/B（最推荐）**：菜单里换一个游戏 / 用 launcher 内置 `Hello.jar` 再跑一轮。
   所有 jar 都冻在同一处 ⟹ 通用适配缺陷（`JVM::start` / `primordial_to_current_thread` /
   `start_lightweight_thread_asm` 这条路径）；只有 `game.jar` 冻 ⟹ 该 jar 内容/类初始化相关。
   **（v02.16 已把这步做成不经过菜单的确定性路径，配方见下节）**
2. **零成本调度 A/B**：`cldc_ticker` 优先级 0xa0 → 0x9f/0xa1（或钉 cpu0），vm 线程不钉核 ——
   检验"同核同优先级 + 定时器中断"是否为触发条件。
3. **方向 B（需 taihen+henkaku CFW 真机，Vita3K 不加载内核插件）**：kubridge +
   `kuKernelCpuUnrestrictedMemcpy` / `kuKernelRegisterAbortHandler`，直接读 cpu1 停摆处的内核态
   （锁 / runnable 队列 / 定时器中断路径），把"cpu1 停在哪个内核函数"点名。

### 8. 与"代码没适配好"框架的调和

触发点、死点、调用链**全在我们的移植代码路径上**（`midp_run_midlet_with_args_cp` →
`JVM_Start` → `JVM::start()` → `primordial_to_current_thread` → `start_lightweight_thread_asm`），
这正是 phoneME ARM 移植里最"手写汇编、最平台相关"的一段（`str sp,[gp+0x4e4]` 保存 primordial sp、
`ldr lr,[jsp+4]; mov pc,lr` 手工跳转）。**即：我们的移植执行模式把 cpu1 的 Vita 内核带进了停摆，
用户态已无法再观测** ⟹ 必须转内核侧（方向 B）。

## 2026-09-26 v02.16：**A/B 基础设施——launch.cfg `menu=0` 跳过原生菜单**（提交 `3ddccf0`）

log40 判决后要回答的下一个问题是"所有 jar 都冻在同一处吗"。但 log40 那轮**是走菜单**
选的 jar（`menu jar: games/app (1)/game.jar`），而菜单本身（2MB framebuffer、触摸初始化、
`games/` 扫描）是每轮共有的变量，直接在菜单里换 jar 会让 A/B 结果无法归因。所以先把
"目标 MIDlet"做成一条**不经过菜单**的确定性路径。

### 代码改动（仅 launcher，VM 源码未动）

`vita-port/src/vita_main.c`：

- `read_launch_cfg()` 增加出参 `int *skip_menu`；在**换行被改成 NUL 之前**扫原始缓冲区，
  找**行首** `"menu=0"`；
- `main()` 把 `read_launch_cfg()` 提到菜单之前调用一次：
  - `cfg_ok && skip_menu` → 完全跳过 `vita_menu_run()`，直接用 cfg 的 jar/class；
    日志打 `launch.cfg jar (menu=0): …`；
  - 否则 → 与 v02.15 **逐字节相同**（菜单 → 失败回落 launch.cfg → 再回落内置默认）。
- **顺带修 bug**：旧 `read_launch_cfg()` 边解析边写 out 参数，一份只有 `jit=0` 的
  launch.cfg（菜单保存 JIT 设置时写出）会让函数返回 -1 却留下 `jar="jit=0"`，于是
  "回落内置默认值"的分支真的去加载一个名为 `jit=0` 的 jar。现改为解析到局部变量、
  成功才拷出。
- 注释明确：**jar 路径必须相对**（main `chdir(DATA_DIR)` + classpath 以 `:` 分隔，
  绝对路径会在第一个冒号处被切成 `ux0` + `/data/...`）。

`vita-port/ab/`：四份样例 cfg（`launch.cfg.hello` / `.tonetest` / `.canvas` / `.game`）
+ `README.md`（部署步骤 + 判读判据）。

### A/B 配方（真机，`ux0:/data/J2ME00001/launch.cfg`）

```
Hello.jar
HelloMIDlet
portrait
menu=0
```

`Hello.jar` 是自编测试包里**最小**的 MIDlet（纯 LCDUI，无音频/网络/RecordStore），
执行画像与 `com.app.filemanager.FileManagerMIDlet` 相差最大。

### 判读

- 冻结签名与 log36..log40 逐项相同（死点在 `start_lightweight_thread_asm+0x18` 附近、
  `runClocks d=0`、cpu1 `idleN/switchN d=0`、`VMPROG NEITHER`、KICK 无效）
  ⟹ **通用路径**问题，与 MIDlet 内容无关，方向 A 的"换 jar"分支关闭；
- 不冻 ⟹ 与 `game.jar` 内容 / 类初始化相关，转去比对两者启动路径。

### 构建验证

`J2ME Player v02.16 b348 (9b3952e)`；`out/vpk/midp_vita_v02.16.vpk`（3655912 B）；
`strings` 确认 `menu=0` / `launch.cfg jar (menu=0): ` 已进 ELF。

> 坑：`CMakeLists.txt` 里 `set(VITA_VERSION …)` 必须排在它自己的注释块**之后**。
> v02.16 第一次编辑把 `set(VITA_VERSION "02.16")` 写在注释前面、后面又跟着
> `set(VITA_VERSION "02.15")`，被后者覆盖 —— 构建出的 VPK 仍叫 `v02.15`。
> 已把 `set()` 移到注释块末尾（866 行）。

## 2026-09-26 v02.14：**方向 A 审计落地——green-thread sp 安全 + 事件泵正常 + 跨核 ticker 竞态修复（钉核）**

> 用户方向 A 选择：先做代码审查 + 最小修复，无效再上 kubridge（方向 B）。
>
> **⚠ 实测判决（log39 真机）：钉核生效但无效——假说排除。**
> `cldc_ticker aff=0x20000(init 0x20000) cpu=32767 last=1` 证明 ticker 已与 vm 同核
> （v02.14 设计目标达成），但死机签名与 log38 **逐项一致**（vm runClocks d=0、cpu1
> switchN/idleN d=0、vm `RUNNING wait=none cpu=1`、KICK no effect、死点
> `start_lightweight_thread_asm+0x8`）。**⟹ 跨核 ticker 竞态不是根因。**
> 副产物：钉核把唯一的独立心跳源也拖进 cpu1 —— log38 ticker 在 cpu0 时 `rt_ticks=1400`
> （tick 持续到达），log39 同核后 `rt_ticks=0` 且 `vita_tick_count` 冻结在 1118。
> 详见下方 log39 节。

### 方向 A 审计结论

1. **green-thread sp 切换架构安全（非缺陷）**：ARM 上 `jsp=r6`（堆上 Java 栈）与 `sp=r13`
   （OS/primordial 栈）是不同寄存器；所有 native C++（含事件泵的 pthread 等待）通过
   `get_primordial_sp(sp)` 切回 OS 栈执行，syscall 全在 OS 栈。改 sp 不指向堆，不构成
   "内核 TCB 损坏"路径。
2. **事件泵链路正确（非缺陷）**：`JVMSPI_CheckEvents → ANI_WaitForThreadUnblocking →
   PoolThread_WaitForFinishOrTimeout → Os_WaitForEventOrTimeout → pthread_cond_timedwait`
   跑在 OS 栈；`ANI_Initialize()` 已在 vita_main.c 显式调用（PROJECT_MEMORY 3006 的历史
   bug 已修）；log38 `ani_mark "CE #418 ANI OUT"` 证明最后一次泵正常退出，未卡 pthread。
3. **唯一明确的"代码未适配"缺陷 = 跨核 ticker 竞态**：上游 linux 用 `setitimer+SIGVTALRM`
   在 vm 自身线程（同核）递 tick；vita 移植改成独立 `cldc_ticker` 线程，创建时
   `cpuAffinityMask=0`（继承），log38 thread audit 显示它落在 cpu0（`last=0`），而 vm 钉
   cpu1（0x20000）→ `real_time_tick()` 跨核非原子写 `_rt_timer_ticks`/`_real_time_has_ticked`
   /`_compiler_stack_limit`/`_timer_has_ticked`，是上游同线程模型不存在的数据竞态。

### v02.14 修复（最小 diff）

- **OS_vita.cpp**：`cldc_ticker` 创建时 affinity 从 `0` 改为
  `sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId())`（继承 vm 主线程当前核），
  失败回退 `SCE_KERNEL_CPU_MASK_USER_ALL`。恢复上游"tick 与 vm 同核"语义，消除跨核可见性
  问题。`+23` 行（含注释），`#include <psp2/kernel/cpu.h>`。
- **CMakeLists.txt**：VITA_VERSION 02.13 → 02.14 + 版本注释。

### 构建验证

- `rebuild_vm.sh`：33 ARM 对象 OK（OS_vita.o 已重编，`nm` 确认引用
  `sceKernelGetThreadCpuAffinityMask`）；libcldc_vm.a 29 members repack 成功。
- `vita-port/build.sh`：链接 + 打包成功，产物 `out/vpk/midp_vita_v02.14.vpk`（3655860B）
  + `.sym`（4289648B）；ELF 内 `sceKernelGetThreadCpuAffinityMask@811069b0` 在位。

### 遗留风险 / 下一步

- **钉核消除的是竞态，不保证根治内核停摆**：若真机仍死机，说明根因在"green-thread 高频
  切换 + pump wait syscall"模式触发的内核 bug（非 ticker 竞态），需上 kubridge（方向 B）
  读停摆核内核态定位卡点。
- 次选：降低 TickInterval / pump 50ms 窗口（log38 已列），观察停摆延迟是否随频率变化。

## 2026-09-26 log39：**v02.14 判决 + core dump 内核侧视角——cpu1 取走定时器中断后再未返回调度器**

> v02.14（b344, `1274c64`）真机日志。目录 `debug/logs/log39/`。

### 1. 判决：v02.14 钉核生效但无效（见 v02.14 节顶部）

### 2. **两条旧结论必须作废（探针自身有 bug）**

- **`frozen=yes` 是无意义常量**：`OS_vita.cpp::vita_site_dump()` 里
  `s = vita_site_seq;` 后**紧邻**又读 `s2 = vita_site_seq;`（无任何延时）→ `s == s2` 恒真
  → 永远打印 `yes`。log35~log39 所有基于 "ring frozen" 的推论**全部无效**。
- **`q5 ... CYCLE(3)` 不是环损坏**：`Scheduler.cpp:2635-2680` 注释与实现写明优先队列是
  **环形单链表**，walk 回到 head 即停并打印 `CYCLE(count)`，`count` = 该队列元素数。
  `q5: id=4, id=1, id=3, CYCLE(3)` = q5 有 3 个线程，**正常**。log36 同名输出同理。疑点关闭。

### 3. core dump 首次给出内核侧视角（决定性新材料）

标定：`vita_site_mark` nm=`0x81054730`、运行时=`0x81055730` ⟹ **slide = 0x1000**
（`self`/`mark`/`ticker_thread_routine` 三处互验一致）。

`THREAD_REG_INFO`(note 0x1004) = 8 字节头 + 5 × 0x178 块，块 1 = vm 主线程（uid `0x40010003`）：

```
vm  : r0=0x81ede328(=cur_thread)  r6/jsp=0x81edf390  sp=0x8137ff70(primordial栈 0x81341000+0x40000)
      lr = 0x810bde14 -> start_lightweight_thread_asm + 0x0
      pc = 0x810bde1c -> start_lightweight_thread_asm + 0x8   (ldr r1,[r0,#104] 读 status)
      cpsr = 0x60070010 -> User mode, IRQ/FIQ 均使能, ARM 态
      r6/r10/fp 等与 site_ring 的 jsp 一致
 tone: pc = sceKernelDelayThread + 0x8      (正常睡在 10ms 延时里)
 tick: pc = sceKernelDelayThread + 0x8      (同上; lr = ticker_thread_routine+0x48)
 wd  : pc = wd_thread_routine + 0x69a       (正常跑, 正在请求 dump)
 cdw : pc = 0xe00073a4                      (SceCommonDialogWorker, 系统模块)
```

**5 个线程里 4 个健康，只有 vm 冻在普通用户态指令上，且内核保存的是 User 模式、中断使能上下文。**

### 4. 修订后的根因模型（比 log38 更硬）

> cpu1 在 **vm 线程位于 `start_lightweight_thread_asm+0x8` 时取走定时器中断**（用户态、
> IRQ 使能，属正常抢占），**此后 cpu1 再也没有回到调度器**。

判据：ticker 是 `READY`、优先级 `0xa0`，**与 vm 完全相同**，且钉死在 cpu1。只要 cpu1 调度器
活着，10ms 内必轮转调度 ticker、`vita_tick_count` 必推进。它冻结在 1118 ⟹ **cpu1 不再执行
"中断返回 → 调度"这条内核路径**。这不是用户态活锁，是内核侧停摆。

### 5. 地址符号化（slide=0x1000）

| 裸地址 | 符号 |
|---|---|
| `0x810bde1c` (pc) | `start_lightweight_thread_asm + 0x8` |
| `0x810bde14` (lr) | `start_lightweight_thread_asm + 0x0` |
| `0x81085010` ring[0],[3..15] | `switch_thread + 0x28`（Scheduler.cpp:2175 mark） |
| `0x81082e04` ring[1],[2] | `pump_events_vita + 0x48`（Scheduler.cpp:748 mark） |
| `0x8100f7c2` | `wd_thread_routine + 0x69a` |
| `0x81107948` | `sceKernelDelayThread + 0x8` |
| `0x810556a0` | `ticker_thread_routine + 0x0` |

ring 按写入序还原 = `switch×13, pump, pump, switch`（无 `wait_for_event_or_timer` mark ⟹ 慢通道从未使用，与 slow=0 一致）。

**`switch=225264` 不是"切换次数"，而是解释器 `shared_call_vm` 的线程检查点次数**
（`SharedStubs_arm.cpp:508` 的 `bl("switch_thread")`），纯 Java 执行量指标——这解释了它为何比
`yield()`（141）大 1600 倍。

时间账自洽：`vita_tick_count=1118 × 10ms ≈ 11.18s`（ticker 寿命）、`pump_runs=216 × 50ms ≈ 10.8s`
（泵寿命）、`ttk=116` 且 stage 1..7 全 116（冻结前每个 tick 都完整跑完）。

### 6. v02.15 判定实验（已实现并构建，真机判决见顶部 log40 节）

让 watchdog 在 hang 后对 **VM 侧计数器做双采样**（间隔 500ms），打印 delta：
`_vita_switch_count`、`_vita_pump_runs`、`_vita_ttk_count`、`vita_tick_count`、`vita_site_seq`；
顺带修掉 `frozen` 假探针（`s2` 延迟重读）。判据矩阵：

| switch (VM 执行) | tick (cpu1 调度) | 结论 |
|---|---|---|
| 涨 | 涨 | VM 在跑 Java，cpu1 健康 → VM/Java 层活锁（回 VM 侧找循环） |
| 涨 | 不涨 | VM 在跑且独占 cpu1（ticker 同优先级被饿死）→ 内核调度策略 |
| **不涨** | **不涨** | **cpu1 彻底停摆（预计结果）→ 转方向 B（kubridge）** |
| 不涨 | 涨 | cpu1 健康但内核丢了 VM 线程 → 内核 TCB 问题 |

**实现状态（`1c2fae4`）**：
- `vita-port/src/vita_watchdog.c` +89：`wd_dump_vm_progress()` 双采样 + 判决行，调用点放在
  **KICK 序列之前**（避免 affinity/priority 变更污染基线）。日志行前缀 `[WD] VMPROG-A/B/verdict`。
- `phoneme-cldc/src/vm/os/vita/OS_vita.cpp` +35/-1：`vita_site_dump()` 里 `s`/`s2` 之间插入
  `sceKernelDelayThread(200*1000)`，使 `frozen=` 恢复真实语义（该函数跑在存活的 watchdog 线程上，
  所以延迟重读可行）。
- `vita-port/CMakeLists.txt`：`VITA_VERSION` → `02.15` + 版本说明注释。
- 构建验证：`rebuild_vm.sh` 33 objects / 29 members、`jvm_fast_globals` 恰好 1 个 D 定义、0 个
  C 解释器符号；`build.sh` 成功，产物 `out/vpk/midp_vita_v02.15.vpk`（3655693B）+
  `midp_vita_v02.15.sym`（4290144B），版本串 `J2ME Player v02.15 b344 (1274c64)`。

**读日志要点**：`[WD] VMPROG verdict exec=0 tick=0 -> NEITHER -> cpu1 scheduler DEAD (direction B: kubridge)`
即预计结果 —— 届时用户态探针到此为止，直接上 kubridge。

**真机结果（log40）：判据矩阵第三行命中 = NEITHER，用户态探针到此为止。详见顶部 log40 节。**

## 2026-09-26 log38：**停摆核严格跟随 vm 宿主核 → "vm 毒核"坐实，内核级单核停摆**——v02.13 换核钉死（CPU1）

> v02.13（b342, `16bb120`）换核钉死对照实验。完整分析见 `debug/logs/log38/ANALYSIS.md`。

### 结论（决定性）

1. **停摆核 == vm 宿主核，双点确认**：
   - log37 钉 CPU0（0x10000）→ cpu0 停摆（`switchN/idleN/idle` 全 d=0）；
   - log38 钉 CPU1（0x20000）→ cpu1 停摆；
   - log36 三核时 vm 迁移过（chgcpu=2839），停摆核 cpu1 是它"曾停靠并触发、随后迁离"的核。
2. **死点三次稳定在 green-thread 切换桩入口**（`start_lightweight_thread_asm` 的 +0xc/+0x14
   /+0x18 内部，全是普通用户态指令，`cpsr=User mode`），lr 恒 `+0x0`，site_ring 回溯恒
   `switch_thread ← pump_events_vita`。
3. **因果链**：vm 在宿主核高频 green-thread 切换（switch=185223，纯用户态）+ 跨核 ticker
   交互 + pump 的 wait syscall → 触发宿主核**内核调度器整体停摆**（switchN d=0 = 该核再无
   上下文切换，idle 也不推进）→ vm 作为该核用户态线程不再被调度 → 冻结。
4. **排除用户态死锁**：`lanes fast=386 slow=0`（yield 慢路径未进）、`pump=418`、`gnrt=0/0`、
   `sf=0/0/0` 全健康——vm 是受害者，不是凶手。

### 判决 + 下一步

- **"vm 毒核"坐实，是 Vita 内核 bug**：某核内核调度器/中断处理在 vm 高频活动下停摆。
- **下一步（首选）kubridge 内核取证**：读停摆核内核态，定位卡在哪个内核函数/锁（定时器中断？
  runqueue 锁？lwmutex？）。用户态探针已到极限（死点全是普通指令）。
- 次选：降低 `TickInterval` / `pump_events_vita` 50ms 窗口，看停摆延迟是否随频率变化，反推触发源。
- 佐证：查 SceKernelThreadMgr lwmutex/调度器/定时器官方 SDK 已知 bug。

## 2026-09-26 log37：**钉核成功仍卡死 → 跨核迁移假说推翻，故障升级为"内核级单核冻结"**——v02.12 对照实验

> v02.12（b339, `f5df64e`）单核钉死对照实验。完整分析见 `debug/logs/log37/ANALYSIS.md`。

### 结论（三条证据交叉闭合）

1. **钉核成功，rc 语义澄清**：`pin vm cpu mask=0x10000 rc=0x70000` 的 rc 是**旧 affinity
   mask**（`sceKernelChangeThreadCpuAffinityMask` 返回旧值，非错误码）。交叉证据：watchdog
   KICK1 `aff->0x30000 rc=0x10000`、KICK2 `aff->0x70000 rc=0x30000` 全返回旧值。watchdog 确认
   `aff=0x10000(init 0x70000) cpu=0 chgcpu=5`（log36 是 2839）→ vm 全程未跨核迁移。
2. **依然卡死**：`vm runClocks d=0` + `status=RUNNING` + KICK 全无效（ce 412→412），与 log36 同构。
3. **新现象：单核整体停摆**。sysinfo 100ms 采样窗口内，恰好一个核的 idle/idleN/switchN 全 d=0
   （log36 停摆 cpu1，log37 停摆 cpu0），其余三核正常涨。→ 内核级核心冻结，非用户态线程死锁。
4. **死点同簇**：vm pc=`start_lightweight_thread_asm+0x14`（`strne r2,[r0,#36]`，普通内存写，
   检查 THREAD_TERMINATING 清 pending_entries），lr=`start_lightweight_thread_asm+0x0`；log36 是
   pc=`invoke_pending_entries+0x10`。site_ring 回溯两轮都在 `switch_thread ← pump_events_vita`。
5. **排除**：线程 `[1]`（tid=0x400100c3, IRQ 模式, pc=0xe00073a4）log36/log37 逐寄存器一致，
   是 coredump 固定背景，非线索。

### 判决 + 下一步

- **跨核迁移假说推翻**（钉核 chgcpu=5 仍卡死）。死点指令是普通用户态内存写，vm 不是"毒指令"
  毒死内核，而是**被内核冻结**；停摆核在两次实验间不同（cpu1→cpu0），指向独立内核 bug。
- **下一步：换核钉死对照**（v02.13）：`VITA_VM_CPU_AFFINITY_MASK=0x20000`(CPU1)/`0x40000`(CPU2)。
  - 停摆核跟随 vm → vm 执行毒死所在核，追查切换桩附近的内核交互；
  - 停摆核固定/随机与 vm 无关 → 独立内核 bug，转 kubridge 内核取证。
- 怀疑点：`switch=188262`（18.8 万次 green-thread 切换）高频切换 + 内核交互竞态。

## 2026-09-26 log36：**JIT-on-tick 假说证伪，死点转到内核调度层**——v02.11 首次实弹

> v02.11（b336, `12ce198`）带 tick-stage 探针 + JIT A/B 开关首跑。完整分析见
> `debug/logs/log36/ANALYSIS.md`。

### 结论（三条证据交叉闭合）

1. **tick 体完整跑完 309 次**：`ttk_stage=7 hits 1..7=309×7`（7 个 stage 全命中
   309）。stage 3→4 包住 `Compiler::on_timer_tick`，同样 309/309 → **JIT-on-tick
   假说被推翻**，冻结点不在 tick 体、不在 JIT 编译。
2. **A/B 开关工作正常**：`round 0: jit cfg=0 -> policy=0`；`vm UseCompiler(pre)=1
   (jit policy=0)`。本轮 JIT 处于禁用态，进一步排除 JIT。
3. **vm 被内核冻结，不是用户态死锁**：
   - `vm runClocks 9517268->9517268 (d=0)`，而 tone d=1964 / wd d=1658 都在涨；
   - `vm status=RUNNING`（内核视角仍在 runqueue）+ `4 个 CPU idle 全在涨`（无死循环）
     + `cpsr=0x60070010`（User mode，不在 syscall）三者并存；
   - `gnrt=0/0`（`get_next_runnable_thread` 从不返回 NULL，runnable 从不空）、
     `lanes fast=346 slow=0`、`sf=0/0/0`（sleep_forever 空转没进过）→ 排除
     "全线程等待→空转"经典死锁。
   - **定性：vm 原生线程在用户态 green-thread 切换路径被内核抢占后不再被调度。**
     KICK0~5（signal/aff/prio/suspend）全无效（`ce 382->382`）。

### 死点定位

- vm `pc=invoke_pending_entries+0x10`（`moveq pc,lr`，pending 为空），
  `lr=start_lightweight_thread_asm`（换出残留帧），与 log21/log35 **同簇**
  （green-thread 桩簇：`primordial_to_current_thread → start_lightweight_thread_asm
  → invoke_pending_entries`）。
- vm 栈回溯：`main → midp_run_midlet_with_args_cp+0x95c → JVM_Start+0xa4 →
  JVM::start+0x178`（`bl primordial_to_current_thread` 之后）→ 桩簇。

### 根因推断 + 下一步

- vm `aff=0x70000`（允许 CPU 0/1/2）与 tone/wd/ticker 的 `aff=0x0` 是唯一显著
  差异，`chgcpu=2839` 表明 vm 死前在多核迁移过。
- **下一步：单 CPU 亲和性对照**（vm 限 `aff=0x10000`），若冻结消失 → 坐实 Vita
  内核多核调度 bug；仍冻结则需内核层手段（kubridge/内核日志）。
- **已实现 v02.12（b339, `f5df64e`）**：`vita_main.c` main() 最早处把 vm 主线程钉到
  CPU0（`VITA_VM_CPU_AFFINITY_MASK=0x10000`，宏在 `CMakeLists.txt` 导出，0=关 A/B）。
  调用链已确认链入：main@0x81000142 `blx` → fstub `sceKernelChangeThreadCpuAffinityMask`
  @0x811068d0；pin rc 写 crumb.log + boot_log.txt。VPK:
  `vita-port/out/vpk/midp_vita_v02.12.vpk`。判据：真机不再卡 → 坐实跨核迁移 bug +
  钉核即 workaround；仍卡 → 排除多核，转内核层（kubridge/内核日志），Vita3K 无法复现。

### 工具教训（新增）

- **coredump 字段映射**（`THREAD_REG_INFO` 块，`stride=0x178`，desc 头 16 字节
  `"NFO\0"|type=0x11|count|stride`，块从 `+0x10` 起）：
  `+0x00` tid，`+0x04..+0x34` r0..r12，`+0x38` sp，`+0x3c` lr，`+0x40` pc，
  `+0x44` cpsr。（log35 手记的 `+0x34=pc/+0x40=lr` 是错的。）
- NOTE name 实际为 `b"\x04\x10\x00\x00THREAD_REG_I"`（带 4 字节前缀且后缀被截断），
  匹配必须用子串 `b"THREAD_REG_I" in name`，不能 `== "THREAD_REG_INFO"`。
- 脚本已修：`debug/tools/parse_core36.py`（跑法 `python3 debug/tools/parse_core36.py
  /tmp/log36.core`，符号表 `/tmp/velf336_nm.txt`）。

## 2026-09-22 目录整治：**VPK 输出目录统一为 `vita-port/out/vpk/`（唯一出口）**

> log25 装到 v01.85 旧版的直接原因查明：工作区曾有三个 VPK 位置，其中 `samples/j2me/out/vpk/` 是 CMake 迁移后**没人更新的僵尸目录**（最新停在 v01.85, 09-19），用户从那里抓的包。本轮已整治：

### 整治内容

1. 旧目录历史 VPK（v01.68~v01.85 共 16 个）**并入** `vita-port/out/vpk/`（无文件名冲突），僵尸目录 `samples/j2me/out/` 与游离副本 `vita-port/midp_vita.vpk` **删除**
2. 全工作区现只有一处 VPK：`samples/j2me/vita-port/out/vpk/`（22 个文件 = 各版本 .vpk/.sym + 当前 `midp_vita.vpk` == `midp_vita_v02.00.vpk`）
3. 脚本/文档无过时路径引用（已 grep 核实）

### 规矩（从本轮起）

- **装包永远从这个目录拿**：`vita-port/out/vpk/midp_vita.vpk`（或带版本号的副本）
- 装完**先看 boot_log 第 2 行版本号**再开始测试（log25 教训）
- 日志固定写在设备端 `ux0:/data/J2ME00001/`（boot_log/watchdog/site_ring/crumb/input_debug 等 16 个文件，Vita3K 侧在 `C:/Users/zyb/AppData/Roaming/Vita3K/Vita3K/ux0/data/J2ME00001/`）——与构建产物目录无关，不会变

## 2026-09-22 log25：**测错了版本**——v01.85 旧 VPK（复测流程教训 + 旧版数据佐证定论）

> 用户带回 log25 报"还是卡死"。判读第一步必须核对 boot_log 版本行：`version: J2ME Player v01.85 b273 (c086d73)` = 9 月 19 日的 v01.85——v01.93/v01.99/**v02.00 修复全都没被测到**。工作区产物没错（midp_vita.vpk == midp_vita_v02.00.vpk, md5 ffe1cd1f, 3651192B, 09-22 04:01），是设备上没重装。

### 复测流程新规矩（硬性）

1. **装完先看 boot_log 第 2 行**：必须是目标版本号（当前 `v02.00`）才允许开始操作。
2. 附件摘要≠文件清单：log25 摘要又写 3 个文件、实际 16 个——**同一坑踩两次**，判读前永远先 ls。

### log25 (v01.85) 与 log24 (v01.99) 同构性（佐证定论）

| 证据 | log24 | log25 |
|---|---|---|
| watchdog | HANG ce=405 polls=12 | HANG ce=350 polls=12 |
| VM 线程 | RUNNING wait=none | RUNNING wait=none |
| tone | WAITING delay(1) | WAITING delay(1) |
| coredump sp/fp/r1 | 8137ffb0 / 0 / 0 | 8137ffb0 / 0 / 0（逐字段同） |

v01.85 是无泵纯净上游行为，其挂死姿势与 v01.99 完全一致——上游裸行为必挂，定论无需修正。ring 的 `verify class=com/app/filemanager/UI` 与 log24 clinit 链头部对上。

### v02.00 复测判据（修复版独有观测点）

- watchdog `lanes` 行尾新增 `sf=entries/spins/last_ms`（sleep_forever 进入/圈数/最后睡眠）
- 线程清单每行新增 `wait=<obj> timed/no-timeout`（谁在等谁、有没有超时）
- `priority-queues` 段已改读真实 Task 队列（ENABLE_ISOLATES 下 Universe 槽位是废证据）
- 判定：不挂=修复生效；挂且 sf=1/spins 涨=泵交付侧问题（查 JVMSPI_CheckEvents 内部）；挂且 sf=0=sleep_forever 定论作废，转向出生桩计数器

## 2026-09-24 v02.00：**挂死真根因定案**——sleep_forever 无泵死睡（THE REAL FIX，推翻 v01.99 判读）

> log24（v01.99 b308）证明 v01.99 泵执行过但无效，且推翻了 log23 定案的"空转环"理论。VM 不是在转，是**睡着了**。本轮解码 dump 全部 17 个内核线程后因果链真正闭合。

### v01.99 判读错误（先认错）

1. `switch=512169` 是**开机累计值**，单次采样无时间序列——"51 万次切换打在环上"是未验证推断，实际挂死时什么都没在切换。
2. log24 site_ring（修正 ASLR slide=0x11000 后符号化）：`[0]=pump_events_vita+0x48`（v01.99 泵第 405 次执行的 mark）、`[1..15]=switch_thread+0x28`——泵确实在跑，最后一帧 CE#405 完整跑完（ani_mark OUT 在案），然后 **seq/pump/CE/心跳全部同时冻结**。转的东西不会冻住；冻住=睡了。

### log24 决定性数据

```
wd_snapshot: cur_thread=0x81401d10 active_count=5 timer_ticked=1(ev_readiness=0)
lanes fast=374 slow=0 pump=405 switch=181549
threads(priority-queues): (null)   ← 5 线程全 ACTIVE 却不在任何队列
watchdog: HANG ce=405 polls=12      ← pump==CE==405 三证同冻
```

- coredump：`pc=start_lightweight_thread_asm+0x8`、`r0=0x81eddfb4`（=线程清单 id=4 的 oop，按键触发的 Timer→TimerThread→Object.wait() 阻塞时的分发上下文残影）。
- `timer_ticked=1` 冻结互证：ticker OS 线程活着在置位，但泵死了没人消费/清零。

### 真因果链（master 模式调度无人区）

1. 游戏等按键 = 所有 Java 线程阻塞（MIDP **正常态**，非应用 bug）。
2. 最后一个可运行线程阻塞 → `block_current_thread → switch_thread`（ring 里 15 个 switch mark）→ `master_mode(next=NULL)` → **`sleep_forever()`**（Scheduler.cpp:424）。
3. 上游实现 `while(队列空) Os::sleep(0x7fffffff)`——**从不泵事件、从不唤醒超时睡眠者**，队列只有泵送事件才能重填 → 永久死锁。`sceKernelDelayThread(0x7fffffff*1000)` 还有 32 位溢出问题（实际非永久）。
4. 上游手机靠宿主 slave 循环泵事件，master 模式把 all-blocked 当调试场景（AbortOnInfiniteWait）；本移植是 master 模式且**无宿主循环**——v01.83/93/99 所有泵位都在"有线程可运行"才走的路径上，全阻塞状态是**调度无人区**。

### 修复（Scheduler.cpp sleep_forever，`#if defined(VITA)`，+25 行）

```cpp
SETUP_ERROR_CHECKER_ARG;
while (*get_next_runnable_thread() == NULL) {
  wake_up_timed_out_sleepers(JVM_SINGLE_ARG_CHECK);  // 唤醒到期等待者（MIDP 事件循环靠带超时 wait）
  pump_events_vita();                                 // 与 switch_thread 同款 50ms 节流泵
  Os::sleep(50);
}
Thread::set_current(get_next_runnable_thread());      // 镜像调试器分支：安装唤醒线程
```

反汇编验证（_MergedSrc002.o @1d91c）：循环体 bl 三件套齐全，退出尾调 set_current。最终 ELF 符号 `_ZN12JVMScheduler13sleep_foreverEv`@0x81082bf4。

### 方法论教训（本轮新增，血泪）

1. **计数器单次快照无时间序列语义**——"累计值很大"≠"正在增长"。
2. **符号化必先减 ASLR slide**（本轮 0x11000），否则张冠李戴（首次错标 monitor_exit）。
3. **附件摘要≠实际文件清单**——log24 实际 16 文件，先 ls 核实再开口。

### 构建/提交记录

- phoneme-cldc 提交 `0f13a59`；VPK `vita-port/out/vpk/midp_vita_v02.00.vpk`(3650852B)/`.sym`(4284248B)。
- 复测预期：同按键序列不再挂死；若再看门狗 HANG，site_ring 应见 pump 持续增长（睡眠循环每 50ms 一泵）。

## 2026-09-23 v01.99：~~按键挂死悬案定案并修复~~——switch_thread 泵背压（判读已被 v02.00 推翻，泵位无效）

> ⚠️ 本章"空转环"定论已被 log24 推翻：switch 计数是累计值非增速，VM 实为全阻塞死睡。泵挂在 switch_thread 顶部只在"有线程可运行"时经过，全阻塞态（挂死态）根本到不了。保留本章仅作教训记录。

> 用户原话："到底有没有分析到原因，一直子在抓log，能不能一步到位"。log23（v01.98 取证装置首战）给出决定性数据，因果链闭合，本轮直接交付修复 v01.99。

### log23 决定性数据（v01.98 b305，site_ring wd_snapshot 段）

```
lanes fast=377 slow=0 pump=370 switch=512169
threads(priority-queues): (null)
threads(active-list): 5 线程全 st=00000000（无任何等待/暂停标志）
```

watchdog：HANG ce=370；input_debug：按键 0x40/0x10 触发模式与 log21/22 一致。

### 因果链定论（三条证据交叉闭合，非推测）

1. **`pump=370 == CE=370`**：挂死后事件泵零执行——泵停在最后一次正常 CheckEvents。
2. **`switch=512169` vs `fast=377 slow=0`**：51 万次切换全部直接打在 switch_thread 上，yield 两车道在正常期后就冻结。**空转环是纯 Java 计算循环**（可能是 UI/Canvas 重绘忙循环），每量子经解释器 back-branch 检查点重入 switch_thread，但从不经过 yield() 或 start0。
3. **v01.83/v01.93 的泵位全部挂错路**：yield 快车道（wake_up_timed_out_sleepers 入口）与 start0 埋点都在这条环不经过的路径上——这就是历轮修复无效的原因。
4. 队列真空 + 5 线程无标志：所有 Java 线程都在等事件/监视器唤醒（callSerially/monitor wait），而唯一能送事件的事件泵已停摆 → 无人入队 → 无人可调度 → 死锁自锁。

### 修复（Scheduler.cpp switch_thread VITA 块，+19 行）

```cpp
Scheduler::_vita_switch_count++;
Scheduler::pump_events_vita();   // v01.99 FIX: 50ms 节流泵挂在唯一必经检查点
```

- **为什么安全**：pump_events_vita 自带 50ms 墧钟节流 + timer_ticked/_estimated_event_readiness 门；健康 VM 每 50ms 至多一次 check_blocked_threads(0)，开销可忽略。
- **为什么有效**：switch_thread 是解释器 back-branch 量子检查点，**每量子必经**。泵活过来 → JVMSPI_CheckEvents 抽干原生事件环 → 唤醒阻塞的 Java 线程 → 重新入队 → 打破死锁。ANI 模式 CheckEvents 每次最多睡 50ms，同时给空转环加上背压。
- 内联证据：`_ZZN12JVMScheduler16pump_events_vitaEvE17vita_last_pump_ms`（bss 静态局部）出现在最终 ELF——泵已内联进 switch_thread 热点。

### 构建/提交记录

- 提交 `9da738c`（phoneme-cldc + vita-port 同 commit）；VPK 产物 `vita-port/out/vpk/midp_vita_v01.99.vpk`(3650733B)/`.sym`(4284208B)。
- 构建流程照旧：`rm _MergedSrc002.o _MergedSrc003.o` → `rebuild_vm.sh`（33 对象，29 成员重打包）→ `cmake --build build -j8`。
- exe 链接报 `fast_memclear` 未定义是**预期行为**（脚本内注释注明，`|| true` 吞掉；库目标不受影响）。

### 遗留

- 待真机/Vita3K 复测：同样按键序列（0x40/0x10 交替数次）不再挂死；观察 site_ring 中 pump 计数在 HANG 期不再停滞。
- 0x810f71b4/0x810f521c（ring 槽值）未逐一符号化——数据已定案，无需回溯。

## 2026-09-18 v01.80：**已安装标签方向键失效**——v01.78f 的 `else` 分支吞掉了"列表模式 tab 0"（最小逻辑落空）

> 用户复测 v01.79：**真机启动不再崩溃**（v01.79 修复确认有效），但报出新 UX 缺陷——"在已安装游戏那里，没有上下移动切换游戏，只能在设置标签中上下切换"。

### 根因（读代码即定案，无需转储）
方向键光标分发器（`vita_menu_run` 主循环内）在 v01.78f 被写成：

```c
if (tab == 0 && set_list_grid == TAB_GRIDMODE) { psel = &gsel; ... }   /* ← 带了模式条件 */
else if (tab == 1) { ... } else if (tab == 2) { ... }
else if (tab == 3) { psel = &setsel; nent = SET_N; }
else { has_cursor = 0; nent = 0; }                                     /* ← 本意只兜 tab 4 退出 */
```

最后那个 `else` 只是给 tab 4（退出，无列表）准备的，但**第一个 `if` 额外带了 `GRIDMODE` 条件**，于是最常见的组合——**tab 0 + 列表模式（默认视图）**——不匹配任何分支，直接掉进 tab-4 的 `else`，`has_cursor = 0` → UP/DOWN 完全不动。**设置标签能动纯粹因为它有独立分支**。网格模式不受影响（命中自己的 `if`），所以按 SQUARE 切视图看起来像"绕过办法"。

**这是一类错误的教科书样本：用"多条件 if"表达"某个 tab"，再用裸 `else` 兜"另一个 tab"，中间的组合必然漏。** v01.78f 修的是同一类 bug（tab 3 抢 tab 0 的光标），结果自己在隔壁引入了它的镜像。

### 修复（最小 diff，`vita-port/src/vita_menu.c`）
- 分支改为 **`if (tab == 0)`**，列表/网格的选择移到**分支内部**：网格把 `psel/ptop` 换成 `gsel/gtop`，列表保持 `sel/top`；`nent = game_count` 两条路径共用。
- **一个 tab 一个分支**：以后新增 tab 不会再继承 tab 4 的 `else`。
- 1 秒心跳补记 `tab=%d focus=%d view=%c`——"哪个 tab 拥有方向键"这类问题现在从 `crumb.log` 一眼可判，不必再改代码复现。

### 交付 / 验证
- `out/vpk/midp_vita_v01.80_tab0dpad.vpk`（v01.80 **b265**，内嵌 hash `1a1923f`，md5 `20ccfad5b1c1bd9e907e2fdb21b454c5`，3640308 B）。提交 `1a1923f`。
- 真机复测要点：① 已安装**列表**视图 UP/DOWN 移动光标（本次 bug）；② 已安装**网格**视图 UP/DOWN 按行、LEFT/RIGHT 按格（回归）；③ 未安装/文件/设置 三标签方向键（回归）；④ 退出标签方向键应无反应（v01.78f 行为保持）；⑤ 启动/退出多轮（v01.79+v01.78h 回归）。

### 教训（新增）
- **条件分支覆盖要穷举**：分发器的每个 tab 都应有自己的分支；用裸 `else` 兜"特殊 tab"时，前面的分支条件必须与 tab 一一对应，不能夹带模式/状态条件。
- **两个相邻 bug 同源**：v01.78f 与 v01.80 是同一个"tab↔光标"映射问题的正反两面。改这类表驱动分发时，先把 tab × 模式 的全组合列出来再写分支。
- **心跳日志要带决策变量**：本次若 v01.79 的心跳已含 `tab/focus/view`，用户报障时看 `crumb.log` 即可定性，无需猜测。

## 2026-09-18 v01.79：**真机启动崩溃定案**——taiHEN 插件桩砸 r7（-O0 帧指针），`-fomit-frame-pointer` 根治（重要！）

> 用户报告"真机启动报错"并附 `psp2core-1789660610-0x0000142ca1` 转储。**首次真机验证 v01.78h**（此前全部版本只在 Vita3K 验证）。本轮解析 Sony core dump（gzip → ARM ELF core，24 个 NOTE）完成取证。

### 真机转储解析方法（新工具链，务必记住）
- `psp2dmp` = gzip 压缩的 ARM ELF core。解压后 `readelf -l`：43 段 = 15 个 `0xe0xxxxxx/0x81xxxxxx` 内核/插件段 + 2 组重复的用户段（0x81641000 栈 0x40000、0xd0001000）+ 24 NOTE。
- NOTE 按**标准 ELF note 格式**解析（namesz/descsz/type + 名字）：`THREAD_REG_INFO`(0x1004)、`PROCESS_INFO`(0x1002)、`MODULE_INFO`(0x1005)、`THREAD_INFO`(0x1003)、`STACK_INFO`(0x101b)。
- **MODULE_INFO 里找 `midp_vita` 条目**：名字后 +0x30 附近是 `base/size`（本次 `0x8106f000 / 0x2c6efc`）。**真机加载器滑动段**（本次 +0x6f000，v01.67 时代观测过 +0x72000）——**重定位现场地址时必须减去模块基址偏移**！
- THREAD_REG_INFO 布局（实测）：`+0x00` 头、`+0x0c` tid(0x178)、`+0x10` CPSR=0x60010030(abort+Thumb)、`+0x14` r0、`+0x28` r7、`+0x44` sp、`+0x48` lr(内核 abort 句柄 0xe00100bf)、`+0x4c` **pc**；`+0x174` 故障状态(0x807=写权限)、`+0x178` FAR。
- 文件名里的异常地址（`0x142ca1`）是野跳转后的 PC，真实现场在 THREAD_REG_INFO 的 pc 里。

### 崩溃链（全部实测）
1. 重定位后故障 PC = **0x81004a1a = `str.w r3, [r7, #812]`**（v01.78h b257 的 `vita_menu_run+0x134`）——**`fe_scan()` 返回后第一条局部初始化**（`tab=0`），即菜单从未画出来，崩在启动五连调用之后。
2. **r7 = 0x81000000**（= 链接基址 = henkaku 用户态系统调用号区间起点 `SYSCALLS_USER`）；FAR=0x8100032c（0x81000000+0x32c，写只读 .text）→ data abort。
3. **全 256KB 栈扫描：0x81000000 仅出现一次**——在 fe_scan 的 `push {r7,lr}` 槽（0x81680648）⇒ **r7 在 fe_scan 入口前已被砸**，且不是从栈恢复的（是直接被写进寄存器）。
4. **MODULE_INFO 显示进程内有 taiHEN 插件 oclockvita、Framecounter**：它们的 hook 桩是 16 字节 ARM `ldr r7,=syscall_id; svc 0; bx lr`——**砸 r7 且从不恢复**。
5. Vita3K 把 hooked 导入走自家 HLE thunk（完整保存 guest 上下文）⇒ **模拟器永不复现**。
6. CMAKE_BUILD_TYPE 为空 → GCC 默认 **-O0** → **每个函数都保留 r7 帧指针** → 菜单启动链（settings_load/menu_touch_init/scan_games/inbox_scan/fe_scan 全是 sceIo*/sceTouch*/sceAppMgr* 重度用户）任一被 hook 调用返回即帧指针报废 → 下一次局部写命中只读 .text。
7. 排除项：字面池扫描（830 个被引用池中无"无重定位的绝对指针"）；veneer 检查（--pic-veneer 仍在且全部 PIC 形式）；fstubs 桩正常。

### 修复
- **`CMakeLists.txt` 给 vita-port 自己的 C/C++ 对象加 `-fomit-frame-pointer`**（v01.79）。反汇编验证 b258+：`vita_menu_run` 序言不再 `add r7,sp,#N`，全部局部经 r3/sp 相对寻址；r7 只在序言/尾声保存恢复——**活动 r7 被砸已无影响**。
- 副作用评估：Thumb 回溯走 `.ARM.exidx` 仍可用；port 层无人读 r7 当 FP；菜单 UX 不变。
- **同时收编 Vita3K 退出卡死家族（v01.78b~h 全系）**：菜单不再依赖 r7 穿过任意 callee 存活。v01.78h 的 longjmp 逃生与哨兵保留（防御纵深：万一还有别的破坏者，哨兵会报 `menu sen BAD`）。

### 与 v01.67 的关系（用户提示"参考 cmakelist 记录"）
- v01.67 = 静态构造期崩（main 之前）：ld 默认 ARM→Thumb veneer 的绝对字面量无重定位条目 → 真机跳到滑动前的旧地址。修复 = `--pic-veneer`。
- v01.79 = 菜单启动期崩（main 之后）：taiHEN 桩砸 r7（-O0 帧指针）。修复 = `-fomit-frame-pointer`。
- **同族规律：真机专有 = (段滑动 | 插件注入) 与 (链接期假设 | ABI 契约) 的交集；Vita3K 两者都不模拟。** 排查顺序：① 模块基址偏移重定位现场 → ② 扫无重定位的绝对地址 → ③ 查进程内插件 → ④ 查 ABI 契约（帧指针/syscall 寄存器）。

### 交付 / 验证
- `out/vpk/midp_vita_v01.79_nofp.vpk`（v01.79 **b259**，内嵌 hash `ed8b2c8`，md5 `4108040449474047bc3c74bcee3d24e2`，3639675 B）。提交 `ed8b2c8`。
- 真机复测要点：① 启动进菜单（本次崩点）；② 菜单操作（浏览/安装/设置——都是被 hook 系统调用的重度路径）；③ 退出菜单→进游戏→退出→再进菜单多轮；④ Vita3K 回归（启动/退出/游戏全流程）。
- 若真机仍有问题：转储在 `samples/j2me/debug/logs/`（本次就是从那里找到的），按"真机转储解析方法"一节处理。

### 教训（新增）
- **真机 coredump 是最高价值证据**：寄存器 + 全栈 + 模块表一次拿全，比 Vita3K 日志强一个量级。用户侧只要把 `psp2dmp` 拷进 `debug/logs/` 即可。
- **-O0 + 帧指针 + 插件环境 = 炸弹**：任何"自己的代码"在别人 hook 的世界里跑，都不能假设 callee 保存 ABI 寄存器。
- 旧转储（9-14 的 3 个，v01.7x 时代）寄存器现场签名不同（pc 在 VM 区），属另外的崩溃，未分析——若 v01.79 后真机还崩，先比对它们的 MODULE_INFO/寄存器。


## 2026-09-17 v01.78h：**退出卡死真定案**——保存寄存器块被会话中途清零，退出改走 longjmp（治标+取证）

> 用户复测 v01.78g："退出，依然卡死"。本轮用 nm/objdump/readelf/od 取证**推翻 v01.78g 的全部归因**（PNG 行距砸堆 + 退出批量 free——那些修复本身没错，但不是本次崩溃的原因），锁定真正崩溃帧并给出确定性修复。

### 取证链（全部实测，非推测）
1. **崩溃 PC = `vita_menu_run` 尾声 `ldmia.w sp!, {r4,r5,r7,r8,r9,sl,fp,pc}`（0x8100682a @ b255）**：`nm -n` 界定函数边界 `0x810048b2..0x8100682d`，objdump 序言 `stmdb sp!,{…lr}` + `sub.w sp,#800`、尾声 `add.w r7,#776; mov sp,r7; ldmia.w sp!,{…,pc}` 全平衡。
2. **崩溃 `SP=0x80000328` = 入口 SP−32 = 保存寄存器块本身**；pop 出的 `r4..r11` 全 0、PC=0 ⇒ **保存块被零填充**。
3. **`LR=0x81010a3b` 是陈旧值**（`crumb_flush` 内 `blx sceIoClose` 的返回地址，落在函数边界之外）——**不可作归因依据**，上轮误用。
4. **栈溢出被推翻**：VELF 文件偏移 0x2d65a0 的模块参数区 `od` 读出 `main_thread_stacksize=0x18000`（96 KB），实际用 ~3 KB。
5. **crumb.log 决定性证据**：心跳健康跑到 30000+ flips（约 8 分钟），`==== menu exit ====`、`menu exit: have_sel=0 games=8` 全部写出，**然后** pop 才读到全零 ⇒ 零写在会话中途某刻发生，pop 只是唯一读点。**只改退出路径永远无效。**

### 根因状态
- **谁写的零未定**（帧内无局部数组能到保存块：`msg[120]` 在帧中下部，帧顶 528..776 全是 4 字节标量槽；所有 memset/memcpy/snprintf 均有界）。嫌疑面：另一线程或 HLE 钉住的钉子（如 `sceAppMgrGetDevInfo` 的 HLE 实现写内存）宜用哨兵下次会话夹逼。
- 崩溃签名口诀（新教训）：**崩溃寄存器全 0 + PC=0 + SP 恰为某函数帧的入口SP−(保存块大小) ⇒ 优先怀疑该函数帧被异步清零，而非调用链下游。LR 须先用 nm 验证是否落在函数边界内，陈旧 LR 不可归因。**

### 修复（确定性，不再依赖找到写者）
- **退出不走尾声 pop**：`vita_menu_run` 所有出口改 `longjmp(vita_menu_escape, 1)`；`main` 调用前 `setjmp(vita_menu_escape)`，返回后读 `.bss` 的 `vita_menu_result`。jmp_buf 在 `.bss`（0x812f6338），清零者够不到；反汇编验证：退出路径 `crumb_flush → str vita_menu_result → bl longjmp`，**无任何 `ldmia …,pc`**。
- **`sel` 改 static**（C99 7.13.2.1p3：setjmp 后经指针修改的非 volatile 自动变量 longjmp 后读取是 UB；同时彻底免疫栈清零）。
- **取证哨兵保留**：帧内 `volatile unsigned sen[8]`（0xC0DE0000+k 图案），心跳每秒校验，一旦被清零打 `menu sen BAD i=… w=… want=…`，下次会话即把清零时刻夹逼到 1 秒窗口，再结合 tab/动作定位写者。

### 交付 / 验证
- `out/vpk/midp_vita_v01.78h_longjmp.vpk`（v01.78 **b257**，内嵌 hash `5ecf5aa`，md5 `7b49312290b132c5b1ab5e1bd98e65c5`，3641291 B）。提交 `5ecf5aa`。
- 复测要点（Vita3K）：① 点"退出"标签；② 焦点在标签栏对"退出"按 X；③ 选游戏→退出→再进菜单循环；④ 若仍异常，看 crumb.log 有无 `menu sen BAD`（有 ⇒ 帧内哨兵被碰，把那 1 秒内的操作告诉开发者；无 ⇒ 写者目标不是本帧，另查）。

### 上轮结论更正
- v01.78g 节的"PNG 行距砸堆 + 退出批量 free ⇒ 退出闪退"归因**不成立**（修复保留，但与本崩溃无关）；`LR=0x81006a04 bl icon_cache_restore_kept` 的归因当时就未验证函数边界。

## 2026-09-17 v01.78g：退出闪退第一轮（归因已在 v01.78h 更正）

> ⚠️ 本节归因已被 v01.78h 取证推翻（崩溃帧实为尾声 pop，与图标释放无关），但 PNG rowbytes 校验、退出不动堆、install 路径越界修复本身正确，全部保留。

### 崩溃现场取证
- Vita3K 日志（Windows 宿主）：`PC: 0x00000000  SP: 0x80000328  LR: 0x81006a09`，`r4-r11` 全 0，`r2=0x3F(63)`；同一帧反复报 `Invalid read of uint32_t at address: 0x0, 0x4, 0x8, ... 0x1774`。
- `LR=0x81006a09` ⇒ 反汇编 `0x81006a04: bl 0x81002dd4 <icon_cache_restore_kept>` ⇒ **崩溃在菜单退出收尾的图标缓存释放序列里**（不是 main、不是 VM）。
- 非法读地址从 0x0 线性递增 ⇒ `free()` 在**已损坏的 malloc 链表**上追垃圾指针；`PC=0` 是链表里取到空函数指针/非法跳转的结果。**先有堆损坏，退出时才爆**。

### 根因（两处叠加）
1. **`vita_icon.c` 假定 libpng 输出恒为 RGBA**：`pixels = malloc(w*h*4)` 且 `rows[y] = pixels + y*w`，但 `png_set_filler()` 对**已经带 alpha 的 PNG 会被 libpng 忽略**，调色板/tRNS/16bit 组合也可能得到别的通道数。rowbytes 与假设不符时 `png_read_image()` 按 libpng 的真实行距写 ⇒ **每行越界、砸坏 malloc arena**，损坏很久之后才在 `free()` 里爆（典型的"退出才崩"）。
   - **修法**：`png_read_update_info()` 后用 `png_get_rowbytes(png, info)` 校验 `== w*4`，不符**直接拒绝**（返回 -1，图标退化为纯文本行），绝不按假设写。目前所有归一化路径（palette/tRNS/gray/filler）都应收敛到 RGBA，因此正常图标不受影响。
2. **v01.78f 的 park/restore 让退出路径去 free 64 个纹理槽**：`icon_cache_clear()` + `icon_cache_restore_kept()` 在退出时批量 free。堆已被 VM 折腾过，这里就是崩溃点（`r2=0x3F` 正是 `ICON_CACHE_MAX-1` 的循环常量）。
   - **修法（本版核心）**：**退出路径完全不动堆**——删掉 `icon_cache_keep()`/`icon_cache_restore_kept()` 与 `cache_hit_pix/w/h[]` 三个数组；释放改到 **`scan_games()` 开头**（每次扫描先把 64 槽全释放再重填），保证槽位永不留悬垂指针，且释放点与 VM/堆活动完全隔离。park/restore 本来也买不到什么：`cache.bin` 已经缓存了 PNG **字节**，重扫走内存解码，不需要"续命纹理"。

### 顺带修掉的真缺陷
- `icon_load()` 里 `if (!vita_icon_decode_png(...))` **判断反了**（该函数 0=成功、-1=失败）⇒ 真失败不打印、每次成功反而打一条 `png decode FAILED`。改为 `!= 0`。
- `install_inbox()` / `install_jar()` 的 `base[blen - 4] = '\0'` **无界下标写**：`ent.d_name`/basename 可长于 `base[128]`，`snprintf` 截断后仍按原长索引 ⇒ 栈越界。改为用**拷贝后** `strlen(base)` 重新判断并截断。

### 新增退出埋点（保险）
- 退出收尾加 `crumb_marker("menu exit")` + `crumb_printf("menu exit: have_sel=%d games=%d")` + `crumb_flush()`。下次若仍崩，`ux0:/data/J2ME00001/crumb.log` 直接说明死在"退出前"还是"退出后"，不用再猜。
- 注意 crumb 走自己的 sceIo 通道，不进 midp_stderr.log。

### 交付 / 验证
- `out/vpk/midp_vita_v01.78g_tabui.vpk`（v01.78 **b255**，内嵌 hash `aca83cc`，md5 `63893f77362e8cabde3fb29155e74cab`，3640913 B）。
  - 内嵌 hash 是**构建时的树状态**（提交前）：本版改动先提交为 `aca83cc`，随后因补录本条记忆 `--amend` 成 `8f5ea66`——VPK 里的 hash 与最终提交号不一致是 amend 的正常副作用（同 v01.78e/`1d39c08` 的情形），代码内容一致。
  - 注意 `--amend` 会改提交号：**若在乎内嵌 hash 与提交号一致，就"先提交、再构建、不 amend"**（本次教训）。
- 复测要点（Vita3K）：① 点"退出"标签；② 焦点在标签栏对"退出"按 X；③ 退出后重启游戏确认菜单可重入；④ 图标仍正常显示（若某图标变纯文本，看 midp_stderr.log 的 `[icon] png decode FAILED`，说明该 PNG 行距不是 w*4，属预期防御）。

### 工具链经验（本轮新增，务必记住）
- `midp_vita.velf` **已剥符号且无 DWARF** ⇒ `addr2line` 全 `??:0`、`nm` 为空。必须用同目录**未剥离**的 `midp_vita` 配 `arm-vita-eabi-nm -n midp_vita`；反汇编必须 `arm-vita-eabi-objdump -D -M force-thumb`（默认按 ARM 解码 Thumb 出乱码）。
- `vita_version.h` **未被 git 跟踪**，构建前必须 `rm -f vita_version.h && touch CMakeLists.txt`，否则版本号/ hash 与提交不符。
- **崩溃日志在 Windows 宿主 `C:\Users\zyb\Downloads\windows-latest\vita3k.log`，容器内无法访问（`/mnt/c` 不存在）** ⇒ 取证要么让用户粘贴日志，要么在代码里自证（`crumb.log` / `midp_stdout.log` / `midp_stderr.log`）。日志链路：`tty->print*` → stdout → midp_stdout.log；`fprintf(stderr)` → midp_stderr.log。
- 教训：**"退出瞬间崩"优先怀疑堆损坏**（越界写把 arena 砸了，触发点在很久之后的某个 free），而不是"退出路径本身写错了"。**（v01.78h 补注：本条对本菜单纯退出场景不成立——实测是尾声 pop 读到被清零的保存块；但结论对 free 序列崩溃场景仍有效。）**

## 2026-09-17 v01.78：TV 风格标签菜单——五标签 + 触摸 + 文件浏览器（未上机）

> 用户需求："菜单界面做下优化…类似 TV 样式，左侧标签栏、右侧内容栏，点击或按不同标签显示不同内容；标签：已安装（单列表/网格可切换）、未安装（自动扫描+缓存记录）、文件（文件管理）、设置、退出"。`vita_menu.c` 主循环重写（+661/-55），扫描/安装/对话框内部逻辑不动。

- **布局**：顶栏 52px（标题+版本+计数）+ 左栏 220px 五标签（中文，选中高亮+竖条）+ 右侧内容区。`TAB_N 5`，L/R 肩键或点左栏切标签；点"退出"标签直接退出。
- **已安装**：SQUARE 列表↔4×3 网格（84px 图标），选择持久化 `ux0:/data/J2ME00001/menu.cfg`（单字符 'l'/'g'）；网格方向键按行移动（上下 ±cols），tap 格子=选中+开对话框；对话框索引前先 `sel=gsel` 同步（对话框用 `games[sel]`）。
- **未安装**：`inbox_scan()` 菜单启动+每次安装后重扫（dopen 遍历很轻，"缓存"就是磁盘上的 jar 本身）；X 全装（`install_inbox`），tap 单装（`install_jar`）。
- **文件**：沙盒根 `ux0:/data/J2ME00001`，`fe_insert` 插入排序保持".."→目录→.jar→其他文件分组序；X/tap 目录=进入、jar=安装（与 inbox 同一 rename+cfg 路径）；O 回上级（".." 条目也覆盖，冗余保留）；SELECT 刷新。
- **设置**：游戏视图切换（X/tap，持久化）+ `sceAppMgrGetDevInfo` 的 ux0 容量 + games/inbox 计数 + JIT 提示 + 数据目录。
- **触摸**：`menu_touch_init()` 开采样（`sceTouchSetSamplingState START`，不开则 reportNum 恒 0）+ `sceTouchGetPanelInfo` 动态 max（真机 1920×1088 / Vita3K 960×544），`poll_tap` down→up 沿触发；归一化同 `vita_input.c`。
- **消息**：4 秒自动过期（`msg_us` + `sceKernelGetProcessTimeWide`），不再永久驻留。
- **自查修复 5 缺陷**：网格对话框光标不同步；网格滚动单位混用（gtop 行单位独立 clamp）；".." 只画不可选（改为 fe_scan 虚拟槽 0 真条目）；inbox tap 误全装改单装；DATA_ROOT 宏序 + fe_parent 前向声明。**另修两次编辑事故**：tab2 CROSS 体误删（恢复 X=打开/安装）；旧变量声明块残留（重定义编译错）；`fe_scan` 首尾双向填充紧缩循环目录/文件交错序（重写为 `fe_insert` 插入排序）。
- 交付：`out/vpk/midp_vita_v01.78_tabui.vpk`（v01.78 b247→e4b8440, md5 13a622d9b41a3a56dc409375e2b2903f）。**未上机**——触摸归一化已按 PanelInfo 动态适配，但真机手感（tap 命中区、网格格子大小）待验。
- 遗留小疵：网格名称超宽不截断（画到下一格）；对话框无 tap 操作（仅按键）；`vita_menu.c` 内有几处 `-Wformat-truncation` 警告（snprintf 截断安全，非错误）。

## 2026-09-17 v01.77：内存/输入/图形审查轮——display flip 补 vblank 同步（未上机）

> 规范审查第二轮（内存+输入+图形）。**内存全绿**：每函数 malloc/free 扫描 7 处失衡命中逐一复核，全部是契约式返回（pcsl_string 三兄弟由上游 `pcsl_string_free` 释放、`javacall_malloc` 配 `javacall_free`、`openfilelist` 配 closefilelist、`addrToString` 调用方释放、`fb_load` 错误路径全走 `fb_fail`（free+NULL）成功路径进程持有）。CDRAM 块（menu 2MB + display 4MB）分配/粒度/释放路径全合规。**输入合规**（DIGITAL 采样模式合法、触摸归一化正确、主指单点符合 MIDP）；input_debug.log 每条 open/write/close 保留不动——挂死取证还要用。

- **`flip_to_display()` 补 `sceDisplayWaitVblankStart()`**（vita_display.c）：NEXTFRAME 要到下个 vblank 才生效，此前扫描输出仍读旧前台；Java 快速连环 repaint 时下一次 blit 可能在换页落地前写完刚交换的后备帧 → 撕裂。menu 侧 `menu_flip()` 一直有成对等待，display 侧没有。顺带把刷新率规约到 60Hz（软件 blit 无所谓）。
- **`sceDisplaySetFrameBuf` 返回码记账**：失败打一次 stderr（`static int reported` 限流）——v01.69 就是靠 menu 侧这个 rc 发现 IMMEDIATE 在真机 3.65 报 0x80290006 的；失败时不交换缓冲（旧前台继续显示）。
- 注释两处（不动代码）：`lfjport_ui_finalize` 释放 CDRAM 块的至多一帧扫描输出窗口（只在 VM 关机时发生，可接受）；`ring_push` 写死 SPSC 前提（生产者=消费者=VM 线程，加第二个生产者必须改临界区）。
- 交付：`out/vpk/midp_vita_v01.77_gfxsync.vpk`（v01.77 b245, a0d153e, md5 89f8d3d6c1c187d758666afe08bbe519）。**未上机**——下次真机会话直接用这版（覆盖 v01.75/76 的验证计划不变）。

## 2026-09-17 v01.76：音频路径审查修复（未上机，与 v01.75 一并等真机验）

> 全端口 SDK 规范审查（每函数 fd 配对扫描 + 人工复核）后的 3 处修复。fd 审计结论：全部配对（持久 fd 是设计，pcsl 句柄走 closefile 收尾）。

- **`alog()` 自旋锁原子化**：旧 `while (lock); lock=1` 是 check-then-set，Vita 3 用户核下真竞态（VM 线程 + tone 线程并发调 alog）——交错行会污染挂死取证证据。改 `__sync_lock_test_and_set/__sync_lock_release`（ARMv7 ldrex/strex）。
- **删除 `src/vita_audio.c` 死文件**：从未进构建；且其 `sceAudioOutOpenPort` 第 2 参数误当"声道数"传 1（实为每调用采样数），谁加回构建就是潜伏 bug。git 留档。
- **`j2me_tone` 亲和裸 0 → 宏**：与 watchdog 统一 `SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT`。
- 低优先已记录不动：`vita_net.c` 4 处 strcpy（inet_ntoa 最长 16B + PCSL 缓冲 ≥256B，实际安全）；`vita_main.c` mkdir 返回值未查（freopen 链会连锁暴露）。
- 交付：`out/vpk/midp_vita_v01.76_review.vpk`（v01.76 b244, 33d42f2）。**未上机**——下次真机会话用这版，watchdog.log 预期不变（alive 首行 + HANG 双时钟行 + 三线程快照）。

## 2026-09-17 v01.75：看门狗规范审查版（未上机，等真机一并验）

> 对照 vitasdk 头文件 + 本项目真机已验证实践的 code review，4 处修复，正常路径无行为变化。

- **栈 0x2000 → 0x4000**：dump 链路（栈上 SceKernelThreadInfo ~200B + snprintf + sceIo*）8KB 偏紧；溢出 = 内核杀线程 = 看门狗无声消失。0x4000 对齐 `j2me_tone`（项目验证过的下限）。
- **双时钟取证**：HANG 报告同时采 `gettimeofday` 与 `sceKernelGetSystemTimeWide()`——若 H2（时钟冻结）为真，**哪一个冻住**直接点名故障层（newlib/sceRtc 链 vs 内核时间子系统）。新格式：`[WD] HANG ce= polls= clock t0/t1/d= | kclock k0/k1/kd=`（kd≈100000us=内核钟健康）。
- **亲和掩码用宏**：`SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT` 替代裸 0。
- **删死包含**：`<psp2/kernel/processmgr.h>` 无对应调用。
- 交付：`out/vpk/midp_vita_v01.75_watchdog4.vpk`（v01.75 b242, 2792319）。**未上机**——下次真机会话直接用这版，watchdog.log 预期：`[WD] alive` 首行 + 挂死后 HANG 行 + vm/tone/wd 三线程 wait 快照。

## 2026-09-16 v01.74：看门狗优先级非法——真机与 Vita3K 的又一差距

> log6 判定：`[WD] create FAILED rc=0x80028023` **每次启动必现**（6/6）= `SCE_KERNEL_ERROR_ILLEGAL_PRIORITY`。

- **根因**：看门狗优先级 0x10000300 在**真机**用户态非法；Vita3K 接受它（模拟器差距 +1：上一个是 freopen 前目录不存在）。本项目真机长期验证过的合法优先级只有 **0x10000100**（`cldc_ticker`、`j2me_tone`）。修复：降到 **0x10000150**（数值越大优先级越低，仍保持"进程内最可抢占"）。
- **v01.73 的存活行设计立竿见影**：`[WD] alive` 行缺失 + FAILED 行存在，一眼定案——"静默失败不可接受"再次被证明。
- **log5 附带收获**：挂死是**概率性**的——8 轮 round 里文件管理器、ToneTest、封神榜全部正常进出多轮，最后挂在文件管理器第 2 轮。**不是首次必现**，复现要多次进出。
- 经验沉淀：**真机线程优先级合法区间以本项目已验证值为准**（0x10000100 一档），新线程优先级不要超出已验证区间；模拟器过 ≠ 真机过。
- 待回收：v01.74 真机 watchdog.log——这次 `[WD] alive` 应出现，挂死后应有 `[WD] HANG ce= polls=12 clock t0/t1/d=` + vm/tone/wd 三线程 wait 快照。

## 2026-09-16 v01.73：看门狗 v2——免时钟检测 + 启动存活行（log5 教训）

> v01.71 (b235) 真机日志（debug/logs/log4/）判定结果：**H3 出局、音频无罪、泵彻底停转**。

- **铁证**：`ani_mark.log` 末态 `CE #115 ANI OUT`——最后会话（封神榜）泵在启动 ~4.4s 后永久停转，且不在 ANI 等待内；`audio_debug.log` `tone done: chunks=38`（400ms×16k×3=19200 帧=37.5 块，数学吻合）——音频线程完整播完无错误。
- **判读修正**：v01.71 心跳每 1024 次泵调用（≈40s）才采样一次，而挂死发生在 2~10s——**H2（时钟冻结）从未被真正排除**。
- **剩余二选一**：(a) VM 线程阻塞在某内核等待（sceIo/互斥/cond/延迟）；(b) VM 线程满转（Java/GC 死循环，永不阻塞所以永不进泵）。
- **v01.72 新增 `vita_watchdog.c`**：低优先级线程盯 `vita_ce_count`，MIDlet round 内停转 >3s 即快照 VM/tone 线程的 `sceKernelGetThreadInfo`（status+waitType+waitId——wait 类别直接点名阻塞原语）+ 双 gettimeofday 采样（d==0=时钟冻结实锤）。每次挂死只报一次，追加到 `watchdog.log`。
- **接线**：`vita_tone_tid` 由 `vita_audio_javacall.c` 导出；`vita_main.c` 在 round begin/end 置 `vita_wd_round_active`、记录 `vita_wd_vm_tid = sceKernelGetThreadId()`（主线程即 VM 解释线程）。
- **附带确认**：`vm_output.log` 链路（JVMSPI_PrintRaw→crumb_append）无缓冲崩溃安全，"最后一行=挂死点"前提成立；封神榜挂点在 `[pcsl] open NEZHA_DOWN.db` 入口行之后。

## 2026-09-16 v01.71：ToneTest 挂死诊断版——三判定点探针 + 音频日志毁证据修复

> 用户回报：**"运行 ToneTest 的时候，会发出一下声响后，就没反应了"**。vm_output.log 止于 `stg1: playTone returned`，之后 2.5s stage 定时、1Hz 心跳、60s 看门狗全灭；但另一会话挂死时仍有 `[INPUT] buttons=` 记录（泵可能还活着）。

- **已排除**（静态取证结论）：playTone 本身（异步，bump `g_req_seq` 即返）；EOM 通知死锁（简单 playTone 路径不发 END_OF_MEDIA）；AudioBridge/vita_audio.c 路径（ROM 未用）；native 链接断链（ELF 符号全闭合）。
- **三互斥假设**：H1=VM 主线程卡在 ANI 等待**之前**的阻塞 native（repaint→`sceDisplaySetFrameBuf` 等）；H2=墙上时钟（`gettimeofday`=`Os::java_time_millis` 源）冻结→所有定时等待者永不超时；H3=卡死在 `ANI_WaitForThreadUnblocking`（pthread_cond_timedwait）内。
- **发现的独立 bug**：`vita_audio_javacall.c` 的 `alog()` 用 `SCE_O_TRUNC` 打开 audio_debug.log——每次进程启动截断，**上一会话（ToneTest）的音频记录被下一会话（UC）启动毁掉**。已改 `SCE_O_APPEND`。
- **探针布局**（全在 `ux0:/data/J2ME00001/`，判读表见 CMakeLists v01.71 注释）：
  1. `input_debug.log` 心跳行升级为 `#N ms=... d=... CE=n ani=e/x`——一行同时证明**泵活性**（N/CE 推进）与**时钟活性**（ms 推进）；ms 冻结+d 不变=H2。
  2. `ani_mark.log` 是 watchdog 槽：ANI 进/出各 `sceIoPwrite` 覆写偏移 0——心跳停 + 末态 `OUT`=H1（卡泵前段）；末态 `IN`=H3。
  3. `audio_debug.log`（追加式）+ tone 线程 `tone req`/`tone done: chunks=` 锚点 + `sceAudioOutOutput` 负返回码记录。
- **改动文件**：`vita-port/src/{vita_audio_javacall.c, vita_input.c, vita_checkevents.c}` + CMakeLists 版本块。
- **遗留疑点（未修，待判定后处理）**：`vita_pcsl.c:2169` `JVM_monotonicTimeMillis` 返回 0 stub——无法解释前 2.5s 时间正常，存疑。

## 2026-09-15 v01.70：真机闪烁定案——单缓冲直写扫描输出帧，双缓冲修复

> 用户回报 v01.69 **"画面显示了，但是一直在闪烁，感觉像是一直在刷新，之前 Vita3K 偶尔会这样"**。

- **根因**：菜单（每帧全屏重绘）和 LCDUI blit 都是**单缓冲**——直接画在显示控制器正在扫描输出的那块帧上，面板读到半成品帧 = 持续闪烁/撕裂。Vita3K 偶尔出现的同现象是同一个竞态（只是很少输）；v01.68 改 CDRAM 后写入变慢（非缓存），真机上竞态从"偶尔"变成"必然"。
- **修复**（提交 d4fe4c8）：两处 framebuffer 各自改为**单 CDRAM 块双帧 ping-pong**——画后备帧 → NEXTFRAME 提交 → 交换指针：
  - `vita_menu.c`：`menu_fb` 别名后备帧，`menu_flip()` 提交+交换；
  - `vita_display.c`：`flip_to_display()` blit 进后备帧、提交、交换；`lfjport_ui_finalize` 释放整块。
  - CDRAM 占用：菜单 4MB + 显示 4MB（256KB 粒度）。
- **产物**：`midp_vita_v01.70.vpk`（b232 (d4fe4c8)，MD5 `086d50d8912751d542426faaa11e7d5f`）。
- **真机五连坑（累计）**：PIC 跳板 / 非缓存 fb / NEXTFRAME / 双缓冲 / （诊断）crumb 心跳+防 FTP 缓存。下一个真机风险点不变：**音频 ring buffer 的缓存一致性**（"有画面没声音"就查它）。

## 2026-09-15 v01.69：真机黑屏真根因定案——`SetFrameBuf(IMMEDIATE)` 被真机拒绝（0x80290006）

> v01.68（CDRAM）后真机仍黑屏。**注意那两轮"还是黑屏"的 log 其实是 FTP 传输缓存了旧文件**（boot_log 显示 v01.67 b226、且无 crumb.log）——真机侧早已在跑 v01.68 b229。教训：**核对版本串，别信文件内容；取日志用改名法（复制成新文件名再拉）绕开 FTP 缓存**。

- **crumb.log 心跳数据（b229 诊断版）一步定性**：
  - `menu: fb=0x61000000 block=0x4001015f` → CDRAM 分配成功（0x61000000 正是 CDRAM 映射基址）；
  - `menu hb: flips=60/120/179...` 每秒 +60 → 菜单循环完全健康；
  - **`rc=0x80290006` 出现在每一次 flip** → `SCE_DISPLAY_ERROR_INVALID_UPDATETIMING`（psp2/display.h:24）。
- **根因**：两个 flip 点（`vita_menu.c menu_flip`、`vita_display.c flip_to_display`）都用 `SCE_DISPLAY_SETBUF_IMMEDIATE`。真机 fw 3.65 拒绝该更新时机、面板从未拿到 framebuffer。**官方三个样例（debugScreen/camera/ime）全部用 `SCE_DISPLAY_SETBUF_NEXTFRAME`**；Vita3K 不校验 IMMEDIATE——模拟器正常的第三个理由。
- **修复**（提交 a79ff3b）：两处统一改 `SCE_DISPLAY_SETBUF_NEXTFRAME`。菜单循环里每次 flip 后已有 `sceDisplayWaitVblankStart()`，NEXTFRAME 语义匹配。
- **产物**：`midp_vita_v01.69.vpk`（v01.69 b230 (a79ff3b)，MD5 `601a7ece827246df5078b0a7fef81a32`）。
- **真机四连坑（最终版）**：
  1. 段滑动 → 跳板必须 PIC（v01.67 `--pic-veneer`）；
  2. CPU 写/硬件读的缓冲必须非缓存（v01.68 CDRAM framebuffer）——**音频 ring buffer 是下一个待查点**；
  3. `SetFrameBuf` 更新时机必须 NEXTFRAME（v01.69）；
  4. **诊断日志是唯一可信源**：crumb 心跳 + 返回值记录一步定性；同时警惕 FTP 缓存伪造"修复无效"。

## 2026-09-15 v01.68：真机黑屏定案——framebuffer 用了缓存内存，显示控制器看不见（CDRAM 修复）

> 用户回报 v01.67 真机 **"启动后黑屏"**；boot_log.txt 出现并停在 `[media] tone player thread created`——pic-veneer 修复已生效（静态构造活下来了、日志能写了），崩溃变成了显示问题。

- **日志解读**：`[media]` 之后 main() 的下一批 dlog 只会在**菜单做出选择之后**才写（菜单自身不写 boot_log），所以日志停在这里 ≠ 卡死，恰好说明执行流进入了 `vita_menu_run()`。黑屏 = 菜单在跑但画面上不去。
- **根因**：`vita_menu.c` 的 `menu_fb` 和 `vita_display.c` 的 `vita_fb` 都是 `memalign()` 的**普通缓存堆内存**。真机上 Cortex-A9 的写回式 D-cache 把 CPU 的画面写留在缓存里，显示控制器扫描的是主存 → 面板永远黑。**Vita3K 不模拟缓存**（与 veneer 那次同类"模拟器专用正常"），所以模拟器一直 looks fine。全 port 层 grep 不到任何 Dcache 回写调用佐证。
- **修复**（提交 fc0008e）：新 helper `src/vita_fbmem.h`——`sceKernelAllocMemBlock(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW)`（256KB 粒度向上取整）分配 framebuffer，CPU 侧**非缓存**映射，对显示控制器即时可见；这正是官方样例（common/debugScreen.c、camera、ime）的做法。两处替换：
  - `vita_menu.c`：menu_fb 走 `menu_fb_blk`，块随进程存活（与旧所有权一致）。
  - `vita_display.c`：vita_fb 走 `vita_fb_blk`，`lfjport_ui_finalize` 里释放。
  - CMakeLists 链接 `SceSysmem_stub`（MemBlock API 属 SceSysmem 组）。
- **产物**：`vita-port/build/cmake/midp_vita.vpk`，v01.68 b227 (fc0008e)，3,634,904 B。
- **教训（真机三连坑，务必记住）**：
  1. 段滑动 → 跳板必须 PIC（v01.67 `--pic-veneer`）；
  2. 显示路径内存必须非缓存（CDRAM 或回写）；任何"CPU 写、其它硬件读"的缓冲（音频 ring、未来 GPU 纹理上传）同样适用——**下一步真机没声音的话先查 `vita_audio.c` 的 ring buffer 是不是同一类问题**；
  3. Vita3K 不模拟缓存也不滑动段——"模拟器正常"不能作为任何真机结论的依据。

## 2026-09-14 v01.67：真机启动即崩定案——ld 默认 ARM→Thumb 跳板无重定位（`--pic-veneer` 修复）

> 用户回报 **"启动即崩,菜单都没显示出来，vm_output,vm_stderr,net_log 还没创建"**。

- **症状**：真机（fw 3.65 + oclockvita/Framecounter 插件）一启动就崩，菜单不出现；`ux0:/data/J2ME00001/` 下**任何**日志文件（boot_log.txt / crumb.log / net_log.txt / vm_output.log / vm_stderr.log）都不存在。Vita3K 完全正常。两份 psp2dmp（v01.65 的 1789400876、v01.66 的 1789403835）现场完全相同。
- **根因**（证据链全部闭环）：
  1. THREAD_INFO 里崩溃线程 entry = 设备端 `_start+1`，对比本地 ELF 的 0x81001269 → **真机代码段被滑动 +0x72000**（插件注入量决定，两台机/两次启动可不同：v01.65 那份滑 +0x3d000）；RW 段独立滑动 **+0x120000**（本地 0x812e0018→设备 0x81400018、0x814a8170→0x815c8170，与转储 r2/r3 逐字吻合）。**Vita3K 恒按 0x81000000 装载，这就是模拟器永不复现的原因。**
  2. 加载器确实应用了 ELF 重定位（否则 movw/movt 装的地址不可能全是正确滑移值），前 3 个 `.init_array` 构造函数都跑完了（栈上残留 `__sinit`/`_malloc_r`/`pte_osTlsAlloc` 帧）。
  3. 但 ld 默认生成的**长距离 ARM→Thumb interworking 跳板**（`____X_from_arm`：`ldr pc,[pc,#-4]; .word 绝对地址`）的**字面量没有重定位记录**——readelf 验证 v01.66 ELF：38 个跳板、0 条重定位。
  4. 第 4 个构造函数（`_GLOBAL__sub_I__ZN8JVMFrame12_in_gc_stateE`，Frame.cpp 的静态对象析构注册）尾部 `b ____aeabi_atexit_from_arm` → 跳到**过期的链接期绝对地址** → 野代码 → data abort。dmp1 的 PC 恰好 = `__aeabi_atexit` 的链接期地址 0x810be194（铁证）；dmp2 落到 `str r2,[r7,#4]`、r7=0（写 0x4）。**崩在 main() 之前**，所以没有任何日志文件。
- **修复**：`CMakeLists.txt` 链接选项加 `-Wl,--pic-veneer`（提交 6b49df3）。38 个跳板全部变为 pc 相对序列（`ldr ip,[pc,#4]; add ip,pc,ip; bx ip`），随段滑动自适应；R_ARM 重定位总数不变（99653→99653），模拟器行为不受影响。
- **教训**：
  - 真机 coredump 符号化**必须先从 THREAD_INFO 的 entry 算出段滑动量再换算**，直接拿本地符号表解设备地址全盘皆错（这次先错解成 png/read_file/net 路径，浪费了一轮分析）。
  - "日志文件一个都没有" 本身就是最强证据：说明崩在 `main()` 之前，只有静态构造/加载器层面的问题才会这样。
  - 上游 CLDC 用 `-marm` 编 ARM 态代码、port 层是 Thumb，这种混合态在**段会被滑动的平台**上必须 `--pic-veneer`。

## 2026-09-14 v01.66：汉字的行盒适配（CJK 18px → 20px）

> 用户回报 **"文件根目录修改成功了，但是字体，英文显示还行，中文显示效果反而不太好了"**。
> v01.65 换字体库后拉丁变好、汉字反而更糟。**根因是几何而非字形**：只改 `tools/fontgen.c` 的一个常量，
> 重新生成银行并重打 VPK；**未重编 MIDP ROM**（字体库是打包资源，不进 ROM）。

### 一、根因：汉字坐在基线上填不满 22 行行盒

v01.65 给 CJK 用 `CJK_PX 18`，而行盒是 `CH 22` / `ASCENT 18`；汉字墨迹约占 em 的 0.85：

| | `CJK_PX 18`（v01.65） | `CJK_PX 20`（本版） |
|---|---|---|
| `中` 的墨迹行 | **5..19**（15 行） | **3..20**（18 行） |
| 墨迹上方空格 | **5 行** | 3 行 |
| 墨迹列 | 2..14 | 3..16 |
| section advance | **18 px** | **20 px**（= cell 宽） |

- 汉字**墨迹本身几乎没变**（13×15 → 13×16），变的是**位置与间距**：18px 的墨迹坐在基线上只够 15 行，
  上方空出 5 行、下方 1 行——中文看起来**又小又沉**；笔尖只前进 18px，比 20px 字身还紧 2px，密排时发挤。
- **拉丁不受影响**：它的墨迹本来就贴着基线（`A` rows 5..17），所以用户看到的是"英文还行，中文变糟"。
- **读旧库的坑（勿拿它当基线）**：v1 银行头部 offset 24 是"位图基准"字段，装机的 v01.64 加载器却把基准算成
  `off + nsec*8`，而旧银行真正基址就是 `off`——**旧库每个字形都错位了一个字形槽**（72 字节）。
  旧版"中文观感"有一部分来自这个错位。

### 二、修复与验证

- `tools/fontgen.c`：`#define CJK_PX 18` → `20`（注释写清两个字体为何用不同 em）。
  `LATIN_PX 20`、`CW 20`、`CH 22`、`ASCENT 18`、`NSEC 11` 与 v2 布局**全部不动**。
- **不裁字（逐字形扫描量化）**：`bash tools/gen_font.sh` 后扫全部 CJK 各区：`U+4E00..U+9FA5` 墨迹下缘
  **最大 row 20**（15,058 字形）、上缘最小 row 2；`U+FF01..U+FF5F` 下缘最大 row 21——都在 cell 的 0..21 内。
  `WARNING clipped: 0 above, 19 below, 53 sideways` 在 **18/19/20 三个字号下数量相同**（50/54/53），
  是拉丁 `xo = -1` 的正常左伸与既有的 19 个下伸字形，**与本次改动无关**。
- **拉丁零回归（分区字节比对，新库 vs v01.65 库）**：

  | 区域 | 差异字节 |
  |---|---|
  | header（0..32） | 0 |
  | 拉丁 advance 表 | 0 |
  | CJK advance 表 | 21,121（17/18 → 19/20） |
  | 拉丁位图 | **0** |
  | CJK 位图 | 796,659 |

  英文部分**逐字节相同**——正是用户说"还行"的那一半。
- 银行 **1,615,280 B 不变**（布局未动、只有内容变）：MD5 `af032dab…` → `1142aeab2860bb7c99b15f9db0c10aa9`。
- VPK：`cd vita-port && cmake --build build/cmake -j4`（**不需重编 MIDP ROM**）→ **v01.66 b222**、3,634,400 B。
  比 v01.65 大 ~100 KB 属正常：20px 汉字笔画更密、熵更高，压缩率下降。VPK 内
  `data/J2ME00001/fontbitmap.bin` 的 MD5 == `config/fontbitmap.bin` ✓。
- `strings build/cmake/midp_vita | grep "version: J2ME"` → `version: J2ME Player v01.66 b222 (60959ac)`。
- **`vita_version.h` 是 configure 时生成的**（`GenVersion.cmake`）：`VITA_PORT_BUILD` = 提交数、
  `VITA_PORT_HASH` = 当前 HEAD。所以**先提交、再构建**，hash 才指向本版代码提交；
  构建前的 HEAD 会滞后一拍（本版第一次构建得到的是 `b221 (5cc5a9c)`，touch `CMakeLists.txt`
  重新 configure 后才是 `b222 (60959ac)`）。

### 三、遗留风险 / 待验收

- **Vita3K 未上机复测**：汉字应填满行盒、笔画更实、字距回到 20px；英文应与 v01.65 完全一致。
- **`0xFF61..0xFF9F`（半角片假名）整区无字形**（等线字体没有；`sec 8` 无墨迹、advance 全 0）→ 渲染成 tofu。
  `0x2460..0x2487` 只有 10/40、`0x3000..0x303F` 35/64——**既有覆盖缺口**（字体 + section 表两方面），本版未处理。
- **CJK 仍是 1bpp**（无灰度）：20px 后笔画还原更好，但曲线边缘仍有锯齿。改 8bpp 会把银行从 1.6 MB 推到约 10 MB，**不建议**。
- `j2me/debug/*.psp2dmp`（崩溃转储）留在工作区，**未提交**。

## 2026-09-14 v01.65：JSR 75 根目录 = 设备存储卷（多 root）+ UI 字体换 J2FB v2

> 两条用户需求一次做完：(1) **"希望文件管理根目录是 psvita 的根目录，而不是当前应用的 data 目录"**；
> (2) **"字体能否换一个，现在的字体显示英文字母很奇怪，有些符号排版也很难看"**。
> 改动全部在 `j2me/jsr75`（Java + native）与 `j2me/vita-port`（字体链路 + 打包），**未碰 `phoneme_source/`、未碰 `cldc/build/share/`**。

### 一、多 root 模型（旧"单 root = data"彻底退役）

| 位置 | 改动 |
|---|---|
| `FileSystemRegistryImpl.java` | `ROOT_DEVS = {ux0,imc0,uma0,app0}` + `ROOT_ALWAYS = {T,F,F,T}`；`availableRoots()`（惰性 + `synchronized`）；`findRoot()`；`resolve(String) → String[3]`；`resolvePath()` 变成 `resolve(url)[0]` 的一行包装；新增 `trimSlash()` |
| `FileConnectionImpl.java` | 新增 `private final String rootName`，构造器改为 `String[] parts = resolve(url)` 一次拿三件；`getPath() = "/" + rootName + (relPath 空 ? "/" : "/" + relPath)`；`totalSize()/availableSize()` 改传 `absPath` |
| `FileStore.java` | `availableSize(String)`/`totalSize(String)` 改签名；`getRootPath()` javadoc 改为"启动器数据目录，**不是** JSR 75 root" |
| `jsr75_file.c` | 新增 `jsr75_is_dev_root()`；`isDirectory` 判定加一项；`availableSize/totalSize` 改为按参数路径取设备前缀；顶部"Root policy"重写（原有一段**重复粘贴**的段落，一并清掉） |
| `FileSystemRegistry.java` / `Protocol.java` | 仅 javadoc：root 是"每个已挂载存储卷的根"，示例 URL 改 `file:///ux0/data/x` |

- **root 名就是设备名**（`"ux0/"`、`"imc0/"`、`"uma0/"`、`"app0/"`），拿到就能直接拼 `file://localhost/" + root`，和 `C:/`、`E:/` 的既有平台习惯一致。
- `file:///ux0/data`（与 `file://localhost/ux0/data`）→ `resolve()` 返回 `{"ux0:/data", "ux0", "data"}`；`{"ux0:", "ux0", ""}` 表示卷根。
- **数据目录不再是 root，但它照常可达**：`ux0/data/J2ME00001`。这正是需求 1 的要点——文件管理器现在能浏览整机，而不是被锁进移植自己放 jar/config 的那个目录。
- `availableRoots()` **必须惰性**，不能写成静态初始化块：探测是 native 调用，而 ROMizer 在**构建期**就链接这些类，类初始化不能依赖 native 代码。
- 旧常量 `ROOT_NAME` / `ROOT_LIST_NAME` / `ROOT_URL` 全部删除（`grep` 已确认无残留引用；`FileConnectionImpl.class` 对 `getRootPath` 的引用数为 **0**，说明旧耦合真的断了）。

### 二、native 两处关键点

- **`jsr75_is_dev_root(path)`**：`sceIoGetstat()` 对**卷根**不保证置 `IFDIR`（和 `jsr75_is_root_path()` 早就在绕的是同一个固件怪癖），所以改为**直接开卷根**（`sceIoDopen("ux0:/")` 成功即存在且已挂载）。这同时回答了"卡插没插"，Java 层拿它决定 `imc0`/`uma0` 要不要列进 `listRoots()`。
- **`availableSize`/`totalSize` 带路径**：`jsr75_dev_of(cpath)` 取设备前缀 → `sceAppMgrGetDevInfo(dev)`，于是每张卡报自己的容量，而不是永远报 data 目录所在卷的。
- 卷根的绝对路径是 **`ux0:`（无尾斜杠）**，因为 `resolve()` 走 `trimSlash()`；这**安全**：`listOpen()` 本来就会把目录路径补成 `"dir/"` 再 `sceIoDopen`（`ux0:/`），而 `parentPath()` 的 `slash <= 0 → null` 分支正好吃掉无斜杠的情况（不会 `substring` 越界）。
- `pending`：`jsr75_is_root_path()` 在多 root 模型下已成死代码，按最小 diff 原则**保留未删**。

### 三、字体：J2FB v2（比例拉丁 + 抗锯齿 + 基线对齐）

- **格式**：32 字节头（`"J2FB"`、version=2、`nsec`@8、`dataOff`@12、ascent/descent/leading@16/18/20、`nglyphs`@24）+ `nsec`×28 字节 section 记录（first/count/gw/gh/stride/bpp/advOff）+ advance 表（1 字节/字形，0 = 无字形）+ 4 字节对齐位图。stride：8bpp → `gw`，1bpp → `(gw+7)/8`。加载器仍兼容 v1。
- **11 个 section / 21,653 码点**：Latin（`0x0020×95`、`0x00A0×96`、`0x0100×128`、`0x0386×56`、`0x2010×24`、`0x2460×40`）= **DejaVu Sans 20px 8bpp 比例宽度**；CJK（`0x3000×64`、`0xFF01×95`、`0xFF61×63`、`0x4E00×20902`、`0x9FA6×90`）= **等线 18px 1bpp**。字身 `CW20×CH22`，基线 `ASCENT 18`，descent 4。银行 **1,615,280 B**。
- **坑 1：stb `yo` 是屏幕坐标（向下为正）**，必须 `cy = ASCENT + yo`。第一版按 `cy = yo` 摆，所有字形整体下移 24 行——回归时先看这个。
- **坑 2：CJK 墨迹宽度会超过字面 advance**（全角框、`0xFF01` 组），section advance 归一到 `max(墨迹上限, adv 上限)`，否则字与字粘一起。
- **坑 3：编译 `fontgen` 必须 `-B/usr/bin`**（`gen_font.sh` 已固化）——PATH 前面是 VitaSDK 的 ARM `as`/`ld`，会拒绝 x86 目标文件。
- **打包瘦身**：`config/font.ttf`（16 MB 等线）**不再进 VPK**，只发 `fontbitmap.bin`。**VPK 13.7 MB → 3.6 MB**。`config/font-latin.ttf` = DejaVu Sans 副本 + `font-latin.LICENSE.txt`（保留来源与许可），`.gitignore` 对这两个新增文件做了白名单。

### 四、构建与验证（本轮实际命令与结果）

- MIDP：`bash phoneme-midp/build_vita.sh` → `EXIT=0`。尾部 `jsr75_file.c: undefined reference to 'sceAppMgrGetDevInfo'` 是**已知无害项**：只出现在 `libmidp.so` 的检查链接里（那条路径不含 `SceAppMgr_stub`），最终可执行文件链接 `libobj.a` 时符号齐全；而且这个调用**旧版 `availableSize` 就在用**，不是本次引入。
- 级联确认（时间戳）：`FileSystemRegistryImpl.java` 11:13:13 → `.class` 11:14:19 → `classes.zip` 11:14:19 → `obj/arm/jsr75_file.o` + `obj/arm/ROMImage.o` → `libobj.a` 11:14:41。
- **反汇编核验**（`arm-vita-eabi-objdump -dr .../jsr75_file.o`）：`FileStore_isDirectory` 内联了 `sceIoDopen`/`sceIoDclose`/`sceIoGetstat`——新的卷根探测确实编进去了。
- **类/ROM 级核验**：`FileSystemRegistryImpl.class` 常量池含 `ux0`/`imc0`/`uma0`/`app0`/`unknown root: `/`localhost`；`FileConnectionImpl.class` 对 `getRootPath` **0** 引用；ELF 中 `imc0`/`uma0`/`unknown root: `/`localhost` 以 UTF-16LE ROM 字符串存在（1 个）；`Java_com_sun_midp_jsr075*` native 符号 **21** 个（未变）。
- 产物：`vita-port/build/cmake/midp_vita.vpk` = **v01.65 b220 (c568dcc)**，3,534,892 B；VPK 内 `data/J2ME00001/fontbitmap.bin` 与 `config/fontbitmap.bin` **MD5 相同**（`af032dab2d62574dccc4e10d2f12acd8`），`font.ttf` 已不在包内；`version: J2ME Player v01.65 b220 (c568dcc)`。
- 提交：`samples` 主仓 **c568dcc**（`phoneme-cldc`/`phoneme-midp` 本轮无改动）。
- **提醒：版本号在 CMakeLists 里改完必须重打 VPK**，否则 `strings` 还是旧 build 号（v01.48 已踩过一次）。

### 五、未实测 / 遗留风险

- **Vita3K 未上机复测**（本轮用户未装包）：预期 `listRoots()` 给出 `ux0/`（插卡时另有 `imc0/`/`uma0/`）与 `app0/`，`file:///ux0/data/J2ME00001` 可进，`availableSize()`/`totalSize()` 随路径变卡。
- **字体视觉未验收**：拉丁应为比例宽度 + 灰度抗锯齿，下伸部/标点落在基线上。`vita_menu.c` 里写死的 x 坐标（最大 908）在拉丁变窄后只会更宽松，不会溢出。
- `FileConnectionImpl.open()` 对 `relPath == ""` 仍然拒绝（"Cannot open the root"）——**卷根同理**，若某 MIDlet 直接 `open()` 根目录会抛异常（与旧单 root 时代行为一致，未改）。
- `getName()` 对卷根返回 `"ux0:"`（`lastElement()` 找不到 `/`），不是 `""`；设备名 root 的固有小事，无害。
- `FileStore.exists()` 仍走 `pcsl_file_exist()`，其 `app0:` 兜底（`vita_pcsl.c:1408`）对不存在的路径可能报 true（v01.60 起已知）。
- `usedSize()`/`directorySize()` 现在可能对整个卷递归（`MAX_DEPTH 12` 限深不限宽），在 `ux0:/` 上调会很慢——文件管理器真去算根目录体积时注意。
- `jsr75_is_root_path()` 已成死代码（保留未删）。

## 2026-09-14 v01.63：MiniXplorer 目录打不开定案——`resolvePath()` 不认 `file://localhost/...`

> 实测（Vita3K，v01.62 + `[ALERT]`/`[MIDLET]`/`[JSR75]` 诊断）后新的、明确的失败点。
> 症状：MiniXplorer 能正常启动、UI 出现，但一进目录就失败。

### 一、日志给出的完整证据链

```
[MIDLET] startApp: MiniXplorer
[JSR75] listRoots -> data/                     <- listRoots 修好了（v01.61 的修复生效）
[MIDLET] startApp returned: MiniXplorer
[JSR75] open file://localhost/data/ mode=3     <- MiniXplorer 按规范拼 URL
Uncaught exception: java.lang.IllegalArgumentException: not a file URL: file://localhost/data/
  com.sun.midp.jsr075.FileSystemRegistryImpl.resolvePath(), bci=38
  com.sun.midp.jsr075.FileConnectionImpl.<init>(), bci=76
  com.sun.midp.io.j2me.file.Protocol.openPrim(), bci=130
  javax.microedition.io.Connector.open() x3
  MiniXplorer.OpenDir(), bci=73
  MiniXplorer.traverseDirectory(), bci=116
```

之后是 `file://localhost/data/data/`、`/data/data/data/`……**无限级联**——那是 MiniXplorer
自己的错误处理把 root 名反复追加，不是我们的 bug。

### 二、根因：URL 语法只认一种写法

- JSR 75 语法是 `file://<host>/<root>/<path>`，本机 host = `localhost`；空格权威
  （`file:///data/`）是同一件事的公认简写。两种写法都在野外存在。
- 原 `resolvePath()` 硬编码前缀 `"file:///"`，于是**规范的 `file://localhost/...` 一律被拒**。
  MiniXplorer 用的正是 `"file://localhost/" + listRoots()` 这条文档推荐写法。
- 讽刺的是 v01.61 刚把 `listRoots()` 改成返回 root **名**（`data/`），正是为了让调用方能拼出
  `file://localhost/data/` —— 结果 `resolvePath()` 不认这个形式，两处假设没对上。

### 三、修复（`jsr75/src/share/core/common/classes/com/sun/midp/jsr075/FileSystemRegistryImpl.java`）

- `resolvePath()` 改为解析权威部分：`scheme = "file://"` → 取到下一个 `/` 之前作为 host，
  **接受 `""` 与 `localhost`（大小写不敏感）**，其它 host 仍报 `unknown host`（本移植只暴露一个
  root，不能假装认识别的设备）。`file:///data/x` 与 `file://localhost/data/x` 现在归一到同一路径。
- 顺带更新类头注释与 javadoc（原文写"Accepted form: file:///data"）。
- 其余路径全部未动：`getPath()` 仍返回 `/data[/...]`（规范要求含 root），`getURL()` 原样返回
  打开时的 URL，`FileConnectionImpl.listEntries()` 已保证目录带尾 `/`（对应原生
  `jsr75_file.c:listNext` 的 `SCE_S_ISDIR` 分支），`Connector` → `Protocol.openPrim` 未改。

### 四、副产品：那块 "Done" 屏的最终解释

- `[ALERT] new Alert title=null text=This application does not use the screen and runs in the
  background.` 是 phoneME **自己的** `HeadlessAlert`（`ResourceConstants.LCDUI_DISPLAY_HEADLESS`）。
- `CldcForegroundController.registerDisplay()` 把 `new HeadlessAlert(...)` 作为
  `getCurrent()==null` 的**占位显示对象预先构造**——所以 MIDlet 一调 `Display.getDisplay()`
  就会看到这条 `[ALERT]` 日志，**与是否真的弹屏无关**。
- 真正弹屏的条件在 `CldcMIDletStateListener.midletActivated()`：`startApp` 返回后
  `Display.getDisplay(midlet).getCurrent() == null` 才 `requestForegroundForDisplay()`。
- 结论：之前的 "Done" 屏是 MIDlet **没留下任何可显示对象**的症状（当时 `startApp` 后半段
  已经因为 JSR 75 抛异常而中断），不是病因。本次运行 MIDlet 正常留下 UI，故不再出现。

### 五、产物与验证

- `vita-port/build/cmake/midp_vita.vpk` = **v01.63**，13,692,084 B（10:46:50），
  `strings midp_vita | grep "version: J2ME"` = `J2ME Player v01.63 b218 (cf48224)`，
  `Java_com_sun_midp_jsr075*` 原生符号 21 个。
- `classes.zip` 10:45:46、`ROMImage_01.cpp` 10:46:03、`obj/arm/ROMImage.o` 10:46:06（同代）。
  **注意：本机没有 `unzip`**，验证类内容用 `python3 -c "import zipfile..."`；
  已确认 `FileSystemRegistryImpl.class` 常量池含 `file://` / `localhost` /
  `equalsIgnoreCase` / `unknown host: `。
- romgen 通过（无 `illegal immediate` / `Error 133`）。

### 遗留

- 未实测：MiniXplorer 打开目录后的 `list()` 渲染、以及 `availableSize/totalSize`
  （`libmidp.so` 链接仍报 `sceAppMgrGetDevInfo` 未定义——该符号只在旧 `libmidp.so` 路径上，
  VPK 走 `libobj.a`，不影响；但 JSR 75 的 `availableSize()` 在真机/模拟器上可能返回失败）。
- 诊断打印仍在（`[JSR75]` / `[MIDLET]` / `[ALERT]`），稳定后删除。

### 六、实测结果：**通过** ✅

用户装 v01.63 在 Vita3K 实测——**MiniXplorer 正常启动，目录可以打开了**。JSR 75
（`javax.microedition.io.file`）在 PS Vita 上从零到可用，至此完成。

## 2026-09-14 v01.64：JSR 75 收尾——诊断打印全部撤除（最小 diff 还原）

> 承接 v01.63 的实测通过。功能已稳定，按最小 diff 原则把为定位而加的临时诊断全部删除。

### 一、撤除内容（`git checkout --` 整体回退，均为纯诊断文件）

| 文件 | 撤除内容 |
|---|---|
| `jsr75/.../com/sun/midp/io/j2me/file/Protocol.java` | `[JSR75] open file:... mode=` |
| `jsr75/.../com/sun/midp/jsr075/FileConnectionImpl.java` | `[JSR75] connect ...`、`listEntries` 的 3 处 |
| `phoneme-midp/.../MIDletStateHandler.java` | `[MIDLET]` 的 startApp / startApp returned / THREW / destroyApp / destroyed / state loop |
| `phoneme-midp/.../Alert.java` | `[ALERT]` 的构造 + setString |

- **重要澄清**：`MIDletStateHandler` 里 `[MIDLET] creating` / `created+registered OK` /
  `FAILED(CNFE|IE|IAE|RE|Error)` / `startSuite` 这 8 处**不是临时诊断**，它们来自
  **2026-08-31 的移植基线提交 `8a04358`**，是项目固有启动日志 → **保留**。
  （用 `git log -S` 定位时注意：双引号里的 `\[` 会被当成字面反斜杠，要用
  `git log -S'[MIDLET] creating'` 单引号写法，否则会搜不到而误判为"未提交"。）
- `FileSystemRegistryImpl.java` 只删掉 `[JSR75] listRoots ->` 一行，**真实修复全部保留**。

### 二、最终保留的真实改动（JSR 75 全部改动清单）

| 文件 | 改动 | 版本 |
|---|---|---|
| `jsr75/.../FileSystemRegistryImpl.java` | `listRoots()` 返回 root **名**（`data/`）；`resolvePath()` 解析权威部分，接受 `""`/`localhost` | v01.61 / v01.63 |
| `jsr75/.../javax/microedition/io/file/FileSystemRegistry.java` | javadoc：root 是**名**不是 URL | v01.61 |
| `vita-port/config/system.config` | `microedition.io.file.FileConnection.version: 1.0` | v01.62 |
| `phoneme-cldc/src/vm/cpu/arm/BinaryAssembler_arm.cpp` | 字面量池绑定复用窗口 off-by-8 修复 | v01.62 |
| `rebuild_vm.sh` | `set -e` + `grep -c` 假失败修复（`\|\| true`） | v01.62 |

（另有 v01.60 起新增的整棵 `jsr75/` 树与 native 层 `src/share/native/jsr75_file.c`。）

### 三、产物与验证

- `vita-port/build/cmake/midp_vita.vpk` = **v01.64**，13,690,011 B（10:52:39），
  `version: J2ME Player v01.64 b218 (cf48224)`，jsr75 原生符号 21 个。
- `classes.zip` 10:52:00 → `ROMImage_01.cpp` 10:52:18 → `obj/arm/ROMImage.o` 10:52:21（同代），
  romgen 无 `illegal immediate` / `Error 133`。
- 用 `python3` zipfile 逐类核验常量池：
  - `FileSystemRegistryImpl.class` → 含 `localhost`、`unknown host`，**不含 `[JSR75]`** ✅
  - `FileConnectionImpl.class`、`Protocol.class` → **不含 `[JSR75]`** ✅

### 四、遗留（下一轮候选）

- `availableSize()` / `totalSize()` 未在设备上验证（`sceAppMgrGetDevInfo`）。
- `list()` 结果在 MiniXplorer 里的渲染、以及 >1 层的深层目录遍历未逐一验证。
- 若要继续调试其他 MIDlet，可再次临时加诊断——**但务必避开 AOT 大方法**（见 v01.62 第六节）。

## 2026-09-14 v01.62：romgen SIGTRAP 定案（ARM 字面量池 off-by-8，上游潜伏 bug）+ JSR75 属性补全

> 承接 v01.61。给 `MIDletStateHandler.startSuite` 加 `[MIDLET]` 诊断 println 后，
> romgen 变成 SIGTRAP(133)，构建阻断。本轮把根因查到"1 字节"级别并修掉。

### 一、症状与定案过程（可复现）

- `phoneme-midp/build_vita.sh` 在 `cldc_vm.gmk:234` 报 `Error 133`；手动跑 romgen
  （只看 stdout）才见到真正的断言：`size 4096 too big for 12 bits` →
  `assert(has_room_for_imm(imm, size), "illegal immediate value")`
  （`phoneme-cldc/src/vm/cpu/arm/Assembler_arm.hpp:73`，`#ifndef PRODUCT` 下）。
- gdb（romgen **未 strip、带 debug_info**）符号化栈：
  `imm_index` ← `access_literal_pool (BinaryAssembler_arm.cpp:248)` ← `ldr_from` ←
  `ldr_literal` ← `ldr_oop` ← `invoke` ← … ← `JVMCompiler::compile` ←
  `JVMMethod::compile` ← `ROMOptimizer::precompile_methods (ROMOptimizer.cpp:3626)`。
- 关键局部量：`#2 pos=7188, target=3100` ⇒ `target-(pos+8)` = **-4096**（12 位合法范围 ±4095）；
  `#19 i=165, compiled_count=147, impossible_count=18, precompile_size=364` ⇒ 崩溃项 =
  AOT 编译队列第 165 项（0 基）。对照上一次成功构建的 `ROMLog.txt` 的
  `[AOT compilation report]` 第 165 项 = **`com/sun/midp/midlet/MIDletStateHandler.startSuite`**
  —— 正是本轮加打印的那个大方法。

### 二、根因：VFP 分支的绑定字面量复用窗口多 1 字节（上游 bug）

- `write_literal()` 写池时 `literal->set_bci(position)` ⇒ `_bci` = 池在代码里的位置。
- `access_literal_pool()` 用 `imm_index(pc, target - (pos + 8))` 生成 PC 相对偏移，
  要求 `|offset| ≤ 4095` ⇒ 可复用字面量的**最远回距** = `pos - 4087`。
- `find_literal()`（`ENABLE_ARM_VFP=1` 走的那支）：`offset = 4088 - 8`，
  `min_offset = _code_offset - offset` = `pos - 4088`，判据 `ptr->_bci >= min_offset`
  ⇒ **多接受 1 字节**（`_bci == pos-4088` → 偏移 -4096）。
  debug 构建断言崩溃；**PRODUCT 构建会静默把 4096 截断成 0 → 载入错误地址**（潜在错码）。
- 非 VFP 分支用 `ptr->_bci <= position` 本来就把该边界正确排除，所以这是 VFP 侧独有的
  off-by-one，不是"设计如此"。
- 触发条件：只有 AOT 大方法（`startSuite` 约 7.2KB 代码 + 大字面量池）才会出现
  "恰好 4088 字节之前的绑定字面量"。普通方法永远碰不到，所以上游一直没暴露。

### 三、修复

- `phoneme-cldc/src/vm/cpu/arm/BinaryAssembler_arm.cpp`（VFP 分支 `find_literal`）：
  `const int min_offset = _code_offset - offset + 1;` + 11 行推导注释。
  **不做 VITA 条件编译**：这是跨平台真实 bug，`#if defined(VITA)` 反而会把它藏起来。
- 重编宿主工具：`bash rebuild_vm.sh tools`（loopgen 需先建好；`-j` 下 romgen 会抢跑
  半成品 loopgen → `Permission denied`/Error 127，**再跑一次即可**）。改 VM 代码后
  **必须**重建 romgen，否则 ROM 生成物与源码不一致。产物
  `phoneme-cldc/build/vita_arm/dist/bin/romgen`（重建后 6,421,100 B）。
- 运行库同步：`bash rebuild_vm.sh build` ⇒ 只有含该文件的 `_MergedSrc003.o` 同代重编
  （10:35:12）；重打包后 29 成员、`jvm_fast_globals` 唯一 `D`、无 C-interpreter 符号。
  （运行期 JIT 关闭 `-int`，此修复对当前运行是惰性的，纯粹为源码/库一致。）

### 四、两个排查/脚本陷阱（务必避免重复踩）

1. **romgen 的两条输出链**：VM 崩溃报告走 **stdout**；`tty->print_cr` 诊断
   （`ClassFileParser.cpp:2157` 的 `PRE_NATIVES/POST_NATIVES`，4 万余行）走 **stderr**。
   用 `2>&1` 会把断言淹没 ⇒ 用 `> /tmp/out.txt`（或 `2>/dev/null`）。
   另：`gdb -ex 'run > file'` 会破坏 romgen 的 argv（报 `class not specified`），
   重定向必须在 shell 层做。
2. **`rebuild_vm.sh` 的 `set -e` + `grep -c` 假失败**：`ccount=$(... | grep -cE ...)`
   在 0 匹配时返回状态 1，`set -e` 让脚本在**重打包成功之后**静默 `exit 1`
   （日志无 FAIL、库却已更新 ⇒ 极易误判为"半成品库"）。已在两处加 `|| true`
   （2026-09-14）。若看到 `EXIT=1` 且最后一行是 `(full-generation repack...)`，就是它。

### 五、v01.62 交付内容与状态

- JSR75：`FileSystemRegistryImpl.listRoots()` 按规范返回**根名**
  （`ROOT_LIST_NAME = ROOT_NAME + "/"`）；
- `vita-port/config/system.config` 新增 `microedition.io.file.FileConnection.version: 1.0`
  （属性系统只搜 `applicationProperties` = system.config，须随包分发）；
- **临时**保留诊断打印用于 MiniXplorer 定位：`[ALERT]`（Alert 4 参构造 + setString）、
  `[MIDLET]`（startSuite/startApp/destroyApp/state loop）、`[JSR75]`（Protocol/
  FileConnectionImpl/FileSystemRegistryImpl）。**稳定后必须删除**（最小 diff 原则）。
- 产物：`vita-port/build/cmake/midp_vita.vpk` = **v01.62**，13,691,870 B（10:37:12），
  `arm-vita-eabi-nm midp_vita | grep -c Java_com_sun_midp_jsr075` = **21**；
  `ROMImage_01.cpp` 10:36:54、`obj/arm/ROMImage.o` 10:36:57（含新诊断）。

### 六、教训

- **不要给 AOT 大方法加 println**：新增字符串常量 + 代码会移动字面量池布局，可能正好踩到
  12 位边界（本例就是 10:18 那次 println 直接触发）。诊断打印优先放小方法，或用开关隔离；
  修完 off-by-one 之后仍要遵守，因为边界是"位置敏感"而非"长度敏感"。
- 编译脚本退出码不等于结果：先 grep 日志里的 `illegal immediate` / `VM Error` / `Error 133`。

### 遗留风险

- 字面量池**前向**窗口（`set_delayed_literal_write_threshold`）未审：按构造约有 15 字节余量，
  本轮未触发；`BinaryAssembler_thumb.cpp:497` 的同名函数未审（Thumb 路径未启用）。
- `microedition.io.file.FileConnection.version` 是否被运行期 `System.getProperty` 读到，
  待 MiniXplorer 实测确认。

## 2026-09-14 v01.60：JSR 75 (`javax.microedition.io.file`) 落地——独立子系统 `samples/j2me/jsr75`，单 root = `ux0:/data/J2ME00001`

> 承接 v01.59 的"地基已备、Java 层下一轮做"。本轮把 Java 层 + native 层 + 构建接线一次做完，**不碰任何共享 phoneME 文件**（新增 `jsr75/` 树 + `build_vita.sh` 两处开关）。

### 一、许可证 / 来源纪律（用户明确要求）

- 上游 phoneME **没有 JSR 75 实现**（只有 `USE_JSR_75` 空钩子 + 权限/清理接口桩），所以只能参考外部实现。
- 决定：**只取 JSR 75 规范的公开 API 签名**（`javax.microedition.io.file.*` 的 5 个类型，规范公开、签名本身不受版权保护），`com.sun.midp.*` 侧的一切 Impl（`FileConnectionImpl` / `FileStore` / `FileSystemRegistryImpl` / `Protocol`）**全部自己写**，写在现有 `pcsl_file_*` 之上。**没有从任何 GPL 项目复制代码**（`freej2me-plus` 的 `FileConnectionImpl.java` 是 GPL 系，只用于核对行为，未复制）。
- `phoneme_source/` 全程只读查阅，未复制任何文件进工作副本。

### 二、Root 策略（唯一 root，可读写）

- 唯一暴露的根就是启动器 `chdir()` 的那个目录：`vita_main.c:63 #define DATA_DIR "ux0:/data/J2ME00001"`。
- native 侧 `jsr75_root()` 用 `getcwd()` 取（fallback 硬编码同一字符串），因此**与 `vita_pcsl.c` 的 `vita_resolve_path()` 天然一致**，不需要重复 `#define`。
- Java 侧 `FileSystemRegistryImpl.ROOT_NAME = "data"`、`ROOT_URL = "file:///data/"`；`FileStore.getRootPath()` 返回 `ux0:/data/J2ME00001/`，Java 把 `file:///data/<x>` 翻成 `ux0:/data/J2ME00001/<x>` 后下传，路径直接命中 `vita_resolve_path()`。

### 三、关键机制发现（改之前必须知道）

1. **native 绑定全自动**：`romgen` 侧 `SourceObjectWriter::put_c_function()` 直接为 `KNIDECL(m)` 生成 `extern "C" Java_<class>_<method>`，**不需要重生成 `NativesTable.cpp`**。所以新增 native 只要 C 侧函数名与 Java 侧 `native` 方法名对上即可（本轮 C 侧 21 个 ↔ Java 侧 21 个，`nm` 逐一核对）。
2. **`pcsl_directory.h` 那一组（v01.59 补的 8 个）在 `libmidp.so` 里拿不到**——它们实现在**可执行文件**（`vita-port/src/vita_pcsl.c`），不在 MIDP 共享库链接的那套 PCSL 里。第一版 native 直接调 `pcsl_file_is_directory/mkdir/rmdir/get_time/getfreesize/gettotalsize`，链接期全部 undefined。**结论：`libmidp.so` 内的代码必须自洽**，目录/时间/容量这类事只能走 `sceIo*` / `sceAppMgrGetDevInfo`（`sceIo*` 与 `sceAppMgr*` 的 stub 在最终可执行文件链接里已由 CMake 提供）。
   - 另注：`phoneme-midp/src/vita_stubs.c` 里的 `pcsl_file_*` 是**死代码**（v01.59 已定案，CMakeLists 注释：old vita_stubs.o is NOT linked），别照它写。
3. **`preprocess_jpp.py` 的 DEFINES 为空**⇒ JSR 75 的 Java 文件是**普通 `.java`**，不能写 `#ifdef ENABLE_JSR_75`。
4. 构建钩子（`Subsystems.gmk:240`）：`USE_JSR_75=true` → `JPP_DEFS += -DENABLE_JSR_75` + `include $(JSR_75_DIR)/build/cldc_application/subsystem.gmk`；`SubsystemDefs.gmk:390` 用 `$(SUBSYSTEM_JSR_75_JAVA_FILES)` 进 `classes.zip` 规则、`$(SUBSYSTEM_JSR_75_NATIVE_FILES)` 进 `JTWI_NATIVE_FILES`。**include 顺序是对的**（`SubsystemDefs.gmk:31` 先 include `Subsystems.gmk`），所以 `subsystem.gmk` 里的变量在被消费前就已定义，不需要 jsr120 那套 `subsystem_defs.gmk`/`subsystem_rules.gmk`。

### 四、改动清单（全部新增，共享文件零改动）

- **新增 `samples/j2me/jsr75/`**（Vita 本地子系统）
  - `src/share/core/common/classes/javax/microedition/io/file/{FileConnection,FileSystemRegistry,FileSystemListener,ConnectionClosedException,IllegalModeException}.java`
  - `src/share/core/common/classes/com/sun/midp/jsr075/{FileConnectionImpl,FileStore,FileSystemRegistryImpl}.java`
  - `src/share/core/common/classes/com/sun/midp/io/j2me/file/Protocol.java`（GCF `file:` 协议，`Connector.open("file:///data/...")` 的入口）
  - `src/share/native/jsr75_file.c`（21 个 native，646 行）
  - `build/cldc_application/subsystem.gmk`（9 个 Java 文件绝对路径 + `vpath % $(JSR_75_NATIVE_DIR)` + `SUBSYSTEM_JSR_75_NATIVE_FILES = jsr75_file.c`）
- **修改 `phoneme-midp/build_vita.sh`**（唯一改动的既有文件，+6/-2）：两处 `USE_JSR_75=false` → `true` + `JSR_75_DIR`/`PROJECT_JSR_75_DIR`。
- **修改 `vita-port/CMakeLists.txt`**：`VITA_VERSION` 01.59 → **01.60** + 注释块。
- **未改** `build/vita_arm/Options.gmk`（其 `USE_JSR_75 = false` 被命令行变量覆盖，GNU make 命令行优先）。
- **`vita-port/CMakeLists.txt` 未加 `jsr75_file.c`**：它由 MIDP makefile 编进 `libobj.a`，CMake 只是复用该归档。

### 五、语义决定（记录下来，避免以后推翻）

- **filter**（`list(String filter, boolean includeHidden)`）：只按**条目名**做 `*` / `?` 通配匹配。目录在 native 侧带尾 `/` 上报，匹配前剥掉，所以 `list("*.txt", ...)` 不会因目录把 `dir.txt/` 也匹配进来造成歧义。
- **隐藏语义**：本移植把**名字以 `.` 开头**定义为隐藏。原因：Vita 文件 API 没有"隐藏"属性位（`pcsl_file_get_attribute` 的 HIDDEN 只能恒 0），而 `.` 前缀是唯一**同时驱动** `isHidden()`、`setHidden()`（实现为加/去前导点的改名）和 `list(..., includeHidden)` 的约定，三者天然自洽。
- **权限**：JSR 75 的 `javax.microedition.io.Connector.file.read/.write` 在 `domain: minimum,unsecured` 下**未授予**，因此 `Protocol` 刻意**不做 `AccessController` 检查**（否则 MIDlet 必然 SecurityException）。上游 MIDP 核心 `Connector` 不做 scheme 级权限检查（`grep Connector.file` 只命中 CDC 的 `Permissions.java`），所以这条路是通的——**但未上机验证**。

### 六、踩坑记录（本轮新增）

1. **`strncpy` 触发 `-Werror=stringop-truncation`**：MIDP 这套构建**默认 warnings-as-errors**（`build_vita.sh` 里那一串 `-Wno-error=...` 就是为此）。改成限长 `memcpy` + 手动补 NUL。
2. **KNI 宏展开缺分号**：`JSR75_HANDLE_METHOD(name, body)` 第一版 `body` 后没分号，5 处展开全部 `expected ';' before '}'`。宏内部补 `;` 后调用点写起来像赋值语句。
3. **KNI mangled 名手误**：`seekFile` 写成 `..._FileStore_seekFile` 之前错成 `jsr75` 而非 `jsr075`，链接期才暴露。**`arm-vita-eabi-nm <某个 .o> | grep " T "` 是最快的 KNI 名字自检**。
4. **头注释里写了 `sceIo*/sceAppMgr*`，其中的 `*/` 提前结束块注释**，导致后面 30 行全变成代码，报了一串莫名其妙的 `unknown type name 'PCSL'` / `ptrdiff_t`。注释里不要出现 `*/` 字面量。
5. **make 侧 `libmidp.so` 链接失败是既有无害项**：`_rom_linkcheck_mffd_false` undefined（`libobj.a(_MergedSrc005.o)`，与 JSR 75 无关，v01.48 条目已记录过），CMake 的真实可执行文件链接能解析它。所以判断 JSR 75 是否成功**不能看 `build_vita.sh` 的退出码**（它被 `| tee` 吞掉），必须看 `obj/arm/jsr75_file.o` + CMake 那一步。

### 七、验证（已做）

- `javac -source 1.3 -target 1.3`（bootclasspath = cldc_classes.zip，classpath = MIDP classes）**0 error**，产出 12 个 `.class`（含 `FileConnectionImpl$InnerInputStream/OutputStream`）。
- `jar tf classes.zip`：`javax/microedition/io/file/*`、`com/sun/midp/jsr075/*`、`com/sun/midp/io/j2me/file/Protocol.class` 全在。
- `obj/arm/jsr75_file.o` 编译通过；21 个 `Java_com_sun_midp_jsr075_FileStore_*`。
- `grep -c jsr075 ROMImage_01.cpp` = 21；`nativeFunctionTable.cpp` 45 处。
- `cmake --build build/cmake -j4` 链接通过；`arm-vita-eabi-nm build/cmake/midp_vita | grep -c Java_com_sun_midp_jsr075` = **21**。
- 产物：`vita-port/build/cmake/midp_vita.vpk` 13,689,448 B（2026-09-14 07:29）。

### 八、遗留 / 下一步

- **完全未上机/未进 Vita3K**：`FileConnection` 的运行时行为（root 枚举、`list` 过滤、`setHidden` 改名、流读写、`availableSize`）全是设计预期，没有任何实测证据。
- `com.sun.midp.jsr075.FileConnectionCleanupImpl` **不存在**：上游 `ams/.../Installer.java:1803` 会 `Class.forName` 它，会抛 `ClassNotFoundException`（该路径只在套件删除时走，暂判无害，待实测确认）。
- `availableSize/totalSize` 走 `sceAppMgrGetDevInfo`：CMake 可执行文件链接能解析，但 make 侧 `libmidp.so` 那条线缺 `SceAppMgr_stub`——如果将来真要让 `libmidp.so` 独立链接，需要补 `LIBS += -lSceAppMgr_stub`。
- 记忆里的"JIT 二次启动"、"物理尾巴字节留盘"等旧遗留不变。

## 2026-09-14 v01.59：JSR 75 地基——补齐 `pcsl_directory.h` 的 8 个 native

> 承接 v01.58 的"文件操作能力盘点"结论：`pcsl_file.h` 的 21 个 API 已全实现，缺口在 JSR 75。本轮先补**它依赖的最后一层 native 空洞**，Java 层下一轮做。

### 一、盘点修正：`vita_stubs.c` 是**死代码**

- `phoneme-midp/src/vita_stubs.c`（1195 行，`pcsl_file_*`/`pcsl_socket_*` 桩、`[file_open]` 调试串、写 `debug_log.txt`）**没有进链接**：`vita-port/CMakeLists.txt` 第 11 行注释明写 "old vita_stubs.o is NOT linked (replaced by src/vita_pcsl.c)"。
  ⇒ 查"某个 pcsl 函数有没有实现"**只能看 `vita_pcsl.c`**，改 `vita_stubs.c` 没有任何效果（本轮曾差点改错文件）。
- `gen_stubs.py` 生成的 30 个 Java 桩类（chameleon / orientation / `com.sun.midp.main.MIDletSuiteLoader`/`Configuration`/`CommandState` / GCI / `javax.microedition.media.*`）**不含 JSR 75**，所以"JSR75 的打桩代码"并不存在，无从"补全"。

### 二、真实缺口：`pcsl_directory.h` 8 个入口（`grep pcsl_directory_ vita_pcsl.c` = 0）

`phoneme-midp/build/vita_arm/pcsl/vita_arm/inc/pcsl_directory.h` 声明、而本移植从未实现，任何调用都会**链接期失败**：

| 函数 | Vita 实现 |
|---|---|
| `pcsl_file_is_directory` | `sceIoGetstat` + `SCE_S_ISDIR(st_mode)`，stat 失败返回 0，路径非法 -1 |
| `pcsl_file_mkdir` | `sceIoMkdir(path, 0777)`（单级，与 POSIX mkdir 一致） |
| `pcsl_file_rmdir` | `sceIoRmdir(path)` |
| `pcsl_file_getfreesize` | `sceAppMgrGetDevInfo(dev, ...)` → `free_size` |
| `pcsl_file_gettotalsize` | 同上 → `max_size` |
| `pcsl_file_get_attribute` | READ/WRITE 恒 1（Vita 无 per-file 权限位），EXECUTE/HIDDEN 0，未知 type -1 |
| `pcsl_file_set_attribute` | READ/WRITE 当成功 no-op，其余 -1 |
| `pcsl_file_get_time` | `st_mtime` → epoch 秒（见下） |

新增/复用的辅助：

- `vita_dir_path()`：`pcsl_string` → UTF-8（512 缓冲）→ 去尾 `/`、`\` → 空判 → `vita_resolve_path()`（与其它 `pcsl_file_*` 同一条路径解析）。
- `vita_stat_any()`：`sceIoGetstat` 失败时回退 `app0:` 形式——复刻 `pcsl_file_exist` 里 `ux0:/data/` → `app0:/` 的前缀置换，只有真实设备才需要。
- `vita_device_of()`：从绝对路径抽出 `"ux0:"` 式卷前缀，让容量查询按卷走而不是硬编码 `ux0:`。
- `vita_sce_datetime_to_epoch()`：**踩坑记录**——`SceIoStat::st_mtime` 不是 epoch 秒，而是 `SceDateTime` 结构体（`psp2common/types.h:195`：`unsigned short year, month, day, hour, minute, second; unsigned int microsecond;`）。第一版写 `(long)st.st_mtime` 直接编译不过（`aggregate value used where an integer was expected`）。改用 Howard Hinnant 的 `days_from_civil` **纯整数**换算，不引 `psp2/rtc.h`、不加 `SceRtc_stub`、不依赖 libc 时区，Vita3K 与真机结果一致。代价：RTC 按原值读，若主机时钟设的是本地时间，epoch 差一个 UTC 偏移——MIDlet 只做相互比较，无影响。

### 三、改动（最小 diff，只动工作副本）

1. `vita-port/src/vita_pcsl.c`
   - `#include <pcsl_directory.h>`（紧随 `pcsl_file.h`）。
   - 在 `pcsl_file_getpathseparator()` 与 `PCSL Print stubs` 横幅之间新增 "PCSL directory / attribute service (JSR 75 groundwork)" 一节：8 个 `pcsl_directory.h` 入口 + 4 个 static 辅助。
2. `vita-port/CMakeLists.txt`：`VITA_VERSION` 01.58 → **01.59** + 注释块。

### 四、验证

- `cmake --build build/cmake -j4` 通过（无 error/warning）。
- `arm-vita-eabi-nm build/cmake/midp_vita | grep -E " T pcsl_file_"`：**21 → 29**，新增 8 个正是上表函数（`81008788`..`810089f0`）。
- 产物：`build/cmake/midp_vita.vpk` 13,667,710 B，md5 `a973d3f2516243bd591ccf49c0a20f30`。

### 五、遗留 / 下一步

- 这 8 个函数**目前无调用者**（JSR 75 Java 层还没写），因此 01.59 行为与 01.58 **完全等价**，纯地基。
- JSR 75 上游 phoneME **无源码**（只有 `USE_JSR_75` 空钩子 + 权限/清理接口桩）；互联网可参考：`nikita36078/J2ME-Loader`（Apache-2.0，只有 5 个 API 接口）与 `TASEmulators/freej2me-plus`（含 `FileConnectionImpl.java`，许可证 GPL 系）。计划：按 Apache-2.0 那套**只取公开 API 签名**，Impl 自己写在现有 `pcsl_file_*` + 本轮 `pcsl_directory_*` 之上。
- 待办（Java 层）：`javax.microedition.io.file.{FileConnection, FileSystemRegistry, FileSystemListener, ConnectionClosedException, IllegalModeException}` + Impl + `com.sun.midp.io.j2me.file.Protocol`，接到 `vita_overlay.mk` 或新子系统目录（**不改 `phoneme_source/`**），把 `ux0:/data/J2ME00001/` 注册成 root；任一 `.java` 变更会自动触发 romgen 级联重生成 `ROMImage_*.cpp`。

## 2026-09-14 v01.58：JIT 策略开关（`launch.cfg` 第 4 行 `jit=0|1|2`）+ 文件操作能力盘点

> 真机问题按用户要求**只记录、不上机复测**（v01.57 诊断包已就位，见下一节）。
> 本轮转向"补全 J2ME 功能"，先做能力盘点，再交付第一个可用开关：JIT。

### 一、文件操作能力盘点（结论：**native 层无缺口**）

- `vita-port/src/vita_pcsl.c` 中 `pcsl_file.h` 声明的 **21 个 API 全部已实现**：
  `init / finalize / open / close / read / write / unlink / truncate / exist /
  commitwrite / rename / openfilelist / closefilelist / getnextentry / seek /
  sizeofopenfile / sizeof / getusedspace / getfreespace / getfileseparator /
  getpathseparator`。
  ⇒ MIDP 自身（RMS、jar/class 读取、资源加载）所需的文件操作**已经不缺**，
  不需要新增 native 代码。
- 缺口只在 **JSR 75 `javax.microedition.io.file.FileConnection`**，而它在
  **上游 phoneME 里就不存在**：`phoneme_source/phoneME/` 无 `jsr75` 目录，
  只有 `abstractions/` 下的 `FileConnectionPermission` 权限桩与
  `ams_jsr_interface` 里的 `com.sun.midp.jsr075.FileConnectionCleanup`；
  `phoneme-midp/build/vita_arm/Options.gmk` 中 `USE_JSR_75 = false`（与其它 JSR 一致）。
  `phoneme-midp/src/protocol/file/` 是 `com.sun.midp.io.j2me.storage`（RMS 存储后端），
  **不是** JSR75。
  ⇒ 要做 JSR75 得**新写 Java API 类 + native 接线**（可架在现有 `pcsl_file_*` 之上），
  属于新增特性而非补漏，**暂缓**（等真机可用再决策，见"遗留"）。

### 二、JIT 现状调查（结论：JIT **一直在**二进制里，只是被无条件关掉）

- `phoneme-cldc/src/vm/share/utilities/Globals.hpp:231`：
  `product(bool, UseCompiler, true, ...)` ⇒ **默认开**；
  VPK 里可见 29 个 `T .*Compiler` 符号；`Universe.cpp:532-533` 调用
  `CompiledMethodCache::init()` 与 `Compiler::initialize()`。
- 但 `phoneme-midp .../jams/native/runMidlet.c` 里有一句**无条件的**
  `JVM_ParseOneArg("-int")` ⇒ `Arguments.cpp:129` `UseCompiler = false`，
  自 v01.46 起 JIT 从未真正跑过。
- 关掉的原因：**第 2 轮（UC）启用 JIT 会崩**，faulting PC 落在堆内
  `compiler_area`（v01.45 章节），根因未定位。

### 三、改动（最小 diff）

1. `vita-port/src/vita_main.c`
   - 新增 `#define VITA_JIT_DEFAULT 0` + `int vita_jit_policy = VITA_JIT_DEFAULT;`
     （全局，供 MIDP 侧读取）+ 只读 `launch.cfg` 的 `read_jit_policy()`。
   - `for(;;)` 轮循环增加 `int round` 计数，每轮开头重读 cfg 并折算：
     `vita_jit_policy = (cfg_jit == 1 && round > 0) ? 0 : cfg_jit;`
     （`jit=1` = 仅第 1 轮开 JIT），并 `crumb_printf("round %d: jit cfg=%d -> policy=%d", ...)`。
   - `runMidlet` 前打印真实 VM 标志：
     `crumb_printf("vm UseCompiler=%d (jit policy=%d)", ...)`（weak extern 读取
     `UseCompiler`，无编译器子系统时也能链）。
2. `phoneme-midp .../jams/native/runMidlet.c`
   - 把无条件 `-int` 换成**经弱符号读取策略**后才注入：
     ```c
     extern int vita_jit_policy __attribute__((weak));
     if (&vita_jit_policy == NULL || vita_jit_policy == 0) { "-int" }
     ```
     非 Vita 构建 / 独立 `libmidp.so` 链接里该弱符号为 NULL ⇒ 行为与旧版**完全一致**。
3. `vita-port/CMakeLists.txt`：`VITA_VERSION` 01.57 → **01.58** + 注释块。
4. `vita-port/README.md`：`launch.cfg` 说明补第 4 行 `jit=`，修正"默认 orientation"
   描述（代码默认是 **portrait**，原文误写成 landscape），补全日志文件清单。

### 四、关键机制（下次改 JIT 前重读）

- `-int` → `Arguments.cpp:129` `UseCompiler = false`；`-comp` → `MixedMode=false; UseCompiler=true`。
- `UseCompiler` 是**进程级全局**，`JVM.cpp:594-672` 只在**宿主 romization 期**
  保存/恢复它 ⇒ 运行时一旦 `-int`，**后续所有轮次都保持解释器**——
  这正是 `jit=1`（仅第 1 轮）自动变成"第 2 轮起解释器"的原因，无需额外代码。
- `CompiledMethodCache::init()` 每轮由 `Universe::initialize()` 重做（清零
  `Map/weights/upb/size/last_old`），所以 `jit=2`（每轮都开）不需要手工重置缓存。

### 五、验证（二进制级已过）

- 重编 `vita-port/build.sh midp`（触发 `runMidlet.o` 重编）→ **Build successful**。
- `arm-vita-eabi-nm .../obj/arm/runMidlet.o | grep vita_jit_policy` → `w vita_jit_policy`
  （弱未定义，符合预期）；VPK 里 → `812d2c48 B vita_jit_policy`（启动器强定义胜出）。
- **反汇编确认判断没被优化掉**（`objdump -dr runMidlet.o`）：
  `movw/movt r3, vita_jit_policy` → `cmp r3,#0` → `beq 注入-int` →
  `ldr r3,[r3]` → `cmp r3,#0` → `beq 注入-int`：地址检查与值检查都在。
- 产物：`vita-port/build/cmake/midp_vita.vpk` = **v01.58 b215 (143f5cd)**，
  13666113 B，MD5 `a1ca93a299fbdce68595853d7a8bf8c2`。

### 遗留

- **第 2 轮 JIT 崩溃根因仍未修**（v01.45）。默认 `jit=0` 不改变 v01.46 以来的稳定性；
  `jit=2` 是该崩溃的最小复现开关，`jit=1` 可拿回第 1 轮 JIT 性能——两者都**待真机**。
- JSR 75 `FileConnection`：上游无实现，需新写 Java 类 + native 接线；
  **需用户决策**后再动手（真机可用前建议暂缓）。

## 2026-09-14 v01.57：真机 core dump 取证——`pte_osSemaphoreCreate` 空 pHandle 落盘写空指针 + 全链路去 stdio 诊断包（vita-port `vita_crumb.c/.h`，phoneme-midp `midp_run.c`）

### 用户现象

真机跑一次后留下崩溃转储 `psp2core-1789319139-0x00001636c3-eboot.bin.psp2dmp`。
**当前没有可用真机复测**，故本轮只做静态取证 + 交付一个"下一次上机就能指认凶手"的诊断包。

### 取证（**纠正了第一版结论**）

- 第一版结论（已作废）：把 fault 归到 JDWP `VMEvent::exception_event`。错因是拿栈上残留
  地址当现场——SP±16B 与 saved r5 落在 `0x813c0f50/0x813c0f64` 的旧内容上是**陈旧栈残渣**。
- **真实现场**：活动 PC = `0x810bb25a`，指令 `strge r3,[r5]`，`r5 = 0`。
  该地址落在 `pte_osSemaphoreCreate` 内（函数首 `0x810bb234`）：
  `push {r4,r5,lr}; mov r2,r0; sub sp,#12; movs r4,#0; movw r0,#0x1848; mov r5,r1;
   movw r3,#0x7fff; mov r1,r4; mov r4,lr; str r4,[sp,#0]; movt r0,#0x812b;
   blx sceKernelCreateSema; subs r3,r0,#0; itet ge; movge r0,r4; movlt r0,#2;
   strge r3,[r5,#0]`
  → `r5` 就是 `pHandle` 出参。`sceKernelCreateSema` **成功**（r0>=0 ⇒ ge 分支），
  随后把句柄写回 `*pHandle` 时 `pHandle == NULL` ⇒ 空指针写。
- **为什么不是 JDWP**：`_debugger_active` 只由 `JavaDebugger.cpp:1112`
  (`connect_java_debugger`) 置位，而 `Frame.cpp:948` 的异常钩子以
  `if (_debugger_active)` 门控；且 `USE_ON_DEVICE_DEBUG=false`，
  `midp_run.c` 的 `midpInitializeDebugger` 不会自动注入 `-debugger -nosuspend`。
  整条 JDWP 链是死的。
- **主要假设**：某个**VM 工作线程的首次 stdio 写**触发 newlib 惰性 FILE 锁初始化，
  该路径调用 `pte_osSemaphoreCreate` 时传了 NULL 出参。（Vita3K 对此宽容——与
  之前 `g_caps_tone`、PNG 编码器同类的"模拟器盲区"。）

### 修复（本轮 = 诊断包，不是根修）

1. **新增 `vita-port/src/vita_crumb.{c,h}`**：完全绕开 stdio 的面包屑通道。
   - `crumb_append(path, s, len)`：`__atomic_exchange_n` 门闩 + 按路径缓存句柄 +
     `sceIoOpen(O_CREAT|O_WRONLY|O_APPEND)`/`sceIoWrite`，**不碰任何 FILE\***。
   - `crumb_marker/printf/flush`，宏 `CRUMB(...)` / `CRUMB_SEC(l)`。
   - `extern int __real_pte_osSemaphoreCreate(int, void**);` +
     `int __wrap_pte_osSemaphoreCreate(int initialValue, void **pHandle)`：
     每次创建都记 `[pte] sem create init=%d handle=%p -> %p rc=%d from %p`
     （`__builtin_return_address(0)` = 调用者地址）；`pHandle == NULL` 时记
     `[pte] BLOCKED NULL-pHandle sem create from %p` 并直接返回 2
     （= 内核失败分支的同一 pte_osResult），不再走空指针写。
2. **热打印路径全部去 stdio**（这就是"下一次不会再崩"的那一刀）：
   - `phoneme-midp .../native/midp_run.c` 的 `JVMSPI_PrintRaw`：逐字符
     `fopen/fwrite/fclose` → `crumb_append("ux0:/data/vm_output.log", s, length)`，
     之后照旧 `pcsl_print_chars`。文件内加了 `crumb_append` 的 **weak no-op 兜底定义**
     （保证独立链接 `libmidp.so` 时不因缺符号失败；VPK 链接里 `vita_crumb.c` 的强符号胜出）。
   - `vita-port/src/vita_pcsl.c` 的 `pcsl_print_chars`：`fprintf(stderr,...)` →
     `crumb_append("ux0:/data/vm_stderr.log", s, len)`。
3. **启动器锚点** `vita-port/src/vita_main.c`：`crumb_marker` 打在
   launcher start / net early init done / round begin / runMidlet enter / runMidlet exit，
   外加 `crumb_printf("version: %s", ...)`、`crumb_printf("launch: %s / %s", jar, class)`、
   `crumb_printf("runMidlet returned %d", status)` + `crumb_flush()`（块内声明保持 C89 合法）。
4. **链接期拦截** `vita-port/CMakeLists.txt`：源列表加 `src/vita_crumb.c`；
   链接块加 `-Wl,--wrap=pte_osSemaphoreCreate`（必须排在 `-lpthread` 之前）。

### 构建/产物

- 重编：`cd vita-port && export VITASDK=/home/zyb/.local/vitasdk && export PATH=$VITASDK/bin:/home/zyb/tools/jdk8u502-b07/bin:$PATH && rm -f build/cmake/vita_version.h && ./build.sh`
  → **Build successful**（版本号同时 bump 01.55 → 01.57）。
- 产物：`vita-port/build/cmake/midp_vita.vpk` = **v01.57 b214 (199c975)**，
  13666575 B，MD5 `4a488fc550ce579a664e9a3bb22107ab`。

### 验证入口（二进制级已过）

- `strings midp_vita | grep "version: J2ME"` → `v01.57 b214`
- `arm-vita-eabi-nm midp_vita | grep pte_osSemaphoreCreate` →
  `__wrap_pte_osSemaphoreCreate @0x8100b4bc` + `pte_osSemaphoreCreate @0x810bb51c`
  （重定向已生效：`pthread_mutex_init`/`sem_init` 走 wrapper，只有 wrapper 调真函数）
- `nm | grep crumb` → `crumb_append/flush/marker/printf` 全在位。

### 下次上机怎么做（**待真机验证**，本轮无机器）

1. 先删 `ux0:/data/J2ME00001/crumb.log`（避免旧内容混淆）。
2. 装上 v01.57 跑一遍。
3. 打开 `ux0:/data/J2ME00001/crumb.log`：出现
   `[pte] BLOCKED NULL-pHandle sem create from 0x<caller>` 那一行的 `<caller>` 就是凶手，
   用**本次**构建的 `build/cmake/midp_vita`（含符号、`main` 在 `0x81000060`）
   `arm-vita-eabi-addr2line -e midp_vita -f -C 0x<caller>` 即可定位。
   （**注意**：dump 里的 `0x8105e099`/`0x810bb25a` 等地址来自**上一个**二进制，
   重链后全部作废，别再拿旧数字对地址。）
4. 若 `crumb.log` 里连 launcher 锚点都没有 ⇒ 崩在比 `vita_main` 更早的阶段。

### 遗留

- 根因（谁把 NULL 传进 `pte_osSemaphoreCreate`）本轮**未修**，只做了断路 + 取证；
  真机日志到手后再收敛。
- 已知无害项：`libmidp.so` 链接尾部报 `undefined reference to _rom_linkcheck_mffd_false`
  （`MIDP.gmk` 给 ROMImage.o 硬编码 `-DMSW_FIRST_FOR_DOUBLE=1` 而 `libcldc_vm.a` 是 0）；
  `phoneme-midp/build/vita_arm/bin/arm/` 从来没产出过 `libmidp.so`，VPK 走 `libobj.a`
  + 启动器 `vm_rom_stubs.c`（其中已定义该符号），**VPK 链接不受影响**。
- JIT 二次启动根因仍未修（`-int` 掩盖中）——见 v01.45 后续章节。

## 2026-09-13 v01.56：真机安装 0x8010113D 定案——sce_sys PNG 必须是索引色（vita-port `assets/sce_sys/`）

### 定案过程（12 包 A/B 矩阵，真机实测）

历时三轮的排查矩阵（A–Q 包，全在 `build/test_vpks/` 生成后 FTP 实测）：

| 包 | 变量 | 真机结果 |
|---|---|---|
| A 无data / C 完整换ID / D 未压缩SELF / E 修template | 我们 sfo + **RGB PNG** | ❌ 全败（88–99% 漂移） |
| H hello eboot + 我们壳 | **RGB PNG** + 手写 gate template | ❌ |
| F hello+2MB填充 / G hello+16MB填充 / J python重打包hello | 纯 hello + python zipfile 打包 | ✅ 全过 |
| I 我们的 eboot + hello 壳 | eboot 变量 | ✅（eboot 洗清） |
| N 我们的完整 sfo + hello 资源 | sfo/TITLE_ID 变量 | ✅（sfo 洗清，含数字前缀 J2ME） |
| O hello + 仅 TITLE_ID=JMEE00002 | 数字前缀理论 | ✅（该理论作废） |
| Q 我们 RGB PNG + hello 原版 template | **PNG 单变量** | ❌ **定罪** |
| P 我们全内容 + **索引色 PNG** | 修复验证 | ✅ **定案** |

### 根因

- **SceShell/promoter 拒绝真彩色（color-type 2）PNG**，只接受索引色（color-type 3）。hello_world 三个资源全是 palette 型（bg 甚至是 1-bit），我们的全是 8-bit RGB——尺寸合规（128×128/840×500/280×158）但编码不对。
- **Vita3K 不校验** → 模拟器永远能装，掩盖问题。
- 失败点漂移（88/92/99%）= promoter 收尾逐个解码资源，死点随包内内容变化——"固定字段错误失败点应稳定"的反向推理是关键突破口。

### 修复（vita-port 提交 f43ff34）

- `icon0.png`/`bg.png`/`startup.png`：RGB → 8-bit palette 重编码（纯 Python zlib 手写 PNG 编解码，无 Pillow/ImageMagick 可用；与真机验证过的 P 包逐字节同源）。
- `template.xml`：`content-ver="01.00"` → `content-rev="1"`；startup 从 frame2/liveitem 移到标准 `<gate><startup-image>` 写法。
- 正式包重建核验：三 PNG ctype=3、template 含 gate、191 条目 testzip OK（13,664,532 字节，MD5 a5ba9e64）。

### 教训

1. **对照样本要逐字节剖到编码层**——"PNG 尺寸合规"不等于"PNG 合法"，color-type 差异肉眼不可见。
2. **A/B 矩阵是最后手段但极有效**——12 个包把 eboot/sfo/体积/传输/打包器/PNG 六个假设全部一次洗清。
3. **失败点漂移是数据敏感错误的指纹**——固定字段错误失败点稳定，漂移意味着校验器在扫描内容流。
4. 模拟器宽容性 = 排查盲区：Vita3K 不查的东西（MEMSIZE、PNG 编码）恰好都是真机雷区。

## 2026-09-13 v01.55：总根因定案——`pcsl_string` 桩违反上游 NUL 终止语义，所有 RMS 库共享同一个物理文件（vita-port `vita_pcsl.c`）

### 用户报告（v01.54 实测）
> "第一次保存书签，提示保存成功，实际没有，退出的时候提示存储空间少。第二次进卡在初始化。"

### 决定性证据（v01.54 的 `name=` 面包屑 + FFFFFFFF hexdump）
```
[rms] open name='coreA' suite=-1 exists=0 (new store)     ← 新建库，写全新头（signature+next=1+时间戳）
[rms] open name='A'     suite=-1 exists=1 next=1 live=0 ver=0 data=0   ← A 读到的正是 coreA 刚写的头！
[rms] open name='coreA' ... exists=1 next=2 live=1 ver=1 data=0        ← coreA 二开读到别人的写入结果
[rms] open name='G2'/'B'/'P'/'CK'/'R' ... 全部 exists=1 next=2 live=1 ver=1 data=0
[rms] SALVAGE ... scan_end=104  /  64 bytes preserved   ← 所有库完全相同
FFFFFFFF 文件：next=2 live=1 ver=1 data=0 + offset40 一个空闲块(id=-1,size=0x38)，共 104 字节
```
**不同名字的库在读同一个物理文件 `appdb_1A35/FFFFFFFF`——库名与 .db 后缀根本没进路径。**

### 根因（上游语义 vs 我们的桩）
上游不变量（`phoneme_source/phoneME/pcsl/string/utf16/pcsl_string.c`，对读确认）：
1. `pcsl_string.data` **永远以 NUL 结尾**，`->length` **计入这个 NUL**（`:110` "Do not count terminating '\0'" 是访问器的职责）
2. `PCSL_STRING_EMPTY = {&zero_char, 1, 0}`（length=**1**，不是 0）
3. 字面量宏 `PCSL_DEFINE_*_LITERAL`：`length = sizeof(arr)/sizeof(jchar)`（NUL 含在内）
4. `pcsl_string_cat`（`:490`）**吃掉第一个串的结尾 NUL**（"Strip the terminating zero at the end of the first string"）再拼接
5. `midp_suiteid2pcsl_string(-1)` 返回 `{L"FFFFFFFF\0", length=9}`（静态缓冲区，8 位 hex+NUL）

我们的桩（`vita-port/src/vita_pcsl.c`）全部违反：
- `PCSL_STRING_EMPTY = {NULL, 0, 0}`；`pcsl_string_length` 返回 `->length` 原值不减 1
- `cat`/`append`/`append_buf`/`append_char` 原样拷贝 `->length` 个 jchar → **字面量的内嵌 NUL 被拼进路径中部**
- `convert_to_utf8` 按 `->length` 逐字转换（含内嵌 NUL），`snprintf("%s")`/`sceIoOpen` 在第一个 NUL 截断

于是 `root + "FFFFFFFF\0" + "A" + ".db"` 的 C 视图 = `…/rms/appdb_1A35/FFFFFFFF`——**全部库同文件**。这解释了一切症状：
- "保存成功实际没有"：写到共享文件，下次开库 header 又被别的库覆盖
- "二次进全丢"：每次开库都读到别的库的 header → bad chain → salvage
- "存储空间少"：所有库的写入叠在一个文件里，反复 salvage/compact 把它搅成浆糊
- `[pcsl] unlink blocked .../FFFFFFFF`：UC 删自己的库（路径=FFFFFFFF）时句柄还开着
- 上游 `rms.c`/`suitestore_intern.c` **零 bug**（diff IDENTICAL 证实）——问题全在我们自己的 pcsl 桩

### v01.55 修改（`vita-port/src/vita_pcsl.c`，全部标注 v01.55）
1. `PCSL_STRING_EMPTY` → `{&static_zero, 1, 0}`（与上游一致）
2. `pcsl_string_length`/`utf16_length`：末位是 NUL 则减 1
3. `pcsl_string_utf8_length`/`convert_to_utf8`：只转换内容（停在终止零）——消除 C 串截断点
4. `convert_from_utf8`/`from_utf16`：**剥掉全部尾部 NUL**（上游 `:315`"Strip trailing zero characters"）+ 存 `length+1`（含单个终止零）
5. `pcsl_string_cat` 重写：吃掉首串结尾 NUL、保证结果恰好一个终止零、空结果返回 EMPTY 常量（**核心修复**）
6. `pcsl_string_dup`：非堆（常量/字面量）直接结构体拷贝（上游语义，避免 free 静态数据）
7. `append`/`append_char`/`append_buf`：全部改走 cat/convert_from_utf16
8. `equals`/`compare`/`starts_with`/`ends_with`/`index_of*`/`convert_to_jint`/`substring`：全部改为内容长度语义（终止零不参与）
9. `pcsl_file_open` 新增 `/rms/` 路径一次性面包屑 `[pcsl] open path=<abs> flags=0x.. size=<N>`（8 槽去重）——下一轮日志直接看到每个库映射到哪个物理文件

### 构建验证（v01.55）
- `./build.sh midp`：`Build successful`，VPK `build/cmake/midp_vita.vpk` 13,679,669 B，`vita_version.h`=01.55 b211（build 号没变是正常的，计数器按 git hash+配置）
- ELF 内 ASCII 验证：`[pcsl] open path=`=1、`01.55`=2、`01.54`=0
- `libmidp.so` 的 `collect2: error: ld returned 1 exit status`（`_rom_linkcheck_mffd_false` 未定义）是已知无害噪音（v01.53 起就有，最终 ELF 由 CMake 链接成功）
- `git diff --stat`：`vita_pcsl.c` 本轮大幅修改（pcsl_string 系列全量对齐上游）

### 下一轮验证判据（v01.55 实测）
1. **`[pcsl] open path=...` 应当出现多个不同路径**：`…/FFFFFFFF/A.db`、`…/FFFFFFFF/coreA.db`、`…/FFFFFFFF/G2.db`…——每个库一个文件，不再全是裸 `FFFFFFFF`
2. `[rms] open name='X'` 各库头状态**互不相同**（不再全部 `next=2 live=1 ver=1 data=0`）
3. `[rms] salvage` 不应再对每个库反复触发；书签保存后二次进入仍在
4. 旧数据：共享文件 `FFFFFFFF` 里只有 104 字节垃圾（空闲块），无有效数据可救——**首次进 v01.55 后各库从零开始是预期行为**，之后才是真持久化
5. 注意：v01.54 的 `SALVAGE skipped ... 64 bytes preserved` 保护了这个共享文件没被清零（v01.53 的零收获保护立功了）

### 遗留 / 未做
1. `pcsl_esc_attach_string` 仍是 dup 桩（非上游转义编码）。RMS 库名是 ASCII（A/B/G2/coreA）时 dup 与真转义等价；**中文名/特殊字符库名仍可能撞路径**——真转义实现留待需要时从 `phoneme_source/phoneME/pcsl/escfilenames/pcsl_esc.c` 移植
2. `pcsl_string_convert_to_jlong` 仍是 jint 精度桩（无调用方受影响，观察）
3. `[pcsl] unlink blocked .../FFFFFFFF` 调用方定位：v01.55 修复后该路径不再被构建，预计自然消失
4. 上游 `pcsl_string.c` 还有 `pcsl_utf16_convert_to_utf8` 等转换助手未移植——目前手写转换已覆盖需求


## 2026-09-12 v01.54：第二次进 UC 书签/历史全丢——三个真凶：db 头永不落盘、抢救把空闲块当 8 字节跨、抢救"零收获"时毁灭式重写（phoneme-midp `RecordStoreImpl.java` + vita-port `CMakeLists.txt` 版本号）

### 用户报告（v01.53 实测）
> "第二次进 uc，之前保存的书签，历史记录都没了。和首次进入一样。"

v01.53 把"空间不足"压下去了，但**跨次启动持久化**这条线没修好：第一次进能存，第二次进就是空的。

### 日志证据（用户提供的 v01.53 stderr）
```
[RMS] created appdb: ux0:/data/J2ME00001/rms/appdb_1A35
[rms] salvage (bad chain) live=0 data_size=64
[rms] SALVAGE live=1 max_id=1 old_data_size=64 scan_end=104
[pcsl] unlink blocked, file open elsewhere: ux0:/data/J2ME00001/rms/appdb_1A35/FFFFFFFF   (重复多次)
[rms] salvage (bad chain) live=1 data_size=0
[rms] SALVAGE live=0 max_id=0 old_data_size=0 scan_end=5280        ← 5KB 数据被就地抹掉
第二次进 UC:
[rms] salvage (bad chain) live=0 data_size=64        ← 与第一轮开头一字不差
[rms] SALVAGE live=1 max_id=3 old_data_size=64 scan_end=5280
[rms] salvage (empty header, live blocks present) data_size=0
[rms] SALVAGE live=1 max_id=54307759 old_data_size=0 scan_end=6032  ← 垃圾块被收下
[rms] compact skipped: block chain inconsistent
```
`FFFFFFFF` 文件 8464B 的 hexdump 显示 header 自洽（`next_id=0x033CABB0`、`live=1`、`data_size=5248`）但 **offset 40 起全是 0**——头和块对不上，人眼可见。

### 三个根因（都能独立造成"数据看起来没保存"）

**1. db 头的写入只进 `midp_file_cache`，永不落盘（持久化缺失）**
- 上游 `RecordStoreImpl` 里 **10 处 `// dbFile.commitWrite();` 全是注释掉的**（`phoneme_source` 原样如此，见其 215/454/514/714/863/884/1040/1086/1118 行，只有 1228 行抢救路径是活的），上游靠"关库时 cache 落盘"。
- **Vita 上库从不被关**：`vita_main.c` 的 launcher 主循环让 VM/MIDlet 常驻（`runMidlet` 返回后进程不退出），所以：
  - `addRecord` 写的 db 头 = 4~20 B → 进 `midp_file_cache`（`RMS_CACHE_LIMIT=3072`）；
  - 记录 payload 大 → **`uncachedWrite` 直接下盘**（超过 cache limit）。
  - ⇒ 磁盘上出现"**payload 在，header 不在**"的结构：`next_id=1 live=0 data_size=0` 压着一个 5KB 块。这正是每次 `salvage (empty header, live blocks present)` 的形状，也是 MIDlet 每次都拿到"空库"的机制。
- **修复**：在 MIDP 规范规定的**持久化提交点**上恢复 commit——`addRecord`（规范原文 "The record is written to persistent storage before the method returns."）、`deleteRecord`、`setRecord`。子步骤 `addBlock`/`freeBlock`/`writeBlock` 不各自提交（否则一次 addRecord 刷三次），`compactRecords` 末尾本来就有 `truncate()` 带刷。

**2. 抢救扫描把空闲块当"跳过 8 字节 block header"处理（垃圾块的来源）**
- 规范走法是 `currentOffset += currentSize`，**活块和空闲块一样按整块跨**（`RecordStoreIndex.getRecordIDs()` 与 `getFreeBlock()` 均如此，已对读确认）。
- v01.52/v01.53 的 `salvageStore()` 只在"活块"分支 `offset += size`，**`else` 分支一律 `offset += BLOCK_HEADER_SIZE`**——于是扫描器踩进空闲块的 payload 里、按随机字节"重新同步"，把 payload 当成块头收下 → 日志里 `max_id=54307759` 就是这么来的（`1615147117` 同源）。随后按这个垃圾块致密重写 + 截断，**真实记录被覆盖/截掉** = 第二次进书签没了。
- **修复**：新增分支 `id < 0 && dataSize >= 0 && size > 0 && blockFitsInFile(offset, size)` → `offset += size`（健康空闲块按整块跨）；只有真正坏头才走 `+= BLOCK_HEADER_SIZE` 重新同步。

**3. 抢救"零收获"时是毁灭式的（v01.52 起就埋着）**
- 原逻辑：`liveCount==0` → 照样致密重写（无内容）→ 把 `[40, scanEndOffset)` **整段零填** → 写一个 `data_size=0` 的空头 → `truncate(40)`。日志里的 `SALVAGE live=0 max_id=0 ... scan_end=5280` 就是**一个 5KB 库被物理抹掉**，不可恢复。
- 若扫描器本身误判（根因 2 正是），这一步就把用户数据从"没读出来"变成"真没了"。
- **修复**：`liveCount == 0 && scanEndOffset > DB_HEADER_SIZE` 时**直接 return，不写不填不截**，只打一行
  `[rms] SALVAGE skipped name='<store>': no live block found, <N> bytes preserved`。
  代价仅是该库这次仍以空库打开、下次还会再扫一遍；**任何情况下都不能因为"我读不懂"而销毁唯一一份数据**。

### v01.54 修改
**A. `phoneme-midp/src/rms/rms_api/reference/classes/com/sun/midp/rms/RecordStoreImpl.java`**
1. `addRecord`/`deleteRecord`/`setRecord` 三处 `dbFile.commitWrite();` 恢复（带 VITA 说明注释）。
2. `salvageStore(String storeName, byte[] dbHeaderData)` 签名加库名（构造器 3 个调用点同步），日志全部带 `name='...'`——**下一轮日志必须能直接看出是哪个库**。
3. 扫描循环新增"健康空闲块按整块跨"分支（根因 2）。
4. `liveCount==0` 且文件非空时提前 return（根因 3）。
5. 构造器新增开库面包屑（`exists` 两分支各一条，含 `next/live/ver/data/free`）：
   ```
   [rms] open name='' suite=-1 exists=1 next=.. live=.. ver=.. data=.. free=..
   [rms] open name='' suite=-1 exists=0 (new store)
   ```
   **每次开库头状态一次打全**，"上次存的数据这次还在不在盘上"一眼可判，不再靠猜。
**B. `vita-port/CMakeLists.txt`**：`VITA_VERSION` 01.53 → 01.54。

### 构建验证（本轮）
- `./build.sh midp`：`Build successful`，VPK 13,678,862 B（`build/cmake/midp_vita.vpk`），`vita_version.h` = `01.54 b211`。
- ROM 链验证（遵守 v01.53 教训 1，不看日志看字节）：ELF 内 **UTF-16LE** 计数
  `SALVAGE skipped name=`=1、`open name='`=1、`salvage (bad chain) name=`=1、`salvage (empty header, live blocks present) name=`=1、`SALVAGE name='`=1（UTF-8 全 0，符合 Java 串规则）；`01.54` ASCII=2 / `01.53`=0。
- `git diff --stat`（phoneme-midp）：`RecordStoreIndex.java` +13、`RecordStoreImpl.java` +663/-34（累计，含 v01.49~v01.53）。

### 下一轮日志怎么看（v01.54 实测判据）
1. 每个库开库一行 `[rms] open ...`：**第一轮写过数据后，第二轮开库时 `data`/`live` 是否与写入时一致**。
   - 若一致、且没有 `[rms] salvage` ⇒ 持久化修好了（根因 1 成立）。
   - 若 `exists=1` 但 `next=1 live=0 data=0` 压着非空文件 ⇒ header 仍然没落盘，继续查 `commitWrite` 之后的 cache 路径。
2. 出现 `[rms] salvage (bad chain) name='X'` 时看 `X` 是谁——把 8464B 的 `FFFFFFFF` 和"书签库"对上号（空名库才是 `FFFFFFFF`）。
3. `SALVAGE skipped ... bytes preserved` 出现即说明扫描器仍有盲区，**但数据没有被销毁**，可以再迭代抢救器。
4. `max_id` 必须是正常量级（个位数/千位）；再出现 54307759 这种说明还有第二处垃圾来源。

### 遗留 / 未做
1. **`[pcsl] unlink blocked .../FFFFFFFF` 的调用方仍未定位**（候选：`rms.c:215 rmsdb_record_store_delete`、`fileCache.c`、`suitestore_task_manager.c`、`storageFile.c`、`components_storage_kni.c:173`）。若该路径是在"每次启动删库重建"，那它本身就是数据丢失的一环——v01.43 的"held 就拒删"保护了数据，但也可能让 MIDlet 的删除语义失败后走"当作新库"分支。**下一轮用 `name=` 面包屑对照 `unlink blocked` 的时序**。
2. `midp_file_cache_truncate` 的记账仍未与钳后 `size` 对齐（v01.53 观察项，非本次根因）。
3. `midp_file_cache` 仍是**单例且只缓存一个文件**：空名库的 db/idx 共用 `FFFFFFFF` 一条路径时，第二个句柄的 I/O 全部退化为非缓存路径，`cachedFileSize`/`cachedAvailableSpace` 与之无关——**长期项**，涉及共享 native 文件，改动要谨慎（最小 diff + 明确收益再动）。
4. v01.54 **尚未在设备/Vita3K 实测**；上面判据是设计预期。

## 2026-09-11 v01.53：UC 报"RMS 使用空间不足"——v01.52 抢救会接受垃圾块并把 data_size 写爆（phoneme-midp `RecordStoreImpl.java` + vita-port `vita_pcsl.c`）

### 用户报告（v01.52 实测）
> "uc 浏览器打开网页，提示 rms 使用空间不足。"

v01.52 解决了"第二次进 UC 记录/书签丢失"（抢救成功、数据回来了），但引入新症状：**空间永远报不足**。方向一致——数据回来了，但库的账目被写坏了。

### 根因定案（v01.52 的抢救缺少上界）
v01.52 之前那条日志是关键证据：
```
[rms] SALVAGE live=1 max_id=1615147117 old_data_size=0
```
`max_id=1615147117` 是**垃圾块头被当成活块收下**了（`0x6040602D` 之类的随机字节被解析成 `id`）。v01.52 的接受条件是 `id>0 && dataSize>=0 && size>0`，**对 `size`/`dataSize` 没有任何上界**，于是：
1. 致密重写按这个垃圾 `size` 推进 `writeOffset`（`writeOffset += size`，即使实读短了也照样加）；
2. header 重算 `data_size = writeOffset - 40` → **`getSize()` 变成天文数字**；
3. `getSizeAvailable()` = min(200000 − getSize(), storageFreeSpace) 的第一项变成**负数** → 钳到 0 → 每次 `addBlock()` 都抛 `RecordStoreFullException`；
4. 同时 `truncate(writeOffset)` 记下一个巨大的 logical clamp → 该 store 之后所有 size/read 都被钳到一个虚假大小。

**结论：不是磁盘真的满了，是 `data_size` 这一个 int 被垃圾块写爆，导致 200KB/每 suite 的额度恒为 0。** 这也解释了为什么 v01.52 之前"书签丢失"和 v01.53 "空间不足"会先后出现——同一处缺边界。

### v01.53 修改

**A. `phoneme-midp/src/rms/rms_api/reference/classes/com/sun/midp/rms/RecordStoreImpl.java`（抢救加边界 + 空间面包屑）**
1. 新增常量 `SALVAGE_MAX_BLOCK = RMSConfig.STORAGE_SUITE_LIMIT + BLOCK_HEADER_SIZE`（即 200008）、`SALVAGE_MAX_SPAN = 2 * RMSConfig.STORAGE_SUITE_LIMIT`、`PROBE_BYTE[1]`。
2. 新增 `blockFitsInFile(offset, size)`：`size<=0 || size>200008` 直接 false；否则 `seek(offset+size-1)` + `read(PROBE_BYTE,0,1)==1`（payload 末字节必须真实存在）。**垃圾 size 在第一道就被拒**。
3. `physicalFileHasLiveBlocks()` 改用 `blockFitsInFile()`（去掉局部 `byte[] one`）。
4. `salvageStore()` 扫描循环加跨度上界：`offset > DB_HEADER_SIZE + SALVAGE_MAX_SPAN` 即停（`scanEndOffset=offset; break;`），接受条件加 `blockFitsInFile(offset, size)`；日志补 `scan_end=`。
5. `getSizeAvailable()` 新增一次性命中面包屑（`spaceReported` 字段，只打一次）：
   ```
   [rms] space low area=<storageAreaId> fileSpace=<..> limitSpace=<..> data_size=<..> rv=<..>
   ```
   **三项一次打全**，"空间不足"到底是 per-suite 额度负数、还是磁盘 free space 归零，日志直接给答案，不用再猜。
6. 注意：`recordStoreName` 只是构造器**参数**、不是字段，面包屑里用 `RmsEnvironment.getStorageAreaId(suiteId)`（见下方构建陷阱）。

**B. `vita-port/src/vita_pcsl.c`（把"逻辑截断"补成真的物理截断 + 让 used space 认逻辑大小）**
1. 新增 `vita_logical_size_for(abs_path)`：在 `g_open_handles` 里找同路径、`logical_size >= 0` 的句柄，返回最小逻辑大小，否则 -1。
2. 新增 `vita_physical_truncate(abs_path, size)`:先 `truncate(path, size)`（newlib 路径版，Vita 上是 `sceIoChstat` 包装），失败再 `open(O_WRONLY)`+`ftruncate(fd)`+`close`；`size<0` 或 `app0:` 前缀直接返回 -1。
3. `pcsl_file_truncate()` 重写：
   - **先取 `cur_pos` 再 `SEEK_END`**（旧代码先 SEEK_END，"恢复位置"实际是 seek 回 EOF，是潜伏 bug）；
   - 已有 clamp 时取更小值（`size = min(size, logical_size)`），保证物理收缩永不落在给调用方看的大小之上；
   - `!vita_handles_held_ex(path, vf)` 时才真收缩（空名 store 的 db/idx 共用一个路径，**twint handle** 情况下收缩会剪掉对方还相信的数据）；成功后若物理 EOF `<= size` 就清掉 clamp。
4. `pcsl_file_close()`：关闭前取 `pending = logical_size`，`sceIoClose` 后若无人再持有该路径则 `vita_physical_truncate(path, pending)`——**clamp 在关文件时落地**，不留"逻辑小了但磁盘没回收"的尾巴。
5. `pcsl_file_getusedspace()`：逐目录项拼 `abs_dir + "/" + d_name`，用 `vita_logical_size_for()` 取逻辑大小，按 `(logical>=0 && logical<physical) ? logical : physical` 计入；并加一次性诊断 `[pcsl] getusedspace <dir> = <N> bytes`（4 槽去重、`total>1MB` 才打）。**这是"物理 st_size 被当成 used space"的直接修复**：bloated 的物理文件不再把 free space 压到 0。
6. `vita_handles_held_ex()` 去掉 `__attribute__((unused))`（重新被 `pcsl_file_truncate` 使用）。

**C. `vita-port/CMakeLists.txt`**：`VITA_VERSION` 01.52 → 01.53。

### 构建陷阱（本轮新增，务必记住）
1. **javac 失败会静默跳过 ROM 重生成，而 `build.sh` 仍然打印 "Build successful"**：`./build.sh midp 2>&1 | grep ...` 里 `make` 的失败在管道中被吞掉。第一次改完 `RecordStoreImpl.java` 引用了不存在的 `recordStoreName`，`javac` 报 `cannot find symbol` → `classes.zip` / `ROMImage_05.cpp` / `ROMImage.o` / `libobj.a` **全部停在旧时间戳**（11:46），而 VPK 照样产出、脚本照样说成功。**改 Java 后必须用时间戳链证明 ROM 真的重生成**（见下）。
2. **判断"Java 改动进产物没有"的时间戳链**：`build/vita_arm/classes/com/sun/midp/rms/RecordStoreImpl.class` → `classes.zip` → `ROMImage_05.cpp` → `obj/arm/ROMImage.o` → `obj/arm/libobj.a` → `vita-port/build/cmake/midp_vita`，必须**逐级晚于源文件**。本次实测：源 16:32 → class/classes.zip 16:52 → ROMImage_05.cpp/ROMImage.o/libobj.a 16:53 → ELF 16:53。日志中可见 `... searching updated .java files` / `... compiling 1 .java files` / `romgen -romconfig ... -romize`。
3. **字符串验证一分为二**（v01.52 定的 UTF-16LE 规则只对 Java 串成立）：Java 串（`ROM_BL`，UTF-16LE）用 `data.count(s.encode('utf-16-le'))`；C 串（`vita_pcsl.c`）用 `data.count(s.encode())`。本次实测：`[rms] space low area=` utf16le=1 / `scan_end=` utf16le=1 / `[pcsl] getusedspace ` ascii=1。`.self` 是压缩的，查串只能查 `build/cmake/midp_vita`。
4. **Vita newlib 的 truncate 家族（用 objdump 反汇编 `lib_a-truncate.o` 定案）**：`sceIoTruncate` 不存在；`truncate(path,size)` = `sceIoChstat(path, SceIoStat{st_size})` 纯路径版；`ftruncate(fd,size)` 走 `__vita_fd_grab(fd)`，而该函数只认 newlib 自己 fd 表里的 **1..255**，**`sceIoOpen` 拿到的 SceUID 不能直接喂给 `ftruncate`**。⇒ 路径版 `truncate()` 是唯一可靠路线，`ftruncate` 只作为 `open()` 得来句柄的备选。
5. **`midp_file_cache` 是全局单例、且只缓存第一个文件**（`midp_file_cache_truncate` 里 `cachedAvailableSpace += cachedFileSize - size` 用的是调用方给的 `size`，不是 `pcsl_file_truncate` 实际生效的钳后值）。**本轮未改**：实测该差额只在"物理收缩失败（twin handle）"时才出现，方向是少记可用空间（保守），不足以解释本次症状；留作观察项，不再无凭据地动它。

### 实测预期（v01.53）
- 打开 UC 时 stdout 出现 `[rms] space low area=0 fileSpace=<N> limitSpace=<M> data_size=<K> rv=<R>`——**看这三项谁是 0/负数即可定案**：
  - `limitSpace` 为负/0 且 `data_size` 巨大 ⇒ 库头被写爆（v01.52 的垃圾块），需要一次抢救把 `data_size` 收回真实值；
  - `fileSpace` 为 0 ⇒ 磁盘账目问题，看同一轮的 `[pcsl] getusedspace <dir> = <N> bytes` 是否仍然偏大。
- 坏库首次开库：`[rms] SALVAGE live=N max_id=M old_data_size=K scan_end=E`，`max_id` 应当是**正常量级**（不再出现 1615147117 这种）。
- 正常库：无 `[rms]` 输出。

### 遗留 / 未做
1. **`midp_file_cache_truncate` 的记账未与钳后的 `size` 对齐**（见构建陷阱 5）——已评估，非本次根因，未改。
2. **`[pcsl] unlink blocked, file open elsewhere: .../rms/appdb_1A35/FFFFFFFF` 的调用方仍未定位**（空名 store 的 db/idx 共用 `FFFFFFFF` 路径）。候选：`rms.c:215 rmsdb_record_store_delete`、`fileCache.c:84/259`、`suitestore_task_manager.c:452`、`storageFile.c:183`、`components_storage_kni.c:173`。
3. **v01.53 尚未在设备/Vita3K 上实测**，上面"实测预期"是设计预期，不是观察结果。
4. 顺手清理：`vita-port/build/cmake/midp_vita.vpk.out.0nUsn9`（v01.52 jar 自吞噬事故留下的 72GB 临时文件）已删除；`samples/j2me/09:20，理论上用户装的可能是`（误重定向产生的 63B 垃圾文件，09-09 09:58）仍在，待确认后删。

## 2026-09-11 v01.52：v01.51 的"坏链重置"被证实抹掉用户数据——改为抢救（salvage）策略

### 用户报告（v01.51 实测）
> "还是存在问题，第二次进 uc 记录和书签都没了，和首次进入一样。"

用户附的 `FFFFFFFF` hexdump 是决定性证据：`sig=midp-rms` / `next_id=1` / `num_live=0` / **`version=1`** / `data_size=0` / `free_size=0`，**物理文件 5856 字节**、偏移 40 起还有 `id=-1 size=168` 的自由块。`version=1` 只可能由 v01.51 重置块写入（`else` 初始化分支不写 version）⇒ **v01.51 的自愈在含 5856 字节活数据的库上触发了，把库抹成空**。

### 根因定案（为什么"简单写入/seek 搞不好"）
1. **`midp_file_cache` 的写缓存在内存**：小的 header 写（`data_size`/`num_live` 等 4~20 字节）走缓存，块数据大写直通磁盘。UC 退出若未走 `closeRecordStore()`（或进程被杀），磁盘留下"块数据新、header 旧"的错位文件——这就是链校验检测到的不一致来源。**块本身几乎总是完好的，坏的只是 header 的口径。**
2. **v01.51 的应对（重置为空）方向性错误**：把 5856 字节的完好数据当成垃圾扔掉。正确策略是**按物理内容抢救**。
3. `[pcsl] unlink blocked ... FFFFFFFF` 反复出现是框架清理循环（`rmsdb_remove_record_stores_for_suite` / `quietDeleteFile`）对空名 store 的删除尝试被预检挡住，**无害**（失败即保留活数据）。

### v01.52 修改（`RecordStoreImpl.java` 单文件，+1 新方法群替换 2 个重置块）
1. **`salvageStore(byte[] dbHeaderData)`（新）**：
   - 扫描到**物理 EOF**（不信 header 的 data_size——它就是谎言来源；被 v01.51 抹过的库 data_size=0 但记录物理还在，照样能找回）；
   - 坏块头按 8 字节滑步重同步；`id>0 && size>0` 收进活块表（上限 512）；
   - **按 id 去重**（陈旧尾巴里是旧副本；compact 把数据下移 ⇒ 最低偏移处是最新副本，取第一个）；
   - 活块**致密重写**到 `[DB_HEADER_SIZE, ...)`（边读边写、`n<=0` 即断尾丢弃该块及之后）；
   - header 按实际保留内容重算：`next_id = max(旧next_id, maxId+1)`（不复用已删 id，符合 RMS 规范）、`num_live=kept`、`version+1`、`data_size=writeOffset-40`、`free_size=0`；
   - **`commitWrite()` 后再 `truncate(writeOffset)`**（header 是 40 字节小写，会留在 midp_file_cache 里！必须先 flush 再钳制尾巴）。
2. **`physicalFileHasLiveBlocks()`（新探针）**：header 声称空库（`num_live==0`）但物理文件里存在"payload 末字节可读"的活块 ⇒ 抢救。payload 校验防止垃圾头（声称越界 size）每次开库空触发。**用户当前这台设备上被 v01.51 抹掉的书签，装上 v01.52 后首次启动即被此探针救回。**
3. **`isBlockChainConsistent()` 增强**：除原有检查外，交叉核对**扫描到的活块数 == header 的 num_live**（捕获"header 写丢失但块完好"的形态）。
4. 构造器两个重置块 → 改调 `salvageStore()`；面包屑统一 `[rms] salvage (...)` / `[rms] SALVAGE live=N max_id=M old_data_size=K`。
5. `compactRecords()` 的坏链早退（v01.51）保留不变。

### 实测预期（v01.52）
- **本机已被 v01.51 抹掉的书签**：首启 v01.52 时 stdout 出现 `[rms] salvage (empty header, live blocks present)` + `[rms] SALVAGE live=N ...`（N>0），书签应恢复。
- 正常库：无 `[rms]` 输出（探针在第一个活块处即终止，代价可忽略）。
- 若 `SALVAGE live=0` 且数据确实没了：行为等同 v01.51（空库重建），不会再更糟。

### 构建事实（沿袭 v01.51 定案，v01.52 修正两条）
1. ~~ROM 输入是 `classes.zip`~~（正确，保留）；~~`strings | grep` 可验证字符串常量~~（**v01.52 修正：错误**）。
2. **romgen 把字符串常量编码为 UTF-16LE 的 `ROM_BL` 宏打包**（见 `ROMImage_00.cpp` 的 `_rom_text_block0`），**不是明文 ASCII** ⇒ `strings <ELF> | grep '<面包屑>'` 必然为 0，**不能**当"改动没进产物"的证据。正确验证方式：`python3 -c "open('midp_vita','rb').read().count('<字符串>'.encode('utf-16-le'))"`。
3. `Logging.*` 编译期被剥（留痕只能 `System.out`）——仍然正确。
4. ~~CMake 的 jar 依赖 glob `*.class`~~ —— 方向正确但**激活了潜伏的 jar 自吞噬**（见第 5 条）。
5. **`MIDP_SYSTEM_JAR` 输出路径必须在 `classes/` 目录之外**：历史上它是 `${MIDP_BUILD}/classes/midp_system.jar` 而打包命令是 `jar cf <out> -C classes .`——输出在被打包目录内。旧 `DEPENDS` 盯目录 mtime 几乎不重跑（掩盖了 bug）；v01.51 glob `*.class` 让它**每次重跑**，每跑一次把上一代 jar 塞进自己 → 指数膨胀（实测 1.2MB → 138GB，`classes.zip` 又把怪兽打包 → 30GB）。**v01.52 已改为 `${MIDP_BUILD}/midp_system.jar`**。
6. `libmidp.so` 的 `_rom_linkcheck_mffd_false` 报错无害（仍然正确）。
7. 判"改动是否进产物"：时间戳单调链 + `javap -classpath classes.zip` + **UTF-16LE 字节串搜索**（第 2 条）。

## 2026-09-11 v01.51：块链损坏定位 + Java 侧面包屑 + ROM 出货链取证（phoneme-midp / vita-port）

### 用户报告（v01.50 实测）
> "删除 rms，第一次进正常，保存书签，第二次进卡在初始化，进不去。"

首启（空库）正常、存书签后二启卡死 ⇒ 差异只在**落盘的 RMS 内容**，与 v01.50 的"数据丢失"不同：这次头部合法，坏的是**数据区的块链**。

### 定案：v01.50 的守卫维度不够
- v01.49 挡 `calculateBlockSize <= 0`（块循环），v01.50 挡头部字段不可能值（data_size < 0 等），**都看不到"头部合法但数据区字节不是合法块头"**。
- 数据区出现非法块头（Vita3K 丢 O_TRUNC + `pcsl_file_truncate` 只是随句柄消失的逻辑钳制 ⇒ 陈旧尾巴留在 live 区），任何一次行走（`getRecordIDs` / `getRecordHeader_SearchFromTo` / `getFreeBlock`）都会跳过数据区末尾 ⇒ 找不到任何记录 ⇒ MIDlet 恢复状态时永远等不到数据 = "卡在初始化"。
- 更糟：CLOSE 时 `compactRecords()` 按"遇到的每个 8 字节头"搬数据并改写 data_size，遇到假头就搬垃圾并把 data_size 压小 ⇒ 首次启动还读得出的库被压成坏库。

### v01.51 修改
1. **`RecordStoreImpl.isBlockChainConsistent(byte[])`（新增，只读）**：按 `calculateBlockSize(getInt(header,4))` 走 `[DB_HEADER_SIZE, data_size+DB_HEADER_SIZE)`，每块必须 `size > 0` 且 `size <= 剩余`，最终必须**正好落在 dbSize**。只读，不写盘，只动文件位置。
2. **`compactRecords()`**：链不一致 → 直接 return（绝不基于坏链搬数据/改 data_size）。
3. **打开自愈（构造器 `if (exists)` 分支）**：链不一致 → 整店重置为 pristine 空店（next_id=1 / num_live=0 / version=1 / last_modified=now / data_size=0 / free_size=0）＝ 等价于用户手动"删 rms"，这是唯一验证过能让 UC 再启动的操作。
4. **`RecordStoreIndex.getFreeBlock()`**：块越界（`currentSize > size - offset`）→ 打印 + `return 0`（追加到末尾），绝不返回越界偏移覆盖活数据。
5. **`vita-port/src/vita_pcsl.c` `pcsl_file_seek()` 去掉位置钳制**：`storagePosition()` 只把 `-1` 当错误，v01.47 的"钳制到 logical_size 的正数返回值"会被当成成功 ⇒ file cache 位置与句柄位置静默分裂。
6. **Java 侧面包屑（本轮新增，见下）**：`System.out.println("[rms] ...")`。

### 本轮踩到并定案的构建事实（比修复本身更值钱）
1. **ROM 的输入是 `classes.zip`，不是 `midp_system.jar`**：`Defs.gmk:114  MIDP_CLASSES_ZIP ?= $(MIDP_OUTPUT_DIR)/classes.zip`，romgen 规则 `cldc_vm.gmk:227  $(MIDP_OUTPUT_DIR)/ROMImage.cpp: $(MIDP_CLASSES_ZIP) ...`。`midp_system.jar` 只是被塞进 VPK 的数据副本（UseROM 下运行时不用它）。
2. **romgen 去掉方法名**（ROMGEN_ARGS 含 `+EnableAllROMOptimizations`）⇒ 在 ROM/ELF 里 `strings | grep <方法名>` **必然为 0**，不能当"改动没进产物"的证据（本轮据此误判过一次，白白折腾）。可用的标记只能是**字符串常量**（`System.out.println` 的字面量一定进 ROM）。
3. **`Logging.REPORT_LEVEL` 是编译期常量**：release ROM 里 `if (Logging.REPORT_LEVEL <= Logging.WARNING) { ... }` **整块被 javac 删除**（`javap -v classes.zip` 里搜不到该字符串，实测为 0）⇒ Java 侧若想留下痕迹，**只能用 `System.out`**（tty → `JVMSPI_PrintRaw` → stdout/`vm_output.log`）。现有 phoneME 的 `Logging.report` 全部是 no-op。
4. **CMake 盯目录的依赖不可靠**：`add_custom_command(DEPENDS ${MIDP_BUILD}/classes)` —— javac **覆盖**已存在的 `.class` 不改目录 mtime ⇒ `midp_system.jar` 一直停留在 04:36 的旧货，v01.51 的类一度没进 VPK。已改为 `file(GLOB_RECURSE ... ${MIDP_BUILD}/classes/*.class)` + 资源文件作 `DEPENDS`。
5. **`libmidp.so` 报 `undefined reference to '_rom_linkcheck_mffd_false'` 是无害的**：VM 库（`libcldc_vm.a` 的 `_MergedSrc005.o`/`Interpreter_arm.o`）期望 `..._mffd_false`，而 romgen 生成的是 `..._mffd_true`，`vita-port/src/vm_rom_stubs.c` 提供前者补齐；最终 ELF 用 `libobj_no_main.a`（含 `ROMImage.o`）链接，两者共存故能解析。**该错误发生在 ROM 重生成/`libobj.a` 归档之后**，所以 `./build.sh midp` 会在这一步 make 失败但产物齐全，脚本仍打印 "Build successful"（管道吞了非零退出码）。
6. **判"改动是否进产物"的正确姿势**：时间戳单调链 `*.java → classes/*.class → classes.zip → ROMImage.cpp → ROMImage.o → libobj.a → libobj_no_main.a → midp_vita → midp_vita.vpk` + `javap -classpath classes.zip <类>` 看方法是否存在 + `strings <ELF> | grep <字符串常量>`。

### 实测预期（v01.51）
- 首启（空库）：无 `[rms]` 输出，正常。
- 存书签 → 二启：若库坏 ⇒ `midp_stdout.log`/`vm_output.log` 出现 `[rms] RESET store (bad chain) ...`，UC **应能启动**（库被重置为空，书签丢失但不再卡死）。
- 若二启**仍然卡初始化**且**没有** `[rms]` 输出 ⇒ 说明卡死与 RMS 块链无关，需要另抓 `midp_stderr.log` + `FFFFFFFF` 头 64B 重新定位。

## 2026-09-11 v01.50：RMS 存储砖死定案与自愈——data_size 负值头部被写盘（phoneme-midp 提交 a3fee44、vita-port 提交 60a2947）

### 用户报告（v01.49 实测）
1. UC 记录（历史/书签）没保存或无法恢复，第二次进和第一次一样。
2. 书签卡顿应该与 IO 无关，可能是网页加载中造成的。

### 法证闭环（用户提供 FFFFFFFF 文件 hexdump 3104B + midp_stderr.log + vita3k.log）
头部逐字段解析（布局见 AbstractRecordStoreImpl.java:48-87）：
- sig `midp-rms` ✅ / next_id=2 / **num_live=0**（零存活记录=用户看到"全丢"）/ version=4 / last_modified 2026-09 合法 / **data_size=0xFFFFFED8=-296** ❌ / free_size=0
- 偏移40 块头：id=-1(空闲), size=0xB38=2880 → 算术铁证：**compact 前 data_size=2584，2584-2880=-296 分毫不差**
- 第二次启动 `getSize()=40+(-296)=-256` = stderr 里 `[pcsl] seek with negative offset -256`（v01.49 拦截生效：仅 1 条、无崩溃、三次启动 MAIN_EXIT=2001 干净退出、23 秒空转消失）
- addBlock 走 `blockOffset=getSize()=-256` → 保存全部静默失败 → 报告#1 完全解释

### 坏文件产生机制（回答"每次装新版都删 rms 为什么还会坏"）
删 rms 是对的——坏文件不是旧版带入，是**本次会话运行时自己写坏的**：
1. Vita3K 丢 `O_TRUNC`（v01.41 实锤）→ compact 的 truncate 从不物理收缩 → 陈旧尾巴永存
2. 空白名 store 的 db/idx **共用一个文件**（buildSuiteFilename: nameLen>0 才加 .db/.idx 后缀）→ 互相踩
3. compactRecords 走块把垃圾字节当成 2880B 空闲块（`currentId<0 && currentSize>0`，v01.49 的 `<=0` 守卫拦不住）
4. 走完**无条件**执行 `data_size -= moveUpNumBytes` 并写回头部 → -296 上盘 → store 永久砖死
5. 用户每次删 rms 后 UC 一保存书签又确定性复现同一链条

### 修复（双守卫，phoneme-midp a3fee44）
- **compactRecords 写回前**：`compactedSize < 0` → 直接 return 放弃本次 compact（绝不写不可能的头部）；下次打开由开盖自愈兜底
- **openRecordStore(exists 路径)**：next_id<1 / num_live<0 / data_size<0 / free_size<0 任一命中 → 重置为干净空库（putInt 1/0/1/0 + 写回 DB_HEADER_SIZE）→ 曾砖死的文件复活，MIDlet 恢复保存能力；尾巴垃圾留在盘上但 data_size=0 不可见，首次 append 即覆盖

### 构建（Java 变更 → ROM 重生成链再次验证）
- `./build_vita.sh` EXIT=0，RecordStoreImpl.class 10490B 与 libobj.a 同批 04:24；`libmidp.so` _rom_linkcheck 失败照旧无害
- vita-port：需 `export PATH=/home/zyb/tools/jdk8u502-b07/bin:$PATH`（javac）+ `rm build/cmake/vita_version.h` 破版本缓存
- 产物 `midp_vita.vpk`，`strings` 验证 `v01.50 b209`

### 实测预期
- 第一次启动 UC：开盖自愈把 -296 头部重置为空库（日志可观察 seek -256 不再出现）
- 保存书签/历史后退出重进：**数据应存活**（compact 守卫阻断 -296 再产生）
- 若仍复现：抓 `rms/appdb_1A35/FFFFFFFF` hexdump 头 64B + midp_stderr.log，重点看 data_size 是否再次为负（若为负说明还有第三条写坏路径）

### 教训
- "打开时校验"防不了"运行时写坏"——**写回前的变量必须与写盘值同源校验**（本例 compactedSize 先算后判，而不是先 putInt 再读回判断）
- RMS 砖死的三要素齐了：物理 truncate 缺失（模拟器）+ 路径共用（空白名 store）+ 无守卫的减法（上游代码假设存储栈可靠）
- v01.49 守卫只挡 `<=0` 的**块循环**，挡不住 `>0` 的**幻影空闲块**——守卫要覆盖"值合法但内容是垃圾"的情况，只能靠算术不变量（data_size ≥ 0）

### 遗留
- 幻影 2880B 空闲块的精确写入者未定位（0x6D393001 掩码在代码中无来源，疑为 db/idx 同文件互踩的 idx 写入）；开盖自愈使其失去危害性，观察优先
- 报告#2（书签卡顿疑为网页加载）：本次 stderr 全程无 seek 风暴无空转，v01.49 已解决 IO 侧；网页加载卡顿属 UC 自身单线程解释器性能范畴，暂不动

## 2026-09-10 v01.49：UC 书签保存 23 秒无响应 + PC=0x0 崩溃——calculateBlockSize 负块大小无界循环（phoneme-midp 提交 5eea6a1、vita-port 提交 508ae93）

### 用户报告（v01.48 实测）
- UC 浏览器保存书签时界面很卡（约 23 秒无响应），随后退出；再次启动 UC 后 Vita3K 崩溃刷屏（EXCEPTION_ACCESS_VIOLATION PC=0x85f05fd8 → PC=0x0 循环）。
- **用户已先删除 `ux0:/data/J2ME00001/rms/appdb_1A35` 再测**（日志 `Creating new dir rms/appdb_1A35` 佐证）——排除存量脏数据假设，根因在代码路径自身。

### 日志全量定位（vita3k.log，642,731 行二分）
- 10:58:54 新建 rms/appdb_1A35 → 10:59:58 书签写入本身很快（fd 0x11D 写 2863B，~2ms，**保存动作不卡**）。
- 10:59:58 起 fd 0x12E 反复 open→`seek_file 0x80010051` 失败（同 fd write 8B 正常 → fd 是好的，是偏移量错）。
- 11:00:13.6→11:00:36.9 **23 秒无 IO 空窗** = 用户感知的"很卡"；11:00:37 UC 退出。
- 11:01:16 第二次启动 UC，boot 部署阶段 11:01:16.302 首异常（**崩溃发生在第二次启动，非保存时**）。

### 根因机制（本轮定案）
- `RecordStoreUtil.calculateBlockSize`：remainder==0 时返回 `dataSize+8`，否则 `dataSize+(8-remainder)+8`。垃圾 dataSize 为 8 的负倍数（如 -16）→ 返回 -8；dataSize=-8 → 返回 0。
- 块大小 ≤0 → `currentOffset` 不前进/倒退 → `compactRecords`/`getRecordIDs`/`getRecordHeader_SearchFromTo`/`getFreeBlock` 块循环无界空转（23 秒）→ 算出垃圾 offset → seek 失败链 → 进程状态破坏。
- Vita3K 对垃圾/负偏移 `sceIoLseek` 报 0x80010051 且**文件位置保持不变**，后续操作在陈旧偏移上进行，进一步放大损坏。

### 修复（phoneme-midp 5eea6a1 + vita-port 508ae93）
- **Java 4 处守卫**（守卫失败契约与坏 header 读一致）：
  - `RecordStoreImpl.compactRecords`：`currentSize <= 0` → throw IOException
  - `RecordStoreIndex.getRecordIDs`：break
  - `RecordStoreIndex.getRecordHeader_SearchFromTo`：return INVALID_OFFSET
  - `RecordStoreIndex.getFreeBlock`：throw IOException
- **C 层拦截**（`vita-port/src/vita_pcsl.c`）：`pcsl_file_seek` 对 `offset < 0` 记一次 stderr（去重）并返回 -1，借 storagePosition 既有 -1→getLastError 链路上抛到 Java。
- 版本 `CMakeLists.txt` 01.48→01.49。

### ROM 重生成链路（Java 系统类改动必须走全，否则白改）
- phoneme-midp 的 `ROMImage.cpp: $(MIDP_CLASSES_ZIP) $(ROMGEN_CMD) rom.config` 规则（`build/common/makefiles/cldc_vm.gmk:227`）会自动级联：改 .java（比 classes.zip 新）→ 重编该类 → 更新 classes.zip → `romgen -romize`（phoneme-cldc/build/vita_arm/dist/bin/romgen，32 位 x86 宿主工具）→ 重新生成 ROMImage_*.cpp → 重编 ROMImage.o 进 libobj.a。
- **增量构建即可**，无需 rm classes.zip；验证法：`classes/` 下改动类 .class 时间戳 == 本次构建时间 + `grep "RecordStoreImpl.java" alljavalist.txt` 命中。
- `libmidp.so` 链接失败（`_rom_linkcheck_mffd_false`）已知无害，VPK 走 libobj.a。
- vita-port 构建需 JDK：`export PATH=/home/zyb/tools/jdk8u502-b07/bin:$PATH`（build_jar.sh 要 javac；MIDP 构建本身已带）。
- 版本缓存陷阱复现：v01.49 时 `vita_version.h` 仍是旧值，需 `rm build/cmake/vita_version.h` 再 cmake；验证 `strings midp_vita | grep "version: J2ME"`。

### 验证
- MIDP 构建 EXIT=0，ROM 15.43s 重新生成（20,195 对象），RecordStoreIndex/Impl .class 均为本次构建产物，libobj.a/ROMImage.o 15:41 同批。
- VPK：`vita-port/build/cmake/midp_vita.vpk`（14.4MB，15:53），`strings midp_vita` 确认 `v01.49 b207`。
- 待用户实测：删 appdb_1A35 后 UC 保存书签应快速返回（守卫触发时该次保存抛 IOException 而非卡死）；二次启动不再崩溃。

### 遗留（观察项）
- 崩溃 PC=0x85f05fd8 落在堆内 JIT 区与 `-int` 纯解释器矛盾——推测进程级状态污染延续或 LR 巧合；Java 守卫落地后大概率消失，若复现需单独排查。

## 2026-09-09 v01.43：消除残留的「占用」日志三连——unlink 前查注册表跳过注定失败的 remove（vita-port 提交 547a471）

### 用户报告（v01.42 实测）

- "文件占用问题还是存在"。实测 VPK 已是 b192（stderr 横幅 `v01.42 b192 (503cd4b)` + `[pcsl] unlink blocked, file open elsewhere: ux0:/data/J2ME00001/rms/appdb_1A35/FFFFFFFF` 面包屑生效），UC 全程正常（联网、vmStatus=2001 干净退出）。
- 注意：用户附的 vita3k.log 是旧文件（Downloads\windows-latest 遗留副本：17:49→00:02 那次 6 小时会话、Version: 1.28、debug_log.txt 写入全是 b188 特征），不要拿它对照新构建。

### 现状定论

- 唯一残留的"占用" = `rms/appdb_1A35/FFFFFFFF*`（internal suite -1 的活 RMS 数据）被 phoneME 的 suite 清理逻辑 unlink，而 UC 自己的句柄还开着 → Windows/Vita3K 无 FILE_SHARE_DELETE 必失败（Error 32 三行噪音，v01.36 已实锤）。
- v01.37 的"如实报失败"在功能上是对的（数据活着、UC 正常），代价是每轮三行装饰性日志。

### 修复（v01.43）

- `pcsl_file_unlink` 在调 sceIoRemove **之前**先查 `g_open_handles` 注册表（新 `vita_handles_held()`）：本进程有同路径活句柄 → 直接跳过注定失败的 syscall，`vita_report_held` 打一条面包屑后返回 -1。
- 语义与 v01.37 完全一致（调用方拿到同样的失败、文件照样保住），只是 Vita3K 再也看不到这次删除——三行噪音从源头消失。
- 注册表由 open/close 维护，路径是 resolve 后的绝对路径，与 unlink 的 abs_path 同源，不会误判。

### 验证

- v01.43 b194 (547a471) 构建成功。预期：下一轮日志中 `[pcsl] unlink blocked` 面包屑仍在（每路径一条），Vita3K 日志中 `Cannot remove file / Error code: 32 / io_error_impl` 三连消失。
- 若用户仍报"占用"：需要用户提供新的 vita3k.log 确认三连是否消失，以及面包屑出现次数。

## 2026-09-09 v01.42：6 小时长会话 Vita3K 闪退——菜单循环累积内存泄漏（vita-port 提交 503cd4b）

### 用户报告（b188 实测）

- "vita3k 直接闪退了"。时间线：17:49 启动 Vita3K → 安装 VPK → UC **完整正常一轮**（联网 OK、vmStatus=2001 MAIN_EXIT 干净退出、回菜单）→ 用户构建为 04:30 的 b188（v01.41 核心但**无 v01.41a fd 泄漏修复**）→ vita3k.log 尾部停在 [00:02:20.513] input_debug.log 打开（约 6 小时 13 分后），闪退。

### 根因排查（坐实）

1. **`menu_fb` 每轮 2MB 泄漏**（vita_menu.c:1380）：`vita_menu_run` 每次进菜单 `memalign(2MB)` 直接覆盖 static 指针，旧块永不释放。注释声称"每进程 2MB 可接受"——但代码是**每轮**执行，菜单↔游戏切换 N 轮 = N×2MB。修复：`if (menu_fb == NULL)` 才分配，进程生命周期复用。
2. **icon 纹理覆盖泄漏**：`icon_cache_restore_kept()` 把上一轮的解码纹理放回 `icon_pix[]` 槽位后，`scan_games()` 的 cache-hit/miss 两条路径的 `vita_icon_decode_png(&icon_pix[idx],...)` 直接覆盖指针不 free——每游戏每轮漏一张 icon（~16-64KB）。修复：decode 前 free 旧槽位。
3. **次要结论**：b188 无 v01.41a 修复，O_TRUNC 恢复路径的 fd 泄漏（open 成功后 free 未 close）会在多轮后累积孤儿 FILE*——Error 32 与资源消耗的放大器。v01.41a（8a4db28）已修。
4. **debug_log.txt 之谜定论**：用户 b188 日志有 debug_log.txt 写入（open/write/close 每次重开模式）但 git 历史中 b188/b189/b190 的 `VITA_FILE_DEBUG` 都是 0——04:30 构建发生在 1204cac 提交前，构建时工作区临时开着 `=1`，提交时改回 0。**构建产物与提交内容可能不一致，横幅只保证 HEAD，不保证工作区干净**。教训：构建前 `git status` 确认干净。
5. **image_wash 双重前缀疑云排除**：摘要中的 "app0:ux0:/..." 证据实为 09-05 v01.32 时代的老分析残留；b188 的 `pcsl_file_exist` 已正确做前缀交换（本轮日志唯一 Missing file 是 vm_boot.log 探测，正常）。
6. **vita_stubs.c 旧双实现确认不碍事**：`phoneme-midp/src/vita_stubs.c` 是旧 pcsl 实现（无条件 debug_log、path[256]、`app0:%s` 裸拼接），其 .o 在 phoneme-midp 构建树存在但**不在任何链接的 .a 里**（libobj.a/libpcsl_file.a 均不含）；当前 velf 的 pcsl 符号单一定义且为新代码。

### 附带修复

- **build.sh pipefail**：`make | tail -20 || {...}` 的退出码来自 tail——make 失败也打印 "Build successful"（05:16 事故根因）。已加 `set -o pipefail`。

### 验证与遗留

- v01.42 b192 构建成功，版本横幅与 HEAD 一致；待用户长会话复测（菜单↔游戏多轮 + 挂数小时）。
- 闪退若仍复现：向用户要 vita3k.log 被省略段（592-18790 行）的尾部 ~50 行 + 是否在 [00:02] 启动了第二轮游戏。
- 6 小时菜单空转的 Vita3K 侧资源累积（host 侧）仍是候选——若 b192 仍闪退且时间点与游戏轮次无关，转向模拟器本体。

## 2026-09-09 v01.41：I/O 适配层系统性重审——Vita3K O_TRUNC 盲区是占用/EBADFD/慢的共同根因

### 用户反馈（v01.40 实测无效）

- 占用（Error 32）仍在、seek EBADFD 警告仍在——path 600 + truncate 兜底没治本。用户要求"好好分析下 io 是不是没适配好"。

### 系统性对照结论（上游 pcsl posix/win32 vs 我们的 vita_pcsl.c vs Vita3K HLE 源码）

逐函数读完三层后的**决定性发现**：

1. **Vita3K `translate_open_mode()`（io/src/filesystem.cpp）没有 SCE_O_TRUNC 分支**——所有 open 都映射 `"rb+"`（master 与历史提交 345d807488 确认一致，用户 v0.2.1 同样中招）。`O_TRUNC` 被**静默丢弃**。
2. 因此 v01.40 的 truncate（close→reopen(O_TRUNC)→写回）在 Vita3K 上**从未真正截断过**：保存的前缀覆盖了文件头部，**旧尾巴永久残留**；RMS compact 报告成功但垃圾记录头活在新逻辑尺寸之后 → 二启读损坏数据/慢；`pcsl_file_sizeofopenfile` 一直返回旧尺寸。**真机上 O_TRUNC 是生效的——这是模拟器特有的静默数据损坏**，也是 v01.40 日志分析看不见它的原因。
3. Vita3K `close_file` 是同步 `std_files.erase(fd)`（shared_ptr<FILE> 即时 fclose）——排除"句柄延迟释放"假说；Error 32 = remove 时**确有另一个 Vita3K FILE\* 仍开着**（`_wfopen` 无 FILE_SHARE_DELETE，Windows 下进程内任何打开句柄都阻止删除）。我们的 truncate close→reopen 窗口 + `OPEN_READ_WRITE_TRUNCATE` 路径都是制造这种窗口的源头。
4. `FileStats` 自带 `truncate()`（`_chsize_s`）且 io.cpp 有 `truncate_file()`，但**未挂到任何 sceIo 导出**（SceLibKernel 导出表无 sceIoTruncate）——真机也无此 syscall，不能依赖。
5. 上游 win32 pcsl：truncate=`_chsize(fd)`（不动 fd）、close 先 commitwrite——语义参考；Vita 无对应 syscall，只能重建文件。
6. RMS 栈事实（排除项）：`storage_open` 返回 `(int)handle`（指针截断，ARM32 无损，仅日志噪音）；`ENABLE_RECORDSTORE_FILE_LOCK=1` 已启用；Java 侧 `RecordStoreFile.close` 有 `handle=-1` 防护（396-401 行）；`storageCleanup` finalize 依赖该字段防 double-close——Java 层句柄生命周期健康，问题全在 pcsl 语义层。

### v01.41 修复（vita-port 提交 1204cac）

- `pcsl_file_truncate`：close→**remove→create(O_RDWR|O_CREAT)→校验新文件为 0 字节→写回**。校验失败（remove 被共享冲突挡掉）就诚实报错，不再静默损坏。真机/模拟器行为一致（remove+create 在两边都真正清空）。
- `pcsl_file_open`：带 `SCE_O_TRUNC` 打开成功且文件非空时，同样 close→remove→create 仿真（`storage_open` 的 `OPEN_READ_WRITE_TRUNCATE` 路径，suitestore 临时文件复用场景）。
- `pcsl_file_seek/read/write/sizeofopenfile`：fd<0 快速失败 + 一次性 stderr 面包屑（`[pcsl] seek on dead handle '<path>'`）——EBADFD 系统调用噪音在源头掐断，且下次复现能直接定位肇事路径。

### 预期与验证点（等用户实测）

- 二启不再卡数据初始化（RMS compact 真正生效，垃圾尾巴没了）。
- seek EBADFD 警告应消失或变成一条可定位的 stderr 面包屑。
- 占用（Error 32）应大幅减少：truncate 不再有 O_TRUNC reopen 窗口；若仍有，剩下的就是 midp_file_cache flush 时序，届时再查。
- 慢：本地缓存恢复后 UC 应明显变快（不再每次全量重下）。

### 教训（补充进方法论）

1. **模拟器的 syscall 语义要用它的源码验证，不能按 POSIX 直觉假设**——`translate_open_mode` 一共 20 行，读了就绝不会写出依赖 O_TRUNC 的代码。
2. "修了报错但没好"时，下一步不是再修报错，而是**找没报错的静默失败**（O_TRUNC 被丢弃时不报任何错）。
3. 跨平台文件操作的正确性锚点是"**这个序列在两个平台都产生相同的磁盘状态**"，不是"这个调用在 POSIX 语义下正确"。

## 2026-09-09 v01.40：EBADFD(0x80010051) seek 风暴 = 句柄路径截断；真机 0x8010113D 仍开放

### v01.39 实测结论（用户确认）

- ✅ 不再卡死、二启正常启动——调度器忙转修复生效。
- ⚠️ 还是慢；vita3k.log 出现 `seek_file (_sceIoLseek) returned 0x80010051`，UC 无法读本地缓存。
- ❌ 真机安装仍报 0x8010113D（v01.30 的 MEMSIZE 修复没解决真机问题）。

### EBADFD 根因（v01.40 修复）

- 0x80010051 = errno 81 = EBADFD。Vita3K seek_file 只在 fd<0 或 fd 不在打开表时返回它——**phoneME 拿着死 fd 在 seek**。
- 链条：`VitaFileHandle.path[256]` 太短 → UC 深层缓存路径被 strncpy 截断 → RMS compact 走 `pcsl_file_truncate`（close→reopen O_TRUNC）→ reopen 打开**不存在的截断路径**失败 → `vf->fd=-1` 但 handle 仍被 Java 层持有 → 之后每次 seek/read 都是 `sceIoLseek(负fd)`=EBADFD → **UC 本地缓存永久失效，每次启动全走网络**（"太慢"的真因之一）。
- 修复（vita-port 提交 6e9be4b）：path 扩到 600（与 abs_path 同宽）；truncate reopen 失败时用无 O_TRUNC 重开兜底恢复 fd（truncate 本身仍报失败，但句柄不再半死）。

### 真机 0x8010113D 排查记录（未解决，需分流实验）

- 已排除：①eboot.bin 与 `vita-make-fself -s -c`（无 -m）参考输出**逐字节一致**（CMP 验证）；②param.sfo 与 VitaSDK 默认输出一致（ATTRIBUTE=32768 是所有 homebrew 的 mksfoex 默认值，非异常）；③VPK 结构完整（livearea/icon/template 齐全，13.8MiB 压缩）。
- 待做的 A/B 实验（按成本排序）：
  1. 用 VitaSDK 官方 samples 的 hello_world.vpk 装真机——若也失败，是**真机环境问题**（Hen 版本/存储/安装方式），与本 VPK 无关；
  2. 若 hello_world 能装：做一个"hello 壳+我们的 eboot.bin"混装 VPK 定位是 eboot 还是资源问题；
  3. 检查真机 Hen（h-encore 版本对 fself -c 的兼容性）与安装途径（VitaShell 复制 ux0:/vpk 后安装 vs FTP 直装）。
- 教训：模拟器能装 ≠ 真机能装；但**先验证对照样本**再怀疑自己，避免又是 MEMSIZE 式的误判。

## 2026-09-08 v01.39：调度器忙转根因——os_port.cpp 超时条件抄反（ms == 0 应为 ms != 0）

### 症状与诊断路径

- UC 症状链：一启正常（能开 m.baidu.com）→ 退出卡住 → 强关窗口 → 二启卡在数据初始化、fps=1、极卡。
- 决定性证据组合：
  - `runmidlet_debug.log`：**2 次 ENTRY、0 次 RETURNED**——`midp_run_midlet_with_args_cp` 从未返回，VM 轮次没正常走完；
  - `input_debug.log` 二启会话：心跳 #1→#8193（约 840 万次事件泵调用）无触摸——**VM 活着但全速空转**；
  - `midp_stderr.log`：二启网络全通（连到 m.baidu.com/ms.bdstatic.com），说明 UC 引导走完，卡在初始化阶段。

### 根因（确定 bug，非猜测）

- `phoneme-cldc/src/anilib/vita/os_port.cpp` 的 `Os_WaitForEventOrTimeout`：上游 `anilib/linux/os_port.cpp` 是 `if (ms != 0)`（基于 gettimeofday 算绝对超时），**Vita 移植时抄成 `if (ms == 0)`**——`ms > 0` 时 timespec 恒为 `{0,0}`（1970-01-01）→ `pthread_cond_timedwait` 立即 ETIMEDOUT。
- 后果链：`JVMSPI_CheckEvents`（vita_checkevents.c）把 ANI 等待 clamp 到 50ms → `ANI_WaitForThreadUnblocking` → `PoolThread_WaitForFinishOrTimeout(…, 50)` → 立即返回 → 调度器**永不真正睡眠**，50ms 等待变成 0ms 忙循环 → CPU 烧满、fps=1、心跳计数狂奔、UC 初始化被拖死。基线提交 98b12b5 就带此 bug（git show 确认）。

### 修复（v01.39）

- `os_port.cpp`：`if (ms == 0)` → `if (ms != 0)`（+注释说明来历），phoneme-cldc 提交 5956a99。
- 重编：`cd build/vita_arm/target/release && make CLDCBUILD=fp ENABLE_ENABLING_CHECK=false os_port.o`（用体系自身的 release 配方，`arm-vita-eabi-g++`，注意 build/vita_arm/dist/lib 下的 `g++` 同为交叉编译器符号链接）。
- 重打包：在 `dist/lib` 内 `arm-vita-eabi-ar r libcldc_vm_ani.a <新os_port.o>`、同法 `_r` 变体（vita-port 链接的是 `libcldc_vm_ani`/`_anix`）。
- 验证：velf `.text` 内 `Os_WaitForEventOrTimeout` 指令流为 `bl gettimeofday; orrs r3,r6,r8; streq…; beq`——与库内修复版逐指令一致。VPK=build/cmake/midp_vita.vpk（APP_VER 01.39）。

### 教训

1. **移植平台代码时逐字符对照上游**：一个 `==`/`!=` 抄反在 pthread 超时路径上不报错、不崩，只表现为"性能差/卡顿"，极难归因。凡从上游复制的函数，落地前 diff 一遍原版。
2. **心跳计数是免费的性能仪表**：input_debug 心跳每 1024 次泵一条，速率异常（百万级 vs 预期 ~2万/分钟）直接暴露忙转。
3. 待观察：退出卡死（一启退出挂住）是否同根因（忙转下 ANI/pthread 竞态）或另有独立原因——v01.39 实测后若退出仍卡，下一嫌疑是 SVM 模式 MIDletDestroyTimer 失效（上游注释明说 timer 在 SVM 不工作）与 EventQueue.shutdown 等待链。

## 2026-09-08 v01.38：UC 数据初始化慢的 I/O 放大器（debug_log）+ 网络修复实测确认

### 网络线关闭（实测证据）

- 用户 v01.37 实测 net_log：`gethostbyname` 全部秒回、`connect immediate OK fd=15/16`（连 uc.ucweb.com/ucus.ucweb.com 均直连成功）——**v01.35 的 g_handles 修复实测生效**，select 唤醒链正常（连接走的是 immediate OK，未触发 EINPROGRESS 也无妨）。网络层不再有已知问题。

### UC「卡在数据初始化」的 I/O 放大器（v01.38 修复）

- `pcsl_file_open` 每次被调都 `debug_log()`：**open+write+close 三个 syscall 写 debug_log.txt，成功路径写两行 = 6 个额外 syscall/次**。UC 初始化要开几百个文件（RMS 恢复/配置/皮肤），I/O 放大约 3 倍；Vita3K 还为每个 syscall 打一行 trace 日志，双重放大。
- 修复：`VITA_FILE_DEBUG` 宏开关（默认 0 关闭），所有 debug_log 调用点进 `#if`。
- 附带：`pcsl_file_unlink` 在"ux0 文件存在但被锁"时不再 fallback 删 app0 副本（必然再失败，占用日志翻倍的来源）。

### 「还是报占用」的定性与现状

- v01.37 起"占用报错"= 预期行为：共享冲突在保护 UC 活存档不被 suite 清理循环删掉（见 v01.36/v01.37 章节）。
- v01.38 后噪音减半（不再有成对的 app0 fallback 失败）。剩余：每个受保护文件一组三行（Vita3K 打的），**无害、无法从应用侧消除**（除非模拟器修 FILE_SHARE_DELETE）。

### 教训

1. **调试日志本身会成为性能 bug**：per-call 的 open/write/close 型日志在高频路径（file_open）上等于把 I/O 放大数倍——发布前必须用编译期开关关闭，不是靠"日志文件小"判断影响。
2. 修复网络/文件层后要主动找"实测证据"关闭问题线（本次 net_log 的 immediate OK），避免已修问题继续占用排查注意力。

## 2026-09-08 v01.37：撤销 v01.36 句柄驱逐——共享冲突一直在「意外保护」UC 存档（重要教训）

### 症状（用户 v01.36 实测）

- UC 第一次启动正常，第二次不行（NPE 回归）+ 严重卡顿 fps≈1。
- 时间线对照：v01.35 时代用户只抱怨"占用日志还在"，**从未抱怨二启失败**；v01.36（驱逐）后立即出现。v01.36 相对 v01.35 的唯一行为差异就是驱逐。

### 根因：删除成功才是错的

- `midp_remove_suite` 清理循环删除的目标 `appdb_1A35/FFFFFFFF*` **是正在运行的临时 suite（UC）的活 RMS 数据**。
- 11:28 日志时序实锤：`remove FFFFFFFF 失败(Error 32) → 紧接着又 open fd 0x171 同一路径`——删除失败后马上有代码重新打开它，说明它是活数据。
- **v01.36 之前**：Windows 共享冲突（Error 32）让这些删除**一直失败**——这个"烦人的占用报错"其实一直在**意外保护** UC 存档跨启动存活。
- **v01.36**：驱逐持有句柄（fd→-1）+ 重试 → 删除成功 → 但 Java 层还持着这些 fd 继续写 → 全部落空 → RMS 文件半写损坏 → 二启读坏数据 NPE + 写失败重试风暴烧 CPU → fps≈1。
- **比全删更糟**：全删（用户手动删 appdb）反而等价"首次启动"，能进；半坏数据直接崩。

### 修复（v01.37，已提交）

- `pcsl_file_unlink` 撤销驱逐：真占用时**如实返回失败**，让文件活下来。三行 Vita3K 日志（Cannot remove/Error 32/io_error_impl）是模拟器打的装饰性噪音，接受它。
- 句柄注册表与 `vita_handles_evict`（标 `__attribute__((unused))`）保留，供将来"真卸载时才驱逐"的精细化修复。
- **用户须知**：v01.36 跑过的 `rms/appdb_1A35` 可能已被写坏，装 v01.37 后先手动删一次该目录再测。

### 教训（最高优先级）

1. **"修好报错"前先问：这个失败在保护什么？** 报错 ≠ bug。删除失败的背后可能是数据的生命周期还没结束。尤其清理循环 vs 活数据的竞争。
2. **修改删除/关闭语义的补丁，必须验证"数据跨启动存活"场景**（跑两次 + 检查数据文件完整性），只看单次运行日志不够。
3. 平台语义差异（POSIX unlink-open-file vs Windows）不能机械补偿——补偿方向错了一样是 bug。上游依赖的语义在这台"机器"上恰好被模拟器缺陷满足时，先搞清楚谁依赖它再动手。

## 2026-09-08 v01.36：「FFFFFFFF 被占用」真相 = Windows 共享冲突，v01.35 误判纠正（对照 Vita3K 源码逐行验证）

### v01.35 为什么没修好（误判复盘）

- v01.35 从 `sceIoRemove returned 0x80010002` 推出"ENOENT=文件不存在，Vita3K 映射错误"——**被 Vita3K 骗了**。
- **实锤（拉取用户实际版本 Vita3K v0.2.1 496939b6 的 `io.cpp` 源码对照）**：`remove_file()` 里 `fs::detail::remove()` 失败时，`LOG_ERROR("Error code: {} ({})", error_code.value(), error_code.message())` 打印的是 **Windows API 原话** —— `Error code: 32` = `ERROR_SHARING_VIOLATION`，**这次是真实占用**，不是误报；然后 Vita3K 对"不存在"和"被占用"**一律硬编码返回** `IO_ERROR(SCE_ERROR_ERRNO_ENOENT)` = `0x80010002`。返回码无区分度，v01.35 的"ENOENT=不存在"推理在此场景失效。
- 另一实锤：`open_file()` 的 `FileStats` 用 `create_shared_file`（`_wfopen`，**无 FILE_SHARE_DELETE**）持有 `FILE*`——只要进程内任何 fd 开着该文件，Windows `fs::remove` 必 Error 32。**POSIX 允许 unlink 已打开文件**（phoneME 清理循环依赖此语义），Windows 不允许——平台语义差异是根因。
- 日志三行（`Cannot remove file` / `Error code: 32` / `io_error_impl returned 0x80010002`）**全部是 Vita3K 打的**，应用侧改返回值处理消不掉，必须让 sceIoRemove 真正成功。

### 修复（vita_pcsl.c）：句柄注册表 + 删除前驱逐

- `g_open_handles[64]`：`pcsl_file_open` 成功时注册、`pcsl_file_close` 注销（`vita_handle_register/unregister`）。
- `pcsl_file_unlink` 新流程：remove 失败 → `sceIoGetstat` 仍存在（=真占用，区别于不存在）→ `vita_handles_evict(abs_path)` 把**同路径**的所有句柄 `sceIoClose`+`fd=-1`（模拟 POSIX"名字先消失，fd 随后失效"）→ 重试 remove。"已不存在=成功"逻辑保留。
- `pcsl_file_truncate` 内部 close→reopen 同一 `vf`（注册表指针/路径不变），无需改动。
- 验证：编译通过，APP_VER=01.36，VPK 11:16。

### 教训（重要）

1. **模拟器的错误返回码可能无区分度**：Vita3K 把 ENOENT 和共享冲突都折成 `0x80010002`，判断真实失败原因**不能只看返回码**，要对照模拟器源码的 HLE 实现（这次是 `io.cpp remove_file` 的 `error_code.message()` 帟暴露了 Windows 真错）。
2. **跨平台语义差异要在移植层补偿**：POSIX "unlink open file" vs Windows 共享冲突——上游代码天然依赖 POSIX 语义时，移植层要么提供句柄注册表驱逐，要么保证删除前句柄全关。
3. **教训的教训**：v01.35 的推理链（返回码→ENOENT→"文件不存在"）单看自洽，但没有对照模拟器源码验证就下了结论——**涉及模拟器行为时，先拉对应版本源码核对，再下结论**。

## 2026-09-08 v01.35：破除「FFFFFFFF 被占用」误报 + UC 联网失败根因（g_handles 表未初始化）

### 一、`remove_file appdb_1A35/FFFFFFFF "文件被占用"` ~~是 Vita3K 的误报~~（v01.36 纠正：一半误判，见上章节——ENOENT 部分确是误吞，但 Error 32 部分是真实共享冲突，v01.35 的"纯误报"结论不成立）

- **真错误码**：`io_error_impl` 明确打印 `remove_file (sceIoRemove) returned 0x80010002`。SCE I/O 错误编码 = `0x80010000 + errno`，`0x80010002 - 0x80010000 = 0x2 = ENOENT`（对照新lib `sys/errno.h: ENOENT 2`）。**是"文件不存在"，不是"共享冲突"**。
- **误报机制**：Vita3K 打日志时把 SCE errno 映射到 **Windows errno 表**，错把 `ENOENT(2)` 显示成 `Error code: 32 (另一个程序正在使用此文件)` —— 纯映射错误。用户看到的"被占用"是假的。
- **谁触发**：`midp_remove_suite` 清理循环（`suitestore_task_manager.c:452`）对枚举出的**每个**条目无条件 `storage_delete_file` → `pcsl_file_unlink` → `sceIoRemove`。当目标已不存在（裸 `FFFFFFFF` 只是 suiteId 前缀，真实文件是 `FFFFFFFF<name>.db`；或已被前序删除步骤删净），`sceIoRemove` 返回 ENOENT → 走 `io_error_impl` 打印。**与文件是否被删无关，删不删文件夹都会报**（因为删的是一个本来就不存在的条目）。
- **修复（vita_pcsl.c）**：`pcsl_file_unlink` 在 `sceIoRemove` 失败后，用 `sceIoGetstat` 探测——若路径已不存在则视为"删除已达成"，返回成功。不硬编码错误码，真机/Vita3K 通吃。附带收益：清理循环不再因 ENOENT 提前 `break`，后续文件能删干净。
- **结论**：这条日志是 Vita3K 特有害噪音，真机不会有；修复后归零。

### 二、UC 联网失败根因：`g_handles` 表未初始化，所有非阻塞 connect 静默失败

- **症状**：net_log.txt 中连真实服务器 `uc.ucweb.com`(120.241.3.205)/`u.ucfly.com`(219.133.46.180) 只有 `socket_open`，**无任何 connect 结果**；只有不可达的移动 WAP 代理 `10.0.0.172` 打印 `connect FAILED errno=116`（ETIMEDOUT，UC 内置 CMWAP 回退，当前网络必超时，正常）。
- **根因**：`vita_net.c` 的 `static NetHandle g_handles[VITA_NET_MAX_FDS]` 是 **BSS 全局，默认全 0**。`na_create()` 靠 `g_handles[i].fd == -1` 找空槽——初始全是 0，**永远匹配不到，永远返回 NULL**。于是所有走 `EINPROGRESS` 的非阻塞 connect（NBIO 由 v01.33 的 `vita_set_nbio` 正确设置）都在 `h == NULL` 处静默 `close(fd); return PCSL_NET_IOERROR`——**没有日志**。这就解释了两类主机的日志差异：可达主机走 EINPROGRESS→na_create NULL→静默失败；不可达代理 connect 直接返回 ETIMEDOUT→打 FAILED（在 na_create 前）。
- **修复（vita_net.c）**：`g_handles` 显式初始化为全部 `{ -1 }`（32 个）。na_create 从此能正确注册 fd，select 表生效，EINPROGRESS→select→NETWORK_*_SIGNAL 唤醒链恢复。
- **教训**：依赖"表里某个哨兵值"的空槽查找，表必须显式初始化；BSS 零值（fd=0）恰好是非法 fd，但不是哨兵 `-1`，会使"找空槽"逻辑永久失败。此类 bug 无编译告警、无运行日志（NULL 路径常被上层吞掉），只能靠审查 + 实测日志追。

### 验证

- 两修复均编译通过，APP_VER=01.35，VPK `/home/zyb/vitasdk/samples/j2me/vita-port/build/cmake/midp_vita.vpk`（08:52）。
- 提交：见 `git -C vita-port log`。

## 2026-09-08 v01.34：目录枚举 stub 是 UC 二次启动 NPE 根因（重要教训：stub 的连锁失效模式）

### 症状与误导性线索

- 用户实测：UC 首次能进（v01.33 修复生效），**第二次启动 `java.lang.NullPointerException: 0 at ao.a(), bci=4`**，删除 `rms/appdb_1A35` 后又能进一次。
- 日志里 `remove rms/appdb_1A35/FFFFFFFF` Error 32（文件被占用）一度是头号嫌疑——**不是根因**（那是 suite 关闭时的良性清理尝试），真根因是枚举 stub。

### 真根因：pcsl_file_openfilelist 是 NULL stub

- 调用链（全部源码核实）：`RecordStore.listRecordStores()` → `RecordStoreFile.listRecordStores` (jpp) → native `getNumberOfStores` → `rmsdb_get_number_of_record_stores` → `rmsdb_get_number_of_record_stores_int` → `storage_open_file_iterator` → `pcsl_file_openfilelist` → **NULL** → 返回 `OUT_OF_MEM_LEN` → JNI 层 `KNI_ThrowNew(midpOutOfMemoryError)` → UC 启动状态恢复中断 → `ao.a()` NPE。
- **首启能过的原因**：RMS 创建走 `pcsl_file_open`/`pcsl_file_exist`（真实实现）；只有"列出已有 store"需要枚举。删目录后列表合法为空 → 绕过失败路径 → "又能进一次"。这就是"删 appdb 恢复"症状的机制。
- 受害面不止 UC：`midp_remove_suite` 的文件清理循环、`rmsdb_remove_record_stores_for_suite`、`RecordStore.listRecordStores` 全走此枚举。

### 修复（vita_pcsl.c，+158 行）

- `pcsl_file_openfilelist/getnextentry/closefilelist` 用 `sceIoDopen/Dread/Dclose` 实现，**严格按上游 POSIX 参考契约**（`pcsl/file/posix/pcsl_posix.c` + `pcsl/file/util/pcsl_util_filelist.c`）：
  - 输入串语义 = `root目录 + match前缀`；按最后一个分隔符拆分（`pcsl_string_last_index_of`，jchar 单位）
  - 每次 getnextentry 返回下一个前缀匹配的目录项，结果 = `string[0..rootLength) + entry名` 完整路径（caller free）
  - 跳过 `.`/`..`；目录本体经 `vita_resolve_path` 解析（相对路径防御）
- RMS 文件布局（顺带查实）：suite 存储是 **sRoot 下扁平文件** `root + <suiteId8位hex> + <后缀>`；后缀集合 `.ss/.ii/.ap/.db/.idx/.jar/.ssr/.tmp` + `_suites.dat/_trans.dat/_icons.dat`（`suitestore_constants.xml`）。`FFFFFFFF` = `INTERNAL_SUITE_ID(-1)`，临时 suite（从 jar 直接启动的 UC）的 RMS 基名 = `getSecureFilenameBase(-1)` = `build_suite_filename(-1, EMPTY)`。
- 验证：ELF 三符号 T；`getnextentry` 内 `sceIoDread` 调用确认；APP_VER=01.34。提交 79f5dbc。

### 教训（重要）

1. **stub 的失效模式是"条件性成功"**：创建路径真实、枚举路径 stub → 首启成功、二启失败——极易误判为"持久化数据损坏"。以后遇到"首启 OK 二启挂"先查该功能的**完整 API 面**（创建/读/写/枚举/删除）是否都有实现，而不是先怀疑数据损坏。
2. 上游契约不能凭直觉写：`pcsl_string_last_index_of(const pcsl_string*, jint)` 签名与 POSIX 版 `PCSLStorageDirInfo`（rootLength jchar 单位含分隔符）都从参考实现核对后才动手。
3. 日志里 Error 32/0x80010002 的 remove 失败在 suite 生命周期里大量出现，多数良性——**判断主因要看调用时序与是否有后续读坏数据**，单条 remove 失败不构成根因。

### 遗留风险

- `pcsl_file_getfreespace/getusedspace` 仍返回 0 的风险已在补全提交（b98c165）消除：
  - `getusedspace`：sceIoDopen/Dread 单层遍历 storage root 求和（POSIX 参考语义，不含子目录）
  - `getfreespace`：`sceAppMgrGetDevInfo("ux0:")` 真实空闲字节（long 32 位饱和 LONG_MAX）
  - **关键联动**：`internal.config`(+landscape) 增加 `system.jam_space: 100000000`——`storage_get_free_space = totalSpace - used`，而 `totalSpace` 默认 4MB（`DEFAULT_TOTAL_SPACE`），不设此属性时 RMS 数据超 4MB 会误报 `RecordStoreFullException`。属性单位字节（上游 linux_fb 参照 1000000），读取点在 `midpInit.c:253` `getInternalProperty("system.jam_space")`。
- RMS 的 `.idx` 索引文件（tree_index/linear_index）路径未实测。
- 真机未测；Vita3K 的 sceIoDread 对大目录行为需观察。

## 2026-09-08 v01.33：fcntl NBIO 对 sceNet fd 无效（connect 冻结 VM 27s）+ vm_output 竖排修复

### 用户 v01.32 实测反馈 → 三个现象，一真两良性

1. **connect 卡死 27 秒**（真 bug）：net_log 显示 `socket_open ip=120.241.3.205 port=80` 后无 connect 结果，Vita3K 日志 `sceNetConnect` 从 11:28:32 阻塞到 11:28:59 才返回 `0x8041013C`（= `SCE_NET_ERROR_ETIMEDOUT`），vita_net.c 日志 `errno=116`（= `ETIMEDOUT`）。
2. **vm_output.log 一字符一行**（真 bug）：CLDC 的 `PrintStream.write(int)`（`write(int b) → byteOut.write(b)`）**逐字符**调 `JVMSPI_PrintRaw(s,1)`；v01.28 的逐行钩子每次 `fprintf("%.*s\n")` 强加换行 → 竖排。
3. `sceNetInit 0x80410110`（EBUSY）+ `sceNetResolverCreate` stub 警告（良性）：第二轮启动 newlib `gethostbyname` 内部 lazy 再 init 网络栈；`10.0.0.172` 是 UC 内置 CMWAP 移动代理回退（直连失败才走），当前网络环境必超时，非我方 bug。
4. **v01.32 修复确认生效**：suite 落在 `rms/appdb_1A35/FFFFFFFF`（per-game appdb）；`[MIDLET] created+registered OK: cn.uc.application.app.WebClient`（此前崩溃点已过）。`remove_file FFFFFFFF` 失败是 suite 关闭清理，良性（2026-09-07 已有结论）。

### 根因：fcntl(O_NONBLOCK) 对 sceNet 描述符是 no-op

- 二进制级验证链：newlib `socket()`@810f8e98 → `sceNetSocket`（fd 直通，错误码经 `__vita_scenet_errno_to_errno` 转换）；newlib `select`@810f15f8 → `sceNetEpollCreate/Control/Wait/Destroy`（select 对 net fd 有效）；但 `fcntl` 只作用于文件描述符标志，sceNet fd 的非阻塞模式是**socket 选项**：`sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO=0x1100, &on, 4)`（net.h:388）。
- 后果：socket 一直是阻塞模式，`connect()` 阻塞在 syscall 里，VM 事件泵线程全冻结（`checkForSystemSignal`/`vita_net_poll` 也跑不了），27s 后 ETIMEDOUT。此前 v01.30 时代"网络能用"是因为当时网络调用全 stub 直接失败返回，从没真正 connect 过。
- 修复：`vita_net.c` 新增 `vita_set_nbio(fd)`，在 TCP client（`pcsl_socket_open_start`）、server（`pcsl_server_socket_open_start`）、UDP（`pcsl_datagram_open_start`）创建时统一调用；`vita_socket_available()` 的 MSG_PEEK 依赖 socket 已 NBIO（删掉原来的临时 fcntl 翻转）。
- 恢复的设计链：NBIO connect → `EINPROGRESS` → `PCSL_NET_WOULDBLOCK` → Java 线程 `midp_thread_wait(NETWORK_WRITE_SIGNAL)` → `vita_net_poll()` select 就绪 → `midp_thread_signal` 唤醒 → `getsockopt(SO_ERROR)` 完成握手。

### 修复：JVMSPI_PrintRaw 原样透传（phoneme-midp cf018c2）

- `midp_run.c`：`fprintf("%.*s\n")` → `fwrite(s,1,length)`；换行由 Java println 自带。逐次 fopen/fclose 防强杀丢日志方案不变。最终 ELF 验证 `fopen→fwrite→fclose`。

### 踩坑（新增）

- **build.sh 的 make 失败会被吞**：`make ... 2>&1 | tail -20 || {...}` 里管道退出码是 tail 的（成功），`set -e` 拦不住——脚本继续打出成功横幅但 VPK 是旧的（本次 APP_VER 仍 01.32、VPK 时间戳早于 midp 重编才发现）。判断构建成败要 `ls -la --time-style` 对时间戳 + 查 APP_VER，或手动 `make`。本次 datagram 段漏改的 fcntl 就是手动 make 才暴露的。
- errno/错误码速查：newlib errno 116=ETIMEDOUT、119=EINPROGRESS、120=EALREADY、11=EAGAIN/EWOULDBLOCK；sceNet 0x8041013C=ETIMEDOUT、0x80410110=EBUSY（`psp2/net/net.h`）。

### 验证与产物

- `midp_run.o`：`JVMSPI_PrintRaw` 内 `bl fopen/fwrite/fclose`（无 fprintf 格式串）。
- 最终 ELF：`pcsl_socket_open_start` 序列 `socket(8100766c) → sceNetSetsockopt(810076c0) → connect(810076e0)`；全 ELF 6 处 sceNetSetsockopt 调用。
- VPK：APP_VER=01.33，`build/cmake/midp_vita.vpk`（04:20:15）。
- 提交：phoneme-midp cf018c2（PrintRaw）+ samples c5722f0（vita_net.c NBIO + CMakeLists v01.33 注释）。

### 遗留风险

- 直连 `120.241.3.205:80`（uc.ucweb.com）本身超时与否取决于网络环境（该 IP 是 UC 老服务器的电信出口）；修好 NBIO 后若仍连不上，属服务器/路由问题，换网络验证。CMWAP 代理 10.0.0.172 在当前环境必不通（UC 无设置页时可能反复重试）。
- datagram/available 路径的 NBIO 改动未实测（无 UDP MIDlet）；MSG_PEEK 假设 socket 已 NBIO，若后续有 UDP MIDlet 异常优先查这里。
- 若 UC 在连接阶段表现"卡住但没死"，属预期：EINPROGRESS + select 唤醒链首次真正运转，观察 net_log 是否出现 `connect DONE`/`connect EINPROGRESS` 行。

## 2026-09-08 v01.32：UC 卡初始化真根因（皮肤图 app0 fallback 失效）+ appdb 覆盖修复 + rms/ 目录归整

### 三个用户反馈 → 两个根因 + 一个修复

1. **"UC 还是卡住初始化"** → 直接根因：30 张皮肤 PNG 全部 `FAILED (0x80010002)`（ENOENT）。chameleon 皮肤资源池初始化带缺图进行，LCDUI 初始化链路挂起。位置在 `games/app/game.jar` 与 `appdb/FFFFFFFF` 打开日志之后。
2. **"很多 png 资源没有"** → **资源其实都在**：VPK 内 `data/J2ME00001/lib/` 有全部 178 张 PNG（python3 zipfile 解包验证，`screen.image_wash`/`scroll.*`/`softbtn.*`/`ticker.*`/`alert.*` 一个不缺）。打不开是 fallback bug（见下）。
3. **"为什么数据都访问 appdb/FFFFFFFF，appdb_XXXX 却全空"** → `FFFFFFFF` = `INTERNAL_SUITE_ID(-1)` 的 8 位 hex（`GET_SUITE_ID_LEN=8`，`midp_suiteid2pcsl_string`），suite 存储路径 = `sRoot + suiteId`。`sRoot` 永远是共享 appdb，因为 per-game 设置被上游覆盖（见下）。

### 根因 A：pcsl app0 fallback 拼接非法路径（皮肤图打不开）

- `vita_pcsl.c` 的 `pcsl_file_open/unlink/exist/sizeof` 在 ux0 打开失败后 fallback：`snprintf(app0_path, "app0:%s", path_utf8)`——用**原始输入路径**。输入是绝对路径 `ux0:/data/...` 时产生 `app0:ux0:/...`（恒 ENOENT）；该命中 VPK 的场景只有皮肤图（`lib/<name>.png`，绝对路径），于是全军覆没。
- 路径链：`ResourceHandler.getSystemImageResource` → `getAmsResource(name+".raw"/".png")` → `File.getStorageRoot(INTERNAL_STORAGE_ID)` = `sRoot`（= appdb dir = `ux0:/data/J2ME00001`）→ `lib/...` 由 skin 代码拼上。VPK 内布局是 `data/J2ME00001/lib/*.png`，所以正确 fallback 是把 `ux0:/data/` 前缀换成 `app0:/`。
- 修复：四处 fallback 统一改为基于 `vita_resolve_path` 产物的前缀替换：`strncmp(abs,"ux0:/data/",10)==0 → "app0:/"+(abs+5)`（注意 +5 跳过 `ux0:`，无重复斜杠）。字节级验证：`ux0:/data/` @810ffff4、`app0:/%s` @81100000。
- ROM 化皮肤机制说明：`lfj_load_image_from_rom`（SkinRomizationTool 生成的 178 项查找表，在 `lfj_image_rom.o`）**已生效**，但 Java 侧部分资源（`SkinResourcesImpl` 之外的 `ResourceHandler` 路径）仍走文件系统，两条路都要通。

### 根因 B：runMidlet 无条件覆盖 midpSetAppDir（appdb_XXXX 全空）

- 覆盖链：`vita_main.c` 启动器每轮 `midpSetAppDir(per-game appdb_<TAG>)` → `runMidlet()`（`phoneme-midp/src/ams/example/jams/native/runMidlet.c:134-141`）**无条件** `midpSetAppDir(getApplicationDir(...))` → `commandLineUtil_md.c` 的 `getApplicationDir` 从 `MIDP_HOME` 重建 `MIDP_HOME + "/appdb"`（APPDB_DIR 硬编码）→ sRoot 永远 = 共享 appdb。
- `storageInitialize(config_home, app_dir)` 在 `midpInit(LIST_LEVEL)` 里用 `midpAppDir` 填 `sRoot[0]`；`midp_suite_exists(INTERNAL_SUITE_ID)` 直接返回 OK 不查 `_suites.dat`，所以**空的 per-game appdb 目录可直接用**，无需种子拷贝。
- 修复（phoneme-midp 0767127，3 文件 +29/-8）：新增 `midpGetAppDir()`（midpInit.c + midpAMS.h）；runMidlet 仅当 `midpGetAppDir()==NULL` 才回退 `getApplicationDir`。USE_NATIVE_APP_MANAGER=false，nams 的同类调用点不在链接里。
- 附带修 UB：`midpSetAppDir` 只存指针，vita_main 传的是栈缓冲 `char appdb_path[80]` → 出块悬空。改 `static char appdb_path[96]`。

### 目录归整（用户诉求：数据文件夹收进子目录）

- suite 存储统一移入 `rms/` 子目录：`ux0:/data/J2ME00001/rms/appdb`（共享/Hello）+ `rms/appdb_<TAG>`（per-game）。`get_per_game_appdb` 输出带 `rms/` 前缀。
- 迁移：启动时 `sceIoDopen(DATA_DIR "/appdb")` 存在则 `sceIoRename` → `rms/appdb`（一次性，老存档保留）。旧 `appdb_XXXX` 空目录不迁移（无数据），会残留在原处，可手动删。
- 目录布局变为：`J2ME00001/{lib/, games/, inbox/, rms/, cache 等}`。

### 验证与产物

- `bash build_vita.sh`（phoneme-midp）exit=0：`libobj.a` 已含 `midpGetAppDir` T 定义 + runMidlet.o U 引用。**注意**：构建尾部 `libmidp.so` 链接失败（`_rom_linkcheck_mffd_false` 未定义）是**预先存在**的中间产物问题——`CREATE_MIDP_SHARED_LIB=true` 的检查性链接需要 `vm_rom_stubs.c`（在 vita-port 侧），不影响 `libobj.a` 生成与最终链接。
- `bash build.sh`（vita-port）exit=0；`midpGetAppDir` T @8103d95c 在最终 ELF。
- VPK：178 张 lib PNG 在包内；param.sfo APP_VER=01.32。
- 提交：phoneme-midp 0767127 + samples 145a382（CMakeLists 版本注释含完整因果链）。

### 遗留风险

- UC 实测若仍卡：下一个埋点是 `SkinResourcesImpl_ifLoadAllResources0`（AMS isolate 分支，`ENABLE_MULTIPLE_ISOLATES=false` 时返回 FALSE 走 lazy load）与 chameleon `ResourceHandler` 的 `/raw` 路径；另需看 `vm_output.log`/`midp_stderr.log`。
- `libmidp.so` 链接噪音每次 `build_vita.sh` 都会报 Error 1（exit 码却为 0 的原因是脚本最后一步 cp/chmod 成功）——判断成败要看 `libobj.a` 时间戳，不要只看脚本退出码。
- 若用户嫌弃旧 `appdb_XXXX` 空目录残留：下次可加"检测 rms/ 存在则清理旧 appdb_*"。

## 2026-09-08 v01.31：LCDUI 初始化崩溃修复（UC 进不去死因）+ 菜单扫描缓存

### 崩溃根因（Vita3K EXCEPTION_ACCESS_VIOLATION，UC 等 MIDlet 启动即死）

- 现象：Vita3K 崩溃 `EXCEPTION_ACCESS_VIOLATION 0xC0000005`，PC=0x810346c6，"Invalid read at 0x0"；net_log.txt 只有 3 行 init 成功日志（网络调用从未发生）。
- 归因（二进制级）：`arm-vita-eabi-addr2line -e midp_vita -f -C 0x810346c6` → `DisplayDeviceContainer.getDisplayDevicesIds0`。反汇编确认：native 侧拿 `lfjport_get_display_device_ids` 返回的 ids 指针（r5）→ `SNI_NewArray` → 循环 `ldr r2,[r5,#4]!` 解引用 NULL。
- 根因：`vita-port/src/vita_display.c` 的 `lfjport_get_display_device_ids` 违反契约——`*n=1` 但 `return NULL`。正确契约见上游 `phoneme-midp/src/highlevelui/fb_application/reference/native/fbapp_export.c:398`：返回 `static jint display_device_ids[] = {0}`。
- Java 链路：`DisplayDeviceContainer` 构造（LCDUI 初始化即触发）→ `getDisplayDevicesIds0()` → 循环 `ids.length` 读元素 → 死。HTTP 任何调用之前。
- 修复：vita_display.c 返回静态数组 `{0}`；反汇编验证新实现 `str 1->*n; ldr r0,[pc,#4]`（返回 0x813bf4b8 非 NULL）。提交 3c30ee8。
- 为何此前游戏能跑：待查（可能 v01.30 前该函数路径不同，或游戏走不同初始化顺序）；不影响本修复正确性。

### 次要理论排查（closed，不成立）

- `DisplayEventListener` type=54 未知 id NPE 理论：ROM 化 midp_system.jar 里**没有任何类调用 `MMEventHandler.setListener`**（JSR135 Player 实现 BasicPlayer 不在构建里，`USE_JSR_135` 实际 false）；MMAPI 事件即使到达 Java 队列也因无注册 listener 被丢弃。vita_media_notify.c 的 push 侧字段布局与上游 javacall 桥（midp_msgQueue_md.c:197）一致，无害。

### 菜单扫描缓存（用户诉求：首次启动慢，是否逐一解析 jar → 是）

- 原状：`scan_games()` 每游戏开 jar 4-5 次（MANIFEST x3 + 中央目录 class 验证 + icon PNG）+ PNG 解码，每游戏几百 ms，每次启动/重扫重复。
- 方案：每游戏目录 `cache.bin`（magic 'J2CB' v1），键 = jar size + mtime 双字（`sceRtcGetTick`）。命中：不读 jar，name/cls/icon 从缓存取，game.cfg 只刷新人工 class 覆盖与 landscape；icon 用缓存里的 PNG 原始字节解码（免 jar IO）。不匹配：全量重解析 + 重写缓存。
- rescan 纹理保留：`icon_cache_keep/restore_kept` 让未变游戏已解码纹理跨扫描存活（SELECT 重扫零解码）。
- 踩坑记录：①`g->dir "/" CACHE_NAME` 字符串拼接不能直接进 `sceIoOpen` 参数（要 snprintf）；②`sceRtcGetTick(const SceDateTime*, SceRtcTick*)` 出参型，不是返回 tick 值；③multi_replace 衔接处又吞了一次 `icon_load` 函数签名行（第二次发生，**多段替换后必须编译**）；④git commit -m 中文长消息会被终端回显污染，用 `git commit -F <file>`。
- 提交：27d56b8（cache.bin）+ 3c30ee8（崩溃修复）+ 55c2a17（APP_VER 01.31）。

### 验证状态

- 增量构建 EXIT=0，midp_vita.vpk（APP_VER=01.31）已重新打包，待用户 Vita3K/真机装新 VPK 实测 UC。
- 若 UC 仍进不去：net_log.txt 现在有全链路埋点，可定位网络阶段卡点。

## 2026-09-07 v01.30 修订 2：真机安装 0x8010113D + 网络日志全链路埋点

### 真机 0x8010113D（安装失败）
- **根因**：`vita_create_self(... MEMSIZE 128)`——fself `-m` 是 **KiB
  内存预算**，Normal app 合法值仅 0（0x1000-0x12800 仅 System mode），
  128(0x80) 写入 SELF control info 后真机 SceShellSync 拒绝安装；
  Vita3K 不校验该字段所以模拟器一直没事。128 来自初始 commit 288337c
  （作者可能误以为单位是 MB）。
- **修复**：CMakeLists 去掉 MEMSIZE（留注释说明）；字节级验证 VPK 内
  eboot.bin 与 `vita-make-fself -s -c`（无 -m）输出完全一致。
- **教训**：`vita.cmake` 的 MEMSIZE 直接透传 fself -m，语义是 KiB 预算
  而非 MB；模拟器宽松真机严格，**首次真机部署前应 diff 一次 fself
  参数**。

### 网络日志全链路（vita_net.c）
- 之前 `vita_net.c` 一行日志都没有 → "网络不可用但看不到 log"。
- 新增 `vnet_log()`：stderr（midp_stderr.log）+ `ux0:/data/net_log.txt`
  追加式双写（stderr 每次启动被截断，net_log 跨启动留存；每行
  fopen/fclose 防强杀丢日志，同 vm_output.log 方案）。
- 埋点覆盖：early_init 三步返回码、getLocalIP、gethostbyname 成败、
  socket open（ip/port）、connect 即时/异步/失败+errno、SO_ERROR、
  recv/send 失败+errno、EOF。任一环失败都能定位。

### 其它
- sceNetInit 内存池 64KB → **1MB**（DNS resolver 也从池分配；官方
  net_http_bsd 示例用 1MB，64KB 极易饿死 DNS）。
- net_poll 从 vita_input.c 移到 vita_checkevents.c（JVMSPI_CheckEvents
  step 0，MIDP 泵之前）：输入泵不应知道网络存在；调用频率/时机不变。
- `multi_replace` 一次吞掉了 `VITA_NET_MAX_FDS` 定义（替换串衔接处
  消费未补回）→ 编译报 undeclared 才发现。多段替换后必须编译验证。
- build.sh 依旧吞 make 错误码照报 successful（v01.28 已知坑，再次
  实锤：本次 Error 2 被包成 "Build successful"）。

## 2026-09-07 v01.30 网络层恢复：UC 浏览器进不去 → socket 全 stub 所致

### 现象与根因
用户反馈 UC 浏览器一直进不去。排查确认：**当前树的网络是纯 stub**
（`vita_pcsl.c` 的 `pcsl_socket_open_start` 直接 return -1），任何
`Connector.open("http://...")` 在 TCP 层即失败 → UC 启动即挂/退出。
8-30 曾有真实现 `vita_net.c`（~845 行，BSD socket 全量），但该版从未
单独 commit，8-30 全面回退时被 git clean 掉，之后所有版本网络均为
stub。

### 恢复来源（git 考古）
`vita_net.c` 找回自 **dangling commit d93fc0a3**（8-30 "save change"，
`git fsck --lost-found` 发现）。内容：pcsl socket/server-socket/
network/datagram 全量真实现——newlib BSD socket 包装（内部 sceNet）、
非阻塞 connect（EINPROGRESS→WOULDBLOCK）、gethostbyname（getaddrinfo）、
`vita_net_early_init()`（sceSysmoduleLoadModule(NET)+sceNetInit+sceNetCtlInit）、
`vita_net_poll()`（select 全 fd 集→NETWORK_READ/WRITE/EXCEPTION_SIGNAL
唤醒阻塞协议线程）。HTTP 是纯 Java（com.sun.midp.io.j2me.http）跑在
socket:// 上，TCP 通即 HTTP 通。

### 改动（4 文件 +16/-311 + 新增 845 行）
1. `src/vita_net.c`（恢复）：全部 pcsl 网络函数真实现。
2. `src/vita_pcsl.c`：整段删除网络 stub（原 1008-1317 行，37 函数 +
   htons/htonl 辅助——这些在 net.c 有同名真实现，**必须删否则重复
   定义**）。
3. `src/vita_main.c`：main 里 `vita_net_early_init()`（VM 启动前）。
4. `src/vita_input.c`：checkForSystemSignal 里 `vita_net_poll()`
   （input/touch poll 之后、media 之前）。
5. `CMakeLists.txt`：+`src/vita_net.c`、+`SceNet_stub SceNetCtl_stub`
   （SceSysmodule_stub 原已有）。

### 经验
- **功能合并回退时必须先把每个功能的文件单独 commit**——8-30 的教训
  重演：vita_net.c 只存在于 patch/dangling commit 里，恢复靠 git fsck
  考古。此后每完成一个功能立即 commit。
- pcsl 网络函数在 stub 与真实现间切换时，htons/htonl/getRawIpNumber
  这类小辅助也是重复定义点，别只看 socket_* 大函数。
- dangling commit 是救命稻草：`git fsck --lost-found` 找回未提交工作。

### 验证
- 构建成功；`nm midp_vita`：`vita_net_early_init/vita_net_poll/
  pcsl_socket_open_start/pcsl_network_gethostbyname_start` 全部 T 符号
  唯一（无重复定义）。
- 产物 `vita-port/build/cmake/midp_vita.vpk`（16:13）。
- 待用户实测：UC 浏览器联网、NetTest MIDlet（HttpConnection GET
  example.com）。**注意**：DNS（getaddrinfo）与 sceNetCtlInit 在
  Vita3K 的支持度未知——若 UC 仍进不去，看 midp_stderr.log 的
  `[net]`/errno 定位是 DNS 还是连接阶段。

## 2026-09-07 v01.29 修复第 2 轮：zip 大小写比较单向折叠 + 触摸坐标系 2 倍（用户日志实锤）

### 用户日志（midp_stderr.log）关键证据
- `口袋灵兽`：`entry[2] len=6 '40.png' match=0` + `[icon] zip entry
  not found: '40.png' (from '/40.png')` —— 条目就在 jar 里（len 精确
  相等）却失配 → 排除前导 '/' 单一原因，暴露比较 bug。
- 其余 jar：`'icon.png' match=0`、`'resource/icon.png' match=0` 全灭；
  唯一能匹配的是全大写 `META-INF/MANIFEST.MF` —— 模式指向单向折叠。

### 根因 2 个（都是确定性 bug）
1. **`zip_read_entry` 的"大小写不敏感"只把条目名转大写，`want` 没转**：
   条目 `'p'→'P'` ≠ want `'p'` → 所有小写 want 失配。修复：两边都折叠
   （vita_menu.c，比较循环加 b 折叠）。
2. **前触摸板 report 坐标是 1920x1088 空间，不是 960x544**：SDK 官方
   touch 示例用 `report[0].y >= 1000` 做判定即为实锤。原代码按
   960x544 边界判定 → 屏幕右半/下半触摸全被当 letterbox 丢弃，左上
   区域坐标也错 2 倍。修复：`vita_touch_init` 用 `sceTouchGetPanelInfo`
   读 maxAaX/maxAaY（默认 1919/1087），采样处归一化到 960x544 再进
   `vita_display_map_touch`（vita_input.c）。
   前轮的 SetSamplingState(START) 修复仍然必要（两个 bug 叠加）。

### 经验
- **带调试日志发布是正确的**：本轮两个根因都是用户回报 midp_stderr.log
  一次定位（match=0 + len 相等 = 比较逻辑 bug；y>=1000 = 2x 坐标系）。
- **zip 大小写折叠必须双向**；写这类比较时用 "want 里没有大写字母"
  的用例自测（小写 want + 混合大小写条目）。
- **SceTouch report 空间 ≠ 显示空间**：真机 1920x1088 vs 960x544。
  永远用 sceTouchGetPanelInfo 的 active area 归一化，别硬编码。
- 官方 samples/（如 touch/src/main.c）是坐标系/API 行为的权威参考。

### 验证
- `./build.sh` 成功（rm .obj 后重建 + make 显式检查退出）。
- zlib 解压 self 段确认 `[TOUCH] init: state=%d set=%d max=(%d,%d)`
  新格式串在产物内。产物 `vita-port/build/cmake/midp_vita.vpk`（15:55）。
- 待用户实测：图标应能显示（40.png/icon.png 类）；触摸全屏区域命中、
  坐标正确（input_debug.log 的 down INSIDE 行 virt 坐标应在 0..239/
  0..319 内且与手指位置成比例）。

## 2026-09-07 v01.29 修复：触摸无效（sceTouch 采样未启动）+ 图标不显示（前导 '/'）

### 用户实测反馈
中文显示 OK；触摸无效果；游戏列表不显示应用图标。

### 根因（两个都是确定性 bug，非模拟器限制）
1. **触摸：`vita_touch_init()` 从未调用 `sceTouchSetSamplingState(START)`**。
   Vita 前触摸面板上电默认 STOP，`sceTouchPeek` 返回成功但 `reportNum`
   恒为 0 → 永远走不进事件分支 → 静默"不支持"。与 MIDP 事件链无关
   （按键走同一链路正常即为反证）。
2. **图标：MIDlet-1 图标路径常带前导 '/'（如 "/icon.png"）**，而 zip
   条目名无前导 '/'，`zip_read_entry` 按 `name_len==strlen(want)` 精确
   长度匹配必然失败 → icon_pix 全 NULL → `draw_icon_scaled` 静默 no-op。

### 改动（仅 2 文件）
1. `src/vita_input.c`：
   - `vita_touch_init()`：先 `sceTouchGetSamplingState`，非 START 则
     `sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
     SCE_TOUCH_SAMPLING_STATE_START)`。**注意枚举是 STOP/START，没有
     "OFF"**（首版编译错即此）。
   - `vita_touch_poll()` 加状态迁移日志（仅状态变化时写，不刷屏）：
     init 结果 / peek FAILED / up / down OUTSIDE(物理坐标) /
     down INSIDE(物理+虚拟坐标) → `input_debug.log`。用于区分
     "事件没产生" vs "被 letterbox 丢弃" vs "Java 层丢弃"。
2. `src/vita_menu.c` `icon_load()`：查 zip 前 strip 前导 '/'；失败时
   fprintf(stderr)（`[icon] zip entry not found` / `[icon] png decode
   FAILED`，菜单阶段 stderr 已 freopen → 进 midp_stderr.log）。

### 经验
- **sceTouch 必须 SetSamplingState(START)**——这是 Vita SDK 隐式要求，
  不调用则 Peek 永远空数据，且无错误返回。
- zip 条目查找一律 strip 前导 '/'（JAD/manifest 资源路径书写惯例 vs
  zip 命名规则的经典差异）。
- build.sh 会吞 make 错误码照报 "Build successful"——验证必须看 make
  输出或检查产物符号/字符串。
- VPK 的 eboot.bin/.self 中 ELF 段被 zlib 压缩，strings 直接 grep 查
  不到新代码；需先 zlib 解压段再查（见验证方法）。

### 验证
- `./build.sh` 成功；python zlib 解压 midp_vita.self 的 0x1000 段确认
  `[TOUCH] init`、`[icon] zip entry` 字符串在内（产物确为新代码）。
- 产物：`vita-port/build/cmake/midp_vita.vpk`（12:30）
- 待真机/Vita3K 验证：触摸日志链（init state=1）、图标行显示。

## 2026-09-07 v01.29 触摸启用 + 菜单中文 + 游戏列表真实名称/图标（三个新功能）

### 需求
1. 游戏列表显示 jar 内真实名称（MIDlet-Name）与图标（MIDlet-1 第 2 字段）
2. J2ME 支持中文显示、触摸功能

### 关键调研结论（改动前先读，避免误判）
- **触摸链路本来就已通**：vita_input.c 的 `vita_touch_poll()` 早已采样
  `sceTouchPeek(SCE_TOUCH_PORT_FRONT)` 并发 `MIDP_PEN_EVENT`（ACTION=
  KEYMAP_STATE_PRESSED/DRAGGED/RELEASED，X_POS/Y_POS 为虚拟屏幕坐标，
  经 `vita_display_map_touch` 从物理 960x544 反算）。事件分发链
  `DisplayEventListener` PEN_EVENT → `Display.handlePointerEvent` →
  Canvas `pointerPressed/Released/Dragged` **不检查 pen_supported**；
  `Constants.POINTER_SUPPORTED` 在 generated/classes 里已是 true。
  唯一卡点是 `lfjport_is_display_pen_supported/motion_supported` 返回
  KNI_FALSE（只影响 `DisplayDevice.hasPointerEvents()` 查询 API）。
- **Java 层中文渲染本来就已通**：vita_font.c 的 gxjport_draw_chars 走
  J2FB v1 字库（fontbitmap.bin，GB2312 全集 + 假名/希腊/全角），Java
  的 drawString 一直能画中文。**只有原生菜单**（vita_menu.c draw_text
  5x7 ASCII）不支持：idx>126 画 '?'。
- **游戏列表名称原来用目录名**（`strncpy(g->name, ent.d_name)`），
  图标从未解析。

### 改动（全部在 vita-port，最小 diff，新增文件优先）
1. `src/vita_display.c`：pen_supported / pen_motion_supported 改返回
   KNI_TRUE（各 1 行 + 注释）。触摸至此完全启用。
2. `src/vita_font.c`（文件尾追加 ~100 行，不改既有代码）：新增
   `vita_menu_draw_utf8()` / `vita_menu_font_gw/gh()`——菜单侧 UTF-8
   解码 + fb_glyph blit 到 32bpp framebuffer（0xAABBGGRR），缺字画
   tofu box。复用 fb_ensure/fb_glyph，无重复字库代码。
3. `src/vita_menu.c`：
   - `draw_text()` 开头探测多字节：含 >=0x80 字节则改走
     vita_menu_draw_utf8（字库加载失败时画 '?' 占位，行仍可见）；
     纯 ASCII 走原 5x7 快速路径（零回归）。
   - 新增 `manifest_get()`（大小写不敏感 + manifest 续行规则）、
     `parse_manifest_name()`（MIDlet-Name，回退目录名）、
     `parse_manifest_icon()`（MIDlet-1 第 2 个逗号字段）。
   - **注意调用顺序**：`g->jar` 必须先 snprintf 再 parse_manifest_*，
     否则 jar 路径为空解析必败（曾踩，自查 diff 时发现修正）。
   - GameEntry 加 `icon[128]` 字段；scan_games 时 `icon_load()` 解码
     并缓存（ICON_CACHE_MAX=MAX_GAMES，菜单退出时 icon_cache_clear）。
   - 列表行/对话框画图标：`draw_icon_scaled()` 按框适配（等比最近邻
     采样，含 alpha 混合；首版整数 scale 溢出 bug 已修）。
4. `src/vita_icon.c/.h`（新增）：libpng 解码（VitaSDK 自带
   arm-vita-eabi/lib/libpng16.a），内存 reader + setjmp 错误路径，
   归一化到 8bpp RGBA（png_set_palette_to_rgb/strip_16/gray_to_rgb/
   filler）。RGBA 字节序按小端读即 0xAABBGGRR，无需重排。
5. `CMakeLists.txt`：源文件加 vita_icon.c；链接 `png`（必须在 `z`
   之前，pngwrite 的 deflateEnd 需要 z——顺序反了会 undefined
   reference）；版本 01.28 → 01.29。

### 验证
- `./build.sh` 成功；`arm-vita-eabi-nm build/cmake/midp_vita | grep
  vita_icon_decode_png|vita_menu_draw_utf8` 确认符号进 ELF。
- 产物：`vita-port/build/cmake/midp_vita.vpk`
- 待真机/Vita3K 验证：中文游戏名列表显示、图标显示、游戏内触摸。

### 遗留风险
- MIDlet 图标常为 16x16~32x32 小 PNG，最近邻放大到 22/44px 框会有
  颗粒感（可接受，未做平滑）。
- 菜单 draw_text 的多字节路径 1:1 blit（20x22px），与 ASCII 3x 缩放
  视觉尺寸相近但混排时基线略有差异（列表场景几乎不混排）。
- JVM 内 GBK 编码的 MANIFEST（少数老 jar）：MIDlet-Name 会以 GBK 字节
  被 UTF-8 解码画成乱码/tofu。多数国产 jar 为 UTF-8，暂不做 GBK 转换。
- pen_supported 改 true 后，`Display.hasPointerEvents()` 为 true，
  某些游戏会切到触摸 UI 模式（这正是需求，但个别游戏按钮布局可能
  依赖触摸坐标校准，需真机观察）。

## 2026-09-07 v01.28 回归修复：堆回退 + 日志加固 + watchdog 体系移除（重要！）

### 背景：v01.27 回归
- 用户实测 v01.27：打开 UC 没画面、强制退出后 Vita3K 卡死、日志全空
- v01.27 把堆从 32MB→48MB、_newlib 64MB→96MB（为 UC 二次启动 OOM 扩容），疑似回归根因

### 关键实证（推翻 v01.27 归因，本段最大发现）
1. **watchdog 从未进过任何产品二进制**：
   - `vita_interpreter_heartbeat`/`vita_interpreter_dump`/`vita_vm_booted_flag`/`vita_vm_stopping_cleanly` 的**定义全在 `Interpreter_c.cpp`**（romgen 宿主工具专用，`ENABLE_C_INTERPRETER` 仅 IsRomGen=true 分支设 true）
   - ARM target 的 `_MergedSrc*.incl` 不含 Interpreter_c.cpp（v01.28 前），好库（出厂代 v0109_hybrid_pre_gcfix）与 midp 侧 libobj.a（173 成员）全量扫描**零命中**
   - 出厂 v0110 VPK（09-05 03:37）velf strings 无 `cldc_watchdog` → **v01.27"强退→Vita3K 卡死"与 watchdog exit(-3) 无关**，归因作废
2. **`Main_vita.cpp` 的 tty 镜像不会生效**：产品里 `JVMSPI_PrintRaw` 由 **midp 侧 `midp_run.o`**（`midp_run.c:491`）提供，cldc 库不带；`Main_vita.o` 恰在 rebuild_vm.sh 的 EXCLUDE 列表 → 镜像必须落到 `midp_run.c`
3. **`JVM.cpp` 的 `vita_vm_clean_shutdown` 是无定义 extern 调用点**：900dd85 加进 JVM::stop()，但定义在 romgen-only 的 Interpreter_c.cpp → 好库 005 已含 `U vita_vm_clean_shutdown`，靠 `shim_clean_shutdown.o` 成员兜底解析（v01.27 能链接成功的原因）

### 决策：回退 watchdog 体系（它在 ARM 面无定义点，从未生效也无从生效）
- 若按原计划把带 watchdog 引用的新 OS_vita.o 打进库：心跳恒 0 + booted 无置位点 → 死代码线程，一旦 booted 被置 true 就变真杀手
- 回退范围（phoneme-cldc，全部回到基线 98b12b5）：
  - `JVM.cpp`：`git checkout 98b12b5 --`（移除 clean_shutdown 调用点）
  - `OS_vita.cpp`：删除 watchdog 线程/start/宏/initialize 调用（95 行）
  - `Main_vita.cpp`：`git checkout 98b12b5 --`（PrintRaw 镜像迁移到 midp_run.c）
  - `Interpreter_c.cpp`：剥离 v01.28 加的 `vita_vm_booted_flag` 定义+置位（保留 900dd85+d206ff5 的心跳/面包屑/窗口验证，romgen-only 无害）

### v01.28 保留/新增改动
- `vita_pcsl.c`：堆 48MB→**32MB**（回退，回归根因候选）
- `vita_main.c:54`：_newlib 96MB→**64MB**（回退，与堆同步）
- `JVM_vita.cpp`（+49 行）：新增 `vita_boot_log()`（fopen("vm_boot.log","a")+vfprintf+fclose，**逐行钩子式**，幸存 Vita3K 强杀）；JVM_Start/JVM_Start2 打 classpath/main/argc、set_arguments done、每次 JVM::start() loop enter/returned
- `midp_run.c`（midp 侧）：`JVMSPI_PrintRaw` 改为**逐行 fopen/fclose 钩子式**写 `ux0:/data/vm_output.log`（原常开 FILE* 宿主侧缓冲强杀全丢，8-31 空日志事故）
- `vita_menu.c`：free(menu_fb) 改为保留分配（防 VM 堆复用后扫频花屏/"没画面"）
- `CMakeLists.txt`：VITA_VERSION 01.27→**01.28**

### 构建修复（本段第二个大坑）
- **陈旧 incl 雷**：`target/generated/incls/__MergedSrc006.cpp.incl`（09-03 03:28）比 loopgen 版多一行 `#include ".../vm/cpu/c/Interpreter_c.cpp"`（09-03/09-04 cinterp 实验遗留）→ 删除该行（恢复 34 成员）
- **Skeleton 泄漏雷**：repack 把宿主工具 stub `InterpreterSkeleton.o`/`OopMapsSkeleton.o`（空 `interpreter_dispatch_table()` 等）打进库 → 守卫拦下 → 加入 rebuild_vm.sh EXCLUDE
- **教训**：永远不要删除 `_MergedSrc*.o`（会踩 09-03 遗留坏 incl）；target 编译真实 incl 在 `target/generated` 不在 `loopgen/generated`；库成员抽取验证（ar p|nm）是发现"守卫之外静默陈旧"的唯一手段

### 验证
- `rebuild_vm.sh`：33 对象编译 OK，守卫双检通过（C 解释器符号 0、jvm_fast_globals 单 D），库 29 成员
- `build.sh`（需 `export PATH=/home/zyb/tools/jdk8u502-b07/bin:$PATH` 供 javac）：全绿，产物 `midp_vita_v0128.vpk`（v01.28 b166）
- velf 验证：`vita_boot_log` T 已解析；`vm_boot.log`/`ux0:/data/vm_output.log` 字符串在；**无** cldc_watchdog/vita_vm_booted/vita_interpreter_heartbeat/vita_vm_clean_shutdown
- 待用户 Vita3K 实测：①UC 首启是否恢复（堆回退）②强退是否还卡死 ③`ux0:/data/J2ME00001/vm_boot.log` 给出精确 breadcrumb

### 遗留风险
- 堆回退到 32MB 后 UC 二次启动 OOM 可能复发（v01.27 扩容的原始动机）——若 32MB 首启正常但二次 OOM，再二分 48MB
- 若实测仍"没画面"，优先看 vm_boot.log 的 [JVM_Start] 行与 vm_output.log 的 [tty] 行定位卡点

## 2026-09-07 用户实测确认 + `appdb/FFFFFFFF` 删除失败警告调查（良性，无需处理）

### 用户实测结果（v01.28）
- **UC 能正常进入了**（堆回退 32MB 生效，v01.27 回归修复成功）
- 启动时有一条 `remove_file: Cannot remove file: ux0:/data/J2ME00001/appdb/FFFFFFFF` 警告（`Error code: 32` 文件被占用），不影响运行

### `appdb/FFFFFFFF` 删除失败调查结论（良性，无需处理）
- `FFFFFFFF` = `INTERNAL_SUITE_ID`（-1，`suitestore_common.h:65`），`appdb/FFFFFFFF` 是 MIDP 内部 suite 的存储目录
- **MIDP 侧不可能删除它**（证据链闭合）：
  1. `midp_remove_suite`（`suitestore_task_manager.c:324`）对内部 suite 有双重保护：`get_suite_data` 找不到内部 suite（`suitestore_intern.c:327` 只匹配 `COMPONENT_REGULAR/PREINSTALLED`）→ 返回 NOT_FOUND 提前 break；即便走到 `remove_from_suite_list_and_save`（`suitestore_intern.c:1341`）也直接 `return 0` 跳过
  2. `midp_run.c`/`midpInit.c`/`main.c` 均无删除 `appdb/FFFFFFFF` 的调用
  3. "Cannot remove file"/"Error code: 32" 字符串**不在 phoneme-midp 源码**（`pcsl_file_unlink` 只返回负 Vita 错误码，不打印该文案）
- **结论**：该警告来自 **Vita3K 模拟器自身**（app 启动时同步/清理 ux0 文件系统，遇到被占用文件打印），与 MIDP/VM 代码无关。`Error 32`=文件被占用（可能是模拟器对 `appdb/FFFFFFFF` 目录内句柄未释放），删除失败后 MIDP 继续运行，UC 正常进入 → **纯良性，无需处理**
- 若日后想消除噪音：可在 `main.c` 启动时对 `appdb/FFFFFFFF` 目录做一次 `sceIoRemove` 清理（但需确认模拟器句柄已释放，否则仍失败）——当前不建议，属低优先级

## 2026-09-06 启动器循环化（游戏退出回菜单）+ UC 二次启动 OOM 堆扩容（v01.27）

### 需求与方案
- 用户需求：①游戏（MIDlet）退出后回到游戏菜单，整个 VPK 进程不退出；②UC 浏览器第二次启动报 `java.lang.OutOfMemoryError`
- 方案：`vita_main.c` main() 重构为 `for(;;)` 菜单循环——每轮 `vita_menu_run` 选游戏 → 播种 config/appdb/heap → `runMidlet`（VM 完整 start/cleanup 一轮）→ 回菜单。**进程永不退出**，同时规避 Vita3K 的 `sceKernelExitProcess → request_process_exit → on_game_closed` GUI 竞态崩溃（此前实锤的模拟器闪退链）

### phoneME 同进程 VM 重启的可行性依据（全源码确认）
- `JVM_Initialize` 注释"OK to call more than once"；`runMidlet.c` 每次调用走 `midpInitialize → midp_run_midlet_with_args_cp → midpFinalize` 完整配对
- `Universe::apocalypse()`：dispose Scheduler/Thread、flush jar caches、memset persistent handles、重置全部 bootstrap 状态标志、`ObjectHeap::dispose` free 堆 chunk（Vita 的 chunk 走 jvm_malloc=malloc，真释放）
- `midpInit.c` 注释明确"midpInitialize should be called again ... such as running MIDP in a loop"
- **pcsl 内存池再初始化安全**（压缩时遗留的最后疑点，已闭环）：vita_arm 的 `pcsl_memory_impl.h` 把 `pcsl_mem_initialize_impl(x,y)` 定义为 `(0)`、`pcsl_mem_finalize_impl` 同为 no-op 宏——分配直通 `pcsl_malloc_port`=malloc。即** vita 配置根本没有 pcsl 私有池**，midpFinalize→pcsl_mem_finalize 后再 initialize 是空操作，无泄漏无双重初始化风险

### 关键改动（3 文件）
1. `vita-port/src/vita_main.c`（核心）：
   - 循环外（一次性）：pte_osInit、mkdir/freopen/chdir、Hello.jar 拷贝、**ANI_Initialize + javacall_media_initialize**（后者每调一次创建一个常驻 tone 线程且无幂等守卫——放循环内会每轮泄漏一个线程，实测源码确认）
   - 循环内（每轮）：默认值重置 → vita_menu_run → 方向+internal.config/system.config/midp_system.jar 播种（midpFinalize 会拆属性存储，必须每轮重拷）→ setenv MIDP_HOME + midpSetConfigDir → per-game appdb（midpSetAppDir）→ setHeapParameters（Arguments::finalize 清配置，每轮重设）→ runMidlet → 回菜单
   - 移除原 `fclose(g_log); return status`——g_log 保持打开持续追加，多轮日志连续
2. `vita-port/src/vita_pcsl.c`：`JAVA_HEAP_SIZE` 32MB→**48MB**（`getInternalPropertyInt`；UC 首次能跑 32MB 但二次启动 OOM，循环模式下还需容纳 VM 重启路径）
3. `vita-port/src/vita_main.c:51`：`_newlib_heap_size_user` 64MB→**96MB**（预算：MEMSIZE 128MB 分区，48MB Java 堆 + native/VM/ROM 对象/帧缓冲 < 96MB；若仍 OOM 可再升）
4. `CMakeLists.txt`：VITA_VERSION 01.26→**01.27**

### 验证
- `bash build.sh` 全绿，产物 `vita-port/build/cmake/midp_vita.vpk`（v01.27）
- 反汇编实锤两处改动编入：`getInternalPropertyInt` 内 `mov.w r0, #50331648 ; 0x3000000`（48MB）；strings 见 `runMidlet returned %d - back to menu`
- 待用户 Vita3K 实测：①游戏退出→回菜单→再进游戏 ②UC 重进是否仍 OOM

### 遗留风险（已知未处理）
- **watchdog**：`OS_vita.cpp:326` 解释器心跳超时会 `sceKernelExitProcess(-3)`——循环模式下会杀整个启动器（连带触发 Vita3K 退出竞态）。若实测中出现"回菜单前闪退"，优先查这里
- **ROM 二次 bootstrap**：`ROM::dispose()` 后 persistent handles 重填路径未经实测（apocalypse 有 memset+标志重置，理论安全）
- **runMidlet 内部 appdb 覆盖疑点**：`runMidlet.c:134` 内部 `midpSetAppDir(getApplicationDir())` 可能用 MIDP_HOME/appdb 覆盖我们的 per-game 路径——但历史日志显示 appdb_1A35 生效，需实测 RMS 隔离确认

## 2026-09-05 音乐开关卡死第三轮根因：ENABLE_JSR_135 未生效的旧 .o（重要！）

### 根因链（反汇编实锤）
- v01.08/v01.09/v01.10 三轮"音乐开关卡死"的最终根因：`midp_master_mode_events.o`
  是 **08-28 03:12** 编译的——当时构建配置还是 `USE_JSR_135=false`。
- 09-01 提交 9240db2 启用 `USE_JSR_135=true`（jsr135 subsystem.gmk 会注入
  `-DENABLE_JSR_135=1`），但 **GNU make 不跟踪 CFLAGS 变化**，旧 `.o` 原样打入 libobj.a。
- 结果：`midp_check_events` 的 switch 里 `#if ENABLE_JSR_135 case MEDIA_EVENT_SIGNAL:`
  分支被编译剔除，跳转表第 36 项（signal-1 索引）指向 default 丢弃分支。
- 完整死锁链：音乐开关 → tone Player 播完 → `javanotify_on_media_notification(END_OF_MEDIA)`
  → vita_media_notify.c SPSC ring → `vita_media_poll` 取出 → `checkForSystemSignal` 设
  `waitingFor=MEDIA_EVENT_SIGNAL` → **midp_check_events switch 丢弃** → Java
  EventDispatcher 永远收不到 END_OF_MEDIA → 卡死。
- 验证方法（以后排查同类问题的模板）：
  `arm-vita-eabi-objdump -d midp_master_mode_events.o`，数 `StoreMIDPEventInVmThread`
  调用点（旧 .o 只有 1 处=UI_SIGNAL；新 .o 2 处），读 switch 跳转表
  `ldrls pc, [pc, r3, lsl #2]` 的第 36 表项是否指向 memcpy→StoreMIDPEvent 分支。

### 修复
1. `rm build/vita_arm/obj/arm/midp_master_mode_events.o` 后 `bash build_vita.sh` 重编
   ——同类问题还有 `midpServices.o`（`ENABLE_JAVA_DEBUGGER` 的 `midp_isDebuggerActive`
   同样缺失，链接报 undefined 才暴露），一并删除重编了 midpEvents/midpEventUtil/midpInit/
   midpMidletSuiteUtils/heap/lfj_cskin。
   **教训：构建配置里的条件编译宏变更后，必须删所有受影响 .o 强制重编，make 不会替你做。**
2. libmidp.so 链接仍失败（`_rom_linkcheck_mffd_false` 未定义 + `_in_gc_state` 重复定义，
   混合 VM 库与 .so 链接方式不兼容）——**不影响我们**：vita-port 只消费 `obj/arm/libobj.a`，
   该归档已含修复后的 .o（已用 ar tv 验证成员时间戳）。
3. **教训：make 单对象重编要先 rm 旧 .o** 这条旧规则同样适用于"配置变更"场景，且
   `USE_*` 布尔开关的变更影响面要用 `grep -rln "ENABLE_XXX" src/ --include="*.c"` 排查。

### 2026-09-05 romgen 修复（同日连环事故）
- `build/vita_arm/dist/bin/romgen` 被写坏成全零文件（09-04 05:00 事故残留），
  08-31 时代的 `romgen/generated/ROMImage.cpp` 也被某次 romize 输出覆盖成 include 壳
  （1509 字节，丢失 ROMSkeleton 的 `romgen_check_oopmaps`/`_rom_constant_pool` 定义）。
- 修复步骤（可复用）：`export PATH=/usr/bin:/bin:$PATH`（关键！PATH 里的 vitasdk
  gcc/as 是 ARM 交叉工具，会污染宿主构建）→ `export JVMWorkSpace=$CLDC` →
  `cd build/vita_arm/romgen && cp src/vm/share/ROM/ROMSkeleton.cpp generated/ROMImage.cpp`
  → `cd app && rm -f ROMImage.o && make`。产物为 32 位 i386 ELF（pie），与 32 位 ARM
  目标指针宽度一致，romize 的 slice 算法才不会断言（64 位 romgen 在
  ObjectHeap.cpp:3511 断言崩溃——指针宽度不兼容，不可用 64 位版替代）。
- 注意：`romgen/app/` 里曾混入 ARM 对象（Interpreter_arm_arm.o/ani.o/os_port.o/
  poolthread.o/_MergedSrc006.o 旧代），手工 `g++ -m32 *.o` 重链会失败，正确方式是
  走 jvm.make 的正规 `$(ROM_GENERATOR)` 规则（它会重编缺失 TU 并按 Obj_Files 链接）。

## 2026-08-30 缺口分析与 vita-port 重建（重要！）

### 功能缺口分析结论（当天完成，详见对话记录）
游戏三链路断裂的根因全部定位：
- **渲染链唯一断点**: `libobj.a` 的 `lfjport_fb_export.o` 里 `lfjport_refresh()` 是空 TODO——
  整条 gxj 渲染管线画进 `gxj_system_screen_buffer` 后无人上屏。
  链路: Graphics native → gxj_system_screen_buffer → DisplayDevice.refresh0 → lcdlf_refresh → lfjport_refresh(空)。
- **输入链唯一断点**: `libobj.a` 的 `mastermode_export.o` 里 `checkForSystemSignal()` 是空函数——
  VM 周期回调 `midp_check_events()` 轮询到它什么都不做。
  正规链路: checkForSystemSignal 填 MIDP_KEY_EVENT+UI_SIGNAL → midpStoreEventAndSignalForeground → Java EventQueue。
  键码用 `keymap_input.h` 的 KEYMAP_KEY_* 值（'0'-'9'、UP=-1..SELECT=-5、SOFT1/2=-6/-7、GAMEA-D=-13..-16）。
- **旧自研代码是死代码**: `phoneme-midp/src/porting/vita/*` 的 KNI 名与 Java 侧 native 声明不匹配，
  且双 framebuffer（vita_fb vs gxj_system_screen_buffer）从未汇合。已放弃，不再演进。
- 其余缺口: 无音频（JSR-135 关+无 SceAudio 代码）、仅 5x7 ASCII 字体、网络全 stub、
  internal.config 的 display 尺寸配置（240x320 虚拟屏需同步三处：lfjport、config、gxj buffer）。

### vita-port/（新的权威移植层目录）
**所有新代码只写 `samples/j2me/vita-port/`，phoneme 两树零改动**。构建方式：链接前
`ar d libobj_no_main.a lfjport_fb_export.o mastermode_export.o runMidlet_md.o`，
再链入本目录实现（`--allow-multiple-definition` 下先到的 .o 赢）。
- `src/vita_display.c`: lfjport 22 符号全覆盖；240x320 虚拟屏 → 960x544 最近邻缩放居中 blit（RGB565→A8B8G8R8）
- `src/vita_input.c`: 真 checkForSystemSignal + 16 槽 SPSC 事件环 + SceCtrl 边沿检测
- `src/vita_main.c`: 启动器，读 `ux0:/data/J2ME00001/launch.cfg`（首行 jar 路径、次行 MIDlet 类名）运行任意游戏
- `src/vita_pcsl.c`: 复制自旧 vita_stubs.c（sceIo 文件 I/O 真实现，socket 仍 stub）
- `config/internal.config`: 改为 display 240x320 后随 VPK 打包
- `midlets/`: HelloMIDlet + CanvasTest（渲染验证）+ InputTest（按键验证）
- 构建: `./build.sh`（`clean`/`midp` 参数）；产物 `build/cmake/midp_vita.vpk`
- 构建已验证: 链接成功、覆盖符号已确认在最终 ELF（checkForSystemSignal=0x160 新实现、
  lfjport_refresh 尾调用 flip_to_display）、VPK 含 240x320 config；**Vita3K 运行验证待做**
- **Vita3K 首次运行反馈 (8-30)**: 能启动，屏幕只显示 Form 的 softbar "Exit"，标题/正文空白。
  根因: `midp_system.jar` 里没有任何 PNG——chameleon 皮肤资源
  (`src/highlevelui/lcdlf/lfjava/resource/`，178 张 skin PNG + indicator) 没进 classes/ 目录，
  CMake 的 `jar cf -C classes .` 自然打不进去。皮肤加载失败 → 布局字段未初始化 → Form 主体不可见。
  修复: vita-port/CMakeLists.txt 的 jar 命令追加
  `-C ${MIDP_DIR}/src/highlevelui/lcdlf/lfjava/resource .`（jar 878KB→1.09MB，183 张 PNG）。
  渲染链已被此次运行间接验证（softbar 上屏=字体+blit+refresh 都工作）。

### 当前构建管线事实（2026-08-30 起，取代早前 ROM 化描述）
- 现行链路是**非 ROM 化**的：CLDC 库 `dist/lib/libcldc_vm*.a`（8-29 20:01）+ MIDP `obj/arm/libobj.a`
  （145 成员）→ midp-vita 式 CMake 链接 → 类从 VPK 内 `midp_system.jar` 文件系统加载
- 旧 8.2MB `libcldc_vm_midp_g.a` 已不存在；`USE_ROM=true`/ROMImage 路线当前未启用
  （`build/vita_arm/ROMImage_*.cpp` 8-30 00:02 生成过，但未链入现行库）

### 网络层落地（2026-08-30，vita_net.c，已构建验证）
- **`src/vita_net.c`**（~800 行）: pcsl socket/server-socket/network/datagram 全量真实现，
  替换原先 vita_pcsl.c 里的 stub 段（已删）。
  - Vita 侧 BSD socket 走 newlib libc.a 里的包装（内部是 sceNet），无 `sys/ioctl.h`，
    `available()` 用 MSG_PEEK 1 字节探测实现
  - 非阻塞 connect: EINPROGRESS→PCSL_NET_WOULDBLOCK；配套 `vita_net_poll()`（select 全 fd 集）
    在 `vita_input.c` 的 `checkForSystemSignal()` 里被周期调用，就绪时发
    NETWORK_READ/WRITE/EXCEPTION_SIGNAL 唤醒 `midp_thread_wait` 阻塞的协议线程
  - NetInit: `vita_net_early_init()`（sceSysmoduleLoadModule(NET)→sceNetInit 64KB→sceNetCtlInit），
    在 vita_main.c 最先调用；幂等（g_net_up）
  - 错误码映射 EWOULDBLOCK/EAGAIN→WOULDBLOCK，其余→IOERROR；`pcsl_lastNetworkError` 对齐上游
  - datagram 读写签名为 `char *buffer`(pcsl_datagram.h 原样)；addrToString 用 plain malloc
    （libpcsl_memory.a 只导出 pcsl_memory_alloc/free，与 pcsl_mem_malloc 同义不同名）
- CMake: 加 `src/vita_net.c` + `SceNet_stub SceNetCtl_stub`；libpcsl_network.a 实际零导出符号，
  无重复定义风险；velf nm 验证: 114 个 `pcsl_*` T 符号全部来自 vita_net.o
- **HTTP 即免费获得**: com.sun.midp.io.j2me.http 是纯 Java，跑在 socket:// Protocol 上
- **NetTest MIDlet**（midlets/NetTest.java，MANIFEST MIDlet-4）: HttpConnection GET
  http://example.com/，线程化，状态+响应头+正文前 128 字节上屏；build_jar.sh 已纳入
- 构建状态: exit=0，VPK 含 Hello.jar(NetTest.class)+midp_system.jar+178 PNG；**Vita3K 真跑待测**

### 按键 repeat + 音频结论（2026-08-30 续）
- **vita_input.c 增加 key-repeat**: PRESSED 后 400ms 起发 KEYMAP_STATE_REPEATED(3)，
  每 60ms 一次，RELEASED 清状态；vita_input_poll 两条路径（无变化/有变化）都调用 repeat_tick。
  构建过、velf 内 checkForSystemSignal/vita_input_poll 确认生效。
- **MMAPI 评估结论（重要，勿重复调研）**: 本构建是 CSF-lite，`javax.microedition.media`
  的 Manager 是**编译期死的**——javap 确认 createPlayer 无条件抛 MediaException、
  playTone 只做参数校验后 return、getSupportedContentTypes 返回空数组；
  libobj.a 中无任何 Java_com_sun_mmedia native。做音频 ≈ 从零移植 MMAPI native+Java，
  成本远超收益。**结论: 放弃 phoneME 体系内音频，游戏音效若需要走 future 原生 hack**。
  可按下 L/R+START 的原生菜单（vita_menu.c）承担声音开关。

---

## 项目概述

**目标**: 在 PS Vita 上运行 J2ME/MIDP 应用（Java ME）

**核心组件**:
- phoneME CLDC - Java VM 实现
- phoneME MIDP - MIDP 2.0 实现  
- Vita SDK 交叉编译工具链

**工作目录**: `/home/zyb/vitasdk/samples/j2me/`

---

## 项目结构

```
j2me/
├── phoneme-cldc/          # Java VM (CLDC 1.1)
│   ├── src/vm/
│   │   ├── share/handles/Universe.cpp    # HiddenPackage bypass
│   │   ├── share/ROM/                    # ROM 生成相关
│   │   └── os/vita/                       # Vita 原生代码
│   └── build/vita_arm/
│       ├── dist/lib/libcldc_vm_g.a         # 编译好的 VM 库
│       ├── dist/lib/cldc_classes.zip      # CLDC 类
│       └── romgen/generated/              # ROMImage_*.cpp
├── phoneme-midp/          # MIDP 2.0 实现
│   └── build/vita_arm/
│       └── classes/       # 编译好的 MIDP 类
│           ├── com/
│           ├── java/
│           └── javax/
└── midp-vita/             # Vita 启动器
    ├── src/main.c         # 入口点
    ├── build/             # VPK 输出
    └── build_midp_vita.sh # 构建脚本
```

---

## 技术细节

### 工具链
- 编译器: `arm-vita-eabi-gcc`
- 构建系统: CMake + 自定义 Makefile
- ROM 生成: `romgen` (32-bit x86 Linux binary)
- 环境: `/home/zyb/.local/vitasdk`

### ROM 生成流程
```
cldc_classes.zip + MIDP classes (com/, java/, javax/) + midp_system.jar + Hello.jar
    → min_rom.jar (合并，包含所有系统类)
    → dist/bin/romgen
    → ROMImage_*.cpp (19,567 objects, ~1.2MB)
    → libcldc_vm_g.a (链接，UseROM=true)
```

### UseROM 标志
- **关键**: `UseROM = true` 从 ROMImage 加载系统类，不再需要文件系统上的 midp_system.jar
- 配置位置: `phoneme-cldc/build/vita_arm/vita_arm.cfg`
- 启用条件: `ENABLE_ROM_GENERATOR = true`, `ROMIZING = 1`

### VPK 打包路径
```
data/J2ME00001/
├── midp_system.jar      # (嵌入 ROM，不再需要)
├── Hello.jar            # 用户 MIDlet
└── lib/
    ├── internal.config  # MIDP 内部配置
    └── system.config    # MIDP 系统配置
```

### 类加载路径
- ROM 包含: 所有 CLDC + MIDP + midp_system.jar + Hello.jar 类
- VM 启动: 从 ROM 加载 `com.sun.midp.main.MIDletSuiteLoader`
- 不再需要从文件系统加载

---

## 已解决问题 ✅

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| HiddenPackage error | JVM class 未标记 hidden | 修改 `Universe.cpp` 添加 VITA bypass |
| NoClassDefFoundError | midp_system.jar 未打包 | 嵌入 ROM |
| 多 JAR classpath | `:` 分隔符问题 | 修改 `getClassPathPlus()` |
| ROM skeleton | 未生成真实 ROM | 实现 romgen 集成 |
| 构建脚本混乱 | 单文件 | 拆分为模块化 |
| `app0:/u/` 路径错误 | `UseROM = false` 导致从文件系统加载 | 启用 `UseROM = true`，ROM 包含所有类 |
| ClassFormatError:154 | 用户 jar 未做 CLDC preverify | `build_jar.sh` 加 preverify 步骤（javac → preverify → jar） |
| IAE "does not support title" | `lfjport_get_display_capabilities` 返回 0；`lfjport_get_display_device_ids` 返回 NULL 但 KNI 会解引用 | `lfjport_fb_export.c` 返回 255（全能力位）；ids 返回静态数组 `{0}` |
| **VPK 内 ELF 陈旧（本次白屏反复的元凶）** | `libobj_no_main.a` 只经 `target_link_options` 传入，CMake 未注册文件级链接依赖，归档更新后 make 判定无需重链；`vpk` 目标链只依赖 `.self/.sfo` 也探测不到 | CMakeLists 给 midp_vita 加 `LINK_DEPENDS "${LIBOBJ_NO_MAIN}"`；**每次构建后必须 `arm-vita-eabi-objdump` 验证 ELF 内实际代码**。教训：改完源码后 ELF mtime 不变 = 链接没发生，产物不可信 |

---

## 当前状态 🔄

### 2026-09-05 v01.10：音乐开关卡死真凶 #2——GC encode 无门控转储洪水（最后一个）
- **用户实测 v01.09 重发版（b162）**：✅ 能进游戏、CP tags 洪水消失（上轮修复生效）；❌ 音乐开关仍卡死。
- **根因**：`ObjectHeap.cpp:2134` 的 **GC 压缩编码转储无任何门控**——每次 GC 压缩对**每个存活对象**打一行 `GC encode:`。音乐开关 → MMAPI 类链加载 → 分配压力触发 GC → 数千对象 = 数千行 stderr → Vita3K 慢 I/O = "卡死"。与 CP tags 洪水**同一故障模式、不同源头**。用户日志铁证：洪水内容从 CP tags 变成了 GC encode。
- **不是存储问题**：日志里 RMS/文件系统零异常，卡死瞬间恰好是 GC encode 洪水开始处。
- **修复**：①`ObjectHeap.cpp` 加 `#ifdef VITA_GC_DEBUG` 门控（cldc b82a81f，与 2959a08 同款最小补丁）②`tools/build_merged_o.sh` 新工具：按库内同款 PRODUCT ABI 旗标重编单个 `_MergedSrcNNN.cpp` 作替换成员③重编 003（GC encode 所在成员）移植进库。
- **全库终验**：转储串（GC encode/CP tags/PRE idx/NOT in heap/var_oops_do）全 0、`D jvm_fast_globals` 唯一、C 解释器符号 0。ELF：`81381ae0 D jvm_fast_globals`。
- **产物**：`samples/j2me/midp_vita_v0110.vpk`，md5 `08ab999d33e9bdbbe440fce9bc312e2e`，版本串 `v01.10 b164 (8d02608)`。
- **源码 dump 审计（本轮已做）**：`grep fprintf(stderr` 全量过一遍，ClassFileParser 全部门控、JavaDebugger 仅 KDWP 异常路径（不可达）、ObjectHeap 已修——**已知无门控洪水源清零**。若音乐开关仍卡，stderr 应已干净，届时转向 vita-port 层 MMAPI 状态机排查（`vita_audio_javacall.c`，看 `midp_stderr.log` 定位）。
- **库备份**：`libcldc_vm.a.v0109_hybrid_pre_gcfix`（GC 修复前）、`.v0109_hybrid`（混合库初版）、`.kg_puredumps`（纯 KG）。

### 2026-09-05 v01.09 重发：上次归档拷错文件，用户从未运行过混合库
- **事故**：0904 归档 `midp_vita_v0109.vpk` 时 `cp` 的源是 `vita-port/build/midp_vita.vpk`（旧布局残留，Sep 4 08:57 = b159/fc3c534 世代），而真正 17:16 的混合库构建产物在 `vita-port/build/cmake/midp_vita.vpk`。**用户装的其实是 v01.08 世代二进制**——日志版本串 `b159 (fc3c534)` + CP tags 转储洪水与 v01.08 行为完全吻合（能进游戏、音乐开关卡死）。
- **教训两条**：
  1. **SELF/VPK 内的 eboot.bin 是压缩封装，strings 验证无效**——只有链接前的裸 ELF（`build/cmake/midp_vita`）可做字符串验证
  2. **版本串是防呆底线**：VITA_VERSION 停在 01.06 长期未 bump，导致用户日志无法区分二进制世代，事故漏网。以后**每次发布必须 bump VITA_VERSION 并核对日志版本串**
- **修复**：VITA_VERSION → 01.09；删除旧路径残留 `build/midp_vita.vpk`；build.sh 输出注明唯一产物路径。重链后版本串 `v01.09 b162 (5c96440)`（提交 5c96440 后重链）。
- **产物（最终版）**：`samples/j2me/midp_vita_v0109.vpk`，md5 `24d1033da5edda5c08ce022691c9f873`（14295657B）。ELF 终验：`81381b58 D jvm_fast_globals`、C 解释器符号 0、转储串 0。
- **测试判据**：装新包后日志第 4 行必须是 `version: J2ME Player v01.09 b162 (5c96440)`。若显示其他版本 = 装错/没覆盖。预期游戏正常进、音乐开关不再卡死、stderr 无 CP tags 洪水。

### 2026-09-04 v01.09：音频开关卡死根因 = KG 库带回类加载转储洪水；库合成（KG 骨架 + 无转储新世代 004/005 + 独立补丁成员）
- **用户实测 v01.08**：✅ 能正常进游戏（GP 表双定义根因闭合确认）；❌ 音乐开关处卡死无响应——"回到前几天的问题"。
- **根因（实测证据链）**：
  1. 音乐开关触发 MMAPI 类链首次加载 → **KG 库（8-31 世代）的 `_MergedSrc004.o`/`_MergedSrc005.o` 里类加载 stderr 转储还活着**（strings 实测：004 含 `CP tags`/`PRE idx`/`NOT in heap`，005 含 `var_oops_do`；9-4 世代库 = 0）→ Vita3K 慢速 I/O 下每类数千行 = "卡死"。这正是 9-2 cldc 2959a08（`VITA_CP_DEBUG` 门控三处转储）修掉的老病——**修复在源码里但不在 KG 二进制里**（KG 备份于 9-2 16:15，早于该源码修复进库的世代）。
  2. v01.08 恢复 KG 库 = 连同修复一起回退，老病复发。**源码树当前是干净的**（`ClassFileParser.cpp`/`ConstantPoolDesc.cpp` 转储均已被 `#ifdef VITA_CP_DEBUG` 门控）。
- **修复：库合成而非整库回退**（既保留 KG 世代"无 C 解释器"的正确骨架，又拿到无转储的类加载代码）：
  1. **KG 库为基础**（D 版 GP 表、无 C 解释器、事件泵修复都在，反汇编确认）
  2. **替换 `004`/`005` 为 9-4 世代的无转储版本**（来自 `libcldc_vm.a.cinterp_broken` 提取——这两个成员自身干净：0 转储串、0 C 解释器符号）
  3. **世代分组漂移补齐**：新世代把 SNI_*/StackmapGenerator*/fplib 从 004/005 挪进了 006（006 带 C 解释器不能整拿）→ 用 `tools/build_extra_o.sh`（006 头文件链 hpp-only + include 源文件 + 完整 PRODUCT 旗标）独立编译 7 个补丁成员补回：`sni.o`、`StackmapGenerator.o`、`Cosine/Sine/Tangent_kernel.o`、`JFP_lib_sin/cos.o`，外加 `vita_vm_clean_shutdown` 空 shim（真身在 Interpreter_c.cpp，仅设看门狗标志；KG 世代无看门狗，no-op 安全）
  4. **闭合验证**：71 个缺失定义全部闭合；库级符号闭合检查（`tools/symcheck.sh`）泄漏 111 个全是预期外部（JVMSPI/libc/_rom_*）；最终 ELF `81381b58 D jvm_fast_globals` ✅、C 解释器符号 0 ✅、转储字符串 0 ✅
- **产物**：`samples/j2me/midp_vita_v0109.vpk`（md5 `ed0d9563b2e0258f3134549d566f1f3e`，14308809B）。库双备份：`/tmp/libcldc_vm.a.V0109_HYBRID` + `dist/lib/libcldc_vm.a.v0109_hybrid`。
- **rebuild_vm.sh 已修正（本轮）**：①删除 `ENABLE_C_INTERPRETER=true`（根源矛盾开关，注释说明双定义机理）②`pack_lib` 加硬校验：`jvm_fast_globals` 必须恰好 1 个 D 定义、库内 0 个 C 解释器符号，违反即 die——防再犯。禁用令解除，但**运行后必须跑 `tools/symcheck.sh` 复验**。
- **工具沉淀**：`tools/build_extra_o.sh`（独立 TU 补丁成员编译：006 头链 hpp-only + PRODUCT ABI 旗标）、`tools/symcheck.sh`（库符号闭合检查）——从 /tmp 固化进项目。
- **测试预期**：v01.09 = v01.08 全部 + 类加载转储清除。音乐开关应不再卡死（Vita3K stderr 也不再刷 fd 0x7 洪水）。若音乐开关仍卡但无日志洪水 → 剩余嫌疑是 MMAPI Player 状态机（vita-port 层 `vita_audio_javacall.c`），届时看 `midp_stderr.log` 定位。

### 2026-09-04 v01.08：★ 回归根因闭合——C 解释器混入导致 GP 表双定义被静默消解（已恢复 8-31 世代 VM 库）
- **用户的判断是对的**：不是"音频改动"本身，也无需逐症状打补丁。8-31 可玩 → 后续崩溃的**真正回归点 = 9-2 01:20 的 `rebuild_vm.sh`（samples 0ff302d）**。
- **根因链（全部实测证据）**：
  1. **8-31 可玩世代（KG 库，`/tmp/libcldc_vm.a.KNOWN_GOOD`，9-2 16:15 备份）没有 C 解释器**——其 `_MergedSrc006.o` 无 `g_jpc`/`g_jpc`/`interpreter_dispatch_table` 符号；VM 跑 **ARM 汇编快速解释器**（`Interpreter_arm.o`），它定义 **`jvm_fast_globals` D 版（1036 字节 .data，r10 基址访问的 GP 指针表）**——这是唯一定义 ✅
  2. **9-2 `rebuild_vm.sh` 从第一版就自相矛盾**：`ENABLE_C_INTERPRETER=true` 把 `Interpreter_c.cpp`（定义 **B 版 `JVMFastGlobals jvm_fast_globals`**，176 字节零初始化 bss，`Interpreter_c.cpp:4860`）编进 `_MergedSrc006`，又从旧库提取 `Interpreter_arm.o`（D 版）塞回库 → **同名双定义**
  3. `--allow-multiple-definition`（8-30 8a为修双定义链接错误加的，当时无害）把冲突静默消解 → **最终 ELF 绑定 B 版**（曾实测 `81390564 B`，r5=0x8139255c 现场离它 +0x2000 越界）→ 快速解释器 GP 表全是零/垃圾 → 野跳进 ROM 字节码区滑行 → 崩。9-3~9-4 诊断的"jsr/ret 无实现"（0e8c406）、"dispatch 表污染"（ae48cdb）、"PC=0"（900dd85）、"ROM text 冒充 handler"（d206ff5）**全是这个双定义的下游症状**——C 解释器本来就不该在构建里，jsr/ret 在快速解释器里本来就有实现。
  4. 崩溃寄存器 ASCII 碎片（"skin"/"btn_"/"srcx"/"srcy"/"pngP"）= 滑行路过的 chameleon 皮肤字符串表（.rodata 0x812ee280+，"keyboard.btn_"、"btn_mid_sel"），佐证执行流失控范围。
- **修复**：恢复 `/tmp/libcldc_vm.a.KNOWN_GOOD` 为 `phoneme-cldc/build/vita_arm/dist/lib/libcldc_vm.a`（混装问题代备份为 `libcldc_vm.a.cinterp_broken`）；vita-port `./build.sh` 重链。**验证**：最终 ELF `81381ae4 D jvm_fast_globals`（D 版 .data 指针表胜出 ✅），`g_jpc`/`interpreter_dispatch_table` 符号数为 0（C 解释器不在 ✅）。
- **产物**：`samples/j2me/midp_vita_v0108.vpk`（md5 `0827c5011d9c9a198edd51197f5f7add`，14289566B）。
- **构建坑（再次踩中，务必牢记）**：
  - **CMake 库文件级依赖缺失**：库归档更新后 make 判定无需重链，ELF 陈旧——`rm build/cmake/midp_vita` 强制重链后必须 `arm-vita-eabi-nm` 验证绑定（v01.07 就是没验证发了陈旧包）
  - **javac 不在 PATH**：`build_jar.sh` 需要 `export PATH=/home/zyb/tools/jdk8u502-b07/bin:$PATH`
- **rebuild_vm.sh 必须修正后才能再用**（本轮未改，防再犯）：`ENABLE_C_INTERPRETER=true` 与"从旧库提取 Interpreter_arm.o"的组合 = 必然双定义。要么去掉 C 解释器（回到快速解释器，当前 KG 世代的做法），要么全量用重新生成的 Interpreter_arm.o（需先恢复 f2i/d2i stub）。**在修正前禁止运行 rebuild_vm.sh**。
- **9-3~9-4 的 C 解释器侧改动全部无效但无害**（jsr/ret 实现、undef_bc 安全网、面包屑环、看门狗、handler 窗口）——它们编在 `_MergedSrc006` 里，但该成员现在来自 KG 世代（无这些代码）；源码保留，不回滚（若未来切回 C 解释器可复用）。但注意 **OS_vita.cpp 的看门狗（900dd85）也不在了**（KG 世代的 OS_vita.o 无 watchdog 符号）——挂起型崩溃将无转储，只能靠 Vita3K 日志。
- **测试预期**：v01.08 = 8-31 世代 VM + 之后全部 vita-port/MIDP 层改进（音频 playTone 状态机、media 事件桥、RMS 隔离 groundwork、日志修复）。若仍崩，则根因在 vita-port/MIDP 层（音频线程、事件桥、RMS 隔离），需用 git 在 vita-port 提交线上二分（`b29d725` 之后的提交都只动 vita-port 层，二分成本低）。

### 2026-09-04 v01.07：dispatch 校验窗口改为表自身采样（拦截 ROM-text 冒充 handler）
- **01.06 实测崩溃形态（用户 vita3k.log）**：进游戏 35ms 后硬崩，PC=0x73726378（"srcx"）、r12=0x73726379（"srcy"+Thumb 位）、r4=0x50676e70（"pngP"）、LR=0x8112bc18（ROM text 内）、Thumb:true——**执行流滑进 ROM 字节码数据当指令执行，间接跳转目标被字符串碎片冒充**。
- **01.04 校验的漏洞（本轮核心发现）**：旧窗口 `[0x81000000,0x81400000)` **包含 ROM text 块（.rodata 内 0x810fbe58 起）**，被污染成 ROM 字节码地址（r2=0x8112bbf0）的表项顺利通过校验 → blx 跳进数据区滑行。01.03 的 0x125d1784（Java 堆）能拦，01.06 的 ROM 地址拦不住。
- **修复（cldc d206ff5）**：`Interpreter_c.cpp` 增 `interpreter_handler_min/max`，init 后从初始化完的表采样 min/max 作为唯一合法窗口。实测验证：bc_impl_* 全在 .text `0x8108fb54..0x810a519c`，ROM text 起 0x810fbe58——ROM 地址必然被拒，且无需硬编码链接边界（链接器可自由排布）。污染表项会在 blx 前触发 `bc_crash_report`（64 条面包屑 + slot/handler/legal window 全套诊断），而不是无声硬崩。
- **构建**：`rebuild_vm.sh` 全代重编（romgen 宿主端仍报错——ROMImage 无需重生成，Java 类未变；31 objects + 21 成员库 OK）。VPK md5 `d38a6c7fe1ab58219f1889b465fe4cb2`。
- **测试预期**：装 01.07 跑口袋灵兽。**两种结果都有信息量**：
  - 若出 `dispatch table entry outside handler window` 报告 → 表污染被拦，日志含 slot/handler/窗口——回传即可定位污染源；
  - 若直接正常进游戏 → 说明 01.06 的硬崩就是 ROM 地址骗过旧校验所致，窗口法闭环。
- **遗留**：若表项反复被污染，说明有越界写源头未除（01.03 的 0x5d1784 低 24 位与 01.06 皆指向同一污染源）——需在拦截报告后追查写入者（候选：某 bc_impl 的越界 store、或 GC/ROM 化类的静态初始化写穿）。

### 2026-09-04 v01.06：PC=0 崩溃机制修正 + 解释器看门狗线程（cldc 900dd85，待用户复测）
- **01.05 崩溃形态再定位（推翻 01.04 的"opcode 越界"结论）**：用户日志显示校验通过后 PC=0（取指错误自刷 53 万行，Vita3K 对 PC=0 只刷日志不杀进程）。反汇编证明分发循环 `blx r2` 前的 `ldrb r2,[r3,#-512]` 取 handler 正常——**表项未被破坏**；PC=0 是 handler 内部间接跳 0（r2 残留为证），即 bc_impl 内部控制流损坏，非分发表越界。01.04 的 handler 范围校验因此拦不住（handler 本身合法）。
- **看门狗方案（cldc 900dd85，3 文件 +147 行）**：PC=0 后解释器线程彻底失联，任何解释器内巡检都无法执行 → 改由独立线程兜底：
  - `Interpreter_c.cpp`：主循环心跳 `vita_interpreter_heartbeat++`（volatile）；`undef_bc_stop`/`bc_crash_report` 开头置 `vita_fatal_reported_flag`（防看门狗误报）；`vita_vm_clean_shutdown()` 供 JVM::stop() 正常退出时通知看门狗。
  - `OS_vita.cpp`：`Os::initialize()` 末尾启动看门狗线程（2000ms 检查间隔、30000ms 超时）；心跳冻结超时 → 调 `vita_interpreter_dump()` 转储面包屑环 64 条到 stderr（设备侧 `ux0:/data/J2ME00001/midp_stderr.log`）→ `sceKernelExitProcess(-3)`。
  - `JVM.cpp`：`stop()` 调 clean_shutdown（文件级 extern "C" 声明——函数内 extern "C" 不合法）。
- **误杀风险（如实）**：>30s 无字节码执行的合法暂停（如长加载、阻塞 IO）会触发看门狗转储+退出。若误杀，调大 OS_vita.cpp 的 `WATCHDOG_TIMEOUT_MS`。
- **构建系统新认知（本轮核心教训，防再犯）**：
  - **ABI flavor 必须单代**：`Deterministic`/`jvm_perf_count` 是非 PRODUCT 专属符号（develop flag 在 `#ifdef PRODUCT` 下宏为空；`ENABLE_PERFORMANCE_COUNTERS` 在 `ENABLE_MINIMAL_ASSERT_BUILD` 分支被清 0——但该分支 PRODUCT 下不生效，实际由 jvmconfig 的 ENABLE 表决定：非 PRODUCT=1/PRODUCT=0）。`JVM_TRAPS` 在 PRODUCT 下展开为空（GlobalDefinitions.hpp:1244 `#ifndef PRODUCT`）→ 符号 mangled 名都不同（`new_symbolEP12JVMTypeArrayPci` vs `...PciP8JVMTraps`）。**基线库是 PRODUCT 代；任何手动重编对象必须带 rebuild_vm.sh 的完整 PRODUCT 旗标**，否则混装必链接失败（本轮先后报 Deterministic/jvm_perf_count undefined、再报 JVMUniverse 三方法 undefined，全是同一根因的两个侧面）。
  - **手动重编单个合并对象的正确命令**（rebuild_vm.sh 同款旗标，在 target/release 下）：`make _MergedSrc003.o BUILD_DIR_NAME=vita_arm IsLoopGen=true LOOP_GENERATOR_DIR=../../linux_arm/loopgen/app ENABLE_ENABLING_CHECK=false ENABLE_C_INTERPRETER=true FORCE_GCC= GNU_TOOLS_DIR=$VITASDK CPP_DEF_FLAGS="-DPRODUCT -DROMIZING=1 -DARM -DVITA -D__PSP2__ -Wno-narrowing -fpermissive -marm -march=armv7-a -mtune=cortex-a9 -mfloat-abi=hard -mfpu=vfpv3 -DSUPPORTS_MEMORY_MAPPED_FILES=0 -DSUPPORTS_ADJUSTABLE_MEMORY_CHUNK=0 -DSUPPORTS_TIMER_THREAD=1 -DSUPPORTS_TIMER_INTERRUPT=0 -DUSE_VM_EXCEPTIONS=0 -DUSE_BSD_SOCKET=1"`——**先 rm 旧 .o**（touch 不够，make 判定依赖未变时直接跳过）。
  - **glue 符号定义在 _MergedSrc006**（Interpreter_c.cpp 的 FUNC_UNIMPLEMENTED 宏），`compiler_glue_code_start/end` 声明在 GlobalDefinitions.hpp:1954（`#if ENABLE_COMPILER && USE_COMPILER_GLUE_CODE`）；ObjectHeap.cpp:1240 引用 glue 的代码在 PRODUCT 目标编译下无错——此前脚本日志里的 ObjectHeap/Natives 编译错误来自 romgen 宿主端（CROSS_GENERATOR），可容忍。
  - **ar 重打包必须在 target/release 目录内以 basename 执行**（`ar r $L $m`，m 不带路径），否则报 "No such file"。
- **产物**：`samples/j2me/midp_vita_v0106.vpk`（md5 b7cc90fd1f9194041c808c178a784d06，14308905B，param.sfo AppVer=01.06）。库 21 成员单 PRODUCT 代（Interpreter_arm 沿用旧库 ARM 汇编成员）。
- **vita_main.c 顺序修复（本轮）**：原 `freopen(midp_stdout/stderr.log)` 在 `sceIoMkdir(DATA_DIR)` **之前**——首次安装时 `ux0:/data/J2ME00001/` 不存在，freopen 静默失败，stderr 停留在 tty 设备（Vita3K `*** TTY:` 逐字符流），导致 midp_stderr.log 为空（8-31 日志丢失事故的根因）。已将三个 `sceIoMkdir`（DATA_DIR/appdb/lib）移到 freopen 之前。注意：**8-31 事故的日志并没有真丢**，它进了 Vita3K 的 TTY 流；此后修复了目录顺序，日志会正常落盘。
- **测试预期**：装 01.06 跑口袋灵兽，若再冻结/崩溃，看门狗会自动转储 64 条面包屑到 `ux0:/data/J2ME00001/midp_stderr.log` 并退出进程（Vita3K 日志出现干净退出而非 53 万行刷屏）——**请回传该日志**，面包屑 bcp/opcode 可直接定位坏 handler。

### 2026-09-04 v01.04：dispatch 表项破坏修复（待用户复测）
- **01.03 崩溃定位（vita3k.log 已闭环）**：`Invalid read at 0x125d1784, PC=0x125d1784, LR=0x8109d7a4`。LR 正是分发调用点 `8109d7a0: blx r2` 的下一条 → PC==跳转目标 → r2=handler=0x125d1784（Java 堆地址）→ **`interpreter_dispatch_table[code]` 表项被越界写破坏成野指针**。tripwire 拦不住（g_jfp/g_jsp 合法，表内容本身坏）；面包屑无法落盘（blx 后 CPU 立即异常）。与 01.02 r4=0xdc5d1784 低 24 位相同（0x5d1784）——同一污染源的多轮表现。
- **修复（ae48cdb）**：Interpret() 分发循环每次 `blx` 前校验 handler ∈ [0x81000000, 0x81400000)，越界先打 slot/handler/表边界到 stderr，再走 bc_crash_report()（面包屑+干净停机）。
- **romgen 排障笔记（未修，暂不需要）**：`romgen/app/ROMImage.o` 只有 576B 空壳——`ROMImage.cpp` 全包在 `#ifdef ROMIZING` 里，而 jvm.make:348 只在 `IsTarget=true` 时定义 `ROMIZING_CFLAGS`，romgen 是宿主工具（IsTarget=false）所以没 -DROMIZING。链接缺 `_rom_data_block` 即此因。**本轮只改 Interpreter_c.cpp，Java 类库未变，ROMImage 无需重生成**，build 阶段（31 objects + repack libcldc_vm.a 21 members）成功即够。若日后要重生成 ROM，需在 romgen 的 CPP_DEF_FLAGS 里补 -DROMIZING=1。
- **注意**：`cmake --build build` 的真产物在 `vita-port/build/` 根下；`build/cmake/` 是旧目录（02:08 的 01.03 包），勿混淆。
- 产物：`samples/j2me/midp_vita_v0104.vpk`（md5 a00017838b809c19d3157e7cacc6fb99）

### 2026-08-31 里程碑：游戏可玩、中文正常显示 ✅
- 口袋灵兽等 J2ME 游戏可在 Vita3K 进入并正常游玩，图像与中文文本渲染全部正常
- 最终根因见"关键文件修改记录"一节（fb_load 无调用点，commit 0fc9e8d）
- 游戏菜单（安装/卸载/启动/横竖屏）可用；诊断日志体系（font_debug.log 走独立 fd）就绪

### 构建成功
- VPK: `vita-port/build/midp_vita.vpk` (~13MB，含 1.4MB fontbitmap.bin + 16MB font.ttf)
- ROM: 19,567 objects, ~1.2MB (包含 midp_system.jar + Hello.jar)
- UseROM: enabled

### 遗留事项
- [ ] RMS 未按游戏隔离（所有游戏共用一个 suite 空间，存档可能互相污染）
- [ ] 音频未实现（游戏无声；需接 sceAudio 或 BGM/PFS stub）
- [ ] 游戏菜单本体仍是 ASCII 5x7 字体（可复用 fontbitmap.bin 做 CJK 菜单）
- [ ] 真机验证（目前仅 Vita3K）
- [ ] 性能观察（解释器执行、绘制帧率）

### 2026-09-05 音频链路深挖：Manager 双版本矛盾 + ToneTest v3
- **midp_system.jar 是陈旧存根**（8-31 构建，早于 9-01 的 USE_JSR_135=true）：
  `src/media/reference/classes/.../Manager.java` 的 createPlayer 直接 throw
  MediaException、playTone 空操作。jar 中 com/sun/mmedia 只有 10 个内部视频类。
  **但 ROM（9-05 重建）已含完整 jsr135 实现**（DirectTone/HighLevelPlayer/
  NativeTonePlayer/DirectPlayer…，最终 ELF strings 验证）——ROM 化类优先于
  classpath jar，运行时系统类实际来自 ROM，音频 Java 链路是好的。jar 存根
  被 ROM 遮蔽，不构成运行时 bug，但掩盖排查视线；重建 classes.zip 时需注意
  `Options.gmk`（USE_JSR_135=false）与 `Settings.gmk`（true）的覆盖顺序。
- **v01.12 用户实测"按键没反应"再分析**：日志证实菜单写入 game.cfg（18B =
  "ToneTest\nportrait\n"）且 VM 打开 games/Hello/game.jar——**新 jar 确实在跑、
  类名正确**，替换操作成功。midp_stdout.log 为空是 v2 设计盲区：第一句日志要
  等按 CROSS 才产生，"ToneTest 没起来"与"VM 冻结/事件泵死"无法区分。
- **ToneTest v3（自驱动）**：ctor+startApp 即打 banner（"[TONE] ctor v3"）、
  1Hz 心跳（hb 计数器，每 10 次落 stdout）、t=2.5s 起每 2.5s 自动推进一阶段
  （无需按键）、TRIANGLE/CROSS 仍可干预、60s 总看门狗自动退出。一次运行即可
  分流：无 banner=类加载失败；banner 停、hb 停=VM 冻结；hb 走而按键无
  lastKey=输入链路断；stg1-5 最后一条日志=音频卡点。
- VPK v01.13（b166, 1c4730e 链）产物 vita-port/build/cmake/midp_vita.vpk，
  含 v3 ToneTest（ctor v3 / alive # 标记已验证在 class 内）。
- **v01.14：默认启动类改为 ToneTest**（vita_main.c 默认 class_name，音频排障
  期间临时）。理由：启动器每次启动都把 VPK 内 Hello.jar 重新复制到数据目录
  （copy_file(app0:...)），默认路径永远跑最新构建，用户操作简化为
  "装 VPK → 启动 → 三角退出菜单"，不再涉及 games/Hello/ 副本（VPK 升级不
  覆盖已安装 games/ 副本的坑被整个绕开）。注意 launch.cfg 若存在仍会覆盖
  默认值——排障期间确认用户机器无 ux0:/data/J2ME00001/launch.cfg。
  排障结束后改回 "HelloMIDlet"。

### 2026-09-05 音乐卡死真根因：链接顺序遮蔽 vita 音频实现（v01.15 修复）
- **v01.14 实测**：ToneTest v3 完美运行（ctor banner → 60s 心跳全活 →
  60s 看门狗正常退出），**VM 无冻结、事件泵正常**；此前"按键没反应"实为
  v2 盲区。stg1 稳定抛 `MediaException: Failed to play tone`（非存根消息，
  证明 ROM 里的 jsr135 真实现已在跑）。
- **根因**：`phoneme-midp/build/vita_arm/pcsl/vita_arm/lib/libpcsl_multimedia.a`
  是 `gen_stubs.py` 生成的**全桩 archive**（所有 javacall_media_* 直接
  return JAVACALL_NOT_IMPLEMENTED）。CMake `target_link_options()` 永远排在
  对象文件**之前**，导致链接顺序 = whole-archive(libobj) → libpcsl_multimedia
  → 端口层 .obj；`--allow-multiple-definition` 下未定义符号从先出现的桩
  archive 解析，`vita_audio_javacall.c` 的真实现被静默丢弃。KNI 把
  NOT_IMPLEMENTED 映射为 RESULT_FAIL → "Failed to play tone"。
  **audio_debug.log 从未出现同因**——真实现代码从未执行。
- **验证手段（可复用）**：`nm` + `objdump -d` 看 ELF 中 `javacall_media_play_tone`
  的函数体：桩 = 7 行直接 `mvn r3,#1`（return -2）；真实现 = push r4-r6 +
  movw 取字符串地址（ALOG 日志调用）。
- **修复**：CMakeLists.txt 把 whole-archive 块 + 库 group 从
  `target_link_options` 移入 `target_link_libraries`（CMake 自身对象永远
  在 libraries 前），链接顺序变为 端口层 .obj → libpcsl_multimedia.a。
  修复后 play_tone/create/start/stop 全部落在 0x8100xxxx 端口层区域。
  **不能删除桩 archive**：libobj 的 KNI 对象还引用其中的 javautil_* 字符串
  工具与录音/MIDI 函数（126 个符号中 vita 实现只覆盖 31 个）。
- 遗留：ToneTest 异常路径不推进 stage（stg1 失败后重试到 60s 看门狗），
  属测试代码行为非缺陷；游戏音乐是否依赖 EOM 桥待 v01.15 实测。
- **v01.15 实测：链接修复完全生效**——stg1 playTone 返回 OK（不再抛异常）、
  stg2 createPlayer OK、stg3 realize/prefetch OK，native 音频链路全通。
  stg4 抛 `IllegalStateException: cannot set seq after prefetched` 是
  ToneTest 自身 bug（MMAPI 要求 setSequence 在 realize 后、prefetch 前，
  v3 放在了 prefetch 后）。
- **v01.16**：ToneTest 修正调用顺序（realize → getControl+setSequence →
  prefetch → addPlayerListener → start → 等 EOM）。待实测 stg4/5。
- **v01.16 翻车：全类 CNFE**——同一启动路径 v01.15 能解析 jar、v01.16 抛
  `ClassNotFoundException: ToneTest`。本地与 VPK 内 jar md5 一致、zip 结构
  校验全过（EOCD 校验/CD 遍历/CRC），两版仅差 8 字节 deflate 输出。
  根因判定：JDK `jar` 工具写的是 **streamed zip**（LFH crc/sizes=0 +
  data descriptor，flag=0x0808），phoneME `JarFileParser` 对该布局的偏移
  处理有脆弱边界，特定压缩输出恰好踩中（open_entry 用 CD 的 locOffset+
  CENOFF 寻址 LFH 后按 CENSIZ 解压——任何 DD 吞吐错位即崩）。
  **修复（v01.17）**：`build_jar.sh` 打包后追加 python 规范化步骤——丢弃
  目录桩条目、以固定时间戳重写为**非 streamed** zip（LFH 带 crc/sizes，
  flag=0x0000，无 DD）。此类布局 phoneME 上游 romgen/JAR 工具链即默认格式。
  **教训：交付给 phoneME 的 jar 一律规范化，勿直接用 JDK jar 输出。**
- **v01.17 仍 CNFE → 规范化假设被证伪**。停止猜测改分流实验（v01.18）：
  ①默认类改回 HelloMIDlet（自 v01.13 未变）——若它也 CNFE 则是 jar/环境
  问题，若正常则问题特定于 ToneTest 类字节；②启动日志加
  "runtime Hello.jar size=N (expected M)"（HELLO_JAR_SIZE 烘进
  vita_version.h，CMake file(SIZE)），排除复制截断。
  注意：GenVersion.cmake 新增 HELLO_JAR_SIZE 参数；CMake 中 file(SIZE)
  必须在 gen_version 目标之前（MIDLET_JAR 定义相应上移）。
- **v01.18 决定性数据：runtime jar=9460B ≠ VPK jar=9204B → 根因找到**。
  HelloMIDlet 也 CNFE（排除类字节问题）。9460 恰是 v01.15 jar 尺寸：
  `copy_file` 用 `SCE_O_TRUNC` 覆盖写，但 Vita3K 不实现截断——新 jar
  (9204) 比旧 (9460) 短，写入后旧文件尾巴残留，长度仍 9460。
  JarFileParser 的 EOCD 校验（endpos+22+com==len）对 corrupted 长度
  全部失败 → 整个 jar 不可解析 → 全类 CNFE。
  时间线完全吻合：v01.14→15 新 jar 更长（全量覆盖无残留）→ 能跑；
  v01.15→16 起新 jar 更短 → 残留尾巴 → CNFE。
  **修复（v01.19）**：`copy_file` 改为先 `sceIoRemove(dst)` 再
  `O_CREAT` 打开，不依赖 O_TRUNC；默认类改回 ToneTest（继续 stg4/5
  EOM 桥验证）。
  **教训：Vita/newlib 的 sceIo O_TRUNC 不可靠（Vita3K 实测不截断），
  覆盖写文件必须先 remove 再 create。**
- **v01.19 实测：CNFE 修复确认**（size=9204 匹配、ToneTest 注册 OK、
  stg1-4 全过）。新发现：`playerUpdate: started/stopped/closed` 事件
  **已到达 Java 监听器**——MMEventHandler 派发机器是通的。
  但 EOM 环节测试无效：stage 4 也被 2.5s 自动推进跳过（音序 ~2.25s，
  跳过与 EOM 到达赛跑）→ "stg5: skipped, eom=false" 不说明任何问题。
  **v01.20**：自动推进只覆盖 stage 0..3；stage 4 只能由 EOM 到达或
  15s 看门狗结束；CROSS 仍可手动跳过。
- **v01.20 实测：音频出来了**（链接修复彻底生效），但 **EOM 仍 STUCK、
  按键无响应、60s 退出时 Vita3K 闪退**。症状指纹：Java 内部事件
  （started/stopped/closed，不跨 native）全通；native→Java 注入事件
  （按键、EOM，都走 checkForSystemSignal）全灭。
  **根因（第三次链接遮蔽！）**：`libcldc_vm_ani.a` 的
  `anilib/share/ani_bsd_socket.cpp` 定义了**强符号** `JVMSPI_CheckEvents`，
  函数体只处理调试器 + `ANI_WaitForThreadUnblocking`，**从不调
  midp_check_events**；而 `midp_run.c` 里桥接事件泵的版本是 weak ——
  强符号静默获胜，`checkForSystemSignal` 从未运行。这也是按键从
  ToneTest v1 起就"没反应"的真正原因。
  **v01.21 修复**：新增 `src/vita_checkevents.c` 强符号版：先
  `midp_check_events(bt, n, 0)` 泵事件，再 `ANI_WaitForThreadUnblocking`
  （timeout 钳到 ≤50ms 保证响应）。CMake 对象先于库 →
  `--allow-multiple-definition` 先到先得。ELF 已验证：
  0x81006544 先 blx midp_check_events 再 b.w ANI_Wait。
  **教训：电话本移植三大链接坑——gen_stubs 桩 archive、mastermode_export
  桩、ANI 强符号，全部用同一手段（项目对象放前面）压制。**
  遗留：60s 退出闪退（EXCEPTION_ACCESS_VIOLATION）待 EOM 验证后再查。
- **v01.21 实测全绿**：按键（TRIANGLE 退出到达）、EOM（`EOM RECEIVED -
  bridge OK`）、音频全部工作。事件桥闭环。
- **游戏启动报错分析（三例）**：GameMidlet OK；UC WebClient NPE；
  FileManagerMIDl CNFE。两个根因：
  1. **internal 套件无属性**：`CldcMIDletSuiteLoader.createMIDletSuite()`
     只在 `args[0]` 以 .jar/.jad 结尾时才调 getJarProps/getJadProps 装载
     MANIFEST 属性，否则空 Properties → `getAppProperty()` 全 null →
     框架型应用（UC 等）startApp 读属性即 NPE。
  2. **MANIFEST 类名与 jar 不符**：`FileManagerMIDl`（结尾小写 l）在
     jar 里根本不存在 → Class.forName CNFE。国产 jar 常见。
  **v01.22 修复**：
  1. `vita_main.c` run_argv 增加第 6 参数 jar 路径（arg0 槽位：
     -classpathext 摘除后落在 argv[3]）→ internal 套件装载 MANIFEST
     属性。注意路径必须相对（cwd=DATA_DIR），JarReader native 走
     midpOpenJar 与类装载器同源。
  2. `vita_menu.c` 新增 `zip_walk_names`（中央目录名遍历回调）、
     `verify_class_in_jar`（<class>.class 存在性校验）、
     `resolve_class_in_jar`（兜底挑选：*MIDlet=3 > *Midlet=2 > 顶层类=1，
     排除 $ 内部类与 META-INF，同分短 FQN 优先）、`validate_game_class`
     （扫描+安装两处接线，修正后写回 game.cfg）。宿主侧等价测试
     三项全过。
- **v01.23（属性装载的安全时序修复）**：v01.22 实测 UC 仍 NPE。日志
  分析：`CldcMIDletSuiteLoader.createMIDletSuite()`（runMIDletSuite 第
  286 行）读 jar MANIFEST 发生在 `initSuiteEnvironment()`（第 303 行，
  AccessController.setAccessControlContext）**之前**——上游 internal
  套件从不带属性所以无此路径；我们的 arg0=.jar 修复触发了
  `JarReader.readJarEntry → AccessController.checkPermission`（读
  javax.microedition.io.Connector.http 权限）在 context==null 时抛
  SecurityException，被 getJarProps 的 catch(Throwable) 吞掉 →
  props=null → 空属性 → NPE 依旧。
  **修复**：`phoneme-midp` 的
  `src/security/access_controller_cldc_port/classes/com/sun/j2me/security/AccessController.java`
  checkPermission(name,resource,extraValue) 的 `context == null` 分支
  从 throw SecurityException 改为 return（附 VITA 注释：bootstrap 窗口
  只有受信 loader 代码运行，MIDlet 代码必在其套件 context 设置之后，
  安全性不受影响——MIDlet 代码不可能在 createMIDletSuite 阶段执行）。
  **ROM 再生链（本次跑通）**：`phoneme-midp/build_vita.sh`（make
  GNUmakefile 默认目标）依赖链自动完成：
  AccessController.java → midp_classes/classes.zip（javap 验证
  `ifnonnull; return`，异常字符串已消失）→ `$CLDC_DIST_DIR/bin/romgen`
  → build/vita_arm/ROMImage.cpp → obj/arm/ROMImage.o → obj/arm/libobj.a。
  注意：构建尾部 `bin/arm/libmidp.so` 链接报 multiple definition（库内
  _MergedSrc002/003 重复符号）属**已知无害失败**——vita-port 用的是
  obj/arm/libobj.a（已成功再生），不用 libmidp.so。
  **产物**：`vita-port/build/cmake/midp_vita.vpk`（v01.23，1430 万字节）。
  **FileManagerMIDl 的 JSR-75 结论**：类名兜底已生效（日志显示解析为
  com.app.filemanager.FileManagerMIDlet），但启动报
  NoClassDefFoundError: javax/microedition/io/file/FileConnection——
  JSR-75（FileConnection）在 phoneme_source 全树无源码（只有
  jsr120/135/172/177/211/239/280），phoneme-midp/src/protocol/file 也
  只有 storage 辅助类，无 FileConnection 接口。build_vita.sh 一直
  USE_JSR_75=false（源码不存在）。**实现 JSR-75 是大工程，本阶段
  搁置**；该游戏无法简单修复，需向用户说明。
- **v01.24（"JAR not found"修复——pcsl 相对路径 vs sceIo）**：v01.23
  实测 UC 仍 NPE，但异常已换形：`JarReader.readJarEntry → IOException:
  JAR not found`（SecurityException 消失证明 AccessController 放行
  生效，暴露下一层）。根因是**两条文件通路的相对路径语义不一致**：
  类装载走 `ClassLoader → OsFile_vita.cpp → jvm_fopen`（newlib
  fopen，用进程 cwd——vita_main.c chdir(DATA_DIR) 后能解析
  `games/app/game.jar`）✅；而 JarReader 走
  `midpOpenJar → storage_open → pcsl_file_open（vita_pcsl.c）→
  裸 sceIoOpen("games/app/game.jar")`——**Vita 的 sceIo* 不支持
  相对路径**（无 cwd 概念，必须 ux0:/app0: 绝对前缀）→ 打开失败
  → IOException → props 仍为空 → NPE 依旧。
  **修复（vita-port/src/vita_pcsl.c）**：新增 `vita_resolve_path()`
  （含 `:` 或以 `/` 开头视为已绝对，否则前缀 `VITA_DATA_ROOT =
  ux0:/data/J2ME00001`，与 vita_main.c 的 DATA_DIR 保持同步），
  应用于 `pcsl_file_open`（主修复点）与 `pcsl_file_unlink` /
  `pcsl_file_exist` / `pcsl_file_sizeof`（同病症预防）。
  app0: 兜底逻辑保留（VPK 内只读资源仍走 app0 前缀）。
  产物：vita-port/build/cmake/midp_vita.vpk（v01.24，14296404B）。
  **教训：vita-port 的 pcsl_file_* 层是唯一不经 newlib cwd 的文件
  通路，任何相对路径入口都必须显式解析到 ux0: 绝对路径。**
  调试便利：pcsl_file_open 有 debug_log（ux0:/data/debug_log.txt），
  实测可看 [file_open] path= 行确认路径解析。
- **v01.25（"JAR Corrupt"修复——pcsl_string UTF-8 桩语义全错）**：
  v01.24 实测异常换形为 `JAR Corrupt`——路径修复已生效（open +
  getJarInfo 的 EOCD 定位/大小校验/PK 头验证全过，那些失败会报
  "not found"），死点推进到 midpGetJarEntry。
  **根因（我们自己写的桩）**：`vita_pcsl.c` 的
  `pcsl_string_utf8_length` 返回 `str->length * 3`（"粗估"）——
  `midpGetJarEntry` 用它做条目名长度，与 `findJarEntryInfo` 的
  `entry.nameLen == nameLen` 精确比较：`META-INF/MANIFEST.MF`
  20 字符被算成 60，永远匹配不上 → CD 遍历越过最后一个条目 →
  在 EOCD 位置读 46 字节（CENHDRSIZ）只剩 22 → JAR_CORRUPT。
  同时 `pcsl_string_get_utf8_data` 返回静态 buffer（非上游契约的
  malloc 缓冲），`pcsl_string_release_utf8_data` 不 free → 泄漏。
  **修复（vita-port/src/vita_pcsl.c）**：
  1. `pcsl_string_utf8_length`：真实 UTF-8 字节数（1/2/3 字节
     按码位分类，ASCII 1:1），NULL 返回 -1（上游契约）。
  2. `pcsl_string_convert_to_utf8`：真 UTF-16→UTF-8 编码（多字节
     序列、NUL 结尾、BUFFER_OVERFLOW 判定），替换原 & 0xFF 截断。
  3. `pcsl_string_get_utf8_data`/`release_utf8_data`：malloc 配对
     free，符合上游契约。
  4. `pcsl_file_open/unlink/exist/sizeof` 路径转换统一走
     convert_to_utf8（中文目录名不再被 & 0xFF 弄坏）。
  5. 用户指正：vita_resolve_path 的 VITA_DATA_ROOT 硬编码改为
     运行时 getcwd()（跟 vita_main.c 的 chdir 联动，仅 getcwd
     失败时才回退字面量）。
  **教训：vita-port 的 pcsl_string 桩不是"能编过就行"——上游
  API 有精确语义契约（长度精确性、缓冲所有权），中游代码
  （jar 条目匹配、installer、suitestore）依赖这些契约。写桩
  前先读上游 pcsl/string/utf16/pcsl_string.c 的对应实现。**
  产物：vita-port/build/cmake/midp_vita.vpk（v01.25，14296493B）。
- **v01.26（RMS 持久化修复——"首启正常、之后一直失败"根因）**：
  v01.25 实测 UC 的 "JAR Corrupt" 异常消失（属性链全通），但 NPE
  仍在 `ao.a() bci=4 ← aa.c() ← aa.b() ← aa.h() ← WebClient.startApp()`
  与 v01.24 完全相同。用户提供决定性时间线："**一开始安装的时候有
  正常打开启动过，后面就一直不行**"。
  **根因（RMS 持久化两个实锤桩缺陷）**：
  1. `pcsl_file_truncate` 空操作但返回 0（伪成功）：
     `RecordStoreImpl.compactRecords()`（RecordStoreImpl.java:886）
     删记录后靠 `dbFile.truncate(getSize())` 收缩 db 文件——空操作
     导致文件头 data size 与物理内容错位、陈旧块残留 → 二启
     `getRecordIDs()/getRecordHeader()` 按偏移遍历读出脏数据 →
     UC 混淆代码 `ao.a()` 拿到 null → NPE。**首启无 RMS 数据不需
     truncate，所以首启正常**。
  2. `pcsl_file_rename` 恒返回 -1：suite 存储写路径
     `write_file()`（suitestore_intern.c:497）用"写临时文件→改名"
     模式提交——rename 恒失败导致数据滞留 .tmp、目标文件永远
     不更新。
  连带发现：`vita_storage.c` 的 `get_per_game_appdb` 只匹配
  `"/games/"`，而 vita_main.c 传入相对路径 `games/app/game.jar`
  （无前导斜杠）→ 匹配失败 → 所有游戏退回共享 "appdb"（日志实证
  `[RMS] appdb: ux0:/data/J2ME00001/appdb`），per-game RMS 隔离
  从未生效。
  **修复（vita-port/src/vita_pcsl.c + vita_storage.c）**：
  1. `pcsl_file_truncate`：读-重写方案（Vita 无 sceIoFtruncate）——
     读出前 size 字节到 malloc 缓冲 → sceIoClose → 同路径
     sceIoOpen(SCE_O_RDWR|SCE_O_CREAT|SCE_O_TRUNC) → 写回 →
     文件位置恢复（截到 size）。重开依赖 `vf->path`（open 时记录
     的解析后绝对路径），已补 strncpy 后的显式 NUL 终止。
  2. `pcsl_file_rename`：双路径 convert_to_utf8 → vita_resolve_path
     → sceIoRename（fcntl.h:166 存在）。
  3. `vita_storage.c`：新增 `find_games_seg()` 同时匹配 "games/"
     开头（相对）与 "/games/"（绝对），per-game RMS 隔离生效。
  排查中确认无害的链：free space（totalSpace 默认 4MB、used 恒 0
  → free=4MB，RMS 创建不被挡）；`vita_stubs.c` 里的旧 pcsl 副本
  是死代码（未链接，修复时勿改错文件——objdump 验证 ELF 中是
  vita_pcsl.c 的新实现）。
  **验证**：构建成功；objdump 确认 pcsl_file_truncate 内含
  sceIoLseek/Read/Close/Open/Write 完整读-重写链、
  pcsl_file_rename 内含 sceIoRename。
  **用户验证步骤**：装 v01.26 后**先删除 Vita3K 侧**
  `ux0:/data/J2ME00001/appdb` **目录**（清除旧损坏 RMS 数据），
  再启动 UC——预期首启正常且二次启动也正常。若删 appdb 后仍
  NPE，则 RMS 假设被削弱，需把 game.jar 复制进容器做字节码级
  分析（javap -c 反汇编 ao.a() 的 bci=4）。
  **教训：桩函数"返回 0"不等于无害——truncate 伪成功比直接失败
  更危险（错误被静默吞掉，损坏延迟到下一次读取才爆发）。写桩
  前必须对照上游消费者（RecordStoreImpl/suitestore_intern）的
  语义契约。**
  产物：vita-port/build/cmake/midp_vita.vpk（v01.26）。

---

## 构建命令

```bash
cd /home/zyb/vitasdk/samples/j2me/midp-vita

# 完整构建
./build_midp_vita.sh

# 清理后重建
./build_midp_vita.sh clean

# 仅构建 Hello.jar
bash build_jar.sh
```

---

## 关键文件修改记录

### ⭐⭐⭐⭐ 2026-09-04 第三轮：新包确认生效但换形崩溃 → 面包屑环形缓冲+不变量巡检（cldc 3494149，VPK 01.03）
- **版本确认与教训**：用户第三次日志（09:43）确认跑的是 17:02 新包：模块 NID 0xD947F3F4、e_entry 0x36E9E0（旧包 0xE92EFCF7/0x36E930）；此前 09:37 那份日志用户装的是旧包（LR=0x8108ee90=旧 undef_bc 的 svc 地址，与 23:25 现场逐字节相同）。教训：版本号 01.02 不变导致无法从日志分辨包版本——**已把 VITA_VERSION 升为 01.03 并写进 CMakeLists 注释惯例，今后每轮交付必须 bump 版本**。
- **好消息**：`NID 0xE1A00001 not found`（undef→svc）已从日志消失——分发表 0..201 全注册 + undef_bc 安全停机均生效，jsr/ret 补洞被实测确认。
- **新崩溃形态**：PC=0x8109d750（分发循环），LR 同值，SP=0x8043f028；r4=0x810911fc 是**代码地址**（bc_impl_lload_3 函数体内 `pop {r4,pc}` 指令处，0x810911dc）；`Invalid read uint8_t at 0xe8bd8010` 即该指令编码被当数据读；二次执行 0xe58320b4=`str r2,[r3,#0xb4]` 编码。机制：某 bc_impl 尾声 `pop {r4,pc}` 从栈上弹回脏 r4（代码指针）返回分发循环，循环用 r4 当解释器全局基址（gp=0x81390564 时 g_jpc=[r4+0xb4]、g_jsp=[r4+0xb0]）→ 野读。与上一轮 r4=bc_impl_lshr 函数首同病：**栈/帧不平衡类，坏栈底**，非特定 opcode 缺失。
- **本轮修复（cldc 3494149，Interpreter_c.cpp）**：64 项面包屑环形缓冲 bc_ring[]（每条分发前记 code/bcp/jsp）；分发前两条不变量巡检：g_jfp 4 字节对齐、g_jsp>=_current_stack_limit（声明在 GlobalDefinitons.hpp:1917，定义在 InterpreterSkeleton.cpp:218）；命中即 bc_crash_report 打印 g_jpc/g_jsp/g_jfp/总数/方法名（debug 打印版用 Method::print_name_to，release 版无方法名）+ 最近 64 条面包屑到 stderr（设备侧落 midp_stderr.log）与 tty，然后 Universe::set_stopping()+BREAK_INTERPRETER_LOOP(JMP_STOPPED_MANUALLY) 安全停机。该巡检也能覆盖 jsr/ret 等分支改变 g_jfp 后的损坏。
- **验证**：31 对象 OK；velf 内 bc_ring/bc_ring_idx/bc_ring_count 符号在（.bss 0x102714238 区域）、"VITA FATAL"字符串在、undef_bc 仍在 0x8108eef8；param.sfo AppVer=1.03；VPK 14306614B。
- **下一步**：用户装 01.03 跑口袋灵兽，若再异常，Vita3K 日志里应出现干净停机（无野崩二次异常），**拿 ux0:/data/J2ME00001/midp_stderr.log**——"VITA FATAL:" 块给出方法名+64 条面包屑（bcp/op/jsp），bcp 可对照 jar 反汇编定真凶；巡检未触发但 VAN 崩的场景则比较 g_jsp 窗口找异常回卷。
- **遗留风险**：面包屑本身不修根因，是定位手段；若巡检从未触发而崩在 bc_impl 内部（g_jsp 还没低于 limit、g_jfp 恰好对齐），需把巡检加密度（如同时校验 g_jsp<=stack_base）。

### ⭐⭐⭐⭐ 2026-09-03 第二轮：jsr/ret 修复后游戏仍崩 → undef_bc 安全网改造（cldc a9b44c1）
- **背景**：用户装上含 jsr/jsr_w/ret/ret_wide 的 VPK（cldc 0e8c406 / samples 5ec5d45）后实测"口袋灵兽"仍异常。新 vita3k.log 显示**同一 undef_bc→svc 0x1 链再次发生**（新地址：LR=0x8108ee90 恰落在新构建 undef_bc 的 svc 指令上；模块 NID 0xE92EFCF7 也证明用户跑的是新构建）。
- **分发表权威复核结论（排除 jsr/ret 注册问题）**：Python 全量比对（TAGS=0/CPU_VARIANT=1 条件求值）enum 0..201 共 202 项，注册后 MISSING 仅 `[(186,'xxxunusedxxx')]`=保留值；WIDE 表 12 项齐全（含 ret）。**jsr/ret 注册确认无误**。真 opcode 值未出现在 vita3k.log——undef 打印走 fd 0x7 进设备侧 `ux0:/data/J2ME00001/midp_stderr.log`，用户尚未提供该文件。
- **根因（机制层面，与游戏启动 60ms 崩溃同一机制）**：undef_bc 原实现是 tty 打印 + BREAKPOINT。Vita 上 BREAKPOINT=SWI 1，Vita3K 报 "Import function for NID 0xE1A00001 not found" 后**继续执行**——undef_bc 尾声继续跑，pop 回坏 r4 返回分发循环，野指针崩溃远离第一现场。
- **修复**：Interpreter_c.cpp undef_bc 重写（commit a9b44c1，+38/-2）：新增 `undef_bc_stop(code,bcp)`——fprintf+tty 打印 opcode/bcp → `Universe::set_stopping()` → `BREAK_INTERPRETER_LOOP(JMP_STOPPED_MANUALLY)`（与 current_thread_to_primordial 相同的合法 longjmp 退出路径，primordial_to_current_thread 主循环 4593 行已有 JMP_STOPPED_MANUALLY 处理）。debug 打印开启时额外经 undef_bc_report 打方法名（Method::print_name_to 有 PRODUCT 门控，故包在 `#if !defined(PRODUCT) || USE_DEBUG_PRINTING` 内）。release 分支走 undef_bc_stop（无方法名但有 opcode/bcp）。
- **踩坑记录（重要，防再犯）**：
  1. `JVMUniverse`（handles/Universe.hpp）**没有** `stop()` 方法，只有 `set_stopping()`/`is_stopping()`——首次重编报 `'stop' is not a member of 'JVMUniverse'`。
  2. 编辑时误删旧 `#if !defined(PRODUCT) || USE_DEBUG_PRINTING` 的开启行留下孤儿 #endif → `unterminated #if` 波及 JarFileParser/Natives.hpp 假错误。**教训：#if 块改写要整体删旧换新，不能只换中段。**
  3. Interpret() 主循环 guard（查表命中 undef_bc 预拦截）已实现又回退：冗余且若 stop 返回会双重分发同一坏 opcode。undef_bc 自身停机足够。
  4. vita-port/build.sh 需要 JDK：`export JDK_DIR=/home/zyb/tools/jdk8u502-b07 && export PATH=$JDK_DIR/bin:$PATH`（javac 不在默认 PATH）。
  5. romgen 宿主链接报 BSDSocket.o wrong format 是已知无害噪音（VM 31 对象 OK + repack 完成即成功）。
- **验证**：rebuild_vm.sh build 31 对象 OK；库成员 _MergedSrc006.o 反汇编确认 undef_bc 控制流 fprintf→print_cr→longjmp、**无 svc**；新 velf（midp_vita.velf）undef_bc@0x8108eef8（旧构建 0x8108eea4），undef 区域 objdump svc 计数=0。VPK：`vita-port/build/cmake/midp_vita.vpk`（eboot 2417440B，17:02 生成）。
- **待办/下一步**：用户装新 VPK 后 (1) 若崩，预期 vita3k.log 出现干净退出而非野崩，且 midp_stderr.log 必有 `FATAL: Undefined bytecode 0xNN at bcp 0x…`——拿到 NN 即可定点修真 opcode；(2) 请用户提供 ux0:/data/J2ME00001/midp_stderr.log（Windows 侧 Vita3K 目录 `C:/Users/zyb/AppData/Roaming/Vita3K/Vita3K/ux0/data/J2ME00001/`）——当前已装构建的该文件里其实已有旧格式打印 `Undefined bytecode hit: 0x?? at 0x??`，是最快定凶途径。
- **遗留风险**：若坏流来自 JSR 子程序外的其它路径（如宽索引/坏 class），undef_bc 只是把野崩变成可控停机+诊断输出，不解决根因；真 opcode 定位后仍需定点修复。

### ⭐⭐⭐⭐ 游戏启动 60ms 崩溃根因闭合：undef_bc→svc 0x1 野崩溃链 + jsr/ret 无实现（2026-09-03，cldc 0e8c406）
- **症状**：03:46 全编译 VPK 打开游戏即卡死闪退；Vita3K 日志 `unknown NID 0xE1A00001`（~60ms，12:21:55.046）+ `EXCEPTION_ACCESS_VIOLATION Read 0xe3a03000`，PC=0x8109d85c（分发循环 `blx r3`），LR=0x8108ee10，r4=0x81093df0（.text 字节被当指针）
- **根因链（全链条二进制证据闭合，不依赖用户日志）**：
  1. C 解释器分发表 **256+256 槽唯缺 jsr(0xa8)/ret(0xa9)/jsr_w(0xc9)**（Bytecodes.hpp 枚举 vs DEF_BC 全量机械比对，其余全部已注册）
  2. 游戏 jar（2005 前后 javac target<1.5 编译）的 try/finally 产生 jsr/ret；ROM 化系统类已 jsr-free（midp_system.jar 744 类精确流式扫描：此前报的 FloatingDecimal 0xa8@134 是 **ldc2_w 操作数**、CalendarImpl 0xa9@7 是 **iflt 分支操作数**——两次字节扫描均为假阳性，教训：扫描必须线性解码不能裸找字节）
  3. 命中未注册槽 → `undef_bc()`（Interpreter_c.cpp:4154）打印 "Undefined bytecode hit" 后执行 `BREAKPOINT`
  4. **BREAKPOINT 在 Vita = SWI 1**（GlobalDefinitions_gcc.hpp:96 注释即此前 Vita 移植改动："0xe6000010 undefined 会崩，改用 0xEF000001"）——Vita3K 不停机、当未知 syscall 报 NID 0xE1A00001（svc 下一条 `mov r0,r1` 的编码 E1A00001）
  5. svc 返回后 `pop {r4,pc}` 恢复出坏栈 → r4=0x81093df0 → 分发循环把 .text 编码当 g_jpc/表项 → 野指针崩溃（两个变体 0xe3a03000=`mov r3,#0`、0xe3070c9c=`movw r0,#0x7c9c` 均对上）
  6. Hello.jar 不触达 jsr（其 opcode 集全覆盖）——所以 Hello 测试通过而游戏崩
- **修复**：Interpreter_c.cpp +34 行实现全部四个入口：`jsr`（PUSH((jint)(address_word)(g_jpc+3)) + branch(true)，与 goto 同 tick 路径）、`jsr_w`（PUSH(jpc+5)+GET_INT 跳转，对齐 goto_w 不加 check_timer_tick）、`ret` 窄形式（g_jpc=GET_LOCAL(GET_BYTE(0))）、wide `ret`（DEF_BC_WIDE→bc_impl_ret_wide，g_jpc=GET_LOCAL(GET_SHORT(0))——bc_impl_wide ADVANCE(1) 后入口 g_jpc 指向 ret opcode 本身，与 iinc_wide 语义一致）。returnAddress 是裸 jint 绝对 bci，**不用 OBJ_PUSH**（非 oop，GC 不追踪）
- **构建链**：rebuild_vm.sh 全量重编（31 对象全代际）→ libcldc_vm.a 21 成员 → vita-port/build.sh 链接打包。velf 终验：`bc_impl_jsr`@0x102092b3c（push 序列+revsh 分支+tick 调用）、`bc_impl_ret`（ldrb 操作数→GET_LOCAL→写 [r3,#0xb4]）、`bc_impl_jsr_w`（PUSH+rev 全字跳转）逐一反汇编正确
- **构建环境变化**：宿主 g++-11 已不存在，vita_arm.cfg 两处 FORCE_GCC 改为 `/usr/bin/g++-13 -B/usr/bin -m32`（g++-13-multilib 在位）——cfg 与 0e8c406 一起提交
- **路径修正（本轮实证）**：VPK 真正构建入口是 **`vita-port/build.sh`**（vita-port/CMakeLists 含 vm_rom_stubs.c 的 InitFPU/_rom_linkcheck 补符号 + 完整 Vita 移植层）；`midp-vita/build_midp_vita.sh` 是旧路线，其 CMake 链接会报 InitFPU/_rom_linkcheck undefined（vm_rom_stubs 不在其闭包），**不要再用它出包**
- **遗留**：①BREAKPOINT=svc 0x1 加固（改成安全停机）未做——jsr/ret 补齐后 undef 概率已极低，但未来任何 undef 命中仍会野崩 ②游戏 jar 实测待用户确认 ③milestone 崩溃微机制（svc 如何扰栈致 pop 出坏 r4）未完全证明，不影响修复

### ⭐ 事件泵死锁根治：Scheduler 门控放宽（2026-09-02，cldc 83ece23 / vita-port e711a0b，VPK 01.02 b153）
- **症状**：01.01 版（触屏+input_debug.log 日志版）进入游戏后"卡死"无输入响应，但 **input_debug.log 根本不出现**——心跳日志在 checkForSystemSignal 首行，没日志=事件泵从未被调用，这是决定性证据
- **根因（实锤）**：`Scheduler.cpp:653` wake_up_timed_out_sleepers 上游门控 `if (!is_slave_mode() && !Universe::scheduler_async()->is_null())`——主模式下**必须有 Java 线程阻塞在异步 native 调用**才驱动事件泵。我们的音频全同步返回（JAVACALL_MM_ASYNC_EXEC）→ 游戏运行期 scheduler_async 恒空 → checkForSystemSignal 永不执行 → 输入/触摸/媒体事件全部滞留 ring。这也解释了"修好音频同步后反而卡死"的矛盾
- **修复**：`#if defined(VITA)` 分支去掉 scheduler_async 判空，仅保留 `!is_slave_mode()` + `_timer_has_ticked || _estimated_event_readiness > 0`——事件泵改由 10ms timer tick 驱动（ticker 线程→real_time_tick→set_timer_tick→解释器 check_timer_tick→yield 链路已验证完整）
- **构建陷阱（本次最大教训）**：**不能只替换单个 .o 进旧库**。dist/lib/libcldc_vm.a 是 21 成员的既有 ABI 体系（JVM_TRAPS 宏在 PRODUCT 下展开为空、非 PRODUCT 下带 `Traps*` 参数——符号名都不同）。单换 MergedSrc002 必然与其他 20 个成员 ABI 不匹配，链接期缺 JVMScheduler::start/wait/notify 等一大批。正确做法：`rebuild_vm.sh`（VM_BUILD.md 配方）**全量 32 个 target/release 对象统一重编**再打包
- **rebuild_vm.sh EXCLUDE 修正（重要）**：脚本原版漏排 6 个"可执行专属"对象（BSDSocket/Main_vita/NativesTable/ROMImage/ReflectNatives/jvmspi——jvm.make LIB_OBJS 1030-1048 行明确 subst 掉的）。混入主库导致 Main_vita 的 main/module_start、ROMImage 与 MIDP 侧冲突、符号拉取链断裂（pte_osInit/ANI_Initialize 链接失败假象）。~~从 KNOWN_GOOD 基线重播种~~ **2026-09-03 起改为全代际重打包（见下方 03:46 记录）**
- **release flavor 与 jvm_natives_table**：release（非 PRODUCT）+ROMIZING 下 `Natives.cpp:1638` 会引用 `jvm_natives_table`（`#if (!ROMIZING)||(!PRODUCT)`），需把 `NativesTable.o`（纯数据表 R 符号+Java_* 外部引用，无副作用）加进主库；PRODUCT 下无此引用（旧库即 PRODUCT ABI 所以从没暴露）
- **验证**：最终 eboot `wake_up_timed_out_sleepers` @0x81072588 反汇编确认 is_slave_mode 检查后直接读 _timer_has_ticked([r5,#340])/_estimated_event_readiness([r3,#1580/1588])，**无 scheduler_async 判空**；库 21+1 成员、jvm_f2i 浮点 stub 在位；VPK 14530343 字节、版本串 `J2ME Player v01.02 b152 (e711a0b)`
- **测试预期**：装 01.02 后进游戏，`ux0:/data/J2ME00001/input_debug.log` 应出现（[HEARTBEAT] 每 1024 次 checkForSystemSignal + [INPUT] 每次按键变化）——文件出现即事件泵复活；触屏（Vita3K 鼠标点击）走 MIDP_PEN_EVENT 路由到 Canvas.pointerPressed/Released

### ⭐⭐ Thumb/ARM 混编崩溃：rebuild_vm.sh 丢 -marm（2026-09-03，VPK 01.02 b153 修复版）
- **症状**：装上 rebuild_vm.sh 全量重编的 01.02 进游戏即崩。Vita3K 日志：`Undefined instruction at 0x810C0E88, instruction 0x1AFFFFFA` + `Thumb: true` + **同一 PC 无限循环、SP 每轮递减 0x14（无限递归栈下溢）**——与 b139 事故特征同款
- **根因（实锤）**：`rebuild_vm.sh` 的命令行三清 `CPP_DEF_FLAGS=` 把 cfg 注入的 `-marm` 架构标志一并清掉 → 32 个新 C++ 对象全部编成 **Thumb-2**（`f245/b08b/b9a3` 编码），而老库 `Interpreter_arm.o` 是 **ARM 汇编**。崩溃链：新 Thumb `adjust_heap_size`（_MergedSrc003）Thumb `bl fast_memclear` 不切模式 → 跳进 ARM 汇编 → ARM 编码被 Thumb 状态解码 → 未定义指令。LR=0x810861cb 正是 `bl fast_memclear` 调用点
- **误判提示**：崩溃点指令 `1afffffa` 本身是合法 ARM `bne`（链表遍历循环）——"指令合法但 Thumb:true 执行"就是模式错乱指纹，别往坏数据/GP 表方向查
- **正确配方（已被 03:46 全编译路线取代，保留下述宏结论）**：编译命令**必须显式带 `-O2 -DNDEBUG -DPRODUCT -DROMIZING=1 -marm -march=armv7-a -mtune=cortex-a9 -mfloat-abi=hard -mfpu=vfpv3`**（PRODUCT 宏给出无参 JVM_TRAPS ABI + 无 jvm_natives_table 引用，-marm 保证 ARM 编码）。注意：任何路线下这些标志都不能丢——GNU make 命令行变量会压制 makefile 内 `+=`，`CPP_DEF_FLAGS=` 一旦传空，cfg 里 vita_arm.cfg 232-254 行的全部 `CPP_DEF_FLAGS +=`（架构+特性宏）都被清掉
- **入库前必须验证四项**：①反汇编为纯 ARM 编码（e3xx/e59x/e1a 开头，无 f2xx Thumb 前缀）②符号为无参版（`wake_up_timed_out_sleepersEv`，非 `EP8JVMTraps`）③`nm | grep -c jvm_natives_table` = 0（PRODUCT 生效）④`wake_up_timed_out_sleepers` 反汇编无 scheduler_async 门控（VITA 修复在位）
- **CMake 陷阱**：CMake 不把 libcldc_vm.a 当文件依赖——**换库后必须 `rm -f build/midp_vita midp_vita.velf midp_vita.self midp_vita.vpk` 强制重链**，否则只是用旧 self 重打包 VPK（本次 16:47 的 VPK 就是这么混过去的）
- **最终验证**：新 eboot `wake_up_timed_out_sleepers` @0x81071e70 纯 ARM 编码；`adjust_heap_size → fast_memclear` 为 ARM `bl`（eb00ce87）模式一致；版本串 v01.02 b152；HEARTBEAT/checkForSystemSignal/sceTouchPeek 全在位；VPK 14286958 字节（02:24）
- **VM_BUILD.md 待补**：三清中 `CPP_DEF_FLAGS=` 只是为了清 romgen 分支的 `-B/usr/bin -m32 -DCROSS_GENERATOR=1`，但 make 层面 cfg 的 -marm 也在 CPP_DEF_FLAGS 里——**用 rebuild_vm.sh 后新对象是 Thumb，与老库 ARM 成员不兼容**。全量重编路线必须同时验证 `-marm` 在编译命令中（grep 干跑输出）；单对象替换路线用上面的显式命令

### ⭐⭐⭐ 库混代崩溃 + 全编译终局修复：rebuild_vm.sh 两缺陷（2026-09-03 03:46 VPK，验证通过待真机/模拟器实测）
- **症状**：02:24 的 v01.02 修复版进游戏仍崩，Vita3K 日志 `EXCEPTION_ACCESS_VIOLATION, Read violation at 0x13A2C499`，PC=0x8108e6d8（JVMMethod::name()），LR=0x8107c014（Throwable::fillInStackTrace 遍历调用栈）。**注意 Thumb 已不是问题**（日志 `Thumb: false`）——崩溃性质从"指令模式错乱"变成"读野指针"
- **根因 A（库混代，实锤）**：上一版 pack_lib 从 `/tmp/libcldc_vm.a.KNOWN_GOOD`（Sep 2 基线）重播种 20 个旧对象、只换新 `_MergedSrc002.o` → 库内同名成员字节不同、大小差 2 倍级（旧基线混装配产物）→ 异常路径 fillInStackTrace 遍栈时 method/constants oop 为垃圾（0x13a2c499）→ 崩。**触发条件是 Java 异常抛出**，所以此前正常路径的测试全通过、一进游戏就崩
- **根因 B（编译标志丢失）**：`CPP_DEF_FLAGS=` 命令行赋值压制 cfg 的全部 `CPP_DEF_FLAGS +=` → 丢 `-marm/-march/-mfloat-abi=hard/-DPRODUCT/-DARM/-DVITA` 等全套 → FLAVOR=release 本身不产生 `-DPRODUCT`（jvm.make 833-835：`CPP_DEF_FLAGS_release=` 为空、product 才有 `-DPRODUCT`）
- **修复（rebuild_vm.sh，+44/-3 行）**：①build_target() 新增 `CPP_DEF_FLAGS_TARGET`（完整 17 项标志：-DPRODUCT -DROMIZING=1 -DARM -DVITA -D__PSP2__ -Wno-narrowing -fpermissive -marm -march=armv7-a -mtune=cortex-a9 -mfloat-abi=hard -mfpu=vfpv3 -DSUPPORTS_*×4 -DUSE_VM_EXCEPTIONS=0 -DUSE_BSD_SOCKET=1），make 显式传入 ②pack_lib() **废除 KNOWN_GOOD 重播种**，改为全代际重打包：所有 target 对象逐个 `ar r`；`Interpreter_arm.o` 沿用旧库成员（regenerated .s 丢了 jvm_f2i/jvm_d2i 浮点 stub，且其内嵌 GP 全局符号正是链接期所需）③EXCLUDE 清单不变（12 个可执行专属对象）
- **全编译验证（本轮"就该全编译"结论的实证）**：31 个 target/release 对象全新生成（03:28）——**全部纯 ARM**（逐对象统计 0 条 16-bit、共 113753 条 32-bit 指令）、**全部 0 debug-ABI 痕迹**（无 `Traps`/`EP8JVMTraps`）；主库 1169032 字节、21 成员全代际一致
- **强制重链**：删 midp_vita/velf/self/vpk 四产物 → cmake --build 重链 → VPK 14305891 字节（03:46）
- **eboot 终验（nm 地址 0x102… 与 objdump VMA 0x810… 换算：VMA = nm − 0x81000000）**：①Thumb=0 ②jvm_f2i@0x810a60fc / jvm_d2i@0x810a6154 在位 ③GP 全局在位（_old_generation_end@0x1027019d4、_task_class_init_marker@0x102701bfc 等，由旧代 Interpreter_arm.o 供给——GPSkeleton.cpp 那套只在 ROM 生成器里链入）④修复版 `wake_up_timed_out_sleepers`@VMA 0x81062308（0x19C 字节）全部 6 个 bl 解析确认：JVMScheduler::start+0x98、Protocol::writeByte+0xac（编译器布局巧合，非语义调用）、JVMOs::sleep、add_sync_thread、JVMSynchronizer::enter、gc_epilogue——函数体特征与 09-02 门控放宽修复版一致
- **产物**：`vita-port/build/midp_vita.vpk`（14305891B, 03:46）；`phoneme-cldc/build/vita_arm/dist/lib/libcldc_vm.a`（1169032B, 03:28，21 成员全代际）；`.bak` 为旧混代库勿再使用
- **构建期链接报错（预期，无害）**：rebuild_vm.sh 末尾 exe 链接 crt0 失败 `|| true`，38 个 undefined reference（_task_class_init_marker/_old_generation_end/_primordial_sp 等）本就由链接期 Interpreter_arm.o/vita-port 供给，属脚本设计内行为
- **遗留风险**：异常路径（fillInStackTrace 深栈遍历）在本库代际下是**首次完整实测**；若仍崩，下一步查①事件投递真空（mastermode checkForSystemSignal 空 stub 时机）②C interpreter 帧布局与 oop 遍历一致性

### 启动即崩真因：e_entry 被 patch 成 _start，破坏 SCE module_info 定位器（2026-09-02）
- **症状**：新 VPK 打开即卡死、Vita3K 闪退。日志：`Loaded module segment 0/1`（尺寸与 velf 完全一致）→ `Linking SELF app0:eboot.bin` → `eboot.bin module NID: 0xAC3E25AB` → **同毫秒** `EXCEPTION_ACCESS_VIOLATION, Read violation at 0x4A6F84D2F`
- **根因（实锤）**：SCE ELF 的 `e_entry` **不是程序入口**，是 `sce_module_info` 定位器：`(段号<<30) | 段内偏移`（Vita3K `load_self.cpp:492` `module_info_offset = e_entry & 0x3fffffff`、`:689` `module_info_segment_index = e_entry >> 30`；真正的程序入口在 `module_info->module_start`）。vita-elf-create 生成 `e_entry=0x364740` 本来就正确——精确指向 `.sceModuleInfo.rodata`（0x81364740 = 0x81000000+0x364740）。而 `tools/patch_velf_entry.py`（b141 引入）把它改写成 `_start` 偏移 0x6e9 → Vita3K 把段0+0x6e9 处的 .text 机器码当 `sce_module_info_raw` 解析，`import_top/import_end` 全是代码垃圾 → `load_imports` 的 `for (imports=begin; imports<end; imports+=imports->size)` 走野指针 → 读违规闪退
- **误诊链条（教训）**：入口补丁源于"JVM 秒退/入口错误"误判，此后每次构建症状变化都被归因到 entry，一路强化错误修复；`2f578a6` 的 whole-archive ani 同样基于"ANI_Initialize 链接失败"误诊（实测普通 `-lcldc_vm_ani` 完全能解析）
- **修复**：①删除 CMakeLists 的 entry patch POST_BUILD（CMake 留注释说明 SCE e_entry 语义防再犯）②`patch_velf_entry.py` 改名 `.DANGEROUS-DISABLED` ③撤掉 whole-archive ani。b148 的 pte_osInit 保留（无害且有防御价值）
- **排查中排除项（以后别再查一遍）**：reloc 表完整（0x11004c 字节按 SCE 格式精确走完+4 pad）；`.init_array` 4 个构造（register_fini/pthread_setup/frame_dummy/JVMThrow GLOBAL__sub_I）均安全——`pthread_init` 自带守卫幂等调 `pte_osInit`；ani 成员无 init_array；RW 段 1088KB（含新增 jts events 48KB）不是问题；`vita-elf-create` 输出确定性；`vita-make-fself` 输出含时间戳非确定（仅元数据）
- **验证**：`make` 全绿；产物 velf `e_entry=0x364740` 与 `.sceModuleInfo` 对齐；`ANI_Initialize`/`PoolThread_InitializePool`/`javanotify_on_media_notification`/`jts_expand`/`checkForSystemSignal` 全部在位
- **方法论**：崩溃在 loader 阶段（"Linking SELF" 后同毫秒）⇒ 查 ELF/SCE 结构与链接脚本，别查应用代码；Vita3K 源码（load_self.cpp/relocation.cpp）是 loader 崩溃第一手资料

### 启动闪退修复：pte_os pthread 初始化缺失（2026-09-02，samples 93372ca，VPK b148）
- **症状**：启动闪退，Vita3K 日志刷 `Invalid write at 0x4` + `pte_osAtomicExchange` + `pthread_mutex_unlock`——newlib FILE 锁（freopen/printf）触发 pthread_mutex_unlock → pthread_self() → pte_os 线程跟踪未初始化 → NULL+4 崩溃
- **根因**：vitasdk pthread 模拟层（pte_os）需要至少一次 pthread 调用初始化内部线程跟踪。旧 libcldc_vm.a 某些代码隐式触发了初始化；VM 重编去转储后不再调用，首次 pthread_mutex_unlock 即崩
- **修复**：vita_main.c 的 main() 最前面加 `PTHREAD_MUTEX_INITIALIZER` 的 lock/unlock 周期。**以后新增任何 pthread 使用时都不能删除这段初始化**

### ⭐ VM 编译权威文档（2026-09-02，samples 0ff302d）
**改 VM 源码（phoneme-cldc/src/**）后：先读 `VM_BUILD.md`、用 `rebuild_vm.sh`**——完整配方（宿主工具/ARM 目标/打包三阶段）+ 十条陷阱表已固化。要点：命令行三清（FORCE_GCC=/GNU_TOOLS_DIR=/CPP_DEF_FLAGS=）+ `_release` 子目标 + AsmStubs 预置 + Interpreter_arm.o 用旧库成员 + ani.o 等 anilib 文件不得混入主库 + ar r 后逐个验证成员非空。

### ANI 链接回归修复 + 入口补丁恢复（2026-09-02，samples 63015e9，VPK b146）
- **ANI_Initialize 链接失败根因**：重造的 `libcldc_vm_ani.a` symbol index 损坏/过期——ld 扫描 archive 拉入 os_port.o 但不拉 ani.o（定义 ANI_Initialize 的成员）。ranlib 修 index 后最小链接过了但完整链接仍失败（不明 archive 边角案例）
- **解法**：PRE_LINK 提取 ani.o/ani_bsd_socket.o/os_port.o/poolthread.o 四个对象，以普通对象直接链入（对 archive 边角免疫）；从 group 移除 `-lcldc_vm_ani`
- **入口补丁恢复**：`tools/patch_velf_entry.py` POST_BUILD 把 velf e_entry 从 0x395520（无关函数）修回 _start（0x1471，**Thumb 位保留**）。此前 b143 闪退=Thumb 位被 strip，b141 秒退=入口指向普通函数
- **VM_BUILD.md 陷阱 11 补充**：链接期症状 jvm_f2i undefined=库成员坏；运行期症状 Undefined instruction + GP 表地址=坏 GP 表成员；秒退=入口指向普通函数；闪退=Thumb 位丢失

### 音乐开关"卡死"根治：VM 重编去转储（2026-09-02，cldc 2959a08 / samples 357f014，VPK b137）
- **真相**：音乐开关触发 MMAPI 类链**首次加载**（音频集成前这些类 CNF 根本不走加载）→ VM 三处调试转储（ClassFileParser 的 CP dump、ConstantPoolDesc 的 var_oops_do、Universe 的 hidden 警告）每类倾倒数千行 stderr → Vita3K 慢速 I/O 下几分钟出不来 = "卡死"；17403 行日志零异常。fd 0x7 洪水、suite 反复 startSuite 都是伴生现象
- **修复**：全部转储 gate 在 `VITA_CP_DEBUG` 后（默认关）；libcldc_vm.a 重编（eboot 中三组格式串清零）
- **VM 重编配方（血泪总结，cldc commit 2959a08 message 有完整版）**：① loopgen/romgen 按 cfg 原设置编 ② target 用 `IsLoopGen=true LOOP_GENERATOR_DIR=... FORCE_GCC= GNU_TOOLS_DIR=... CPP_DEF_FLAGS=（命令行三清）... _release` + JVMWorkSpace/JVMBuildSpace/JDK_DIR/TOOLS_DIR 环境变量 ③ AsmStubs_x86_64.o 从 romgen/app 预置 touch 旧 ④ **release flavor**（debug 引 AZZERT 符号、product 缺编译器成员）⑤ Interpreter_arm.o 用旧库成员（重生成 .s 丢浮点 stub）⑥ ar r 后必须 ar p 验证成员非空（&&链会静默断）⑦ 37 个 `_rom_check_*` 由 vita-port `src/vm_rom_stubs.c` 补
- **Interpreter_c.cpp:1506**：`invoke_native_entry(..., NULL, ...)` → `va_list()`（ARM EABI va_list 是结构体，NULL 只在 32 位 x86 可编）

### device://tone Player 全链路打通：tone 状态机 + MMAPI 事件桥（2026-09-01）
- **背景（"开启音乐"冻结根因）**：游戏 `Manager.createPlayer("device://tone")` → `javacall_media_create` 对 tone URI 返回 INVALID_ARGUMENT → `nInit` 抛 `MediaException("Unable to create native player")` → 主线程异常/重试 → UI 冻结。此前 vita_audio_javacall.c 只有 playTone 快捷路径，无 Player 状态机
- **`vita_audio_javacall.c`（+681 行）实现完整 tone Player 状态机**（create/realize/prefetch/start/stop/resume/close/destroy + get/set_volume/mute + JTS 缓冲与播放）：
  - `create` 按 UTF-16 精确匹配 `device://tone`，命中则 `javacall_malloc` 分配 `vita_player`（~32KB，内含 32KB JTS 缓冲），未命中返回 INVALID_ARGUMENT
  - **`get_format` 返回 UNKNOWN 是关键设计**：Java 侧 `HighLevelPlayer` 构造器对 UNKNOWN+`TONE_DEVICE_LOCATOR` 自动映射为 DEVICE_TONE 且 `handledByDevice=true`（HighLevelPlayer.java:291-294），跳过 source.connect/download；若误返回 TONE 会走 MediaDownload 路径
  - **全部 mandatory API 同步返回 OK**：CLDC green-thread VM 下 KNI 阻塞会冻结全部 Java 线程；`JAVACALL_MM_ASYNC_EXEC` 宏（mm_async_exec.h）在同步 OK 时直接返回，不设 reentry、不阻塞
  - JTS 播放：`start` 把 `g_jts_req=pl; g_jts_seq++` 交给常驻 `j2me_tone` 线程（复用 playTone 线程），线程内 `jts_expand` 展开为扁平事件表 → 逐事件 `generate_tone_pcm` 方波合成（16k 单声道）→ 3 倍重采样 → SceAudioOut；无 JTS 数据时立即发 END_OF_MEDIA
  - **REPEAT 语义按规范实现（4 字节 `REPEAT multiplier tone_event`）**：校验器（jts_check_sequence）、pass1 定位（pos+=4）、pass2 展开（读紧随其后的 note/dur 发射 multiplier 次）、PLAY_BLOCK 块体（块尾扫描 +4 偏移、块内 REPEAT 展开）四处一致。**上游 win32 mmtone.c 校验器只消费 2 字节、播放循环把 REPEAT 落 default 当 note 播——是 win32 bug，勿效仿**；权威依据 ToneControl.java（`repeat_event = REPEAT multiplier tone_event`）+ DirectTone.jpp 校验器（pos+=4）
  - END_OF_MEDIA 的 data 单位是**秒**（Java 侧 MMEventListener `intParam2 * 1000` 转 ms），native 发 `(void*)(long)(duration_ms/1000)`
- **新建 `vita_media_notify.c`（~90 行，javanotify 事件桥）**：`javanotify_on_media_notification` 在 vita_arm 构建中无实现（唯一上游实现 javanotify_midp_jsr.c 属 javacall_application 子系统，不参与链接；midp_jc_event_send 管线也不在闭包）。仿 vita_input.c 模式：SPSC ring（16 个 MidpEvent，满丢最旧）→ `vita_media_poll`；字段映射与上游 midp_slavemode_javacall.c 对 MIDP_JC_EVENT_MULTIMEDIA 的处理一致（MM_PLAYER_ID/MM_DATA/MM_ISOLATE/MM_EVT_TYPE/MM_EVT_STATUS，MMAPI_EVENT=45）
- **`vita_input.c` checkForSystemSignal 排水媒体事件**：`vita_media_poll` 命中则 `waitingFor = MEDIA_EVENT_SIGNAL` 返回（媒体事件优先于输入事件，防 END_OF_MEDIA 饿死）；未命中再走原有 UI ring
- **事件链闭环**：native `javanotify_on_media_notification` → MidpEvent 入 ring → VM 线程 `checkForSystemSignal` 排水 → `midp_master_mode_events.c` `StoreMIDPEventInVmThread` + `eventUnblockJavaThread` → Java `MMEventListener` 收 END_OF_MEDIA/STARTED/STOPPED → `PlayerListener` 回调正常派发，游戏状态机不再卡在等待
- **编译验证**：`vita-port/build && make` 全绿，`[100%] Built target midp_vita.vpk-vpk`；改动仅 vita-port 三文件 + 新文件，符合最小 diff
- **已知边缘限制**：PLAY_BLOCK 嵌套 PLAY_BLOCK 不支持（上游校验器同样禁止）；REPEAT multiplier 上限钳到 JTS_MAX_REPEAT(8)（规范上限 127，防事件表溢出）；JTS 序列上限 32768 字节（`nGetJavaBufferSize` 返回值，DirectTone.setSequence 一次性整段传输）

### WMA/SMS(JSR 120) 接入 Vita（2026-09-01）
- **目标**：让 `sms://`、`cbs://` 协议被 GCF 识别（`Connector.open("sms://:1234")` 不再抛 InvalidArgumentException；WMA API 类进 ROM）
- **构建接线（核心发现）**：`build_vita.sh` 用命令行覆盖一切 makefile 赋值——启用 JSR 只需在它里面加 `USE_JSR_120=true JSR_120_DIR=/home/zyb/vitasdk/samples/j2me/phoneme_source/phoneME/jsr120`（jsr120 在 phoneme_source 里，同 jsr135 模式）。jsr120 的 `common_defs.gmk` 会强制 `USE_ABSTRACTIONS=true`，而 `build_vita.sh` 本来就传了 `USE_ABSTRACTIONS=true + ABSTRACTIONS_DIR`，刚好满足
- **首编 60 个 javac 错误的真相**：不是源码问题——`alljavalist.txt` 是增量文件列表（appendfiles 写 `$?`，即比 classes.zip 新的文件），jsr120 源码时间戳（8/25）早于旧 classes.zip（8/31）导致只有 4 个 JPP 生成文件（其规则让其必"新"）被编译。**修复：`touch` jsr120 全部 .java/.jpp + 删 classes.zip**
- **javacall_sms_* 符号缺失**：native 层 `smsProtocol_javacall.c` 调 `javacall_sms_*`/`javacall_cbs_*`/`javacall_strdup`，无实现则 `libmidp.so` 链接失败。新增 `vita-port/src/vita_sms_javacall.c`（stub：send/listen 返回 FAIL，`get_number_of_segments` 按 GSM 03.40 真实计算，strdup 用 javacall_malloc）
- **把 Vita 源文件编进 phoneME 构建的通用机制（本项目原创）**：`build/vita_arm/vita_overlay.mk`（vpath % → vita-port/src；JTWI_NATIVE_FILES += vita_sms_javacall.c），经 `export MAKEFILES=<overlay>` 在 `build_vita.sh` 里注入。GNU make 的 MAKEFILES 预加载文件可追加 vpath 与变量，**零修改上游 gmk**，符合最小 diff 原则
- **验证方法**：① `jar -tf classes.zip | grep wireless` 显示 5 个接口类 ② `nativeFunctionTable.cpp` 有 `Java_com_sun_midp_io_j2me_sms_Protocol_*` 全部 7 个 KNI ③ `ROMImage_01.cpp` 中 grep 到 romize 后的 sms native 指针 ④ `libmidp.so` 无 undefined reference
- **运行时行为**：`sms://` 连接可创建；发短信返回失败（Vita 无蜂窝）；收听端口注册失败。游戏内 SMS 功能会优雅降级而非崩溃
- 注意：`vita_overlay.mk` 在 `phoneme-midp/build/vita_arm/`（生成目录，重建不丢）；若频繁 clean build 建议移到 `midp-vita/` 并调整 build_vita.sh 路径

### 游戏卡死真因：VM 每类 stderr 转储洪水（2026-09-01，samples 27d7ef0）
- **症状**：游戏"卡死不动"且无异常——实为 `ClassFileParser.cpp` 里遗留的两段调试转储（`CP tags`/`PRE idx`/`Utf8 idx ... NOT in heap`，诊断 UTF8/hidden 问题时加的）**每加载一个类向 stderr 倾倒几百行**（小游戏 17k 行），Vita3K 模拟 sceIoWrite 慢，I/O 洪水即"卡死"。Vita3K 日志特征：`sceIoWrite: fd 0x7, size: 5x` 高频重复
- **即时缓解**：vita_main.c 把 stderr 重定向 /dev/null（freopen 失败则 `sceIoClose(2)` 走廉价 EBADF）。Java 输出走 `JVMSPI_PrintRaw` 自己的 fd（vm_output.log）不受影响
- **正确修复（已提交待生效）**：cldc commit `291ab15` 用 `VITA_CP_DEBUG` 宏 gate 掉 dump + 静音 Universe 的每类 hidden 警告——**待 VM 重编环境恢复后重编生效**
- **⚠️ CLDC VM 重编配方问题（未解决）**：完整 `make debug` 会失败——romgen 分支的 `CPP_DEF_FLAGS += -B/usr/bin -m32 ...` 与 `-DCROSS_GENERATOR=1` 泄漏进 target 构建上下文（root.make 的 tools/loopgen/_romgen/_debug 多阶段在同一 make 里共享变量，且 jvm.make 1462 行 `FORCE_GCC` 非空时所有编译角色都被替换）。曾试：`IsTarget=true`+`FORCE_GCC=`+`GNU_TOOLS_DIR=`+`CPP_DEF_FLAGS=` 命令行覆盖→卡在 AsmStubs_x86_64.s / Interpreter_c.cpp（解释器兼容）。重编前需理清 root.make 的多阶段变量流
- **版本号陷阱**：GenVersion 取 HEAD commit，先改代码后 commit 会导致 VPK 版本串不反映二进制内容——**务必先 commit 再 build**

### 游戏卡死修复：playTone 同步阻塞冻结 VM（2026-09-01，samples 64652d4）
- **症状**：ANI 修复后的 VPK，游戏运行中卡死不动、无任何异常日志
- **根因**：`javacall_media_play_tone` 原实现是**同步阻塞**——生成 PCM 后逐块 `sceAudioOutOutput` 播完整段才返回。CLDC 是绿色线程模型，nPlayTone 阻塞 = 所有 Java 线程冻结。游戏主循环高频调 tone（按键音/BGM 序列），表现为卡死
- **修复**：专职播放线程（`sceKernelCreateThread`，原生线程；libpthread 链接有玄学问题不用）+ 无锁序列号握手：VM 线程只写 note/dur/vol 并 `g_req_seq++` 即返回；播放线程 4ms 轮询，播新前比对 seq 实现抢占，`stop_tone` 置标志中断输出；audio port 由播放线程打开消除竞态
- **下一嫌疑（未实证）**：若修复后仍"启动即永久卡死"，则指向 http→socket 挂起：`CommonDS` 对 `http:` locator 阻塞式 `Connector.open`+`getResponseCode`，而 Vita 的 pcsl 网络是 53 字节 stub（`pcsl/vita_arm/lib/libpcsl_network.c`）。届时可二分验证（临时禁 http 子系统重编 ROM）

### 游戏启动崩溃修复：ANI 线程池未初始化（2026-09-01，samples 152d350）
- **症状**：jsr135 集成后游戏启动即死循环，Vita3K 日志刷 `Invalid read of uint32_t at 0x4`，`ldrex/strex` 原子指令重试，PC=0x810aca7a
- **根因链**：MMAPI 可用后游戏 `Manager.createPlayer(http://...)` 走通 → 媒体下载进 ANI 阻塞框架 → `PoolThread` 的静态事件（全局零初始化=NULL）被 `Os_SignalEvent/Os_WaitForEvent` 使用 → `pthread_mutex_unlock(&NULL->mutex)`（偏移 4）→ pteos 原子操作死循环。**`ANI_Initialize()` 在 CLDC-HI 启动流程中无任何调用者（上游同样），线程池从未初始化**
- **修复**：vita_main.c 在 `runMidlet` 前显式调 `ANI_Initialize()`（符号未修饰，C 可直接链接）
- **诊断方法论**：Vita3K 的 PC/LR → `arm-vita-eabi-addr2line -e build/midp_vita -f` 定位到函数 → objdump 全量反汇编 + grep `bl <目标地址>` 枚举调用点 → addr2line 每个调用点还原调用图。这套路子可复用

### 中文字库扩容 + 自描述 header（2026-09-01，samples b593d80/a9c1049）
- fontgen 新增 4 段：带圈数字 0x2460、希腊 0x0386、CJK 尾段 0x9FA6-0x9FFF、半角片假名 0xFF61（共 21429 字形，1.41MB）
- bank header 升级为自描述（magic "J2FB" v1：nsec/gw/gh/stride/data_off/段表），**vita_font.c 加载时动态解析并校验**——以后扩段只改 fontgen.c，运行时零改动
- **构建资产已入库**：`config/font.ttf`（16MB，fontgen 唯一输入，此前被顶层 `*.bin`/缺失规则挡在库外！）和 `config/fontbitmap.bin`；tools/font.ttf 重复副本已删；重生成用 `tools/gen_font.sh`（必须 `/usr/bin/gcc-11 -B/usr/bin`，PATH 里的 vitasdk gcc 是 ARM 交叉工具）
- Vita 端已知空缺：半角 ｱ(U+FF71) 等个别字形该字体缺失（62/63 覆盖）；CJK Ext-A（0x3400 段）未纳入，需要时在 fontgen.c 加段即可

### 音频/MMAPI 集成（2026-09-01，samples b29d725 / phoneme-midp 9240db2 / phoneme-cldc 4000337）
- **JSR-135 已进构建**：`build_vita.sh` 开 `USE_JSR_135=true`；javax.microedition.media + com.sun.mmedia 共 228 类已 ROM 化（ROMLog 3442 处引用）；jsr135 KNI 对象（31 个）进 `obj/arm/libobj.a`；nativeFunctionTable 含 nPlayTone 等 247 条
- **javacall 层**：`vita_audio_javacall.c` 目前只实现 playTone（SceAudioOut 合成方波）+ 24 个符号；头文件声明的 87 个 media 函数中 ~63 个未实现（realize/prefetch/start 等 DirectPlayer 路径）——但链接零错误，因当前 caps 只声明 tone，Java 层不会走未实现路径。**WAV 文件播放是下一迭代**
- **工具链事故与修复**：系统 g++-13 被卸载 → 8/29 的 loopgen/romgen 二进制要求 GLIBC_2.38 全废。修复：cfg 改 `FORCE_GCC = /usr/bin/g++-11 -B/usr/bin -m32`；另发现 romgen/loopgen 的 vpath 错拿 MIDP 的 ROMImage.cpp（应链 ROMSkeleton stub），解法是把 Skeleton 副本放到 `romgen/app/`、`loopgen/app/` 下（当前目录优先于 vpath）。**若重编宿主工具，先确认这两个目录里的 ROMImage.cpp 副本还在**
- **构建陷阱记录**：①`unzip` 从 WSL 环境消失导致 zip 内容检查假阴性（用 python zipfile）；②MIDP java 编译按 alljavalist.txt 增量（mtime），上游 jsr135 文件 mtime 老会被跳过——启用新子系统后要删 `tmpclasses/`+`classes.zip` 强制全量；③顶层 `GNUmakefile` 的 build 输出是 `classes.zip`（不是 midp_classes.zip）

### VPK 图标与版本体系（2026-08-31，samples commit 10b4fac）
- LiveArea 资源在 `vita-port/assets/sce_sys/`（icon0 128x128 / bg 840x500 / startup 280x158 / template.xml），由 `vita-port/tools/gen_assets.py`（Pillow）再生成
- 版本自动生成：`vita-port/cmake/GenVersion.cmake` 构建时产出 `vita_version.h`：APP_VER（手动，CMake `VITA_VERSION`）+ 构建号（samples 仓库 commit 数）+ 短哈希，形如 `J2ME Player v01.00 b120 (0fc9e8d)`
- 版本显示在菜单标题栏，并写进 boot_log.txt 首行——**从截图或日志即可定位用户装的是哪个构建**
- 迭代规则：功能里程碑手动升 `VITA_VERSION`（如 01.00→01.01）；其余改动构建号自动递增，无需维护

### 仓库基线整理（2026-08-31）
- `phoneme-cldc` commit `98b12b5`：103 个修改的上游文件 + 23 个新增（`src/vm/os/vita/`、`src/anilib/vita/`、`AsmStubs_x86_64.s`、`build/vita_arm/{Makefile,vita_arm.cfg,vita_arm.mk,embed_midp.py}`、`.gitignore`）
- `phoneme-midp` commit `8a04358`：16 个修改的 gmk/native + 50 个新增（`src/porting/vita/`、各 `vita_arm/` 平台目录、顶层 `GNUmakefile`/`build_vita.sh`/`PORTING.md`、`build/vita_arm` 平台 makefile、`.gitignore`）
- `.gitignore` 策略：`build/vita_arm/*` 全忽略 + 白名单放行手写配置（ROMImage_*.cpp、ROMImageGenerated.hpp、classes/、obj/、pcsl/、*.o、*.a 均为产物）；忽略便利符号链接（cldc 的 `src/midp`、midp 的 `src/src`）
- 已删除垃圾：`Main.java.bak`、`Defs.gmk.original`、`null.o`、`libcldc_vm_midp_g.a.backup_*`
- 之后判断"我改了什么"直接 `git diff 98b12b5`（cldc）/ `git diff 8a04358`（midp）

### vita-port/src/vita_font.c — 文字全空白的最终根因（commit 0fc9e8d）
- **症状**：font_debug.log 中所有 draw 调用 `pixels_drawn=0`，连屏幕中央的 CJK 都是 0；字母和汉字全都不显示。
- **根因**：多轮改写后，`fb_load()` 函数定义存在但**全文件无任何调用点**，`fb_ready` 永远为 0，`fb_glyph()` 永远返回 NULL → ASCII 走"未找到且 <0x2E80"直接不画，CJK 只画豆腐框。
- **修复**：新增 `fb_ensure()` 懒加载（先 `ux0:/data/J2ME00001/fontbitmap.bin` 覆盖，再 `app0:/data/J2ME00001/fontbitmap.bin` VPK 内副本），在 gxjport 三个入口（get_font_info / get_chars_width / draw_chars）调用。
- **验证结论（PC 端）**：fontbitmap.bin 1,397,920 字节布局正确：header 40B + 5 段（ASCII 95 / CJK 20902 / punct 64 / full 95 / gen 24）× 66B 字形，CJK 20902 个字形全部非零。
- **教训**：加日志必须让日志能区分各分支（豆腐框分支此前不计数，导致无法区分"画了豆腐"和"根本没画"）；per-call 日志要节流（此前 7000 行重复）。

### Universe.cpp
```c
#if defined(VITA) || defined(VITA_SDK)
    if (!jvm_class()->access_flags().is_hidden()) {
        fprintf(stderr, "[VITA] Warning: JVM class not marked as hidden, skipping check\n");
    }
#else
    if (!jvm_class()->access_flags().is_hidden()) {
        JVM_FATAL(jvm_class_must_be_hidden);
        return false;
    }
#endif
```

### main.c
```c
char *midp_args[] = {
    "runMidlet",
    "-classpathext",
    "ux0:/data/J2ME00001/midp_system.jar:ux0:/data/J2ME00001/Hello.jar",
    "ux0:/data/J2ME00001/Hello.jar",
    "HelloMIDlet"
};
```

### midp_run.c (getClassPathPlus)
支持 `:` 分隔的多个 JAR 路径

### vita_arm.cfg (启用 UseROM)
```makefile
# Enable ROM (loads system classes from ROMImage.o, embedded in VPK)
# This eliminates the need for midp_system.jar on filesystem
export ENABLE_ROM_GENERATOR := true
export ENABLE_ROM_GENERATOR__BY := vita_arm.cfg
export ROMIZING := 1
export USE_ROM := 1
```

### build_midp_vita.step1_5_rom
ROM jar 现在包含:
- CLDC classes
- MIDP classes (com/, java/, javax/)
- midp_system.jar 内容
- Hello.jar 内容

---

## 调试日志

Vita3K 日志位置:
- `C:/Users/zyb/AppData/Roaming/Vita3K/Vita3K/ux0/data/midp_debug.log`
- `C:/Users/zyb/AppData/Roaming/Vita3K/Vita3K/ux0/data/debug_log.txt`

---

## 参考资料

- phoneME CLDC: `src/vm/share/ROM/` 目录
- MIDP 构建: `phoneme-midp/build/vita_arm/GNUmakefile`
- romgen 配置: `dist/lib/cldc_rom.cfg`
- UseROM 配置: `src/vm/share/utilities/Globals.hpp`
- ROM 初始化: `src/vm/share/ROM/ROM.cpp`
- **8-30 第二轮修复（用户反馈"只有 Exit"+ 截图分析）**:
  1) 缩放公式反了：旧代码把 240x320 拉伸成 720x544（截图白区宽~720 证实），改为等比
     fit: scale=min(PW/VW,PH/VH)，竖屏 407x543 / 横屏 726x544 居中。
  2) **Java 堆仅 1.25MB（Form 空白根因）**: `getInternalPropertyInt("JAVA_HEAP_SIZE")`
     在复制来的 vita_pcsl.c stub 里返回 0 → 默认 1280KB，装载 183 张皮肤图 OOM
     （大图失败→标题/正文不画，softbar 小图幸存；GC 风暴→3FPS）。stub 现返回 32MB。
  3) **方向**: Vita 横持，默认虚拟屏改为 320x240 landscape；launch.cfg 第三行
     portrait|landscape 可切（vita_display_set_orientation 须在 VM 启动前调）。
     internal.config 与 internal.landscape.config 两份随 VPK 打包，启动器按方向拷贝。
  待确认: 用户重测 Form 是否显示、FPS 是否恢复。
- **8-30 第三轮修复（闪退）**: JAVA_HEAP_SIZE=32MB 后 VM 报 "Could not allocate VM heap"
  直接退出。根因: CLDC 堆走 jvm_malloc→newlib，vitaSDK newlib malloc 池默认仅 32MB
  （sbrk.c 的 `_newlib_heap_size`），32MB Java 堆塞不进。修复: vita_main.c 定义
  `unsigned int _newlib_heap_size_user = 64MB`（覆盖弱符号，app 分区 MEMSIZE=128MB 内）。
  链接验证: nm 显示 _newlib_heap_size_user 已从 w 变为 D 81251f48。
  经验: Vita3K 日志 tty 输出（vm_output.log: "Could not allocate VM heap"）定位此类问题最快。
- **8-30 第四轮（用户要求）**: 默认方向改回竖屏 240x320（J2ME 游戏主流为竖屏设计，
  320x240 默认会让竖屏游戏布局错乱）。vita_main.c 默认 orient="portrait"，
  VITA_DEFAULT_LANDSCAPE=0。landscape 仍可经 launch.cfg 第三行选择。
- **8-30 第五轮（日志确诊，Form 空白真根因）**: vm_output.log 抛
  `IllegalArgumentException: This display does not support title` @ Display.setCurrent(bci=45)
  → startApp 失败，Form 从未真正显示（白屏+Exit 只是显示系统初始层）。
  根因: 我写的 lfjport_get_display_capabilities() 返回 0 = 什么都不支持。
  修复: 返回 0x3FF（DisplayDevice.java 位: 1=input,2=commands,4=forms,8=ticker,
  16=title,32=alerts,64=lists,128=textboxes,256=tabbedpanes,512=fileselectors）。
  教训: 参考 lfjport_fb_export.c 复制来的"返回 0"stub 不是无害默认值，是硬故障源。
  另: debug_log.txt 显示 SkinResources 会先找 ux0:/data/J2ME00001/lib/*.png 再回退 jar
  （lib 无自定义皮肤属正常回退路径）。
- **🎉 8-30 里程碑: 首次完整运行成功**。Form 标题/正文/Exit 全部正常显示（竖屏 240x320）。
  渲染链+高级UI+皮肤+字体+32MB堆+能力位修复全部验证通过。
  待验证: InputTest（按键链）、CanvasTest（drawImage/PNG）、真实游戏。
- **8-30 游戏管理菜单 (vita_menu.c v1)**: JVM 启动前的原生菜单（C 直接画 960x544 fb + 5x7 字体）。
  目录约定: games/<名>/game.jar+game.cfg（首行类名/次行方向）、inbox/*.jar 待安装。
  键位: ↑↓选择 ✕启动 ○切方向 □×2卸载 START装inbox SELECT重扫；列表空时 ✕ 退回内置 demo。
  MIDlet 类名自动解析: 复用 libobj.a 的 midpOpenJar/midpGetJarEntry 读 MANIFEST "MIDlet-1:" 第三段
  （注意 midpFree 是 midpMalloc.h 的宏；pcsl_string 可栈上构造 {jchar*,len,0}，路径走 vita_pcsl 直通）。
  launch.cfg 降级为手动覆盖方式（菜单无游戏时 ✕ 退出后生效）。
  已知限制: 所有游戏共享 internal 套件 RMS（appdb 单一空间），存档互通；按套件隔离待做。
  用户自行在 CMakeLists 加了 VITA_SKIN_FILE_ARGS 把皮肤 PNG 同时打入 VPK 的 lib/（皮肤加载双保险）。
- **8-30 菜单重写为 vita2d（修闪烁/4058FPS）**: 用户反馈菜单疯狂闪烁（Vita3K 显示 4058FPS）。
  原因: 手画 framebuffer 循环无 vsync，全速 SetFrameBuf(IMMEDIATE)。按用户建议改用 Vita 专门
  UI 接口 **vita2d**（SDK 已内置 libvita2d.a）: vita2d_init/load_default_pgf(系统 PGF 字体)/
  start_drawing/clear/.../end_drawing/swap_buffers(带 vsync)。输入连发改时间基
  (sceKernelGetProcessTimeWide, 300ms 首延 100ms 连发)，原帧计数连发在千 FPS 下会爆炸。
  链接: vita2d + ScePgf_stub + SceAppUtil_stub + SceCommonDialog_stub
  （无 SceFont/SceCommonGui stub；sceCommonDialogUpdate 由 CommonDialog 提供）。
  vita2d_fini 后 JVM/裸 framebuffer 显示接管，无冲突。
- **8-30 修菜单 jar 路径 bug**: debug_log 显示 midpOpenJar 路径成 "u0/aaJM001gms壢絅/aejr"
  （= ux0:/data/J2ME00001/games/.../game.jar 隔字节丢字符）。根因: vita_menu.c 把 UTF-8
  字节指针直接强转 pcsl_string.data (jchar*=16位单元)，读取按 16 位步长隔字节取。
  修复: make_pcsl_string() 把每字节扩展为一个 jchar 再传，用完 free。
  教训: pcsl_string.data 是 jchar 数组不是字节串，构造时必须逐字节扩展。
- **8-30 菜单交互重构（用户反馈"装完不知道怎么打开"）**: 列表 → 点击/✕ 打开游戏 → 弹出
  操作对话框（启动/方向/卸载×2确认/返回），对话框可物理键或触摸直接点选项，点外部关闭。
  新增前触屏支持: sceTouchSetSamplingState(START)+sceTouchPeek（本 SDK 无
  sceTouchPeekBufferPositive/sampling_mode，是 state 命名），坐标 1920x1088→/2 映射，
  tap=按下抬起沿。链接加 SceTouch_stub。
  注意: 菜单中文文案依赖 Vita3K/系统 PGF 字体的 CJK 覆盖，真机可能豆腐（Vita 官方无简中），
  到时改英文文案或加载自定义字体。
- **8-30 菜单中文渲染修复（用户截图: 中文全是"_"）**: Vita 系统 PGF 字体无简体字形。
  方案: vdpm 装 freetype/zlib（SDK 原缺），链接链 vita2d→freetype→png→bz2→z
  （freetype 开了 png+bzip2 支持），打包 Windows Deng.ttf(16MB) 到 VPK
  data/J2ME00001/font.ttf，vita2d_load_font_file + vita2d_font_draw_text 渲染
  （ui_text/ui_textf 封装，PGF 回退）。VPK 12MB。
  字体版权: Deng 为微软字体，仅本地使用；分发需换开源字体（Noto Sans SC 子集等）。
  触摸: 代码已用 sceTouchPeek（tap=抬起沿），Vita3K 需鼠标左键=前触摸；物理键为主。
  按键布局已 Vita 风格: ✕确认/打开、○返回。
- **8-30 修自动解析失败的真根因**: vita_pcsl.c 的 pcsl_string_utf8_length 返回 length*3
  （粗略上限），而 pcsl_string_get_utf8_data 产出 length 字节——midpGetJarEntry 用该
  长度做 jar 条目名匹配（20 字节名 vs 60 字节 length）永远失败，MANIFEST 读不出 →
  安装时 game.cfg 写 "-" → "没有 MIDlet 类"。修复: 两个 stub 改为一致的真 UTF-8
  编码/计数（ASCII 1 字节、BMP 2-3 字节）。口袋灵兽的类名 = GameMidlet（用户手动
  填 cfg 亦可立即游玩）。
- **8-30 启动游戏闪退（选中游戏后、JVM 前死亡）**: boot_log 只有首行 = 死在菜单返回前。
  嫌疑: vita2d_free_pgf/vita2d_fini 在 Vita3K 上的 GXM teardown。处理: 跳过菜单的
  vita2d 卸载（进程单次运行泄漏无碍，JVM 用裸 sceDisplay 与 vita2d 的 GXM 上下文无关），
  并在启动链路加 [menu] stderr trace（parse 结果/launching/handing over）定位下一次问题。
  注意: 用户并行修改了 vita_display.c（加 get_virtual_size/get_viewport 供触摸映射），
  dst_* 声明已上移修编译；后续编辑前先读文件避免冲突。

- **8-30 功能补全批次（P2触摸/P4生命周期/P3中文字体）**: 全部构建通过，velf nm 验证生效。
  *P2 触摸*: vita_input.c 加 touch_poll()——sceTouchSetSamplingState(START)+sceTouchPeek
  （采样起点修复: 菜单代码有、input 此前漏，peek 前 must START），4095 坐标经
  vita_display_get_viewport() (dst_x/dst_y/dst_w/dst_h) 映射 240x320 虚拟坐标，
  状态机 PRESSED/DRAGGED(坐标变)/RELEASED(抬手) → MIDP_PEN_EVENT (X_POS/Y_POS/ACTION
  用 KEYMAP_STATE_*)，与 qvfb 参考一致走 UI_SIGNAL；vita_display.c pen_supported/
  motion_supported 翻成 KNI_TRUE。
  *P4 生命周期*: lifecycle_poll() 轮询 sceAppMgrReceiveSystemEvent，任意事件→
  midp_suspend()（libobj.a 已有，发 PAUSE_ALL_EVENT→AMS→MIDletPeer.pauseApp()），
  ON_RESUME→midp_resume()（ACTIVATE_ALL_EVENT→startApp()）。PS 键切后台/回来即触发。
  *P3 中文字体*: 新文件 vita_font.c——CMake 删 libobj 的 gxjport_text.o（空 stub），
  本文件供 gxjport_draw_chars/get_chars_width/get_font_info 三符号；
  stb_truetype v1.26 运行时光栅化 TTF（src/stb_truetype.h，PUBLIC DOMAIN），
  字体在 VPK data/J2ME00001/font.ttf（用户已放 Deng.ttf 16MB；ux0:/data/J2ME00001/font.ttf
  可同名覆盖）。glyph 缓存 1024 桶 per (codepoint,size,style)，MIDP SIZE 8/16/24→
  16/20/24px，bold=alpha 右移浸染，CJK 缺字形画 tofu 框；返回 KNI_TRUE 接管渲染，
  无字体/无 glyph 返回 KNI_FALSE 回退内置 5x7（ASCII 兜底）。gxj_screen_buffer
  字段名注意是 pixelData/alphaData 不是 pixels/alpha。
  *测试 MIDlet*: FontTest（MIDlet-5）中英混排+三字号+加粗 Canvas 直绘；NetTest(MIDlet-4)
  上轮已有。MANIFEST 同步 5 个。
  构建链注意: build_jar.sh javac -source 1.3 不能用枚举/泛型/静态嵌套类以外的
   JDK5+ 语法；FontTest 的内部类须 static class（CLDC preverify 要求）。
  *遗留*: vita_menu.c 中文渲染走 vita2d+freetype 独立于本字体系统（互不影响）；
  FontTest 的 chameleon 高级 UI 路径未测（Canvas 直绘已覆盖 gxjport 链）；
  触摸 Vita3K 需鼠标左键模拟；P5 横屏 launch.cfg 第三行 portrait|landscape 上轮已
  实现（vita_main.c read_launch_cfg + vita_display_set_orientation）。
- **8-30 游戏空白诊断（口袋灵兽）**: boot_log 全链路 OK、GameMidlet 自动解析成功、VM 启动、
  Java LCDUI init 执行（绿点出现）、但 refresh_count 恒 1（游戏从未 paint）、无任何异常输出
  （USE_CLDC_RELEASE=true 关闭 phoneME 内部日志；startApp 挂起而非异常——异常会走
  MIDletStateHandler→stdout）。结论: 游戏 startApp 挂起，头号嫌疑 MMAPI 空壳阻塞
  （UnsatisfiedLinkError 是 Error 穿透普通 catch 也可能）。对策: 新增 ProbeMIDlet
  （props/canvas/game包/offscreen image/PNG解码/RMS/MMAPI tone/wav/threads 逐项
  PASS/FAIL 打印到 System.out→midp_stdout.log），launch.cfg 类名填 ProbeMIDlet 运行。
  VM stdout 通路: JVMSPI_PrintRaw→jvm_printf(=printf)→stdout→midp_stdout.log ✓。
- **8-30 彻底移除 vita2d（最终方案）**: 实验证实 vita2d_fini/free_pgf 在 Vita3K 必崩
  （调用→闪退；跳过→不崩但 GXM display queue 占着显示→后续 MIDlet 全空白）。
  菜单最终方案: 手绘 960x544 fb + **freetype 渲染 Deng.ttf 中文**（纯 CPU，无 GXM）
  + sceDisplayWaitVblankStart 限速 60fps。显示路径全程单一（裸 SetFrameBuf），
  JVM 接管零冲突。移除链接里的 vita2d/ScePgf/SceAppUtil/SceCommonDialog stub。
  中文菜单文案改回英文（避免引freeterm复杂度）……（实际: 菜单文案英文，游戏名 UTF-8 经
  freetype 正常渲染中文）。

- **8-30 RMS 验证 MIDlet (RmsTest, MIDlet-6)**: P1 收尾任务。调研确认 RMS 链路:
  Java RecordStore -> com/sun/midp/rms native -> rmsdb_record_store_open
  (rms/record_store/file_based/native/rms.c:515) -> midp_file_cache_open ->
  storage_open (core/storage/reference/native/midpStorage.c:463) ->
  pcsl_file_open = vita_pcsl.c sceIo 实现——理论上开箱即用，RmsTest 用于实测。
  行为: 启动计数器存 RecordStore "vita_rms_test" 记录 1，第 2 次以后运行显示
  [PERSIST OK]; SCREEN 键=Wipe 清档重测。
  *修复一路障*: vita_menu.c 编译 fatal error ft2build.h + FB_H 未定义——
  根因是 cmake 缓存陈旧（增量 make 未感知 CMakeLists 加的 freetype2 include;
  之前菜单中文渲染改造时引入的依赖），touch 重编即过，同时确认 CMakeLists 需
  include_directories($ENV{VITASDK}/arm-vita-eabi/include/freetype2)。
  MIDlet 总清单(Hello.jar 内 6+类): HelloMIDlet/CanvasTest/InputTest(上上轮)/
  NetTest(网络)/FontTest(中文字体)/RmsTest(RMS 持久化)。exit=0，VPK 186 条目。
- **8-30 Hello 空白回归（绿点仅 ~10 帧、画面纯白、19fps 持续刷新）**: 显示链路正常
  （blit 区域=407 竖屏 ✓ 绿点 ✓），chameleon 画的是空内容且持续 repaint；jar 皮肤 183PNG
  完好。对照手段: ProbeMIDlet 增强——加 skin 资源加载测试 + 结尾显示皮肤无关的纯红 Canvas：
  红屏=渲染/flush 链好、问题锁定 chameleon 皮肤/UI；无红屏=Canvas 路径问题。
  midp_stdout.log 的 [PROBE] 逐项给出子系统答卷（含 MMAPI tone/wav、RMS、PNG、线程）。
- **8-30 用户确认回归点=菜单引入后 chameleon 全空（纯白无 Exit，连里程碑的内容也没了）**。
  最大嫌疑: 菜单的 freetype 16MB 字体缓冲常驻 64MB newlib 池（FT Memory_Face 要求 buffer
  存活，一直未释放），加上 VM heap 32MB 把运行期分配挤爆 → chameleon 静默失败。
  修复: 菜单返回前 ft_shutdown()（FT_Done_Face/FreeType + free 字体缓冲），同步菜单
  之后字体无用途。待验证。
- **8-30 重要发现（用户确认回归点后重新对照）**: 里程碑 Form 成功显示时的配置 =
  **虚拟屏 960x544 全屏**（internal.config 原版 960、gxj buffer 960x544）！240x320
  虚拟屏是里程碑之后引入的，chameleon 从此空画（纯白持续 repaint）。皮肤图为小 tile
  （27x27 等）无越界问题。当前实验: 恒定 960x544 虚拟屏（游戏自适应 getWidth/getHeight，
  全屏可玩），config 恢复原版。若 960 恢复显示→240x320 虚拟屏破坏 chameleon 的具体
  机制待查（怀疑 chameleon/skin/gxj 某处对非原生屏尺寸有隐含假设）；虚拟屏/缩放功能
  需在 960 基线上重新设计。
- **8-30 用户回退**: 用户自行将 vita-port 回退到无菜单、240x320 竖屏直启版本
  （删除 vita_menu.c，vita_display/main/input 回退早期版）。已按回退后代码重新编译
  VPK（2.4MB）成功。菜单功能移除；游戏启动暂回 launch.cfg/默认 Hello 方式。
  待办: 基于此干净基线重新验证 Hello 显示，再逐步 reintroduce 功能。
- **8-30 空白回归定案（用户回退验证）**: 回退版（pcsl=旧截断语义）Hello 正常 → 空白真凶 =
  **pcsl utf8 精确化改动**（get_utf8_data 对非 ASCII 输出 2-3 字节真 UTF-8，phoneME 内部
  按 pcsl_string_length（字符数）准备缓冲的调用点被溢出 → 堆破坏 → chameleon 空白/游戏
  挂起）。触发物=口袋灵兽 jar 的中文条目名。"与菜单同轮构建"造成菜单连坐假象。
  **合并决策**: pcsl utf8 保持旧截断语义（不合并精确化——需先审计全部调用点缓冲分配）；
  heap 32MB/皮肤双打包/240x320 竖屏/Probe 等 MIDlet/launch.cfg 保留（回退版已含且验证正常）；
  菜单暂缓（重新设计：确定性资源释放 + utf8 安全前提）。240x320 破坏 chameleon 的旧假设
  同时被推翻（回退版=240x320 且正常）。
  经验: **改 pcsl_string 这类底层字符串原语的语义 = 全局风险**，任何精确化都必须配套
  审计所有调用点的缓冲约定；"同轮多改动"出回归时优先用回退+二分定位。
- **8-30 合并 0001-save-change.patch**（另一会话的完整功能版，基于回退基线）:
  - vita_menu.c: freetype 手绘版（无 vita2d，ft_shutdown 释放）✓
  - **vita_font.c + stb_truetype**: 替换 libobj 的 gxjport_text.o 空壳（ar d gxjport_text），
    游戏 drawString 经 stb_truetype 渲染 app0:/data/J2ME00001/font.ttf（用户可用
    ux0:/data/J2ME00001/font.ttf 覆盖）→ **游戏内 CJK 文字可显示**
  - **vita_net.c**: 真 BSD socket（SceNet/SceNetCtl）替换 pcsl socket stub +
    vita_net_early_init(VM 前) + vita_net_poll(select 挂 checkForSystemSignal 路径)
  - vita_input.c: +PEN 事件(触摸 viewport 映射) + suspend/resume(appmgr) + net_poll
  - vita_display.c: 恒 960x544 pin + viewport 导出
  - midlets: FontTest/NetTest/RmsTest 诊断 MIDlet
  - **合并时排除**: pcsl utf8 精确化两处已回退为旧截断语义（空白真凶，见上）
  构建 ✓。测试顺序建议: Hello(基线) → FontTest(游戏内 CJK) → RmsTest → NetTest → 口袋灵兽。
- **8-30 合并版 Hello 空白 → 二分开始**: 合并 patch 后 Hello 空白（utf8 已排除）。
  Vita3K 日志: font.ttf(16MB) 在 VM 启动 0.1s 被读入（vita_font 惰性加载），之后 14s
  静默直到关闭 → 白屏 = paint 卡死在第一次 drawString（stb_truetype 渲染 16MB Deng.ttf）
  或 16MB font_data 挤爆 64MB newlib 池。二分: 本轮构建已排除 vita_font（ar d 列表恢复
  gxjport_text.o stub、vita_font.c 移出编译）+ vita_net_poll 事件泵旁路
  （ENABLE_VITA_NET_POLL 条件编译）；vita_net.c 保留编译（pcsl 网络符号需要，
  net_poll 被旁路，early_init 无害）。待用户验证 Hello。
  若恢复 → 逐个开回 font/net 定位；font 的修复方向 = 换小子集字体 + 查 stbtt 渲染路径。
- **8-30 找到事件风暴真凶（lifecycle_poll）**: config 一致性假说也被推翻（960+960 一致
  仍空白）。最终定位: patch 版 vita_input.c 的 lifecycle_poll()——Vita3K 的
  sceAppMgrReceiveSystemEvent 恒返回成功（零填充事件），while 循环无限轮转且事件非
  RESUME → **midp_suspend() 风暴** → PAUSE_ALL 事件灌爆 → MIDlet 启动即被暂停 →
  永不 paint → 白屏（19fps = suspend 事件风暴的泵频）。修复: lifecycle_poll 整体旁路
  （注释说明，真机+正确过滤后才能启用）。cfg "-" 占位说明: scan 时 manifest 兜底成功
  （boot_log class 正确），后续加 scan 成功回写 cfg 优化。
- **8-30 修 scan_games cfg 解析 bug**（用户手动改 cfg 未生效）: 重写版读 cfg 第二行时
  path 未初始化（sceIoOpen 垃圾路径）→ 手写的方向/类名可能被忽略。已修: 正确路径 + 两行
  完整解析（首行类名、次行方向）+ manifest 解析成功后 save_cfg 回写。
  等待用户反馈: lifecycle_poll 旁路版 Hello 是否恢复显示。
- **8-30 suspend 风暴修复后的新状态**: Form 管线走通大半（softbar "Done" 出现——里程碑后
  首次有非白内容），4FPS 偏低、标题/body 仍未画。已按用户要求切回 240x320 竖屏默认
  （landscape 320x240 可选），internal.config/landscape 双份同步。
  下一步诊断工具: ProbeMIDlet（含 skin 资源加载测试 + 纯红 Canvas 对照）——若红屏显示
  则 Canvas 链 OK、问题锁定 chameleon skin；[PROBE] 答卷在 midp_stdout.log。
- **🎉 8-30 里程碑: 真实游戏链路跑通**。用户在 games/Hello/game.cfg 手填 HelloMIDlet 后
  成功运行（此前自动解析的类名疑似带控制字符污染 → Class.forName CNFE，异常消息不可见
  尾字符）。修复: parse_manifest_class 只拷贝可打印字符（>=0x20）。
  待验证: 口袋灵兽（GameMidlet）实际运行表现（画面/按键/速度）。
  已知遗留: ①MMAPI 音频未实现（游戏静音，某些游戏可能异常）②RMS 全游戏共享未隔离
  ③性能 4FPS 待优化 ④240x320 小屏 chameleon 空白待查（当前 960x544 全屏直出可玩）
  ⑤菜单功能已回合并（freetype 版）。
- **8-30 CNFE 随机回归（HelloMIDlet ClassNotFound 再次出现）**: 用户手填类名成功一次后
  再次 CNFE → 随机性指向**内存水位**: CLDC JarFileParser 的 BufferedFile 缓存/类字节
  读取走 64MB newlib 池，池被 font 16MB(菜单)+VM heap 32MB+杂项瓜分后类加载分配可能
  失败→CNFE。对策: _newlib_heap_size_user 64→96MB（MEMSIZE=128 分区内）。
  备选嫌疑: ①game.jar 内容（确认用户 games/Hello/game.jar 是最新构建的 Hello.jar，
  含 HelloMIDlet.class 1587 字节）②JarFileParser 的 CacheJarEntries 缓存。
  若扩池后仍 CNFE → 在 JarFileParser 加载失败处加 trace 打印具体失败点。
- **8-30 Plan B 落地（绕开 VM jar 加载）**: CNFE 持续（多次运行均 ClassNotFound HelloMIDlet，
  jar 本地验证含该类）→ CLDC 从 classpath 第二个 jar 加载应用类不可靠。利用 CLDC 原生
  目录 classpath 支持（ClassPathAccess::open_entry_from_file jar 打不开即按目录找文件）:
  启动器把游戏 jar 全条目解压到 ux0:/data/J2ME00001/appclasses/（midpIterateJarEntries
  收集名字[jchar→正确UTF-8] + midpGetJarEntry 解出 + 逐级 mkdir 写文件），classpath 第二项
  指向目录。解压失败自动回退 jar 模式。中文条目名在 GetJarEntry 匹配（截断语义）下可能
  skip（日志计数）——类（ASCII）必出。boot_log 有 [extract] 统计。
- **8-30 全面回退（用户要求）**: 发现 samples/ 是 git 仓库且 HEAD=288337c "j2me success run
  demo"=用户确认 Hello 正常的版本。git checkout + clean 完整恢复：src 仅 4 文件
  （display 283/input 160/main 223/pcsl 1205 行），无菜单/无net/无font/stb，CMake 极简。
  构建成功。**当前 VPK = 已验证可跑 Hello 的版本**。
  教训（重要）:
  1. patch 合并轮的多项改动（菜单/utf8/lifecycle/net/font）从未逐项验证就叠加，
     出问题时无法定位——以后每加一项必须单独构建+用户验证
  2. samples/.git 的存在一直在 AGENTS.md"非 git 仓库"认知之外，导致走了大弯路
  后续路线（如继续）：以 288337c 为基线小步走，每步：改一项→构建→用户确认→git commit。
- **8-30 功能回归第一步: 游戏菜单（提交 447177d）**: 基于干净基线 288337c 只加一个功能。
  设计（吸取全部教训）: 手绘 fb + 内置 5x7 ASCII 点阵（零新库/零链接变化）、每帧
  sceDisplayWaitVblankStart、退出 free(menu_fb)、不碰 display/input/pcsl、菜单期间独立
  轮询 SceCtrl、MANIFEST 解析（make_pcsl_string 逐字节扩展 + 最后逗号 + 只拷贝可打印字符）。
  功能: games/ 列表、✕ 弹窗（launch/orientation/uninstall×2/back）、START 装 inbox、
  SELECT 重扫、空列表 ✕ 退回 launch.cfg/Hello。中文游戏名第二步（freetype 或点阵中文）。
  测试要点: 菜单正常 + Hello 照常显示 + inbox 装游戏 → 启动。
- **8-31 修 "no MIDlet class in jar"（含 Hello，提交见 git）**: 定案——
  findJarEntryInfo 要求 entry.nameLen == 传入 nameLen（精确相等），而基线 pcsl 的
  pcsl_string_utf8_length = length*3（粗略上限）→ 任何条目查找必失败（与 jar/类名无关，
  Hello 同样中招）。菜单改为自带 mini zip 解析（sceIo 读文件 + EOCD/central directory
  遍历 + zlib raw inflate（inflateInit2 -15），大小写不敏感匹配），不再调用 midpJar/
  pcsl 链。CMake 仅加 zlib 链接。
- **8-31 MIDlet 生命周期插桩**: MIDletStateHandler.createAndRegisterMIDlet 加
  System.out.println（creating/OK/FAILED+栈→midp_stdout.log），编译后类已替换进
  classes/ 与 midp_system.jar。javac 1.3 无 Throwable 重抛→按 CNFE/IE/IAE/RE/Error
  分类重抛。诊断流程: 跑游戏→midp_stdout.log 看 [MIDLET] 行 + vm_output.log 异常栈。
- **8-31 zip 解析诊断插桩**: zip_read_entry 每步（open/size/EOCD/entries/前4条目名/
  匹配/method）fprintf(stderr)→midp_stderr.log；JVMSPI_PrintRaw 发现于 midp_run.c——
  VM 全部输出（Java println/异常栈）都写 ux0:/data/vm_output.log（追加）。MIDletStateHandler
  已插桩（[MIDLET] creating/OK/FAILED+栈 → 同文件，经 stdout）。
  注意: 之前"System.out 被 release 裁剪"推断不成立——JVMSPI_PrintRaw 在 midp_run.c 有
  文件实现，Java 输出必然落 vm_output.log。
- **8-31 终极根因修复（截图日志实锤）**: classpath "ux0:..." 的冒号被 CLDC 分隔符切断
  （entries: "ux0" + "/data/..."），系统类加载成功纯因相对残段碰巧命中 app0:；games/ 下
  的游戏 jar 相对残段不存在 → CNFE → 空白 + skin 加载失败 + Alert 空白崩溃连环。修复:
  main.c chdir(DATA_DIR)（newlib chdir，SceIofilemgr 传递）+ classpath 全相对
  （"midp_system.jar:games/x/game.jar"，条目内零冒号）。960 全屏 + 菜单保留。
  此修复同时应解决 skin 图标加载失败（同 classpath 根因）。
- **8-31 修 midp_system.jar 不更新问题（用户日志: NCDFE FullCanvas + 目录方式查类）**:
  ux0:/data/J2ME00001/midp_system.jar 是运行时文件（无任何代码维护它）——用户机器上是
  旧版（无 Nokia stub 类）→ NoClassDefFoundError: FullCanvas。同时确认: 冒号修复已生效
  （HelloMIDlet 加载成功，推进到 Nokia 类），Nokia stub 类已进 VPK 的 midp_system.jar。
  修复: main.c 每次启动 copy_file(app0 midp_system.jar → DATA_DIR)。
- **8-31 CJK 文字渲染（提交 80f5a12）**: 游戏能跑后用户报告中文不显示——gxjport_text.o
  是空壳（KNI_FALSE→回退 5x7 ASCII）。合并 patch 的 vita_font.c + stb_truetype.h:
  ar d gxjport_text.o + vita_font.c 提供同符号，stb_truetype 渲染 config/font.ttf
  (Deng.ttf 16MB, VPK data/J2ME00001/font.ttf, 可被 ux0:/data/J2ME00001/font.ttf 覆盖)。
  状态: 游戏能进（GameMidlet 全屏 960x544）+ 中文待用户验证。
- **8-31 文字渲染修复（yoff 语义）**: vita_font 恢复（cherry from 80f5a12）+ 修字形垂直
  定位: stbtt yoff 是基线相对（上正），gxj_text 传 line-box top（TOP anchor 原样/
  BASELINE 预调整），正确字形顶部 = y + ascent - yoff。原实现 pen_y+yoff 把字形推出
  clip 框 → 全部不可见。font.ttf 曾被 git clean（未跟踪）删除——已 git add 跟踪。
  提交: 见 git log（CJK glyph vertical position fix）。
- **8-31 文字渲染诊断插桩**: vita_font.c 全路径 trace（try_load_font 路径/字节数/init、
  draw_chars 进入[fallback 或 n+首码点+clip]、每字形渲染尺寸、完成 pen_x）→
  midp_stderr.log。等待用户一轮运行定位文字消失的确切断点。
- **8-31 关键诊断数据（用户 TTY 日志）**: [font] draw_chars n=3 first_cp=0x88C5("装")
  clip=(0,0,240,302) 打印 3 次但 **无 [font] glyph 行** → glyph_get 在 stbtt_GetGlyphBitmap
  前后未到达打印 → stbtt 渲染挂死/极慢/异常返回。已加计时 trace（首次渲染 + 异常值打印
  耗时 ms）。下轮日志见 [font] glyph ... took=XXms 即知 stbtt 是否卡/慢。
- **8-31 修 CJK 宽度 0（提交 8c7e0a4）**: 日志实锤 glyph bitmap=0x18（宽 0 高 24）。
  根因: vita_font.c:166 stbtt_GetGlyphBitmap(&font_info, 0, scale, ...)——x 方向 scale 传 0！
  → 每个字形宽度 0 → draw_chars 里 mask 非 NULL 但 aw=0 → 被跳过 → 文字全消失。
  修复: x/y 同用 scale_for(sc)。经验: **签名 (info, scale_x, scale_y, glyph,...)，
  位置参数 0 恰好落在 scale_x 上，编译器无警告（0 是合法 float）**。
- **8-31 vita_font v2 = 离线点阵字库**: stbtt 在 Vita ARM 渲染字形宽度恒 0（PC 正常），
  放弃运行时渲染。tools/fontgen.c（PC, stbtt 已验证路径）生成 1bpp 点阵库
  （ASCII 95 + CJK 20902 字, 20x22, 1.38MB）→ VPK data/J2ME00001/fontbitmap.bin。
  vita_font.c 加载 bank 查表 blit（前景色, clip 裁剪, CJK tofu 标记）。
  get_chars_width 等宽 20px。fontbitmap.bin 也可被 ux0 覆盖。
- **8-31 修 fontbitmap 头解析错位（v2 不显示的确定根因）**: fb_load 按错位偏移读头
  （fb_gw 读成 0 → 绘制循环零次迭代 → 无任何文字像素）。已按 fontbitmap.bin 实测字节
  布局重写解析：u16@12=GW、u16@16=GH、u16@20=STRIDE、u32@24=data_off、
  u32@28=ascii_count、u32@32=cjk_first、u32@36=cjk_count。
  下轮运行 midp_stderr.log 应出现 "[font] bitmap bank loaded ... (gw=20 gh=22 ...)"。
- **8-31 vita_font 诊断插桩**: draw_chars 进入打印（fb_ready/n/首码点/clip/dest 尺寸）+
  前 40 字符的字形命中打印（glyph=ok/MISSING）。一轮运行即可区分:
  ①无 [font] 行=draw_chars 未被调用（问题在 gxj_text 之前的调用链）
  ②glyph=MISSING=点阵库覆盖/码点问题
  ③全 ok 但屏幕无字=blit 坐标/颜色问题。
- **8-31 日志丢失根因（用户点破）**: CLDC VM 启动时重新绑定 stdio 到 tty0（Vita3K 日志的
  *** TTY: 逐字符输出即证据）→ vita_font 内 fprintf(stderr) 全被劫走，midp_stderr.log 空。
  修复: vita_font 自带 flog()（sceIoOpen ux0:/data/J2ME00001/font_debug.log 直写，
  独立 fd，不经过 stdio）。**重要经验: VM 线程内的日志必须自开 fd，不能依赖
  main.c 的 freopen**。
- **8-31 文字消失终极根因（font_debug.log 实锤）**: dest=320x-2128011192 ——
  gxj_text.c 在调 gxjport_draw_chars 前用 getScreenBuffer() 宏把 NULL dst 映射为
  &gxj_system_screen_buffer（gxj_intern_graphics.h），vita_font.c 未做此映射，
  从 NULL dst 重新推导出未初始化栈结构 → 全部字形画进垃圾内存。修复: 与 gxj_text.c
  相同的 getScreenBuffer 规则。日志链修复也同批提交（flog 独立 fd）。
- **8-31 类型混淆修复（真正的修复）**: font_debug.log 仍显示 dest=320x-2128011192 →
  根因是 gxjport_draw_chars 的 dst 参数**已经是 gxj_screen_buffer\***（gxj_text.c
  自己完成 imagedata→screen_buffer 转换 + getScreenBuffer 映射后才调用），
  vita_font.c 却将其当 java_imagedata* 二次解析 → 读垃圾字段 → 画进野内存。
  修复: 直接 (gxj_screen_buffer*)dst 强转。上一轮 getScreenBuffer 补丁是治标。
- **8-31 决定性诊断（用户要求停止猜测）**: vita_font draw_chars 一次性输出:
  ① dst 与 &gxj_system_screen_buffer 的指针同一性
  ② dest 尺寸/pixelData/alphaData 指针
  ③ **直接向 dest 四角画 40x40 色块**（白/红/绿/蓝 RGB565）——屏幕上看到角标 =
  缓冲即显示缓冲，缺字=字形内容问题；看不到=缓冲不对
  ④ 每字形非空白行计数 ⑤ 首个字形像素写入的物理地址/颜色。
  一轮运行 + 一张截图即可完全判定。
- **8-31 修 fontbitmap 头偏移**: fontgen 输出的头字段在偏移 6(GW u16)、8(GH u8)、
  10(STRIDE u16)、12(data_off u32)、16(ascii_first)、20(ascii_count)、24(cjk_off)、
  28(cjk_first)、32(cjk_count)。vita_font 之前的偏移全错。
- **8-31 fontbitmap 加载修复**: CLDC VM 启动时把 stdio 重绑到 tty0（Vita3K 日志的
  *** TTY: 输出即证据），font.c 内 fprintf(stderr) 全被劫走 → midp_stderr.log 空。
  修复: font.c 内所有日志改用独立 fd 直写 ux0:/data/J2ME00001/font_debug.log。
- **8-31 回退到 288337c 干净基线**: CJK 字体渲染多轮修复仍未解决，回退到已验证
  Hello 正常显示的提交。CJK 字体将作为独立、经过完整测试的功能单独实现。
  基线状态: 无菜单（launch.cfg/默认启动）、240x320 竖屏、5x7 ASCII 文字、
  皮肤已修复、heap 32MB、newlib 64MB、classpath 冒号修复、能力位修复。
  所有这些修复在基线里工作正常。
- **8-31 vita_font 完全重写**: 移除所有残留 vita_font_init 调用（旧 stbtt 版遗留），
  硬编码 GW=20 GH=22 STRIDE=3 DATA_OFF=40，fb_glyph 查 5 个区段返回 1bpp 位图指针，
  draw_chars 直接 blit。fontbitmap.bin 由 tools/fontgen.c 生成（覆盖 ASCII+CJK+punct+
  fullwidth+gen 共 21180 字形），打包进 VPK。

## 2026-09-09 v01.44：Error 32 终局定案（b195, 22f5ffc）

### 用户 vita3k.log 全量取证结论（17:49-00:02 会话，~2 万行）
1. **设备上的二进制不是 b194**。指纹：日志中每次文件 open 前后出现
   `debug_log.txt` 的 64B+22B 写入 = vita_pcsl.c `VITA_FILE_DEBUG=1` 的
   `[file_open] path=ux0:/data/J2ME00001/appdb/FFFFFFFF flags=0x2`（64B）
   + `[file_open] ok direct\n`（22B）。v01.38 (44e7e71, 9-08 12:12) 才把
   该开关置 0，b194 ELF 无 "debug_log.txt" 字符串。用户 17:49/20:17 装的
   VPK 是 pre-v01.38 的 debug 构建（8-31 04:30 时代）。
2. **Error 32 的真实删除目标是目录本身**：`ux0:/data/J2ME00001/appdb/FFFFFFFF`
   （suite id 目录，无文件名后缀）。链路：midp_remove_suite 清理循环 →
   storage_get_next_file_in_iterator → 我们的 pcsl_file_getnextentry
   不跳过子目录 → FFFFFFFF 目录被当文件返回 → storage_delete_file →
   pcsl_file_unlink(目录)。Windows 下目录被 RMS 句柄锚定必然拒绝 →
   Error 32 ×2（ux0 + Vita3K 内部 app0 fallback）。
3. **崩溃日志也是旧二进制的**：00:01:37.746 EXCEPTION_ACCESS_VIOLATION
   PC=0x8103350e（用 b194 addr2line 解析为 Graphics_drawArc，但旧二进制
   地址空间不同，仅供定位参考）。Vita3K exception_handler 记录后进程继续，
   崩溃后句柄生命周期错乱 → remove 执行 → Error 32。v01.42 后未再复现 6h
   闪退，b194/v01.44 上是否仍崩需复测。
4. **正常退出路径（17:56 会话）同一旧二进制无 Error 32**：v01.37 evict 语义
   命中后 remove 重试成功。Error 32 只在崩溃/异常路径出现。
5. b194 的 v01.43 预检查本身工作正常（拦截活句柄路径），但覆盖不到
   "目录被当文件删" —— 这是 v01.44 (22f5ffc) 修的：getnextentry 加
   `entry.d_stat.st_attr & SCE_SO_IFDIR` 跳过（与 getsizeoffilelist 同款过滤）。

### 验金方法（防再犯）
- 判定设备二进制版本：看菜单右上角 `vXX.XX bNN (hash)`，或 vita3k.log 中
  有无 per-open `debug_log.txt` 写入（有 = pre-v01.38）。
- b194 产物验证：`strings build/cmake/midp_vita | grep debug_log.txt` = 0。

## 2026-09-10 v01.45：b195 残余 Error 32 定案（truncate 换file踩孪生句柄）

### 用户 vita3k.log 铁证（23:55:33）
```
|E| [remove_file]: Cannot remove file: .../rms/appdb_1A35/FFFFFFFF
|E| [remove_file]: Error code: 32
|W| [io_error_impl]: remove_file (sceIoRemove) returned 0x80010002
```
sceIoRemove **真的执行了** → 不可能来自 pcsl_file_unlink（v01.43 预检查
拦截时不发 syscall）。穷举全部 7 个 sceIoRemove 调用点后锁定：
**pcsl_file_truncate 的 remove+create 换file**（pcsl_file_open 的 O_TRUNC
仿真同款问题）。此前的"b195 不会再有 Error 32"判断是错的。

### 根因：空名 store 的 db/idx 孪生句柄同路径
- `buildSuiteFilename`（rms.c）只在 store 名非空时追加 `.db/.idx` 后缀；
  nameLen==0 时**忽略 extension 参数** → db 句柄和 idx 句柄打开的是
  **同一条路径** `appdb_XXXX/FFFFFFFF`（RecordStoreImpl 构造 dbFile 后
  立即构造 RecordStoreIndex → idx 文件；POSIX 下同文件双开合法）。
- truncate 换file流程只关自己的 fd → sceIoRemove → **孪生 idx 句柄仍
  锚定名字** → Vita3K（_wfopen 无 FILE_SHARE_DELETE）拒绝 → Error 32。
- v01.41 的后验检查已把 swap 判失败返回 -1（fd 落回旧文件），syscall
  却已发出 → 日志噪音；且面包屑去重只按路径 → 该路径启动时已报过
  "unlink blocked"，23:55 的 "truncate blocked" 被**静默吞掉**，
  stderr 与 vita3k.log 对不上号。

### v01.45 修复（71a6ce0）
1. 新增 `vita_handles_held_ex(path, self)`（排除自己的 held 检查）：
   `pcsl_file_truncate` 与 O_TRUNC 仿真在换file**前**预检查，孪生句柄
   存在 → 直接 -1 + 面包屑，不发注定失败的 syscall，不再空关 fd。
2. `vita_report_held` 去重键改为 **op+path**（原只按 path，会吞掉
   同路径不同操作的证据）。
- 对调用者返回值与 v01.41~44 一致（-1）；真机 POSIX 语义下该 swap 本可
  成功，现被预检查拦下属保守策略——真机实测若有 truncate 失败再议。

### 教训（防再犯）
- **"不会再有 Error 32" 类断言前必须穷举 syscall 出口**：预检查只护住
  pcsl_file_unlink，truncate 换file / O_TRUNC 仿真两处直发 sceIoRemove。
- 面包屑去重维度必须包含操作类型，否则吞掉后续不同操作的证据。
- stderr 面包屑与 vita3k.log 时间线**对不上号本身就是线索**（本案：
  23:55 有 syscall 无面包屑 → 去重吞了或来源未埋点 → 两样都查）。

## 2026-09-10 v01.45 后续：第二次进 UC 崩溃定案（JIT 二次启动，-int 隔离）

### 崩溃现场（用户装 b200 复测，11:01:16.302）
第一次进 UC 正常，退出回菜单后**第二次进** → Vita3K
`EXCEPTION_ACCESS_VIOLATION 0xC0000005, Read 0x4E5060004`（35 位地址），
PC=0x85f05fd8（**JIT 代码缓存区 = Java 堆 compiler_area**），fault 指令
`ldr r4,[r5,#0x404]`；随后 PC=0x0 无限刷屏 → 闪退。

### 指令链解析（全部坐实，非猜测）
- r5=0x813b17d4=`gp_base_label`（nm 直读运行时地址），+0x404=
  `_compiler_stack_limit`(0x813b1bd8，合法地址)。
- `ldr r4,[r5,#0x404]` 出自 `CodeGenerator_arm.cpp:1190` JIT 序言
  "stack overflow + timer tick" 检查；GP 访问宏在
  `SourceAssembler_arm.hpp`（`ldr_gp_base`→`ldr_label("gp_base_label")`）。
- **fault 地址 0x4E5060004 ≠ r5+0x404** → 寄存器转储是损坏后状态；
  真相是执行流已跳进坏代码（垃圾字节把自己解码成同样的 ldr 形态）。
- JIT 编译代码不是独立 code heap：`Compiler::allocate_and_compile` →
  `Universe::new_compiled_method` 分配在**堆内 compiler_area**
  （`Compiler.cpp:1073` reserve_compiler_area），PC=0x85f05fd8 落在
  堆范围完全吻合 → "跳进已失效/被覆盖的 JIT 代码"。

### 跨轮状态排查结论
- `Universe::apocalypse()` 不复位 Compiler 静态状态，但每轮
  `Universe::bootstrap()` 会 `CompiledMethodCache::init()` +
  `Compiler::initialize()`（memset 全部 _state），跨轮复位基本完整。
- 第二轮 JIT 代码是新编译的，PC 指向第二轮的堆——所以不是"第一轮
  悬空缓存被复用"，而是 JIT 代码在堆里**被 GC/压缩搬移后失效**或
  二次启动时序触发的堆状态异常（未最终定论，属 VM 深水区）。

### 处置（phoneme-midp 6525f4a）：-int 纯解释器隔离
`runMidlet.c` 在 `JVM_Initialize()` 后注入
`JVM_ParseOneArg(1, {"-int"})` → `Arguments.cpp: UseCompiler=false`
（进程级静态，覆盖所有后续轮次）。标准 VM 选项，零新平台代码，
 phoneme-midp 改动规模 +1 文件。**性能换稳定；根因（JIT 代码在
二次启动场景下的失效机制）未修，待 UC 流程跑通后回头深挖。**
- 重编：`phoneme-midp/build_vita.sh`（增量，runMidlet.o 重编成功；
  尾部 libmidp.so multiple definition 仍为已知无害失败）→
  `vita-port/build/cmake` cmake --build 重打 VPK。
- 验证手段：`arm-vita-eabi-objdump -d runMidlet.o | grep -c JVM_ParseOneArg`
  = 3（原 2 + 新 1）。

## 2026-09-10 v01.47：truncate 改惰性逻辑截断（-int 隔离生效后的根修）

### 用户双测试结论（b201）
1. 退出 UC 再进**不再闪退** → v01.46 的 -int JIT 隔离有效
   （JIT 崩溃未再复现；根因仍在深水区，暂不回头）。
2. 删 rms/appdb_1A35 → UC 可进；**退出 Vita3K 重进 UC 卡初始化**。
   第二轮 stderr 在 `created+registered OK` 后零输出（无 [NET]、
   无任何 [pcsl] 面包屑）；第一轮出现过 `truncate blocked`。

### 根因链（v01.45 拦截 → compact 半途而废 → 脏数据 → 下轮卡死）
- compactRecords（RecordStoreImpl.java:782）先把数据块前移、更新
  header 的 RS6_DATA_SIZE（此时新大小已写盘），最后才 dbFile.truncate。
- v01.45 的预检查让 truncate 永远返回 -1 → IOException → compact
  中止，但 header 已改。物理文件仍保留旧尾块。
- 下一轮（新进程也一样）按 header 的新 DATA_SIZE 遍历，块边界与
  物理内容错位 → 读到垃圾 record 头 → UC 初始化流程卡死。
  与 v01.41 记载的 "first launch OK, every launch after broken"
  是同一数据破坏类。

### 根修：lazy logical truncation（vita_pcsl.c，j2me 658f744）
- Vita 无 ftruncate syscall；Vita3K 丢弃 SCE_O_TRUNC；物理截断只剩
  remove+create 换file，而空名 store（FFFFFFFF）的 db/idx 孪生句柄
  同路径锚定 → 换file必败。此路彻底放弃。
- VitaFileHandle 加 `logical_size`（-1=无钳制）。truncate(size) 只记
  钳制（多次截断取小者）；`pcsl_file_read` 过界返 0（EOF）、
  `pcsl_file_seek` 结果钳到 logical_size、`pcsl_file_sizeofopenfile`
  报逻辑大小；`pcsl_file_write` 写过界即解除钳制（文件重新变大）。
- 契约依据：phoneME RMS 全部读边界来自 db header（RS6_DATA_SIZE，
  RecordStoreImpl:799 `while (currentOffset < getSize())`）与 idx
  offset 表（RecordStoreIndex.getRecordHeader），**从不依赖物理
  EOF** → 逻辑截断对上层完全透明。
- O_TRUNC 仿真（storage_open 的 temp 文件路径）同改：打开即置
  logical_size=0，不再 remove+create，不再有"remove 被孪生句柄挡住
  → create 落在旧文件 → 交出内容矛盾句柄"的坑。
- v01.45 的 truncate/O_TRUNC 两处 vita_handles_held_ex 预检查随之
  移除（换file本身不存在了）；held_ex 函数保留备用。

### 遗留
- 物理尾巴字节留在盘上（RMS 库 KB 级，可接受）；真机可换真 ftruncate。
- JIT 二次启动根因未修（被 -int 掩盖）；若后续恢复 JIT 需先复现并
  定位"JIT 代码在堆内被失效"的机制。
- 验证入口：`strings midp_vita | grep "version: J2ME"` 应显示
  v01.47 b202 (658f744)。

## 2026-09-10 v01.48：RMS 文件缓存悬空句柄根修（数据不持久 + 书签卡死）

### 用户现象（b203 复测反馈）
- 二次进入 UC 正常（v01.47 生效）、`truncate blocked` 消失
- **但数据不持久化：每次启动都是全新开始**
- **保存书签直接卡死，无法操作**

### 根因：midp_file_cache.c 单例缓存悬空句柄（phoneme-midp f979b68）
`mFileCache` 是进程级单例，缓存"当前文件"的 handle。close 时只 flush
不释放单例，`mFileCache->handle` 永久指向已 free 的 VitaFileHandle。
UC 多 store 并存（空名设置库 FFFFFFFF + 书签库 + 缓存库），open/close
交错后：下次 `midp_file_cache_open` malloc 大概率复用同一块内存（新
文件合法 fd 占住了 fd 字段），走 else 分支对悬空 handle 执行
`finalize(stayOpen=TRUE)` → `storagePosition(悬空handle, 旧cachedPos)`
→ **把新文件 seek 到旧文件残留偏移**：
- 新开 store 读 db header 错位 → DB_SIGNATURE 校验失败 →
  "invalid record store contents" → UC 视为损坏重置（每次全新开始）
- 写路径位置污染 → 书签保存流程死循环卡死

### 排除项（本轮分析定案）
- **`unlink blocked .../FFFFFFFF` 良性**：FFFFFFFF = INTERNAL_SUITE_ID(-1)
  的 8 位 hex 目录名，UC 的空名设置库 db/idx 共用同一路径。退出时的
  unlink 来自 UC 自身清理逻辑，被 v01.43 held 预检查挡住反而保护数据。
  Windows/Vita3K 共享语义下打开的文件不可删，不存在"句柄先关则误删"。
- **v01.47 逻辑截断无涉**：写路径 sceIoWrite 直写落盘 ✔，commitWrite
  → flush → 真实写穿 ✔，读边界/sizeof 钳制正确 ✔。数据其实一直在盘上，
  坏在读回时的 header 校验。
- **RMS 文件锁未启用**：TARGET_VM=cldc_vm 时 lib.gmk 不编译
  rms_file_lock.c，RecordStoreLock 用 no-op stub，无死锁嫌疑。
- 空间计算正常：getusedspace 真实现（v01.34+）+ jam_space=100MB。

### 修复（最小 diff，16 行）
`midp_file_cache_close`：finalize 后 `midpFree(mFileCache); mFileCache
= NULL;`，恢复上游不变量"mFileCache->handle 必须指向打开中的文件"。

### 构建/产物
- 重编：`rm build/vita_arm/obj/arm/midp_file_cache.o && ./build_vita.sh`
  （exit=0，尾部 libmidp.so `_rom_linkcheck` 链接失败为已知无害项，
  VPK 走 libobj.a 不受影响；libobj.a 已含新对象）
- VPK：vita-port/build/cmake/midp_vita.vpk = **v01.48** (f53174e)
  14458298 B
- 提交：phoneme-midp f979b68（+16）；j2me 主仓 8f512a0 + f53174e

### 验证入口
- `strings midp_vita | grep "version: J2ME"` 应显示 v01.48
- 复测预期：UC 二次启动数据仍在（书签/设置保留）、保存书签不卡死、
  `unlink blocked .../FFFFFFFF` 仍会出现（预期，良性）

### 遗留
- JIT 二次启动根因未修（-int 掩盖中）
- 物理尾巴字节留盘（v01.47 定案，可接受）

### 修正（同日）
- 首次 VPK 重编未触发 gen_version（版本头缓存 b203 d841b85），重建后
  正确：**v01.48 b206 (eb9b719)**，14458185 B。装 VPK 前先核对
  `strings midp_vita | grep version`。

## 2026-09-18 v01.83：log9 coredump 定案"游戏轮次挂死"+ 泵墙钟兜底修复（dd73f18 / phoneme-cldc 57a1b3d）

### 五轮取证链（log3→log9）
- log3-log6：所有会话 0 次 `runMidlet returned`；挂点集中在 MIDlet 启动早期
  （ToneTest stg1→stg2、FileManager startApp、封神榜 RMS open），println 逐字符打印中途冻结
- log7：方向键分发器修复（v01.80）+ 心跳回放正常
- log8（v01.81 看门狗优先级 0x10000100 修复后）：H2（时钟冻结）出局——
  双时钟都活（d=100, kd=100062）；**vm RUNNING wait=none = 用户态满转**
- log9（v01.82 看门狗故意数据中止触发 coredump）：转储解析定案

### log9 转储解析方法（已验证，可复用）
- `gunzip -c *.psp2dmp > core`；NOTE 布局：THREAD_INFO(0x1003)@0x694（5 线程，
  步长 0xc8，名字 +0x10，pc +0xa4）；THREAD_REG_INFO(0x1004)@0x1e90
  （主线程记录 +0：r0-r12 从 +0x10、sp=+0x44、lr=+0x48、pc=+0x4c）
- MODULE_INFO：midp_vita 基址 0x81066000（本次 slide +0x66000，每次启动不同）；
  栈回溯 = 扫栈段中 [基址,基址+大小) 的 dword 减 slide，nm bisect 定位
- 转储只含栈段，不含代码/data/bss/Java 堆——BSS 静态变量运行时值不可读
- 寄存器布局验证法：watchdog 线程 r0=0x81000000 r1=0x53455244（"DRES"）应与
  触发指令 `*(volatile int*)0x81000000=0x53455244` 吻合 ✅

### 现场重建（定案）
- VM 线程 pc=`start_lightweight_thread_asm+0x8`、lr=桩入口、sp=分发层浅帧
  （[sp+0]=`JVM::start+0x178`，`bl primordial_to_current_thread` 返回槽）、
  r1=0（新线程无 TERMINATING 位）→ 调度器正把"从未运行过的 green 线程"交给引导桩
- 残留帧：类加载 fread 链（open_entry→Inflater→parse_stackmaps→_read_r）+
  一次线程切换（adjust_priority_list→set_current→on_task_switch）+ ani_mark CE#342 OUT
- vm_boot.log 只有 `loop 0 enter`（JVM::start 从未返回）
- **结论：green 线程生灭环**（线程死亡→重建→再死亡），环内无解释器 tick
  检查点；`Scheduler::yield()` 的 VITA 泵门
  `_timer_has_ticked || _estimated_event_readiness > 0` 永不满足
  （后者只在 check_blocked_threads 内部递减；ticker 可被饿死）→
  JVMSPI_CheckEvents 3 秒 0 次 → 整机表现卡死

### 修复（v01.83）
1. **phoneme-cldc Scheduler.cpp**（`#if defined(VITA)` 内，最小 diff）：
   `wake_up_timed_out_sleepers` 加墙钟兜底——`vita_now - vita_last_pump_ms >= 50`
   则无条件驱动 `check_blocked_threads(0)`。泵内 ANI 等待会真睡 50ms，
   生灭环被节流、ticker/watchdog 不再饿死、事件恢复流转
2. **OS_vita.cpp**：`vita_tick_count`（extern "C" volatile）计数 real_time_tick
3. **vita_watchdog.c**：HANG 报告加 `tick=%u`——未来若再挂，一条日志即可分辨
   "ticker 死了"vs"门没开"

### 构建/产物
- VM：`./rebuild_vm.sh`（33 对象，库守卫全过：jvm_fast_globals 单 D、无 C 解释器泄漏）
- VPK：`out/vpk/midp_vita_v01.83_pump_backstop.vpk`（3639606 B，
  md5 7c43c420624d495490aa83d8a4380850）
- 提交：phoneme-cldc 57a1b3d（Scheduler+OS_vita，+21/-1）；
  j2me 主仓 dd73f18（watchdog+CMakeLists 版本注释）

### 复测判据（真机）
- 正常轮次应能跑完（runMidlet returned 首次出现）
- 若仍挂：watchdog.log 的 HANG 行 `tick=` 值——tick=0 → ticker 死；
  tick 增长 → 门未开（本修复应已免疫）；且 v01.83 起泵每 50ms 必进，
  `ce=` 不应再长时间冻结

### 遗留
- 生灭环的"第一因"（哪个 MIDlet 线程反复生灭）未从转储直接观测到
  （Java 堆不在转储中）；兜底修复把环节流到可运行，若复测仍异常，
  下一步可在桩入口加生命周期计数器

## 2026-09-20 v01.84：log10 案情反转 + site ring 取证装置（118f8fd / phoneme-cldc ffdc429）

### v01.83 真机验证结果：未修复，仍挂死
- watchdog.log：`HANG ce=337 polls=12 tick=745`，双时钟活（d=100, kd=100252），
  vm RUNNING wait=none——形态与 log8/log9 完全一致
- 会话时长 7.5s（hb#1 t=1789783837332 → HANG t0=1789783844805）；
  **tick=745 × 10ms ≈ 7.45s ≈ 全会话 → cldc_ticker 从头活到尾**

### 案情反转（推翻 log9 的"ticker 饿死"推论）
- tick 在涨但解释器没消费：说明解释器 3 秒没跑过字节码（方法入口 +
  RESCHEDULE_COUNT 双检查点都没过）
- CE #337 冻结 3.2s → `yield()` 没被调用过（v01.83 兜底在 yield 内，
  反汇编证实已链接：wake_up_timed_out_sleepers@0x8108326c、50ms 分支@+0x1b0）
- **新结论：环 = green 线程 Java 堆栈上的原生 C++ 自旋**——OS 栈只见
  分发帧（pc=link stub+4，sp 距栈底 0x1050），Java 堆不在转储 →
  环身份从转储物理不可见，已到信息极限
- 残留帧（vfprintf→sscanf→JVMGlobals::parse_argument→__sinit）与 log9 的
  类加载 fread 链不同——旧帧被覆写，仅说明"早期启动路径走过 stdio"，
  不能定位环

### log10 转储解析增量（相对 log9）
- **双 slide 发现**：text slide=+0x1c000，data slide=+0x20000（不同！）
  验证锚点三件套：ticker pc 落 sceKernelDelayThread 桩+8 ✅、
  主线程 r5=gp_base+0x20000 ✅、VM pc/lr 落 stub+4/stub+0 ✅
- NOTE 布局与 log9 不同（线程表@0x694 步长 0xc8、watchdog 寄存器@0x2300、
  主线程寄存器@0x1e90）——**每份转储都要重新定位，不能套用**

### v01.84 site ring 装置
- `vita_site_mark()`（OS_vita.cpp，extern "C" noinline）：16 槽环形缓冲 +
  `vita_site_seq`，存调用者返回地址（反汇编验证：`str lr, [..]` 原样入槽，
  无 bl 冒充）；零锁零分配，单写者=VM 线程
- 打点 8 处（每点 `#if defined(VITA)` 两行）：Thread::finish、
  force_terminated、Thread::lightweight_thread_exit、thread_task_cleanup、
  Task::cleanup_terminated_task、ObjectHeap::collect、Verifier::verify_class、
  JarFileParser::open_entry
- **故意不打 Scheduler::yield**——log10 证明环绕过 yield，打了只会淹没 ring
- 声明集中 `Globals_vita.hpp` 尾部（每个 VM 翻译单元经生成 .incl 链第 7 行
  包含它，share/ 打点无需新头文件）
- `vita_site_dump()`：sceIo* 直写（绝不用 stdio——log10 残留帧恰指向
  stdio 域，取证工具不进案发现场）；输出 `self=` 锚点 + frozen 判定 +
  16 槽（最新在前）
- watchdog HANG 分支在 coredump 请求**之前**调
  `vita_site_dump("ux0:/data/J2ME00001/site_ring.log")`

### 构建注册方式（本轮踩坑记录）
- 新 .cpp 文件无法直接进构建：对象清单由 includeDB→makedep 生成，
  `<os_family>` 宏替换登记，无通配入口 → ring 实现并入 `OS_vita.cpp`
  尾部（它是 `OS_<os_family>.cpp`，必被编译），独立文件已删
- 打点对象在 _MergedSrc002(Scheduler)/003(ObjectHeap,Task)/004(Thread,
  Verifier)/006(JarFileParser)；改后 rm 对应 .o 重编（33 对象，库守卫全过）

### 构建/产物
- VPK：`out/vpk/midp_vita_v01.84_site_ring.vpk`（3640814 B，
  md5 90e94fe1935df7ade3c1e670a6e6ce27）
- 符号验证：vita_site_mark@0x1020556b8 / dump@0x1020551b8 /
  seq@0x102793840 已链接进 velf
- 提交：phoneme-cldc ffdc429（7 文件 +98）；j2me 主仓 118f8fd（+31）

### 复测判据（真机）
- 正常轮次跑完（runMidlet returned）→ 修复顺带生效（可能性低）
- 仍挂死 → 取回 `site_ring.log`：
  - `frozen=yes` + 尾部重复地址 = 环入口 → `addr2line -e midp_vita.velf`
    直接得函数名（self= 锚点可校准 slide）
  - `frozen=NO`（seq 仍在涨）= 环经过某个打点 → 重复的地址即环路径
  - 全空 ring = 环在 8 个打点之外 → 下一轮加桩（候选：InterpreterRuntime
    入口、EventFlag 等待路径）

## 2026-09-20 v01.85：log11 ring 开口——自旋锁定在字节码验证器内部（c086d73 / phoneme-cldc 8c82c4b）

### log11 取证结果（v01.84 装置首次命中）
- `site_ring.log`：`seq=10 frozen=yes self=0x8105a178`
- **self= 锚点定 slide**：vita_site_dump 运行时 0x8105a178 ↔ 静态 0x1020551b8
  → slide=0x80FFB040（静态 = 运行时 + slide）——比 NOTE 段模块基址推法更直接可靠
- ring 解码（时间序旧→新）：`open_entry → verify_class → open_entry×5 →
  verify_class → open_entry → verify_class(最后)`
- 两个地址符号化：`0x810aa8b8 = JVMVerifier::verify_class+24`、
  `0x810bb098 = JarFileParser::open_entry+28`——正常类加载节奏

### 定案
- **最后事件 = verify_class 进入，此后 3+ 秒零打点**（无 GC collect、
  无线程死亡路径、无下一次 open_entry）→ **自旋在 verify_class 子树内部**
- core11 佐证：VM 线程 pc 仍为 `start_lightweight_thread_asm+0x14`
  （分发桩帧，green 线程在 Java 堆栈执行，转储不可见）；主栈残留帧扫描
  （0x8137f000-0x81381000 全窗口）只有更早的类加载链
  （PNG/Inflater/parse_stackmaps/open_entry/fread），**零验证器帧**
- watchdog：HANG ce=347 polls=12 tick=726（7.26s 会话，ticker 全程活）
  ——与 log10 同形态，再次确认 tick 交付但未消费
- vm_output.log 显示 MIDlet 构造完成两轮 startSuite 后冻结——自旋发生在
  startApp 触发的后续类加载验证中

### 代码走查排除（verify_class 子树内的循环）
- `GetInstructionStarts::run`（wide_len≤0 有 `goto error` 保护）
- `Method::iterate`（第一遍扫过则第二遍同序推进）
- `populate_stackmap_cache`、`check_handlers`、`check_news`、
  `SignatureStream::next`、`write_short/long_map`、`compress_verifier_stackmaps`
  ——全有界
- 剩余嫌疑（无法静态定案）：`check_stackmap_match` 对每分支目标做
  stackmap 线性扫描（O(方法数×分支数) 平方复杂度）、巨方法/巨 switch 的
  验证循环、`SystemDictionary::resolve` 触发的类加载递归（ring 只看到
  verify_class 内层，resolve 引发的 open_entry 也该留痕——但没有，
  故 resolve 排除）

### v01.85 验证器深追装置
- `vita_site_verifier_class(name)`：verify_class_internal 入口记录类名
- `vita_site_verifier_method(name)`：VerifyMethodCodes::verify 入口记录方法名
- `vita_site_dump_verify(path)`：HANG 时追加 `verify class=... method=...`
  行到 site_ring.log（watchdog 调用）
- ring 加两个阶段锚：verify_class_internal 进入 + 返回、iterate 开始
  ——下一轮 ring 能判"卡在 verify 哪个阶段"
- 名称缓存 64 字节 static（无锁，单写者 VM 线程；Symbol::base_address()
  直拷，NUL 截断）

### 构建/产物
- VPK：`out/vpk/midp_vita_v01.85_verifier_trace.vpk`
  （md5 a50e8fc792fef92af7b2ce009da8a728）
- velf 符号：vita_site_verifier_class@0x1020556f0、method@0x102055738、
  dump_verify@0x102055788
- 提交：phoneme-cldc 8c82c4b（4 文件 +79）；j2me 主仓 c086d73（+20）

### 复测判据（真机）
- site_ring.log 应出现 `verify class=X method=Y`——直接点名肇事类与方法
- ring 尾部：最后事件若仍是"verify_class 进入后静默"，配合类名可定位
  具体方法代码（反编译该 jar 的该方法，查巨 switch/巨 stackmap）
- 若 ring 显示 verify_class 返回后静默 → 自旋在
  compress_verifier_stackmaps 或 set_verified 之后的下一环

## 2026-09-20 v01.86：log12 定案——UseVerifier 默认 true 是根因，已关闭（ca95a95 / phoneme-cldc 43eda74）

### log12 取证结果（v01.85 装置命中）
- `site seq=51 frozen=yes self=0x810b0188`，slide=0x80FA5040
- ring：`[0]=0x81100aa4`（最新）、`[1..12]=0x8110079c`（**同一地址重复 12 次**）、
  `[13]=0x81100840`、`[14]=0x81100a3c`、`[15]=0x81111220`
- `verify class=com/app/filemanager/UI<乱码> method=<clinit>`
- watchdog `HANG ce=348 polls=12 tick=733`（ticker 全程活）
- 乱码字节 = v01.85 Symbol 拷贝 bug 实证（见下）

### 根因定案（触发链走查 + Globals.hpp 实证）
- **`UseVerifier` 默认 true**（`phoneme-cldc/src/vm/share/utilities/Globals.hpp:292`）
- `Universe::load_jar_entry`（Universe.cpp:1132）：`if (UseVerifier || VerifyOnly)
  instance_class.verify()` → **每个从 jar 懒加载的类都在 VM 线程上被验证**
- Java 侧全部排除：`VERIFY_ONCE=false` 使 Installer/SuiteVerifier/AppInfo 的
  验证路径全部编译 out 或跳过；普通类加载（Class.forName → SystemDictionary::resolve
  → load_jar_entry）正是懒加载验证路径——此前"普通加载不验证"的结论是错的
- 12 次连续 mark = 游戏运行中 12 个类依次懒加载+验证；`UI*` 的 `<clinit>`
  （UI 资源初始化，典型巨方法）撞上验证器平方复杂度
  （`get_stackmap_index_for_offset` 每分支目标线性扫 stackmaps，HotRoutines.incl.hpp:234）
  → 验证耗时数秒 → watchdog 判 HANG
- **不是死循环，是病态慢**：验证在 VM 线程内同步执行，green 线程调度没机会跑，
  完美解释 log9→log12 全链（ticker 活、CE 冻结、yield 不被调、coredump/OS 栈零验证器帧）

### 修复（两仓）
1. **phoneme-midp ca95a95**：`midp_run.c` 在 `midpRunVm` 前
   `JVM_SetUseVerifier(JNI_FALSE)`（`#if defined(VITA)` 隔离，+13 行）。
   MIDlet jar 均经 WTK preverify（J2ME 规范），运行时再验冗余。
   `#if (VERIFY_ONCE)` 的保存/恢复块在本构建编译 out，无人再打开它
2. **phoneme-cldc 43eda74**：修 Symbol 拷贝 bug——Symbol 是长度前缀存储、
   非 NUL 结尾（Symbol.hpp: `length()`/`base_address()`），v01.85 的
   `for (i<63 && name[i])` 会越过名字结尾读入相邻符号数据。
   `vita_site_verifier_class/method` 改为显式传 `n().length()`（4 文件 +27/-15）

### 构建/产物
- rebuild_vm.sh：33 对象 OK（loopgen 链接失败=已知无害）；库守卫
  `jvm_fast_globals`=1、`interpreter_dispatch_table`=0 通过；
  新签名 `vita_site_verifier_class/method` 已在 libcldc_vm.a
- VPK：`vita-port/midp_vita_v01.86_noverify.vpk`
  md5 `4d0112f0f0278c92d0ae97c5ccd365bb`，版本串 `J2ME Player v01.86 b274 (9945938)`

### 复测判据（真机）
- **主判据**：原挂死游戏能过轮次不再冻结 → 根因修复生效
- site_ring.log 不应再出现 verify_class 打点（验证器已关）
- 若仍挂：ring 数据照旧可定位新肇事点（装置保留，打点仍在）
- 遗留风险：若某游戏 jar 未 preverify（罕见），类加载时会抛
  VerifyError 而非静默通过——届时看 vm_output.log 即可定位

## 2026-09-20 v01.87：log13 定案——v01.86 从未编译进 VPK（JNI_FALSE 编译错误被遗漏）

### log13 取证结果（v01.86 VPK 真机测试）
- boot_log 确认跑的是 `J2ME Player v01.86 b274 (9945938)`
- site_ring.log：`verify class=com/app/filemanager/UI method=<clinit>`
  —— 类名干净（v01.85 Symbol 长度 bug 修复已生效，cldc 侧是新的），
  但验证器仍在跑，12 次重复 mark 模式与 log12 完全相同
- watchdog HANG（ce=342 polls=12）→ coredump，与 log9..12 同族

### 根因（构建链断层，非 VM 逻辑）
- `midp_run.c:873` 的 v01.86 改动用了 `JNI_FALSE`，**midp 侧没有这个宏**
  （只有 kni.h 的 `KNI_FALSE`）→ 单对象重编报
  `'JNI_FALSE' undeclared` 编译错误
- 错误未被察觉，`libobj.a` 里的 `midp_run.o` 保持 9 月 14 日旧对象
  （比 v01.86 源码 edit 旧 5 天）→ VPK 链接的还是没有
  `JVM_SetUseVerifier` 调用的旧代码
- 时间戳证据链：midp_run.o = Sep 14 05:17；midp_run.c = Sep 19 09:06；
  libobj.a = Sep 16（内含旧对象）

### 修复（phoneme-midp bd6f30d）
- `midp_run.c:873`：`JNI_FALSE` → `KNI_FALSE`，注释记录 v01.87 教训
- 重编流程：`rm build/vita_arm/obj/arm/midp_run.o && ./build_vita.sh`
  （教训重申：单对象重编必须先 rm 旧 .o）
- `libmidp.so` 链接失败（sceAppMgrGetDevInfo / _rom_linkcheck 未定义）
  是**既有独立问题**，vita-port 不链 libmidp.so，直接用 libobj.a，无害
- 验证闭环：`midp_run.o` 成员含 `U JVM_SetUseVerifier`；
  `libobj_no_main.a`（vita-port 链接用）同含

### 构建产物
- VPK：`vita-port/midp_vita_v01.87_noverify2.vpk`
  md5 `42d2f0864560a548f990df1246a70fe1`，版本串
  `J2ME Player v01.87 b275 (6fa4282)`
- velf 符号：`vita_site_verifier_class`(0x102055708) /
  `vita_site_verifier_method`(0x102055774) 打点保留

### 教训（写入流程纪律）
- **改 midp 侧源码后必须跑 `build_vita.sh` 并检查编译错误**——
  编译失败会把旧 .o 留在 libobj.a，VPK 静默链接旧代码
- 判断"修复是否上机"不能只看版本串（版本头是 vita-port 侧生成的，
  与 midp/cldc 对象新旧无关），要核对对象时间戳 + nm 符号
- midp 侧布尔常量用 `KNI_TRUE/KNI_FALSE`（kni.h），不是 JNI_*

## 2026-09-20 v01.88：log14 定案——验证器确认已关，自旋转移进 <clinit> 执行

### log14 取证结果（v01.87 VPK 真机测试）
- boot_log：`J2ME Player v01.87 b275 (6fa4282)` 确认新 VPK 在跑
- **site_ring.log：`verify class=(none) method=(none)`——验证器确认关闭**（v01.87 修复生效）
- ring 仅 seq=3：3 个 mark 全是 `JarFileParser::open_entry+0x1c`
  （0x8111b280，slide=0x80F9B040 由 self= &vita_site_dump 换算）
- **游戏正常跑了 345 帧（~5 秒，HEARTBEAT/input 日志 + 345 次动画心跳）**，
  用户按键 2 次后冻结 → watchdog HANG（ce=345 polls=12）→ coredump
- THREAD_REG_INFO 解析（NOTE@0x1e90，5 线程 × 0x178）：VM OS 线程
  PC=LR=`start_lightweight_thread_asm`（泊位，与 log11 相同）→
  **自旋在绿色线程的 Java 栈上，coredump 不含 Java 堆，无法直接定位**

### 结论
- log12 的"验证器平方慢"只是第一层：关掉验证器后冻结依旧，但时间点
  从启动期移到游戏运行 5 秒后。第 4 个懒加载类的 `<clinit>` 执行本身
  死循环（或病态慢）——之前的验证器抽象解释同样会走进该循环，所以
  log9~13 的"验证冻结"其实是同一个 `<clinit>` 病毒码的两次发作
- crumb 的 `vm UseCompiler=1` 是 runMidlet 前读的默认值；runMidlet 内
  `-int` 解析后 JIT 实际关闭（v01.46 起默认），JIT 嫌疑排除

### v01.88 取证装置（phoneme-cldc d16d385 / vita-port efb956c+8c9f）
- `InstanceClass::clinit()` 头部：记录类名 + ring mark
  （照 v01.85 verifier 模式，显式长度拷贝）
- `OS_vita.cpp`：`vita_site_clinit_class(name,len)` +
  `vita_site_dump_clinit()`（watchdog HANG 时追加 `clinit class=...` 行）
- `vita_watchdog.c`：HANG 时调用 dump_clinit
- CMakeLists：VPK 输出统一到 `out/vpk/`
  （`midp_vita.vpk` + `midp_vita_v<ver>.vpk`，独立 `vpk_out_copy`
  target 依赖 VPK 目标——POST_BUILD 挂可执行目标会在打包前跑，
  拷到上一次的 VPK，md5 比对抓出）

### 构建产物
- `vita-port/out/vpk/midp_vita_v01.88.vpk`
  md5 `34ac7d0e23aaf9c5c87bc418b5437dd2`，
  版本串 `J2ME Player v01.88 b280 (efb956c)`
- velf：`vita_site_clinit_class` @0x102055868 打点在位

### 复测判据（v01.88）
- site_ring.log 若出现 `clinit class=<肇事类名>` → 直接定案，
  下一步针对该类做超时/补丁
- 若 `clinit class=(none)` 但 ring 3 个 mark 不变 → 卡点在
  open_entry 与 clinit 之间的 native 解析段（类文件 parse），需
  给 ClassFileParser 加打点

## 2026-09-20 v01.89：log15 定案——肇事类点名：UI.<clinit> 类加载风暴

### log15 取证结果（v01.88 打点命中）
- boot：`v01.88 b280 (efb956c)`；游戏跑 336 帧（~5 秒）后按键冻结
- **site_ring.log：`clinit class=com/app/filemanager/UI`** —— 肇事类定案
- ring（seq=38）：13 次 `JVMInstanceClass::clinit+0x34` 交替
  3 次 `JarFileParser::open_entry+0x1c`
  （slide=0x80f9f040，由 self=0x810b61a8 ↔ dump 静态 0x1020551e8 换算）
- core15 寄存器组：VM OS 线程 PC=`start_lightweight_thread_asm+4`，
  栈上 `JVM::start+0x178` ← `JVM_Start+0xa4` —— 与 log11/14 相同泊位
- Java 堆不在 coredump（搜类名只命中 dump 缓冲自身）

### 定案
- **`com/app/filemanager/UI` 的静态初始化器永不返回**：执行中不断
  触发新类的懒加载（clinit↔open_entry 交替），形成类加载风暴
- 与 log12 闭环：验证器开启时对同一 `<clinit>` 做抽象解释同样走进
  该路径（表现为"验证平方慢"）；关闭后真执行照样发作。一个病毒
  `<clinit>`，两种发作形态
- ring 16 槽内 13:3 的 clinit:open_entry 比例 + seq=38（5 秒内 38 个
  事件）说明风暴在进行中，非单次卡死

### v01.89 装置（phoneme-cldc f3ff88e / vita-port 84767d9）
- 16 槽 clinit 历史 ring（48 字符槽）+ count/depth/max_depth 计数
- `set_initialized()` 挂 `vita_site_clinit_return()`：深度递减，
  可区分"顺序 clinit 风暴"（depth 低 count 高）vs"递归 clinit 链"
  （depth 高）
- HANG dump 输出：`clinit stats count=N depth=D max=M` + 16 行历史

### 构建产物
- `vita-port/out/vpk/midp_vita_v01.89.vpk`
  md5 `dd909e82455c6c4f44dd4f22ef5f915b`，
  版本串 `J2ME Player v01.89 b283 (84767d9)`

### 复测判据（v01.89）
- `clinit stats depth=` 高（>10）→ UI.clinit 递归触发链，看历史
  ring 找环（A→B→A）→ 需要懒加载改造或 jar 预全量加载
- `depth` 低但 `count` 高 → 顺序风暴（UI.clinit 内部循环 new 类），
  可能是游戏代码病态（如反射/Class.forName 循环）
- 下一步修复候选（定案后实施）：
  (a) 预先全量加载 game.jar 所有类（启动时一次解压+定义，消除懒加载
      风暴面）——最贴合 phoneME 设计（UseROM 的文件系统版）
  (b) 若历史 ring 显示环，对该 jar 启用-clinit递归保护

## 2026-09-20 v01.90：watchdog 进度感知——风暴≠挂死（真机/模拟器差异的对策落地）

### 动机（log16 复盘 + 用户三问）
- 第 3 次按键（▲UP=菜单导航）触发 UI 类懒加载 → 类加载风暴
  （clinit 历史：UI←MenuCanvas←FileManagerMIDlet...，count=35）
- 真机卡死的本质是**病态慢**（真实 ux0: I/O + 软件 inflate × 35 类），
  而模拟器（x86 JIT + 宿主文件系统）亚秒完成、旧手机 KVM 整 jar 预载
  内存，都"看不见"这个风暴
- 旧 watchdog 3 秒判死 → 主动 data abort → 用户看到的"闪退"其实是
  我们自己杀的进程

### v01.90 改动（phoneme-cldc 34ecb7c / vita-port 861de1b）
1. **修正 v01.89 depth 伪数据**：return 钩子从 set_initialized() 移到
   Java_java_lang_Class_init9()——后者才是 clinit 真正完成点
   （java.lang.Class.invoke 在 <clinit> 跑完后调它）；前者被
   bootstrap_initialize()（ROM 类）也调用，偷跑递减导致 log16
   depth=0 假象
2. **vita_gc_count**：GC pass 计数（ObjectHeap::collect 入口）——
   stop-the-world GC 自旋对现有所有取证装置不可见（coredump 无
   Java 堆），至少能在 HANG dump 里点名"GC 在跑、跑了多少轮"
3. **watchdog 进度感知**：CE 冻结但 clinit/gc 计数在前进 = 风暴进行
   中，不杀；上限 WD_STORM_MAX_POLLS=120（~30 秒）后仍前进才判死。
   STORM 事件记一行日志（ce/clinit/gc 快照）
4. clinit/gc 计数从 static 改导出（Globals_vita.hpp 声明）

### 行为变化（用户可感知）
- 真机风暴期间：游戏最多卡 ~30 秒后要么恢复（风暴收敛，类缓存热了
  后续不再卡）要么才被 dump——不再 3 秒闪退
- watchdog.log 出现 `[WD] STORM ce=N clinit=M gc=K` 行 = 风暴实录

### 构建产物
- `vita-port/out/vpk/midp_vita_v01.90.vpk`
  md5 `ce69ab3a34486049942f7d12747ece9d`，
  版本串 `J2ME Player v01.90 b285 (861de1b)`

### 复测判据（v01.90）
1. 游戏第 3 键后卡 ~5-30 秒**然后恢复** → 定案"病态慢"，终极修复
   方向=jar 全量预载（v01.91 候选）
2. 卡满 30 秒被 dump → 看 clinit stats：count 涨到多少（35→N）；
   gc= 大数 → GC 自旋主导，需查堆压力
3. depth 正常应 ≥1（UI.clinit 未返回时冻结）；depth=0 且 count 不涨
   → 自旋在 clinit 之外的纯字节码/native/GC 区，看 gc 计数分流

## 2026-09-20 log17 + v01.91：指令级定案 + 启动期全量预载（终极修复）

### log17 取证（v01.90 真机复测）
- 现象：依旧卡顿后立即闪退；无 STORM 行，`HANG ce=340 polls=12`（3秒即杀）
- **STORM 假设被证伪**：count 冻结在 35、gc=0 —— 计数完全不动 = 真死锁
  非"病态慢"。watchdog 进度感知逻辑本身无 bug，只是计数不动
- coredump 首次取得**非泊位冻结点**：VM 线程 PC=`invoke_pending_entries+0xc`
  LR=`start_lightweight_thread_asm+0`

### 指令级解码（本轮方法论成果，务必复用）
1. **地址域勘误**：velf symtab 用链接域 `0x102xxxxxx`，section 头用
   `0x81xxxxxx` 域（差 0x81000000）。真机 slide 很小：
   `slide = ring self(0x81070278) − ELF 里 vita_site_dump(0x81055278)
   = 0x1B000`。**换算公式：静态域 = 运行时 − 0x1B000**（此前用
   0x80fe5040 是域混淆，符号相对关系恰好没算错纯属侥幸）
2. 验证方法：`bash CMakeFiles/midp_vita.dir/link.txt` 重链出原始 ELF
   `midp_vita`（其符号即静态域），objdump/nm 均可用
3. **THREAD_REG_INFO 解析**（note desc @0x1e90）：`ver,count,esz` 头，
   条目 = tid(4B) + r0..r15(64B)。SP=w[13] LR=w[14] PC=w[15]
   （w 从 tid 之后数）
4. THREAD_INFO（@0x684 note）：ver=18 count=5 esz=0xc8；条目 = tid +
   32B 名字 + 状态字。`0x1`=RUNNING、`0x80000008`=WAITING(delay)
5. core17 五线程全景：VM 停在 cmp r1,#0（r1=0 必然立即返回——不可能
   自旋于此）；j2me_tone/cldc_ticker 正常 WAITING(delay)；watchdog
   自己 RUNNING；SceCommonDialogWorker 寄存器垃圾（采样残留）

### 定案：绿色线程生灭风暴（churn）+ clinit/open_entry 交替链
- `setup_stack_asm`（InterpreterRuntime_arm.cpp:124）给每个新绿色线程
  栈底预置 `start_lightweight_thread_asm` 为返回地址 → 三次 dump
  （log9/10/17）全撞在首调度路径头部 = VM 线程高频反复走线程启动
- **烟枪**：site ring 冻结时 16 槽只有两个地址——
  `JVMInstanceClass::clinit+0x34` ×13、`JarFileParser::open_entry+0x1c` ×3
- 机制：按键 → UI 懒加载链（clinit→open_entry→define→下一 clinit）
  走**纯 native pending-entry 调度循环**——不经解释器 tick 检查、
  不经 yield 的 pump backstop（ani_mark 停在 `CE #340 ANI OUT`）、
  不经任何打点 → CE 饿死 → 输入无响应 → watchdog 3 秒杀
- log16/log17 两次独立运行 ring 完全一致 = 确定性卡死非偶发竞态
- 三方环境差异定论：模拟器（x86 JIT+宿主fs 亚秒消化）/旧手机 KVM
  （整 jar 预载 RAM+无 watchdog）/真机（真实 I/O+逐类懒加载+激进
  watchdog）——只有真机踩中这条路径

### v01.91 改动（phoneme-cldc ec747ab / vita-port 5b1e0c7）
- `JVM.cpp` load_main_class：VITA 下先 `Universe::load_all_in_classpath()`
  （上游 romizer 同款路径，ENABLE_PERFORMANCE_COUNTERS=1 使
  USE_JAR_ENTRY_ENUMERATOR=1，函数本就在 velf）——bootstrap 完成、
  app_classpath 就绪、首字节执行前。**只 define 不 initialize**
  （clinit 链留在解释器有 tick 检查；后续 resolve 全命中字典不再碰盘）
- `Universe.cpp` load_jar_entry：成功 define 后 `vita_preload_count++`
- `OS_vita.cpp/Globals_vita.hpp`：定义+导出 `vita_preload_count`；
  site dump stats 行加 `preload=%u`
- `vita_watchdog.c`：STORM 分支并入 preload 计数；**preload 前进 =
  启动期有界扫描，无截止等待**（大 jar 真机 I/O 可超 30 秒）；
  clinit/gc 风暴维持 30 秒收敛上限不变；STORM 行加 preload=%u
- OOM 自适应降级：load_jar_entry 内部清异常跳过该类，装不下的
  自动回退 lazy 加载

### v01.86 教训的变体（第二次踩，引以为戒）
- `_MergedSrc005.o` 时间戳 03:01（v01.90 构建），改 JVM.cpp（05:45）
  后 make **不重编**——`.incl` 生成文件不更新，make 不跟踪其对源文件
  的直接 #include 依赖。**改 share/ 下源码必须先 rm 对应
  _MergedSrc*.o**（本轮：rm _MergedSrc004.o _MergedSrc005.o OS_vita.o）
- 验证方法：`arm-vita-eabi-objdump -dr _MergedSrc005.o | grep -c
  "R_ARM_CALL.*load_all_in_classpath"` 必须 ≥1（第一轮=0 抓了个正着）

### 构建产物
- `vita-port/out/vpk/midp_vita_v01.91.vpk`
  md5 `8630deb58eafa6c053e214f9955694aa`，
  版本串 `J2ME Player v01.91 b288 (222b42e)`（velf grep 验证；
  注意 222b42e 与 5b1e0c7 代码相同，仅多记忆文档提交）
- 库守卫：`vita_preload_count`(B) + `load_all_in_classpath`(T)
  在 libcldc_vm.a 与 velf 均就位；`jvm_fast_globals` 无重复定义、
  `interpreter_dispatch_table` 未泄漏（守卫计数 0）

### 复测判据（v01.91）
1. vm_boot.log 出现 `[preload] begin/done` 两行 → 预载路径执行
2. 启动变慢数秒属预期（全 jar define），watchdog.log 若出现
   `STORM ... preload=N` 且 N 在涨 → 预载被正确宽容
3. **第 3 键不再卡死**（clinit/open_entry 触发面已消灭）→ 案结
4. 若仍卡：ring 里 clinit/open_entry 应消失，新冻结点直接点名剩余
   问题；preload 若卡在中间不动 → resolve 死循环，看 dump 的类名

---

## 2026-09-20 v01.92：log18 取证结案 —— NATIVES dump 洪水打瘫 ux0: 存储

### 根因（coredump 三重铁证，非猜测）
1. VM 主线程冻结点 PC=sceIoWrite+8（静态 0x81106b98），LR=_write_r+0x37
   （紧跟 `blx sceIoWrite`@0x810fe236）
2. coredump 内存 r0=0x813bf590 缓冲区内容 =
   `POST_NATIVES idx=166 oop=0x812143f8 klass_bytes=[4d 6f 64 65 15 00 ...]`
3. midp_stderr.log 大小 0xCECC=52940 字节（log10~17 恒 1644），713 行
   NATIVES dump，冻结时正在写最后一行

### 因果链
v01.91 预载 sweep 把 8 个 ROM 常驻 natives 系统类拉进运行时
ClassFileParser → ClassFileParser.cpp 历史遗留调试 dump（016e041 带入，
上游没有）对每个 natives 类**每个常量池 Utf8 条目**无条件 fprintf 一行
（PRE_NATIVES/POST_NATIVES 两个循环块）→ stderr 无缓冲、每行一次独立
sceIoWrite → 数百次背靠背小写打瘫真机 ux0: 存储栈 → 冻在 syscall 内 →
watchdog 3 秒 HANG 取证杀进程。Vita3K 宿主 FS 微秒级延迟，从不复现。

### 修复（v01.92，已提交 cldc 6c601f5 / vita-port 1a81fbf）
- ClassFileParser.cpp：删 PRE_NATIVES/POST_NATIVES 两个无条件 dump 循环，
  **保留** register_natives_for 与 BEFORE/AFTER_NATIVES 单行、保留条件式
  CORRUPT 扫描块（静默，无 I/O 洪水风险）
- JVM.cpp 预载注释重写为 log18 实况（565 行 clear 在 apocalypse 路径，
  预载块从来没有 clear——此前注释有误导，已修正）
- vita_watchdog.c：preload 宽容改为**有界**（extension 计数 ≤
  WD_STORM_MAX_POLLS，~30s），"preload 在涨"不再能无限豁免死锁
- 预载 sweep 本身保留（preload 28→303 正常推进，是对 log16/17 的正确解法）

### coredump 解析方法学（复用资产）
- `.psp2dmp` 是 **gzip 流**（magic 1f8b）→ gunzip 得 5MB ELF32 ARM core
- THREAD_REG_INFO note：ELF phdr[7] off 0x1e74，desc@0x1e90，
  ver=17/count=5/esz=0x178；线程条目按 CPSR 签名 0x800f0010 扫描定位
- 工具：`debug/tools/parse_core18.py`（phdr/notes）、`parse_core18b.py`
  （寄存器+CPSR 签名法）；解压产物 /tmp/core18/core
- **slide 每个构建都不同**：v01.91=0x59000（log17=0x1B000）。校准公式：
  `site_ring.log self 值 − vita_site_dump 静态地址(0x810552d8) = slide`。
  每个新 coredump 必须重标，不可沿用旧值
- 符号化：`arm-vita-eabi-addr2line -f -C -e vita-port/midp_vita <静态地址>`
  （v01.91 的 ELF 是 Sep 20 05:56 那份）

### 构建产物（v01.92）
- `vita-port/out/vpk/midp_vita_v01.92.vpk` md5
  `37e759fa0f3cea31a9a7549ca1c742ba`，velf 版本串 01.92×2
- 库验证：`arm-vita-eabi-strings -a libcldc_vm.a | grep -c PRE_NATIVES`
  = 0、POST_NATIVES = 0、BEFORE_NATIVES = 1（保留项在位）
- **loopgen 链接失败是已知噪音**：rebuild_vm.sh 的 `|| true` 容忍 ARM
  对象参与宿主工具链接（fast_memclear 等 undefined），产物看 repack 成功
  与守卫计数，不要被吓到；宿主工具坏了单独跑 `./rebuild_vm.sh tools`

### 复测判据（v01.92）
1. vm_boot.log 出现 `[preload] begin` **和** `[preload] done` 两行
2. midp_stderr.log 回到恒定 ~1.6KB，0 行 NATIVES
3. 进游戏第 3 键不再卡死；watchdog.log 无 HANG
4. 若仍卡：ring/clinit 应不再是 open_entry 链，看新冻结点

## 2026-09-20 v01.93：log19 指令级定案 —— 泵兜底挂在 yield 上，绿线程自旋根本到不了（cldc f95de68 / vita-port d5916a5）

### 先说清一个事实：v01.92 修复被 log19 证明**生效**
- vm_boot.log `[preload] begin` + `done` 两行齐
- midp_stderr.log 3924 字节、29 对 BEFORE/AFTER_NATIVES、**零行** NATIVES dump
- watchdog 只有一行 STORM（preload=28），preload 计数 864 正常推进后转 HANG（有界宽容按设计工作）
- log18 那层（I/O 洪水）已关闭。log19 的卡死是**另一层**：log9/16/17 同族自旋，这次无 I/O 干扰、现场最干净

### log19 现场指令级铁证（**v2 更正**：parse 工具修复后重解出正确归属）
- **更正**：最初把 PC=0x810d04c4 标成 `start_lightweight_thread_asm+0x10` 是**错的**——
  那次是拿错 ELF（v01.91 布局）+ 拍脑袋 entry=PC-0x10 的产物。用 v01.92 ELF +
  slide=0x15000 + nm 二分（parse_core18b.py 修复版，objdump 逐条核对）：
  - 静态 PC=0x810bb4c4 = **primordial_to_current_thread+0x14**：`ldr r6,[r1,#100]`
    （jsp = THREAD->stack_pointer）；start_lightweight_thread_asm 实际从 0x810bb4d4
    才开始（v01.92 布局）
  - 静态 LR=0x810bb4b4 = primordial_to_current_thread+0x04（`push {lr}` 那条地址，
    不是"函数入口"也不是调用者返回地址）
  - **r1=0**：`_current_thread` 快速全局（jvm_fast_globals）在该瞬间为 NULL——
    PC 那条 load 若真执行会取 [0x64] 触发数据 abort；进程没死，说明是挂起快照点
- 冻结点在**绿线程切换桩（primordial_to_current_thread 序言）内**，而非
  start_lightweight 桩体内——仍在 spawn/die 自旋经过的同一套切换机制里，
  与"自旋不经 yield"诊断不矛盾；但旧解读"LR=入口=每次新调用"作废
- site ring 16 槽全部 = `JVMInstanceClass::clinit` 内 mark（seq=1048 冻结），连
- site ring 16 槽全部 = `JVMInstanceClass::clinit` 内 mark（seq=1048 冻结），连
  Thread::finish 的 mark 都没有 → 新线程在原生层即死
- clinit hist 35 条与 log16 **逐字节一致**，尾部 java/util/Timer；CE 冻在 #364 共 3.3 秒
- 其余线程健康：[4] cldc_ticker 在 delay 循环（ticker 活着、vita_tick_count 在涨）

### 根因（这次是钉死的，不是推断）
ARM 上 `start_lightweight_thread` 是生成器桩（`InterpreterStubs_arm.cpp:229`），
C 版被 `#if !defined(ARM)` 排除。游戏代码 `new Thread().start()` 的循环：
`Java_java_lang_Thread_start0`(Natives.cpp:1277) → `Thread::allocate`（栈帧种入
stub 地址）→ `Scheduler::start` → 返回解释器切到新线程 → 桩执行 → 新线程在
native 里死亡 → 再次 start0……**这条路径不经过任何 yield()**：
- v01.83 的 50ms 泵兜底挂在 `wake_up_timed_out_sleepers`，只有 yield()/wait 循环会调它
- ticker 置的 `_timer_has_ticked` 标志，消费点在**解释器 tick 检查点**→
  `Thread::timer_tick`→yield——只有执行 Java 字节码才到达；被 spawn 的线程还没跑
  字节码就死了，泵永远等不到消费者
→ CE 泵饿死 3.3 秒，watchdog HANG 取证。v01.83 修的是"自旋**经过** yield 但门不开"，
log19 是"自旋**根本不经过** yield"——同一症状，不同盲区

### 修复（最小 diff：3 文件 +45/-8，全部 `#if defined(VITA)`）
- `Scheduler.hpp/cpp`：把 v01.83 gate 抽成 `Scheduler::pump_events_vita()`
  （同一 50ms 节流、共享 static，声明放 public 区——注意类里 `private:` 在前）
- `Natives.cpp` `Java_java_lang_Thread_start0` 尾部调用它：每次 spawn 循环迭代
  必经此处；上下文安全（当前线程在 native 里 RUNNING，无半成品调度状态；
  JVMSPI_CheckEvents 只轮询原生队列并 unblock ASYNC 线程）
- 关键教训：**stub 是宿主生成器产物，改 `SharedStubs_arm.cpp` 进不了二进制**
  （`pack_lib()` 里 `Interpreter_arm.o`——全部桩所在成员——故意从旧库回填，
  因为重新生成的丢了 jvm_f2i/jvm_d2i 浮点桩）。插桩点必须选会被重编的 C++ 成员

### coredump 解析新坑（两个叠加差点误判）
- `debug/tools/parse_core18b.py` 路径**硬编码**（`/tmp/core18/core` + 旧 ELF），
  忽略 argv → 解析 log19 时静默返回 log18 旧寄存器。使用前必须 sed 替换路径
- CPSR 签名 `0x800f0010` **不通用**：log19 VM 线程 cpsr=0x60070010 扫不到。
  thread[0] 寄存器窗口在 note entry +0x08 起：sp=+0x38、lr=+0x3c、pc=+0x40、
  cpsr=+0x44
- log19 slide=0x15000（site_ring self 0x81068a38 − vita_site_dump 静态 0x81053a38）。
  符号化必须用**与现场同版本的 ELF**：v01.92 版 nm 校验锚点
  primordial_to_current_thread@0x810bb4b0；v01.93 的 build/midp_vita 布局已变
  （同地址是 png_set_quantize）。`parse_core18b.py <core> <elf> 0x1e74 0x15000`
  现已支持 slide + nm 符号表二分（addr2line 对 strip 的 velf 只回 ??，不可用）
- core 的 LOAD 段**不含 .data/.bss**（0x814bxxxx 不可读）：vita_clinit_hist/
  gc_count/preload_count 读不了，好在 site_ring.log 已打印全部

### 构建产物（v01.93）
- 重编：`rm _MergedSrc002.o _MergedSrc003.o`（Scheduler→002、Natives→003，用
  `target/generated/incls/__MergedSrc*.incl` 查归属）→ `./rebuild_vm.sh`
- 守卫：jvm_fast_globals 唯一 D 定义、C 解释器泄漏 0、
  `_ZN12JVMScheduler16pump_events_vitaEv` 在 002 定义 T / 003 引用 U 闭合
- `vita-port/out/vpk/midp_vita_v01.93.vpk` md5 `46ab6e0af78f856094b4b1fbc070af2a`，
  velf 版本串 01.93×2，`nm build/midp_vita | grep pump_events` = 0x81080af8

### 复测判据（v01.93）
1. 按键进游戏后不再 3 秒冻结；CE 计数持续单调涨
2. watchdog.log 无 HANG；若仍 HANG，新 core 的 ring 应不再是 clinit 满槽
   （pump_events_vita 生效后自旋会被 50ms 泵节流，ring 会看到新标记）
3. 若还挂：注意残留风险——若自旋是纯 stub ping-pong（既不回 start0 也不进
   wake_up_timed_out_sleepers），v01.93 的泵仍点不着；届时看新 core 的 PC 是否
   仍在 primordial_to_current_thread / current_thread_to_primordial 簇内，
   若是则下一步把泵点挂到这两条 stub 的 C++ 调用侧
   （Scheduler.cpp:1707 / JVM.cpp:280）。解析用修复版工具：
   `python3 debug/tools/parse_core18b.py <core> <elf> 0x1e74 0x15000`

---

## v01.94（2026-09-19）：log20 复测结论 + site ring 取证覆盖

### log20 取证（v01.93 复测，同一游戏 com.app.filemanager.FileManagerMIDlet）

- **v01.93 确认在跑**：boot_log "version: J2ME Player v01.93 b293 (f95de68)"。
- **修复起效证据（关键）**：ani_mark.log "CE #371 ANI OUT" vs v01.92 期 log19 的
  "CE #364 ANI OUT"——boot 期多泵了 ~7 次。v01.93 的 start0 泵在 spawn 循环里
  确实开火，**不是原地打转**。
- **但冻结同签名**：最后一个泵完整跑完 "ANI OUT"，下一个泵从未开始
  （CE #371 后冻结）→ 冻结循环**不再经过 start0**，比 log19 的 spawn 路径
  更深一层（残留风险第 3 条应验："纯 stub ping-pong 或其它不回 start0 的循环"）。
- **log20 证据面教训**：只有 4 个文件（boot_log/crumb/ani_mark/audio_debug），
  watchdog.log、site_ring.log、midp_stderr.log、psp2dmp 全缺。
  启动成功必写 "[WD] alive"——watchdog.log 缺失最可能是**用户拷日志不全**
  （Vita3K 在 Windows 侧：C:/Users/zyb/AppData/Roaming/Vita3K/Vita3K/ux0/data/J2ME00001/）。
  以后每次复测汇报必须附"完整收集清单"。

### v01.94 改动（取证增强，不是修复）

放弃新建 Sites_vita.cpp 路线（其依赖的 incls/_Sites_vita.cpp.incl 不存在；
includeDB 驱动的 MergedSrc 机制对新 .cpp 不透明），改在既有文件内联 mark，
diff 最小（2 文件 +21 行，全在 #if defined(VITA) 内）：

| 插桩点 | 文件:行 | ring 区分 |
|---|---|---|
| pump_events_vita() 门开处 | Scheduler.cpp:666 | "泵在跑且冻在泵内(查 JVMSPI/MIDP pump)" vs "泵彻底饿死" |
| Java_java_lang_Thread_start0 头 | Natives.cpp:1277 | "循环仍回 start0 但泵门不开" vs "绕开 start0" |
| Scheduler::start 头 | Scheduler.cpp:1249 | spawn 登记点；配合 Thread::finish(:216 已有) 可分辨 spawn/die 循环 |

- 声明可见性已验证：Globals_vita.hpp 经 .incl 链对 MergedSrc002/003 可见
  （grep "Globals_vita.hpp" __MergedSrc00{2,3}.cpp.incl 均命中）。
- nm 落位：U vita_site_mark 出现在 target/release/_MergedSrc00{2,3,4,6}.o
  （02=Scheduler 泵+start、03=Natives start0、04=Thread finish(旧有)、
  06=OS_vita 定义）；库级未定义引用闭合；jvm_fast_globals D=1；
  C 解释器泄漏=0。

### 复测判读表（下一次 HANG 的 site_ring.log）

1. ring 尾 = pump site → 泵最后一次真的跑了，冻在 check_blocked_threads/
   JVMSPI_CheckEvents 内 → 下一步查 MIDP pump 与 ANI 路径
2. ring 尾 = start0 头 → 循环仍回 start0 但 pump 门不开（50ms 窗口/两旗标都
   不满足）→ 下一步放宽门条件或在门内加 trace
3. ring 尾 = Scheduler::start + finish 交替 → 仍在 spawn/die，但绕过 start0
   （查其它 spawn 入口，如 Isolate/Task 层）
4. ring 尾 = clinit 满 / 其它 → 新循环，ring 序列直接点名

### 构建

- cldc HEAD=1d4f2b8（Scheduler.cpp/Natives.cpp，+21）；vita-port HEAD=6b56847
  （CMakeLists.txt 版本行 01.94 + 案情注释）。
- VPK: out/vpk/midp_vita_v01.94.vpk，3648309 字节，
  md5 e869d14222da32c6d0df79ab597d6aff；版本串 "J2ME Player v01.94 b296 (53a558d)"；
  vita_site_mark@0x81053f38。
## v01.95（2026-09-21）：log21 定案后的活快照取证 + 取证工具固化

### 改动（三项，均为取证增强）

| 改动 | 文件 | 目的 |
|---|---|---|
| vita_wd_snapshot() | OS_vita.cpp（新 extern C 函数）+ Scheduler.cpp 尾部 4 个 vita_sched_* 包装 | watchdog HANG 时**先读活进程**：Thread::current() 的 status/pending/jsp、scheduler 的 active/async 计数、timer_ticked、ev_readiness。log21 证明 coredump 读不到 .bss 头（jvm_fast_globals），尸体不可读就读病人 |
| switch_thread mark | Scheduler.cpp:2063 头部 | 冻结 ring 里若**有** switch mark = 纯桩 ping-pong（切换在走但进不了任何 C++）；**没有** = 派发冻结在第一次 switch 之前 |
| Thread::allocate mark | Thread.cpp:293 头部 | 冻结期间 spawn 计数（log21 clinit ring 终于 java/util/Timer <clinit>，疑似 spawn 风暴） |

- Scheduler.hpp 加 VITA 块只读访问器（get_timer_has_ticked /
  get_estimated_event_readiness / get_async_count，类内 inline 读 private
  static 合法）；OS_vita.cpp 经 extern "C" 桥接调用（其 incl 链无
  Scheduler.hpp，不能直接写 Scheduler::）。
- 三处 mark 的汇编落位已验证：switch_thread 头部 bl vita_site_mark 在
  前 40 条内；_MergedSrc004.o 内 _ZN9JVMThread8allocateEv 含 mark；
  库守卫 jvm_fast_globals D=1、C 解释器泄漏=0。

### 构建与产物

- cldc HEAD=b67e8ad（4 文件 +128）；vita-port HEAD=81ff590（watchdog 调用
  + 版本注释 + .sym 归档目标）。
- VPK: out/vpk/midp_vita_v01.95.vpk，3649012 字节，
  md5 5517ed4b0fe33208d3998ad17d87d876；版本串 "J2ME Player v01.95 b301
  (81ff590)"；代码段较 v01.94 增 720 字节（新代码已链接）。
- **新流程**：每次构建自动归档 out/vpk/midp_vita_v<版本>.sym（未 strip
  链接产物）——psp2dmp 解析必须用与 dump 同版本的符号，build/ 下的会被
  覆盖。

### 任务 2：psp2dmp 解析技能化（已交付）

- `debug/tools/parse_psp2dmp.py`：gzip 自动解压、ELF32 PT_LOAD/PT_NOTE、
  THREAD_REG_INFO(0x1004) 寄存器窗口自适应（+0x10 实测校准）、slide
  自动求解（site_ring.log self= 行作 anchor 按符号名定位，就近取页 +
  残差<0x800 容版本漂移）、nm 符号化 PC/LR、栈走（runtime 区间过滤）。
  log21/log19 双样本验证通过（slide 0x44000/0x15000 均自动解出）。
- 技能文件：`.instructions/psp2dmp-forensics.instructions.md`（判读要点、
  日志链路、16 文件收集清单、版本对应）。
- PSVita core note 布局实测：name 段无对齐填充；descsz=0x760。

### 任务 3：方法论复盘——为什么 20 轮取证没根治

**根因：观察点与故障点系统性错位。**

1. v01.84→v01.94 十版的 mark 全部埋在 C++ 层（pump 门、start0、Scheduler
   方法），而 log21 定案循环在**纯 ASM 桩簇**（start_lightweight_thread_asm
   ↔ invoke_pending_entries，PC/LR 都在桩入口、C 栈只剩 JVM::start 一帧）。
   ring 16 槽全残影 = 冻结期不经过任何 C++ 站点。**在错的层撒再多 mark
   也只能证明"没经过"，不能回答"在桩里干什么"。**
2. 每轮只加观察不加修复，且观察结果常是"排除"而非"定位"（r5/r8 排除、
   slide 自洽、捕获空洞定性）——排除法在 4+ 个自由度上收敛极慢。
3. coredump 捕获空洞（脏页-only）直到 log21 才定性，此前多轮对着不可读
   的 .bss/堆做推断，等于少了最重要证据源还不自知。

**优化（已开始执行）：**

1. **假设驱动替代撒网**：log21 的 PC/LR/fp=0/r1=0 已足够提出具体假设——
   "primordial_to_current_thread 每圈重载 LR=桩入口 → bleq invoke_pending
   → pending=NULL 早退 → 折返"的单圈路径。下一版直接在桩内加计数器
   验证单圈路径，而非再撒 C++ mark。桩是 C++ 生成器产的 ASM
   （InterpreterRuntime_arm.cpp 的 setup_stack_asm / SharedStubs_arm.cpp
   的 generate_invoke_pending_entries / InterpreterStubs_arm.cpp 的
   generate_primordial_to_current_thread），生成器可嵌 mark 调用。
2. **活快照**（本版 vita_wd_snapshot）：死物不可读就先读活的，直接判死
   "反复全新派发"假设（若 wd_snapshot 里 cur_thread 稳定同一指针 +
   status 不变 = 不是全新派发，是同一圈循环）。
3. **工具固化**（本版 parse_psp2dmp.py + 技能文件）：把每轮 ~30 分钟的
   手工解析降到一条命令，取证成本不再挤占分析时间。
4. **符号归档**（本版 .sym）：杜绝"构建覆盖符号源导致旧 dump 无法解"。

### 下一步（log22 预期判读）

1. wd_snapshot（新文件，watchdog 写）：cur_thread 指针稳定？status 值？
   pending 条目数？spawn 计数还在涨？
2. site_ring：尾槽有 switch mark → 纯桩 ping-pong 定案 → 下一步桩内
   计数器；无 switch mark → 冻结在第一次 switch 前 → 查 dispatch 初始化。
3. Thread::allocate mark 计数暴涨 → Timer/spawn 风暴实锤 → 查 log21
   clinit 链上 java/util/Timer 的触发方。

---

## v01.96（2026-09-21）：WAV PCM 播放（MMAPI 最大功能缺口）——JSR-135 分发链打通

### 根因分析（Java 侧源码引证，本次调研核心成果）

**WAV 数据通路断裂点 = stub `javacall_media_download_handled_by_device` 不写 OUT 参数。**

- `libpcsl_multimedia.c:224` 的 stub 返回 `JAVACALL_NOT_IMPLEMENTED` 且**不写**
  `*isHandled`；`KNIHighLevelPlayer.c` 的 `nIsHandledByDevice` 预置
  `returnValue = KNI_TRUE` → Java 恒得 `handledByDevice=true`。
- 后果：`HighLevelPlayer.realize` 永不创建 `MediaDownload` → `nBuffering`
  永不触发 → **任何字节都到不了 native**（tone/JTS 不受影响：DirectTone 的
  `setSequence` 静态直调 `MediaDownload.nBuffering`，绕过该判断）。
- 修复：vita_wav.c 定义 `vita_port_media_download_handled_by_device()` 写
  `*isHandled = JAVACALL_FALSE` 返回 `JAVACALL_OK`，同文件提供
  `javacall_media_download_handled_by_device` 覆盖包装。链接顺序（vita-port
  对象先于 stub 库 + `--allow-multiple-definition`，v01.12-14 机制）保证
  第一定义胜出。**已用 objdump 验证**：`javacall_media_download_handled_by_device
  @0x8100ce50` 为尾调用 `b.w vita_port_media_download_handled_by_device`。

### 分发链其余要点（Java 侧行为，全部源码核实）

- `nGetMediaFormat` 返回 UNKNOWN 时 Java 走 `ext2Format`/`mime2Format` 回退，
  mFormats 表**只有 gif→GIF，无 wav** → 必须 native 侧回答 MS_PCM：
  caps 表加 `{JAVACALL_MEDIA_FORMAT_MS_PCM, "audio/x-wav", memory|file, 0}`
  （终结符保留）+ `realize` 记 MIME + `get_format` 对 wav.valid 返 MS_PCM。
- `KNIMediaDownload.c doBuffering` 滑窗协议：`KNI_GetRawArrayRegion` 把
  Java buffer 当前窗口拷到 **native buffer 开头（非累积）** → native 必须
  自行 memcpy 追加；`next_chunk_size` 返回作下一包大小；Java 结束标记是
  `nBuffering(null, 0, 0)`（buffer NULL）。
- `MediaDownload.java`：先 `nSetWholeContentSize`（contLength>0 时）→
  `nGetJavaBufferSize`/`nGetFirstPacketSize` → 循环 nBuffering。
- `getPlayerFromType`：nIsToneControlSupported→DirectTone（tone 路径不变）、
  nIsMIDI→DirectMIDI、否则 DirectPlayer。
- END_OF_MEDIA 通知数据单位为**秒**（Java 乘 1000），沿用 JTS 约定。

### 改动清单（全部在 vita-port，phoneme-cldc/midp 零改动）

| 文件 | 改动 |
|---|---|
| `src/vita_wav.c`（新建，~180 行） | RIFF 解析器（PCM 8/16bit、1-2ch、4-96kHz、fmt 必须先于 data、奇数 chunk 补齐）+ `vita_wav_parse_header`/`vita_wav_duration_ms` + stub 覆盖 |
| `src/vita_audio_javacall.c`（8 处） | wav_state 字段；caps 加 MS_PCM 行；`g_wav_req/g_wav_seq/g_wav_done` 握手；tone_player_thread 加 WAV 分支；`wav_sample_at`（8bit unsigned→<<8、16bit LE、volume 缩放）+ `wav_render_player`（48kHz 立体声流式渲染、nearest 重采样、逐 chunk 查 stop/seq、自然结束发 END_OF_MEDIA）；create 去掉 tone-only 拦截；get_format/realize MIME 驱动；start 握手投递；do_buffering 滑窗累积 + 结束标记时解析 RIFF；close 清 g_wav_req；clear_buffer 重置；get_java_buffer_size WAV 首包 4096；uri_is_device_tone 改 #if 0（防 static 未用警告） |
| `CMakeLists.txt` | add_executable 加 `src/vita_wav.c`；版本 01.96 + 版本注释块 |

### 构建验证（已通过）

- `./build.sh` 全绿无新警告；产物 `build/cmake/midp_vita.vpk` +
  `out/vpk/midp_vita_v01.96.vpk/.sym`。
- 符号验证：`arm-vita-eabi-nm` 见 `javacall_media_download_handled_by_device`(T)、
  `vita_port_media_download_handled_by_device`(T)、`vita_wav_parse_header`(T)、
  `vita_wav_duration_ms`(T)；反汇编确认覆盖生效。
- git 自查：vita-port 仅 2 M + 1 新增；phoneme-cldc/phoneme-midp 内部
  `status --porcelain` 均为空。

### 运行时验证方法（待上机）

- Vita3K 加载含 `Manager.createPlayer(url, "audio/x-wav")` 的 MIDlet 或游戏
  BGM 路径；看 `audio_debug.log`：应出现 `wav buffered: <rate> Hz <ch> ch
  <bits> bit, <ms> ms` 与 `wav done`；失败路径为 `wav parse FAILED`。
- 回归点：ToneTest 的 device://tone 路径必须照常（DirectTone 静态调
  nBuffering，不经过 handled_by_device）。

### 遗留风险

1. ~~WAV >32KB 被截断~~ → **v01.97 已解决**（缓冲动态化，见下节）。
2. `create` 不再按 URI 拦截，坏内容延迟到 realize/nGetMediaFormat 才抛
   MediaException（Java 侧行为一致，但失败点后移）。
3. need_more_data 循环依赖 Java `pKniInfo->needMoreData`（nNeedMoreDataImmediatelly），
   协议已核实但未经上机验证。

---

## v01.97（2026-09-21）：WAV 缓冲动态化——消除 32KB 截断（v01.96 遗留风险 1）

### 协议核实（决定设计的三个 Java/KNI 事实）

1. `MediaDownload.java:194` `roffset += num_read`：Java **恒按 native 报告的
   length 推进读偏移**——v01.96 的 clamp（take=room）等于丢字节，流永久错位。
   所以扩容是唯一安全解，不能"截断容忍"。
2. `KNIMediaDownload.c doBuffering`：nBuffering 的返回值就是 Java 下一轮
   packetSize，**need_more_data=TRUE 时返回 0 会让 KNI 把下一次 nBuffering
   当结束标记**（length<=0 分支）→ next_chunk_size 恒 4096。
3. EOF 由 Java `stream.read` 返回 -1 触发（eom → NULL 结束标记）；
   `whole_size` 只是提示（不是所有源都知道长度）→ do_buffering 的 WAV 分支
   need_more_data 恒 TRUE，终止交给 Java 的结束标记。

### 改动（3 文件，均在 vita-port）

| 文件 | 改动 |
|---|---|
| `src/vita_audio_javacall.c` | `jts_data` 改堆指针 + `wav_cap`（结构体 32.5KB→~200B）；create 分配初始块（JTS_MAX_SIZE）；`wav_ensure_cap()` 倍增扩容（WAV_MAX_BUFFER=32MB 上限 ≈3 分钟 44.1k stereo 16bit）；do_buffering WAV 分支改扩容+整包语义；close 退避等待（50×4ms）渲染线程响应 stop_flag 后再 free（use-after-free 防护）；解析失败缩回初始块 |
| `src/vita_pcsl.c` | 补 `javacall_realloc`（此前只有 malloc/free，链接期发现缺失） |
| `CMakeLists.txt` | 版本 01.97 + 注释块 |

JTS 路径不受影响：序列必 fit 初始块，仅多一层指针间接。

### 构建与验证（已通过）

- 第一轮链接失败 `undefined reference to javacall_realloc` → 补
  vita_pcsl.c 后全绿。产物 `out/vpk/midp_vita_v01.97.vpk`（3650329 字节）。
- 符号/反汇编核验：`javacall_realloc` T 0x8100aaa4；
  `javacall_media_do_buffering` 内 `bl javacall_realloc`（扩容调用在位，
  wav_ensure_cap 被 -O 内联）；`javacall_media_close` 内 sceKernelDelayThread
  退避循环在位。

### 运行时验证方法（待上机）

- 用 >32KB 的 22kHz/16bit mono WAV（44KB 起，v01.96 会截断的文件）测
  createPlayer("audio/x-wav")：audio_debug.log 应出现完整
  `wav buffered: ... bytes`（data_len 应等于文件实际 PCM 长度，非 32KB）
  且播放到自然结束发 END_OF_MEDIA。
- 32MB 上限路径：构造超大 WAV 看日志 `wav buf grow FAILED` + MediaException
  （预期失败行为，非崩溃）。

### 遗留风险

1. 倍增扩容峰值内存 = 2×文件大小（realloc 拷贝瞬间），32MB 上限文件峰值
   ~64MB——Vita 用户态可用堆 ~192MB，单文件安全，多 player 并发大文件
   未测。
2. close 退避上限 200ms，若渲染线程因 audio_out 阻塞超过 200ms 仍可能
   free 竞争（实际 OUT_CHUNK_FRAMES=512@48kHz ≈10.7ms/块，余量 20 倍）。

## log22 判读（2026-09-22，v01.97 b303 运行件）：ping-pong 定案 + 事件泵饿死机制

### 数据完整性

16 文件全（含 log20 曾缺失的 watchdog.log）。v01.95 三装置首次同时开火。
parse_psp2dmp.py slide=0x3a000 自动求解（anchor 法第三次实战），v01.97.sym
符号化零漂移。

### 三方证据

| 装置 | 数据 | 判读 |
|---|---|---|
| site_ring | 15×switch_thread+0x28、1×pump_events_vita+0x3c（残影）、seq=513589 frozen=yes | v01.94 判读表第 1 分支命中：纯桩 ping-pong |
| wd_snapshot | cur_thread=0x81401d10 status=ACTIVE(0) pending=0 active_count=5 async=1 timer_ticked=1 ev_readness=0 | 判死 log21"全新派发态"（cur 稳定非初生非死）；新矛盾：5 活线程无人做功 |
| coredump | PC=start_lightweight_thread_asm+0x8（ldr r1,[r0,#0x68] 读 status 槽）r1=0、LR=同函数+0x0、栈走唯一 JVM::start+0x178 | 与 wd_snapshot status=0 互证 |

### 指令级机制链（把 log21"卡在桩簇"细化到指令）

- 桩头（810bbb44）：`+0x8 ldr r1,[r0,#0x68]`（status 槽，非 pending）→
  `tst r1,#0x40000000`(TERMINATING) → 无该位 → `bleq invoke_pending_entries`
- ASM 版 invoke_pending_entries（810bb86c）：头三句
  `ldr r1,[r0,#0x24](pending); cmp r1,#0; moveq pc,lr`——pending=0 直接
  早退，与 wd_snapshot pending=0 严丝合缝
- Scheduler::yield（Scheduler.cpp:1092）：**next_thread 非空 → 跳过整个
  wait_for_event_or_timer 循环（事件泵挂在此路径）→ 直接回 switch_thread**

### 机制结论

active_count=5 → get_next_runnable_thread 永远选得出 → yield 永远走
"快车道" → 事件泵永久饿死（ring 仅 1 次 pump 残影）→ 按键不再分发 →
表象"按键几次就挂死"。触发同 log21：同游戏、末次按键 0x10 释放后、
clinit 同终态 java/util/Timer(count=35)。

### 下一步（按性价比排序）

1. vita_wd_dump_threads（扩展 OS_vita.cpp 快照，遍历 active 链 +
   scheduler_waiting 链，打印每线程 id/status/is_just_born）——直接回答
   "5 个线程是谁、谁被反复选中又反复让出"
2. yield 快车道空转计数器进 wd_snapshot——量化"泵饿死"
3. active_count 泄漏排查：Universe::invoke_pending_entries（工作副本
   Universe.cpp:1201）里 active_count<=0 时每圈 initialize_main+start，
   若主线程被重复登记则计数虚高（5 可能 = 真实 3~4 + 泄漏）
4. 桩计数器（生成器嵌 mark）：区分早退/走到桩尾比例

## v01.98（2026-09-22）：log22 判读落地 + 泵饿死量化装置

### 判读复盘（详见上节 log22 判读）

三方证据定案纯桩 ping-pong：ring 15×switch_thread+0x28、wd_snapshot
active_count=5 pending=0 status=ACTIVE、PC=start_lightweight_thread_asm+0x8。
核心矛盾升级为：**5 个活线程为何无人做有用功**。指令级链路（本轮补全）：
ASM 版 invoke_pending_entries(810bb86c) 头三句 `ldr r1,[r0,#0x24]; cmp;moveq pc,lr`
与 pending=0 严丝合缝；yield() 快车道（next_thread 非空）完全跳过事件泵循环。

### v01.98 改动（取证增强，全部 #if defined(VITA)）

| 文件 | 改动 |
|---|---|
| `phoneme-cldc/src/vm/share/runtime/Scheduler.cpp` | 静态区 4 计数器（_vita_yield_fast/slow、_vita_pump_runs、_vita_switch_count）；yield 快/慢车道埋点；pump_events_vita 泵执行计数；switch_thread 计数；文件尾 vita_sched_dump_threads()（全局线程链 + 优先级队列清单，id/pri/status）+ 4 个 extern C 计数器桥 |
| `phoneme-cldc/src/vm/share/runtime/Scheduler.hpp` | 4 计数器静态成员声明 + 类内读取器 |
| `phoneme-cldc/src/vm/os/vita/OS_vita.cpp` | wd_snapshot 输出 lanes 行 + 调用线程清单转储（static 4KB 缓冲） |
| `vita-port/CMakeLists.txt` | 版本 01.98 |

### 构建踩坑记录（本轮新增教训）

1. **修改 MergedSrc 分组内源文件后，必须 rm target/release/_MergedSrc00N.o 再重编**——make 依赖挂在 wrapper .cpp（Sep 3 生成）上，不追踪 incl 内的源文件；本轮 Scheduler.cpp 改动就落在 _MergedSrc002 组，首次重编静默沿用旧对象（rebuild_vm.sh 的 `|| true` 掩盖了编译错误输出）。另注意 switch_thread 是 extern "C"（类外），引用计数器必须写 `Scheduler::_vita_switch_count`。
2. **build_midp_vita.sh 走旧 midp-vita 工程（只编 main.c，无 vm_rom_stubs.o）**——现行工程是 vita-port（含 vm_rom_stubs.c 定义 _rom_linkcheck_mffd_false）；走错工程会报 undefined reference to `_rom_linkcheck_mffd_false`。正确构建命令：`cd vita-port && cmake --build build -j8`。
3. step1_cldc 的 Universe.cpp md5 marker 过时会触发遗留的 debug/AZZERT 重编路径（违反 PRODUCT ABI 规则）；CLDC 库已用 rebuild_vm.sh 重编时，刷新 `.cldc_universe.md5`/`.cldc_jvmspi.md5`/`.cldc_built` 三个 marker 跳过。

### 产物与提交

- VPK：`vita-port/out/vpk/midp_vita_v01.98.vpk`（3650773 字节，b305）
- .sym：`vita-port/out/vpk/midp_vita_v01.98.sym`（4284200 字节）——下轮 coredump 符号化用
- 提交：387c451（CLDC 装置）+ 9b70c2d（版本）+ 35d1827（补 v01.96/97 WAV 遗留源码，使树与 b303 产物一致）

### 下一轮 HANG 判读表（v01.98 增量）

看 site_ring.log 尾部 wd_snapshot 段：

| 证据组合 | 结论 |
|---|---|
| fast 增长 + pump 停滞 | 快车道饿死泵（log22 假说成立）→ 修 yield 快车道补泵 |
| fast 增长 + pump 增长 | 泵在跑但事件送不到 → 查 check_blocked_threads/JVMSPI_CheckEvents |
| slow 增长 | 假说推翻：线程都不可运行 → 看线程清单谁是等待者 |
| threads(priority-queues) 有 DEAD/TERM 线程 | 死线程未摘队列 → 修 remove_from_active/adjust_priority_list |

### 遗留

- 0001-save-change.patch（历史遗留未跟踪文件）仍在。

---

## 2026-09-23 log27 分析（v02.02 取证版 / build 9081d2e 系）

> **⚠️ 勘误（2026-10-14，log29 判决）**：本章使用的 coredump 符号化 slide 常数**错了 0x37040**（用了旧 build 的 .sym 换算）。用四锚点重新定标后，"PC=png_do_rgb_to_gray"、"ring 落 libpng"、"寄存器残留 skin/pba r.im" 等全部判读为**符号化假象**；"执行流损坏"的判决**作废**。正确判读见下方 log29 章节（真实现场 = log23 同型"纯 Java 计算饿死事件泵"指纹：clinit com/app/filemanager/UI）。本章 THREAD_INFO/REG_INFO 布局等**结构层数据仍有效**。

### 证据清单

- `watchdog.log`：`HANG ce=401 polls=12`（tick=1713 停滞，双时钟健康 d=100/kd=100326）——与前几轮完全同指纹
- `site_ring.log`（v02.02 新线程表，**本次最有价值的证据**）：
  - 全进程只有 **3 个注册线程**：j2me_tone（WAITING delay 1ms）、j2me_watchdog（RUNNING）、cldc_ticker（WAITING delay 10ms），全部 prio=0xa0
  - **没有 ANI 池线程**——pthread_create 路径在挂死场景从未创建过线程 → **pte 优先级倒置/饿死理论正式证伪**
- `thread_audit.log`：`wrap active, first tid=... 'j2me_tone'`（wrap 机制工作正常）
- coredump（THREAD_INFO + THREAD_REG_INFO 完整解码，工具改进版已固化到 `debug/tools/`）：
  - 全进程 **5 个内核线程**：J2ME00001(VM主)、SceCommonDialogWorker、j2me_tone、j2me_watchdog、cldc_ticker
  - **VM 主线程 status=RUNNING、wait=none**，PC=0x810d0120 = `png_do_rgb_to_gray+0x12c0`（libpng NEON 变换循环），lr 同函数内（+0x1594），cpsr=0x40070010（用户态、无异常位）
  - 寄存器残留字符串 `skin/pba r.im`（chameleon 皮肤进度条图片名）
  - j2me_tone/cldc_ticker 的 `+0xa0` 字段落在 `_rom_task_mirrors`（ROM 数据区）→ **WAITING 线程的该字段不是有效 PC**（仅 RUNNING 态可信）

### 判决

1. **饿死理论（log26 头号嫌疑）证伪**：VM 是 RUNNING 不是 READY；不存在更高优先级竞争者；ANI 池线程根本没创建。
2. **VM 冻结点在 libpng 的 png_do_rgb_to_gray NEON 循环内**，且经调用图全量排查：
   - MIDP 的 `decode_png_image`（imgdcd）是纯 C 自带解码器，**从不调用 libpng**
   - 全二进制中 `png_read_image` 的唯一调用者是 `vita_icon_decode_png`（vita-port 菜单图标），且菜单阶段早已结束
   - **没有任何代码启用过 `png_set_rgb_to_gray`**（rgb_to_gray 变换不可能被合法触发）
   - → VM 主线程出现在 libpng 内部 = **执行流损坏（野跳转/垃圾 PC）**，非合法调用链
3. **watchdog 被 v02.02 的 `sceKernelGetThreadRunStatus` 冻死在内核里**（PC=e000df82，40s 不返回；fw3.65 该 API 从用户态调用挂起）——导致 site_ring/vm 标记/abort 全没执行，coredump 由系统机制兜底产生。**教训：未在真机验证过的内核 API 一律先包一层或不用**（GetThreadInfo 自 v01.75 已验证安全）。

### 新头号嫌疑（log27 后重排）

1. **JIT/I-cache**：VM=UseCompiler=1（crumb: `jit cfg=0 -> policy=0` 但 boot_log 显示 UseCompiler=1），真机 Cortex-A9 多核 I-cache 一致性 vs Vita3K 不模拟。已查 `CompiledMethod_arm.cpp`/`SharedStubs_arm.cpp` 调用 `arm_flush_icache`→`OsMisc_flush_icache`→`__builtin___clear_cache` 存在，但**romgen 生成的 ROM 代码块与 JIT 代码缓冲区的维护路径未逐点核实**（需要下轮对 `_rom_text_block*` 写入点与 flush 时序做全审计）
2. 函数指针表/ROM 镜像数据被写坏（PC 落在数据区或库函数中段 = 跳转表损坏的另一形态）
3. 栈溢出踩坏返回地址（VM 主线程栈顶 sp=0x8137ffb0 距栈底仅 0x30 字节余量的历史问题仍在——本次 sp 距 0x81380000 仅 0x50）

### 本轮代码改动（v02.03，commit 4e7992d）

- `vita_thread_audit.c`：删除 `sceKernelGetThreadRunStatus` 调用（真机挂起的元凶）；新增 `--wrap=png_read_image` 计数器 `vita_png_reads`（dump 尾部打印 `[AUDIT] png_read_image calls=N`）
- `CMakeLists.txt`：加 `-Wl,--wrap=png_read_image`；版本 02.03
- 产物：`out/vpk/midp_vita_v02.03.vpk`（3652821B，b318）
- 下轮判读：若再次 HANG 且 coredump 仍见 VM 在 libpng 内 → 看 `png_read_image calls`：菜单图标扫描应≈游戏数（本档 5）→ 远超即异常；为 0 则证明 PC 完全不可信（垃圾上下文），主攻 I-cache/跳转表审计

### log27 系列日志的判读基准

- THREAD_REG_INFO 布局（376B/entry）：`+0x0c..+0x3c`=r0..r12，`+0x40`=sp，`+0x44`=lr，`+0x48`=pc，`+0x4c`=cpsr
- THREAD_INFO 布局（desc=[u32 0x12][u32 count]，200B/entry）：`+0x00`=esz(0xc8)，`+0x04`=tid，`+0x08..+0x27`=name[32]，`+0x2c/+0x30`=init/currentPriority（工具 `debug/tools/threads27.py` 已按此实现并验证）
- REG_INFO 行数多于 THREAD_INFO（含 tid=0 的内核内部行），且用户线程行的 tid 字段不可靠（VM 行 regtid=5）——按精确 tid 匹配名字，匹配不上就标 `?`；VM 主线程靠"pc/lr 落用户 .text 且 sp 在其栈内"识别

---

## 2026-09-23 log28 分析（v02.03 取证版 / build 9081d2e）

> **⚠️ 勘误（2026-10-14，log29 判决）**：同 log27——slide 错位 0x37040 使 "pc=lr=0x81108374 落 .rodata"、"ring 返回地址无 BL 来源"、"wd_snapshot 三值解码为 ARM 指令"全部是**错位符号化产物**；"执行流损坏维持判决并升级"与"stdio 并发写"假说**作废**（wd_snapshot 三垃圾值的真因是 v02.01 引入的 handle 双重解引用 bug，见 log29）。正确判读见下方 log29 章节。

### 证据清单

- `watchdog.log`：HANG ce=398 polls=12 tick=1713（同指纹）；**v02.03 取证链完整生效**——线程行（VM=RUNNING wait=none）、site ring、vm 标记、主动 data abort 全部到位（log27 教训的 API 已摘除）
- `site_ring.log`（首次完整 ring）：
  - `site seq=189824 frozen=yes`——site mark 已调用 18.9 万次后停摆
  - ring 16 条中 15 条 = `0x810cf614`（`png_do_rgb_to_gray+0x834`），1 条 = `0x810cd5a4`（`png_image_read_colormap+0x7b4`）
  - **反汇编验证：两个地址的前一条指令都不是 BL**（`mov r3,r1` / `adds r6,#3`）——不可能是 `vita_site_mark` 的合法返回地址 → **ring 本身被写坏**（或 mark 被野调用）
  - `wd_snapshot` 读到 `status=0xe5957710 / pending=0xe586b014 / jsp=0xe5933034`——**三个值全部解码为合法 ARM 指令**（LDR R7,[R5,#1808] / STR R11,[R6,#20] / LDR R3,[R3,#52]）——机器码涂进了 Thread 对象字段
  - 同一 dump 内 active-list 先读 `st=00000000`（正常）后读指令字 → dump 期间 VM 线程仍在持续乱写
- coredump（PC 换代，log27→log28）：
  - VM 主线程 pc=lr=0x81108374 = `_rom_method_variable_parts_src+0x130`——**.rodata 数据区**（该处是指针表，非代码）
  - 寄存器 r5/r6/r7/r10/r11/r12 与 log27 **完全相同**——同一条确定性路径复现
  - **走栈决定性证据：[sp]=`_svfprintf_r+0x894`**——最内层真实帧是 newlib vfprintf 内部；8167e8xx 区还有成对 `0x81141bdb/0x811526cc`（ROM 尾部数据符号）残留
- 堆段扫描：无成片指令字涂抹（最长 run=0）、text 指针密度 0.25%（正常 vtable 水平）→ 污染是**点状精准**，不是大面积刷写

### 判决

1. **JIT 假说出局**：`runMidlet.c` 确凿注入 `-int`（policy=0 → `JVM_ParseOneArg({"-int"})` → `UseCompiler=false`，Arguments.cpp:128）。crumb `vm UseCompiler=1` 是**打印点缺陷**——在 runMidlet 调用前读的是默认值（v02.04 已修：pre/post 两条）。phoneME 的 `-int` 自 v01.46 起就是默认。
2. **执行流损坏维持判决并升级**：VM 线程 pc=lr 落在 .rodata、ring 返回地址无 BL 来源、Thread 对象字段是指令字——三处独立证据同指"执行/数据边界被击穿"。栈残留 `_svfprintf_r` 内部帧 + OS_vita.cpp log10 时代注释（"residue frames point at the stdio domain"）+ v01.92 注释（log18 曾冻死在 stderr 写路径）——**newlib stdio（vfprintf/sceIoWrite 未缓冲 stderr）是反复出现的案发现场邻近物**。
3. **新头号嫌疑：phoneME 多线程对同一 FILE*（stdout/stderr 经 freopen 绑定 ux0: 文件）的非同步并发写**。VM 线程（tty->print → jvm_printf → stdout）与启动器/其他线程并发 fprintf 同一 FILE，newlib 的 _svfprintf_r 内部锁在 Vita 上若行为异常（或 FILE 结构被部分写坏），轻则互相踩格式化状态、重则 longjmp/野返回。堆扫描排除大面积涂抹后，"stdio 内部状态机被并发破坏"与点状精准污染的形态吻合。
   - 反证条件：log28 里 BEFORE_NATIVES×30 都是顺序打完的（未冻结在写中），冻结点在挂死时段的某次输出。真机独有 + Vita3K 不挂：Vita3K 的 sceIoWrite 时序/调度不同，竞争窗口不同。

### v02.04 改动（commit 37c7408）

- `vita_thread_audit.c`：dump 尾部补打 `[AUDIT] png_read_image calls=%u`（v02.03 的 wrap 计数器从未被打印，判据落空；.bss 不进 psp2core，事后也无法恢复）
- `vita_main.c`：UseCompiler crumb 拆 pre/post 两条（pre=默认值，post=实际轮次模式）
- 产物：`out/vpk/midp_vita_v02.04.vpk`（3652473B，b322）

### 下一步（v02.05 候选）

1. **stdio 并发假设的正面验证**：给 `jvm_printf`/`fprintf(stderr,..)` 高频路径套 `--wrap` 计数器（_svfprintf_r 无法直接 wrap，但可 wrap `fwrite/fputc/fflush` 统计挂死时段的 stdio 流量与线程归属）；或干脆将 VM 的 tty 输出改为 sceIoWrite 直写（绕开 stdio）做对照轮
2. ring 中毒溯源：`vita_site_mark` 是 noinline 直接被 bl——在 mark 入口记录 lr 到独立数组（两份 ring 交叉验证是否 ring 数组本身被涂还是 mark 被野调用）
3. 若确认 stdio 并发：最小修复 = VM 侧所有输出加一把专用互斥锁（JVMSPI_PrintRaw 与 ClassFileParser 的 fprintf 改走同一加锁通道）

---

## 2026-10-14 log29 分析（v02.04 / build 37c7408 系）：符号化勘误 + 判决大反转 + v02.05 取证装置

### 换算公式（最高优先级记忆，取代一切旧 slide）

- 唯一可 nm 的 ELF：`vita-port/build/midp_vita.velf`
- **VMA = symtab − 0x81000000**（symtab 里的地址直接减模块基）
- **runtime = VMA + 0x37000**（psp2core dump 里的地址）
- 等价：**symtab = runtime + 0x80FC9000**
- log27/28 用的 slide 差了 **0x37040**（旧 build 残留换算）——所有"pc 落库函数/.rodata"的判读全是错位假象

### 四锚点定标法（本轮固化，今后每次符号化必做）

1. thread_audit 的三个 creator 地址（`__wrap_sceKernelCreateThread` 的 BL 返回地址）
2. watchdog abort 写 0x81000000 处的 "DRES" 标记
3. site ring 的 switch_thread/pump 地址（代码形态交叉验证）
4. 上面 3+ 个独立锚点必须给出**同一个** slide 才可信

**教训**：①符号化必须 ≥2 独立锚点；②工具自身输出也要验证——`nm` 标签可有 **+0x40 系统性偏移**（本轮 site_mark/switch_thread/__wrap_sceKernelCreateThread 三例），矛盾时以**反汇编代码形态**为准（看 BL 指令、函数序言，不信标签）。

### 判决反转

- **log27/28 的"执行流损坏"全部作废**：PC 不在 png_do_rgb_to_gray、不在 .rodata；ring 没有中毒；寄存器"残留字符串"是错位解读。
- **wd_snapshot 三垃圾值的真因**：v02.01 引入的 handle **双重解引用 bug**——`Thread::current()` 返回的是 handle cell（`&_current_thread`），`ct->status()` 这种方法调用**本来就经 `*this` 解引用到真 oop**；再显式 `*(OopDesc**)ct` 等于把 oop 首字段当指针再解一次 = 读到错位字段（垃圾值/指令字全是这个机制的产物）。**phoneME handle 语义：对 Thread* 调方法 = 自动解引用；显式二次解引用 = 永远错。**
- **真实现场（log29 定案）**：VM 主线程 pc = `interpreter_timer_tick+0x14`（timer tick stub 内、完全健康的位置），走栈 `JVM::start ← JVM_Start ← midp_run_midlet_with_args_cp`；辅助线程健康（tone/ticker 在 delay 睡眠）；**全 liveness 冻结**：fast=372 slow=0 pump=381（==CE） switch=188996，clinit=`com/app/filemanager/UI` count=35 depth=0——与 **log23 "纯 Java 计算饿死事件泵"完全同指纹**。

### 未解矛盾（v02.05 取证目标）

ticker 活着（每 10ms `real_time_tick` → `Thread::set_timer_tick` → `_rt_timer_ticks++`），VM 若还在跑字节码，back-branch 检查点应每 10ms 开一次泵——但 ce/pump/fast 全冻结，而 pc 停在 timer_tick stub 内。三个假说存活：

- (a) 解释器不再过 back-branch 检查点（pc 采样只是冻结前最后一瞬，VM 实际在别处/汇编 berth 循环）
- (b) 检查点触发但 `Thread::timer_tick` 永不完成（冻在函数中段）
- (c) 触发且完成但内部 yield 每次早退（is_stopping/pending exception 反复触发）

### v02.05 取证装置（本轮落地）

| 探针 | 位置 | 区分什么 |
|---|---|---|
| `vita_sched_rt_ticks()`（读 `_rt_timer_ticks`） | Scheduler.cpp 访问器桥 | ticker→检查点置位链是否活着（rt_ticks 攀升=置位链活着） |
| `_vita_ttk_count`（`vita_sched_ttk_count()`） | Thread.cpp，`Thread::timer_tick` 顶部 | 检查点是否真的进到了 C++ 层 |
| `vita_site_java_frame()`（采样当前 Java class.method + `vita_java_sample_count`） | 同上（JavaFrame 构造仿上游 ENABLE_PROFILER 块） | **给 Java 循环点名**；samples 攀升而 lanes 冻结 = 直接杀 (a) |
| wd_snapshot 新行 `rt_ticks/ttk/jsmp` | OS_vita.cpp | 三值一次快照 |
| `vita_site_dump_java` | watchdog HANG 链（site_ring.log） | dump 文本（VM 线程写、wd 只读，无锁同 clinit 探针纪律） |

判读表：

| 证据组合 | 结论 |
|---|---|
| rt_ticks 攀升 + ttk 冻结 | (a) 成立：检查点死了 → 主攻解释器为何不过 back-branch（ berth stub / 汇编循环） |
| ttk 攀升 + samples 攀增 + lanes 冻结 | (c) 方向：yield 早退或 pump 门判据问题 |
| 三者全冻结 + rt_ticks 也冻结 | ticker→置位链断了（更深：ticker 线程被饿死？） |
| samples 攀升 + 名字稳定在某一 method | 该 method 就是饿死泵的 Java 循环本体 |

### 已修 bug（v02.05 顺带）

- `vita_wd_snapshot` 双重解引用（见上"判决反转"）——`cur` 删除，直接用 `ct` 调方法；保留 `cur_cell`/`cur_thread` 打印（`cur_thread` 现在打印 `*(OopDesc**)ct` 即 cell 内容地址，仅供与旧日志对照）。

### 本轮代码改动（v02.05）

- `phoneme-cldc/src/vm/os/vita/OS_vita.cpp`：wd_snapshot 修 bug；`vita_site_java_frame`/`vita_site_dump_java`/`vita_java_sample_count`；wd_snapshot 补 `rt_ticks/ttk/jsmp` 行；访问器 extern 声明两条
- `phoneme-cldc/src/vm/share/runtime/Thread.cpp`：`_vita_ttk_count` 定义 + timer_tick 顶部计数与采样调用（`#if defined(VITA)`）
- `phoneme-cldc/src/vm/share/runtime/Scheduler.cpp`：`vita_sched_rt_ticks`/`vita_sched_ttk_count` 访问器 + `_vita_ttk_count` extern 声明
- `phoneme-cldc/src/vm/os/vita/Globals_vita.hpp`：三条新声明
- `vita-port/src/vita_watchdog.c`：HANG 链补 `vita_site_dump_java`
- `vita-port/CMakeLists.txt`：版本 02.05 + 注释块
- 产物：`vita-port/out/vpk/midp_vita_v02.05.vpk`（3652606B，b324，版本串 `J2ME Player v02.05 b324 (9d221a2)`）；.sym 4287160B（下轮 coredump 符号化用，注意按 log29 换算公式重算 slide）

### 遗留风险

- `vita_site_java_frame` 在 timer_tick 顶部构造 JavaFrame——若冻结发生在**汇编 berth stub 循环**（无 Java 帧），`f.method()` 可能读到残帧/空帧：探针对 NULL Symbol 已防御，但**名字可能停在冻结前最后一帧**，判读时需与 samples 计数联动（samples 冻结=名字不可全信；samples 攀升=名字可信）。
- `_rt_timer_ticks` 是 int 非原子，watchdog 跨线程读可能撕裂——仅用于"是否还在攀升"的定性判读，不做精确值。

## 2026-10-14 log30 分析（v02.05 / build 9d221a2 系）：三假说全灭 + 判决转向"内核调度位丢失"

### 定标（per-build 必做）

- 本 build slide = **0x32000**（`symtab = runtime − 0x32000`；MODULE_INFO 里 midp_vita text 基址三处 0x81032000，ELF text 基址 0x81000000，绝对闭合）。
- 四锚点验证一致：site_ring self=0x81086290↔symtab 0x81054290；tone creator 0x8103eec1↔`javacall_media_initialize+0x5d`；wd creator 0x81040385↔`vita_watchdog_start+0x2d`；ticker creator 0x8108650c↔`JvmOs::start_ticks+0x6c`。
- log29 是 0x37000——**slide 是 per-build 的，永无通用常数**（log27/28/29 三轮教训的根源）。psp2core dump 是 gzip 包装（非裸 ELF，自写解析器要先 `gzip.decompress`）。

### 关键数据（v02.05 探针全量生效）

watchdog：`HANG ce=401 polls=12 tick=1660` 双时钟健康；VM `RUNNING wait=none`；3 注册线程。wd_snapshot（双重解引用修复后首次）：
`status=0x0 pending=0x0 jsp=0x81edef60 active_count=5` 全部合理；**核心三值 `rt_ticks=394 ttk=349 jsmp=349`**（ticker 挂死后仍积压 394 tick ≈3.94s；timer_tick 总进入 349 次且每入必采到样）；lanes fast=381 slow=0 **pump=401==ce** switch=189669 sf=0/0/0 gnrt=0/0；timer_ticked=1 ev_readiness=0；clinit=`com/app/filemanager/UI` count=35 depth=0 gc=0 preload=864；`java site=.unknown..preprocess samples=349`；input 最后键 `0x10`(DOWN) 后心跳断（与 log21/22/23/24/29 同触发指纹）。

### 三假说判决（本轮目标，全部证伪）

| 假说 | 判决 | 证据链 |
|---|---|---|
| (a) 检查点死（解释器不过 back-branch） | **存活但降级**：不在"纯 Java 循环"形态 | ttk=jsmp=349：第 349 次 timer_tick 进去了且采到了 Java 样；但 rt_ticks=394 说明之后再无第 350 次。若 Java 循环还在转必再进 |
| (b) timer_tick 永不完成 | **证伪** | VM 线程 pc 不在 timer_tick/任何 C++ 函数内（见下） |
| (c) yield 早退循环 | **证伪** | ring 尾部 6×`switch_thread+0x28` 无一 pump mark；switch_thread 顶部无条件 `pump_events_vita()`，早退循环必留 pump 痕迹 |

再加 ring 时序（seq=191131，mod 16=11 → 时间序 [10]→…→[11]，尾 6 槽全 switch 无 pump）：**挂死起点的最后动作是纯粹的线程切换**——第 349 次 timer_tick 完成后（rt_ticks 清 0），切到某线程，然后一切冻结。

### VM 线程冻点符号化（本轮决定性证据）

- 寄存器：pc=0x810ee62c→symtab 0x810bc62c = **`start_lightweight_thread_asm+0x18`**；lr=trampoline 入口；fp=0；r0=r3=0x81eddfb4（=wd_snapshot cur_thread ✓）；cpsr=0x40070010（Z=1）；sp=0x8137ffb0。
- threads27.py 标的 "inflate+0xee0" 是**未减 slide 的假象**（工具无 slide 处理，第三次踩中）。
- OS 栈（sp→栈顶 0x50 字节内）唯一返回地址 `JVM::start+0x178`——green-thread 模型正常形态（Java 执行全在 Java 栈）。
- **指令级判读**：+0x18 是 `bleq invoke_pending_entries`，目标 stub 头两条：`pending==NULL ? 返回 : 调 shared_entry`。wd_snapshot 显示该线程 `pending=0x0`、status=0x0——**分支会走且目标立即返回**。冻点与 +0x14 之间无任何可阻塞指令。
- 语义：这是**新 green 线程首次调度点**（`setup_stack_asm` 布置的返回地址链：lightweight_thread_exit→uncaught→finish→force_terminated→start_lightweight_thread_asm）。VM 冻在"刚切到一个新线程、trampoline 都没走完第一步"处。
- log29 冻点 `interpreter_timer_tick+0x14` 同为**桩区无害指令**（bl shared_call_vm 后恢复寄存器返回解释器 dispatch 的尾部）——两轮 pc 全部停在"无害指令、下一步本该正常继续"的位置。

### q5 "幽灵环" 勘误（log26 判定作废）

- 打印项恰 **48 = `VITA_SCHED_DUMP_MAX`**：`add_to_list` 上游注释原文 "the list **is circular**"，首元素 `set_next(thread)` 自环，打印循环无环检测只截断到 48。3 线程（id4/3/2）×16 圈 = 48 项。
- **q5 环形打印是正常形态，不是队列损坏**。log26 "幽灵环/berth stub 环"判决中"环"的部分作废。
- 取证装置缺陷顺带记录：**`_vita_gnrt_hits` 从未被递增**（v02.01 只写了 `nulls++`，hits 分支漏了），gnrt=0/0 里 hits 位无信息量，只有 nulls=0 有效。

### 综合判决（log30 定案方向）

全部 user-space 证据收敛到同一结论：**冻结瞬间 VM 线程持有 CPU 且停在一条本会正常完成的指令上，没有任何 user-space 循环/等待/异常路径能解释"停在那不动"**。结合：

1. pc 停在无害指令、下一步必走（bleq 条件成立、目标秒回）
2. OS 栈空、无锁等待（wait=none）、无 pending exception
3. 双时钟健康（d=100/kd=100063）排除机器暂停
4. **真机独有**（Vita3K 从不挂）——模拟器无真机内核调度器
5. HANG 时 VM status=RUNNING——但 Vita 真机双核 per-core runqueue，`sceKernelGetThreadInfo.status` 的 RUNNING 可能实为"READY 且绑定核上没有对手"——**该核调度位从未再给 VM 线程**

→ **新头号假说：内核调度位丢失（thread stuck in READY / per-core runqueue 异常）**——即真机某次上下文切换后，VM 线程被置回 READY 却从未再被 pick（或 affinity/优先级瞬时异常使其饿死在 runqueue 里）。这是纯内核态现象，完美解释：真机挂/Vita3K 不挂、user-space 全冻结、pc 停在任意无害指令、多轮复现同指纹。

### 与旧假说谱系的关系

- log23-24 "纯 Java 计算饿死泵"：v01.99 修复后 pump=ce 依旧冻结，且 log29/30 pc 都停在桩区——**该谱系已不解释 log29/30**（但 log23 时代的 pump 确实死过，修复真实有效）。
- log26 "幽灵环"：环=正常形态，作废。
- log27/28 "执行流损坏"：符号化假象，已作废。
- log19/9 "spawn/die 环"：ring 无 Scheduler::start/start0 mark 残留，本轮排除。
- 唯一贯穿 log9→log30 不变的核心事实：**挂死指纹完全一致**（同游戏、0x40/0x10 触发、~3-4s、全冻结、pc 停无害指令）——**这是同一个内核级 bug 的稳定复现**。

### 下轮取证计划（v02.06）

1. **watchdog 线程行补 `currentCpuAffinityMask/currentCpuId/lastExecutedCpuId/attr`**（SceKernelThreadInfo 已有字段，只需打印）——HANG 时看 VM 线程是否 READY/affinity 异常。判读表：`affinity=0 或单核 且 cpuid≠lastcpu` → 调度位丢失实锤路径。
2. HANG 时主动 `sceKernelChangeThreadCpuAffinityMask(vm_tid, 3)` 或 `sceKernelReleaseThreadRenice`/提高优先级，观察是否恢复——**直接治疗性试探**（若恢复=调度位假说实锤，且就是 workaround 本体）。
3. coredump 之外补一份 `sceKernelGetThreadInfo` 全字段 dump（HANG 链内、abort 前）。
4. 观察点：VM 主线程创建路径（midp_run / JVM_Start）的 affinity 设置是否有历史遗留（如 `sceKernelCreateThread` attr 未设 affinity 默认单核？查 main.c 创建参数）。
5. crumb 侧 `[pte] sem create` 洪流（sem_init/pthread_mutex_init，v02.05 前日志）与挂死时间相关性——次要，先不动。

## 2026-10-15 v02.06 装置实施（build 9ff6fe2 / b326）：affinity 取证 + 治疗性 kick

对应 log30 章节五条计划中 ①②③④ 的落地（⑤暂缓）。产物 `out/vpk/midp_vita_v02.06.vpk`（3653964B，md5 f1d4f554a2da59fd1dca06ecc0d729bf），版本串 `J2ME Player v02.06 b326 (9ff6fe2)`。

### 改动内容（vita-port，3 文件 +101/-4）

- `vita_watchdog.c`：
  - `wd_dump_thread()` 线程行补 `attr / aff=0x%x(init 0x%x) / cpu / lastcpu / preempt` 五字段（SceKernelThreadInfo 已有，仅打印）。
  - **治疗性 KICK**（HANG 链内、所有只读取证之后、data abort 之前）：`sceKernelChangeThreadCpuAffinityMask(vita_wd_vm_tid, 3)` 后以 12×250ms 观察 ce 计数器。
- `vita_thread_audit.c`：audit 表行补 `aff=0x%x(init 0x%x) attr=0x%x`。
- `CMakeLists.txt`：版本 02.06；链接清单补 `SceKernelThreadMgr_stub`——**`sceKernelChangeThreadCpuAffinityMask` 只在 ThreadMgr stub 里，SceLibKernel_stub 只重导出 `GetThreadCpuAffinityMask2` 变体**；跨 stub 重复符号由既有 `--allow-multiple-definition`（v01.67）兜底。

### 查证结论（动手前完成）

- SDK 头 `threadmgr.h:355/365`：`sceKernelGetThreadCpuAffinityMask(SceUID)` / `sceKernelChangeThreadCpuAffinityMask(SceUID, int mask)` 均存在；stub 符号 `nm` 确认。
- VM 主线程不是自建线程：`vita_main.c` 直接 `vita_wd_vm_tid = sceKernelGetThreadId()`——就是引导进程主线程，无 `sceKernelCreateThread` attr 历史遗留可言，affinity 只能运行时观察/修改（计划④排除，无源代码级遗留）。

### 下次 HANG 的判读表

- wd/audit 线程行 `aff==0 或单核 且 cpu!=lastcpu` → **调度位丢失实锤路径**（READY 但 per-core runqueue 没摘到）。
- `[WD] KICK RECOVERED (ce X -> Y)` → **假说实锤 + kick 即 shipping workaround**（恢复后 watchdog 重新武装，最多 3 次；第 4 次挂起改走 data abort 拿 coredump，捕捉复发现场的内核侧状态）。
- `KICK no effect` → 假说存疑，按原流程 coredump；affinity 字段仍提供旁证。
- 注意：KICK 恢复场景下 ring/snapshot dump 已先行落盘（kick 前执行），不影响取证完整性；但 abort 前的 `requesting coredump via data abort` 日志不会出现，属预期。

## 2026-10-15 log31 分析（v02.06 / build 9ff6fe2 / b326）：判读表①未命中、KICK 哑弹、冻点=timer_tick+0x28（调度位丢失成为唯一幸存假说）

产物链：真机 log31（watchdog/site_ring/input/midp_stderr + psp2core coredump）→ v
02.07（build 149b3ce / b328，`out/vpk/midp_vita_v02.07.vpk`，3653717B，md5 e029c2
dc9f6860747796bf22388ba46e）。

### 判读表结果（v02.06 装置实弹首跑）

- **① affinity 行：未命中**。vm 行 `status=RUNNING wait=none prio=0xa0 attr=0x800
00000 aff=0x70000(init 0x70000) cpu=2 lastcpu=2 preempt=0`——三核掩码健康、cpu=
=lastcpu。"mask 损坏"简单形态死亡。
- **② KICK：哑弹（假说仍未测）**。`rc=0x80028025` = `SCE_KERNEL_ERROR_ILLEGAL_C
PU_AFFINITY_MASK`（error.h:172）。我传的 mask=3 非法：**Vita affinity 编码不是
1<<cpu，而是 bit16 起**——vm 线程报 aff=0x70000（bit16|17|18=三用户核），tone/w
d/ticker 报 0x0（MASK_DEFAULT，threadmgr.h:20）。`KICK no effect (ce 398 -> 398
)` 因此无效，不能作为假说证伪。
- 装置其余部分运转正常：ring/snapshot dump 先行落盘，DRES data abort 精确自杀
（pc=wd_thread_routine+0x3ea = `mov.w r3,#0x81000000; str r2,[r3]`），coredump 产
出并被 Vita 写盘。

### 符号化（本轮方法论收获：DRES 指令级定标法）

- coredump 的 `.text` 段不被捕获（GAP 0x81000000-0x81329000），pc 符号化必须借
velf。velf 双域：objdump section 域 0x810xxxxx，nm 域 0x102xxxxxxx，`nm = sectio
n + 0x81000000`。
- **slide=0x11000**，锚点 = DRES 自杀指令双重定标：wd pc rt=0x8101f42a → sectio
n 0x8100e42a = `mov.w r3,#0x81000000; str r2,[r3]`（DRES store 本尊）✓，与 MO
DULE_INFO text 基 0x81011000 页对齐一致。**最终公式：`nm_addr = rt − 0x11000 +
0x81000000`**。
- 教训（已计入记忆）：锚点法（creator 0x8101f51d 对 vita_watchdog_start nm 0x10
200e530）算出非页对齐 slide=0x7f010fed，产出过 `primordial_to_current_thread+0
xf` 的**错误结论**——内核 slide 必页对齐，非页对齐即算术错误信号。DRES 指令级
定标是唯一可信方法，且每次 dump 都有该锚点。

### 冻点符号化结果（交叉验证自洽）

| 线程 | rt | 符号化 |
|---|---|---|
| vm pc | 0x810cd7d4 | **interpreter_timer_tick+0x28**（`bleq 810bc4e4`） |
| vm lr | 0x810cd7bc | interpreter_timer_tick+0x10 |
| bleq 目标 | 0x810bc4e4 | L433+0x2c ∈ **shared_monitor_exit+0x78**（nm 0x1020bc46c） |
| ring0 | 0x81094a4c | JVMScheduler::wait_for(JVMThread*, long long)+0x54 |
| ring1 | 0x81094a4c→0x810929bc | JVMScheduler::pump_events_vita()+0x8 |
| ringSelf | 0x81065438 | ticker_thread_routine+0x50 |
| tone lr | — | tone_player_thread+0x6ab |

VM 线程寄存器：r0=0x81ede328（=wd_snapshot cur_thread，green id=4）、r5+0x40c=
cur_cell ✓、cpsr=0x40070010（**Z=1，user mode**）。timer_tick+0x10..+0x28 序列：
`ldr r5,[pc,#120]; ldr r0,[r5,#1036](=cur_cell); ldr r1,[r0,#104](=thread statu
s); tst r1,#0x40000000; strne r2,[r0,#36]; bleq shared_monitor_exit`；wd_snapsh
ot 显示 status=0x00000000 → Z=1 → strne 不执行、**bleq 必然跳走**。

### 冻点指纹家族（连续两轮同型，强指纹）

- log30：`start_lightweight_thread_asm+0x18 = bl invoke_pending_entries,eq`
- log31：`interpreter_timer_tick+0x28 = bleq shared_monitor_exit`
- 共性：**cpsr Z=1、pc 停在"即将进入的条件分支"上、目标都是 trampoline/tick 家族
无害代码**（pending=0 时一两条指令即返回）。log29 也停在 timer_tick 内（+0x14）
——假说 (b) "timer_tick entered but never completes" 家族复燃。
- shared_monitor_exit 实现已定位：`cpu/arm/SharedStubs_arm.cpp` 汇编生成体（Sha
redStubs::generate_shared_monitor_exit，随 SharedStubs::generate_all 顺序生成，
Skeleton 的 C++ 桩只是声明）。

### 综合判决（log31 定案）

内核视角 VM 线程 RUNNING、affinity 健康、双时钟健康，但 ce 在 12×250ms 内纹丝
不动；pc 停在一条会立即返回的条件分支上。若线程真在核上，3 秒=数十亿条指令，ce
/gc/clinit（全在 .data）必动。**线程没在执行，但内核以为它在跑 → 调度账本丢失
（runqueue 除名/从未重新派发）假说是唯一幸存者**。两种细分解释仍并列：userland
在 shared_monitor_exit 内自旋（可能性低：目标是无害 trampoline）vs 内核 RUNNING
是快照假象（调度位丢失，KICK 哑弹未测）。

### v02.07 装置（本轮落地，build 149b3ce / b328）

- **KICK 三段式**（替换哑弹单 kick，vita_watchdog.c）：
  - KICK1 `aff->0x30000`（核0+1，强制离开 cpu2 的 per-core runqueue）
  - KICK2 `aff->0x70000`（恢复三核）
  - KICK3 `prio->0x99` 后回 0xa0（最后手段 requeue）
  - 任一刺恢复 ce → 假说实锤 + kick 即 shipping workaround；恢复后重新武装（≤3
次），第 4 次挂起走 data abort 拿 coredump。
- CMakeLists.txt 版本 02.07 + log31 判决注释。

### 下轮判读表（log32）

- `[WD] KICK1 aff->0x30000 rc=0` + `KICK RECOVERED` → **调度位丢失实锤**，kick
  即 workaround，v02.08 转向"自动 kick 守护 + 复发率统计"。
- KICK1/2/3 全部 `no effect` → 调度位假说被证伪，转回 userland 自旋方向：在 s
  hared_monitor_exit 入口（bleq 目标 0x810bc4e4 附近）加 site mark / 计数器插桩
  （v02.08）。
- KICK rc≠0（再次非法掩码）→ 检查掩码编码假设（可能 firmware 差异）。

## 2026-09-25 log32 分析（v02.07 / build 0ea9500 / b328）：三刺全生效但无效——"调度位丢失"死亡，Z=1 冻点指纹三连

产物链：真机 log32 → v02.08（build 0ea9500+ / b329，`out/vpk/midp_vita_v02.08.vpk`，3654065B，md5 f3470e66452aa1fe85a006f6a843c8f7）。

### KICK 判决（本轮决定性证据）

```
[WD] KICK1 aff->0x30000 rc=0x70000   ← rc=旧掩码（成功），非 0x8xxxxxxx 错误码
[WD] KICK2 aff->0x70000 rc=0x30000   ← rc 返回的正是 KICK1 写入值 = KICK1 生效铁证
[WD] KICK3 prio->0x99 rc=0x0         ← 成功
[WD] KICK no effect (ce 397 -> 397)
```

- **rc 语义修正**：`sceKernelChangeThreadCpuAffinityMask` 返回**旧掩码**（正值=成功），不是 0。log31 的 `rc=0x80028025` 是真错误（非法掩码 3）。
- 三段全部被内核接受并真实作用于 VM 线程（两次强制 runqueue 迁移 + 优先级变更），ce 仍纹丝不动。
- **判决**：一个能被两次强制迁移、且内核账本持续报 RUNNING 的线程，不可能"READY 但从未被 pick"——**"调度位丢失（runqueue 除名）"假说死亡**。剩下两类解释：
  1. **内核态烧核自旋**（线程真在核上跑，pc 快照看不到——如轻量锁/信号量内核路径）
  2. **userland 活锁**（线程真在跑，但 green-thread 机制永远不再推进 ce——pc 残留总落在 tick/trampoline 桩）

### 冻点符号化（slide=0x12000，DRES 指令级定标，b328 velf）

- 锚点：wd 块 +0x480=`'DRES'`/0x81000000；b328 DRES store = Thumb `str r2,[r3]` @section 0x8100e578（wd pc rt=0x81020578）→ **slide=0x12000**（log31 为 0x11000，逐 build 变化，必须每次重定标）。
- 公式：`nm = rt − slide + 0x81000000`；THREAD_REG_INFO 三元组 `[sp][lr][pc][cpsr]` 位于块尾。
- VM：sp=0x8157ffb0、lr=**interpreter_timer_tick+0x10**、pc=**shared_monitor_exit+0x7c**（nm 0x1020bc578）、cpsr=0x40070010（Z=1）。
- **+0x7c 处反汇编**（b328 ARM 代码）：
  ```
  810bc574: ldr r0,[pc,#40]   ; → 0x812e1d10 = cur_cell（字面量+data slide，语义闭环 ✓）
  810bc578: ldr r0,[r0]       ← pc 冻点
  810bc57c: ldr r1,[r0,#36]   ; pending entries
  810bc580: cmp r1,#0
  810bc584: moveq pc,lr       ; 无 pending 秒回
  ```
  **无环、无系统调用**——活线程不可能在此停 3 秒。这是 bleq 跳入后的第一条指令（timer_tick+0x28 `bleq` 的目标就是 0x810bc574），pc 与 lr 同族自洽。
- **Z=1+条件分支冻点三连**：log30 `start_lightweight_thread_asm+0x18 bl,eq` / log31 `timer_tick+0x28 bleq` / log32 `shared_monitor_exit+0x7c`（bleq 目标体内首条 ldr）。**三次都停在"进入 tick/trampoline 桩"的瞬间快照**——这是"pc 快照 ≠ 线程真实位置"的最强提示：真实冻点在别处，pc 只是上次换出时的残留。

### 其余证据

- site_ring：`rt_ticks=408 ttk=348 jsmp=348`、`switch=188601`（log31:193559 同量级）、q5 队列 id=4/3 重复 15 组（log31 一模一样）——**既有形态，非新损坏**。
- 输入链正常死亡：HEARTBEAT 到 #385，`[ANI] CE #397 ANI OUT` 后无新 CE——与历次同型。
- 内核行：vm `aff=0x70000 cpu=0 lastcpu=0`（这次在核 0，log31 在核 2——漂移，非钉死）。
- tone：lr=tone_player_thread+0x6ab（正常 delay 等待侧）。

### 综合判决（log32 定案）

"调度位丢失"已死。幸存两假说：
1. **内核态烧核自旋**——线程真在跑（内核账本 RUNNING + 可迁移 + 双时钟推进），但烧在内核态（pc 快照残留无关）；userland 状态永不变化 → ce 冻结。
2. **userland 活锁**——线程真在跑，但 green 机制死循环（如 q5 双线程乒乓），ce 不再推进。

两者的共同点：**线程活着且在执行**；区别在烧核位置（内核 vs 用户）。v02.08 一次实验即可切开。

### v02.08 装置（本轮落地，b329）

- `wd_dump_thread()` 尾部追加 **runClocks 双采样**：间隔 500ms 采样 `SceKernelThreadInfo.runClocks`（线程累计 CPU 周期），d>0 = 真在核上（内核自旋或 userland 活锁），d==0 = 账本幽灵（从未被派发）。同时打 release/chgcpu 计数。
- HANG 链补 **sceKernelGetThreadRunStatus** 行：每核当前正在运行的 tid——若 4 核无一名指 vm tid 而 status=RUNNING，RUNNING 即虚构。
- 均在 KICK 之前执行（kick 恢复不会污染采样）。

### 下轮判读表（log33）

- `runClocks d>0` + 某核 tid=vm → **线程真在跑**：转内核态自旋方向（联系 SceKernel 等待链）或 userland 活锁（q5 乒乓破局——给 wait_for 加看门狗）。
- `runClocks d==0` + 无核持有 vm → **账本幽灵**：调度位丢失复活（但形态变为"账本 RUNNING 却从未派发"），需内核侧取证。
- `runstatus vm cpuN=tid...` 里 vm 持续在场但 runClocks 不动 → 内核统计自身矛盾，报 Vita 内核 bug 方向。

## 2026-09-25 log33 分析（v02.08 / build e984a29 / b329）：runClocks d=0 —— 账本幽灵实锤，两假说同灭，runstatus 装置自毁定因

判决与全部证据链见 `debug/logs/log33/ANALYSIS.md`。要点：

- **vm runClocks d=0（500ms 零周期）、release 冻结、tone d=2005 / wd d=1662 正常** → 内核自旋（烧核必增）与 userland 活锁（烧 CPU 必增）一并死亡；**账本幽灵实锤**（账本 RUNNING(cpu=1) 但从未派发）。
- 与 log32 三刺全 rc=成功却无效自洽：对账本"在核上"的线程，affinity/priority 改写不触碰任何 runqueue。
- **wd 死因**：v02.08 runstatus 块——SDK `SceKernelThreadRunStatus` 52 字节 vs 内核写 0x178 字节 → 栈砸、返回地址打飞（fault 0x352e27 奇地址野跳转；log 断流于 runstatus 行前；site_ring 未创建；core 内 wd 块无 DRES 锚点）。判决数据恰因采样先行而幸存。
- 冻点四连（log30-33 全在 yield/换出路径）：本轮 `interpreter_timer_tick+0x10`（slide=0x16000）——pc 残点=换出残留实锤，与 d=0 互证。
- v02.09（d013e54）：runstatus 安全化（静态 1KiB 缓冲 + hex dump + 3 采样 MOVING/frozen）后置；KICK0=`sceKernelSendSignal`（对账本 RUNNING 线程唯一能触碰派发状态的原语）；KICK4=`sceKernelChangeThreadPriority2`（stub 有、头文件无，即时生效变体）。
- log34 判读：KICK0 rc=0+ce 恢复=幽灵治疗成功；4 核表持续无 vm=幽灵终审；全无效则携 hex dump 转内核 bug 方向。

## 2026-09-25 log35 分析（v02.10 / build b332）：**Phase-1 判决已推翻 —— 头号嫌疑是 tick 内同步 JIT 编译（`Compiler::on_timer_tick`），v02.11 已装阶段探针 + A/B 开关**

> ⚠️ 本节经历**两轮修正**：① "绿灯无界 park / `suspend_ticks()` 非真挂起"（即 git 里的 `69ea4b0`）**已撤回**；
> ② 一切按 `nm − 0x81000000` 换算出的符号归属**全部作废**（正确常数是 `0x81000040`，见下文"site ring 归属"）。
> JIT 编译只是**嫌疑**，未证；v02.11 的两件装置就是为判定它。

判决与全部证据链见 `debug/logs/log35/ANALYSIS.md`。要点：

- **地址映射钉死**：slide=0x77000，两条**指令级**锚点互证——① wd 自己的 DRES
  `0x8108578a`→vma `0x8100e78a` = `str r2,[r3,#0]`（r3=0x81000000）；② vm pc `0x81133a90`→`0x810bca90`
  = **`start_lightweight_thread_asm+0x14`**（green-thread 启动桩簇；`tst r1,#0x40000000` /
  `strne r2,[r0,#36]` / `bleq invoke_pending_entries`）——**不是** `interpreter_timer_tick`
  （初版按错误的 `−0x81000000` 换算得出 `interpreter_timer_tick+0x24`，见下文 0x40 修正）。核心转储 `THREAD_REG_INFO` 布局解出：块基址 0x1e9c、步长 0x178、
  **`+0x040` = saved PC**；5 线程全部符号化（vm / 外来系统线程 tid=0x400100c3 / tone / wd=DRES / ticker），
  tone+ticker 的 pc 都落在 `SceThreadmgr` fstub（各自在 delay 里）→ 交叉验证换算正确。
- **d=0 四连**（`vm runClocks 10227223 -> 10227223`；tone d=2002 / wd d=1642 健康对照）。
- **KICK 全灭且路线终结**：KICK0/1/2/3/4 全部"rc 成功但 ce 394→394 无效果"；
  **KICK5 `SuspendThreadForVM`/`ResumeThreadForVM` rc=`0x80020008` = SCE_KERNEL_ERROR_ILLEGAL_PERMISSION**
  （特权原语）→ 能改派发状态的那条路**封死**。原因：线程停在**合法内核等待**里，
  信号/亲和/优先级都不触碰其等待状态。
- **sysinfo 的 "cpu0 冻结" 降级为噪声**：cpu0 idle/idleN/switchN 三项 200ms 全冻，但 cpu0 = wd 自己
  （其 pc 正是自陷那条 DRES），且快照里另有外来系统线程；vm `aff=0x70000` **不含 cpu0**。
  → 不是独立楔死，不作为证据。
- **决定性新证据（本轮首次看清）**：site 快照 `active_count=5 async_count=1`，
  **五个 Java 线程全部 `wait=<对象>`**（4 个 no-timeout），`timer_ticked=1 ev_readiness=0`，
  **`rt_ticks=1341 ttk=357 jsmp=357` → 约 984 个 tick 被 raise 却从未被消费**；
  site ring `frozen=yes`，`site[0]=switch_thread+0x28`（= `vita_site_mark()` 返回地址）、
  `site[1]=pump_events_vita+0x48`；`lanes fast=382 slow=0 pump=394 switch=190999`。
- **与 log23 指纹逐字节复现**（fast/switch/pump==CE/clinit=`com/app/filemanager/UI` 全同）
  → 长期存在、可复现的同一 bug，**非 v02.09/v02.10 回归**。
- **根因（源码级，v02.11 修正）**：先前 Phase-1 判决"绿灯无界 park（`sleep_time=-1`）+
  `suspend_ticks()` 非真挂起 → 内核无限阻塞"**被本轮证据推翻，现予撤回**：
  - `slow=0` → `Scheduler::yield()` 的慢车道（park 路径）**从未被走过**；
  - **master 模式实锤**（构建产物 `midp_master_mode_events.o` 存在、`slavemode_port/` 无 vita 端口、
    `midp_slave_mode_events.c` 未编译）→ `wait_for_event_or_timer()` 的 `sleep_time=-1` 分支**不可达**；
  - `rt_ticks ≫ ttk` **不是**本次特有证据，而是全部留存挂死日志的共有形态（log30 394/349、
    log31 357/353、log32 408/348、log34 573/350、log35 1341/357），它只说明 **ticker 线程健在
    而 VM 线程不再消费 tick**；
  - **新线索：`ttk` 冻结值在所有日志都落在 349–357**（≈3.5 s VM 运行），而 `rt_ticks` 从 357 到 1341 不等
    → **挂死点由 tick 计数而非用户时序决定**，跨 v02.08–v02.10、与 log19/20/22/23 同指纹
    → 与"首次 JIT 编译被策略在固定 tick 数触发"高度吻合，但同样未经证实；
  - `vita_checkevents.c` 早已把 ANI 等待钳到 ≤50ms；`Main_vita.cpp` 的 slave 模式 `JVM_TimeSlice()`
    循环未被使用（遗留风险 ② 结案）；
  - `pump=394 == CE=394` + 冻结前输入心跳正常 → 泵不是饿死，而是"VM 再没回来调用它"。
- **真实形态**：`ttk == jsmp == 357` 且（由独立 ticker 线程累加的）`rt_ticks` 继续上涨
  → **第 357 次 tick 体进入过（ttk 自增），此后不再有任何 tick 体进入**；`gnrt=0/0`（空队列退出计数为 0）= runnable 队列从未空过；
  `ani_mark.log = "CE #394 ANI OUT"`（下一次 IN 从未发生）= log19/20/22/23 一脉相承的指纹
  （长期 bug，非 v02.09/v02.10 回归）。内核保存的 pc 停在 **`start_lightweight_thread_asm+0x14`**
  （green-thread 启动桩簇，**不是**任何 syscall 入口桩 → VM 不在内核里等；但也不再支持
  "pc 在 tick 桩里"的旧说法）—— 故 **"VM 线程不再消费 tick**（= 不再执行解释器回边检查）"
  成立，而"卡在 tick 体内部**具体哪一步**"要等 v02.11 阶段探针点名。
- **头号嫌疑（v02.11 主攻）**：该窗口内唯一的重活是 **tick 上的同步 JIT 编译** ——
  `Thread::timer_tick()` 在 `#if ENABLE_COMPILER` 下直接调用
  `Compiler::on_timer_tick(_real_time_has_ticked)`（`Compiler.cpp:211` 起的完整编译入口：
  采样当前方法 → `_compilation_abstinence_ticks` 倒数 → 真的编译），**中间没有 safepoint**；
  且本构建**编译器确实编进去了**（`midp_vita.velf`（b332 nm）：`_ZN11JVMCompiler13on_timer_tickEi`=`102074a30`、
  `_ZN22JVMCompiledMethodCache13on_timer_tickEv`=`1020b4df0`；`crumb.log`：`vm UseCompiler(pre)=1`）。
  再加"挂死点恒定在 ~350 tick"这条横向线索，这是当前最强假说，但**仍属嫌疑**：
  pc/ring 两个老定位器都已作废，需要 v02.11 探针/A-B 开关来判定。
- **site ring 归属（已结）**：热点槽（16 槽里 15 个同址）raw `0x810fad0c` − slide `0x77000` =
  0x81083d0c，在 **b332 的 `switch_thread`(VMA `0x81083ce4`, 长 492) + 0x28** 处，
  即 `Scheduler.cpp:2175` 的 `vita_site_mark()` 调用**返回地址**（其前一条 `bl` 目标
  `0x81054bf8` = `vita_site_mark` 本身，b332 VMA）。**上一轮把它算成
  `Os::check_compiler_timer+0x44`、以及"落在 wait_for 区间内且无 mark 调用点"
  都是 nm→VMA 常数取错的产物，该"同向证据"作废。**
  更正：**nm 值 = VMA + `0x81000040`**（不是 `0x81000000`！`.text` VMA = `0x81000040`，
  nm 把 .text 基址当 0）。这就是困扰多轮的"0x40 差异"。**以后取真实 VMA 一律用
  `arm-vita-eabi-readelf -sW <velf>`**（`out/vpk/midp_vita_v0X.YY.sym` 其实是 velf 副本，
  可直接 readelf 回查历史版本符号）；`nm` 输出只用于相对比较。
  ring 的真实含义：它是"**最后一个 mark 站点**"记录器 —— `switch_thread` 是每量子必经之路，
  只要 VM 在跑 Java，16 槽就会被它刷满，**故 ring 内容不能定位冻结点**（log23 已得出同一结论）。
- **v02.11 装置自检（已在最终 velf 闭环验证）**：`readelf -sW midp_vita.velf` →
  `on_timer_tick` VMA `0x81074b20`、`vita_tick_compile_enabled` VMA `0x81054c2c`；
  反汇编 `0x81074b2c: bl 0x81054c2c` —— **A/B 开关确实被 `Compiler::on_timer_tick` 调用**（全 velf 唯一调用点）；
  探针符号 `_vita_ttk_stage`(B `0x10279570c`)、`vita_sched_ttk_stage`(T `0x102082b00`)、
  `vita_sched_ttk_hits`(T `0x102082b10`) 均在 `.sym` 中。
- **v02.11 已实现（探针提交 `12ce198`；由该提交链接产出的 vpk banner =
  `J2ME Player v02.11 b336 (12ce198)`；产物 `vita-port/out/vpk/midp_vita_v02.11.vpk`）**：
  1. `Thread.cpp`：**tick 阶段探针**（stage 1..7 + 各阶段命中数：1=进入 tick、2=java-frame 采样后、
     3=进编译器前、4=编译器返回后、5=`is_stopping` 后、6=`yield()` 后、7=`clear_timer_tick()` 后），
     wd 快照输出 `ttk_stage=N hits 1..7=...`；判读规则：stage 冻结在 N 且 `hits[N]==hits[N-1]` 即卡点
     （3/4⇒JIT 编译中；5/6⇒换出路径；7⇒tick 已完成）。下一次挂死**直接点名**，不再符号考古。
  2. `Compiler.cpp`：**tick 编译 A/B 开关** `vita_tick_compile_enabled()`，读
     `ux0:/data/J2ME00001/tick_compile.cfg`（文件不存在=启用=b332 行为；写入 `0`/`n` 即关闭 tick 上编译）。
     一个 vpk 两种行为：写 `0` 后挂死消失 ⇒ JIT 假说实锤，正式修法改为 safepoint/显式入口驱动编译。
  3. `Scheduler.cpp`：优先级队列 dump 加**环检测** —— 队列是**环形单链表**，定长走法把 log35 的 q5
     打成 ~32 组 id=4/id=3（缓冲写满即停，还静默吞掉了之后所有队列）。现在遇头结点即打
     `CYCLE(n)`，并显式标注 `(buffer full)`/`(max)`。
  4. `OS_vita.cpp`：`site_ring.log` 增打印 `mark=0x%08x`。
- **遗留风险结案**：① `async_count=1` 结构上已定位 —— `_async_count` 只在
  `Scheduler::add_to_asynchronous()` 自增，唯一调用者 `block_current_thread()`
  （上游另见 `sni.cpp:50`），MIDP 侧经 `midpEvents.c:356 blockMonitorThread()`；
  运行期具体线程名未点名（优先级低）。② `Main_vita.cpp:78`/`timeout=-1`：master 模式下不可达
  且 `vita_checkevents.c` 有钳位 → **结案**。③ q5 两元素环：**dump 形态，结案**。
- 下一次实弹判读顺序：先看 `ttk_stage`/`hits` → 若卡在 3/4 则写 `tick_compile.cfg=0` 复跑验证 JIT；
  若卡在 5/6 转 `Scheduler::yield()` 深处；`mark=0x...` 让 ring 槽位可直接 addr2line。

## 2026-09-25 log34 分析（v02.09 / build 8bf56ee / b330 实弹）：runstatus 二杀 wd（死在内核）+ site_ring 终局（VM 冻结于 wait_for 停车点）

> ⚠️ **本节的符号归属与结论已按 `0x40` 换算修正**（原因见下方 log35 章节"第 2 轮修正"）：
> `nm` 值 = `VMA + 0x81000040`，本节所有用 `− 0x81000000` 算出的 `xxx+0xNN` 都要 `+0x40` 重新归位；
> 其中 `site[0]/site[1]` 实为 **`switch_thread+0x28` / `pump_events_vita+0x48`**，
> 即 `vita_site_mark()` 的返回地址 —— ring **不是** "VM 冻结 native 栈"，只是"最后一次 mark 站点"，
> 因此下面的"green 调度器 idle 等待路径停车点 / 冻结于 wait_for"读法**作废**；
> 同理"冻点五连"里的 `interpreter_timer_tick+0x10` 等偏移全部无效，需重算。
> 仍然成立的是 **d=0 三连** 与 **runstatus 二杀 wd（内核态数据中止）** 这两条与地址无关的事实。

判决与全部证据链见 `debug/logs/log34/ANALYSIS.md`。要点：

- **d=0 三连**：vm runClocks 500ms 增量为 0（tone d=1946 / wd d=1671 仪器健康对照）——账本幽灵第三次独立确认。
- **runstatus 二杀 wd，栈砸理论死亡**：v02.09 静态 1KiB 缓冲 + 0xCC 预填完好，wd 仍死——pc 停在 `blx sceKernelGetThreadRunStatus` 桩的**下一条指令**（wd_thread_routine+0x2f5，b330 slide=0x14000 指令级定标），r0/r1 参数原封、一行输出未打。fault 0x1a92c4b 非用户域。**根因：syscall 对幽灵线程（TCB 损坏）查询触发内核态数据中止，查询者陪葬**。该探针永久退役（log33 栈砸 + log34 内核中止 = 两杀）。
- **site_ring 终局**（首次完整落盘）：VM 冻结 native 栈 = `Scheduler::wait_for+0x54 ← pump_events_vita+0x8`——green 调度器 idle 等待路径的正常停车点，非自旋非活锁。**rt_ticks=573 vs ttk=350**：ticker 在走、VM 少消费 223 tick——与 d=0 完全自洽（VM 从未再被调度）。
- **冻点五连**（log30-34 全在 yield/换出路径）：本轮 `interpreter_timer_tick+0x10`。
- v02.10（b4c6006）：runstatus 删除，换 `sceKernelGetSystemInfo` per-core 账本（idleClock/idleN/switchN 双采样，全局仪表不碰 TCB）；KICK 阵列提前（audit 后、site/snapshot 前）成为主实验；新增 **KICK5**=`SuspendThreadForVM`/`ResumeThreadForVM`（stub 有、头文件无，extern）——唯一强制停止/恢复派发状态的原语。
- log35 判读：任一 KICK 恢复 ce = workaround 到手；KICK5 rc 错误 = TCB 损坏致 suspend 失败，转内核 bug 方向；sysinfo switchN 分流 per-core 调度器卡死 vs 单线程幽灵。

## 2026-09-26 log41 分析（v02.16 / build b348 9b3952e）：slide 双向定标结案 + VM 冻结于"新线程派发"路径 + 纠正 CYCLE(4) 误读

对应 coredump：`debug/logs/log41/psp2core-1790442481-0x0001c92af3-eboot.bin.psp2dmp`
（141760 字节，解压后 5183052；49 程序头 / 25 PT_LOAD / 5 线程寄存器块）。

### 1. slide 定标（本次最大收获，此前 3 轮取证都建立在未定标的 slide 上）

**代码 slide = 0x37000，7/7 无歧义收敛**（判别式：`lr−4` 必须是 ARM `BL`（top nibble 0xB）
或 `BLX`（0xFA/0xFB），Thumb `(h & 0xF800) == 0xF000`）。用**栈上 6 个独立锚点交叉复核**，
全部精确落位（`runtime − 0x37000` = `nm`/`objdump` 给出的链接期地址）：

| 栈上值（运行时） | − 0x37000 | 符号 |
|---|---|---|
| 0x810376c7 | 0x810006c7 | `main+0x667`（`main`@link 0x81000060）|
| 0x81037a63 | 0x81000a63 | `_start+0x5b`（`_start`@0x81000a08）|
| 0x810e776c | 0x810b076c | `JVM::start+0x178`（`JVM::start`@0x810b05f4）|
| 0x810e78e4 | 0x810b08e4 | `JVM_Start+0xa4`（@0x810b0840）|
| 0x8107a1bc | 0x810431bc | `midp_run_midlet_with_args_cp+0x95c`（@0x81042860）|
| 0x81088c28 | 0x81051828 | `runMidlet+0x8e8`（@0x81051340）|

**闭环铁证**：`JVM::start` 在 link 0x810b0768 处 `bl primordial_to_current_thread`
⇒ 返回地址 = `JVM::start+0x178`，而 coredump 里 t0 的 `[sp] = 0x810e776c = JVM::start+0x178`
**逐位相等**。反过来也证明布局读法（块 +0x08..+0x44 = r0..r15）正确。

**数据 slide = 0x120000（与代码不同！）**：`primordial_to_current_thread`/`start_lightweight_thread_asm`
都用 `ldr r5,[pc,#…]` 取 link 0x810bd184 的字面量；文件里该字值 = 0x812e1904，而
coredump 里 **r5 = 0x81401904 = 0x812e1904 + 0x120000**。与段表吻合：

| 段 | 链接 VMA | 真机运行基址 | 搬移 |
|---|---|---|---|
| `.text`（0x81000040，AX） | 0x81000040 | 0x81037040 | +0x37000 |
| `.rodata`（0x81106e10，A，同 RX 段）| 0x81106e10 | — | +0x37000 |
| `.data`（0x812e0018，WA） | 0x812e0018 | 0x81400018 | **+0x120000** |
| `.bss`（0x812e2b58，WA） | 0x812e2b58 | 0x81402b58 | +0x120000 |

⇒ **内核把第二个 LOAD 段按独立 4MB 边界重排**，code 与 data 的段间距离比链接期大了 0xe9000。
这条解释了一切"真机挂 / Vita3K 好"的候选（Vita3K 固定 0x81000000 加载，代码与数据一起不动）。

### 2. 冻结现场：VM 主线程正卡在"新线程派发"路径（寄存器逐条对上）

t0（`tid=0x40010003`）寄存器与 `primordial_to_current_thread`（link 0x810bd0e0）逐条对齐：

```
810bd0e8: ldr  r5,[pc,#148]  → r5 = &globals      dump r5 = 0x81401904   ✓
810bd0f0: str  sp,[r5,#1252] → 保存 primordial sp dump sp = 0x813bfdd0   ✓
810bd0f4: ldr  r6,[r1,#100]  → r6 = 线程保存上下文 dump r6 = 0x81ec4e5c   ✓
810bd0f8: ldr  lr,[r6,#4]    → lr = 该线程入口 pc  dump lr = 0x810f4104
                                                  = start_lightweight_thread_asm ✓
810bd0fc: ldr  fp,[r6],#8    → fp = 保存的 fp      dump fp = 0x00000000   ✓
810bd100: mov  pc, lr
```

- `[r6+4] = start_lightweight_thread_asm` + `fp = 0` 正是 `setup_stack_asm`
  （`InterpreterRuntime_arm.cpp:120-140`）给**全新**轻量线程搭的初始帧（帧尾是 `0` 当 frame pointer）。
  ⇒ **被恢复的是一个从未运行过的 Java 线程**（老线程的 `[r6+4]` 会是它上次被换出的地址）。
- 调用链（栈扫描）：`main → runMidlet → midp_run_midlet_with_args_cp → JVM_Start → JVM::start
  → primordial_to_current_thread → start_lightweight_thread_asm → invoke_pending_entries`，
  且栈里还有 `_start+0x5b`/`pte_osTlsAlloc`/`__sinit` ⇒ 这段就是**主 OS 线程自己的 C 运行栈**。
- **结构洞察**：`JVM::start` 里那一句 `bl primordial_to_current_thread`（@0x810b0768）就是
  **primordial 派发环**——`switch_thread` 尾部 `b current_thread_to_primordial`（0x81084364）
  把控制权交回原始栈、回到 `JVM::start+0x178`，再由 `JVM::start` 重新 `bl primordial_to_current_thread`
  派发下一个线程。所以"`[sp] == JVM::start+0x178`"是**稳态**，不是异常。
- `invoke_pending_entries`（link 0x810bce2c，**只有 0x34 字节**，不可能自旋）：
  `ldr r0,&_next_runnable_thread / ldr r0,[r0] / ldr r1,[r0,#36] / cmp r1,#0 / moveq pc,lr / … bl shared_entry`。
  coredump 报的 pc = 0x810f3e38 = link 0x810bce38（`cmp r1,#0`，即该函数第 3 条），
  但 `lr` 与该调用点不自洽（`bleq` 应把 lr 置成 `+0x120`，实测仍是入口值）
  ⇒ **t0 的 pc 字段不可当作单一程序点使用**；可信的是 `sp/fp/r5/r6/lr` 这套（五个寄存器同时吻合）。

### 3. 与"真机挂 / 模拟器好"的关系：能证伪的与不能证伪的

- **重定位漏字假说 → 降级（不是主因）**：`debug/tools/abscheck.py` 静态核查——
  `.data` 里"疑似绝对地址"共 975 字，**975/975 全部被 `.rel.data` 覆盖**（干净）；
  `.rodata` 里有 234 字"疑似绝对地址却未重定位"，但抽样 `0x81140b1c = 0x81108d81`
  → `0x81108d80` 落在**字符串常量 `.LC13`（0x81108d68~0x81108d90）中间**，不是函数；
  其余同型 ⇒ 全部是 1 字节字符串数据里的 4 字节窗口偶然撞上地址区间，**误报**。
  （`-Wl,-q` 保留了重定位，`readelf -r` 共 100644 条：ABS32 68386 / CALL 9746 / THM_CALL 7034 …）
- **`svc 0x009f0001`（phoneME 断点）→ 结案**：二进制里 13 处 + `svc 0x9f0002` 1 处，
  逐条反汇编确认**全部是不可达的断言占位**（`mov pc,lr` 返回之后的填充、跳转表 default）。
  且 `OsMisc_flush_icache` 走 `__builtin___clear_cache`，`arm_flush_icache`/`user_mode_flush`
  整条路不可达。**不要再追这两个 svc。**
- **`CYCLE(4)` → 纠正（我一度当成队列成环真凶，是误读）**：它是 `Scheduler.cpp:2663`
  的**环检测**输出，而 v02.11 的注释（`Scheduler.cpp:2640` 附近）已明确写出
  **优先级队列本来就是环形单链表**，定长走法会重复打印，遇头节点即打 `CYCLE(n)` 是**正常**结果。
  log41 的 `q5: t0x81eeedfc(id=6) t0x81eed958(id=5) t0x81ec3e60(id=3) t0x81e9e934(id=1) CYCLE(4)`
  = **4 个元素的环形运行队列，结构完好**。**不是**成环故障。
- **coredump 覆盖范围（新事实）**：25 个 PT_LOAD **只含线程栈**（t0 0x81381000+0x40000、
  t2 0x813e5000+0x4000、t3 0x813ed000+0x4000、t4 0x81621000+0x10000）与 0xe0xxxxxx/0xd0xxxxxx
  内核区，**不含 0x81000000（代码）和 0x81400000（数据）** ⇒ 以后**不能**从 coredump 验数据段内容，
  只能靠栈 + 寄存器。

### 4. 由此得到的判读与下一步（尚未执行）

冻结形态 = **所有 VM 推进停止**（`_vita_switch_count=84794` 冻结、cpu1 `idleN/switchN`
冻结、`vm runClocks` d=0、`tick` 冻在 1071、ticker 线程却健康地停在 `sceKernelDelayThread`）
**而 cpu1 一直在用户态忙**——即"**不是**在跑一个长 Java 循环（否则 ticker 会走），
**而是** VM 停了、且有一个用户态自旋"。结合 v01.83/v01.93 注释（log9/log19 两次
"pc 冻在 `start_lightweight_thread_asm`，反复重入这条**永不经过 yield** 的路径"），
**冻结位置集中在"新线程启动引导"这一段**。

下一步（按性价比排序，**均未执行**）：
1. 在 `primordial_to_current_thread` / `start_lightweight_thread_asm` / `invoke_pending_entries`
   / `Thread::finish` 各加计数器（复用 `vita_site_mark` 或独立 static），
   下一次卡死即可区分"卡在启动引导"vs"卡在 Java 解释器"。
2. snapshot 增打 `_next_runnable_thread` 与 `*((u32*)_next_runnable_thread + 0x24)`
   —— 后者正是 `invoke_pending_entries` 的判定字（`[thread+36]`），若非 0 说明待处理项无限续期。
3. `menu=0` 的 A/B 至今**从未生效**（log41 日志里是 `launch.cfg not found, defaults in use`）。
   要跑必须把 `launch.cfg` 放到 `ux0:/data/J2ME00001/`（4 行，jar 路径**相对**目录，末行 `menu=0`；
   模板见 `vita-port/ab/launch.cfg.*`）。
   **更正（v02.17 后补）**：这条只是"少跑了一个可选实验"，**不是故障、也不是复现
   冻结的前提**。`launch.cfg not found, defaults in use` 是正常输出——没有它时默认
   就是 `Hello.jar` / `ToneTest` / `portrait`，六轮 log36..log41 都是在这条默认路径
   上复现的。`menu=0` 唯一的作用是跳过原生菜单、省掉每轮手点。真正需要卡上文件的
   是 v02.17 的运行期开关（`tickgate` / `ticker_cpu`），而它们是硬编码绝对路径，
   与 `launch.cfg` 无依赖关系。

### 5. 本次新增取证工具（`debug/tools/`）

- `stack41.py <dmp> <elf> [tid_hex] [slide_hex]`：从 coredump 的 PT_LOAD 里**读线程栈**
  并符号化（fp 链 + 裸扫 sp..sp+0x4000 里的代码指针）。**这是拿到"冻结调用链"的正确办法**，
  此前只看寄存器永远只能猜。
- `ctx41.py <dmp> <elf> <r6_hex> <slide_hex>`：读线程保存上下文块（`[r6]=fp [r6+4]=入口 pc`）。
  注意该块在 Java 堆（0x81exxxxx），**coredump 不含堆**，所以对 log41 返回"未映射"。
- `abscheck.py <elf>`：静态列 `.data`/`.rodata` 里"疑似绝对地址但无重定位覆盖"的字
  （用于复查"装载器漏重定位"类真机专属 bug）。

---

## v02.17（2026-09-26）：一次性插桩 + 运行期 A/B

**提交**：`cba7bac`（`feat(j2me): v02.17 一次性插桩 ...`）
**产物**：`vita-port/out/vpk/midp_vita_v02.17.vpk`（3658851 B）+ `.sym`（4293268 B，
对比 v02.16 的 4290424 → +2844 B，与新增代码量吻合）

### 0. 动机（用户原话）

> "建议能调式的东西，一次性都加上，真机调试比较麻烦"

真机一轮成本 = 重编 + 重装 + 重启 + 取日志，远高于任何探针的运行时开销。
所以 v02.17 把**剩下所有问题需要的一次性全部加上**，并让多个假设能在
**不重编**的前提下 A/B（改 `ux0:` 上的小文件即可）。

### 1. 核心设计：每个读数打印两次（delta 取代单帧猜测）

`site_ring.log` 现在每个样本出现两遍，中间 `sceKernelDelayThread(400*1000)`：

```
===== v02.17 sample 1 =====
===== v02.17 sample 2 (+400ms) =====
```

**log36..log41 六轮全部倒在同一个歧义上**：单个冻结样本无法区分"死锁"与
"只是慢"，为此反复追加日志、反复上机。现在看 delta：

| 现象 | 结论 |
|---|---|
| 两个样本完全相同 | 该路径彻底停住 |
| `switch`/`loops`/`ticks` 在涨，`pump`/`ttk` 不动 | 有东西在跑，但不是有用调度 |
| `ticker loops` 不涨 | **ticker 线程本身没被调度**（直接检验 v02.14 同核改动 / log40-41 "cpu1 scheduler DEAD"） |

实现：`vita_wd_snapshot()` 拆成 `vita_wd_snapshot_one(fd)` ×2（`OS_vita.cpp`）。
看门狗线程本来就是活的，所以在看门狗线程上做双采样是安全的 —— **绝不对外来
TCB 发系统调用**（log33/log34 就是这么把看门狗搞死的）。

### 2. 新增探针清单

| 探针 | 埋点 | 回答 |
|---|---|---|
| `vita_run_count` | `JVM::run()`（`JVM.cpp`） | VM 主循环是否进入过（log41 的冻结函数） |
| `vita_sched_start_count` | `Scheduler::start()`（`Scheduler.cpp`，v01.94 站点旁） | 环只有 16 槽会被覆盖，这里是永不丢的分母 |
| `vita_thread_start_count` + `vita_tc_note()` | `Thread::start()`（`Thread.cpp`） | 线程创建数 + **名字** |
| `vita_thread_finish_count` | `Thread::finish()`（`Thread.cpp`） | debugger 路径的线程结束 |
| `vita_lw_exit_count` | `Thread::lightweight_thread_exit()`（`Thread.cpp`） | ARM 路径上**真正**的最后一口气 |
| `vita_start0_count` | `Java_java_lang_Thread_start0`（`Natives.cpp`） | Java 层 `Thread.start()` 调用数 |
| `vita_justborn_dispatch` | `switch_thread()` 的 `THREAD_JUST_BORN` 分支（`Scheduler.cpp`） | 与 `th_start` 相减 ⇒ "创建了但从没被派发" |
| Java 线程名环 `vita_tc_class/method[8][40]` | `Thread::start()` | 创建/死亡自旋会**重复同一对** `Class.run` ⇒ 不需要 Java 堆就能点名 Java 代码 |
| 调度环 `_vita_disp_ring[32][4]` | `switch_thread()` 顶部 | 冻结前在哪些线程间轮转（`id/pri/st/pe`，`pe`= 是否还有待执行入口） |
| `_next_runnable_thread` 缓存 | `switch_thread` 之外 | `nrt cell/obj` + `id/pri/st/jsp/wait`；非 NULL 缓存指向死对象 ≠ NULL 缓存 |
| **Java 栈顶 16 字** | `vita_sched_dump_stack()` | coredump **只含 native 栈**，`setup_stack_asm` 的引导帧（fp=0、`start_lightweight_thread_asm`、`TestCompiler`）此前**从未可观测**。栈顶仍符合模板 ⇒ 该线程**从没进过 Java**。带 `0x81000000..0xa0000000` 范围保护。 |
| ticker 活性 `vita_ticker_loops` / `vita_ticker_last_ms` | `ticker_thread_routine`（`OS_vita.cpp`） | ticker 是否还活着；`ago_ms` 两采样增长 ⇒ 完全没被调度 |
| `vita_tickgate_mode` | `vita_jit_tick_compile_enabled()` | 当前 tick 编译门模式（0/1/2） |

### 3. 运行期 A/B 开关（`vita_cfg_flag` / `vita_cfg_digit`）

`ux0:/data/J2ME00001/<name>`，**文件不存在 = 行为与 v02.16 完全一致**：

| 文件 | 值 | 含义 |
|---|---|---|
| `tickgate` | `1` | ticker >1 s 没刷新（VM 独占 CPU）⇒ **跳过** tick 触发的同步 JIT 编译（这既是探针也是**修复**） |
| `tickgate` | `2` | 完全不在 tick 上编译（= `tick_compile.cfg` 写 `0`） |
| `ticker_cpu` | `0`/`1`/`2` | ticker 用全部用户核 / 钉 cpu1 / 钉 cpu2 |
| `tick_compile.cfg` | `0` | v02.11 原开关，语义未变 |

`vita_cfg_flag` 带按名缓存（单次上机只读一次文件），`vita_cfg_digit` 返回数字。
**教训**：第一版缓存查找用 `const char*` 指针比较（`vita_cfg_names[i] == name`），
两个内容相同但地址不同的字面量会误回落到默认值 —— 已修成 `vita_cfg_name_eq()`
（先指针相等快路径，再逐字符循环）。

样例文件：`vita-port/ab/tickgate.{1,2}`、`ticker_cpu.{0,1,2}`（复制到真机时**去掉
扩展名**）。完整跑法见 `vita-port/ab/README.md`。

**这些开关与 `launch.cfg` 无关**（这一点曾经被写重，特此更正）：

- `OS_vita.cpp` 里的路径是**硬编码绝对路径** `ux0:/data/J2ME00001/<name>`，
  不经过 `chdir()`、不读 `launch.cfg`、不看菜单选了哪个游戏。
- 因此"本轮日志里 `launch.cfg not found, defaults in use`"**完全不影响**
  开关能否生效——没有 `launch.cfg` 时默认值是 `Hello.jar` / `ToneTest` /
  `portrait`，走的是同一条冻结路径，冻结照旧复现。
- 唯一要求：**开关文件必须在应用启动前存在**（每进程只读一次并缓存，
  运行中补放无效）。
- `launch.cfg` 里那行 `menu=0` 的唯一作用是**跳过原生菜单**（省一次手点），
  属可选便利，不是复现冻结的前提。

### 4. 关键文件修改记录（v02.17）

| 文件 | 改动 | 规模 |
|---|---|---|
| `phoneme-cldc/src/vm/os/vita/Globals_vita.hpp` | v02.17 计数器/环/`vita_tc_note`/`vita_cfg_flag`/`vita_cfg_digit`/`vita_jit_tick_compile_enabled`/`vita_tickgate_mode` 声明 | +62 |
| `phoneme-cldc/src/vm/os/vita/OS_vita.cpp` | 状态定义 + `vita_tc_note` + `vita_cfg_*` + tick 编译门 + ticker 活性戳 + `ticker_cpu` A/B + `vita_wd_snapshot` 拆双采样并新增 7 个读段 | +338/-5 |
| `phoneme-cldc/src/vm/share/runtime/Scheduler.cpp` | 调度环宏 + `Scheduler::start` 计数 + `justborn` 计数 + `nrt/dispatch/stack` 访问器与 dump | +151 |
| `phoneme-cldc/src/vm/share/runtime/Thread.cpp` | `th_start`+名字环 / `th_finish` / `lw_exit` | +29 |
| `phoneme-cldc/src/vm/share/runtime/JVM.cpp` | `run` 计数 | +8 |
| `phoneme-cldc/src/vm/share/natives/Natives.cpp` | `start0` 计数 | +4 |
| `phoneme-cldc/src/vm/share/compiler/Compiler.cpp` | 追加 `vita_jit_tick_compile_enabled()` 门 | +9 |
| `vita-port/CMakeLists.txt` | 版本 → `02.17` + 注释块 | +36 |

**总计 +632 / −5，8 个文件**；共享文件只插 `#if defined(VITA)` 块 + 一行自增，
无格式重排、无 `phoneme_source/` 改动、无 `cldc/build/share/` 改动。

### 5. 构建与验证（实际命令）

```bash
source /home/zyb/vitasdk/vitasdk.sh

# 1) VM（PRODUCT 配方必须完整；尾部 loopgen 宿主链接错误是预期噪音）
cd /home/zyb/vitasdk/samples/j2me && ./rebuild_vm.sh build
#    -> "== 33 ARM objects OK ==" / "== library packed: 29 members =="
#    产物 phoneme-cldc/build/vita_arm/dist/lib/libcldc_vm.a（vita-port 实际链接的就是它）

# 2) 校验插桩符号（C 链接、无 mangling）
arm-vita-eabi-nm --defined-only .../dist/lib/libcldc_vm.a | grep -E 'vita_run_count|vita_tc_note|vita_cfg_flag|vita_jit_tick_compile_enabled|vita_tickgate_mode|vita_sched_dump_stack'
#    -> 全部为 T/B（非 mangled）✓

# 3) VPK（需要 JDK8 在 PATH，build_jar.sh 依赖 javac）
export PATH=/home/zyb/tools/jdk8u502-b07/bin:$PATH
cd /home/zyb/vitasdk/samples/j2me && bash vita-port/build.sh
#    -> "Build successful" / out/vpk/midp_vita_v02.17.vpk

# 4) 确认插桩进了最终镜像（.sym 是未 strip 的 ELF；最终 midp_vita 本身被 strip）
strings -a out/vpk/midp_vita_v02.17.sym | grep -E 'vita_run_count|vita_tc_note|vita_jit_tick_compile_enabled|vita_tickgate_mode|vita_sched_dump_stack|vita_cfg_digit|vita_justborn_dispatch'
#    -> 7/7 命中 ✓
```

**坑（重复踩过）**：`bash vita-port/build.sh` 若 PATH 里没有 JDK，会在
`build_jar.sh: line 22: javac: command not found` 处 `exit=127` —— 报错点离
根因很远，先看是不是 JDK 没在 PATH。

### 6. 建议的上机顺序（一个 VPK 回答多个问题）

> **前提澄清**：`launch.cfg` **不必准备**。没有它时内置默认就是
> `Hello.jar` / `ToneTest` / `portrait`，冻结路径一模一样；
> `launch.cfg not found, defaults in use` 是正常输出，不是故障。
> 放 `launch.cfg`（`launch.cfg.hello`，4 行、jar 路径**相对**、末行 `menu=0`）
> 只是为了**跳过原生菜单**、省掉每轮手点，属可选。
> 下面三个开关全是硬编码绝对路径，**不依赖** `launch.cfg`。

1. **基线**：卡上**什么都不放**（只有上一版 VPK 本身）。取双采样基线，
   确认冻结是否仍在 `start_lightweight_thread_asm` 附近。
2. **`ticker_cpu.2`**（ticker 挪离 VM 核）：冻结消失 ⇒ v02.14 的"同核"
   就是原因；`ticker loops` 仍在涨而 VM 不复活 ⇒ ticker 与冻结无关。
3. **`tickgate.2`**：真正跑一次 v02.11 那个**从未生效**的假设。

每轮只换 `ux0:` 上的小文件 + 重开应用，**不需要重装 VPK**。

### 7. 遗留风险

- `vita_sched_dump_stack()` / `vita_sched_nrt_info()` 会解引用句柄；若句柄被
  撕裂（写入方已冻结、正常情况不会），理论上可能让看门狗异常。已加地址范围
  保护，但这是刻意接受的取证风险。
- 双采样让看门狗在 HANG 后多停留 400 ms（原 ~200 ms 的 site 活性测试之外），
  对"尽快抓 coredump"有一点影响；`WD_STALL_POLLS` 等阈值未改。
- `vita_cfg_digit` 未做按名缓存（只在看门狗 dump 与 tick 门里各调一次，tick 门
  自身有 `_vita_tickgate_mode` 一次性缓存），所以没有额外 I/O 负担。
- 所有开关默认关闭 ⇒ v02.17 的**行为与 v02.16 一致**，差异只在取证输出。

## 2026-09-27 log42 分析（v02.17 / build b354 f9965d7）：探针自伤实锤 + 钉核历史澄清 + 三类证据收官方案（v02.18）

判决素材：`debug/logs/log42/`（第 6 轮真机独立复现，钉 CPU1）。

### 1. log42 核心事实

- **卡死照旧**：`HANG ce=222 polls=12`，KICK0~5 全部无效（`KICK5 susp/res
  rc=0x80020008/0x80020008` —— 对幽灵线程 suspend 依旧失败），`kclock kd=100270
  us` 看门狗仪器健康。
- **d=0 四连**：`VMPROG-A switch=234186->234186(d=0) fast=150->150(d=0)
  site=235469->235469(d=0) jsmp=123->123(d=0)` + `VMPROG-B tick d=0 ttk d=0
  pump d=0` —— 与 log36~41 完全同款：**VM 所在核的调度器整体死亡**（idle
  clock / idle-exit / switch count 三冻结），其余核正常跑 tone/wd。
- **vm 线程状态**：`status=RUNNING wait=none aff=0x20000(init 0x70000) cpu=1
  lastcpu=1` —— 内核眼里 VM 线程"正在运行"、无等待对象、亲和性曾被改成
  CPU1（v02.13 起的钉核），但它所在的 cpu1 调度器死了，所以这个"RUNNING"
  永远不会被真正执行。**内核账本说它在跑、硬件说它没跑——这就是幽灵线程
  的完整定义**。
- **ticker 迁核成功但无效**：`cldc_ticker ... aff=0x20000 cpu=32767 last=1
  READY` + `ticker loops=1140 ... ago_ms=14492` —— ticker 挪到 cpu2 后自己
  活着（loops 在涨），但 VM 死不复活 ⇒ **v02.14 的"ticker 同核抢占"假设
  正式排除**。

### 2. 探针自伤实锤（v02.17 快照死因）

`site_ring.log` **恰好 61 行，最后一行是 `nrt cell=0x815d3848 obj=0x81ede328`**，
之后 dispatch ring、java-stack-top、sample 2 全部消失——watchdog 线程死在
`vita_sched_nrt_info()` 内部：`obj=0x81ede328` 之后要打 `id/st/pri/jsp/wait`，
旧实现走 `t().priority()` → `thread_obj()`(+32) → `ThreadObj::priority()`(+4)
链式解引用，**watchdog 线程没有 GC 安全帧**，stale thread_obj 触发 data
abort → 进程死亡 → coredump 吃掉快照剩余部分。

反汇编定位（v02.17 .sym）：`vita_sched_nrt_info+0x38` 是 `ldr r7,[r3,#4]`
（r3=stale thread_obj）——与"恰好死在 nrt 行后"互证。

**教训（写进探针设计准则）**：看门狗线程上的探针**只准裸字读 + 范围检查，
禁止链式解引用任何对象方法**——GC 安全帧之外没有 WriteBarrier/读屏障保护。

### 3. 钉核历史澄清（git 考古结论，写进 v02.18 注释）

| 版本 | 钉核 | 结果 |
|---|---|---|
| v01.x ~ v02.11（log1~36） | 不钉 | 全部卡死 |
| v02.12（log37） | CPU0 | 卡死，cpu0 停摆 |
| v02.13~v02.17（log38~42） | CPU1 | 卡死，cpu1 停摆 |

**钉核从未改变冻结，停摆核永远跟随 VM 所在核**。钉核是诊断对照不是起因；
v02.18 恢复默认不钉，把"不钉核下停摆核是否仍跟随 VM"作为第 (a) 类证据。

### 4. v02.18 三类证据设计（用户要求"一次性做好"）

| 证据 | 载体 | 判读 |
|---|---|---|
| (a) 不钉核停摆核跟随 | `vm_boot.log` 的 `vm cpu:` 行 + watchdog sysinfo per-core | unpinned 下停摆核号 == VM `lastcpu` ⇒ 跟随实锤（第 7 轮） |
| (b) 完整调度器双采样 | `site_ring.log` 6 段 `[rdN ... ok]` 标记 | rd1 dispatch ring / rd2 java-stack-top **提前**，探针自伤也吃不掉；缺哪段标记=死在哪段 |
| (c) 内核态 dump | `kernel_forensics.log`（kubridge） | `kubridge: present` + per-core `cur_tid` 双采样；停摆核的 cur_tid 400ms 不变 = 内核侧同款冻结 |

## v02.18（2026-09-27）：不钉核默认 + vm_cpu 运行时开关 + kubridge 内核取证 + 探针自伤修复

**用户原话**："不钉核 + 分析内核态，一次性做好，真机调试很麻烦，不想来回折腾"

### 1. 改动清单

| 文件 | 改动 |
|---|---|
| `vita-port/src/vita_main.c` | `VITA_VM_CPU_AFFINITY_MASK` 默认 0x20000→**0**（不钉）；新增 `read_vm_cpu_mask()` 读 `ux0:/data/J2ME00001/vm_cpu`（'0'=不钉/'1'=CPU1/'2'=CPU2/缺省=编译默认）；启动日志 + crumb 记录 |
| `phoneme-cldc/.../Scheduler.cpp` | `vita_sched_nrt_info` 完全重写：`Thread::*_offset()` 编译期偏移 + 裸 volatile 字读（id/st/jsp/wait/pe/tobj），obj 范围检查（0x81000000..0xa0000000 外打 skip），**丢弃 priority()**；`dump_threads` 全局链表段去 priority()、wait_obj 改字读 |
| `phoneme-cldc/.../OS_vita.cpp` | `vita_wd_snapshot_one` 重排：**rd1 dispatch ring + rd2 java-stack-top 提前** → rd3 nrt → rd4 ticker/boot → rd5 线程名历史 → rd6 dump_threads；每段尾 `[rdN ... ok]` 标记；sample 头 v02.18 |
| `vita-port/src/vita_kernel_forensics.c/.h`（新） | kubridge 探测（模块枚举 type 0x00，找 "kubridge"，失败即置 absent **绝不调 ku\***）+ threadmgr 定位（segments[1] 数据段）+ `tmgr+0x54` per-core ctx 指针数组双采样（400ms）+ `kf_kaddr_plausible()` 双窗口门控（K0 direct 0x81000000..0x83ffffff / kernel vm 0x88800000..0x8b000000，越界 skip 不 deref） |
| `vita-port/src/vita_watchdog.c` | HANG 路径 sysinfo 双采样后、coredump 自杀请求前插 `vita_kf_dump()` |
| `vita-port/CMakeLists.txt` | 版本 02.18 + `SceKernelModulemgr_stub`/`kubridge_stub` 链接 + `kernel/kubridge.skprx` 打进 VPK 根 + 新源文件 |

### 2. kubridge 安全设计（读源码后的关键决策）

`kuKernelCpuUnrestrictedMemcpy`（kubridge main.c:62）实现 = 存 DACR → 写
0x15450FC3 → 逐字节 `ldrbt/strbt` → 恢复 DACR。**无任何容错：读未映射内核
地址 = 内核态 data abort = 主机 panic 风险，用户态不可捕获**。因此：

1. **模块枚举先行**：只信 `sceKernelGetModuleInfo` 返回的段地址（内核自己
   报告的），不信任何硬编码基址；
2. ** kf_kaddr_plausible() 双窗口**：只允许 K0 direct map 与 kernel vm 两
   个窗口，`tmgr+0x54` 偏移若因固件差异指向窗口外 → skip 行，不 deref；
3. **absent 即静默**：模块列表里没有 kubridge ⇒ 之后绝不调任何 ku\* 函数
   （无插件时 syscall trap 是致命的），log 写一行 ABSENT + 部署提示。

### 3. 上机步骤（一轮拿全三类证据）

1. 安装 `midp_vita_v02.18.vpk`（默认不钉核）；
2. （可选，复现钉核对照）放 `vm_cpu.1`；
3. （可选，内核取证）从 VPK 解出 `kubridge.skprx` → `ux0:/tai/` +
   `config.txt` `*KERNEL` 段加 `ux0:tai/kubridge.skprx` → 重启；
4. 正常进游戏等冻结 → watchdog HANG 自动落盘三类证据。

### 4. 判读表

| 现象 | 结论 |
|---|---|
| `vm cpu: unpinned` + 停摆核 == VM lastcpu | 停摆跟随 VM 实锤（第 7 轮独立确认） |
| `[rd1..rd6 ok]` 全在 | 快照完整，按 v02.17 判读表走 |
| 缺某段 rd 标记 | 探针死在那一段（不会再发生：已无链式解引用） |
| `kernel_forensics.log`: `kubridge: present` + 停摆核 cur_tid 双采样不变 | 内核侧调度器冻结实锤 → 方向转内核 bug（固件/taiHEN 中断屏蔽类） |
| `kubridge: ABSENT` | 插件未部署，本轮只有 (a)(b) 两类证据 |

### 5. 构建验证记录（2026-09-27）

- `./rebuild_vm.sh`：33 ARM objects / 29 members 重打包（Scheduler.cpp 的
  `wait_obj().obj()` 编译错误已修——ReturnOop 本身是指针，去掉 `.obj()`；
  尾部 loopgen 链接 `fast_memclear` undefined 是**预期噪音**，PROJECT_MEMORY
  607 行已有记录）；
- `bash vita-port/build.sh`：`Build successful`，`out/vpk/midp_vita_v02.18.vpk`
  (3671892 B) + `.sym` (4295948 B)；
- 产物核对：`vita_kf_dump` 符号在位（0x8100f26c T）；`kubridge ABSENT`/
  `SceKernelThreadmgr data`/`corectx read FAILED` 字符串在位；`vm cpu:
  unpinned (mask=0, kernel default 3-core)` 在位；`[rd1 dispatch ok]` ~
  `[rd6 dumpthreads ok]` 六段标记在位；VPK 内 `kubridge.skprx` 在位；
  版本串 `J2ME Player v02.18 b354 (f9965d7)`（提交后需重编刷新 commit 号）。

### 6. 遗留风险

- `tmgr+0x54` 是 fw 3.60/3.65 社区 RE 偏移未在本机验证——plausibility 门控
  使错误偏移降级为 skip 行而非 panic，风险已缓解但取证可能空手而归；
- per-core ctx 内 `cur_tid` 之外的 w1/w2/w3 语义未定（idle counter 猜测），
  首轮以"400ms 不变"为判据，不靠语义；
- kubridge 读内核窗口 0x88800000..0x8b000000 是保守包络——若 threadmgr 数据
  段落在窗口外（几乎不可能，K0 direct 映射内核镜像），kf_find_threadmgr
  返回的段地址会因 plausibility 失败而整体 skip。

## 2026-09-28 log43 分析（v02.18 / build b355 4ecee3b）：三类证据一次拿全 + "任意点冻结"形态坐实

判决素材：`debug/logs/log43/`（第 7 轮真机独立复现，钉核形态——卡上有 `vm_cpu` 文件，内容为字符 `'1'`）。

### 1. v02.18 三项设计验收（全部生效）
- 探针自伤修复：site_ring.log 6409B 完整，6 段 rd 标记×2 采样全落盘（log42 只活到 61 行）
- 停摆核跟随 VM：watchdog cpu1 idle/idleN/switchN 全 d=0，cpu0/2/3 活跃；vm cpu=1 lastcpu=1 aff=0x20000
- kubridge ABSENT 分支合规返回（skprx 未部署），内核侧 WHY 证据本轮缺失

### 2. 核心证据链（各环节全有数据）
vm_cpu.1 → VM+ticker 双双钉 CPU1 → CPU1 内核停摆 → VM 冻在任意点 + ticker READY 永不派发
→ timer_ticked=0 → VM 调度器停转（ttk 117 冻结、switch=221083 d=0）→ watchdog CPU0 幸存取证。

### 3. 冻结签名漂移 = "任意点冻结"铁证（本轮最重要结论）
- log41: bc_impl_fast_igetfield（v02.16）
- log42: png_do_strip_channel（v02.17）
- log43: native_stringbuffer_append_entry / bc_impl_fast_anewarray_internal（v02.18，StringBuffer.append 路径）
三个完全无关的站点命中同一形态 → 排除 VM 代码死循环路线，问题在内核/底层夺走 CPU 不归还。
（log43 site ring slide=0：站点地址未滑直接命中符号表。）

### 4. 新证据细读
- q5: id4→id3→CYCLE(2) 是正常形态（循环单链表 2 元素），log35 的 32 对才是 dump-walk 伪影
- dispatch ring 16× id=3：冻结前 32 次切换全从 id3 切走，最终 cur=id4 冻结；id4 切换疑似不走
  VITA_DISP_REC 分支——待查记录点分支条件（低优）
- 线程 wait 状态自洽：id0=0x81eda000, id1/id2=0x81eb6ee4(同对象), id3=0/no-timeout, id4=0/timed
- coredump thread[0]（pc=_fseeko_r+0x124, r0=0x81ede328, r1=0）判定存疑：与自杀路径（写
  0x81000000）不符，r0 把 ThreadDesc 当 _reent* 不合法——倾向寄存器窗口错位（本轮 psp2core
  note 布局有变化，slide42.py 对 log43 失败、threads note 为空，旁证）。遗留：人工重定窗口 offset。

### 5. 下一步
1. 删卡上 `ux0:/data/J2ME00001/vm_cpu` 文件跑不钉核对照轮（判定钉核是否必要条件；boot_log 出现 "vm cpu: unpinned" 即生效）
2. 部署 kubridge.skprx（ux0:tai/ + config.txt *KERNEL 段 + 重启）——CPU1 停摆的 WHY 靠它
3. 低优：VITA_DISP_REC 分支核查 / coredump 窗口重定 / OS_linux.cpp.bak 清理

## 2026-09-27 v02.19（49cf496）：修正 02.18 残缺的不钉核默认值 —— log43 钉核来源纠偏

### 1. 事故：02.18 的"默认不钉核"只改了一半
- vita_main.c 的 #ifndef 兜底改成了 0，但 CMakeLists.txt:1593 的
  target_compile_definitions 仍带 v02.13 的 VITA_VM_CPU_AFFINITY_MASK=0x20000
- 预处理器守卫永不生效 → 02.18 每次启动仍钉 CPU1
- **log43 boot_log "pin vm cpu mask=0x20000" 来自编译常量，不是 vm_cpu 文件——
  卡上从未存在过该文件**（此前"删 vm_cpu.1"的指引是错的，用户发现"没有 vm_cpu"属实）

### 2. 修复（vita-port/CMakeLists.txt，两处）
- 编译默认 VITA_VM_CPU_AFFINITY_MASK=0x20000 → **0**（真正不钉核）
- VITA_VERSION 02.18 → 02.19
- vm_cpu 运行时开关不动，仍是免重编的 A/B 旋钮（'1'=CPU1/'2'=CPU2）

### 3. 验证（反汇编级）
- main 中 sceIoOpen(vm_cpu) 失败分支：02.18 blt → mov r4,#0x20000（缺失=钉核）；
  02.19 直接走 dlog("vm cpu: unpinned...")（缺失=三核默认）
- 产物：out/vpk/midp_vita_v02.19.vpk (3671937B) + .sym，版本串
  "J2ME Player v02.19 b356 (49cf496)"

### 4. 语义纠偏（影响 log43 判决的一处）
log43 仍应读作"钉核形态第 7 轮"（mask=0x20000 是事实），但其成因是编译默认
而非用户放置的 vm_cpu 文件——这不改变停摆跟随 VM 核的结论，只纠正证据来源描述。

### 5. 下一轮（v02.19 上机）
1. 装新版 VPK，什么都不放 → 首次真正的不钉核对照轮（boot_log 应出现
   "vm cpu: unpinned (mask=0, kernel default 3-core)"）
2. 部署 kubridge.skprx（ux0:tai/ + config.txt *KERNEL 段 + 重启）拿内核侧证据

## 2026-09-27 log44 分析（v02.19 / b356 49cf496）：钉核假设判死 + 冻结签名统一收敛到 stdio/sceIo

判决素材：`debug/logs/log44/`（第 8 轮真机独立复现，首次真正不钉核轮）。

### 1. 两大决定性事实
- **钉核不是必要条件**：boot_log "unpinned (mask=0)"（v02.19 修复生效），
  vm aff=0x70000 三核漫游，cpu=0 lastcpu=0；cpu0 idle/idleN/switchN 全 d=0
  （整核停摆，idle clock 停 = 调度器死），cpu1/2/3 活跃。
  停摆核跟随 VM 宿主核：log37~43 钉哪冻哪，log44 不钉冻在漫游落点 cpu0。
- **冻结签名收敛到 newlib stdio**：
  log43 coredump pc=_fseeko_r+0x124（当时误判"窗口错位存疑"——纠偏：真实冻结点）
  log44 site ring=_vfprintf_r+0x1260 / __ssvfscanf_r+0x9f4
  三函数底层全走 sceIoRead/Write/Lseek —— VM 线程卡在文件 syscall 未归。

### 2. 内核态硬事实与新主假设
- idle clock 停止 = 该核不再执行任何代码（用户态不可为）。
- KICK0~5 全无效（signal/affinity/prio/suspend 均失败）——线程在内核不可中断路径。
- 主假设：VM 线程的 sceIo* 进入内核互斥/文件系统驱动路径后死锁（持锁自旋或
  关中断死循环）拖死整核。与三轮全冻在 I/O 函数完全自洽。
- ticker 本轮 rt_ticks=1616（不钉核轮与 VM 同核组，观测性好于 log43 的 0），
  冻结前 ago_ms=2 最后殉葬；watchdog/tone 在他核存活完整取证。

### 3. 遗留
- log44 coredump thread[0] pc=_rom_text_block8+0xc638 与 site ring stdio 签名不一致
  （自杀窗口快照局限，note 加密窗口归属未定）——以 site ring 为准。
- kernel_forensics.log 仍 ABSENT（kubridge 未部署），内核 WHY 证据缺。

### 4. 下一步
1. 部署 kubridge.skprx（ux0:tai/ + *KERNEL 段）——读 threadmgr per-core ctx/runq，
   直接看停摆核的内核路径，定位死锁点的唯一手段。
2. v02.20 候选：VM 侧零文件 I/O 对照轮（日志静默 + jar 预载入内存），
   不冻→锁定 sceIo 死锁；仍冻→转向调度器本身。

## 2026-09-27 v02.21 — kubridge 预检升级：ABSENT 实锤为枚举盲区

### 判决（v02.20 真机 + 静态证据链）
- v02.20 真机 boot_log：`kubridge precheck: ABSENT`（kubridge.skprx 已部署 ur0:tai/）。
- 决定性推理：eboot.bin(FSELF) 解包验证 `kuKernelCpuUnrestrictedMemcpy` 为
  **强符号 fstubs 导入**（.vitalink.fstubs.kubridge）——若 kubridge 模块真缺席，
  模块加载器在 main 之前就拒绝拉起进程；实际进程正常启动并打出 precheck。
- 结论：**kubridge 模块在内核中且 syscall 可用**；`sceKernelGetModuleList(0x00)`
  从用户态**枚举不到内核模块**（boot-config 加载的内核插件对用户态模块表不可见）。
  v02.20 预检假设的风险实锤。

### v02.21 改动（vita_kernel_forensics.c 最小补丁，三处）
1. ku_probe()：依次尝试全部 list 旗标 0x00/0x01/0x02/0x04（fw 语义不一），
   名字命中按旗标 OR；缓存末次 enum rc + 返回模块数（g_ku_enum_rc/g_ku_nmod）。
2. vita_kf_precheck() 升四态：present /
   **installed-NOT-visible**（enum 成功但无 kubridge 且 ux0/ur0:tai/kubridge.skprx
   文件存在 → 盲区实锤，路线换内核侧 dumper 插件，不是重装）/
   ABSENT（文件都没有）/ 枚举失败归 ABSENT。
3. vita_kf_dump() ABSENT 分支加 `enum rc=%d nmod=%u` 诊断行。

### 判读表（v02.21 boot_log）
- `kubridge precheck: present` → 取证路线不变，HANG 后等 kernel_forensics.log。
- `installed-NOT-visible` → 用户态读内核路线判死，v02.22 转内核 dumper 插件
  （ taihen 模块内读 threadmgr + 文件落盘）或第 2 步零-I/O 对照提前。
- `ABSENT` → 插件真没装上（检查 config.txt *KERNEL 段）。

### 遗留
- kernel_forensics.log 的内核读数仍未拿到（盲区未突破前不指望本轮出）。
- 低优：VITA_DISP_REC 16x id=3 疑点；OS_linux.cpp.bak 清理。

## 2026-09-27 log45（v02.21）— 双判决：枚举旗标错杀 + 整核死假设判死

### 判决 A：v02.20/21 "ABSENT" 是枚举姿势错，非盲区非未装
- log45 kernel_forensics.log：`enum rc=0 nmod=0` —— 四旗标全部 API 成功但 0 模块。
- 对照 SDK 示例 samples/prx_loader/main.c:47：`sceKernelGetModuleList(0x80|0x1,…)`
  （0/1=user，0x80=system 域）用户态可见 ~13 模块。裸 0x00..0x04 全空。
- 强符号 fstubs 导入 + 进程正常启动 ⇒ kubridge 一直在内核（v02.21 推理仍成立）。

### 判决 B："整核调度器死"假设判死（重要反转）
- log45：VM cpu=2/lastcpu=2 且 runClocks d=0（线程死），但 cpu2 本身
  idle d=199129 / switchN d=90 —— **宿主核调度器活著**。cpu1 全 d=0 是无关键。
- 停摆本质＝**线程级内核不可中断等待**：KICK0 signal rc=0 却无效、
  KICK5 suspend 0x80020008（非法状态）全部自洽。
- log43/44 的核冻结判读系 sysinfo 短窗口误判；ticker 本轮活着（rt 1412→1473）。
- 冻结签名第 4 轮收敛 stdio→sceIo：site=interpreter_fast_method_entry_4，
  VM C 栈=inflate（png 解码，audit png_read_image calls=5）；
  历史：log43 _fseeko_r、log44 _vfprintf_r/__ssvfscanf_r。
- **主假设唯一化：VM 线程的 sceIo* 内核路径不可中断阻塞（死锁/损坏页等待）。**

### v02.22 改动
1. ku_probe() 旗标矩阵改 0x80|0x1..4 system 域，记命中 flag（g_ku_flag）。
2. 新增 vita_kf_list_modules()：boot 时把模块表（旗标×模块名×24 条）+ tai
   目录探测落入 kernel_forensics.log —— 一轮看清用户态可见性。
3. precheck 四态语义不变，但 ABSENT 分支诊断行加 flag。

### 判读表（v02.22）
- `kubridge precheck: present` → HANG 后 kernel_forensics.log 直接出内核读数。
- `installed-NOT-visible` → 系统域枚举可见但无 kubridge → 换内核 dumper 路线。
- `ABSENT` + module 表里无 kubridge 但 tai 探测 1 → 同上换路线。
- 模块表 flag 行全 empty → 枚举仍全盲 → 直接上内核 dumper，不再试用户态。

### 遗留
- cpu1 全零窗口成因待定（tone/wd 均 lastcpu=1 且活，非本问题阻塞项）。
- VITA_DISP_REC 16x id=3 疑点；OS_linux.cpp.bak 清理（低优）。

## 2026-09-27 log46（v02.22）— 判决：kubridge 真没装；冻结签名第 5 轮收敛 I/O 路径

### 判决 1：kubridge 确实没有运行（三轮探测线终结）
- **模块表实锤**：v02.22 `vita_kf_list_modules` 输出 flag 0x81 域 15 个模块，
  含 taiHEN 注入的 oclockvita/vsh/Framecounter（证明 taiHEN 本身工作正常），
  **唯独没有 kubridge**。
- **文件探测**：`tai ux0:0 ur0:0` —— sceIoGetstat 在两个 tai 目录都找不到
  kubridge.skprx。用户以为部署到了 ur0，实际文件不在。
- **v02.21 推理正式作废**：「强符号导入 + 进程能启动 ⇒ kubridge 必在内核」
  被推翻——VitaSDK stub 库生成 **GEN_WEAK_EXPORTS 弱导出**（vita-elf-create
  对未解析导入不阻链接、不阻启动），进程照常启动与模块在否无关。
- **修复路径比预想简单**：v02.18 起我们自己的 VPK 根就打包了 kubridge.skprx
  （`vita-port/kernel/kubridge.skprx`，14820B，SCE 加密态，CMakeLists:1839），
  随安装落在 `ux0:app/J2ME00001/kubridge.skprx`。taiHEN 只认 config.txt，
  不扫 app0。用户侧只需在 `ur0:tai/config.txt`（或 ux0:tai/）加一行：
  `*KERNEL app0:kubridge.skprx`（或把文件拷到 ux0:tai/ 再写
  `*KERNEL ux0:tai/kubridge.skprx`），重启即生效——无需找文件。

### 判决 2：第 5 轮冻结签名，仍收敛 stdio→sceIo
- site ring frozen（seq=173989）：[0]=JVMObjectHeap::allocate、
  [1]=Java_java_lang_Thread_start0；VM C 栈/寄存器段落 =
  **png_image_read_and_map**（0x810cc5e4..0x810cd560 区间，libpng 经 stdio
  FILE* 读图，audit png_read_image calls=5）。
- 五轮签名链：log42 _fseeko_r → log43 _fseeko_r → log44 _vfprintf_r/
  __ssvfscanf_r → log45 inflate/png → log46 png_image_read_and_map。
  **主假设不变且增强：VM 线程的 sceIo* 内核路径不可中断阻塞。**
- cpu1 计数三零在 log45/log46 两轮相同（与 VM 落点无关）→ cpu1 计数器
  疑似天生不走字，log45 判决 B（线程级停摆，非整核死）维持，但该核计数
  数据从此只作参考不作判据。
- 旁观线程：Permissions.getGroupMessages/sha1_block（MIDP 安全初始化），
  与主停摆无因果。

### v02.23 改动（39bef65）
1. `ku_clock_probe()`：直接调用 `kuPowerGetSysClockFrequency()`——模块真在
   内核则返回 111/166/222/333/444 sane 值；不在则 0/错误码。绕过枚举与
   stat 的一切盲区，终判探针。boot 与 HANG 两条路径都打
   `kuPowerGetSysClockFrequency: <n>`。
2. precheck 新态 `CALLABLE (enum blind)`：clk>=41 即返回，枚举盲区不再
   影响判决。
3. ABSENT 分支输出修复指引：探测到 `app0:kubridge.skprx`（VPK 自带）时
   直接打印 config.txt 该加的那一行；`tai ux0:.. ur0:.. bundled(app0):..`
   三态探测。
4. CMakeLists 版本块 02.23 注释记录 log46 判决与 v02.21 推理作废。

### 判读表（v02.23，用户修好部署后）
- `kubridge precheck: present` → 模块表可见，HANG 后 kernel_forensics.log
  直接出 threadmgr 内核读数（v02.18 链路自动激活）。
- `CALLABLE (enum blind)` → 枚举盲但直调成功 → kf_read 正常走。
- `ABSENT` + `kuPowerGetSysClockFrequency: 0` → 真没装，按日志里的
  config.txt 指引修。
- **注意：用户修好 kubridge 部署后无需等新版本——现有 v02.22/23 的
  vita_kf_dump 在 HANG 时会自动做内核读数。**

### 遗留
- THREAD_REG_INFO 1888B 布局未破解（现用代码域扫描法绕过）。
- cpu1 计数器歧义标注（见判决 2）。
- 下一步候选：若 kubridge 修好后 threadmgr 读数到手，对照 VM 冻结时刻
  的 cur_tid/runqueue 状态定性「不可中断等待」的具体内核对象。

## 2026-09-27 log47（v02.23）— 判决：kubridge 已加载！门控 bug 在我方

### 判决：用户的 config.txt 修好了，kubridge 真的在内核里跑
- 三条铁证：
  1. boot_log：`kubridge precheck: CALLABLE (enum blind)`（v02.23 直调探针
     成功）；
  2. kernel_forensics：`kuPowerGetSysClockFrequency: 190`（合法时钟值）；
  3. **core dump EVENT_LOG 段内内核侧启动日志**：
     `[kubridge ]:Starting... OK 1758usec`（taiHEN 开机加载成功记录）。
- 生效的是 `app0:kubridge.skprx` 行（VPK 自带文件）；`ur0:tai/kubridge.skprx`
  行实际无效（`tai ux0:0 ur0:0`，ur0:tai/ 里没有该文件）。
- 模块枚举 flag 0x81 依旧 15 模块无 kubridge——**用户态枚举对 taiHEN 内核
  插件结构性失明**，与加载状态无关，log46 "ABSENT" 与本轮 "CALLABLE" 的
  矛盾由 v02.23 直调探针解决。

### 本轮冻结形态（第 6 轮，签名继续收敛 I/O）
- watchdog：VM runClocks d=0、exec 三计数全零、ticker 活（rt 1629→1678）、
  KICK 五连无效、suspend 0x80020008——与前五轮完全一致。
- VM 落 cpu1 且 cpu1 三计数 d=0（与 log45 cpu2 活形成对照——cpu1 计数器
  歧义依旧：log45/46 cpu1 恒零与 VM 落点无关，本轮 VM 恰在 cpu1）。
- site ring 全窗 `png_do_read_transformations`（[0]/[1]=810d8930/810d6414，
  归属 png_do_read_transformations+0x377c/+0x1260，符号区间 0x484c 内合法）
  ——第 6 轮冻结在 libpng 变换循环，仍走 stdio→sceIo 读图路径。
- THREAD_REG_INFO 布局部分破解：头 0x28 + 6 条目×0x178；VM 条目
  （tid 0x40010003）用户代码指针全 deadbeef/0 → VM 冻结时**不在用户态**，
  寄存器上下文在内核栈——与"内核不可中断等待"判读自洽。

### v02.24 修复（3d0e6f6）：门控 bug——内核读数一轮都没跑
- log47 暴露：kubridge 已在但 `vita_kf_dump` 打印 "ABSENT (kernel probes
  skipped)" 后直接 return——**kf_read/vita_kf_dump 门控用的是盲枚举
  ku_probe()**，直调成功也进不去。
- 修复 1：`ku_available()` = 枚举 OR 直调 ≥41（最低 Vita 总线时钟）。
- 修复 2（连锁）：`kf_find_threadmgr()` 同样依赖盲枚举（flag 0x00 永远找不到
  内核模块）。主路改走 **taihenModuleUtils 跨域查询**：
  `module_get_by_name_nid(KERNEL_PID=0x10005,"SceKernelThreadmgr",...)` +
  `module_get_offset(pid,modid,segidx=1,0,&addr)`——taiHEN 自己导出的用户态
  API，读内核自己的模块表，绕过用户态枚举盲区。旧枚举留作无 taiHEN 兜底。
  （已核对 taiHEN 源码 module.c：module_get_by_name_nid 用
  ksceKernelGetModuleList(pid,0xff,1) 查全表；module_get_offset 直接给
  segments[segidx].vaddr+offset。stub 库 taihenModuleUtils_stub 4 函数全
  GEN_WEAK_EXPORTS 弱导入，无 taiHEN 时返回错误码安全降级。）
- CMakeLists 链接 `taihenModuleUtils_stub`。
- 产物 `out/vpk/midp_vita_v02.24.vpk`，版本串 `J2ME Player v02.24 b365
  (3d0e6f6)` 已验证。

### 下一轮预期（v02.24 真机）
- boot：precheck 应仍为 `CALLABLE (enum blind)`。
- HANG 后 kernel_forensics.log 首次应出现：
  `kubridge: present` + `SceKernelThreadmgr data @ 0x88xxxxxx` +
  core0..3 ctx ptr + 双采样 cur_tid/计数。若 taiHEN 查询失败则退回
  "threadmgr module not found"（说明 fw 偏移/权限另有问题）。
- 拿到 per-core 双采样后，可对 VM 所在核判定：runqueue 空转？cur_tid
  恒为 VM？idle 不走字？——定性"不可中断等待"的具体内核对象。

## 2026-09-28 log48（v02.24）— 半程成功 + WD 死因抓个正着

### 判决 1：v02.24 门控修复生效，但 taiHEN 查询在 HANG 路径永久阻塞
- kernel_forensics.log 首次出现 `kubridge: present` +
  `kuPowerGetSysClockFrequency: 190`——十二轮以来内核探针第一次真正跑
  起来（此前十一轮全部 ABSENT 误报）。
- 但日志到此中断：`SceKernelThreadmgr data @ ...` 一行没打。core dump
  给出死因：**watchdog 永久阻塞在 module_get_by_name_nid 的 syscall 里**。
  THREAD_REG_INFO WD 条目（布局见判决 2）：
  - r0=0x10005(KERNEL_PID)、r2=0xFFFFFFFF(KF_IGNORE_NID)、
    r3=0x81638b70（info 缓冲区）——正是该调用实参（r1 为
    "SceKernelThreadmgr" 串指针，rodata 页不在 core）；
  - pc=0x02a28948（SVC veneer）、lr=0x8105b55d（veneer 后返回地址）；
  - **info 缓冲区 = 0x40 后全零**：我写的 size=64 在，内核从未写回。
- 机理：taiHEN 桥在内核态拿内部锁后 copy 给用户；目标核卡死时锁不释
  放，调用者不可中断睡眠。教训固化为规则：**跨域查询只能发生在系统
  健康时（boot），HANG 路径只做纯 ku memcpy 读**（log48 证明后者在
  HANG 上下文可正常返回——present/时钟两行就是那时写出的）。

### 判决 2：THREAD_REG_INFO 布局完全破译（本轮最大工具性成果）
- NOTE 段 `THREAD_REG_INFO`(type 0x1004)：头 0x28 + 6 条目，stride=0x178。
  以 tid 字为 +0：r0..r12=+0x04..+0x30，sp=+0x34，lr=+0x38，pc=+0x3c，
  cpsr=+0x40。
- 验证：WD cpsr=0x00010030（USR+Thumb）、tone cpsr=0x40000010（USR+Z）。
- **VM 用户栈就在 core 里**：LOAD 段 0x81381000+0x40000，与 watchdog
  报的 stack=262144 精确吻合；VM 条目的 pc/lr 是堆区垃圾（抢占后寄存
  器块未更新），但栈内容是真的。
- VM 栈顶调用链（sp=0x813bfd88 起）：`_svfprintf_r+0x884` →
  `_svfprintf_r+0x9fc`，帧内穿插 `midpIterateJarEntries+0xaf`、
  `getFirstJarEntryInfo+0x7`、`png_read_transform_info+0x3c`、
  `png_set_text_compression_window_bits+0x18` 与 app0: 路径串指针。
- 结合第 7 轮 site ring（png_do_rgb_to_gray+0x668 /
  png_image_read_colormap+0x13c）：**VM 冻结时在用户态做 PNG/JAR 的
  stdio 格式化，不在任何 syscall 里**——被抢占后所在核停止调度再未
  回来。VM 无辜（没陷进内核），核调度停摆是唯一凶手，边界再收一格。

### 冻结形态（第 7 轮，签名收敛）
- watchdog：VM cpu=2 runClocks d=0、KICK 五连无效（susp 0x80020008）、
  ticker 活（d=49）——与前六轮完全一致。
- sysinfo：cpu2（VM 所在核）三计数 d=0，其余三核全走——"停的核跟
  VM 走"第 7 次复现。

### v02.25 修复（24a9bd3）：查询挪 boot，HANG 路径只留纯读
- 新增 `vita_kf_boot_resolve()`（vita_main.c boot 序列 precheck 后调
  用）：健康上下文跑 taiHEN 查询 + enum 兜底，地址与来源
  （"boot(taiHEN)"/"boot(enum)"/"none"）缓存进 `g_kf_tmgr_cache`/
  `g_kf_resolved_at`（文件级，boot 写、HANG 读）。
- `vita_kf_dump()`：只消费缓存；之后只发 ku memcpy 读。
- 产物 `out/vpk/midp_vita_v02.25.vpk`，版本串
  `J2ME Player v02.25 b367 (24a9bd3)` 已验证。

### 下一轮预期（v02.25 真机）
- HANG 后 kernel_forensics.log 应完整打出：
  `SceKernelThreadmgr data @ 0x88xxxxxx (resolved at: boot(taiHEN))` +
  sample1/sample2 双采样。resolved at: none → boot 查询也败（权限/
  NID/segidx），改 ku 直扫 threadmgr。
- 判读表：VM 核 cur_tid 恒=VM 且 idle 不走 → 核被 VM 独占不切出；
  cur_tid=idle 但 switchN 不走 → 调度器死；其他核正常 → 只剩 VM 核
  runqueue/上下文损坏。

### 事故记录（流程）
- 工作区 PROJECT_MEMORY.md 曾被回退成 9-18 旧版（v01.95 前，4168 行）
  ——`git checkout --` 从 a982426 恢复 6207 行版。怀疑是某编辑器/工具
  用旧缓冲覆盖。今后改文档前先 `git diff` 确认基线。

## 2026-09-28 log49（v02.25）— 判决：v02.25 回归，开机即死（我的 bug）

### 判决 1：闪退根因 = v02.25 的 boot 期 taiHEN 查询杀死了主线程
- 现象：启动游戏秒退。crumb.log 两条 launcher start（用户点了两次），
  两份 core dump 间隔 11 秒。进程里只有 main + SceCommonDialogWorker
  两个线程——VM/watchdog 都还没创建，死在 boot 早期。
- THREAD_REG_INFO 唯一真实条目：tid=0x40010003（主线程）、
  pc=0x02a28948、cpsr=0x00010030（USR+Thumb）——**与 log48 watchdog
  完全相同的 SVC veneer**；r0=0x10005(KERNEL_PID)、r2=0xFFFFFFFF
  (IGNORE_NID)、r3=info 缓冲（内核未写回）——正是
  module_get_by_name_nid 的实参。
- **log48 归因被推翻**：不是"HANG 上下文才阻塞"，是这台机器上该
  taiHEN syscall **任何上下文都不返回**。佐证：module 表脚注
  `tai ux0:0 ur0:0 bundled(app0):1`——用户态 taiHEN 完全不存在，
  veneer 指向的 syscall 门在这台 fw 上是黑洞。
- 教训固化：**无法确认返回的 syscall 不进任何路径**；跨域查询在这台
  机器上已死两次（log48 WD + log49 main），永不复用。

### 判决 2：core 解析工艺更新（log49 顺带收获）
- .psp2dmp 是 **gzip 压缩**的（log48 那份恰好存了未压版？）——先
  `gunzip -c` 再解析；本次 /tmp 解压后 2870140B。
- NOTE desc 偏移可能不按 4 对齐（log49 @0x15f3）——字段要从 0x15f4
  起；THREAD_REG_INFO 头部还有 0x28 前导 + 条目 stride 0x178 不变，
  但 log49 的条目 0 直接从 desc+4 开始且只有部分字段有效（其余
  0xdeadbeef 填充 = 线程退出后的坟墓标记）。解析器要兼容两种布局。
- THREAD_INFO（type 0x1003）可作线程清单交叉验证：log49 里只有
  main（J2ME00001...）+ SceCommonDialogWorker 两条。

### v02.26 修复（7981ca7，b369）：taiHEN Route 1 彻底退役
- 删除 `kf_taihen_resolve()` 函数、`module_get_by_name_nid`/
  `module_get_offset` extern 声明、KF_KERNEL_PID/KF_IGNORE_NID 宏、
  CMakeLists 的 `taihenModuleUtils_stub` 链接——**零 taiHEN syscall**。
- `vita_kf_boot_resolve()` 重写：纯用户态
  `sceKernelGetModuleList/info` 枚举 + `kf_read`（ku memcpy）回读
  自检（首字能读回才收），来源标记 `boot(enum+readback)`。
- 预期：枚举在这台 fw 上仍盲（log46/47 已证），dump 会诚实打
  `resolved at: none` + "threadmgr module not found"。threadmgr 数据
  段地址改由**离线扫 core dump**（MODULE_INFO/SYSTEM_INFO2 段）获得，
  下一版可把扫出的常量硬编进 cfg 作为 manual override。
- 产物 `out/vpk/midp_vita_v02.26.vpk`，版本串
  `J2ME Player v02.26 b369 (7981ca7)` 已验证。

### 下一轮预期（v02.26 真机）
1. **首要：游戏能再次启动**（v02.25 的闪退消失；boot_log 与 log49
   同级别内容 + `kubridge precheck: CALLABLE (enum blind)`）。
2. HANG 后 kernel_forensics.log：
   `SceKernelThreadmgr data @ 0x00000000 (resolved at: none)` +
   `threadmgr module not found - no kernel read`——这是诚实输出，
   不是失败。
3. 若要 threadmgr 四核读数：离线解 core 的 MODULE_INFO 段找
   SceKernelThreadmgr 的内核基址（内核侧段表用户态枚举不可见，
   但 core dump 的 MODULE_INFO 可能含内核模块）→ 下一版加
   hardcode override 探针。

## 2026-09-28 log50（v02.26）— 判决：离线内核取证全面判死 + THREAD_REG_INFO pc 伪影实锤（log48/50 死点判决撤回）

### 判决 0：v02.26 双修复确认生效
- 启动恢复（log49 闪退消失）：boot_log 完整、vm cpu unpinned (mask=0)、
  kubridge CALLABLE、running com.app.filemanager.FileManagerMIDlet
  (games/app (1)/game.jar)、RMS appdb_6B15、jit cfg=0 policy=0（纯解释器）。
- 探针存活：HANG ce=387；watchdog 顺利走到 data abort 触发 core dump。
- HANG 后 kernel_forensics.log：kubridge present、clock 190（log47/48/50
  三轮恒定）、`threadmgr resolved at none`——与 v02.26 预期 2 完全一致
  （诚实输出，枚举在这台 fw 上盲）。

### 判决 1：core dump 里没有内核——离线内核取证三路全灭（本方向终结）
- **SYSTEM_INFO2（4.1MB）判死**：99% 零填充，仅头 32KB + 尾 4KB 非零；
  头部以 `46000000 43454341`（"SECA" 魔数）开头，之后高熵——内核侧
  dump 数据被加密，无字符串可读。搜 SceKernelThreadmgr 零命中。
- **12 个 0xe0xxxxxx LOAD 段判死**：全是用户态系统模块的进程内映射
  （SceCommonDialogWorker 的 entry/栈 0xd0001000 均在其中），含
  SceThreadmgr 等字符串只是系统模块的导入表字符串。非内核内存。
- **MODULE_INFO（17 模块）判死**：全部是用户态模块（midp_vita 基址
  0x81029000 大小 0x2c9108、SceLibKernel、SceCommonDialog、oclockvita
  超频插件、Framecounter 等）——v02.26 预期 3（离线 MODULE_INFO 找
  threadmgr 内核基址→硬编 override）判死，**不要做**。
- 结论：psp2core dump 的用户态视图完整（线程/寄存器/用户栈/模块），
  但内核侧是加密黑盒。threadmgr 内核直读这条线**离线在线均死**，
  后续任何版本不要再投入。

### 判决 2（重大修正）：THREAD_REG_INFO 的 pc 是伪影——log48/log50
"VM 死在 png_write_sPLT 的 mla"判决撤回
- 撤回证据链（五重独立证据）：
  1. **png 写路径是死代码**：我们源码（vita-port/midp-vita 全覆盖
     grep）零调用 png write；objdump 全量 bl 调用者搜索：
     png_write_info←png_image_write_main/png_write_png，而
     png_image_write_main/png_write_png/png_image_begin_write/
     png_write_to_memory/png_write_to_stdio 最外层全部 **0 调用者**
     ——libpng 写路径整链不可达（链接进去但从未被调）。
  2. **cpsr 与指令集矛盾**：VM 条目 cpsr=0x60070010——T 位(bit5)=0
     （ARM 态）、mode=0x10（USR），但 pc=0x810e78f4 处是 Thumb-2
     指令（`fb08 3306` = mla.w）。ARM 态执行 Thumb 代码不可能成立。
  3. **健康线程 pc 同样落死点**：tone/ticker（全系统最健康：delay
     循环、loops 计数在涨、runClocks 在走）的 pc 均为 0x81131418
     ——该地址不在 .text（.text=0x81000040..0x81107e84），落在
     .rodata 的 `_rom_text_block` 区（**ROM 化的 Java 类数据**）。
     健康线程不可能执行 rodata——dumper 的 pc 字段是错的。
  4. **BL 校验栈回溯 0/77**：VM 用户栈扫描 77 个 midp 范围候选字，
     无一前一条指令是 bl/blx——栈内无有效返回地址，与 pc 同源
     伪影（dumper 抓的是陈旧/错误槽位的上下文）。
  5. **log48(v02.24) 与 log50(v02.26) 两次独立 HANG 的 VM pc/sp
     完全相同**（0x810e78f4/0x813bfd88）——两次独立停摆停在同一
     指令的概率趋零；更像 dumper 每次都从同一个错误槽位读出同一
     套陈旧值。版本匹配已排除符号漂移（符号基准 = v02.26 b369
     (7981ca7) = log50 运行版本）。
- 保留的可信证据（不受伪影影响）：
  - r0=0x81eddfb4 = site_ring nrt cell 的 JavaThread obj、r8=
    0x811b49e4 = jsp+16——寄存器与 site_ring Java 状态互恰，说明
    dumper 抓的**寄存器组部分真部分假**（数据真、定位错）。
  - WD 自身 pc=0x81037b40=copy_imageregion+5 是真的（它就是
    faulting 线程，dumper 对 faulting 线程抓真上下文；0x400100bf/
    CDW 的 0xe0013cb4 delay sleep 也合理）。
  - site_ring 的 Java 栈顶（png_do_write_transformations 等）同理
    降级为不可信——那是从 VM 栈槽读出的陈旧栈残留，非真实调用链
    （0x810e5dfc objdump 确实在 png_do_write_transformations+0xa44
    内，但该函数属死代码链，栈残留的来源不明）。
- 修正后的定性结论（不变的事实层）：cpu0 idle/idleN/switchN 三计数
  冻结而其他三核推进 + VM 账本 RUNNING/wait=none/cpu=0/runClocks 冻结
  + KICK0-5 全无效 + Vita3K 不复现——"cpu0 调度死/VM 被困"定性不变，
  **只是"死在哪条指令"永远无法从 core 得知**（内核侧加密 + 寄存器
  pc 伪影）。THREAD_REG_INFO 除 faulting 线程外只可信 tid/名字/栈
  范围/delay/prio/affinity 等静态字段，pc/sp/寄存器一律存疑。
- 教训固化：**非 faulting 线程的"冻结上下文"不可作为死点证据**；
  这台机器的 dumper 对非 faulting 线程抓的 pc/sp 是坏的（可能来自
  线程结构陈旧槽位或 dump 时刻核上下文不可用）。log48 节
  "判决 2：THREAD_REG_INFO 布局完全破译"中的偏移表保留（结构对），
  但其中关于 VM 死点的引申全部作废。

### 判决 3：q5 CYCLE(3) = 正常环标记（调度器损坏假说排除）
- site_ring `q5: t0x81eddfb4(id=4) t0x81eb700c(id=1) t0x81edc2ec(id=3)
  CYCLE(3)` 是 v02.11 修复的 dump 环走完标记（Scheduler.cpp:2850：
  循环单链表走 3 步回到头 = 3 元素正常环）。**不是链表损坏**。
- q5 里 id=4（VM 主 Java 线程）、id=1、id=3 三个 green 线程正常
  runnable 排队——green 调度器队列完好，等的是 native VM 线程恢复，
  与"VM native 线程被困 cpu0"自洽。

### 冻结形态（第 8 轮，签名继续收敛）
- watchdog：VM verdict "exec=0 tick=1 -> ticker RUNS, VM NOT EXECUTING"；
  vm status=RUNNING wait=none cpu=0 runClocks d=0；tone d=2029、
  wd d=1675（仪器健康）；KICK0 signal rc=0 但无效、KICK1/2 aff 迁移
  无实效、KICK5 0x80020008；cpu0 三计数冻结、cpu1/2/3 正常推进。
- 与前七轮一致：停的核跟 VM 走（本轮 cpu0 = VM unpinned 后所在核）。

### 遗留与下一步方向（v02.27 讨论）
- 已终结的方向（不要再投入）：①离线内核取证（SYSTEM_INFO2 加密/
  无内核 LOAD 段/MODULE_INFO 无内核模块）；②taiHEN 跨域查询
  （log48/49 双杀）；③钉核实验（log37-40 四连判死）；④Vita3K
  复现（不复现）；⑤以 core 寄存器 pc 为锚的任何推理（伪影）。
- 存活的事实基座：cpu0 停摆 + VM 困于该核 + green 队列正常 +
  WD/tone/ticker 健康 + KICK 全无效（signal/aff/prio/suspend）。
- 候选方向（下轮与用户讨论，未定）：
  A. **规避类**：接受"某游戏/某操作触发"的偶发性，WD HANG 自愈
     尝试（如检测到冻结后主动 sceKernelExitDeleteThread 重建 VM？
     代价大）或超时自杀重启进程（保用户体验）。
  B. **扩大采样**：WD 双采样间隔从 200ms 提到更大窗口/多轮采样，
     或在 sysinfo 冻结确认后追加一轮"冻结时长"计量，确认停摆是
     永久还是超长暂停。
  C. **用户侧复现条件聚焦**：13 轮 HANG 的游戏/操作分布统计，
     从"什么触发"侧收敛（需用户提供更多轮次记录或回放习惯）。

## 2026-09-30 v02.27：零 I/O syscall 死点追踪器（commit 1bcfa49）

### 背景：用户否决规避方案 + Vita3K I/O 风暴疑云

log50 汇报后用户否决"自杀重启"规避方案（"要找到原因啊……解决不好怎么用"），坚持根因导向。
同时用户观察到 Vita3K 上 J2ME 启动后持续 open/write/close 打印——定量分析结论：

- Vita3K 泵 12000 次/秒（真机 92/s，130 倍），诊断心跳（HEARTBEAT 每 64 pump 写一次
  input_debug.log + ani_mark 每 CE 写一次）被放大成 ~190 ops/s 风暴（126s 内 23830 条折叠消息）
- **判定：模拟器伪影，非 bug**。真机 92/s 泵频下心跳写盘极稀疏；且卡死问题早于诊断代码存在
- 但诊断日志的高频 sceIo 直写确有 churn 嫌疑 → v02.27 顺手改为零 I/O 内存追踪

### 真机死亡时间线（log50 input_debug.log + ani_mark 终判）

- input_debug.log 仅 12 行：`[HEARTBEAT] #1 ms=1790682023456` → `#385 ms=1790682027638`
  = **VM 启动 4.18 秒即死**（不是"操作一会"！），泵 92 次/秒
- ani_mark 最后一行 `CE #387 ANI OUT t=1790682027660`（死前 22ms）
- CE=385 时 ani=384/384 **平衡进出** → VM 线程不是死在事件泵/ANI 等待里，
  **死在两次 CheckEvents 之间的 Java/native 执行窗口**（~11ms 窗口）
- 死亡时间线：boot → 4.18s 正常运行（92 pump/s）→ 死点在 CE#387 与 CE#388 之间

### vita_trace 设计（每线程专属槽——设计修正记录）

第一版共享历史环设计写到一半识别出致命缺陷：tone+ticker+pump ~800 条/s 会在 40ms 内
冲掉死点证据。改为**每线程专属槽**：

- `tr_slot {seq,op,arg,ts_lo}` × 12 槽（VITA_T_MAX），每线程一槽
- seqlock 式：进入调用 seq=(seq&~1)+2+1（ODD=INSIDE），退出清 bit0（EVEN=returned）
- **每线程专属槽保证 entered-never-returned 记录永生**——高频线程只冲刷自己的槽
- `g_hist[32]` 全局历史环仅作上下文参考；`g_thr[12]` 注册表 lazy 自注册
  （首次被追踪调用自动注册，spin gate 一次性进入）
- 每线程每 op 计数 `opcnt[TRACE_OP_N]`——即使无 ODD 槽也能看调用分布

### 18 个 wrap（--wrap 链接器拦截）

sceIo 13 个（Open/Close/Read/Write/Pwrite/Lseek/Sync/Remove/Mkdir/Dopen/Dread/
Dclose/Getstat）+ sceKernelDelayThread + sceDisplay 2 个（SetFrameBuf/
WaitVblankStart）+ sceCtrlPeekBufferPositive + sceTouchPeek。
**sceIoChdir 无用户态 stub，弃用**（链接 undefined reference，已从三处移除）。

模式统一：`enter(op,arg) → __real_* → exit() → return`，零语义变更。

### 判别式（下一轮 HANG 读法）

```
vita_trace_dump("ux0:/data/J2ME00001/trace_ring.log")  # WD HANG 路径，coredump 之前
```

- VM 线程槽 **ODD seq → 死点 FOUND**：该槽的 op 即死在里面的 syscall，arg 给参数
  （io_open 的 arg=文件名指针，dump 时尝试读字符串）
- VM 线程槽 **EVEN → VM 死在任何被追踪 syscall 之外**：纯解释器执行或未追踪路径
  （此时看 g_hist 环 + 各线程槽找异常：如某 native 线程 ODD = 它阻塞了 VM 依赖的资源）
- WD dump 自身也走 wrap（槽最终 EVEN），无害

### 接入点

- watchdog.c：`vita_wd_snapshot(site_ring.log)` 之后、kernel_forensics/data abort 之前
- main.c：boot 序列 `crumb_printf("version:")` 前调 `vita_trace_register()`（锚定主线程 tid）
- CMakeLists：源列表 + wrap 旗标区（png_read_image 之后 18 个）+ VITA_VERSION 02.27

### 验证

- 构建通过：`build/cmake/midp_vita.vpk`（3.6MB，2026-09-30 02:41）
- **wrap 重定向实锤**：`objdump -d` 全二进制 73 个 `sceIoOpen` 调用点全部
  `bl __wrap_sceIoOpen`，唯一直连真实符号的是 wrapper 自身 `__real_` 调用
- 22 个 wrap 符号在 .text 落位

### 下一轮取证清单

1. 装新 VPK 正常玩到 HANG → 取 `ux0:/data/J2ME00001/trace_ring.log`
2. 读法：先找 VM 线程（通常 tid 非最小者，参考 site_ring 的线程表）槽的 seq 奇偶
3. 与 site_ring.log / input_debug.log / ani_mark 交叉对时间戳
4. 若 VM 槽 EVEN 且无他槽 ODD → 死点在纯解释器/native 未拦截路径，
   下一版加 wrap（候选：sceKernelWaitEventCB / mutex / semaphore 家族）

## 2026-09-30 v02.28：sync 家族全量 wrap（commit 05ed8a6）

用户指令："一次性都加上" sync 家族 wrap（不等 v02.27 跑完看 EVEN 再加）。

### 先普查再动手（防 sceIoChdir 型地雷）

对 v02.27 链接产物 `objdump -d` 全量统计（含 `_` 前缀变体）：

| 符号 | 调用点 | wrap? |
|---|---|---|
| sceKernelLockLwMutex / UnlockLwMutex | 9 + 14 | ✅ |
| sceKernelLockMutex / UnlockMutex | 6 + 6 | ✅ |
| sceKernelTryLockMutex / TryLockLwMutex | 0（stub 有定义） | ✅ 防御性 |
| sceKernelPollEventFlag | 5 | ✅ |
| sceKernelWaitSema / SignalSema / PollSema | 4 / 2 / 0 | ✅ |
| sceKernelSendMsgPipe / ReceiveMsgPipe | 2 + 2 | ✅ |
| sceKernelWaitThreadEndCB / SendSignal | 2 + 2 | ✅ |
| sceKernelSetEventFlag / ClearEventFlag | 2 / 0 | ✅ |
| sceKernelSignalCond | 0 | ✅ 防御性 |
| sceKernelDelayThreadCB | stub 有定义 | ✅ |
| **sceKernelWaitEventFlag / WaitCond / WaitSignal** | **0 调用点** | ❌ 无无前缀 stub，wrap 必炸 |
| sceIoChdir | — | ❌ v02.27 已踩过：无 stub |

关键认知：**stub 库里等待类符号多带 `_` 前缀**（`_sceKernelWaitEventFlag` 等），
头文件声明却无前缀——直接按头文件 wrap 会 undefined `__real_`。判据顺序：
ELF 实际调用点 > stub T 符号表 > 头文件声明。

### 新增 18 个 wrap（v02.27 的 18 + 18 = 36 个 __wrap_sce* + 2 旧线程 wrap = 38）

Mutex/LwMutex（lock+unlock+trylock 各 3）、Sema（wait+poll+signal）、
EventFlag（poll+set+clear）、CondSignal、SendSignal、WaitThreadEndCB、
MsgPipe（send+recv）、DelayThreadCB。

arg = SceUID（或 LwMutex work 指针）→ HANG 时冻结槽直接点名**死在哪把锁/管道上**。

### 为什么要 wrap 释放类（不阻塞的调用）

每线程 opcnt 计数器可算 lock/unlock 差值：
- VM 槽 ODD(sema_wait) + 某线程 sema_signal 计数停更 → VM 等的信号量没人发
- VM 槽 ODD(mutex_lock) + 某线程 lock 计数 > unlock 计数 → 它抱着 VM 要的锁
- 这把"谁抱着锁"从离线取证的无解变成了纯算术

### 验证（build/cmake/midp_vita，2026-09-30 构建）

- 一次链接通过（普查先行没有踩雷）
- objdump 抽验 8 符号：全部调用点走 `__wrap_*`，direct=1 均为 wrapper 自身
  `__real_` 调用（LockLwMutex 8/wrap、UnlockLwMutex 13、LockMutex 5、
  WaitSema 3、PollEventFlag 4、ThreadEndCB 1、MsgPipe 1+1）
- `__wrap_sce*` 符号总数 38，账目吻合

### 下一轮 HANG 读法（v02.28 判别式升级）

1. VM 槽 ODD + op 是阻塞类（mutex_lock/sema_wait/msgpipe_recv/delay）→ 死点=该锁
2. VM 槽 ODD + op 是 IO 类 → 死在 IO syscall（v02.27 原判别式）
3. VM 槽 EVEN + 某线程槽 ODD → VM 在等该线程（交叉 site_ring 线程表定位是谁）
4. 全槽 EVEN → 死在纯解释器/未拦截路径，只剩解释器内部死循环或 SMC/timer 类

## 2026-09-30 log51 终判决：trace_ring 首次命中 + slide 发现 + 14 轮同死点确认

### trace_ring 首次命中（v02.27/28 的 38 wrap 首战）

HANG 后 dump 四槽归属：
- 40010003=VM：槽 EVEN，op=io_pwrite returned → **死时不在任何被追踪 syscall**（判别式第 4 格）
- 4001015f=tone：ODD delay（正常睡，runClocks d=2424 活）
- 40010163=watchdog：INSIDE io_write（dump 自身写入，自洽；io_open=28/io_write=182/signal_send=1 与 KICK0-5 历史吻合）
- 400101cf=ticker：正常睡，tick 在涨

VM 槽锁平衡：lwmutex 17655/17656、sema 786:393（2:1 正常配对）→ **无锁泄漏**。

### ★ slide 发现（方法论修正，影响全部历史轮次）

site_ring.log 打印 `self=0x8108d348 mark=0x8108d320`，而 v02.28 ELF 符号
`vita_site_dump=0x81058348 vita_site_mark=0x81058320` → **运行时基址 = ELF + 0x35000**。
log50（v02.26）同法：mark 运行时 0x8107e588 − ELF 0x81055588 = **+0x29000**。

**教训：每份 log 必须用它自己的 site_ring self/mark 字段算 slide，再 addr2line。
此前 log35~50 所有直接解析的运行时地址结论全部作废需重审。**
（v02.11 打印 mark 地址的设计在此兑现了价值）

三重验证：tone/wd/ticker 三个 creator 按各自 slide 修正后全部解出语义吻合的函数
（javacall_media_initialize / vita_watchdog_start / JVMOs::start_ticks）。

### log51 死点证据链（全部 slide 修正后）

1. site ring 尾部：**15×switch_thread mark + 1×pump_events_vita mark**，seq=188039 frozen
   （时间线：pump mark → switch mark×15 → 全冻）
2. jsp 栈：jsp+04−0x35000=0x810bfb94=**L318**（L318..L319 区间）、jsp+48−0x35000
   =0x810bfd04=**invoke5_deoptimization_entry_0** → VM 死时 java 栈在解释器 invoke 桩簇
3. watchdog 交叉：cpu1 idle/idleN/switchN 三计数器全冻结（747035546→747035546）、
   VM runClocks **d=0**（tone 睡觉都有 d=2424 → VM 死时不耗 CPU）、KICK0-5 无效、
   jsmp=351/pump=393/CE=393/switch=186585 全冻结
4. VM 槽 EVEN + d=0 → **不是忙循环、不在 syscall，是"不再被调度"**

### L318 反汇编解码（解释器 invoke 返回点）

```
L318:  mov lr, pc        ; 保存返回地址
       mov pc, r2        ; 调用目标方法（invoke）
0x810bfb94:              ; ← jsp+04 指向 = 调用返回后
       ldr r7/r8/lr      ; 恢复解释器寄存器
       ldrb ip,[lr,#3]!  ; 取下一字节码
       ldr pc,[r5,ip,lsl#2]  ; dispatch 表跳转
```

即 L318 = C 解释器 invoke 调用返回点，紧邻 dispatch 核心循环。栈残留证明最后一次
Java 层活动是 invoke 返回（残留≠死点本身，但结合 ring 时间线收窄窗口）。

### log50 交叉验证：14 轮同一死点

log50 jsp 0x810e5dfc − slide(0x29000) = 0x810bcdfc → **同落 L318..L319**。
两版本、两次独立运行、栈残留各自 ELF 同一标签 → **log35~50 是同一个死点**。
log50 的 png_do_write_transformations 判决确认为 slide 未修正错误（MIDP imgdcd
不用 libpng 的旁证原本就矛盾）。

### JIT 假设排除（但发现未来地雷）

- crumb `vm UseCompiler(pre)=1 (jit policy=0)` 的 (pre) 只是 Globals.hpp 默认值
  （log28 教训注释在案）；实际模式由 runMidlet.c:152-158 决定：policy=0 →
  **确实 `JVM_ParseOneArg("-int")` → UseCompiler=false** → log51 跑纯解释器
- hang 后 runMidlet 未返回所以 (post) 行缺失，与 hang 自洽
- **地雷记录**：`OsMisc_flush_icache`（0x810c41a0）是空壳（`bx lr`，
  `__builtin___clear_cache` 在此工具链没生成 cache 维护指令）；真正的
  `arm_flush_icache`（带 mcr cp15）**零调用者**。一旦 launch.cfg 开 jit=1/2，
  Cortex-A9 分离缓存下 JIT 必因 stale ICache 执行垃圾 → 必须先修 flush 再开 JIT

### 终判决

VM 死在 **switch_thread mark 之后的窗口**（VITA_DISP_REC 之后 → 下个 mark 之前），
即：调度器完成最后一次切换 → 解释器执行若干 invoke（L318 残留）→ 之后
**不再有任何 mark、不耗 CPU、不在 syscall、无锁等待** → 调度器再也没有切换过 VM 线程
（switch 计数冻结）。与 log23（v01.99，纯 Java 计算死循环，switch 持续上涨）形似
但更彻底——log51 连调度器切换本身都停了。

死亡形态=「VM 线程从 ready 队列消失」：可能机制——线程状态机损坏（被误置
SUSPENDED/DORMANT）、green-thread scheduler 的链表损坏导致轮转丢失、或
switch_thread 内部把当前线程 park 后无人唤醒。

### v02.29 取证方向（未实现）

1. 解释器 dispatch 心跳：在 L318 所在的 dispatch 核心循环加每 N 次递增的内存计数器
   （类似 site mark 但更密），区分"解释器在跑但调度停"vs"dispatch 本身停摆"
2. switch_thread 后窗口加密 mark：VITA_DISP_REC 之后、目标线程 resume 处各加 mark
3. HANG 时枚举全部线程运行状态（sceKernelGetThreadInfo 每 tid 的 status 字段），
   直接看 VM 线程是 RUNNING/READY/WAIT/SUSPENDED 哪个——把"不再被调度"变成可观测

---

## v02.29 补洞 wrap（2026-09-30，vita-port bd6d988）

### 动机：log51 判决的 B/D 假说判别器

log51 终判决（上文）确认 VM 死点在 switch_thread mark 后窗口、trace_ring VM 槽
io_pwrite RETURNED（不在任何已 wrap syscall 内）、TCB=RUNNING wait=none、cpu1
三计数器冻结。剩余两假说无法从 log51 区分：

- **B（syscall 内核楔死）**：VM 卡在 v02.27/28 未 wrap 的 syscall 内核路径里。
  RUNNING wait=none 与"syscall 执行中"兼容（syscall 期间线程保持 RUNNING）；
  cpu1 三计数器冻结与"关中断内核循环"兼容。
- **D（switch mark 后自旋）**：VM 在最后一次 switch_thread mark 之后的用户态
  原生代码/调度器内部自旋。VM 是 cpu1 唯一可跑线程 → 不切换不 idle；
  runClocks 只在切换点记账 → d=0 伪影。

### 12 个新 wrap（普查三重核查定稿）

对象 = v02.28 ELF 内有 blx 调用点（>0）且具备可绑定 stub 形态的所有剩余可阻塞
syscall。每符号核查三项：ELF blx 调用点数、stub/fstubs 表项、SDK 头文件原型。

| 符号 | blx 点 | trace op |
|---|---|---|
| sceIoSyncByFd | 1 | io_syncbyfd（**头号嫌疑**=newlib fsync 唯一实现路径，FileManagerMIDlet pwrite 后冲刷） |
| sceIoLseek32 | 1 | io_lseek32 |
| sceIoRename | 5 | io_rename |
| sceIoRmdir | 5 | io_rmdir |
| sceIoChstat | 2 | io_chstat |
| sceIoChstatByFd | 2 | io_chstat_byfd |
| sceIoGetstatByFd | 3 | io_getstat_byfd |
| sceIoDevctl | 1 | io_devctl |
| sceAudioOutOutput | 9 | audio_out |
| sceNetEpollWait | 1 | net_epoll_wait |
| sceKernelExitDeleteThread | 1 | thread_exitdel |
| sceKernelExitProcess | 1 | process_exit |

- **sceKernelExitThread 剔除**：ELF 内 0 直接调用点——所有调用走
  `__sceKernelExitThread_from_thumb` 本地 veneer，`--wrap` 拦不到。
- **Exit 族槽 ODD 语义**：ExitDeleteThread/ExitProcess 成功即不返回，槽留
  ODD 是正确证据（活进程不可能已退出），enter 时 arg=退出码。
- **`_`前缀家族（Lseek32/ChstatByFd/GetstatByFd/Devctl）绑定机制**：ELF 内
  无前缀 sce 符号 = fstubs 三词表项（`.word 0; .word module_id; .word NID`），
  `--wrap=无前缀名` 的 `__real_` 绑到它——v02.27 的 73 个调用点已验证此机制。

### 下一次 HANG 的判别读法

trace_ring.log VM 槽（thread id=VM 线程）：
- **槽 ODD 且 op ∈ 12 新 wrap 之一** → **B 胜**：死点 syscall 被点名（如
  io_syncbyfd ODD = 卡在 fsync 内核路径），下一步做针对性绕过（如 pwrite 后
  不 fsync 或改全量 flush 策略）。
- **槽仍 EVEN（io_pwrite returned 之后无新 enter）** → **D 胜**：死点在所有
  syscall wrapper 之外的用户态原生代码（调度器内部/解释器原生循环），下一步
  对 switch mark 后 PC 区间做反汇编审查 + 考虑解释器心跳 mark。

### 文件与构建

- 改动：`vita-port/src/vita_trace.h`（12 枚举+注释）、
  `vita-port/src/vita_trace.c`（opname 12 项+12 wrap+audioout/net 头）、
  `vita-port/CMakeLists.txt`（12 旗标+v02.29 注释+版本）
- 构建：一次通过，`nm | grep -c __wrap_sce` = 52（40+12）✓；objdump 验证
  1/1/9/1/5/5/2/2/3/1/1/1 调用点全走 `__wrap_*`，真身仅剩 wrapper 自身
  `__real_` 引用 ✓
- 产物：`out/vpk/midp_vita_v02.29.vpk` + `.sym`
- commit：vita-port bd6d988

---

## log52 判决：D 假说胜出——绿线程切换桩死点（2026-09-30）

### 判别器结果：B 拒绝

- **VM 槽（tid 40010003）仍 EVEN**：`io_pwrite (returned)`，12 个新 wrap 已生效
  计数（VM 线程 io_lseek32=2341、io_getstat_byfd=29 只可能来自 v02.29 新计数器）
  但无一 ODD → **B 假说（未 wrap syscall 内核楔死）被拒绝**。
- 二进制身份：b376=98be58b（第 376 个 commit，PM log51 节），构建于 v02.29
  代码生效后；版本串 `v02.29 b376 (98be58b)` 自证。
- **VM 迁到 cpu0**（log51 在 cpu1）——**冻结核心跟着 VM 线程走**（watchdog
  sysinfo cpu0 idle/idleN/switchN 三计数器冻结，cpu1/2/3 活跃）。VM aff=
  0x70000（3 核）但被调度到 cpu0 后不再切换。cpu0 switchN 冻结与"VM 是该核
  唯一可跑线程且不再发生上下文切换"自洽。
- KICK5 susp/res rc=0x80020008 = SCE_KERNEL_ERROR_ILLEGAL_PERMISSION：真机
  上 self-suspend 不允许（Vita3K 允许故 log23 能用）。非新事实，仅确认边界。

### 死点指令级定案（site ring + coredump 双源闭合）

- **site ring**（slide=+0x7D000，由 self=0x810d5d00−sym(vita_site_dump)
  0x81058d00 与 mark 双路一致求出）：[0]=switch_thread, [1..14]=
  pump_events_vita，seq 冻结。与 log51（slide=+0x35000）[1] 同为
  pump_events_vita → **两份独立 HANG 同死点**。
- **coredump**（真机 psp2dmp，解析器 `--slide 0x7d000`）：VM 线程
  **PC=start_lightweight_thread_asm+0x18（0x810c204c）**，LR=同函数+0x0，
  **fp=0、r1(pending)=NULL、r0=Thread obj 0x81eddfb4**，
  SP=0x8167fd88 落在 VM 栈（256KB PT_LOAD 顶）。
- **指令语义**：PC 处是 `bleq invoke_pending_entries`——switch_thread 派发
  新线程时把 PC 打到桩入口（lr=桩+0x0），桩从 Thread obj 加载 pending_entry
  发现为 NULL，**"派发即死"：线程被指派了执行权却从未开始执行**。
- **栈回溯只剩 `JVM::start+0x18c → JVM_Start+0xa4 → midp_run_midlet_...
  with_args_cp+0x95c`** = log21 定案的"切到 primordial 栈后没回来"同型。
- r0 = 0x81eddfb4 与 site_ring `threads(active-list)` 的 `t0x81eddfb4 id=4
  st=00000000 wait=0x00000000 timed` 同一对象 → PC 捕获的是**id=4 线程**
  （"timed" = 最后一次状态转换是 timed wait，但内核视角 RUNNING）。
- 指令序列 `ldr r1,[r0,#104]; tst r1,#0x40000000; strne r2,[r0,#36];
  bleq invoke_pending_entries`：pending_entry 为 NULL 时把 [r0+36] 清零
  （清 JUST_BORN 标志）后尾跳 invoke_pending_entries，尾跳后 PC 会指向
  invoke_pending_entries 而非桩+0x18 —— PC 停在 bleq 本身说明**停在条件
  执行判断处 = 等待/重执行该分支**。结合 fp=0（fp 已被清）与 threads 表
  id=4 "timed"，这**不是 log21 的"从未开始执行"，而是"已在桩内推进到
  JUST_BORN 清除分支"——一个已出生、已派发、卡在出生处理分支的线程**。
- r0 在所有 4 个 dump 线程槽一致（dump 是 sceKernelGetThreadInfo 的用户态
  枚举，r0 为 dump 时刻值，不影响判决）。

### 判决

**log52 = D 假说胜出**：死点不在任何 syscall 内（52 个 wrap 无一 ODD），
死点在绿线程切换桩 `start_lightweight_thread_asm` 的 JUST_BORN 处理分支
（`+0x18`，即清 JUST_BORN 后尾跳 invoke_pending_entries 之前）。

**v02.30 方向（按优先级）**：
1. **在线程结构上加死点可见性**：vita_site_mark 插到 switch_thread 的
   JUST_BORN 分支内 + start_lightweight_thread_asm 桩内无法插（asm），
   但 Scheduler::switch_thread 的 JUST_BORN 分支（L2264 附近）可插 mark，
   下次 HANG 若 ring 冻结在该 mark → 直接命名死点函数。
2. switch mark 后窗口的反汇编审查：switch_thread L2206 mark 之后到
   switch_thread_master_mode 之间的无覆盖区间。
3. (远期) 用 thread id=4 的 Thread obj 字段读取（via coredump PT_LOAD
   0x814018d0 覆盖判断）尝试反推 wait 渠道。

### 危险信号（本次新发现）

- **VM 不再绑定核心**：log51 cpu1 → log52 cpu0。冻结跟着 VM 走 → 问题在
  VM 线程自身（自旋/死循环），不是某个特定核心的外围问题。**oclockvita
  模块在 module 表中**（190MHz 时钟）——若它是 CPU 超频插件，超频状态下
  不稳定性的可能性上升；若用户撤掉 oclockvita 后 HANG 消失，则高度指向
  硬件级不稳定。建议控制变量测试。
- **两份真机 log 的 slide 不同**（log51=+0x35000、log52=+0x7D000）——每次
  启动 ASLR slide 都不同，绝不能跨 dump 复用（已知教训重申）。

### 文件与版本对应

- log52 = v02.29 b376 (98be58b)，对应符号归档 `out/vpk/midp_vita_v02.29.sym`
- 解析：`parse_psp2dmp.py --elf .../midp_vita_v02.29.sym --slide 0x7d000`
- 二进制身份验证：版本串 + b376=git rev-list count of 98be58b

---

## log52 深挖第二轮：dump note 全开采 + 出生/死亡通道判决（v02.29 b376）

日期：2025-09-09（续 log52 判决节）。oclockvita 移除后 HANG 复现 → 硬件嫌疑
排除，问题定死在 JVM 适配层。本节为 dump 取证终局 + v02.30 方案定稿依据。

### psp2dmp note 矿藏（解析器只处理了 2/24 种！）

psp2core dump 含 24 种 PT_NOTE。`parse_psp2dmp.py` 只解 THREAD_REG_INFO
(0x1004) 的**第一条记录**与 PROCESS_INFO (0x1000)。以下为全 note 解码结论：

| note | 偏移 | descsz | 结论 |
|---|---|---|---|
| THREAD_INFO (0x1003) | 0x678 | 1008 | **内核侧 5 线程状态表**（recsize=200） |
| THREAD_REG_INFO (0x1004) | 0x1e74 | 1888 | **5 线程寄存器**（12 字节头 + 5×376） |
| SEMAPHORE_INFO (0x100a) | 0xa84 | 1892 | 27 个系统信号量，无一属 J2ME 线程 |
| EVENTFLAG_INFO (0x100b) | 0x71 | 880 | 无 J2ME 线程 |
| MUTEX_INFO (0x100c) | 0x1590 | 768 | 无 J2ME 线程 |
| LWMUTEX_INFO (0x100d) | 0x18ac | 816 | 无 J2ME 线程 |
| MESG_PIPE_INFO (0x1010) | 0x1bf8 | 608 | VM tid 出现在 SceNetAdhocAuthMbx **+72=creator 字段**，非等待者 |
| STACK_INFO (0x101b) | 0x283c | 88 | {tid,used,max}×5，无新信息 |
| EVENT_LOG_INFO (0x102c) | 0xf47e4 | 2060 | 只含系统 Shell NPXS19999 事件，与 J2ME 无关 |
| SYSTEM_INFO2 (0x102d) | 0xf500c | 4178740 | 哈希数据不可读 |
| SUMMARY_INFO (0x102e) | 0x4f23ac | 612 | SceModule 统计 |
| TTY_INFO2 (0x1031) | 0x4f1384 | 4108 | **psp2core 内核 hook 日志**（见下） |

### 内核侧判决数据（THREAD_INFO + 全同步对象扫描）

**5 线程内核状态**（THREAD_INFO recsize=200, +44=wait/delay 状态字）：

| 线程 | tid | name | state | PC |
|---|---|---|---|---|
| VM main | 40010003 | J2ME00001 | **1 (RUNNING)** | 出生桩+0x18 |
| SceCommonDialogWorker | 400100c3 | - | 8 (delay) | 内核 e000xxxx |
| tone | 4001015f | j2me_tone | 8 (delay) | sceKernelDelayThread |
| watchdog | 40010163 | j2me_watchdog | 1 (RUNNING) | wd_thread_routine |
| ticker | 400101cd | cldc_ticker | 8 (delay) | sceKernelDelayThread |

- **内核认为 VM=RUNNING**，但 PC 冻结、cpu0 switchN 冻结 → 用户态冻结（非内核阻塞）
- VM 不在任何 sema/eventflag/mutex/lwmutex/msgpipe 等待队列 → 排除内核同步阻塞
- SceNetAdhocAuthMbx creator=VM tid：midp 的 net 初始化曾触碰 adhoc（历史痕迹，非阻塞点）

### THREAD_REG_INFO 全 5 线程寄存器（12 头 + 5×376 记录）

**解读**: tid@+0, sp@+56, lr@+60, pc@+64, cpsr@+68。VM: SP=0x8167fd88(原生栈),
LR=0x8113f034(桩入口), PC=0x8113f04c(桩+0x18), CPSR=0x40070010(User/Z=1/中断开)

**全线程死点分布**：其余 4 线程全正常（delay/内核服务/watchdog 在 dump），
**唯独 VM 冻结在出生桩**。三次独立 HANG（log21 Vita3K 桩+0x8、log26 真机
桩+0x14 strne、log52 真机桩+0x18 bl）全落出生桩头 7 条指令内。

### 冻结瞬间原生调用链（栈内存直接判读）

SP 顶帧 `[SP]=JVM::start+0x18c` = `bl primordial_to_current_thread` 返回地址
（gadget `push {lr}` 所压）。调用链：

```
midp_run_midlet_with_args_cp → JVM::start() [JVM::run() 内联]
  → primordial_to_current_thread()  ← SP 冻结在此帧
  → gadget: ldr lr,[r6,#4]; mov pc,lr → start_lightweight_thread_asm
  → PC 冻结在桩+0x18 (bleq invoke_pending_entries)
```

**关键修正**：JVM.cpp:288 `JVM::run()` 被**内联进 JVM::start()**（反汇编
810b5684-569c：`vita_run_count++` 全局 0x814b2ef8 自增紧邻 `bl gadget`）。
此前误以为冻结在 time_slice 循环；实为 **run() 首次派发路径**。但 ring 计数
switch=190836 说明切换发生过 19 万次后才冻结——VM 曾正常运行很久。

### 出生/死亡通道歧义闭合（三大矛盾 + 唯一自洽解）

- dispatch ring 16 条全是 id=3 出让；id=4 从未作为出让方出现
- pe=0（id=4 pending_entries 已消费）+ th_finish=0 + lw_exit=0（无死亡）
- cur_thread=id=4 的 java 栈 jsp+04=L318（id=4 跑过解释器）

**判决**：id=4 的出生桩在绿线程派发时被消费（pe 0←1），thread 曾跑过 Java
代码（L318 帧），但**从未被 time_slice/switch_thread 记入 dispatch ring**。
出生消费不经 switch_thread 的 VITA_DISP_REC——它经 **JVM::run() 首次派发 /
primordial 桩重入**。死亡通道排除（th_finish=0）。

### TTY_INFO2 会话历史（内核 hook 日志）

本次开机内 J2ME00001 **两次 coredump**（前一次 done + 本次 start）+ 4 轮
SceCommonDialog Init/Term type=4（系统错误弹窗，对应 HANG 后弹窗）。另有
PCSG90096 coredump 记录（本机还 dump 过别的应用）。

### 根因假说收敛（v02.30 方案依据）

19 万次正常切换后突然冻结 + 内核 RUNNING + 不在任何等待对象 + cpu0 切换
计数冻结 → **不是逻辑死锁，是物理层冻结**。两个候选：

1. **主候选：VM 线程状态损坏说**——id=4（或 id=3）的绿线程栈链/状态字在
   dump 前被破坏，调度器重入出生桩后无法回解释器。证据：dispatch ring
   全 id=3、pe=0、L318 帧。堆不在 dump 内无法直接验证。
2. **次候选：cpu0 中断/异常静默说**——VM 在 cpu0 用户态执行时遭遇未处理
   异常/中断被静默丢弃，线程永停指令边界。缺乏直接证据，但能解释
   "RUNNING 却不跑"。

### v02.30 方案定稿（判决前不动笔）

本轮已实现所有"看"的能力，下一步是"改"。在关键路径加 mark/计数器：

1. **出生桩消费计数**：`invoke_pending_entries`（C++，SharedStubs_arm.cpp:614）
   入口加 `vita_stub_pe_count++`——直接数出出生桩被进入次数
2. **primordial 重入计数**：`primordial_to_current_thread` 是 asm gadget 无
   C 侧入口；在调用方 C++ 侧（JVM::run 内联点）不可插；改为在
   Scheduler::time_slice() 入口 + switch_thread JUST_BORN 分支加计数
3. 仪表联动：watchdog snapshot 增加新计数器读出
4. （远期）扩展 parse_psp2dmp.py 支持 THREAD_INFO/多线程 THREAD_REG_INFO

### 教训

- **"1 线程 note"是解析器显示限制**——THREAD_REG_INFO 实为 5×376 记录
- **psp2core dump 不含堆**——24 个 PT_LOAD 全是栈/内核区，应用堆 (0x81eXXXXX) 不可读
- **creator 字段 vs 等待者**——同步对象 note 里 tid 出现≠阻塞，先看字段语义
- **JVM::run 内联进 JVM::start**——源码行号与反汇编对不上时先查内联
- **slide 每次启动不同**（log51 +0x35000 / log52 +0x7D000）——绝不跨 dump 复用

---

# v02.30（b384 17ade63）：出生链三探针——两代实现与三次静默失效追捕

## 目标

log52 判决冻结点在出生桩（start_lightweight_thread_asm+0x18，LR=桩入口=primordial
gadget 签名），v02.30 要回答的只有一个问题：**冻结时主循环/gadget 穿越/出生消费
哪个还活着？**

## 第一代实现（58b70e0，部分失效）

- Scheduler.cpp：`_vita_swjb_seen/_vita_swjb_clear`——switch_thread 出让侧统计仍带
  JUST_BORN 位的线程
- SharedStubs_arm.cpp：生成桩 `invoke_pending_entries` pe≠NULL 路径加 4 指令
  `_vita_pe_enter++`
- OS_vita.cpp：wd snapshot 增行 `v230 swjb_seen=… swjb_clear=… pe_enter=…`

## 三次静默失效（每个都是结构性陷阱）

1. **pe_enter 永不生效**：桩符号（invoke_pending_entries/primordial gadget/
   shared_entry/出生桩）全部宿主在 `Interpreter_arm.o`，而 rebuild_vm.sh 的
   pack_lib **故意从备份库回填旧 Interpreter_arm.o**（再生 .s 丢 jvm_f2i/jvm_d2i
   浮点桩）。生成器源码改动永远进不了产物。教训：**改生成桩前先 `nm -A libcldc_vm.a`
   确认符号宿主对象**；若宿主是被回填冻结的成员，asm 层插桩死路。
2. **swjb 两计数器编译死**：`THREAD_JUST_BORN` 仅在 `Scheduler::start()` 的
   `_debugger_active`（本构建恒 0）分支置位——位永远不置，计数器永远 0。
   教训：**加计数器前先 grep 置位点**，确认被测事件在本构建配置下真的会发生。
3. **注释 `*/` 提前终止**：`/* swjb_*/pe_enter ... */` 把块注释拦腰斩断，
   `OS_vita.cpp:1137 'pe_enter' was not declared` 编译失败——但 rebuild_vm.sh
   的 `|| true` 把它吞了，32 对象照常打包（缺 OS_vita.o），库链进 VPK 的是
   04:31 的旧对象。教训：**注释里绝不写 `xxx_*/yyy` 模式**；rebuild 后应
   `strings` 验证新格式串真的入库（本次靠它连抓两层失效）。

第四次是构建系统陷阱：**cmake 不跟踪 `-lcldc_vm` 的 mtime**——库换了 velf 不重链。
改 VM 库后必须 `rm -f midp_vita.velf` 强制重链再 build。

## 第二代实现（30e1e38 + 17ade63，生效）

C 层等价观测（调用图证明覆盖 asm 层）：gadget 仅两处调用——`JVM::run`（首次，
vita_run_count 已覆盖）与 `time_slice`（每次切换）。

- `_vita_ts_enter`：`Scheduler::time_slice()` 入口——Main_vita slave 主循环每圈
  +1（含早退圈）
- `_vita_ts_gadget`：gadget 调用前最后一条 C 指令——数真实穿越进 Java 的次数
- snapshot 行扩为 `v230 swjb_seen=… swjb_clear=… pe_enter=… ts_enter=…
  ts_gadget=…`（swjb/pe 三死计数保留只为快照不撒谎式省略）

### 判读指南（HANG 后两次快照对比）

| ts_enter | ts_gadget | 判决 |
|----------|-----------|------|
| 停滞 | （任意） | 主循环冻死在 gadget 穿越中（log52 情形） |
| 仍涨 | 停滞 | 主循环空转：无 runnable 绿线程，早退 return 100 |
| 仍涨 | 仍涨 | 切换健康，冻结在别处（重看 dump） |

三态覆盖了除"纯 asm 自旋环"外的所有情形；调用图证明纯 asm 环不存在（桩路径必回
C 层）。swjb/pe 预期恒 0，非 0 即本构建配置被改（信息量同样大）。

## 验证链（每层都做了实证）

- `strings libcldc_vm.a | grep v230` → 新格式串入库
- `arm-vita-eabi-nm -A libcldc_vm.a | grep ts_` → accessor T 定义在 OS_vita.o
- `rm velf` 重链后 `strings midp_vita.velf | grep v230` → 新串进 velf
- 产物 `vita-port/out/vpk/midp_vita_v02.30.vpk`（b384 17ade63）：
  - `strings .sym | grep "J2ME Player"` → `J2ME Player v02.30 b384 (17ade63)`
  - `strings .sym | grep "v230 swjb"` → 含 ts_enter/ts_gadget
  - `nm .sym | grep -c vita_sched_ts` → 2

## 部署与预期

- 真机装 `midp_vita_v02.30.vpk`（b384），复现 HANG 后取 psp2core dump +
  wd_snapshot（两次快照对比），读 `v230 … ts_enter/ts_gadget` 行
- 若 ts_enter 停滞（主循环冻死）：冻结点必在 time_slice 体内或 gadget 穿越中，
  结合 dump 的 PC/SP 判据与 log52 一致——顺藤摸 gadget 内部
- 若 ts_enter 仍涨：冻结不在 VM 主循环——转向 MIDP 事件层/绿线程世界内部

## git

- phoneme-cldc（j2me 主仓跟踪）：58b70e0（一代）→ 30e1e38（二代 ts 探针）→
  17ade63（注释修复，b384 实际编译源）
- VM 库：libcldc_vm.a 29 成员（OS_vita.o 06:11 新编、33 对象全量代）
- VPK：v02.30 b384 (17ade63)，06:24 产出


---

## log53 判决：绿线程调度机构用户态自旋转锤 + bleq 微环理论否决（v02.30 b384 首战，2026-10-01）

### 本轮实证（slide=0x21000，self 锚点命中 vita_site_dump+0x0）

**死点（THREAD_REG_INFO 5 线程全解码）**：
- VM(40010003)：PC=0x810e2e2c-0x21000=**invoke_pending_entries+0x8**（`ldr r1,[r0,#36]`
  正在加载 pe 字段），LR=**start_lightweight_thread_asm+0x0**（出生桩入口=gadget
  `ldr lr,[r6,#4]` 签名），r0=0x81ee0d5c=id5 绿线程对象，cpsr=0x40070010（Z=SET、
  User、中断开——与 log52 逐位相同）
- tone/ticker：PC=sceKernelDelayThread+0x8 正常节拍；SceNet 内核态正常
- 原生栈帧与 log52 完全同构：[SP]=JVM::start+0x18c / JVM_Start+0xa4 /
  midp_run_midlet_with_args_cp+0x95c

**watchdog（HANG 时刻）**：VM 内核状态 RUNNING、wait=none、runClocks d=0；
**cpu1 idle/idleN/switchN 三 d=0**（忙而永不换出=用户态自旋铁证）；KICK 全无效；
fast=111/site=19609/jsmp=92 三计数器全冻结（零解释器进展）。

**v230 探针判读（本次核心架构知识）**：`ts_enter=0 ts_gadget=0` 而 `switch=18430`、
`run=1`——绿线程切换**全部**经 trap/gadget 在 VM 内部完成（master 模式），
`Scheduler::time_slice`/Main_vita slave 循环是**死路径**，探针放错了层。
swjb/pe 三计数器如预期恒 0（编译死）。

### 微环理论否决（重要修正）

log52 PM 里"bleq+moveq pc,lr 无限环"假说**错误**：`0x0bffff42` 的 L 位=1，
是 **BL+EQ（带链接）**——ipe 空返回会正确回到 stub+0x1c，桩内无死环。

### 判决：调度器层宏环——僵尸绿线程无限重派（高置信）

环路径：gadget（LR=桩入口）→ 出生桩 → bleq ipe → **pe=0 空返回** → 桩尾
（call_on_primordial_stack 系列 → svc 0x009f0001 绿线程退出 syscall）→ wrap 层
→ 调度器 → **再次派出同一线程**（链仍=出生态、pe 仍=NULL）→ 无限循环。

全部观测吻合：
- 15 轮同死点窗口（log36-53）：两次 dump 采样落在相邻指令（log52=stub+0x18、
  log53=ipe+0x8），环内其余时间在调度器 C 码/桩尾
- cpu1 忙而不换出、零解释器进展、内核 RUNNING 不进等待队列
- id5 反常：th_start=5 已出生、**有活 Java 栈**（jsp=0x81ee1bf0 多帧）、ring 显示
  近期 3 次正常切换——之后最终一次派发却走**出生链**（LR=桩入口）= 链回归出生态
- 死亡通道断裂恒证：**th_finish=0、lw_exit=0（15 轮全 0）**——绿线程死亡从未走完，
  死而不葬的线程留在可运行队列里被无限重派
- ring 末段 id3(13×)+id5(3×) 交替且全无解释器进展 = 双僵尸轮转，与
  "19 万次正常切换后才冻结"（活线程陆续死光只剩僵尸）自洽

### 层2悬案（v02.31 目标）

链为何回归出生态 / 僵尸为何不被移出队列：候选=退出 wrap 未接
Scheduler::thread_finish；出生链重派前未查 pe。**探针必须放 switch_thread
（C 层活代码）与退出 wrap 处**：记录每次派发目标 (id, chain[4], pe, status) 环，
HANG 时 WD 快照直接读出"谁在无限重派、目标是否僵尸"。

### 教训

- **bleq 判读**：objdump 助记符 `bleq` = BL+EQ 带链接，勿与 `beq` 混淆——微环
  假说死于一个 L 位
- **探针先证路径活着**：v230 三探针全 0 不是坏消息而是架构事实（master 模式
  trap 驱动切换）——放置前先在 dump/日志里证明该路径有流量
- **THREAD_INFO note 布局与 THREAD_REG_INFO 不同**（本脚本偏移不通用），
  内核状态直接用 watchdog 的 sceKernelGetThreadInfo 更可靠
- log53 dump 与 log52 结构同源（24 note/5 线程），但 slide=0x21000 再次印证
  **slide 绝不跨 dump 复用**


## v02.31：桩尾 C++ 体六探针 + exit-ring（97ee23b / b387，2026-10-01）

### 设计依据（承接 log53 判决）

log53 判决环在**调度器层宏环**（僵尸重派），且 HANG 期间 switch/yield/gnrt
计数器全冻结——若按原计划把探针埋进 `switch_thread` 将**一无所获**（冻结路径
无流量）。改为埋**出生桩尾 C++ 体**（`call_on_primordial_stack` 的派发目标，
即宏环每圈必经的 C++ 环节）：

| 探针 | 位置 | 语义（宏环每圈若有 C++ 体参与必 +1） |
|---|---|---|
| `vita_v231_finish` | `Thread::finish()` 入口 | 桩尾正常收尾体 |
| `vita_v231_force_term` | `Thread::force_terminated()` 入口 | 强杀体 |
| `vita_v231_setcur` | `Thread::set_current()` 入口 | 任何真实切换必经 |
| `vita_v231_term` | `Scheduler::terminate()` 入口 | 调度器终结路径 |
| `vita_v231_term_listed` | `terminate()` 尾部 raw 扫 global_threadlist 命中受害者仍在链 | 双 terminate / 僵尸重插 |
| exit-ring(8×4) | `lightweight_thread_exit()` 入口：id/status/pe!=NULL/chain(+0x64) | 死后链回归出生态实锤 |

### 宿主对象验证（防 Interpreter_arm.o 冻结类失效）

`force_terminated`/`thread_task_cleanup`/`_ZN9JVMThread6finishEv`/
`_ZN9JVMThread23lightweight_thread_exitEv`/`_ZN9JVMThread11set_currentEPS_`/
`append_pending_entry` 全在 `_MergedSrc004.o`；`Scheduler::yield`/
`Scheduler::terminate` 在 `_MergedSrc002.o`——**全是每轮 rebuild 重编的活对象**。
`invoke_pending_entries` 在冻结 `Interpreter_arm.o`（勿再在那埋点）。

### 编译修复：块作用域 extern "C" 非法

Thread.cpp 首次在函数体内写 `extern "C" void f(...)` 报
`expected unqualified-id`（C++ 标准禁止块作用域 linkage-specification）。
修复：声明移到**文件作用域**（`#if defined(VITA)` 包裹）。另一坑：C++ 文件内
`extern unsigned f(void)` 声明会 mangle、与 extern "C" 定义不匹配——统一
extern "C"。rebuild_vm.sh 的 `|| true` 会吞编译错误，务必 grep error: 自查。

### 验证链（全过）

- `strings libcldc_vm.a | grep -c "v231 finish"` → 1（格式串入库）
- `nm libcldc_vm.a | grep vita_v231` → 6 计数器 B + exit_ring/seq B +
  `vita_v231_exit_ring_rec` T + 2 个 U（Scheduler 侧引用闭合）
- 产物 `midp_vita_v02.31.sym`：版本串 `J2ME Player v02.31 b387 (97ee23b)`、
  8 个 vita_v231 符号链接入

### 判读指南（HANG 后两次 wd_snapshot 对比）

- 任一 v231 计数器仍涨 → 宏环确认**且点名自旋体**（涨者即环内活跃体）
- 全冻结而 cpu1 忙 → 环困在桩簇 asm 内（无 C++ 体参与，层2悬案指向 asm）
- `term_listed>0` → terminate 过的线程仍在全局链表（双 terminate 或重插僵尸）
- exit-ring 最后记录与 dispatch ring 死后仍派发同一 id → 死后重派实锤

### 遗留风险

exit-ring 记录在 `lw_exit` 入口——若环困在更早的 finish/force 阶段则 exit-ring
不动但对应计数器会动，判读时注意层次。`+0x64` chain 读取依赖 primordial gadget
栈链字偏移（log53 实测），若上游重构需同步。

### git

- 主仓 j2me `97ee23b`：Thread.cpp×4 + Scheduler.cpp×2 + OS_vita.cpp（定义+
  记录函数+快照输出）+ CMakeLists.txt（02.31 + 判读注释块）


## log54 判决：ipe 空返回 lr 链污染微环（v02.31 b387 首战，2026-10-01）

### 本轮实证（slide=0x1b000，self 锚点命中 vita_site_dump+0x0）

**死点（THREAD_REG_INFO 1 线程，VM 主线程 tid=0x40010003）**：
- PC=0x810dd2bc-0x1b000=**start_lightweight_thread_asm+0x18**（`bleq invoke_pending_entries`
  指令本身，与 log52 同点）；LR=**桩入口+0x0**（出生链保存值，gadget 签名）
- r0=0x81eddfb4（id4 绿线程对象，与 site_ring nrt 一致，jsp=0x81edef60 活栈）
- r1=0（status 无 TERMINATING 位）、cpsr=0x40070010（Z=1、User、中断开——与 log52/53 逐位同）
- r6=0x81edf01c（出生链块）、r12=0x81ede52c（=stack_limit，帧完好）
- 原生栈底：JVM::start+0x18c → JVM_Start+0xa4 → midp_run_midlet_with_args_cp+0x95c
  （三份 dump 同构）；栈上无任何 Scheduler/switch 层帧——绿线程不在 C 栈上运行

**v231 六计数器判读（核心新证据）**：
两次 wd_snapshot（间隔 400ms）：`finish=0 force=0 setcur=59→59 term=0 term_listed=0
exit_seq=0`——**全冻结且 setcur 非零**（=探针路径活，非 v230 死探针）。
按预设判读表第一分支命中：**环在桩簇 asm 内，无任何 C++ 体参与**。
log53 "调度器宏环"判决修正：环不在调度器 C 层，比想象的更小。

**watchdog 新形态**：忙 CPU 是 **cpu0**（log53 为 cpu1）；VM 线程报 cpu=2、
lastcpu=2，但 cpu0 idle/idleN/switchN 三 d=0 且 cpu2 idle 在涨——"VM 线程锚定
在忙 CPU"的旧锚定法失效（dump 采样与 sysinfo 采样不同时 + 调度漂移），
判读以 dump 寄存器为准。dispatch ring 末段 **id=3 ×16**（log53 为 id3+id5 混合，
本轮更纯）。ce=373/pump=373、ttk d=0 但 tick d=50（ticker 线程活着）。

### 环的精确路径（三份 dump 交叉闭合）

```
某处(WRAP 层) 调 gadget/call_on_primordial_stack 返回序列
  ldr lr,[r6,#4]          ← lr 装载出生链保存值 = 桩入口地址
  jmpx(lr)                → 跳进桩入口（LR=桩入口的观测来源）
桩头: get_thread; ldr r1,[r0+status]; tst TERMINATING(Z=1)
  bleq ipe                → 跳 ipe（log54 PC=bleq 本身、log53 PC=ipe+0x8）
ipe: ldr r0,[_current_thread]; ldr r1,[r0,#36](pe); cmp; pe=0
  moveq pc,lr             → lr=桩入口！回桩头（而非 stub+0x1c）
→ 微环闭合：桩头↔ipe，永不前进到桩尾
```

关键物证：**LR=桩入口而非 stub+0x1c**——bl 语义应写 lr=stub+0x1c，观测值
是链装载值。ipe 空返回 `moveq pc,lr` 用被污染的 lr 跳回桩头。环内无 C++ 体
（v231 全零）、无解释器进展（fast/site/jsmp 冻结）、cpu 忙而不换出。

### 判决

**ipe 空返回 lr 链污染微环**：出生链的 lr 槽（[r6+4]，setup_stack_asm 布局）
被 wrap/gadget 层装载后，ipe 的空返回路径 `moveq pc,lr` 把执行流送回桩头而非
bl 的正确返回点 stub+0x1c，形成桩头↔ipe 死环。环困住的是**最后一次派发**的
绿线程（本轮 id4，jsp 活栈），死因是**链回归出生态+lr 污染**，非"调度器无限
重派"（dispatch ring 冻结、无重派流量）。

### 下一步（v02.32 候选探针）

环内每次过 ipe pe 检查都是热点：在 ipe 空返回路径（moveq pc,lr）前埋计数器
可测环频率；但 ipe 在冻结 Interpreter_arm.o——**不可行**。替代：桩头是
_MergedSrc 活对象（start_lightweight_thread_asm 在 InterpreterStubs_arm.cpp，
需确认宿主对象是否冻结）。更优：**在 wrap 层出口埋点**（master 循环每圈必经
的 C 函数，如 Scheduler::allocate_or_recycle、Scheduler::step 之类活代码），
记录 wrap 每圈派发的 (id, chain, lr 装载值)——若 wrap 在转，环在 wrap↔桩头；
若 wrap 也冻结，则环是纯 asm 微环（桩头↔ipe 两地址间），修复方向为 ipe 返回
路径改 `moveq pc, lr_saved_from_bl` 或桩头自恢复（进桩先 push lr）。

### 教训

- **cpsr 偏移**：THREAD_REG_INFO 记录布局 tid@desc+0xC、16 寄存器@desc+0x10、
  **cpsr@desc+0x50**（PM 旧记载 +68 是记录相对，绝对偏移 0x50）
- **LR 观测值的两义性**：LR=桩入口既可能是"刚被 gadget 装载、BL 尚未覆盖"
  （环在桩头前），也可能是"ipe 空返回用它跳回"（环含 ipe）——需结合 PC 位置
  与计数器判读，单一 LR 不能定环
- **dump 与 sysinfo 不同时**：VM 线程 cpu=2 报告与 cpu0 忙的观测不矛盾
  （watchdog sysinfo 在 dump 之前采样），"VM 在忙 CPU 上自旋"必须用 dump 锚定
- **heredoc/终端内联 python 继续坑**：多行 python -c 被吞，一律先写 /tmp/*.py


### log54 判决修正（同日，排除法闭环后的最终版）

上面"ipe lr 链污染微环"理论**不成立**，自我否决：bleq 是 BL，执行必写
lr=stub+0x1c；ipe 空返回 `moveq pc,lr` 只能回桩尾；桩尾 3 条指令内必调
finish（v231_finish=0 证尾从未运行）。环闭不上。

**三条候选环全部被计数器排除**：
1. 含桩尾的环 → v231_finish/force 必计数 → 全 0，排除
2. 含 gadget 的环 → switch lane 必计数（v230 已证切换全经 gadget）→ 冻结，排除
3. 调度器 C++ 宏环（log53 判决）→ 六计数器全冻结 → 排除

**最终判决：VM 线程不在自旋，而是停在出生窗口不动（调度丢失/停驻态）**：
- runClocks d=0（200ms 零执行时钟）——自旋会烧时钟，停驻不会
- VM 报 cpu=2 而 cpu2 idle **在涨**（d=+199410µs、idleN +35）——cpu2 在变闲，
  VM 没在上面跑；cpu0 的"忙"是 watchdog 自己（wd 报 cpu=0，polls=12 轮询中）
- 内核态 RUNNING、wait=none、KICK 全无效（susp/res rc=0x80020008 非法状态）
  ——"RUNNING 却拿不到 CPU"的丢失线程形态
- 三份 dump PC 落在 bleq/ipe+0x8 两个相邻位置 = 停驻时的快照位置（非环内
  采样命中）；15 轮同窗口 = HANG 总发生在出生窗口（绿线程首次派发/退出重入），
  之后切换机构停摆，现场永远冻结在那里

**死因指向**：master 模式的绿线程切换机构（svc 0x9f0001 trap 处理链）在
HANG 时刻已死亡/卡住，VM 主线程被停在"半切换"状态（LR=出生链值=刚被
gadget 装载、尚未执行 bleq），此后再无任何调度事件发生。cpu0/1 上再无
任何 phoneME 线程在跑（六计数器+fast/site/jsmp+dispatch ring 全冻结）。

**v02.32 方向（改判后的新打法）**：目标从"打破环"改为"切换机构何时死的"：
- 桩/ipe/gadget 全在冻结 Interpreter_arm.o，环内埋点不可行（已证）
- 活对象探测点：midp-vita wrap 层（C 代码，audit "wrap active"）、
  OS_vita.cpp JVMSPI 层、Scheduler 活对象（_MergedSrc002）
- 关键问题：svc 0x9f0001 的处理者是谁（vita 侧无注册代码，OS_ads.cpp 仅有
  SWI 指令本体）——找到 trap 处理链才能知道它为何停摆

### 补充教训（log54）

- **"RUNNING+忙 CPU"必须用 runClocks+idle 斜率双重验证**：cpu0 忙≠VM 在跑
  （本轮忙的是 watchdog 自己）；自旋烧 runClocks，停驻 runClocks=0
- **排除法三连**：桩尾（finish）/gadget（switch）/调度器（六计数器）全排除后，
  "环"叙事本身就该放弃——剩下的是"停驻"
- **冻结对象清单扩大**：start_lightweight_thread_asm、invoke_pending_entries、
  call_on_primordial_stack 全在 Interpreter_arm.o（与 pe_enter 同宿主），
  出生窗口内不可能埋任何探针
- **THREAD_REG_INFO 全记录解码**：note 头{magic=0x11,count=5,recsz=376}，
  每记录 tid@0/sp@+56/lr@+60/pc@+64/cpsr@+68；plausibility 过滤会漏记录
  （本轮 5 条只显 1 条），全解码要自己遍历（/tmp/log54_allrec.py 模式）


### log54 判决终版（同日第二次修正，runClocks 归属表闭环）

上一节"cpu0 的忙是 watchdog 自己"**错误**，修正：wd 报 cpu=1，sysinfo 窗口内
wd 在 sleep 200ms（vita_watchdog.c:629 两采样间 sceKernelDelayThread(200*1000)），
cpu1 idle 恰涨 +199.6ms=wd 的睡眠；而 cpu0 全窗口 idle d=0/idleN d=0/switchN
d=0 = 某线程连续执行 200ms 从未切换、从未进 idle。

**runClocks 归属表（排除法）**：
- VM d=0（若自旋必积累 ~200ms）；wd d=1683µs（睡眠）；tone d=2331µs（delay
  节拍）；ticker delay 正常 → **cpu0 旋者不在任何被采样线程中**=未观测线程
- VM 报 cpu=2 但 cpu2 idle 涨满窗口 → VM 不在 cpu2 上跑过；cpu1/2/3 都有大
  量空闲而 VM（内核态 READY：RUNNING/wait=none）从未被派发 → **不是 CPU
  饥饿，是就绪队列链/TCB 损坏（丢失线程，lost-ready）**
- KICK5 susp/res rc=0x80020008（非法状态）与 log33/34 ghost-TCB 先例吻合：
  同类腐败。log53 重读：当时 cpu1 忙+VM 报 cpu=1 但 runClocks 同样 d=0——
  **log53 把旋者错归给 VM 线程**，"某线程在忙核上用户态自旋"成立、"VM 在
  自旋"不成立；log52 以来三 dump 的 PC 同窗口只是"VM 停驻点"恰好都在出生
  窗口（HANG 高发于绿线程出生/切换边界）

**最终判决（v3）**：VM 主线程在绿线程 id4 出生窗口中段（gadget 装载链后、
bleq 执行前后）**停驻**——内核可见 READY 但永不获派发（TCB/队列损坏），
runClocks 归零、全部 phoneME 计数器冻结皆因载体线程死了。同时另一未观测
线程在某核上无限自旋（idle/switch 双冻结）。两者可能是同一次内存/TCB 损坏
的两个症状（如已释放线程仍挂队列：旋者=僵尸，VM=被挤掉链的受害者）。

### v02.32 探针方案（最终版）：watchdog 线程普查

目标：**点名 cpu0 旋者 + 找 ghost tid**。全部在 vita-port C 活代码（无冻结
对象风险）：
- HANG 时 `sceKernelGetThreadIdList` 枚举进程全部线程 → 逐 tid
  `sceKernelGetThreadInfo`（name/status/cpu/runClocks），200ms 双采样
- 旋者=runClocks d≈200ms 的 tid，名字直接读出；census 与 AUDIT 三注册表
  差集=未观测线程清单（ANI 池/音频/输入/ghost）
- 安全纪律（log34 教训）：**每个 tid 先 log 再查**——若 wd 在某 tid 上死于
  内核数据 abort，最后 log 的 tid 就是腐败 TCB（天然二分标记）；
  sceKernelGetThreadRunStatus 永久禁用（log33/34 已证杀死调用者）


## v02.32：线程普查探针（159e458 / b391，2026-10-02）

### 实施前的补充取证（本轮新物证，改变问题性质）

**THREAD_INFO note（type 0x1003）破译**：布局为 12B 头{magic=0x12,count=5,
recsz=0xC8} + 每记录 200B（tid@+0、name@+0xF 起 32B、attr/cpu 等在后）。
全进程仅 **5 个线程**：J2ME00001 / **SceCommonDialogWorker** / j2me_tone /
j2me_watchdog / cldc_ticker。

**SceCommonDialogWorker 双 dump 逐位冻结**：log53 与 log54 两次独立 HANG，
其 PC/LR/SP 完全相同（0xe0009304 / 0xe000d957 / 0xd0010fe8）。~~PC 落在
Vita3K HLE stub 区间（0xe0000000+）——它在某个 HLE syscall 里从未返回。~~
（**勘误 2026-10-02**：真机 dump。pc=0xe0009304 不落在任何 PT_LOAD——
最低系统段 0xe0010000 起 → 它在**内核保留区 = 阻塞在内核调用里**，两轮
同 pc 只说明等在同一种系统服务上，**正常等待形态，不是卡死**。端口未
链接/未调用 SceCommonDialog，该线程为系统注入。）它是系统线程，
**从不在我们的观测名单**（AUDIT 表只注册 tone/wd/ticker）。

### log54 归属表最终形态

- cpu0 的 200ms 零切换占用者 ∈ {SceCommonDialogWorker, 未知}
- VM 主线程 d=0 停驻（非旋者）；wd/tone/ticker 各有不在场证明
- SceCommonDialogWorker 无 runClocks 采样（census 前无法排除），但其
  dump 形态是内核阻塞等待，不构成指控

### 探针实现（vita_watchdog.c，KICK 块后）

- `sceKernelGetThreadIdList` **不在用户态 stub**（已查 libSceKernelThreadMgr_
  stub.a，174 符号无枚举类）→ 改 **uid 区间扫描** 0x40010001..0x400101ff
  （覆盖全部已知 tid；洞返回错误直接跳过，无害）
- 每个 tid **先 log 再查**（log33/34 教训：若 GetThreadInfo 死在内核，
  最后 log 的 tid 就是腐败 TCB——天然二分标记）
- 逐线程输出 name/status/cpu/lastcpu/aff/prio/runClocks，200ms 后二采样
  输出 d(runClocks)
- 判读一行定案：**d(runClocks)≈窗口时长的 tid = cpu0 旋者**；VM d=0 再证
  停驻；census 名单 vs AUDIT 差集 = 系统注入/ghost 线程清单

### 环境勘误（2026-10-02，用户指正）

**测试环境自 log50 起一直是真机**（PM 6443/6744/6987 行均有记录：psp2dmp
均为真机 core dump）。本节初版把 SceCommonDialogWorker 误判为 "Vita3K
HLE 工作线程"、把 pc=0xe0009304 误读为 "HLE stub 区间"——模拟器思维套
真机，双重错误，已全部修正：

- 端口未链接/未调用 SceCommonDialog；该线程是真机系统注入的
- pc=0xe0009304 **不落在任何 PT_LOAD**（最低系统段 0xe0010000 起）→
  内核保留区 = **阻塞在内核调用里**，两轮同 pc 只说明它等在同一种
  系统服务上——**正常等待形态，不是卡死**，不构成 HANG 原因
- "SceCommonDialogWorker 是唯一无不在场证明的线程"这一表述保留，但
  其内核阻塞形态不构成对其的指控
- "换 Vita3K 版本/转真机" 等下一步全部作废

**教训入纪律**：涉及 dump 判读，先核对测试环境（真机 vs Vita3K），
再解读地址区间；0xe0000000 段在真机上是内核/系统保留区，不是模拟器
HLE stub 区。

### 判读分流（这次没有"猜测"出口）

| census 结果 | 结论 | 下一步 |
|---|---|---|
| SceCommonDialogWorker d≈200ms | 系统线程在真机上烧 cpu0 | 查它等的内核服务为何不返回（ sceAppUtil/sceCommonDialog 类） |
| 其它 tid d≈200ms | 点名具体线程 | 针对性查它 |
| 全部 d=0 但 cpu0 仍忙 | 旋者非本进程线程（系统/中断上下文） | 查中断/内核侧（DmacpIntr 等） |
| VM tid 查询即死 wd | 最后 log 的 tid = 腐败 TCB | 修 TCB 源头 |

### git

- 主仓 j2me `159e458`：vita_watchdog.c census 块（+96 行）+ CMakeLists 02.32
  （含完整判读注释）
- 产物 `vita-port/out/vpk/midp_vita_v02.32.vpk/.sym`（b391）
- **勘误 commit（本轮）**：vita_watchdog.c / CMakeLists / PM 三处 Vita3K
  HLE 错误归因已改为真机表述；产物待重建（字符串注释级改动，不影响二进制行为）


## log55 判决：census 首战——进程内全员洗脱，旋者=VM 自己的内核上下文（6ace1d5/b394，2026-10-02）

### census 五行判决表（200ms 双采样）

| 线程 | d(runClocks) | 判决 |
|---|---|---|
| J2ME00001 (VM) | **d=0** | 停驻再确认（三重窗口一致） |
| SceCommonDialogWorker | **d=0** | **洗脱**——上轮怀疑被证伪；且本轮 dump pc=0xe000cd64 ≠ 前两轮 0xe0009304（在内核里正常走动） |
| j2me_tone | d=26484 (~60µs) | delay 唤醒小活，正常 |
| j2me_watchdog | d=844708 (~1.9ms) | census 自身开销（扫 511 uid + 日志 I/O），正常 |
| cldc_ticker | d=4018 (~9µs) | 周期小活，正常 |

**判读表落在第 3 行：旋者不属于任何用户线程。**

### 关键交叉证据（slide=0x2f000，锚点 self=0x81087fd0）

1. **VM 冻结点**：pc=`invoke_pending_entries+0x4`，lr=`start_lightweight_thread_asm+0`
   （primordial_to_current_thread gadget 新生签名，同 log52/53 的出生窗口）；
   tone/ticker=`sceKernelDelayThread+0x8`（正常）；wd=自身例程
2. **syscall trace**：VM 最后 syscall io_pwrite **已返回**——不是卡在 syscall 入口
3. **三轮忙核 vs VM 声称核**：log53 cpu1=忙=VM cpu1 ✓、log55 cpu1=忙=VM cpu1 ✓、
   log54 cpu0=忙 vs VM cpu2（唯一失配，字段可能陈旧）
4. 本轮 sysinfo 采样窗（含 wd 200ms sleep）：cpu1 idle d=0 全窗黑暗，
   cpu0/2/3 分别 +198/+202/+177ms——**唯独 VM 的核忙**

### 终版因果链（不再是猜测，是排除法闭合）

VM 线程在出生窗口（ipe 头部）**经中断离开用户态后再未返回**：

- status=RUNNING/wait=none（离开时状态留存）
- runClocks 冻结（内核态不记 tick 账）
- 用户态 PC 停在正常指令上（中断打在哪就存哪：+0x4/+0x8/+0x18 三轮不同=时钟中断时机差异）
- 该核 idle d=0 + switchN d=0（内核态关抢占自旋，不再调度任何线程）
- KICK5 susp/res **0x80020008**（ILLEGAL_PERMISSION=线程处于不可挂起的内核过渡态）
- v231 全计数器 0（用户代码从未恢复）

**旋者不是"未观测线程"，就是 VM 线程自己的内核上下文**——用户态探针的观测下限
到此为止。TCB 损坏或内核调度器在该线程上楔死（log33/34 ghost 同族）。

### 下一步（v02.33 方向）：内核侧取证

kubridge 已在场（kernel_forensics: "kubridge: present"）但 v02.18 的
SceKernelThreadmgr 解析失败（"threadmgr module not found - no kernel read"）。
修它：kubridge 模块枚举定位 threadmgr 基址 → HANG 时直接读 VM tid 的内核 TCB，
看它卡在哪个内核等待点/哪把锁上。这是用户态→内核态唯一的观测通道。

### 教训

- census 判读表四出口全部先写后验，本轮落在第 3 行且与预期一致——**先写判读表再跑实验**的纪律有效
- runClocks 记账模型：内核态关抢占不记账——"RUNNING+d=0+核忙"三元组=内核态楔死的签名
- 0x80020008 族错误码要当作"线程处于内核过渡态"的信号读，不只是"权限不足"


## v02.33：内核 TCB 取证——K0 签名扫描（c6bfaf2 / b395，2026-10-02）

### 问题：threadmgr 基址两条路都是死路

- 用户态 `sceKernelGetModuleList` **结构性看不见内核模块**（v02.21 判决：enum rc=0
  nmod=0；flag 0x81 列的全是用户态模块）
- taiHEN 跨域查询（module_get_by_name_nid KERNEL_PID）在本机**永不返回**——
  log48 杀 wd、log49 杀主线程（boot 即崩），v02.26 已彻底退役
- 但 kubridge memcpy 本身**可用**：clock=190（log46/47 判决），且 log48 证明
  HANG 态下 ku 读**能返回**

### 方案：内容寻址，跳过模块基址

- **boot 时**（主线程，健康系统）：kubridge 读整个 K0 直映区
  0x81000000..0x84000000（全部物理 RAM，无洞，读不会 abort），32KB 分块扫
  字符串 `"J2ME00001"`（主线程名，SceAppMgr 进程出生即设，早于扫描执行）
- **tid 自校验**：命中点 ±0x100 内必须能找到本线程自己的 tid
  （`sceKernelGetThreadId()`）——TCB 里 tid 与 name 相邻，假阳性（日志缓冲、
  dump 文件缓存里的同名串）不会有 tid 呼应
- **HANG 时**：`vita_kf_dump` 直接 kf_read 缓存的 TCB 地址，dump 名字±0x80
  的 0x100 窗口，**双采样 400ms**——冻结的 TCB 本身就是"调度器不再碰这个
  线程"的证据；差分还能看到内核侧字段是否在动
- 离线合并：TCB hex 与 coredump THREAD_REG_INFO（内核保存的 pc/lr/sp）、
  runClocks 已知值对齐 → 命名 VM 线程楔死的内核等待点

### 安全边界（不可遗忘）

- **只扫 K0 直映区**；vmalloc 窗口（0x88800000+）有未映射洞，读洞=内核态
  abort 不可捕获=真机变砖风险，永远不扫
- kf_read 的 plausibility 门（两窗口校验）对所有读取仍然生效
- 扫描只在 boot 跑（健康态），HANG 路径零枚举零 taiHEN——log48/49 纪律

### 判读指南（下轮 HANG 的 kernel_forensics.log）

| 输出 | 含义 |
|---|---|
| `VM TCB LOCKED @ 0x...` + TCB hex | 扫描成功，HANG 时同地址双采样 |
| TCB 两次采样逐位相同 | 调度器完全不再触碰 VM 线程（lost-ready 实锤） |
| TCB 有字段在动 | 调度器还在摸它（等特定字段差分定位） |
| `boot scan found none` | TCB 不在 K0（vmalloc 分配）——考虑 NID 直查或放弃 kubridge 路线 |
| scan hit 无 tid 命中 | 命中的是日志/文件缓存里的假串，正常噪音 |

### git

- 主仓 j2me `c6bfaf2`：vita_kernel_forensics.c（+175 行：扫描器+TCB dump+
  varargs 日志）、CMakeLists 02.33
- 产物 `vita-port/out/vpk/midp_vita_v02.33.vpk/.sym`（b395）


## v02.33 事故：K0 扫描致真机重启（v02.34 止血，99bdd3a / b397，2026-10-02）

### 事故

v02.33 首次真机运行即**开机重启**。触发点：`vita_kf_boot_resolve` 里的
`kf_scan_k0`——boot 时对 0x81000000..0x84000000 的 48MB 批量
`kuKernelCpuUnrestrictedMemcpy`。

### 根因（自我勘误，两层错误）

1. **窗口前提错误**：我沿用 v02.18 注释"K0 直映区=全部物理 RAM 无洞"，
   但 log55 的数据早就否定了它——我们进程自己就映射在该窗口
   （site_dump 运行时 0x81087fd0、heap_top 0x8389d580、PT_LOAD 0x813xxxxx）。
   用户态窗口 ≠ 内核窗口：内核在自己的地址空间里执行 ku memcpy，撞到
   内核 ASID 下未映射的页 → 内核态 abort 不可捕获 → 重启
2. **经验来源错误**： VitaTorchlight 用的是内核插件直接跑在内核态，
   从未从用户态批量读该窗口。"社区证明可行"是我套用不当

### 止血与纪律

- v02.34：扫描调用注释禁用，其余探针不动；v02.33 产物作废
- **新纪律（入最高优先级教训）**：任何内核窗口读取，必须先用**单词探测读**
  （单次 4 字节 ku read，结果落盘，真机观察）证明窗口安全，再谈批量读。
  未经证明的窗口批量读 = 故意重启真机
- 事后审计：v02.18~v02.32 的 kf_read 内核读**从未真正执行过**（threadmgr
  解析一直失败早退）——所以这个地雷埋了 15 个版本才被 v02.33 踩响

### v02.33 的观察目标怎么办（kernel TCB 取证路线重评）

- K0 扫描路线：**废弃**。没有安全的窗口证明手段（探测读也会踩雷，见上）
- 替代优先级：
  1. **coredump 离线路线**（零风险）：psp2dmp 的 SYSTEM_INFO2 是 4MB
     hashed 数据，但 THREAD_REG_INFO 里已有内核保存的 pc/lr/sp——本轮
     已用锚点法全部解开。TCB 内核字段可尝试从 dump 的其它 note 挖
  2. **NID 直查**：离线从固件 dump（若用户有）解析 SceKernelThreadmgr 基址
  3. 若都不行：接受"内核侧不可观测"，回到行为学取证（变异法：改条件看
     HANG 率变化）


---

## log55/53/54 coredump 离线全字段挖掘——出生窗口铁案（v02.34 后，零风险路线）

### THREAD_REG_INFO 记录结构完全破译

376B 记录布局（此前只解了 72B）：
`+000=tid`、**`+004..+034 = r0..r12 完整寄存器堆`**、`+038=sp`、`+03c=lr`、`+040=pc`、`+044=cpsr`、`+048`=fpscr、`+060`起 ASCII 时间戳、`+090..+0cf`=VFP d 寄存器、`+150`=μs 时间戳对。剩余 0x7f80dead/0x7ff8dead 是内核未初始化标记。

### 三轮 dump VM 线程冻结点对比（铁案）

| | log53 | log54 | log55 |
|---|---|---|---|
| 构建 | v02.31 | v02.31 | v02.32 |
| slide | 0x35000 | 0x1b000 | 0x2f000 |
| fp(r11) | **0** | **0** | **0** |
| r3 | 0x81ee0d5c=nrt obj **id=5** | 0x81eddfb4=nrt obj **id=4** | 0x81eddfb4=nrt obj **id=4** |
| r0 | =r3（已解引用） | =r3 | GOT 槽 &\_current_thread |
| pc | ipe 头部窗口 | invoke_pending_entries+0x20 | **ipe+0x4（ldr r0,[r0] 第一句）** |
| lr | 蹦床内 | ipe 附近 | **start_lightweight_thread_asm+0** |
| cpu(w33) | 1 | **2** | 1 |
| nrt pe | 0 | 0 | 0 |

三个独立启动轮次，冻结点全部落在**绿色线程出生路径**：`switch_thread → 弹引导帧[fp=0, lr=蹦床] → 蹦床前几条 → ipe 头部 ≤10 条指令窗口`。

### 逐项闭合证据

1. **fp=0**：`setup_stack_asm`（InterpreterRuntime_arm.cpp:112）压引导帧最后一项 `*xsp=0`——新生儿标记，三轮全中
2. **lr=蹦床+0**：蹦床调 ipe 用 `bleq`（branch 不写 lr）——lr 仍是引导帧弹出的原始值，**从出生起没有任何 bl 执行过**
3. **r0=r5+0x40c 精确命中**：caller `810c25b4: ldr r0,[r5,#1036]` 刚加载的 GOT 槽（r5=gp=0x8140191c，静态 0x813d291c+slide 自洽）
4. **r3=nrt obj**：与 site_ring 的 `nrt obj` 同一对象同轮快照（log55 双验 0x81eddfb4）
5. **pc=ipe+0x4**：IRQ 保存惯例 actual+4，第一句还没执行完
6. **新生儿 pe=0**：ipe 对空 pe 是 4 条指令空操作——窗口极窄却三轮全中，非随机
7. **出生计数**：th_start=4 th_finish=0 justborn=0；dispatch ring 全 id=3（主线程连续让步，新线程从未上环=从未跑起来）；ttk_stage=7 hits 全 350（tick 链健康）
8. **栈三源互锁**：STACK_INFO(current=0x1278) = 栈顶 0x81381000 - sp 0x8137fd88；THREAD_INFO w15/16 给出栈基 0x81341000+0x40000

### cpsr 细读（修正此前"I=1"误读）

VM `0x40070010` = USER + **I=0（IRQ 开）** + Z=1 + GE=7（SIMD 残留）。五线程全 USER 态保存（wd 的 0x200f0030=USER+Thumb）。VM 是在纯用户态执行 ipe 时**被中断捕获**，现场停格——与 census"内核态楔死"判决拼合：IRQ 在出生窗口打断 VM → 内核处理中关抢占自旋 → 永不返回。

### 其它 note 判读（无进一步价值）

- EVENT_LOG(0x102c)=shell 应用启动历史（NPXS19999/J2ME00001/TOOLBOX20 + μs 时间戳 0~344s），非调度器日志
- TTY/TTY_INFO2 = 全 0/dead 标记；SYSTEM_INFO2 = 4MB hashed 数据（无法字段化）
- THREAD_INFO 布局：w0=tid、w1..w7=name、w15/16=栈基+大小、w33=最后所在 cpu、w38=pc；MODULE_INFO 可给 0xe0 模块命名

### log54 cpu 悬案闭合

w33 字段：VM log54 真在 **cpu2**（census sysinfo 读 cpu0 忙是失真/过渡态）。三轮 1/2/1 = **VM 线程跨核迁移**，忙核跟随 VM。其余四线程恒 cpu0。

### 判决

冻结不是 VM 代码 bug：出生路径全部是已映射用户态代码、无 syscall、ipe 是空操作。是**出生时序制造了内核竞态条件**（首次 dispatch 新线程的瞬间 IRQ 落在切换关键窗口），内核调度/中断返回路径楔死。187663 次常规切换无事、4 次出生 3 次捕获冻结——事件强绑定"首次 dispatch"。

### v02.35 方案候选（按推荐序）

1. **钉核**：VM pthread 创建后 `sceKernelChangeThreadCpuAffinityMask(0, 1)`（API 见 threadmgr.h:365，已确认存在）。假说：竞态与跨核迁移有关；一行改动、可归因（HANG 消失=迁移致因；仍 HANG=排除）
2. 出生 dispatch 前让步/延迟：避开 tick 边界，改动中等
3. tick 降频：间接减少打断密度，归因力弱


---

## 【判例表】cpu 观测数据源与三轮终值（2026-10-02，永不重推导）

**病根记录**：此前 cpu 结论反复，一是把 census 的"忙核"（sysinfo 采样，回答"哪个核忙"）误当"VM 所在核"用；二是 THREAD_INFO 记录实际 196B/条、按 200B 对齐导致 cpu 列取错（w32/w34 都取到过 attr 值 0x80028028）。已用 pc 锚定法（用已知 pc 值反推列号）定死 **w33 = 最后运行核**。以下为终值，引用即可，禁止重推导。

### 数据源语义表（先分清问的是什么，再谈数值）

| 数据源 | 回答的问题 | 可靠性 |
|---|---|---|
| census sysinfo "忙核" | wd 睡眠窗内哪个核忙（采样） | **会失真**（log54 读 cpu0，实际 VM 在 cpu2） |
| THREAD_INFO w33 | 内核记录的线程最后运行核（dump 时刻） | **权威** |
| THREAD_REG_INFO pc/cpsr | 用户态停格现场 | 权威 |

### 三轮终值（log53/54/55，全部 5 线程）

| 线程 | log53 | log54 | log55 | 结论 |
|---|---|---|---|---|
| J2ME00001 (VM) | cpu1 | **cpu2** | cpu1 | **唯一迁移者** |
| SceCommonDialogW | cpu0 | cpu0 | cpu0 | 恒 0 |
| j2me_tone | cpu0 | cpu0 | cpu0 | 恒 0 |
| j2me_watchdog | cpu0 | cpu0 | cpu0 | 恒 0 |
| cldc_ticker | cpu0 | cpu0 | cpu0 | 恒 0 |

规律：睡眠型线程唤醒全落 cpu0（定时器落点）；VM 唯一满载被负载均衡推到 1/2。**"忙核"一直跟着 VM 走，从未乱跳**；census log54 的 cpu0 读数是采样失真。

### cpu 与 HANG 的关系定位（诚实版）

目前**只有相关没有因果**：VM 唯一迁移 + 冻结绑定出生窗口。~~钉核实验（v02.35 候选）~~ **已判死勿重跑**：log37(v02.12 钉 cpu0)/log39/40(v02.14/15 VM+ticker 同钉 cpu1)三轮都仍卡死——跨核迁移假说早已被推翻，v02.35 方案因此刹车改道（见下节）。

---

## 2026-10-02 v02.35 前最后判读 + birth-delay A/B（phoneme-cldc b454236 / vita-port bc8aeaa / b403）

### 1. 四矿关闭（本节判读，勿重挖）

1. **PIPE `SceNetAdhocAuthMbx` = 背景噪音**：subtype=**0x10c**（SceShellSvc 管道是 0x83，结构不同不可横比）。它是 `vita_net.c:151` `sceNetInit` 内部创建的 AdhocAuth mailbox，创建者=调用线程=主线程=40010003(VM)。`+72` 是 **creator 字段**不是等待者列表。三轮同现="每次启动主线程都初始化网络"的正常背景。
2. **`setcur=97` 判读 + 记忆修正**：`Thread::set_current`（`Thread.cpp:356`，v02.31 插桩）是绿线程切换唯一提交点（switch_thread 与 lightweight_thread_exit 都汇入）。97=冻结前切换总数，冻结后静止。**⚠ 修正：此前"18 万次切换"是把 site ring `seq=219277` 误当切换数。真实切换仅 97 次——冻结恰落在"第一次 dispatch 给新生儿"那一次。**
3. **第 10 证 `swjb_seen=0`**：v02.30 旁路计数（统计"带 JUST_BORN 位的线程被切出"）三轮全 0 ⟹ **从未有一个新生儿完成过首切片后被切出**，与 dispatch ring 全 id=3 互锁：冻结=第一个新生儿的第一次 dispatch。
4. **THREAD_INFO w11 疑似 state（VM=1, tone=0x80000008）但无权威依据**：本 SDK 无 `SceKernelThreadInfo` 结构定义（include 里只有 vita-import/export 头，无 psp2/ 目录），按"不猜字段"纪律关矿。

### 2. 实验空间终验（哪些真没做过）

| 实验 | 状态 |
|---|---|
| 钉核（vm_cpu/ticker 同核） | log37/39/40 **已判死**（钉核仍卡） |
| 内核窗口读取 | v02.33 **致真机重启**，判死（单字探针未做前禁止再碰） |
| coredump 离线挖矿 | 已榨干（10 证闭合） |
| **tickgate/ticker_cpu 行为学开关** | **从未跑过**——log46~55 十轮全部 `cpu_cfg=-1 tickgate=0` 默认态（终验：逐轮 grep site_ring.log） |
| **出生 dispatch 前延迟** | 从未做过 → v02.35 |

### 3. v02.35 改动（最小 diff，3 文件）

- `phoneme-cldc/src/vm/share/runtime/Scheduler.cpp`：`switch_thread` 的 JUST_BORN 分支（`_vita_swjb_seen` 旁路同位置）加运行期门控 `sceKernelDelayThread(dms*1000)`。开关文件 `ux0:/data/J2ME00001/birth_delay`，首字节数字=毫秒（'1'=1ms），缺席/0=与 v02.34 完全一致。
- `phoneme-cldc/src/vm/os/vita/OS_vita.cpp`：wd `boot` 读段加 `birth_delay=%d`，每份 dump 自文档化当轮 A/B 配置。
- `vita-port/CMakeLists.txt`：版本 02.35 + rationale。
- 构建：`rebuild_vm.sh` 全代重编（33 对象/29 成员/守卫全过）→ `vita-port/build.sh` → 产物 `out/vpk/midp_vita_v02.35.vpk`（3681178 B，`J2ME Player v02.35 b403 (bc8aeaa)`，sym 内含 birth_delay 符号×2）。

### 4. 真机跑法（零代码轮次，与 v02.35 一包两用）

**轮 1（对照组，先跑）**：不放置任何开关文件 → 应复现冻结（`NEITHER` 签名）+ dump 里 `birth_delay=0`。确认 v02.35 新代码没有改变复现。

**轮 2（实验组）**：`ux0:/data/J2ME00001/birth_delay` 写 `1` → 同路径复跑。
- **HANG 消失**（菜单能进、游戏能玩 >5min）⇒ **时序竞态实证**：出生 dispatch 撞上 tick/IRQ 窗口是因果链最后一环，下一步做永久修复（出生路径 yield 而非 sleep / 关键区屏蔽）。
- **仍 HANG** ⇒ 排除"IRQ 撞切换窗口"，转向轮 3。

**轮 3（备选，零代码）**：删 `birth_delay`，放 `ticker_cpu.2`（ticker 挪 cpu2）→ 判 ticker 同核 tick 注入。仍卡再试 `tickgate.2`。

判读锚点：`site_ring.log` 的 `birth_delay=` 行自证当轮配置；冻结判据同 log36-55（`vm runClocks d=0` + `KICK no effect` + `swjb_seen` 若仍 0 则冻结仍在首 dispatch）。

---

## 2026-10-02 log56/57/58 判决 + v02.36 修复死门控（v02.36 b407 cac5a7a）

### 1. 三轮结果

| 轮 | 配置（site_ring 自证） | 结果 |
|---|---|---|
| log56 | `birth_delay=0` 对照 | 冻结（dump 只剩 .tmp，site_ring 完整）→ v02.35 新代码零扰动 ✓ |
| log57 | `birth_delay=1` | 冻结，`swjb_seen=0`，pc=**stub+0x18** |
| log58 | `birth_delay=1`+`ticker_cpu=2` 生效 | 冻结，ticker 活跃钉 cpu2（loops 4461→4518 跳动），VM 冻 cpu1，pc=**stub+0** |

### 2. 坏消息（判读时主动刹车发现）：v02.35 门控是死的，log57"delay 无效"判决作废

`THREAD_JUST_BORN` **只在 `_debugger_active` 时置位**（`Scheduler.cpp` start() 尾 `if (_debugger_active) { set_status(THREAD_JUST_BORN); }`），本构建无 debugger。v02.30 注记（OS_vita.cpp "swjb_seen…compiled dead"）早已写明——**改 v02.35 时读过此注却没把死条件应用到自己的门控**。三轮 `swjb_seen=0` 是铁证：delay 一次都没执行。教训入纪律：**复用任何状态位做门控前，先 grep 它的置位点确认本构建路径可达**。

### 3. 两个有效判决（不依赖死门控）

1. **ticker 同核 tick 注入假说判死**：log58 ticker 真实分离 cpu2 且活跃，VM 仍冻 cpu1 出生窗口——与 log37/39/40 钉核判决独立闭合。
2. **时序竞态指纹增强**（同一 v02.35 二进制内）：
   - 死点游走：log57 pc=stub+0x18、log58 pc=stub+0（lr=stub+0 锚定法；对比 log53/54/55 三轮恒 ipe+4）
   - `setcur` 终值波动：126/100/104（log53/54/55 恒 97）
   - 确定性死锁死点应钉死——**同二进制内漂移 = 竞态指纹**

### 4. v02.36 改动（phoneme-cldc 67ba518 / vita-port cac5a7a）

- `Scheduler.cpp` 文件级 `static int vita_last_started_id = -1`（同 `_vita_swjb_seen` 先例位置）：`Scheduler::start()` 尾记录 `thread->id()`（int，GC 免疫）
- `switch_thread()` **门控移到 `next_thread` reload 之后、实际切换之前**（旧位置在 reload 前会错过空缓存的首 dispatch）：`next_thread->not_null() && next_thread->id() == vita_last_started_id` → `sceKernelDelayThread(dms*1000)`，然后锚清 -1（只在首 dispatch 触发一次）
- 类型走查记录：`_next_runnable_thread` 是 `OopDesc*`（不可直接 `->id()`），`next_thread` 是 `Thread*`（372 行 `thread->id()` 先例）——门控用后者
- 版本 02.36；产物 `out/vpk/midp_vita_v02.36.vpk`（3681189 B，sym `v02.36 b407 (cac5a7a)`，birth_delay 符号 ×2）
- 跑法不变：对照（无文件）+ `birth_delay` 写 `1`。**判读新增自证项：v02.36 起 delay 若执行，site_ring 的 setcur 前后应有 1ms 级间隔且 swjb_seen 首次可能 >0（首 dispatch 完成后被切出才计）——若 `birth_delay=1` 仍冻结且 swjb_seen 仍 0，先怀疑门控再怀疑假说**

## 2026-10-02 log59/60 判决 + v02.37 修复 MergedSrc 构建缺陷（v02.37 sym 812e1878，cldc cdee4e0 / vita-port 56a4b1e）

### 1. 两轮结果与判决作废

| 轮 | 配置（site_ring 自证） | 死点符号化（slide 锚定法） | 判决 |
|---|---|---|---|
| log59 | 对照 `birth_delay=0`，setcur=102 | pc=**ipe+0x4**（810c22e0） | 对照干净：回归 log53/54/55 经典位，v02.36 声称的改动零扰动 ✓（但见下） |
| log60 | `birth_delay=1`，setcur=87 | pc=**stub+0x18**（810c25cc） | 表面"仍冻结"——**实验无效，作废** |

### 2. 第三次工具性作废：v02.36 门控代码从未编译进包（MergedSrc 依赖断裂）

判读纪律"仍冻结先查门控再怀疑假说"再次起效。三条证据链：

1. **`.sym` 无门控符号**：v02.36 sym 全查无 `vita_last_started_id`（只有 `_ZL15_vita_swjb_seen`）
2. **库内无符号**：`nm libcldc_vm.a | grep vita_last_started_id` 空（仅 `vita_sched_justborn`）
3. **`.o` 时间戳铁证**：`_MergedSrc002.o` = **Oct 1 11:33**，早于 v02.35（Oct 2 11:30 改）与 v02.36（Oct 2 13:50 改）两次 Scheduler.cpp 修改

**依赖断裂机制**（PM 已知坑"make 单对象重编要先 rm 旧 .o"的 MergedSrc 变体）：
```
_MergedSrc002.cpp (76B 壳文件, Aug 29)  ← make 只盯它的时间戳
  └─ #include "__MergedSrc002.cpp.incl" (220 个 include)
       └─ #213: #include ".../Scheduler.cpp"  ← 绝对路径，无依赖追踪
```
改 Scheduler.cpp 永远不触发 `_MergedSrc002.o` 重编。`rebuild_vm.sh` 报 "33 objects OK" 只清点 .o 数量，**不验证内容**。v02.35 的 log57/58 是死门控（代码在包但条件永假），v02.36 的 log59/60 是无门控（代码根本不在包）——**三次 delay 实验（57/58/60）全部工具性作废**，假说本身仍未被真正检验。

### 3. v02.37 修复（cldc cdee4e0 / vita-port 56a4b1e）

1. `rm target/release/_MergedSrc002.o` → `rebuild_vm.sh` 全量重编（32→33 objects，29 成员 repack）
2. **编译错误顺带修复**：merged-src 单元无系统头可 include，函数内 `extern "C"` 非法（C++ linkage-spec 只能文件级）→ 文件级 `extern "C" int sceKernelDelayThread(unsigned int usec);` 加在 `vita_last_started_id` 声明旁
3. **符号级验证闭环**（新纪律）：`nm libcldc_vm.a` 命中 `_ZL20vita_last_started_id`（.a 内 00000008 d）+ `nm v02.37.sym` 命中 `_ZL20vita_last_started_id @ 812e1878` + `_MergedSrc002.o` 时间戳 Oct 2 15:50（本次构建）
4. 版本 02.37（避免真机测错版本，log25 教训）；产物 `out/vpk/midp_vita_v02.37.vpk`（3681618 B，15:53）

### 4. 新纪律（入判读防呆项）

- **门控实验构建后必须符号级验证**：`nm --defined-only` 查门控变量/函数符号必须在 `.a` 与 `.sym` 双命中，"objects OK"清点不算数
- **MergedSrc 单元改源后必须 rm 对应 `_MergedSrcNNN.o`**：依赖链 `.cpp壳→.incl→绝对路径源文件` 断裂，touch 源文件无效；改哪个 vm/share 源就 rm 哪组（Scheduler.cpp=_MergedSrc002）
- 顺带发现的既成事实：v02.35→v02.36 间 `_MergedSrc004.o`（11:55）编过一次——那是 OS_vita.cpp 走 **独立 .o**（非 MergedSrc）路径，log59/60 site_ring 能打印 birth_delay 证明该路径正常

### 5. 待跑（v02.37 真门控首轮真机）

- 对照轮（无 birth_delay 文件）：预期复现冻结、pc 在出生簇——验证 v02.37 门控在包内但未激活时零扰动
- delay 轮（birth_delay=1）：**首次真实检验** delay 假说。判读自证链：sym 已含门控符号（前提成立）→ site_ring birth_delay=1（开关读到）→ 死点仍在出生簇+swjb/setcur 变化（门控执行）→ HANG 是否消失分流：消失=竞态坐实转永久修复设计；仍在=delay 1ms 不够或位置不对，升 5/10ms 重试

## 2026-10-02 log61/62/63/64 判决：三剂量终验——birth_delay 1/5/10ms 全冻结，术前延迟假说判死（v02.37 b410）

### 1. v02.37 构建修复验证成功（方法论闭环）

- log61（对照 birth_delay=0）与 log62（birth_delay=1）midp_stderr 均自证 `v02.37 b410 (56a4b1e)`——测错版本风险已防住
- **门控执行铁证（新证据源：trace_ring 的 VM 线程 syscall 桶）**：log61 VM 线程（tid 0x40010003）`delay_thread=0`，log62 =**4**——与 `th_start=4` 完美自洽（4 个新生儿各被 delay 一次后正常开跑）。**syscall 桶计数从此列入判读自证项**：门控/延迟类实验必须查 VM tid 桶的 delay_thread 计数
- 死点（v02.37 新锚：stub=810c2644 / ipe=810c236c，重编后地址变了——**每次重编后必须重取锚地址**）：log61 pc=**ipe+0x10**（`moveq pc,lr`）、log62 pc=**stub+0x4**（`ldr r0,[r5,#0x40c]` GP 表基址加载）——都在出生簇内继续游走，竞态指纹持续 ✓

### 2. 三剂量终验：log63（delay=5）/ log64（delay=10）仍冻结 → 假说判死

用户真机回报：5ms、10ms 与 1ms 表现一致，全部冻结。结合 log62 门控铁证（机制 100% 工作），判决：

**术前延迟（pre-dispatch delay）假说死亡。** 冻结不依赖"新生儿到达蹦床前"的时序窗口——把首 dispatch 推迟 1/5/10ms（三个数量级跨度内）都不改变结局。病灶在**切换本身或其后**：要么切换指令序列与某全局状态的互锁，要么新生儿执行的前几条指令（GP 表加载→ipe 检查→entry 跳转）与另一执行流的互锁。

### 3. 死点精确语义（v02.37 velf 反汇编，build/cmake/midp_vita.velf Oct 2 15:53）

```
stub (start_lightweight_thread_asm=810c2644):
  +0x00 ldr  r5,[pc,#120]     ; 文字池 → GP 表基址
  +0x04 ldr  r0,[r5,#0x40c]   ; ← log62 死点：读全局（Universe？）
  +0x08 ldr  r1,[r0,#0x68]
  +0x0c mov  r2,#0
  +0x10 tst  r1,#0x40000000
  +0x14 strne r2,[r0,#0x24]
  +0x18 bleq ipe(810c236c)    ; ← log57/58/60 死点家族（bleq 不写 lr——slide 锚定法依据）
ipe (invoke_pending_entries=810c236c):
  +0x00 ldr r0,[pc,#40]
  +0x04 ldr r0,[r0]
  ...  +0x10 moveq pc,lr        ; ← log61 死点：ipe 入口检查直返
```

**死点全部落在"蹦床头部读全局 + ipe 入口检查"序列**，从未深入——十证铁案"冻结=新生儿首次 dispatch"的指令级形态。

### 4. 遗产：v02.37 门控机制本身是有价值工具

`vita_last_started_id` 锚定 + 首 dispatch 触发的框架已验证可用（计数自洽、零扰动）。后续实验可复用：换 delay 为**内窥/干预动作**（如在门控点 dump 新生儿上下文+q5 环+GP 表目标值），无需再动构建体系。

### 5. 下一步方向（冻结窗口内窥，替代已死的延迟路线）

1. **GP 表/GLOBAL_JAVA状态窥探**：门控点（已验证可达）读出 `[r5+0x40c]` 指向的全局（疑似 `Global::` 侧 Universe/step_enable 类对象），dump 其 0x68 偏移字段——检验"另一执行流持有关键状态"假设
2. **q5 环完整快照**：冻结时 5 线程环（id=4 领头）各自 wait 字段/SP 走向——现有 site_ring 已有雏形（nrt obj 行），补 SP 采样
3. 备选：tickgate.2（从未跑过）+ 门控点干预组合实验

### 6. 版本记录

- v02.37 b410 sym 锚：stub=810c2644 / ipe=810c236c / `vita_last_started_id@812e1878`
- log61~64 全部为 v02.37 b410；log63/64 日志目录暂缺（用户口述），若后续导出可补符号化死点


---

## 2026-10-03：v02.38 vm_cpu 钉核实验（调度器丢线程假说的决定性检验）

### 1. 假说升级：内核运行队列丢失迁移中线程

log61/62 内核态证据链 + 三剂量判决后，假说从"术前时序"升级为：

**Vita3K 内核运行队列丢失迁移中的线程**。证据拼图：
- VM 线程 `status=RUNNING wait=none`，runClocks d=0（死前无时钟推进）
- 最后一个 syscall 已返回（不在内核等待）
- KICK suspend 跨核拒绝 `0x80020008`（线程不在目标核运行队列）
- `aff=0x70000`（3 核全开）+ `chgcpu ~2800`（高频迁移）
- 冻结所在核游走（log58 cpu1 / log61/62 cpu2）——与核无关
- 剂量无关（birth_delay 1/5/10ms 全冻）
- 一个故事解释全部：线程在核间迁移瞬间被 KICK/suspend，运行队列丢失

### 2. v02.38 实验：钉死迁移可能性

`Os::initialize()` 尾部（OS_vita.cpp ~292 行）新增运行期门控块：
- 读 `ux0:/data/J2ME00001/vm_cpu`（0/1/2），调 `sceKernelChangeThreadCpuAffinityMask(self, SCE_KERNEL_CPU_MASK_USER_0<<pin)`
- 缺省（无文件）= off，只打印当前 aff（对照组自证）
- 结果行打 `vm_boot.log`：`[vita] vm_cpu=N aff 0x70000->0x20000 rc=0`
- 注意 API 真名是 **Change**ThreadCpuAffinityMask（SDK 无 Set 版）；
  与 watchdog 的 chgcpu 计数对应同一内核调用

**A/B 判读表**：
| 结果 | 判决 |
|---|---|
| 钉核后 HANG 消失 | 调度器丢线程坐实 → 永久修复=默认钉核或 launcher 侧 |
| 仍冻 | 假说死亡 → 回 stub↔ipe 乒乓线（修 loopgen 浮点 stubs） |
| watchdog 里 vm aff=0x20000 | 门控生效自证 |
| vm_boot.log 无 vm_cpu 行 | 测错包（版本自证失败） |

### 3. 今日构建陷阱新记录（PM 固化）

1. **CMakeLists 版本号：CMake 取文件里最后一个 set(VITA_VERSION)**——本文件历
   史惯例是"新版本块插在旧块前面"，残留的旧 set 会遮蔽新版本（v02.38 首次出包名
   仍 02.37 即此坑）。bump 后必须 `python3 -c` 验证 last wins。
2. **vita-port 有两个 build 目录**：`build/`（陈旧，10-02 15:52 前的产物）与
   `build/cmake/`（真目录）。构建命令必须是 `cmake --build build/cmake`。
3. **loopgen 宿主链接失败（fast_memclear 未定义）不阻断 ARM 库 repack**——33
   objects OK + packed 29 members 即出库成功；loopgen 只影响 ROMImage 再生成，
   本次未改 Java 侧，无需再生成。（既有问题，非本次引入。）

### 4. 今日免费 coredump 三发现（log61/62 内窥）

1. **新生儿执行过指令**：log62 r5=81401924（GP 基址已加载=stub+0 已执行）→ 冻
   结非"从未 dispatch"，是"执行 1~10 条后被饿死"
2. **Z=1 乒乓可能**：log61 cpsr Z=1 + ipe+0x10 `moveq pc,lr` → pending_entries
   ==0 → ipe 返 stub+0 → stub↔ipe 无限互调可能；但 runClocks d=0 矛盾→统一解
   释：乒乓中被抢占后调度器永不再选它
3. **stub 探针从未生效**：`_vita_pe_enter` 符号在（814b464c B）但 stub 无引用
   代码——rebuild 脚本从旧库回抽 Interpreter_arm.o（缺 jvm_f2i/jvm_d2i），v02.30
   起所有 stub 侧改动从未进包；pe_enter=0 四轮恒零是无效证据

### 5. 版本记录

- v02.38 提交：phoneme-cldc 7a54eca（OS_vita.cpp vm_cpu 块）；vita-port 8699ea0
  + fixup（CMakeLists 02.38 块+尾随 set 清理）
- 产物：`vita-port/out/vpk/midp_vita_v02.38.vpk`（3681263B，Oct 3 02:33）
- 符号级验证：`sceKernelChangeThreadCpuAffinityMask` T@8110c170 + 两条 vm_cpu
  打印串 + 门控路径串，三中

---

## 2026-10-04：ASLR 幻影事故 + 真机死点修正（log64/65 判读基础重建）

### 0. 环境事实重申（最高优先级纪律）

**所有 HANG 均为真机冻结，与 Vita3K 无关**。log61~65 coredump 全部由真机
psp2core dump 产生。后续判读禁止引入 Vita3K host 侧分析——用户已多次纠正，
本次会话又犯（读 Vita3K thread.cpp 想解释 runClocks），**再犯即事故**。

### 1. ASLR 幻影事故（log64/65 判读推倒重来）

10-03 会话曾判读"死点落在 ROM 源数组区（_rom_heap_block/_rom_data_block_src），
+0x120000 后落 text_block0 三投三中"——**全部是幻影**。真相：

**真机内核有 ASLR，每次启动 slide 不同**。证据：
- site_ring.log 线程表 creator= 字段是运行时代码地址（Thread 创建调用点）
- v02.38 静态锚（.sym 反汇编）：tone BL@8100d084→ret 8100d089、wd BL@8100ee90
  →ret 8100ee95、ticker BL@810597f8→ret 810597fd
- log61 creator=8100d089 → **slide=0**（无 ASLR 或 slide=0 的启动）
- log62: 81077089-8100d089 = **0x6A000**（wd/ticker 锚互验 ✓）
- log64: 8107a089-8100d089 = **0x6D000**（三锚互验 ✓）
- log65: 8106f089-8100d089 = **0x62000**（三锚互验 ✓）
- log62↔64 creator 恒差、log64↔65 恒差 0xB000 = 每次启动 slide 独立
- r5(GP)=0x81401924 两轮相同 ≠ 代码 slide——GP 指向 RW 段，RW 分配地址与
  slide 无关（误把 r5 差当 slide 是 10-03 幻影的起点）

**判读纪律（固化）**：凡解读 coredump 任何代码指针，先扣 slide。四锚法：
site_ring.log 里 tone/wd/ticker 三行 creator= 减静态 .sym 锚即得，30 秒。

### 2. 修正后的真机死点（四次冻结全一致）

| 轮 | 版本 | slide | pc(静态真身) | 指令 |
|---|---|---|---|---|
| log58 | v02.35 | 未解 | stub+0x8（当时判读） | ldr r1,[r0,#0x68] |
| log61 | v02.37 | 0 | stub+0x18 区 | bleq ipe 附近 |
| log62 | v02.38 | 0x6A000 | **stub+0x04** | ldr r0,[r5,#0x40c] |
| log64 | v02.38 | 0x6D000 | **stub+0x00** | lstub 第一条 |
| log64 lr | | | **stub+0x00** | 出生帧原始返回址 |
| log65 | v02.38 | 0x62000 | **stub+0x08** | ldr r1,[r0,#0x68] |

共同签名：**pc∈蹦床最前 3 条指令、lr=stub+0、sp=出生栈、r0=r3=新生儿对象、
fp=0、cpsr=0x60070010（ARM 态 User 模式，Z=1）**。

**物理检查**：dump 里 stub 代码页 = HOLE（只读页未被写）→ 蹦床代码完好，
"代码被覆写"论死。

### 3. 修正后的最小事实集（全部真机证据）

1. VM 线程冻在出生蹦床入口 1~3 条指令内，从未执行到 bleq ipe（lr 未变）
2. runClocks d=0（watchdog 两次采样，tone/wd 字段同期正常走）→ 线程没有被
   执行，不是在循环里跑
3. 内核态 RUNNING/wait=none/最后 syscall 已返回 → 不在内核等待
4. KICK prio/susp/res 全无效（susp 0x80020008）→ 内核认为它"在运行"
5. 钉核（log65）不救：cpu1 钉死、chgcpu=7，冻结签名不变
6. ticker 同核（log65）也饿死 32 秒 → cpu1 上"什么都没跑"但内核仍报 RUNNING

### 4. 待验证方向（全部真机侧，无 Vita3K 假设）

自旋论已死（runClocks d=0 + 代码页 HOLE + stub 前 3 条无循环结构）。当前
唯一与全部事实相容的图景：**上下文切换器写入 newborn 上下文后，真机内核
从未真正 dispatch 它**——pc/lr/sp 只是切换器写进 TCB 的初始值快照。需要
解释：为什么内核报 RUNNING 却不分配时钟。

候选实验（下一轮）：
- A. 检查切换器（switch_to_asm/Scheduler）对 newborn 首切的完整语义
- B. born 栈顶"TestCompiler 指针"槽：出生帧必经，读 dump 验证出生栈完整性
- C. 出生栈 C 栈（sp=8167fd88/log64）与 TCB 内核栈是两个东西——查 scheduler
  设 sp 的方式与真机内核栈边界检查的关系

## 2026-10-04（下午）：C 栈取证闭环 + 调用链全景 + v02.39 born_dsp 探针（phoneme-cldc 40840fb / vita-port d402aa0）

### 1. C 栈顶帧全解（决定性证据链最后一环）

log64/65 的 C 栈（sp 处 40B，readsp.py）slide 修正后**完全一致**：

| 槽 | 静态值 | 含义 |
|---|---|---|
| [sp+0] | 810b5d2c | **JVM::start+0x18c = bl gadget 的返回地址**（gadget push{lr} 压入） |
| [sp+4] | 814b2f84 | **vita_run_count 地址**——主循环 bl 前自增的那个计数器（r3 残影） |
| [sp+8] | 81567b10 | JVM::start 帧的 r4（[sp-8]=_fini+0x83988 为加载器残影） |
| [sp+12] | 81567aa8 | JVM::start 帧的 r5 |
| [sp-4] | 810c1f78 | L415+0x10（gadget push{r3-r11} 的残影区边界） |

两轮 sp 均为"栈顶-0x278"（log64=0x8167fd88/栈基 0x81680000，log65=
0x813bfd88/栈基 0x813c0000——**栈基址每次启动不同（线程栈分配 ASLR），但
相对深度相同**）。**lr=stub+0 的真相**：gadget 的 `ldr lr,[jsp+4]; mov pc,lr`
把蹦床入口装进 lr 作为跳转载体——lr 不是返回地址，此前"call_on_primordial_
stack 尾声路径"的疑虑解除。

### 2. 调用链全景（5 个 gadget 调用点归属 + run=1 焊死）

- v02.38 全二进制仅 5 处引用 gadget（810c26a0），无 GP 表间接引用：
  - 810b5d28 bl ← **JVM::start**（vita_run_count++ 后，boot crossing）
  - 810b4f3c b  ← **JVM::run**（被 JVM::start 内联，同一 crossing）
  - 810893a4 bl ← **JVMScheduler::time_slice**（slave 模式专用，真机不走）
  - 810b1d14/810b1d68 ← **Universe::invoke_pending_entries**×2（romizer/
    TestCompiler 路径，#if USE_JAR_ENTRY_ENUMERATOR 死代码）
- **JVM::start 无主模式循环**：源码确认 run() 仅调用一次；snapshot
  `run=1`（log64/65）= boot crossing 恰好一次。此后绿线程切换全在执行栈上
  的纯 asm 切换器完成，C 栈**永远停泊**在 boot gadget 帧——这解释了游戏跑
  215 秒后冻结、C 栈顶仍是 JVM::start+0x18c。
- **中途 newborn dispatch 路径**：C++ `switch_thread`（vita_site_mark/
  VITA_DISP_REC/pump 在此）→ birth-delay gate → `switch_thread_master_mode`
  （仅 set_current，无 asm 切换！切换由绿栈上的生成代码完成）→ 蹦床。
  绿-绿切换器**不写 TCB pc**（内核线程一直在跑，从未让出）——"pc 是切换器
  写入 TCB 快照"假说的机制不存在，但"决策后跳转前/跳转中被夺核"仍开放。
- 上游对照：master 模式 VM 线程在 `check_blocked_threads`→JVMSPI_CheckEvents
  睡眠等事件；绿线程全部阻塞时这是唯一的 C++ 可见点。

### 3. dump 覆盖澄清（防误判）

psp2core 只捕获：C 栈（0x81381000..+0x40000）+ 三个环形缓冲 + 内核 note。
**代码段/堆/RW 段均不落盘**——"newborn 对象页 HOLE"、"蹦床代码页 HOLE"
均不能推出运行时 unmapped/被覆写；ROM 数据区幻影事故的教训在此复用。
SYSTEM_INFO(0x1001) 无 crash reason；0xb8dae≈75.7s 为运行时长。APP_INFO
自证版本串。THREAD_REG_INFO 每线程 376B：15 GPR + FPU d0-d15（全 NaN）
+ fpscr，无"停止原因"字段。

### 4. v02.39 born_dsp 探针（判别"pc=快照" vs "蹦床真执行"）

- **位置**（关键避开 loopgen 冻结陷阱）：Scheduler.cpp birth-delay gate
  消费 `vita_last_started_id` 处（= dispatch 决策点，C++ 侧，先于 asm
  跳转）；accessor `vita_sched_born_dsp()`；OS_vita.cpp snapshot v230 行
  加 `born_dsp=%u` 读出。
- **生成器侧探针永远死**：蹦床生成器在 InterpreterStubs_arm.cpp → 编进
  pack_lib 从旧库提取的冻结 Interpreter_arm.o——v02.30 pe_enter=0 的死因，
  C++ 侧是唯一活位置。
- **判读矩阵**（冻结时 snapshot v230 行）：
  - `born_dsp=0`：调度器从未决策 dispatch newborn → 冻结 pc 不可能是该
    路径快照 → **蹦床真执行了 0-2 条**（快照假说死）
  - `born_dsp>0`：决策已做 → 冻结在决策与蹦床完成之间；配合 pe 消费可
    分离"进 stub 前丢核"vs"进 stub 内丢核"
- 构建：rebuild_vm.sh（33 对象/29 成员）→ nm 验证 `_ZL14_vita_born_dsp`
  + `vita_sched_born_dsp` T 符号 → vpk 3681825B。蹦床指令编码逐条比对
  v02.38 无扰动（仅整体平移 +0x28）。
- **产物**：`vita-port/out/vpk/midp_vita_v02.39.vpk/.sym`。

### 5. 事故与基准保护

- **v02.38 产物污染**：版本块误插在 02.38 set 之前（last-set-wins），第一
  次构建把 `out/vpk/midp_vita_v02.38.vpk/.sym` 覆盖成含 born_dsp 的新
  代码（蹦床移到 810c26ec）。out/ 被 gitignore 无法恢复。**真 v02.38 符号
  基准唯一存档：`debug/tools/v02.38_full_disasm.txt.bak`（14MB，333287 行
  全量反汇编，源 /tmp/v38_dis.txt）**——log64/65 的 slide 四锚法仍以它
  为准。
- **v02.39 静态锚（新基准，log66+ 判读用）**：蹦床
  `810c26ec`（start_lightweight_thread_asm）、gadget 810c26a0 不变（其
  前无插入）、born_dsp 定义 814b4658、accessor 81087ef8。
- 版本块追加纪律：**新版本 set 必须在文件中最后一个 set(VITA_VERSION
  之后**（1152 行 02.38 → 1171 行 02.39 模式）。

## 2026-10-04（晚）：log66 判读——快照假说判决性死亡 + 六轮死点总表（版本修正版）

### 0. 版本基准修正（本次会话自查发现的错配）

log61/62/63 是 **v02.37 b410**（三剂量轮），不是 v02.38——用 v02.38 反汇编
解码它们的死点是版本错配（ASLR 事故的镜像错误）。各轮自证（site_ring
version 行）：log61=v02.37 bd=0、log62=v02.37 bd=1、log64/65=v02.38 b415、
log66=v02.39 b419。v02.37 蹦床静态地址与 v02.38 相同=810c26c4（生成器未
变，两版均 grep 确认）。

### 1. log66 核心读数（v02.39, slide=0xB000 双锚互验）

- **born_dsp=5 = th_start=5**（双采样稳定）：5 个 Java 线程首派决策全部
  完成——判别矩阵第二分支，但结合下述证据语义已变。
- **pc 静态=810c26f4=蹦床+8（第 3 条 ldr r1,[r0,#104]）、lr=蹦床入口**、
  fp=0、cpsr=0x60070010（Z+C+GE=111, User/ARM, **I=F=0 未关中断**）。
- **半执行铁证**：r5=0x81401924（instr1 `ldr r5,[pc,#120]` 的装载结果 ✓）、
  r0=0x81ee0d5c=cur_thread（instr2 `ldr r0,[r5,#1036]` 结果 ✓）、**r1=0
  （instr3 未执行**，r0 指向可读堆页，加载本应瞬间完成）——前 2 条真执行、
  第 3 条前停摆。**"pc=TCB 快照"假说判决性死亡**：快照无法解释"寄存器
  状态=指令序列的中间点"。
- **这是 id5 的死亡蹦床，非出生蹦床**：dispatch ring 最新 3 条全 id=5 流出
  （id5 跑过≥3 个量子）、born_dsp=th_start=5（全部首派早已完成）、
  **th_finish=0（进程尚无线程走完 finish——id5 是第一个 run() 返回的）**、
  q5 队列剩 id5/id3/id1 CYCLE(3)。机制：run() 返回→解释器 unwind 到 Java
  栈底哨兵→`mov pc,lr` 重入 start_lightweight_thread_asm（出生/死亡共用
  同一蹦床入口，哨兵=Thread::lightweight_thread_exit 四函数指针组）。
  log61-65 的"出生窗口"判读应修正为"蹦床窗口（出生或死亡）"。
- id5=jsp 0x81ee1bf0 执行栈顶部 [jsp+4]=810c1fb8（v39 静态，L412 生成代
  码）——死亡 unwind 残留。VM 最后 syscall=io_pwrite（已返回），非内核
  阻塞；THREAD_INFO 里 VM wait=none、不持有也不等待任何内核 mutex/sema/
  flag（0x100a-d note 全量核对）。
- 每核账本复现：**cpu0 idle/idleN/switchN 三项全 d=0**（cpu1/2/3 正常），
  ticker/wd 活跃——冻结核调度器完全停摆的签名第三次出现（log64 cpu0、
  log65 cpu1、log66 cpu0）。

### 2. 六轮死点总表（各轮用各自版本基准换算）

| log | ver | slide | pc 静态 | lr 静态 | pc 位置 |
|---|---|---|---|---|---|
| 61 | 37 | 0 | 810c237c | 810c2644 | L434+4（`b shared_call_vm` 尾分支）， lr=ICCE+4 |
| 62 | 37 | 0x6A000 | 810c2648 | 810c2644 | interpreter_throw_IncompatibleClassChangeError+8 |
| 64 | 38 | 0x6D000 | 810c26c4 | 810c26c4 | 蹦床+0（0 条执行） |
| 65 | 38 | 0x62000 | 810c26cc | 810c26c4 | 蹦床+8（2 条执行） |
| 66 | 39 | 0xB000 | 810c26f4 | 810c26ec | 蹦床+8（2 条执行，r1=0 铁证） |

- log61/62（v02.37 三剂量轮）死点在**异常抛出 stub**（shared_call_vm/
  ICCE 路径），不在蹦床——同为"生成代码段边界"家族。
- THREAD_REG_INFO 记录布局（修正，全轮通用）：`[0]=0x11 [1]=5 [2]=0x178
  [3]=tid [4..18]=r0..lr(15) [19]=pc [20]=cpsr`，每记录 376B。
- 全六轮共同签名：**fp=0**（出生/死亡栈哨兵初值）、cpsr=0x60070010、
  sp=各自 C 栈停泊层、r5=0x81401924（GP 锚）。

### 3. log65 钉核实验修正（此前结论作废）

boot 行 `pin vm cpu mask=0x20000 rc=0x70000`——sceKernelChangeThread-
CpuAffinity **返回非 0=失败**，THREAD_INFO 佐证 aff=0x70000（三核浮动）。
KICK1/KICK2 的 aff 改写 rc 也非 0（0x70000/0x30000 残留旧值）。**log65
不是钉核轮，"钉核无效"的结论从未被测试过**——钉核方案仍开放。

### 4. 收束后的机制图景（当前最佳解释）

绿线程切换器（生成 asm）恢复完目标线程全部寄存器、`mov pc,lr` 跳到蹦床
（出生/死亡哨兵）——跳转完成的瞬间到头几条指令之间被 IRQ 抢占进内核，
此后**该核的内核调度器不再运行任何线程**（idle 账本冻结=既不调度也不进
idle）。cpsr I=F=0 证明用户态未关中断。这是**真机内核侧冻结**，冻结点
恒定落在"切换完成边界"——3 个版本 5 轮一致。用户态能做的下一步是把
"切换完成边界"从直跳蹦床改为经 C 着陆垫，或让 VM 线程真正单核化。

# v02.40 A 方案（C 着陆垫）完整落地记录

## 版本快照

- **产物**：`vita-port/out/vpk/midp_vita_v02.40.vpk` / `.sym`（**正确构建树 2026-10-05 17:03 归档**；10-04 18:07 首版为错误构建树产物，已被替换）
- **git**：phoneme-cldc `dddd48b`（垫三部曲）→ `2372686`（ROM.cpp mffd 修复）；vita-port `7b71512`（版本块）
- **v02.40 新锚点（下次 core dump 判读基准，必记）**：
  - 蹦床 `start_lightweight_thread_asm` = **0x810c277c**（v02.39 的 810c26ec +0x90）
  - C 着陆垫 `vita_landing_pad` = **0x8109a4f8**（.text，13 条指令）
  - 探针 `vita_v240_land` = **0x814b4b18**（B）、`vita_v240_fp` = 0x814b4b14、`vita_v240_lr` = 0x814b4b10
  - `setup_stack_asm` = 0x8109b8f0（播种指令 `movw/movt r2 → str [r3,#-20]`，哨兵字=垫地址）
  - ⚠️ **10-05 勘误**：首次发布的 v02.40（10-04 18:07 归档）是**错误构建树产物**——用了旧极简入口 `midp-vita/build_midp_vita.sh`（仅 main.c 对象），缺整个 vita-port 启动器（~300KB：vita_menu/wraps/libpng），真机打开即 C1-2543-4 无 coredump。正确构建树是 `vita-port/build.sh`（或手动 cmake+make）。上表锚点已全部换算为正确构建树 10-05 17:03 产物。

## 1. A 方案设计（冻结边界从 asm 空隙移进可插桩代码）

绿-绿切换尾：`mov pc,lr` 直跳蹦床（哨兵字=蹦床地址），边界落在不可插桩
asm 空隙。A 方案把哨兵字改为 **C 着陆垫地址**：出生线程恢复完寄存器后
先落进垫（计数+拍 fp/lr 快照）再 `bx` 蹦床。冻结若仍发生，pc/探针组合
即可钉死边界落在垫前、垫内、还是垫→蹦床交接。

### 垫 ABI 约束（为什么必须 naked + 手写 asm）

- 垫入口 ABI：lr=垫地址+4（哨兵字本身被 `ldr lr,[jsp,#4]` 装载）、fp=0、
  jsp=r6=lw_exit 字、gp=r5 未定义（蹦床首条自设）
- `Assembler_arm.hpp:150-180` 寄存器约定：gp=r5、jsp=r6、locals=r7、
  cpool=r8、fp=r11 全是 callee-saved；蹦床第 7 条 `ldr r0,[r6,#16]` 依赖
  它们 → **垫绝不可破坏 r4-r11**
- 编译 C 函数会合法使用 r6 且 epilogue 永不执行（尾跳）→ 必须
  `__attribute__((naked))` + 只碰 r0/r1 + movw/movt 绝对寻址（非 PIE）
- **语法教训**：`__attribute__((naked)) extern "C" void f()` 非法——
  必须 `extern "C" void f(void) __attribute__((naked));` 声明后置属性

## 2. 改动文件（3+1）

| 文件 | 改动 |
|---|---|
| `src/vm/cpu/arm/InterpreterRuntime_arm.cpp` | 垫定义（13 条 naked asm）+ v240 探针变量 + `setup_stack_asm` 哨兵字 `#if defined(VITA)` 播垫地址 |
| `src/vm/os/vita/OS_vita.cpp` | watchdog v231 行后新增 `v240 land/fp/lr/pad` 读出行 |
| `src/vm/share/runtime/Scheduler.cpp` | `vita_sched_dump_stack` 哨兵模板注释同步（第 6 字=垫） |
| `src/vm/share/ROM/ROM.cpp` | `#if defined(VITA)` 定义 `_rom_linkcheck_mffd_false`（见 §4） |

## 3. 垫反汇编（发布版逐指令验证通过）

```
vita_landing_pad @ 0x8109a4f8:
  movw r0,#0x4b18; movt r0,#0x814b   ; &vita_v240_land
  ldr r1,[r0]; add r1,#1; str r1,[r0]   ; land++
  movw r0,#0x4b14; movt r0,#0x814b   ; &vita_v240_fp
  str fp,[r0]                           ; fp 快照（垫 ABI 下应为 0）
  movw r0,#0x4b10; movt r0,#0x814b   ; &vita_v240_lr
  str lr,[r0]                           ; lr 快照（应为垫地址+4）
  movw r0,#0x277c; movt r0,#0x810c   ; start_lightweight_thread_asm
  bx r0                                  ; 尾跳蹦床
```

只碰 r0/r1 ✓、重定位全 R_ARM_MOVW_ABS_NC/MOVT_ABS ✓、蹦床首 5 条与
v02.39 逐指令相同（冻结成员原样）✓。

## 4. mffd_false 链接事件（v02.40 专属坑，防复发）

**现象**：VPK 链接 `undefined reference to _rom_linkcheck_mffd_false`
（_MergedSrc005.o .data+0x10）。v02.39 能链、v02.40 不能。

**根因链**：
1. 宏不对称**历史既存**：`ROMImage.hpp:250-275` 的 linkcheck 宏把
   `MSW_FIRST_FOR_DOUBLE` 映射成 `_rom_linkcheck_mffd_true/false` 指针；
   MIDP.gmk（~246 行）编 ROMImage.cpp 用 `-DMSW_FIRST_FOR_DOUBLE=1` →
   定义 **mffd_true**；VM 侧 ROM.cpp 不带该宏（ARM VFP 视角→引用
   **mffd_false**）。两侧永远对不上。
2. v02.39 靠**混入物侥幸**：v02.39.sym 里 mffd_false@8110deac 邻接 11 个
   `_rom_check_*` 符号（romgen 布局 ROMImage.o 特征）——提供者是
   `phoneme-cldc/build/vita_arm/romgen/app/ROMImage.o`（2026-09-05 产物）
   混进链接线，靠 `--allow-multiple-definition` 与 MIDP 的 mffd_true 并存。
3. **全量重打包显形**：rebuild_vm.sh 的 pack_lib 重打包 libcldc_vm_g.a
   后混入物消失 → 未定义引用。

**修复**：ROM.cpp 补 `#if defined(VITA) const int _rom_linkcheck_mffd_false
= 0; #endif`——VM 侧可重编成员（_MergedSrc005），不动上游 MIDP.gmk，精确
复现 v02.39 解析语义。运行时 linkcheck 指针指向链接器选择的定义（v02.39
即如此运行，实机 ROM 加载正常佐证）；ROM.cpp:378 GUARANTEE 仅在
`_rom_compilation_enabled` 时检查，ROM AOT 未启用不触发。

**防复发**：今后凡 pack_lib 全量重打包后出现 linkcheck 类 undefined，
先查宏不对称（ROMImage.hpp 宏映射 × 各编译单元实际 -D 旗标），再看是否
历史混入物消失。

## 5. 判读矩阵（v02.40 冻结时 watchdog 快照读 v240 行）

| 读数 | 判定 |
|---|---|
| land=0 | 冻结在哨兵交接**前**（结合 born_dsp>0 即钉死 asm 空隙——A 方案核心目标） |
| land>0 + pc 在垫内（0x8109a4f8..0x8109a530） | 冻结首次进入新代码（垫本身） |
| land>0 + pc 仍在蹦床头（0x810c277c..） | 边界=垫→蹦床交接 |
| land>0 + 蹦床完整执行 | 出生尾通过，冻结后移——看下一版本锚点 |

探针语义：land=落地次数（每次哨兵交接 +1）；fp 在垫 ABI 下恒 0（若非 0
说明入口 ABI 假设错）；lr=垫地址+4（哨兵字装载值）。

## 6. 遗留风险

- mffd_false 定义属链接自足性补丁，运行时语义不变；但需留意 v02.40 实机
  ROM 加载是否依旧正常——若出现 ROM 相关异常回查 §4。
- `--allow-multiple-definition` 仍在链接线（MIDP 侧 mffd_true 与 VM 侧
  mffd_false 现在是两个不同符号，不再冲突，但该旗标历史上掩盖过混入物，
  长期应移除）。

---

# log67 判读与 v02.41 libpng 屏蔽实验（活锁判决，2026-10-05~07）

详录：`debug/notes/log67_livelock_and_v0241_png_off.md`。要点：

1. **log67 判决（推翻 log66"首次派发即冻结"）**：pc=垫+0x18（首次进新代码）但 land 双快照 303,949,169→309,758,141（~14.5M/s 热旋转）+ dispatch ring id=3 pe=0 x16 + 优先队列 `q5: id=4→id=3 CYCLE(2)` = **绿线程调度器活锁**；log64-66"蹦床半执行"是旋转中途丢 CPU 快照。旋转不经过 C++ 调度器（switch=185,015 静止）。v230/v231/v240 探针行在 **site_ring.log**；dump LOAD 段不含探针 .bss。
2. **v02.41（d48161a）**：`VITA_NO_PNG` 屏蔽 libpng（唯一运行时漏斗 `vita_icon_decode_png` 早退 -1，桩=`mov.w r0,#-1; bx lr`；MIDP 皮肤不走 libpng；cache.bin 无需清）。锚点：垫@0x8109a270、蹦床@0x810c24f4、探针 0x814b4b10-18（.bss 不平移）、128 符号、md5 e6844e2f3f2dfdca602397289047e4d1。
3. **判读矩阵**：图标消失+`[AUDIT] calls=0`=屏蔽自检；冻结消失=libpng 堆破坏实锤（回补查 rowbytes 族 bug）；仍冻结=活锁与 libpng 无关，下一主战场=**优先队列成环成因**（Scheduler 挂回路径/resume 延迟入队/重入 reschedule）。
4. **环境**：`vitasdk.sh` 不存在，用 `export VITASDK=/home/zyb/.local/vitasdk; export PATH=$VITASDK/bin:$PATH`；正确树纪律不变（vita-port cmake+make，midp-vita/build_midp_vita.sh 永久废弃）。

## 2026-10-07（二）：v02.42 队列 forensics 包发货（log68 预备）

1. **背景**：log67 判决活锁为纯 asm 闭环（unwind→哨兵→垫→蹦床→桩 `moveq pc,lr`→再 unwind），switch/lw_exit/ts_gadget 全静止，环内无 C++ 检查点 → 成因观测面只剩"最后一批 C++ 队列操作"。同轮实锤 v02.30 `_vita_pe_enter` 是死探针（冻结 Interpreter_arm.o 吞掉生成器侧计数）→ 活探针只能放 fresh 单元。
2. **v02.42（CLDC 3be09f5 / vita-port 7f71d3d）**：三件套——(a) `_vita_q_ring` 64 条 (id,op,pri,pe) 六 op 插桩（ADD/REM/SEL/NRT×3/PRI/JBC，站点表见 notes）；(b) 垫 `vita_v242_pe[2]` pe 存活计数（读 `_current_thread`+0x24，双槽 pe==NULL/!=NULL，basic asm 只碰 r0/r1）；(c) watchdog `[rd0 qring]` 分节（dump 于 dispatch ring 前，成因先于后果）+ `v242 pe0/pe1` 行。判读矩阵与锚点见 `debug/notes/log68_prep_v0242_qring_forensics.md`。
3. **锚点**：垫@0x8109a5a0（+0x330 平移）、`_current_thread`@0x812e1d30（GP 首槽未平移）、`_vita_q_ring`@0x814b4d34、`vita_v242_pe`@0x814b5718、129 符号、md5 dbb85acf66b72e1aee0706a5b346c993。
4. **新教训**：垫引用 `_current_thread` 使 loopgen 宿主 exe 多一条 undefined（历史既有报错之外）——判据是库内有无 `D _current_thread` 定义与 vita-port 最终链接，非 loopgen 退出码。v02.42 仍携带 VITA_NO_PNG（02.41 块 flag 持续生效）：若直接跑不冻结无法归因 PNG，仍冻结则 qring/pe 数据正交可判读。

## 2026-10-07（二）：log68 判决 + v02.43 发货（SEL 探针修复 + libpng 恢复）

详录：`debug/notes/log68_verdict_and_v0243.md`。要点：

1. **log68 判决：空蹦床弹跳**。pe0 以 ~10M/s 疯涨（187,698,350→191,800,352/400ms）、
   pe1=5 冻结、v240_fp=0（出生帧模板值）；qring seq 冻结 591（自旋不产生任何 C++
   队列操作）；coredump pc=invoke_pending_entries+0、lr=vita_landing_pad+0、r1=0、
   fp=0（桩入口边沿，pc 有 dump 伪影风险）；id4 Java 栈顶 inflate+0x173（JAR 解压）。
   核心谜题：弹跳者 fp=0 出生帧 vs _current_thread=id4 有正常栈——**身份不同步**。
2. **libpng A/B 正式闭合**：v02.42 VITA_NO_PNG + [AUDIT] calls=0 仍冻结 → 排除，
   v02.43 恢复（版本块不设宏，vita_icon.c 零改动走全解码器）。
3. **SEL 探针 bug（v02.42）**：`(Thread*)thread.obj()` 双解引用——int_field 读
   `*(qp_->_obj+off)`，而 obj() 已是 ThreadObj*，实际读 *(klass+id_offset)=ROM
   指令字 0xE5902014（`ldr r2,[r0,#20]`）。SEL 站点 thread 是 `Thread::Raw` 局部
   handle（ADD/REM 是 `Thread*` 参数直传故正确）。修复=`VITA_Q_REC((Thread*)&thread,...)`：
   PRODUCT 下 BasicOop 无虚表、首 word=_obj，与 Thread* 布局同构；同函数先例
   `return (Thread*)&_next_runnable_thread;`。队列本身没坏，SEL 序列 log69 起可读。
4. **v02.43（fa8059a）**：SEL 修复（Scheduler.cpp 2709 行区）+ libpng 恢复
   （CMakeLists 1278 行区版本块）。锚点与 v02.42 完全一致（垫@0x8109a5a0、
   桩@0x810c257c、蹦床@0x810c2854、_current_thread@0x812e1d30、qring@0x814b4d34）
   ——布局未漂移。md5=133206ba046921a41379c4f4f64cab14。
5. **log69 判读要点**：(a) SEL 序列首次可读，与 ADD/REM/NRT 交错重建冻结前调度史；
   (b) 确认弹跳者身份（SEL pe 列 + disp ring 对照，预期 pe=0）；(c) v242 pe0/pe1
   + v240_fp 继续观察；(d) pc 仍落 stub 入口时结合 SEL 历史判断"选择后未执行"还是
   "执行完未出队"。

## 2026-10-08：log69 判决 + v02.44 发货（出生帧解剖 + PNG 真恢复）

1. **log69 判决**：SEL 修复生效（id 全可读，尾部 id4 SEL→NRT，seq=549 冻结，
   pe=0）；coredump pc=**start_lightweight_thread_asm+0x8**（第 3 条指令，读
   status），lr=pad、fp=0、r0=id4 ThreadObj → **弹跳者=id4 本人**；r6=
   0x81ed0804 与 id4 stack_pointer(0x81ed0748) 差 0xBC——jsp 不在出生帧位，
   退出表索引的是残留栈。全部 C++ 计数器冻结 → 纯 asm 环，唯一 suspect 边=
   退出表槽位损坏。
2. **语义修正（判读 [r6+16] 必读）**：出生帧 jsp+16 槽=**TestCompiler 旗标**
   （PRODUCT+ENABLE_PERFORMANCE_COUNTERS=0 → `const bool = false` 编译期常量，
   预处理器实证），模板值=0，非函数地址。[+16]≠0 = 垃圾导向 L444。
3. **PNG 真恢复**：v02.43 失败根因=CMAKE_C_FLAGS 自引用**累积追加**（v02.41
   块 1245 行活代码），"不再设置"≠撤销。v02.44 用 `string(REPLACE)` 显式剔除
   （`${CMAKE_C_FLAGS}` 必须带引号防 list 重连）。二进制级验证：反汇编
   `vita_icon_decode_png` 为真实函数体（png_sig_cmp 链），非 -1 桩。
4. **v02.44 探针**：垫 +0x58..+0x78 九条指令记 `vita_v244_ct`（_current_
   thread）/`vita_v244_r6`（退出表基址）；watchdog **rd7** dump `[r6-8..+16]`
   + 模板对照 + ct.stack_pointer + 两样本 r6 差（call_on_primordial_stack
   `stmdb r6!,{lr,fp}` 每圈 -8 指纹）。
5. **v02.44（63763ff）**：三文件（InterpreterRuntime_arm.cpp / OS_vita.cpp /
   CMakeLists.txt）。VPK md5=6cbfe4350e13f5ef79bee8694d052355，版本串
   "v02.44 b434 (63763ff)"。rm _MergedSrc004.o+OS_vita.o 后重编；符号/反汇编/
   重定位/格式串四链验证通过。log70 判读顺序：rd7 帧字 → v244_ct 身份 → r6
   滑动 → PNG 图标显示。

## 2026-10-08：v02.45 环自陷饱和取证包（c9fac02）

1. **动机**：真机轮次昂贵，用户要求一次打满。思路转变——不再等冻结后采样
   （watchdog/rd7 是 400ms 后的二手现场，coredump 是抽奖且从不映射 Java 栈
   页），让死环**当场自己招供**：垫是唯一保证在环上的已插桩点（log68/69 实证
   环以 ~10M/s 穿过垫），在垫内装洪水检测器。
2. **洪水判据**：连续 256 次 pe==NULL 弹跳。合法运行不可能——正常出生跨垫时
   pe!=NULL（log69 冻结时 pe1=5 恒定），到达阈值即活锁实锤，毫秒级触发。
3. **trip 序列**（垫 asm，只用 r0-r3/ip，r4-r11 只读）：保存 r0-r13 到
   `vita_v245_flood[20]`（[0]consec [1]tripped [2]ct [3..13]r3..lr [14..19]
   保留）；每 4 次弹跳记 (r6, lr) 对到 `vita_v245_hist[9]` 环（lr 直接命名环
   闭合边）；切 `vita_ring_trap_stack_mem+8192` 私有栈（符号布局已验证：
   trap_stack_mem@0x68, flood@0x2068, hist@0x20b8——+0x2000 严丝合缝）；
   `bx vita_ring_trap`。
4. **vita_ring_trap**（OS_vita.cpp，写 ux0:/data/J2ME00001/ring_trap.log 后
   sceKernelExitProcess）：flood 寄存器现场 → hist 弹跳历史（含 per-bounce
   r6 delta）→ 模板地址行（exit/uncaught/finish/force_term/flagword=0/pad）
   → r6 退出表窗口 [r6-8..+24] → ct.stack_pointer + 出生帧窗口 → 保存 sp 的
   C 栈窗口 [sp..sp+60] → qring/dispatch/nrt/threads 全量调度取证。写序按
   证据重要性排列（trap 自身若崩，前面的已落盘）。
5. **后备链**：全部 v02.44 探针保留；rd7 新增 `[rd7 flood] consec/tripped`
   行——若只有 watchdog log 回来，tripped=0 + consec 巨大=环不过垫（陷阱失
   效的世界），tripped=1=trap 已写 ring_trap.log。
6. **v02.45（c9fac02）**：三文件 +281/-5。VPK md5=d1d7a332ffc96f278e262ee
   46d5aee4b，版本串 "v02.45 b436 (c9fac02)"。四链验证：nm（v245 符号 +
   U vita_ring_trap）、objdump（cmp #256 / add #8192 / 73 指令垫）、OS_vita.o
   T vita_ring_trap、ELF 含 "== RING TRAP v02.45 ==" 格式串。
7. **log70 判读顺序**：ring_trap.log 若在 → hist 的 lr（闭合边）→ r6 逐圈
   滑动量（命名 gadget）→ 退出表窗口 vs 模板 → 帧字身份；只有 watchdog log
   → rd7 flood 行分世界。

## 2026-10-08：v02.46 符号化 Java 栈（ring_trap + watchdog）（c5175f3）

1. **起因**：用户问"Java 栈页也可以通过 JVM 去打印吧"。查证结论：这套
   PRODUCT 库里 **VM 自己没有任何栈打印**——Thread::trace_stack 与全部
   *Frame::print_on 在 Frame.cpp:1013-1623 的 #ifndef PRODUCT 里；pss() 需
   ENABLE_PRODUCT_PRINT_STACK（BuildFlags 默认 0，jvmconfig.h 实证）；pretty
   签名解码器 print_decoded_on 也不在。直接调 trace_stack 会链接失败。
2. **正路**：照 Debug.cpp:692 product_trace_stack_from（当年 Sun 为 PRODUCT
   deadlock 诊断手写的模式）自建符号化遍历。全部依赖已在最终 ELF 符号级验
   证：Frame::init(T)、JavaFrame::method(T)、Method::name/signature(T，
   _MergedSrc004)、holder()→Universe::class_from_id(W)、JavaClass::name/
   Symbol::base_address+length(inline)、bci_with_flags(T)、
   is_entry_frame/caller_is/is_first_frame(inline)。GUARANTEE 在 PRODUCT 空
   展开（Debug.hpp:76），遍历安全。
3. **vita_sched_dump_jstack**（Scheduler.cpp 尾，归 _MergedSrc002.o）：遍历
   global_threadlist（≤8 线程 × ≤32 帧），逐帧打 `class.method(原始签名)
   bci=`；Raw 句柄、ROM 只读、零分配、无 tty 无锁——trap 私有栈与 watchdog
   两侧皆安全。**每次解引用前用线程自身 stack_limit/base 范围检查 fp**——
   活锁前提就是 jsp 被弄坏，坏 fp 必须停走而非崩掉 dumper。单任务假设成立
   （MIDP 单 task，trap 上下文即弹跳线程的 task）。
4. **接线**：ring_trap.log 调度取证尾部 + watchdog rd7 尾部（陷阱失效后备）
   各加一段。签名用原始 UTF8（"(I)V" 本来就可读）。
5. **v02.46（c5175f3）**：三文件 +170/-1。VPK md5=435750e76aaf692b5d2ac41
   8394868c9，版本串 "v02.46 b438 (c5175f3)"。验证链：_MergedSrc002.o 出
   T vita_sched_dump_jstack（U 集恰为 4 个库符号）、最终 ELF T 符号 +
   反汇编真函数体（push {r4-r11,lr}，10 处 bl）。
6. **log70 判读新增**：jstack 段直接回答"每个线程冻结瞬间在执行哪个方法"
   ——弹跳者(id4)若 fp=0/坏 fp → 从未真正进过 Java；若有帧名即案发
   第一现场。与 hist lr（闭合边）互为印证。

## 2026-10-11：v02.50 探针+刹车包（4df2961，首个行为版）

1. **起因（log73 判决）**：v249 行全冻结定拓扑 A 极端版——环=纯 asm 的
   pad↔stub 乒乓（land=261=pe0(256)+pe1(5)，hist 三词恒定），trip 现场=
   出生模板精确复刻（fp=0/lr=pad/sp=原始主栈/r6=退出表基址）。**finish=0
   悖论**：蹦床头到 `bl finish` 是无分支直线，255 圈穿越零 finish → 环在
   +0x1c 前离开；蹦床上唯一无计数器的分支谓词是 `_current_pending_
   exception`（gp+0x410）守护的 `blne c_o_p_s(uncaught)`，且 c_o_p_s
   epilogue 与 [r6-4]=pad 逐位吻合——cpe 是最后一个未计数嫌疑者。
2. **探针**：hist 槽 3→4 词（13→17 词），第 4 词=每次穿越的 cpe 快照。
   环若骑在 cpe!=0 分支上则四槽全非零；若全零则悖论另有解释（下一层）。
3. **刹车**：flood 门 trip 时不再永久 park——pad 取 brakes<8 走**设计好
   的死亡门** `ldr r0,[r6]; bx r0`（=[r6]=lightweight_thread_exit）→
   terminate（op-2 REM，破 q5 CYCLE(2)）→ yield → 活线程。tripped=2
   通知 watchdog：dump（同体+brakes 计数）+ **不退出**，监控 CE 泵——
   CE 恢复=自愈（日志+恢复正常轮次）；30s（120×250ms）不恢复=退出
   （不愈合的刹车只是更慢的杀）。brakes>=8 回落 v02.48 park 兜底，最坏
   退化到已验证行为。
4. **实施教训**：仓库根是 `samples`（phoneme-cldc/vita-port 同库）——
   `git add -A` 会卷进 j2me/ 下 8/30 遗留的 19MB 杂散补丁
   0001-save-change.patch，amend 剔除（工作区保留 untracked）；multi_
   replace 工具的 replacements 数组必须逐项带 filePath（三次校验失败教
   训）；OS_vita.cpp 顶部 extern 区须提前声明 vita_v250_brakes（两个
   dump 点在声明前使用）。
5. **验证链**：33 ARM objects + 29 members；反汇编 pad 核验（hist 四元组
   [r2,#4/8/12/16]、16i 缩放链 mov r3,r2+add×4、trip cpe 存 [ip,#56]、
   brakes 自增+镜像 [ip,#80]、cmp #8/bcs 预算分流、BRAKE `ldr r0,[r6];
   bx r0`、park wfe 兜底）；四新格式串 strings 落地；版本串
   "J2ME Player v02.50 b452 (4df2961)"；VPK md5 91b4ba0a95e3d358c550d8
   278e721088（out/vpk/midp_vita_v02.50.vpk）。
6. **下次日志读法**：(1) 头部 "RING TRAP v02.50 BRAKE"=刹车触发、
   brakes=N 圈数；(2) hist cpe 词——四槽全非零=环骑 cpe 分支（悖论定
   谳），全零=下一层；(3) wd 日志 "BRAKE HEALED: CE x -> y"=自愈成功
   （VM 存活、恢复正常轮次），无此行+退出=刹车未愈合（30s 兜底）；(4)
   v250 行 cpe_at_trip=trip 瞬间值。**首个行为改变版**：刹车若破坏
   lw_exit 前提会退化 park 兜底（brakes 停在 8），最坏=v02.48 行为；
   回滚路径=revert 4df2961 即回 v02.49 纯取证。
   详细读法见 debug/notes/v02.50_notes.md。

## 2026-10-09：v02.49 环取证升级包（ff6334c，二进制=ff6334c，注释跟随 d2194c6）

1. **起因（log72 判决续）**：handoff 全链验证成功，环=entry-less 派发的
   pad↔birth-stub 乒乓确认。但反汇编 v02.48 发现**拓扑矛盾**：pe==NULL
   线程 trampoline 死亡尾全为 C++（finish@810c3860→force_term@810c3870
   →task_cleanup@810c3878→lw_exit@8109cefc→GUARANTEE halt），若环每圈
   跑完此尾则 qring seq 必为千万级，而 log72 冻结在 536 → 环闭合于死亡尾
   中途，且 ring_trap.log 无任何 C++ 计数器=观测缺口。
2. **两个 attribution gap**：(a) id4 的 ADD pe=0 反常——出生 ADD（Thread::
   start()→add_to_active）必见 pe=1，pe=0 暗示 ADD 来自 notify_wakeup/超时
   唤醒路径或 entry 首派前消失；(b) id3 有 16 条 SEL 零条 NRT——SEL 后必经
   switch_thread→set_current 产生 op-4 NRT，无 NRT 说明 SEL 来自纯查询者
   （yield/sleep_forever/terminate 都用 get_next_runnable_thread() 轮询）。
3. **四处取证升级（无行为改变）**：
   - qring 第 5 列 caller（`__builtin_return_address(0)`）→ 定 id4-ADD/
     id3-SEL 归属；`_vita_q_ring[VITA_Q_RING][5]`，格式 `from=%08x`
   - v249 计数器行（watchdog + ring_trap 双侧）：`finish/force/setcur/
     term/taskclean/land/pe0/pe1/switch`——taskclean 是死亡尾四体中唯一
     原无覆盖者（vita_v249_taskclean 在 thread_task_cleanup 入口自增）
   - hist 三元组第三词 `[r6-8]`（9→13 词）：跨 4 条记录变化=死亡尾
     prologue（str lr,[r6,#-4]! / str fp,[r6,#-8]!）在环内执行；静止=
     纯 asm 拓扑。**[r6-8] 探针原理即 call_on_primordial_stack prologue**
   - 头部保持 "RING TRAP v02.45"（格式连续性，diff 跨版本可比）
4. **实施教训**：hist 三元组缩放首版写成两次 `add r2,r2,r2`（=4×idx），
   自查抓出重写为 `mov r3,r2 / add r2,r2,r2 / add r3,r2,r3 / add r3,r3,r3
   ×2`（=12×idx 字节）；另修复 OS_vita 误删 v242 行 sceIoWrite、注释块杂
   散 `*/` 两处。
5. **验证链**：33 ARM objects + 29 members（loopgen 宿主链接错误为预期
   噪声）；反汇编 pad（8109b5f8）逐条核验缩放链/三元组落位/trip-park 尾
   段（bx 810c39d4 新址 trampoline）；三新格式串 strings 落地；版本串
   "J2ME Player v02.49 b449 (81d59c9)"；VPK md5 237cbbc3bc2373cd4fb7c85
   8b7ff3a5（out/vpk/midp_vita_v02.49.vpk）。符号：vita_v245_hist@814b7870、
   vita_v249_taskclean@814b77b0。
6. **下次日志三问**：(1) v249 行定拓扑 A/B（全冻结=纯 asm 环 / 百万级=C++
   环，后者与 seq=536 矛盾需重审）；(2) qring caller 定 id4-ADD(id3-SEL
   归属——from= 指向 notify_wakeup 还是 Thread::start）；(3) hist [r6-8]
   定 prologue 是否在环内。刹车（pad 穿越时走 [r6]=lw_exit 干净退场）推迟
   v02.50：候选闭合点不止一个、依赖 r5=gp 前提、盲刹可能切断合法出生路径。
   详细读法见 debug/notes/v02.49_notes.md。
