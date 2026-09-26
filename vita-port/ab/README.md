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

1. **第一轮：什么都不放**（只用 `launch.cfg.hello`）。拿到双采样基线，
   确认冻结是否仍在 `start_lightweight_thread_asm` 附近。
2. **第二轮：`ticker_cpu.2`**（ticker 挪离 VM 核）。若冻结消失 ⇒
   v02.14 的"同核"就是原因；若 `ticker loops` 现在仍在涨而 VM 死不复活 ⇒
   ticker 与冻结无关，方向转 `tickgate`。
3. **第三轮：`tickgate.2`**（完全不在 tick 上编译）。这是 v02.11 那个
   从未真正跑过的假设（日志里 `launch.cfg not found, defaults in use`，
   说明历史各轮**都没有**真正触发过 A/B）。

每轮只需换 `ux0:` 上的小文件 + 重开应用，不需要重装 VPK。
