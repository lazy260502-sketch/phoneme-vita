# log68 预备：v02.42 队列操作 forensics 包

日期：2026-10-07（v02.41 发布次日 / log67 判读后）
包：`vita-port/out/vpk/midp_vita_v02.42.vpk`（md5 `dbb85acf66b72e1aee0706a5b346c993`，4.6MB）
提交：phoneme-cldc `3be09f5`（3 文件 +150）、vita-port `7f71d3d`（CMake 版本块）
前置阅读：`log67_livelock_and_v0241_png_off.md`

## 定位

log67 判决：活锁是**纯 asm 闭环**（解释器 unwind → 哨兵 → 垫 → 蹦床 → invoke_pending_entries 桩 → 桩 pe==NULL `moveq pc,lr` 立即返回 → 再 unwind），switch/lw_exit/ts_gadget 三探针全静止——环内无任何 C++ 检查点。因此**最后一批 C++ 队列操作被冻结在历史里**：谁把 pe=0 线程反复挂回队列、谁反复选中它，是唯一可扩展的成因侧观测面。

另实锤 v02.30 死探针：线上 invoke_pending_entries 桩（810c221c）反汇编无计数代码——生成器侧加的 `_vita_pe_enter` 被冻结的 Interpreter_arm.o 吞掉，永零。**活探针只能放 fresh 编译单元**。v02.42 用垫（_MergedSrc004，fresh）读 `_current_thread->pending_entries` 做 pe 存活计数，从冻结对象外部替代死探针。

## 三类成因侧探针

### (a) 队列操作 ring（Scheduler.cpp，_vita_q_ring）

64 条 (id, op, pri, pe)，六类插桩点：

| op | 语义 | 站点 |
|----|------|------|
| 1 ADD | 线程加入优先队列 | add_to_active 加分支尾 |
| 2 REM | 线程移出优先队列 | remove_from_active 减分支尾 |
| 3 SEL | 调度选点（yield 旋转后缓存） | get_next_runnable_thread 的 `_next_runnable_thread = thread.obj()` 后 |
| 4 NRT | nrt 缓存写入（新当前线程确立） | JVM_TimeSlice + slave_mode/master_mode 三处 set_current 后 |
| 5 PRI | 优先级变动迁移 | set_priority 尾 set_priority_valid 后 |
| 6 JBC | just-born 清 nrt 缓存 | switch_thread JUST_BORN 分支清 nrt 后 |

watchdog dump 为 `[rd0 qring]` 分节，输出在 dispatch ring **之前**（成因先于后果）。newest-first 半环 32 条 + 总 seq。

### (b) 垫 pe 存活计数（InterpreterRuntime_arm.cpp，vita_v242_pe[2]）

垫在每次穿越时读 `_current_thread`（GP 表首槽 0x812e1d30，与 v02.41 一致）+ offset 0x24 的 pe 字段：

- **pe0**（slot0）= pe==NULL 穿越数
- **pe1**（slot1）= pe!=NULL 穿越数

判读：**pe0 涨 pe1 平 = 空蹦床弹跳**（线程序在但没 body）；**pe1 也涨 = Java body 在场但从未被消费**。与 v240 land 计数（同一垫的穿越总数）对账：land == pe0+pe1。

### (c) 死探针教训沉淀

`_vita_pe_enter`(v02.30) 在生成器侧（InterpreterStubs_arm.cpp/SharedStubs_arm.cpp）加的计数被 loopgen 冻结对象吞掉。**任何针对桩行为的探针必须放 fresh 编译单元**（垫是唯一入口侧 fresh 点）。

## 判读矩阵（log68 分析入口）

1. **元凶站点判定**：qring 里任何 op 作用于 pe=0 线程 = 该站点把无 body 线程挂回/移出/选中。反复 ADD pe=0 → add_to_active 是挂回点；SEL/NRT 作用于 pe=0 → 调度在消费空线程。
2. **旋转时长**：最后一条 qring 条目与 hang 之间的 seq 差 = 纯 asm 旋转持续时间（对照 v231 cpu1 busy 时长）。
3. **pe 分流**：pe0 单独涨 = 空弹跳；pe1 也涨 = body 在场。交叉对账 v240 land。
4. **环成员确认**：dispatch ring（id=3 pe=0 ×16）与 qring 的 id 集合对照，确认环成员是否在队列里被反复操作。

## 二进制锚点（v02.42.sym）

- 垫 `vita_landing_pad` @ **0x8109a5a0**（v02.41 0x8109a270 平移 +0x330）
- 垫尾 `bx r0` → 蹦床 0x810c2854
- `_current_thread` @ **0x812e1d30**（GP 表首槽，未平移）
- `_vita_q_ring` @ 0x814b4d34（.bss）、`vita_v242_pe` @ 0x814b5718（.bss）、`vita_sched_dump_qring` @ 0x81088078
- 启动器符号 129（128 + dump_qring）

## 注意：v02.42 同时携带两个实验变量

- v02.41 的 `-DVITA_NO_PNG`（libpng 屏蔽）**仍在生效**（02.41 块 set 的 CMAKE_C_FLAGS 持续累加）
- 归因提醒：若 v02.42 直接跑出“不冻结”，无法区分是 libpng 屏蔽还是其他因素；严格的 A/B 应是 v02.41（仅 PNG off）对比 v02.40（PNG on）。若 v02.42 仍冻结，qring+pe 数据与 PNG 变量正交，可直接判读。

## 构建（v02.37 纪律复用）

```
rm phoneme-cldc/build/vita_arm/target/release/_MergedSrc00{2,4}.o OS_vita.o
./rebuild_vm.sh        # loopgen exe 链接失败为历史既有（crt0/fast_memclear），
                       # 33 ARM 对象编译成功即库已重打包；_current_thread 引用由
                       # 库内冻结对象在 vita-port 最终链接解析（已验证）
cmake --build build -j8
```

新教训：垫引用 `_current_thread` 会让 loopgen 宿主 exe 链接新增一条 undefined（历史既有报错之外），但只要库内存在 `D _current_thread` 定义（冻结 Interpreter_arm.o 的 GP 表宿主），vita-port 链接正常闭合——判断标准是最终 VPK 链接，不是 loopgen。
