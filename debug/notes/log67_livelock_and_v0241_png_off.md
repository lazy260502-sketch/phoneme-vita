# log67 判读与 v02.41 libpng 屏蔽实验（活锁判决）

> 会话日期：2026-10-05 ~ 10-07；上一章：v02.40 A 方案（见 PROJECT_MEMORY.md §"v02.40 A 方案（C 着陆垫）完整落地记录"）

## 0. TL;DR

1. **log67 判决（推翻 log66"首次派发即冻结"）**：真机冻结是**绿线程调度器活锁**——垫↔蹦床纯 asm 环每秒 ~1450 万次空转（pe=0 pending entries 永不消费），旋转数秒后 CPU 真丢（census runClocks d=0 + KICK5 跨核拒）。log64-66 的"蹦床半执行"实为旋转中途丢 CPU 的快照。
2. **v02.41 已发货**：`VITA_NO_PNG` 编译期总开关屏蔽 libpng（唯一运行时漏斗 `vita_icon_decode_png` 入口早退 -1）。A/B 实验：若冻结消失 → libpng 堆破坏实锤；若仍冻结 → 活锁与 libpng 无关，下一嫌疑 = 优先队列成环本身。

## 1. log67 证据链（v02.40 正确锚点，slide=0x5F000）

| 证据 | 读数 | 含义 |
|---|---|---|
| 冻结 pc | `vita_landing_pad+0x18`（movt r0,#0x814b，垫内第 7 条） | **首次冻结在新代码内**——六轮蹦床头冻结被打破 |
| lr / fp | pad 地址 / 0 | 垫 ABI 与设计完全一致（探针 fp=0 符合预期） |
| r0 | 0x00004b14（movw 已执行、movt 未执行） | 半装载铁证：CPU 在指令间丢失 |
| [sp] | `JVM::start+0x18c` | 经典首轮出生冻结栈 |
| **land 计数**（site_ring.log 双快照 L99/L168） | **303,949,169 → 309,758,141（~400ms 内 +5,808,972）** | 垫处于热旋转，~14.5M 次/秒 |
| dispatch ring | 16 条全 `id=3 pri=5 st=0 pe=0` | 线程 3 反复派发、pending entries 永不消费 |
| 优先队列 | `q5: t0x81eddf9c(id=4) t0x81edc2d4(id=3) CYCLE(2)` | **队列成环**——调度器级异常 |
| v230 | born_dsp=4 ts_enter=0 ts_gadget=0 | 出生派发 4 次后不再进 enter |
| boot | th_start=4 th_finish=0 lw_exit=0 | 启动线程从未 finish |
| switch | 185,015（静止） | land=3 亿 vs switch=18 万——**旋转不经过 C++ 调度器**，纯 asm 环 |
| watchdog | HANG ce=404、VMPROG exec=0 tick=1、vm RUNNING wait=none runClocks d=0、KICK5 0x80020008 跨核拒、sysinfo 全核 idle 增长（冻结后） | 旋转停止后才真丢 CPU |
| [AUDIT] | png_read_image calls=5（菜单阶段，菜单退出前完成） | libpng 只在菜单期执行 |

**关键文件去向**：v230/v231/v240 探针行在 **site_ring.log**（非 watchdog.log）；psp2core-1791338132 dump 的 LOAD 段**不含探针 .bss**（0x814xxxxx），探针值只能从 site_ring.log 读。

**机制图景**：非"首次派发即冻结"，而是 id=3/4 双 pri=5 在优先队列成环（CYCLE 检测已触发），垫↔蹦床空转数秒 → CPU 丢失。冻结时 pc 停在垫+0x18 只是旋转环上随机一帧。

## 2. v02.41 改动（提交 d48161a，2 文件 +36 行）

1. **`vita-port/src/vita_icon.c`**：入口 `#if defined(VITA_NO_PNG)` 早退桩（返回 -1），`#else` 保留完整解码器。跳过 png_sig_cmp 及其后一切 libpng 调用（log27 曾抓到 pc 在 png_do_rgb_to_gray——要屏蔽就彻底）。
2. **`vita-port/CMakeLists.txt`**：
   - 新增 v02.41 版本块（line 1224，02.40 块之后——last-set-wins）
   - 版本块尾部 `set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DVITA_NO_PNG")`——**追加式**，与 1785 行旗标追加共存（非覆盖，勿改成不带 += 语义的直觉判断；CMake 的 set 字符串拼接语义在此正确）

### libpng 运行时足迹（本轮普查结论，CMakeLists 1976-1979 注释再确认）

- **唯一运行时漏斗** = `vita_icon_decode_png`（vita_menu.c L1149 jar 内图标 / L1305 cache.bin 图标，两处 caller 均已处理失败为纯文本行）
- MIDP 皮肤 PNG 不走 libpng（构建期 flatten 进 VPK 的 data/J2ME00001/lib/，Java 侧 imgdcd 解码）
- `--wrap=png_read_image` 只是计数器（vita_thread_audit.c），保留无害
- cache.bin 仍存原始 PNG 字节，仅跳过解码——**注意：cache.bin 不需要清**，图标只是不显示

## 3. v02.41 锚点（判读基准）

| 锚点 | 地址 |
|---|---|
| 垫 `vita_landing_pad` | **0x8109a270**（v02.40 -0x288 平移） |
| 蹦床 `start_lightweight_thread_asm` | **0x810c24f4** |
| 探针 `vita_v240_lr/fp/land` | **0x814b4b10/14/18**（.bss 不平移） |
| `vita_icon_decode_png` 桩 | 0x8100713c（`mov.w r0,#-1; bx lr` 三条指令） |
| 启动器符号数 | 128（`nm | grep -cE "T (__wrap_\|vita_)"`） |
| vpk md5 | e6844e2f3f2dfdca602397289047e4d1 |

垫反汇编与 v02.40 逐指令一致（探针地址不变，冻结成员原样）。

## 4. 判读矩阵（v02.41 冻结时）

| 结果 | 判定 |
|---|---|
| 菜单图标消失 + `[AUDIT] png_read_image calls=0` | 屏蔽生效自检（两个运行时铁证） |
| 冻结消失（能正常进游戏） | **libpng 堆破坏实锤**——回补：修 v01.78g 同族 bug（rowbytes 假设）或永久禁用 |
| 仍冻结 + land 仍热旋转 + 队列仍 CYCLE | 活锁与 libpng 无关；下一嫌疑 = **优先队列成环本身**（谁把 id=3/4 双双挂回 q5？查 Scheduler 挂回路径 / resume 延迟入队 /抢占式 reschedule 的重入） |
| 仍冻结但形态不同（land 静止/pc 移动） | libpng 影响时序但不改根因；按新形态重新归档 |

## 5. 环境备忘（本轮再确认）

- `vitasdk.sh` 不存在于 /home/zyb/vitasdk/（环境用 `export VITASDK=/home/zyb/.local/vitasdk && export PATH=$VITASDK/bin:$PATH`）
- 正确构建树纪律不变：`vita-port` 内 cmake+make（`midp-vita/build_midp_vita.sh` 永久废弃）
- Hello.jar 存货校验（boot_log expected 9203）继续适用

## 6. 遗留

- 队列 CYCLE 成因未查（Scheduler 挂回路径）——若实验结果指向它，这是下轮主战场
- `--allow-multiple-definition` 仍在链接线（继承自 v02.40 §6）
- libpng 屏蔽为**暂时**措施，实验结束后按结果回补（修 bug 或改永久配置项）
