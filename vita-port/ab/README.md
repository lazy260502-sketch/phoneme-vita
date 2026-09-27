# log40 之后的 A/B 实验（v02.16）

目的：判定停摆是**与具体 jar 内容相关**，还是**所有 MIDlet 都要走的通用路径**。

## 为什么需要 `menu=0`

log36..log40 五次复现的冻结都发生在 `midp_run_midlet_with_args_cp` →
`JVM_Start+0xa4` → `JVM::start()+0x178` → `primordial_to_current_thread` →
`start_lightweight_thread_asm+0x18` 这条路径上（即"第一次切入 MIDlet 的 Java
代码"）。但每轮都先进了**原生菜单**（2 MB framebuffer、触摸初始化、
文件系统扫描），这是一个共同变量。

v02.16 起 `launch.cfg` 支持一行 `menu=0`：launcher 读到它就**完全跳过**
`vita_menu_run()`，直接启动 cfg 第 1/2 行指定的 jar/class。这样一轮 A/B 里
不再有菜单的任何残留状态可以被归咎。

无 `menu=0` 行时行为与 v02.15 完全一致（菜单优先）。

## 部署

把对应样例改名成 `launch.cfg`，放到真机的 `ux0:/data/J2ME00001/`：

```
ux0:/data/J2ME00001/launch.cfg
```

**⚠ jar 路径必须是相对的**（不能写 `ux0:/data/...`）：main 已 `chdir` 到数据
目录，且 VM 的 classpath 用 `:` 分隔，绝对路径会在第一个冒号处被切坏
（变成 `ux0` + `/data/...`）。

## 判读

开机后不再出现菜单，直接进 MIDlet。跑完（冻结后看门狗会自动 dump）看：

- `boot_log.txt` / `midp_stderr.log` 应有：
  ```
  launch.cfg jar (menu=0): Hello.jar
  launch.cfg class (menu=0): HelloMIDlet
  ```
  → 确认这轮**没有**走菜单（不会有 `menu jar:` 行）。

- 冻结判据（与 log36..log40 逐项一致）：
  ```
  [WD] VMPROG verdict exec=0 tick=0 -> NEITHER
  vm runClocks d=0 ; cpu1 idleN/switchN d=0 ; KICK no effect
  ```
  死点应在 `start_lightweight_thread_asm+0x18` 附近。

- **若 HelloMIDlet 冻结签名与 game.jar 完全相同** ⇒ 冻结是**通用路径**问题
  （与 MIDlet 内容无关），方向 A 的"换 jar"分支关闭，转方向 B / 调度 A/B。
- **若 HelloMIDlet 不冻结** ⇒ 与 jar 内容/类初始化相关，转去分析 game.jar 的
  启动路径差异。

## 样例文件

| 文件 | jar / class | 用途 |
|---|---|---|
| `launch.cfg.hello` | `Hello.jar` / `HelloMIDlet` | **首选**：最小 LCDUI，不碰音频/网络/RecordStore |
| `launch.cfg.tonetest` | `Hello.jar` / `ToneTest` | 自编 MMAPI 测试（会主动播放音频） |
| `launch.cfg.canvas` | `Hello.jar` / `CanvasTest` | 图形 / Canvas |
| `launch.cfg.game` | `games/app (1)/game.jar` / `com.app.filemanager.FileManagerMIDlet` | 复现 log40 的对照组 |

---

# v02.17：一次性插桩 + 运行期 A/B

v02.17 的出发点是**"能调的东西一次性都加上"**——真机一轮的成本（重编、重装、
重启、取日志）远高于任何探针的开销，所以这一版把剩下所有问题需要的证据
全部塞进**一个** VPK，并且让多个假设可以在**不重编**的前提下 A/B。

## 1. 关键设计：所有计数打印两次

`site_ring.log` 里的每一段现在都会出现**两遍**，中间隔 400 ms：

```
===== v02.17 sample 1 =====
  ...
  lines fast=... pump=... switch=...
===== v02.17 sample 2 (+400ms) =====
  ...
```

看的是 **delta**，不是绝对值。log36..log41 六轮都卡在同一个歧义上：
单个冻结样本无法区分"死锁"和"只是很慢"。现在：

- 两个样本**完全相同** → 该路径彻底停了。
- `switch` / `loops` / `ticks` 在涨而 `pump` / `ttk` 不动 → 有东西在跑，但
  不是有用的调度。
- `ticker loops` 不涨 → **ticker 线程本身没被调度**（这就直接检验 v02.14
  把 ticker 钉到 VM 核上的那次改动，也是 log40/log41 "cpu1 scheduler DEAD"
  的直接读数）。

## 2. 新增读段

| 读段 | 回答什么问题 |
|---|---|
| `ticker loops=/tick_count=/last_ms=/ago_ms=/cpu_cfg=/tickgate=/tick_compile=` | ticker 还活着吗？钉在哪个核？两个开关现在是什么值？ |
| `boot run=/sched_start=/th_start=/th_finish=/lw_exit=/start0=/justborn=` | 生命周期走到哪一步断的。"创建了但没被调度"（`th_start` 涨、`justborn` 不涨）与"调度了但没结束"（`justborn` 涨、`lw_exit` 不涨）现在是两个不同的数字。 |
| `last 8 java threads created:` | 创建/死亡自旋会**重复打印同一对** `Class.run` → 直接点名是哪段 Java 代码，不需要（永远拿不到的）Java 堆。 |
| `nrt cell=/obj=` + `nrt id=/pri=/st=/jsp=/wait=` | 调度器缓存的"下一个该跑谁"，以及那个线程的状态。 |
| `dispatch ring (last 16, newest first):` | 冻结前调度器在哪些线程之间轮转（含 `TERM`/`NEW`/`DEAD`/`INA` 与 `pe=` 待执行入口标志）。固定轮转 vs 同一个 id 反复出现，含义完全不同。 |
| `java-stack-top (current):` | 当前线程 Java 栈顶 16 个字。coredump **只含 native 栈**，所以 `setup_stack_asm` 建的引导帧（fp=0、`start_lightweight_thread_asm`、`TestCompiler`）此前从未可观测。栈顶还符合模板 ⇒ 这个线程**从没进过 Java**。 |
| `threads(active-list)` / `threads(priority-queues)` | 同 v01.98（`CYCLE(n)` 是 v02.11 的环首复访探测器输出，phoneME 优先级队列**本来就是环形链表**，不是损坏）。 |

## 3. 运行期 A/B 开关（不需要重编）

放在 `ux0:/data/J2ME00001/` 下，**文件不存在 = 行为与 v02.16 完全一致**：

| 文件名 | 内容 | 含义 |
|---|---|---|
| `tickgate` | `1` | ticker 超过 1 s 没刷新（VM 独占 CPU）时，**跳过** tick 触发的 JIT 编译 |
| `tickgate` | `2` | 完全不在 tick 上编译（等价于 `tick_compile.cfg` 写 `0`） |
| `tickgate` | `0` / 不存在 | 不加门（v02.11 行为） |
| `ticker_cpu` | `0` | ticker 用全部用户核（pre-v02.14 行为） |
| `ticker_cpu` | `1` | ticker 钉 cpu1 |
| `ticker_cpu` | `2` | ticker 钉 cpu2 |
| `ticker_cpu` | 不存在 | 保持 v02.14：与 VM 同核 |
| `tick_compile.cfg` | `0` | v02.11 开关：完全禁止 tick 编译（语义未变） |

样例文件在本目录：`tickgate.1` / `tickgate.2` / `ticker_cpu.0` / `ticker_cpu.1` /
`ticker_cpu.2`。复制到真机时**去掉扩展名**：

```
# 例如：把 ticker 从 VM 核挪到 cpu2
cp ticker_cpu.2 /mnt/ux0/data/J2ME00001/ticker_cpu
```

## 4. 建议的一轮跑法（一个 VPK 回答多个问题）

**先澄清一件事：`launch.cfg` 不是必需的。** 没有它时 launcher 用内置默认
（`Hello.jar` / `ToneTest` / `portrait`），走的是**完全同一条**冻结路径。
历史日志里那句 `launch.cfg not found, defaults in use` 是**正常输出**，
不是故障——它只表示没人在卡上放过手动配置。

`launch.cfg` 唯一的用处是里面那一行 `menu=0`：让启动**跳过原生菜单**。
这只省掉每轮一次手点，属于可选便利；不加就是照旧先点进菜单再选游戏，
冻结一样复现。

**同理，下面的运行期开关（`tickgate` / `ticker_cpu`）与 `launch.cfg` 没有任何
关系。** 它们在 `OS_vita.cpp` 里是硬编码绝对路径：

```c
snprintf(path, sizeof(path), "ux0:/data/J2ME00001/%s", name);
```

不经过 `chdir()`、不读 `launch.cfg`、不看菜单选了什么。唯一的要求是
**文件必须在应用启动前就躺在卡上**——这些开关每个进程只读一次并缓存
（`vita_cfg_flag` 有按名缓存，tick 门有 `_vita_tickgate_mode` 一次性缓存），
运行中再放进去是没有用的。

1. **第一轮：什么都不放**（连 `launch.cfg` 都可以不放）。拿到双采样基线，
   确认冻结是否仍在 `start_lightweight_thread_asm` 附近。
2. **第二轮：`ticker_cpu.2`**（ticker 挪离 VM 核）。若冻结消失 ⇒
   v02.14 的"同核"就是原因；若 `ticker loops` 现在仍在涨而 VM 死不复活 ⇒
   ticker 与冻结无关，方向转 `tickgate`。
3. **第三轮：`tickgate.2`**（完全不在 tick 上编译）。这是 v02.11 那个
   从未真正跑过的假设。

每轮只需换 `ux0:` 上的小文件 + 重开应用，不需要重装 VPK。

## 5. v02.18 新增：`vm_cpu` 开关 + 内核态取证

### `vm_cpu`：运行时钉核开关（v02.12~v02.17 的钉核改为可选）

v02.18 起**默认不钉核**（v01.x~v02.11 的原始行为；log1~36 的卡死全部发生
在不钉核下，v02.12~02.17 的钉核实验证明钉不钉都卡，停摆核永远跟随 VM 所
在核——钉核是诊断对照，不是起因）。要复现 v02.13~v02.17 的"钉 CPU1"行为，
放 `vm_cpu.1`：

| 文件（去扩展名放到 `ux0:/data/J2ME00001/`） | 效果 |
|---|---|
| `vm_cpu.0` | 显式不钉（同默认） |
| `vm_cpu.1` | 钉 CPU1（= v02.13~v02.17 行为） |
| `vm_cpu.2` | 钉 CPU2 |
| 不放 / 畸形内容 | 不钉（编译默认） |

判读：启动日志 `vm cpu:` 行——`unpinned (mask=0, kernel default 3-core)`
或 `pin vm cpu mask=0x... rc=0x...`。

### kubridge 内核态取证（第三类证据）

VPK 根部已捆绑 `kubridge.skprx`。**打包 ≠ 加载**——要真正读到内核，需
一次性部署插件：

1. 从 VPK 里解出 `kubridge.skprx`（Vita3K 直接浏览 VPK 内容；真机用
   FTP/VitaShell 复制），放到 `ux0:/tai/kubridge.skprx`。
2. 编辑 `ux0:/tai/config.txt`，在 `*KERNEL` 段加一行：
   `*KERNEL` 换行 `ux0:tai/kubridge.skprx`（若已有 `*KERNEL` 段则在段内
   追加该行）。
3. 重启主机。

**没装插件的机器也能正常跑**：HANG 时 `kernel_forensics.log` 只写一行
`kubridge: ABSENT (kernel probes skipped)` + 部署提示，其余全部取证照旧。

### v02.18 一轮判读表（三类证据一次拿全）

| 证据 | 文件/日志段 | 关键行 | 说明 |
|---|---|---|---|
| (a) 不钉核下停摆核 | `vm_boot.log` + watchdog 快照 sysinfo | `vm cpu: unpinned` + 停摆核号 | 回答"停摆核是否仍跟随 VM" |
| (b) 完整调度器双采样 | watchdog 快照 6 段 `[rdN ... ok]` | `[rd1 dispatch ok]` … `[rd6 dumpthreads ok]` | 缺哪段标记=截断在哪段，rd1/rd2 最重要（log42 就死在它们前面） |
| (c) 内核态 | `kernel_forensics.log` | `kubridge: present` + `core0..3: cur_tid=...` 双采样 | present 但全 0/skip=偏移不对；ABSENT=插件未部署 |

### log42 教训（探针自伤修复）

log42 的快照死在 `vita_sched_nrt_info` 内部——watchdog 线程没有 GC 安全帧，
`priority()` 链式解引用 stale thread_obj 触发 data abort，进程死亡吃掉了
sample 2 + dispatch ring + java-stack-top。v02.18 修复：

- `nrt obj=... id=... st=... tobj=0x...`：全部改为 `Thread::*_offset()`
  编译期偏移 + 裸 volatile 字读，**不再解引用任何对象方法**；
- 快照段重排：dispatch ring（rd1）与 java-stack-top（rd2）**提前**，
  探针自伤也吃不掉它们；
- 每段落尾 `[rdN ... ok]` 标记：截断文件能直接点名死在哪段。
