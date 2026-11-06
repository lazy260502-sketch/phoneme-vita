# vita-port — J2ME/MIDP on PS Vita 平台移植层

本目录是 Vita 平台集成层的**唯一权威源码位置**。phoneme-cldc / phoneme-midp
源码树零改动；通过链接时从 `libobj.a` 删除被取代的成员、再链入本目录实现的方式工作。

开发计划与架构依据见 [PLAN.md](PLAN.md)。

## 结构

```
vita-port/
├── PLAN.md              # 开发计划（阶段、验证清单、风险）
├── build.sh             # 一键构建（./build.sh [clean|midp]）
├── build_jar.sh         # 测试 MIDlet 打包（javac + preverify + jar）
├── CMakeLists.txt       # 链接配置（ar d 删除被取代的 archive 成员）
├── config/              # VPK 内打包的 internal/system.config（虚拟屏 240x320）
├── midlets/             # 测试 MIDlet: HelloMIDlet / CanvasTest / InputTest
└── src/
    ├── vita_main.c      # 启动器（读 launch.cfg 可运行任意游戏 jar）
    ├── vita_display.c   # lfjport 22 符号全覆盖 + 240x320 虚拟屏缩放 blit
    ├── vita_input.c     # checkForSystemSignal 真实现（SceCtrl → MIDP_KEY_EVENT）
    ├── vita_vkey.c      # 屏幕虚拟键盘（黑边数字/动作键 + 左右软键）
    ├── vita_shot.c      # 截屏（SELECT+START / 相机按钮 → PNG + 结果提示条）
    └── vita_pcsl.c      # PCSL 移植层（复制自旧 vita_stubs.c，sceIo 文件 I/O）
```

## 覆盖关系（链接时生效）

| 删除的 libobj.a 成员 | 取代者 | 原因 |
|---|---|---|
| `lfjport_fb_export.o` | `src/vita_display.c` | 旧 `lfjport_refresh` 是空 TODO，渲染链唯一断点 |
| `mastermode_export.o` | `src/vita_input.c` | 旧 `checkForSystemSignal` 是空函数，输入链唯一断点 |
| `runMidlet_md.o` | （`vita_main.c` 提供 main） | 与启动器的 main() 冲突 |

## 构建

```bash
./build.sh          # 使用现有 CLDC/MIDP 库链接 + 打包 VPK
./build.sh midp     # 先重建 phoneme-midp 再链接
./build.sh clean    # 清理后全量构建
```

前置：`phoneme-cldc/build/vita_arm/dist/lib/libcldc_vm*.a` 与
`phoneme-midp/build/vita_arm/obj/arm/libobj.a` 存在（见 build.sh 自检）。

## 运行

1. 安装 `build/cmake/midp_vita.vpk`（TITLEID `J2ME00001`）
2. 内置测试 MIDlet（改 `launch.cfg` 第二行切换）：
   - `HelloMIDlet` — Form 高级 UI 基线
   - `CanvasTest` — 图元 + drawImage 渲染验证
   - `InputTest` — GameCanvas 按键验证
3. 运行真实游戏：jar 放入 `ux0:/data/J2ME00001/`，写
   `ux0:/data/J2ME00001/launch.cfg`（前三行必需，第四行可选）：
   ```
   ux0:/data/J2ME00001/<game>.jar
   <MIDletClassName>
   portrait        ← 可选，第三行：portrait=竖屏240x320（默认），landscape=横屏320x240
   jit=0           ← 可选，第四行：0=纯解释器（默认）；1=仅第 1 轮开 JIT；2=每轮都开 JIT
   ```
   默认（无 cfg 或无第三行）为 **portrait**（竖屏 240x320）。

   `jit` 说明：JIT 编译器一直编进了 VPK，但第 2 轮起用 JIT 会触发
   未定位尾的崩溃（PROJECT_MEMORY v01.45），因此默认全程关闭。`jit=1`
   可拿回原先第 1 轮的 JIT 性能，`jit=2` 是复现该崩溃的最小开关；
   两个值都只在有机器可调试时使用。

键位：十字键=方向，✕=FIRE，○=SOFT1，□=SOFT2，△=GAME_A，L=GAME_B，R=GAME_C，
START=`*`，SELECT=`#`。

## 截屏（v02.92 / 提示条 v02.96）

两种触发方式，任意一种都会把**当前屏幕**存成 PNG：

| 方式 | 操作 |
| --- | --- |
| 实体按键 | **SELECT + START** 同时按住 |
| 虚拟按键 | 点屏幕**左侧黑边**里的相机图标（左软键下方） |

- 输出：`ux0:/data/J2ME00001/shots/shot_YYYYMMDD_HHMMSS.png`（960x544，
  含虚拟键盘与相机按钮，即玩家实际看到的画面）。
- 记录：每次尝试都追加到 `ux0:/data/J2ME00001/shot.log`（成功路径/大小/
  失败原因），便于和 `midp_stderr.log` 对照时间点。
- **提示**：保存后屏幕顶部中央会弹出 3 秒提示条——绿勾 +「已保存截图
  shot_....png」，失败则红叉 +「截图失败 rc=N」。提示条是截屏**之后**才画的，
  所以 PNG 里不会有它；即使画面静止或 VM 已冻结（没有新帧），轮询线程也会
  在 200 ms 后自己提交一帧把提示显示出来。
- **自动清除**：提示到期后，若 200 ms 内没有任何新帧（静止画面 / VM 冻结），
  轮询线程会把可见缓冲原地重绘一遍（黑边 + 游戏区 + 虚拟键 + 相机按钮，
  无提示条）并重新提交，所以不会留下绿色框；游戏自己在刷帧时则由它的下一帧
  完成清理。
- 冻结场景：截屏由独立的低优先级线程 `j2me_shot` 轮询按键/触摸。如果 VM
  线程已经卡死、不再刷帧，请求发出 500 ms 后会自动抓取**最后一次提交的帧**
  （也就是停在屏幕上的画面），所以卡死/白屏也有图可看。
- 配置 `ux0:/data/J2ME00001/shot.cfg`（可全部省略）：
  ```
  on=1        # 0 = 完全关闭截屏
  button=1    # 0 = 不画屏幕上的相机按钮（实体组合仍可用）
  combo=1     # 0 = 关闭 SELECT+START 组合
  ```
  SELECT+START 被识别时这两个键会被吞掉，游戏不会收到 `#` / `*`。

日志（均在 `ux0:/data/J2ME00001/`）：`midp_stderr.log`、`vm_output.log`
（VM `tty->print`）、`vm_stderr.log`（pcsl_print_chars）、`crumb.log`
（启动器/分阶段标记 + `--wrap` 信号量跟踪）、`boot_log.txt`、`perf.log`
（帧耗时，见下节）。
`ux0:/data/runmidlet_debug.log` 记录 runMidlet 入口阶段。

## 性能：CPU 频率与帧耗时（v02.97）

启动早期会向内核请求高性能时钟，并把结果写进 `ux0:/data/J2ME00001/perf.log`。

配置 `ux0:/data/J2ME00001/clock.cfg`（可全部省略）：

```
arm=444       # ARM MHz，0 = 不动
bus=222       # BUS MHz，0 = 不动
gpu=0         # GPU MHz，0 = 不动（默认；本移植是软件 blit，GPU 只给 IME 用）
stats=1       # 0 = 完全不写帧耗时行
stats_gap=300 # 每 N 帧一行（最小 60 ≈ 1 秒）
stats_max=200 # 本次运行最多写 N 行，防止写满 ux0
```

- 被内核拒绝不是致命错误：日志里留一行
  `arm: request 444 MHz failed rc=0x…, staying at 333 MHz`，游戏照常在旧频率运行。
  三个键彼此独立，一个失败不影响其余。
- `perf.log` 每行含义：

  | 字段 | 含义 |
  | --- | --- |
  | `period` | 相邻两帧间隔 → 真实 fps |
  | `flip` | 一次 flip 的入口到返回（含提交 + vblank 等待） |
  | `prepare` | 清边框 + 缩放 blit + 覆盖层 → 我们自己的 CPU 成本 |

  判读：`period≈16.67ms` 且 `prepare` 很小 ⇒ 受 vsync 限制，优化 blit 没收益；
  `period≈30ms` 且 `prepare≈20ms` ⇒ blit 是瓶颈。
- 超过 60 ms 的"帧"和超过 4 s 的空档被当作卡顿（换轮 / jar 加载），会重置
  统计窗口而不是并进平均。

## 启动加载画面（v02.99）

点开游戏到画面出现之间有 6–11 s（读 jar、预载类、启动 VM）。这段时间屏幕显示
原生加载画面，不再是白屏：

- 深色背景 + 游戏名（取 `games/<名字>/game.jar` 的目录名，支持中文）
- 阶段行：「读取游戏…」→「解压游戏资源…」→「初始化虚拟机…」
- 进度条（缓动向 90% 靠拢，不谎称完成）+「已载入 N 个类」
- 第一帧真实画面出现时自动消失；若 30 s 内一直没画面也会自动退出，不会永久挂着
- 消失时会**主动擦掉黑边里残留的画面**（v03.00）：加载画面覆盖整块面板，而游戏
  缩放只写中间的游戏矩形，不擦就会留下标题和进度条的尾巴

实现要点（改这块前先读）：它画在 `flip_to_display()` 里的**物理帧缓冲覆盖层**，
与虚拟键盘、截图提示条同一层，因此走**唯一**的 `sceDisplaySetFrameBuf + swap`
路径；重绘由预载循环驱动并限流到 4 Hz。它**不写** `vita_screen_pixels`，所以
`frame_has_content()`（用"非 0xFFFF"判断白屏结束）仍测的是真实画面，不会被自己
画的像素骗过去。

## 显示方向

默认 **竖屏 240x320**（J2ME 游戏的主流设计）。画面在 Vita 960x544 屏幕上
等比缩放为居中的 407x543 竖条。横屏游戏在 `launch.cfg` 第三行写 `landscape`
（320x240，近满屏 726x544）。
