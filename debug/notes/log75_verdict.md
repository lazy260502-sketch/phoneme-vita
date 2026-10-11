# log75 判决 + log74 判决修正（v02.51 首跑）

版本：v02.51 b455 (5a5b109)。文件：boot_log / net_log / psp2core（无
ring_trap.log、无 watchdog.log、无 midp_*）。

## 一、三大新事实

### 1. magic 缺席 → v02.51 trip 路径从未执行

全 psp2dmp（含必然 dump 的 256K 主栈页 0x81541000）搜 `0x56353144`
（'V51D'）零命中。迷你现场写在 trip 路径最前（先于门判定）→
**flood 门根本没 trip 过**。刹车没开、park 没发生；崩溃又一次快过
watchdog 250ms 轮询。

### 2. slide 修正 → 退出表两轮都是完好真模板（★推翻 log74 坏门判决）

log75 slide：text **+0x18000**、data **+0x20000**（锚点：pad 静态
8109b850→[sp-4]=0x810b3850 ✓；gp 0x812e1924→r5=0x81301924 ✓。
slide 每轮漂移，**必须每轮重锚定**）。据此判 log75 表：

| 槽 | 运行期 | 静态 | 归属 |
|---|---|---|---|
| [r6-4] | 810b3850 | 8109b850 | pad（sentinel）✓ |
| [r6] | 810b53b0 | 8109d3b0 | **lw_exit** ✓ |
| [r6+4] | 810b3b74 | 8109bb74 | uncaught_exception ✓ |
| [r6+8] | 810b3ab4 | 8109bab4 | finish ✓ |
| [r6+c] | 810b3a80 | 8109ba80 | force_terminated ✓ |
| [r6+10] | 0 | — | TestCompiler=NULL ✓ |

**完璧出生模板。** 用同标准重判 log74（slide +0x50000、v02.50 静态）：
[r6]=0x810ed320-0x50000=0x8109d320=**lw_exit（v02.50 nm 确认）** ✓，
[r6+4..c]=pad+0x2c4/+0x204/+0x1d0=uncaught/finish/force ✓。
**log74 的门也是好的。** 上轮"门装着堆对象 0x81edc2d4"是 slide
算术错误（把 r0 的值误当 [r6]）。0x81edc2d4 一直是 **r0**，不是门。
v02.51 的门校验因此是无害加固（verdict 会=1），但**不是崩溃机制**。

### 3. 崩溃确定论：两轮堆内偏移逐位一致

| | log74 | log75 |
|---|---|---|
| 堆基址 | 0x81700000 | 0x81600000 |
| r0 | +0x7dc2d4 | +0x7dc2d4（**同**） |
| pc=lr | +0x7e0dd8 | +0x7e0dcc（Δ0xC） |

同一确定性路径，与 v02.50/v02.51 代码差异无关。

## 二、修正后的崩溃机制

thr0（J2ME00001）：pc=lr=0x81de0dcc（堆内全零页，XN abort）、fp=0、
sp=0x8157fd88（原始 C 栈）、r5=gp、r6=0x81ddf378（堆内 Java 栈顶，
模板完好）、r7=0x81ddd2d0（堆帧，自链表结构）、r8=0x811a9430=
**_rom_text_block7+0xC（ROM 化方法字节码）**、r9=0、r10=0x81ddd29c。

**[sp-0x1c..-0x8] = r4,r5,r6,r7,r8,r9 当前值逐位溢出** + [sp-4]=pad
sentinel = 解释器寄存器文件现场。pc=lr + fp=0 的签名 =
**resume 型 load-jump 摆件**（`ldr lr,[mem]; ldr fp,[mem]; mov pc,lr`
——出生尾/context-switch 恢复同类）**吃进了垃圾恢复地址**（源词在
64MB 未 dump 堆区，全 dump 搜 0x81de0dcc 零命中可证）。r7/r8 表明
派发正在装入一个 ROM 化方法时恢复跳转进了堆。

r0..pc 区间 4799 词 57% 零、2056 非零——**活堆对象区，非 NOP 滑橇**；
崩点是该路径上首个取指 abort（数据页 XN）。

## 三、洋葱新层：杀手边不在 pad↔stub 环里

log73（v02.49 PARK）证明幽灵有时在环里弹跳（pad 可抓）；log74/75
证明崩溃路径**可以完全不经过 pad**（无 trip、无 magic）——调度器对
never-born 幽灵的派发/resume 直接把垃圾当恢复 PC。**flood 门守在了
错误的边上**：v02.45 以来的 pad 侧仪器看不到这条边。

## 四、v02.52 方向（未实施）

调度器侧（fresh C++，可改）：
1. switch/get_next_runnable 派发前置校验（ghost-filter）：候选线程的
   stack_pointer 落在合法 Java 栈区？pe/state 合法？不合法 → 记录 +
   拒绝派发（把崩溃变成可抓的 dump）；
2. 派发取证：记录每次 switch_to 候选的 {id, stack_pointer, pe, state}；
3. 根因：notify_wakeup 为何 ADD entry-less 线程（log73 caller 待归属）。

## 五、教训入库

1. **slide 每轮漂移**（log74 text+0x50000 / log75 +0x18000）——符号化
   必须每轮用双锚点重算，禁止沿用上轮值；
2. **"r0 的值"≠"[r6] 的值"**——log74 误判源于此；判读表内容必须
   直接读内存而不是从寄存器推断；
3. psp2dmp 只带 fault 页+栈页——64MB 堆仅 28KB 在 dump 里，垃圾源词
   不可寻；只能靠机制签名（pc=lr+fp=0+寄存器溢出布局）定谳。
