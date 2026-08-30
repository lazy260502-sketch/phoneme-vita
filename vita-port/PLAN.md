# vita-port — J2ME/MIDP on PS Vita 移植层（干净重建版）

> 创建: 2026-08-30
> 状态: Phase 0-2 实现中
> 前置分析: 见 `PROJECT_MEMORY.md` 2026-08-30 缺口分析（输入/图形/音频三条链路断裂）

---

## 0. 本目录的定位

`vita-port/` 是 Vita 平台集成层的**唯一权威源码位置**，所有新代码只写在这里。
phoneme-cldc / phoneme-midp 源码树**零改动**（不再往里塞 vita 文件）。

与旧产物的关系：
| 旧位置 | 处置 |
|---|---|
| `midp-vita/src/main.c` | 复制改进为 `src/vita_main.c`（midp-vita/ 保留作参考，不再演进） |
| `phoneme-midp/src/porting/vita/*` | 死代码（KNI 名与 Java 侧不匹配），不复制，重写 |
| `phoneme-midp/src/vita_stubs.c` | 复制为 `src/vita_pcsl.c`（pcsl 文件 I/O 真实现可复用） |
| `midp-vita/CMakeLists.txt` | 复制改进为本目录 `CMakeLists.txt` |
| `build/vita_arm/generated/lib/*.config` | 复制到 `config/`，改虚拟屏分辨率 240x320 |

## 1. 架构结论（本次分析的产出，实现依据）

phoneME 在 Vita 上的事件/渲染链路中，**每条链只断在一两个可链接覆盖的点上**：

### 1.1 渲染链（已断 → 本目录接通）
```
Graphics.java native (gxj_graphics.c 等, libobj.a)
  → 写入 gxj_system_screen_buffer (RGB565, 定义于 lfjport_fb_export.o)
  → DisplayDevice.refresh0 (lcdui_display.o, libobj.a)
  → lcdlf_refresh (lfj_export.o) → lfjport_refresh (lfjport_fb_export.o)
                                     ↑ 空函数 TODO —— 唯一断点
```
**方案**: CMake 链接前 `ar d libobj_no_main.a lfjport_fb_export.o`，
用 `src/vita_display.c` 提供全部 22 个符号（gxj_system_screen_buffer + 21 个 lfjport_*），
`lfjport_refresh` 做虚拟屏→物理屏的最近邻缩放 blit（RGB565→A8B8G8R8）。

### 1.2 输入链（已断 → 本目录接通）
```
VM 周期回调 midp_check_events() (midp_master_mode_events.o, libobj.a)
  → checkForSystemSignal()   ← mastermode_export.o 里的空实现 —— 唯一断点
  → (waitingFor=UI_SIGNAL) → midpStoreEventAndSignalForeground → Java EventQueue
  → KeyConverter (lcdui_input.o) 把 KEYMAP_KEY_* 转 Canvas 键码 → keyPressed()
```
**方案**: `ar d mastermode_export.o`，`src/vita_input.c` 实现真 `checkForSystemSignal`：
轮询 SceCtrl，边沿检测，填充 `MIDP_KEY_EVENT` + `KEYMAP_STATE_PRESSED/RELEASED`，
`CHR` 用 `keymap_input.h` 的 KEYMAP_KEY_* 值（数字键='0'-'9'，方向=-1..-4，
SELECT=-5，SOFT1/2=-6/-7，GAMEA-D=-13..-16）。事件经 16 槽 SPSC 环形队列
从输入上下文传递到 VM 线程（与 timeout 语义无关，恒按非阻塞轮询处理）。

### 1.3 分辨率策略
- 虚拟手机屏 **240x320**（`gxj_system_screen_buffer` 尺寸 + `lfjport_get_screen_*` + internal.config 三处一致）
- 刷新时整数比最近邻放大（240x320 → 408x544 满高居中，黑边）
- 横屏支持（320x240）留作 Phase 6 配置项

## 2. 构建流水线

```
[已有产物，零改动]
phoneme-cldc/build/vita_arm/dist/lib/libcldc_vm*.a     (VM 库)
phoneme-midp/build/vita_arm/obj/arm/libobj.a            (MIDP 库, 145 成员)
        │  vita-port/CMakeLists.txt:
        │    ar d  runMidlet_md.o (去掉 main 冲突)
        │    ar d  lfjport_fb_export.o mastermode_export.o (被本目录实现取代)
        ▼
vita-port/src/{vita_main,vita_display,vita_input,vita_pcsl}.c + libobj_no_main.a
        ▼  arm-vita-eabi-gcc 链接 (--allow-multiple-definition 保持)
midp_vita.velf → .self → VPK (eboot + config/ + midp_system.jar + Hello.jar)
```

## 3. 阶段计划

### Phase 0: 基线 ✅（本轮）
- [x] 依据 2026-08-30 缺口分析确定覆盖点
- [x] 目录创建、有用代码复制（vita_pcsl.c、config 副本、main.c 改写）
- [ ] `./build.sh` 端到端产出 VPK，Vita3K 装入

### Phase 1: 渲染接通 ✅（本轮实现，待真机验证）
- [x] `vita_display.c`: lfjport 22 符号全覆盖，refresh 做缩放 blit
- [ ] Vita3K 验证：HelloMIDlet 的 Form 文本可见（高级 UI 走 chameleon→同一缓冲）
- [ ] CanvasTest MIDlet：fillRect/drawLine/drawImage(PNG) 各画一块

### Phase 2: 输入接通 ✅（本轮实现，待真机验证）
- [x] `vita_input.c`: checkForSystemSignal + SPSC 事件环
- [ ] InputTest MIDlet：keyPressed 显示键码，GameCanvas.getKeyStates 驱动方块移动
- 键位: 十字键=方向, ✕=FIRE, ○=SOFT1, □=SOFT2, △=GAME_A, L=GAME_B, R=GAME_C, START='*', SELECT='#'

### Phase 3: 运行真实游戏（下轮）
- [ ] 把任一真实 J2ME 游戏 jar 放入 ux0:/data/J2ME00001/，launch.cfg 指定 jar+MIDlet 类
- [ ] 已知风险清单逐个排：PNG 解码（imgdcd 在 libobj.a，应可用）、字体只到 ASCII、
      GameCanvas 全屏刷新率、RMS 持久化（pcsl_file 真实现）
- [ ] launch.cfg: 首行 jar 绝对路径，次行 MIDlet 类名；缺省 Hello.jar/HelloMIDlet

### Phase 4: 声音（SceAudio）
- [ ] TonePlayer: 方波合成 → sceAudioOut（JSR-135 类在 libobj.a 是接口壳，需接 media/midp_audio native）
- [ ] PCM wav 播放；MIDI 视需求

### Phase 5: 字体/中文
- [ ] 现状: gxj_font_bitmap + internal.config ISO8859_1，仅 ASCII
- [ ] 引入 12px/16px 点阵中文字库（GBK 常用 3500 字），接 gxjport_text/gxj_text
- [ ] microedition.encoding 调整为 UTF-8 并验证 jar 内中文资源读取

### Phase 6: 体验完善
- [ ] 横竖屏切换（SELECT 长按或 launch.cfg）
- [ ] 触摸屏 → MIDP_PEN_EVENT（前后触板映射指针）
- [ ] 按键重复（KEYMAP_STATE_REPEATED，timer 生成，参照 fb 端口 handle_repeated_key_port）
- [ ] 模拟摇杆 → 方向键（部分游戏）

## 4. 风险与对策

| 风险 | 对策 |
|---|---|
| `midp_check_events` 调用频率不足（VM 空转时） | CLDC JVMSPI 每 tick 调用；若实测输入迟滞，改为独立 pthread 轮询直接调 midpStoreEventAndSignalForeground |
| 删除 archive 成员后隐式符号缺失 | 链接报 undefined 即知；两成员符号表已 nm 确认（mastermode_export.o 仅 checkForSystemSignal） |
| 240x320 下 chameleon 高级 UI 皮肤错位 | internal.config 与 lfjport 尺寸已同步；若仍异常，回退全屏 960x544 模式对比 |
| --allow-multiple-definition 掩盖真冲突 | 构建后 nm 检查 vita_display/vita_input 符号确实被采用（addr2line 验证 refresh 落在本目录代码） |

## 5. 验证清单（每轮构建后）

1. `arm-vita-eabi-nm midp_vita.elf | grep -E 'lfjport_refresh|checkForSystemSignal'` 地址在本目录 .o 中
2. VPK 内容完整: eboot.bin + lib/*.config(240/320) + midp_system.jar + Hello.jar
3. Vita3K: 日志 `ux0:/data/midp_stderr.log` 无 fatal；屏幕出现 Form
4. 按键: InputTest 显示对应键码；GameCanvas 方块随十字键移动
