# log69 判决 + v02.44 发货（出生帧解剖探针 + PNG 真恢复）

日期：2026-10-08
VPK：`vita-port/out/vpk/midp_vita_v02.44.vpk` md5 `6cbfe4350e13f5ef79bee8694d052355`
版本串：`J2ME Player v02.44 b434 (63763ff)`（commit 63763ff）

## 一、log69 判决（v02.43 真机跑批，仍冻结——活锁同型）

### 1. SEL 修复生效（v02.43 唯一改动命中）
qring SEL id 首次可读：尾部（旧→新）`id3 SEL×14, id2 ADD, id2 SEL/NRT, id3
SEL/NRT, id2 SEL/NRT, id1 ADD/REM/ADD, id2 REM, id1 SEL/NRT, id4 ADD, id3
SEL/NRT, id4 SEL, id4 NRT`（seq=549 两样本冻结）。全部记录 pe=0。

### 2. coredump 采样点运气好：pc=出生桩第 3 条指令
- `pc=0x8111585c`（静态 0x810c285c = **start_lightweight_thread_asm+0x8**，
  即 `ldr r1,[r0,#0x68]` 读 status）
- `lr=0x810ed5a0`（pad+0）、`fp=0`、`r0=id4 ThreadObj`、`r6=0x81ed0804`
- 与 log68 的 stub+0 采样是**同一环的两个采样点**（一圈 later）

### 3. 弹跳者身份 = id4 本人
r0=id4 ThreadObj + _current_thread=id4 + fp=0 出生模板 → 弹跳者就是 id4（当前
线程），带着出生帧模板在 垫→出生桩 间循环。

### 4. 核心谜题：r6 与 stack_pointer 差 0xBC
id4 的 `stack_pointer()=0x81ed0748`（rd2），但冻结时 r6=0x81ed0804，差
**0xBC**。jsp 既不在出生帧位，也不等于 ThreadObj 保存值（primordial_to_
current_thread 会从 [r1,#0x64] 恢复 0x81ed0748）→ 进入路径不是正常的 jsp
恢复，疑似解释器 unwind 直接撞上哨兵字。

### 5. 语义修正（重要！改变 [r6+16] 的判读）
出生帧 jsp+16 槽写的是 **TestCompiler 旗标值（0/1）**，不是函数地址！
`*xsp = TestCompiler`（setup_stack_asm）是 bool→jint 隐式转换。且本 PRODUCT
构建 ENABLE_PERFORMANCE_COUNTERS=0 → TestCompiler 降级 develop 旗标 →
`const bool TestCompiler = false`（编译期常量，已用预处理器实证）。
因此：
- 模板 jsp+16 = **0**
- 出生桩 `ldr r0,[r6,#16]; cmp; bne L444` 是 "flag≠0 才跳过退出链"
- 冻结时帧内 [+16] 若非 0 = 残留垃圾把桩直接导向 L444

### 6. 全部 C++ 计数器冻结 = 纯 asm 环
v242 pe0 ~10M/s、pe1=5 冻结；dispatch ring 平；v230 lw_exit=0；v231 全 0。
出生桩每条前进路径 2-3 条指令内必进 C++，但 C++ 计数器全 0 → 环在纯 asm
闭合。唯一无探针的 suspect 边：退出表槽位内容损坏（[r6+16]≠0 走 L444、
[r6] 恰为垫地址则闭环成立，~28 指令/圈 ≈ 10M/s 吻合）。

### 7. dump 局限
psp2dmp 不含 Java 栈页（0x81ed0xxx 处 N/A）→ 出生帧运行时内容必须由
watchdog 进程内 dump（v02.44 rd7 的核心动机）。

## 二、PNG 恢复失败根因（v02.43 的失误）与真修复

- 根因：`CMakeLists.txt:1245`（v02.41 块）`set(CMAKE_C_FLAGS
  "${CMAKE_C_FLAGS} -DVITA_NO_PNG")` 是自引用**累积追加**。v02.43 块"不再
  设置"≠撤销，宏持续生效。log69 证据：`[icon] cached png decode FAILED` ×5
  + `[AUDIT] png_read_image calls=0`。
- 修复：v02.44 块 `string(REPLACE "-DVITA_NO_PNG" "" CMAKE_C_FLAGS
  "${CMAKE_C_FLAGS}")`（注意 `${CMAKE_C_FLAGS}` 必须带引号，否则多词展开成
  list 以 `;` 重连污染 flags）。
- 二进制级验证（本次做到位）：`vita_icon_decode_png` 反汇编为真实函数体
  （png_sig_cmp 调用链），非 `mov.w r0,#-1; bx lr` 桩。

## 三、v02.44 探针

1. **垫记 r2-ct + r6**（InterpreterRuntime_arm.cpp → _MergedSrc004.o）：
   垫 pe 计数后 9 条指令：movw/movt r2 → vita_v244_ct；重载 _current_thread
   → str r1,[r2]；movw/movt r2 → vita_v244_r6；str r6,[r2]。r1/r2 均死寄存
   器（r0 此刻是 &vita_v242_pe，需重载 ct）。
2. **watchdog rd7**（OS_vita.cpp → OS_vita.o）：读 v244_ct/r6，dump
   `[r6-8..r6+16]` 7+ 字，对照模板静态地址 + `ct.stack_pointer`；两样本
   r6 差值暴露 call_on_primordial_stack 的 `stmdb r6!,{lr,fp}` 递减指纹
   （每圈 r6-=8，非返回路径不恢复）。
3. 验证链：nm 见 T pad/B v244_ct(0x68)/B v244_r6(0x6c)（_MergedSrc004.o）
   + U 引用（OS_vita.o）；垫反汇编 9 条新指令（+0x58..+0x78）；R_ARM_MOVW/
   MOVT_ABS 重定位绑定正确；VPK 内 rd7 格式串齐全。

## 四、log70 判读顺序

1. rd7 frame words **第一优先**：[+16]≠0 → 帧已损坏（垃圾导向 L444）；
   [r6]=垫地址 → 闭环实锤。
2. v244_ct vs dispatch ring 的 id：确认弹跳者身份。
3. 两样本 r6 差值：-8/circle → call_on_primordial_stack 递减指纹。
4. PNG：LiveArea 图标应恢复显示（本次二进制级已验证）。
