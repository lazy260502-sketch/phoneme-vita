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

日志（均在 `ux0:/data/J2ME00001/`）：`midp_stderr.log`、`vm_output.log`
（VM `tty->print`）、`vm_stderr.log`（pcsl_print_chars）、`crumb.log`
（启动器/分阶段标记 + `--wrap` 信号量跟踪）、`boot_log.txt`。
`ux0:/data/runmidlet_debug.log` 记录 runMidlet 入口阶段。

## 显示方向

默认 **竖屏 240x320**（J2ME 游戏的主流设计）。画面在 Vita 960x544 屏幕上
等比缩放为居中的 407x543 竖条。横屏游戏在 `launch.cfg` 第三行写 `landscape`
（320x240，近满屏 726x544）。
