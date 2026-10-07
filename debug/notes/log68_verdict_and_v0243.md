# log68 判决 + v02.43 发货（SEL 探针修复 + libpng 恢复）

- 日期：2026-10-07
- 样本：log68（v02.42 真机跑批，仍冻结）
- 产物：`vita-port/out/vpk/midp_vita_v02.43.vpk`，md5 `133206ba046921a41379c4f4f64cab14`
- 提交：`fa8059a`（单仓库 samples：Scheduler.cpp + CMakeLists.txt）

## 1. log68 判决：空蹦床弹跳（empty-trampoline bouncing）

四源交叉（watchdog 快照 / site_ring / qring / coredump）：

| 证据 | 数值 | 含义 |
|---|---|---|
| pe0 计数 | 187,698,350 → 191,800,352（400ms） | ~10M/s 疯涨 = 弹跳线程反复过垫 |
| pe1 计数 | 5（冻结） | 带 C++ 调用链的完整蹦床从未发生 |
| v240 land ≈ pe0+pe1 | — | 垫两条路径合计，自洽 |
| **v240_fp** | **0x00000000** | 弹跳线程 fp 是出生帧模板值（未建立 C++ 帧） |
| qring seq | 591 → 591（两样本冻结） | 自旋不产生任何 C++ 队列操作 |
| coredump pc/lr | `invoke_pending_entries+0` / `vita_landing_pad+0` | 冻结在桩入口边沿（注意 pc 可能是 dump 伪影） |
| coredump r1/fp | r1=0(pe) / fp=0 | 同上，出生帧状态 |
| r0 | 0x81ecf79c（id4 线程对象） | 冻结时的"当前线程" |
| v231 finish/force/term/exit_seq | 全 0 | 弹跳从未走到蹦床的 C++ 退出链 |
| v230 lw_exit=0, th_start=4/th_finish=0 | — | 弹床从未被完整走完 |
| id=4 Java 栈 | `inflate+0x173` + `L318+8` + `invoke5_deoptimization_entry_0`（jsp+48） | 冻结时 id4 在 JAR 解压 native 帧 |

**结论**：活锁 = pe0-only 空弹跳。某线程以 fp=0（出生帧）状态反复过垫走 pe0 路径，
从未进入蹦床 C++ 链（pe1=0 增长），也不产生调度器队列操作（seq 冻结）。
"当前线程"（_current_thread=id4）有正常执行栈（inflate）——**弹跳者与当前线程身份不同步**是核心谜题。

## 2. SEL 探针 bug 定性与修复

**现象**：log68 qring 尾部 SEL 记录 id 全为 0xE5902014（=ARM 指令 `ldr r2,[r0,#20]` 编码），ADD/REM/NRT 的 id 1-4 可读。

**机制**（双解引用）：
```
VITA_Q_REC((Thread *)thread.obj(), ...)   // v02.42 写法
  → 宏内 qp_ = (Thread*)ThreadObj指针
  → qp_->id() = int_field(id_offset) = *(qp_->_obj + off)
  → 但 obj() 返回的已是 ThreadObj*，被当作 _obj 再解一次
  → 实际读 *(klass指针 + id_offset) = ROM 代码区内容
```
- SEL 站点 `thread` 是 `Thread::Raw` 局部 handle（非 `Thread*`）；ADD/REM 站点是 `Thread*` 参数直传（正确）
- `ENABLE_OOP_TAG=0`（jvmconfig.h:873）排除 tag 假说
- 0xE5902014 = *(klass+id_offset) 恰是 ROM 里一条 ARM 指令字

**修复**（v02.43）：`VITA_Q_REC((Thread *)&thread, 3, priority, ...)`——取 Raw handle 地址强转。
PRODUCT 下 `BasicOop` 无虚表、首 word 即 `OopDesc* _obj`，与 `Thread*` 布局同构；
`qp_->_obj` = ThreadObj*，`id()` 读真实字段，与 ADD/REM 语义严格一致。
同函数先例：`return (Thread *)&_next_runnable_thread;`。

**队列本身没坏**：真实调度历史可从 NRT 序列读出；SEL 序列 log69 起首次可读。

## 3. libpng A/B 正式闭合

v02.42 携带 `VITA_NO_PNG` 且 `[AUDIT] png calls=0`，仍冻结 → **排除**。
v02.43 恢复：CMake 版本块不定义 `VITA_NO_PNG`，`vita_icon.c` 的 `#if defined(VITA_NO_PNG)` 守卫自动走全解码器路径（该文件零改动）。

## 4. v02.43 锚点（与 v02.42 一致，布局未漂移）

| 锚点 | 地址 |
|---|---|
| 垫（landing pad） | 0x8109a5a0 |
| invoke_pending_entries（桩） | 0x810c257c |
| 蹦床（trampoline） | 0x810c2854 |
| primordial_to_current_thread | 0x810c2830 |
| _current_thread | 0x812e1d30 |
| [r5,#0x40c] 基址字 | 0x810c28d4 = 0x812e1924 |
| _vita_q_seq / _vita_q_ring | 0x814b4d30 / 0x814b4d34 |
| _vita_disp_ring | 0x814b5268 |
| L318 / L444 | 0x810c0d64 / 0x810c28b4 |

## 5. log69 判读要点

1. **SEL 序列首次可读**：qring 尾部应出现 id∈{1..4} 的 SEL（op=3）记录，与 ADD/REM/NRT 交错——重建冻结前完整调度史
2. **确认弹跳者身份**：SEL pe 列 + disp ring 对照，看谁在被反复选择（预期 pe=0）
3. 垫双槽计数器（v242_pe0/pe1）+ v240_fp 继续观察：弹跳者 fp 是否仍为 0
4. 若 pc 仍落在 stub 入口：结合 SEL 历史判断是"选择后未执行"还是"执行完未出队"
