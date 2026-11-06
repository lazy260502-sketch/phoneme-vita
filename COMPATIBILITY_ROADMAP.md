# J2ME 兼容性与性能提升路线图

> 基线：`v04.32`（2026-11-05）
> 目标：对标主流开源 J2ME 模拟器，补齐"跑不起来 / 跑不对 / 跑不快"的缺口。
> 本文只做分析与规划，不含代码改动。所有"现状"均可在仓库内找到对应文件/配置。
>
> **基线说明**：本文初版基线是 `v02.80`。P0-1…P0-7 已于 v02.83–v02.93 全部落地，
> 2026-11 的主战场是 3D（v04.02–v04.31）与 JIT 内存（v04.17–v04.29）。
> **v04.32 又落地了四项里三项兼容性**（字符集/时区/挂起恢复，见 §0.1）与一项失败回退
> （ServerSocket）。读本文时请以 §0.1 的最新状态为准，各 Pn 小节里的"现状"保留为历史记录。

## 0.1 v04.32 增量（2026-11-05，最新状态）

### 已落地（代码完成，待真机验证）

| 项 | 内容 | 证据 |
|---|---|---|
| **字符集** | phoneME 在本平台**没有通用转换器**（`getLcGenConvMethods()` 是返回 NULL 的桩），现补 SJIS / EUC-JP / EUC-KR / GBK(GB2312) / Big5 双向转换 | `phoneme-midp/src/i18n/i18n_port/vita/native/vitaGenConv.c` + 生成表 `vita_charmap.h`；宿主全表验证 `tools/charsetgen/verify_charsets.py`（5 编码 × decode+encode = 0 不一致） |
| **本地时区** | `getenv("TZ")` 恒 NULL ⇒ newlib 无 TZ ⇒ 所有游戏 GMT+00:00。改为向主机要（`sceRtc` local vs UTC） | `vita-port/src/vita_timezone.c`；`midp_timezone_md.c` 的 `#if defined(__vita__)` 弱引用分支；日志 `[TZ] GMT+08:00` |
| **挂起/恢复** | 后台回来音频端口作废但句柄被永久复用 ⇒ 整局无声；时钟也可能回落。现监听 `ON_RESUME`，重开 `SceAudioOut` 端口 + 重发时钟 | `vita-port/src/vita_lifecycle.c`；`vita_audio_after_resume()`；日志 `[LIFE] resume #N` |
| **ServerSocket**（v04.33） | `USE_SERVER_SOCKET=true` 已可用：`ServerSocketConnection` / `socket://` 服务端不再是缺失 API（`vita_net.c` 的 `pcsl_server_socket_*` 早就写好了，卡的是构建顺序） | `build_vita.sh` 开关 + 重生成 `ROMStructs.h`；ELF 含 `pcsl_server_socket_*` 与 KNI 入口 |
| **虚拟键盘字母页 + 长按重复**（v04.33） | page 0 = 数字 + `abc`，page 1 = A–Z + `123`；字母按 ASCII `'a'+i` 注入，MIDP 不需要新键码；按住 450 ms 后每 90 ms 发 `KEYMAP_STATE_REPEATED` | `vita_vkey.c`（`layout_alpha`/`layout_page`/`VKEY_KEY_PAGE`，`VKEY_MAX_BTN` 24→48，5×7 字形补完 A–Z）、`vita_input.c`（repeat） |
| **每游戏 `jit=`**（v04.33） | `game.cfg` 现在可以单独给某个游戏指定 JIT 策略，不再只能全局设 | `vita_main.c` 的 `read_game_jit()`（优先级：`game.cfg` > `launch.cfg`） |
| **每游戏设置补全 5 项**（v04.34） | 已有的 `CROSS` 对话框（原本只有 orientation/screen/scale/profile/MIDlet）补上 **locale / 虚拟键盘 / 振动 / 权限 / JIT**，全部写回该游戏的 `game.cfg`；`-1 = 跟随全局` 时不写键 | `vita_menu.c`：`GameEntry` 加 4 字段、`game_cfg_load`/`save_cfg` 补键、`DLG_*` 9→13 行（行距 40→34 以塞进 544 px） |
| **字体 size/style**（v04.34） | `gxjport_*` 三个字体入口原本把 `size`/`style` 全 `(void)` 掉；现在按 `SIZE_SMALL 0.75x / MEDIUM 1.0x / LARGE 1.25x` 最近邻缩放，**度量同比例**，`STYLE_BOLD` 右移 1 px 重绘、`STYLE_UNDERLINED` 画线（斜体明确不做） | `vita_font.c`：`font_scale()`、`fb_blit565_scaled()`、三个 `gxjport_*` 重写 |
| **M3G `Texture2D` wrap**（v04.35） | `Renderer` 把 `WRAP_CLAMP` 打包进 texture 参数 bit17/18 → `m3g_native` → `v3d_texture_wrap` → GL 用 `GL_CLAMP_TO_EDGE`（按槽位缓存）、软件光栅不再无条件取小数部分 | `Renderer.java`、`m3g_native.c`、`vita_3d.c/.h`、`vita_gl3d.c/.h`、`vita_sw3d.c/.h` |
| **系统 UI 覆盖时静音**（v04.35） | 用 `_sceAppMgrGetAppState().isSystemUiOverlaid` 轮询（200 ms）：覆盖时 `vita_audio_set_suspended(1)` 让渲染循环**停在原位**不再提交，解除时重开端口 + 重设时钟。（`pauseApp/resumeApp` 仍未接线） | `vita_lifecycle.c`（第二个线程）、`vita_audio_javacall.c` |
| **JSR-179 Location**（v04.36） | 最小实现 11 个类（`LocationProvider`/`Location`/`Coordinates`/`Criteria`/`Orientation`/`AddressInfo`…）：固定坐标（北京）、`getLocation` 立即返回、监听器同步回调一次；不抛 `ClassNotFoundException`。CLDC 1.1 无 `Math.atan2` → 自写多项式近似 | `vita-port/jsr_src/javax/microedition/location/*.java`；`build_nokia_stubs.sh` 新增 `jsr_src` 输入 + `JSR_PKGS` 白名单 |
| **JSR-256 Sensor**（v04.37/38） | 最小实现 15 个类 + **`sensor:` 协议处理器**：`SensorManager.findSensors()`、`SensorInfo`/`ChannelInfo`/`Data`/`MeasurementRange`/`Unit`、同步 `getData()` 与异步 `setDataListener()`（单泵线程）。**数据是主机真六轴**：launcher 用 `sceMotion` 以 50 Hz 采样并发布 `motion.bin`（24 B，g / rad·s⁻¹），Java 侧用 JSR-75 读回；全 0（Vita3K）或读不到时回落合成倾斜 | `vita-port/jsr_src/javax/microedition/sensor/*.java` + `com/sun/midp/io/j2me/sensor/Protocol.java`（`SUN_ALLOW`）+ `vita-port/src/vita_motion.c`（`SceMotion_stub`） |
| **JSR-82 Bluetooth**（v04.39） | 最小实现 16 个类 + **`btspp:` 回环协议**：`LocalDevice` 固定身份、`DiscoveryAgent` 异步报告"无设备"、`ServiceRecord` 本地属性表、`btspp://` 连接共享 4 KB 进程内总线（写→任一连接可读，`acceptAndOpen` 立即返回）。无 `javax.obex`；签名**未核对规范**（镜像缺 JSR-82） | `vita-port/jsr_src/javax/bluetooth/*.java` + `com/sun/midp/io/j2me/btspp/Protocol.java`；脚本 `JSR_PKGS`（完整包路径）与 `SUN_ALLOW` |
| **v10 J3D**（v04.40） | VSCL 1.0/1.1 的 **11 个类型全部落地**（`Graphics3D`/`Figure`/`FigureLayout`/`Effect3D`/`Light`/`Texture`/`AffineTrans`/`Vector3D`/`ActionTable`/`Util3D` + `Graphics3DImpl`），定点 4096、`AffineTrans` 公有 m00..m23。**限制**：模型格式（Aplix M3D）不公开 ⇒ 只画占位；规范也没说 MIDlet 怎么拿 `Graphics3D` 实例 | `vita-port/jsr_src/com/vodafone/v10/graphics/j3d/*.java`（脚本 `JSR_PKGS`） |
| **M3G Fog**（v04.41） | `Appearance.setFog()` 原本存了从不读；现在 `Renderer` → `NativeBridge.setFog`（**参数全 int**，避开浮点 ABI）→ `m3g_native` → `v3d_fog` → **GLES1 真 `GL_FOG`**（`GL_LINEAR`/`GL_EXP`，按状态缓存）+ **软件光栅按 `1/iw` 视距混合同样两条曲线** | `Renderer.java`、`NativeBridge.java`、`m3g_native.c`、`vita_3d.c/.h`、`vita_gl3d.c`、`vita_sw3d.c` |
| **M3G 硬件 alpha test**（v04.42） | v04.02 曾断言"GLES1 没有 alpha test、只能用软件后端"——**误判**：GLES1.0 就有 `glAlphaFunc`+`GL_ALPHA_TEST`，vitaGL 两端都实现。现在 `vita_gl3d_set_state()` 按阈值 enable/disable，**默认（GL）后端上树叶/栅栏/镂空贴图不再被画成实心四边形** | `vita-port/src/vita_gl3d.c`（~12 行） |
| **M3G `MorphingMesh` 权重**（v04.43） | 之前只画 base buffer、目标被忽略（`MorphingMesh.java` 自己写明了）。现在 `Renderer.applyMorph()` 按 `v = base + Σ w_i·(target_i − base)` 混合，**在 base 快照上累加**（就地累加会让第二个目标以已混合值为基准），顶点数取 `min`，每个目标用自己的 scale/bias；在 Java 层做 ⇒ 两个后端都受益 | `Renderer.java`（`applyMorph()` + 一处调用）、`MorphingMesh.java` 头注释 |
| **M3G `Appearance.layer` 顺序**（v04.44，**M3G 收尾关闭**） | submesh 按 **layer 升序、稳定排序**绘制，并**跳过 `layer == -1`**（规范的"不渲染"）；同 layer 保持定义顺序（贴花/玻璃分层依赖这点） | `Renderer.java`（`layerOf()` + `order[]` 排序 + 主循环改走 `order[]`） |
| **JSR-256 条件监视**（v04.46） | v04.37 明确留下的缺口，且不只是两个接口：规范把 `addCondition` 定义在 **`Channel`** 上，而 `Channel` 整个缺失（`SensorConnection` 只有 `getChannel(ChannelInfo)`，参数**实例相等**）。补齐 `Condition`(+5 个 `OP_*`)/`ConditionListener`/`LimitCondition`/`RangeCondition`/`ObjectCondition`/`Channel`/`VitaChannel`，`collect()` 逐值触发，命中后**先摘除再通知**（防刷屏），`Data` 的 validity 恒 true。**宿主 62 项检查全过** | `vita-port/jsr_src/javax/microedition/sensor/{Condition,ConditionListener,LimitCondition,RangeCondition,ObjectCondition,Channel,VitaChannel}.java`、`SensorConnection.java`、`VitaSensorConnection.java`；测试 `debug/tools/sensortest/` |
| **虚拟键下移 + 状态栏半透明**（v04.45） | 诊断浮层固定在左上角，bar 模式软键行/动作 d-pad 正好压住它。`VKEY_Y_SHIFT 20` 把 bar 模式三处下移（数字面板并 clamp 到底边以上），overlay 模式不动。浮层背板改为 **alpha 170/255 混合**（读回后端缓冲；约 4800 像素/帧 ≈ 38 KB，占 blit 的 884 KB 的 5% 以内） | `vita-port/src/vita_vkey.c`、`vita_diag.c`（`fill_box()` 加 alpha 参数） |
| **OGG Vorbis**（v04.45） | `stb_vorbis` v1.22（public domain）接入同一个 `vita_pcm_dec` vtable，播放线程/缓冲/音量/seek 全部复用。**整文件入 native 堆**（≤8 MB）：v1.22 无 read/seek 回调 API，只有 `open_memory` 支持 `stb_vorbis_seek`。MIME 收 `audio/ogg`/`x-ogg`/`vorbis`/`application/ogg`/`x-ogg` | `vita-port/src/vita_ogg.c`（+ `stb_vorbis.c`，仅 include 不单独编译）、`vita_audio_javacall.c`、`vita_pcm_dec.h` |
| **音频 32 位溢出修复**（v04.45，**既有 bug**） | `mp3_read()` 的 `(t * rate) / OUT_RATE` 在 ARM（`long`=32 位）**t≈48700 输出帧（约 1 秒）**就溢出：实测 t=200000 时算出 **4793**（跳回开头）。MP3/OGG 两处改用 `long long` 中间量（`OUT_RATE` 是常量 ⇒ 仍是乘+移位，不引 libgcc 除法） | `vita-port/src/vita_mp3.c`、`vita_ogg.c` |

### v04.32 的 ServerSocket 失败原因（v04.33 已解决，留作教训）

v04.32 以为是"romgen 没把类放进 ROM"，实际是**构建顺序**：`serverSocketProtocol.c` 依赖
`ROMStructs.h` 里的 `struct Java_com_sun_midp_io_j2me_serversocket_Socket`，而 romgen 只对
**已编进 `classes.zip`** 的类生成该结构。当时直接以 `libobj.a` 为目标构建，Java 编译根本没执行
（日志只有 4 行），于是沿用旧的 `ROMStructs.h`。正确配方见 `BUILD_QUICKSTART.md` 坑 20。

### 仍然缺（按建议顺序）

1. **JIT 结构性解锁**：只有 Java 堆 ≤15 MB（VM 块上限 16 MiB）时才可执行；32 MB 堆下 JIT 恒关。
   **v04.47 更新**：log187 判读出「15 MB 堆 ⇒ JIT 真开 ⇒ 必崩」，根因是 `OsMisc_flush_icache()`
   在 Vita 上空实现（`__builtin___clear_cache` 不生成代码），JIT 写的代码永远进不了 icache。
   已接上 `sceKernelSyncVMDomain`（`vita_vm_flush_icache`），**待真机验证**；
   若仍崩先看 pc 是否还在堆内（`0x85d00000..0x86d00000`），在则还有 `arm_flush_icache` 一条路径要接。
   正解是把 compiler area 拆成独立 VM 块（§P1-1）。
2. **虚拟键盘余项**：自定义位置/大小/透明度、多点触控、触摸→指针的逐游戏切换、宏/组合键。
3. **M3G 1.1 已收尾**（v04.44 关闭）：wrap(CLAMP) v04.35、Fog v04.41、硬件 alpha test v04.42、
   MorphingMesh 权重 v04.43、`Appearance.layer` 顺序 v04.44 全部完成。
   **三条作废项**（凭记忆写错，M3G 1.1 里没有可用入口）：`Background.SPHERE/CUBE`（天空盒，属 2.0）、
   "多纹理"（1.1 只有 1 层）、`PolygonMode` POINT/LINE（常量 168/169 在，但无 `setFillMode/getFillMode`）。
   2D 背景（`Background.setImage` + crop/repeat）自 v03.53 起已能画。
   注：VSCL `sprite` 包**不做**——8×8 tile 的硬件引擎 + 私有数据格式，做了也只能画占位。
4. **Mascot Capsule V3 / JSR-239**：**零行代码**（§阶段 5）。~~v10 J3D~~ 的 API 面已落地
   （v04.40，11 个类型），但模型格式（Aplix M3D）不公开 ⇒ **只画占位、不能渲真实模型**；
   规范也未公开「MIDlet 如何获得 `Graphics3D`」，用其它获取方式的游戏仍拿不到实例。
   VSCL 的 `com.vodafone.v10.graphics.sprite` 亦未做。
5. **PIM**：零行代码（`javax.microedition.pim`，要自建联系人/日程存储）。~~JSR-179~~（v04.36）、
   ~~JSR-256~~（v04.37/38，真六轴）、~~JSR-82~~（v04.39，`btspp:` 回环）已最小实现，见 §0.1。
   另：~~JSR-256 的 `Condition`/`ConditionListener`~~ 已随 v04.46 补齐（连同规范要求的 `Channel`，
   见 §0.1）；JSR-82 的 `javax.obex` 仍未做，且 JSR-82 的签名
   未核对规范（镜像缺该 JSR），遇到 `NoSuchMethodError` 先查规范。
   **v04.46 更新**：`Condition`/`ConditionListener`（以及规范要求的 `Channel`）**已实现**，见 §0.1，
   JSR-256 的条件监视现在是完整的，宿主 62 项检查通过。仍未做的只剩两块：
   - **`javax.obex`（JSR-82 的 OBEX 部分）**：规范镜像仍缺该 JSR，但**参考实现来源已实测可用**
     （两次 `curl` 均 200）：`platform/frameworks/base/obex/javax/obex/`（`javax.obex` 包本体）与
     `platform/external/obex/src/com/android/obex/`（Google 实现，含 `ClientSession`/`HeaderSet`/
     `Authenticator`/`ApplicationParameter`/`BaseStream`）。规模 ≈ 15 类 + `ObexPacket` 位流状态机
     （CONNECT/PUT/GET/SETPATH/ABORT/DISCONNECT），建议单独一轮；验证方式同 v04.46（宿主测试 +
     VPK 逐类核对）。
   - **`PIM`（`javax.microedition.pim`）**：约 12 类，且要自建联系人/日程存储（RMS 或文件），
     是这三项里最大的一块。
6. **`MIDlet.pauseApp()/resumeApp()`**：v04.32 恢复了平台侧、v04.35 加了系统 UI 静音，
   但 AMS 状态机仍未接线（需给 `MIDletStateHandler` 加 KNI 可调入口）。

---

## 0. 结论先行（建议按这个顺序做）


| 顺序 | 事项 | 为什么排前面 | 预估 |
|---|---|---|---|
| 1 | **每游戏配置 + 可变虚拟屏幕（含设备画像）** | 屏幕尺寸/平台身份是"游戏按错误设备渲染"的总开关，也是后面 JIT/音频/按键配置的载体 | 2–3 周 |
| 2 | **虚拟键盘 + 触屏映射** | 现在只有 12 个物理键可重映射，数字键/软键无法同时覆盖；大量国产游戏用 `1–9/*/#` | 1–2 周 |
| 3 | **MP3 / AMR 音频 + 振动** | 目前只支持 tone-seq / WAV / SMF；MP3 是 2005 年后游戏的标配 | 1–2 周 |
| 4 | **启动白屏与 CPU 频率** | 444MHz 与 splash 是低成本收益；启动 7–11 s 是用户第一感知 | 3–5 天 |
| 5 | **JIT 二次启动根因 + 分游戏开关** | 解释器是最大的性能上限；JIT 已在二进制里，只被崩溃挡住 | 2–4 周（高风险） |
| 6 | **3D（JSR-184 / Mascot Capsule / v10 J3D）** | 工作量最大、但决定 3D 游戏能不能进门 | 4–8 周 |
| 7 | **HTTPS / JPEG / 长尾 JSR** | 影响面小于上面几项，可并行补 | 持续 |

> 先做 1–4 的原因：它们不碰 VM 深水区，能立刻扩大可运行游戏集，并为性能优化提供度量与配置基础。

---

## 1. 对标对象与方法

### 1.1 对标对象

| 模拟器 | 平台 | 执行模型 | 主要参考价值 |
|---|---|---|---|
| **J2ME Loader**（nikita36078） | Android | 宿主 JVM + Java 兼容层（MicroEmulator 路线），可借 ART 的 JIT/AOT | 厂商 API 覆盖、每应用设置、屏幕尺寸/按键/虚拟键盘的产品化做法 |
| **FreeJ2ME** | Java / libretro | 纯 Java 兼容层，跑在宿主 JVM | API 兼容层组织方式、libretro 前端的输入/缩放 |
| **KEmulator nnmod** | Windows | 原生模拟器 | 3D（JSR-184 / Mascot Capsule）、厂商 API、音频格式的"最广覆盖"参照 |
| **PSPKVM** | PSP | phoneME（同源 VM） | 同级别掌机上的虚拟键盘、按键方案、内存/性能取舍 |
| **MicroEmulator** | Java | 宿主 JVM 兼容层 | MIDP 2.0 参考实现、配置模型 |
| **SquirrelJME** | 多平台 | 独立 JVM（非模拟器） | CLDC/MIDP API 完整性与边界行为 |

> 说明：本表依据各项目公开 README/实现架构整理，当前环境无法联网逐版本核对，具体能力以官方最新版为准。可以确认的是：本仓库 `vita-port/nokia_src/vendor/` 的厂商类清单就是以 **J2ME Loader** 为参照补齐的（见 v02.78/v02.79 记录）。

### 1.2 评价维度

1. **能跑**：CLDC/MIDP 版本、厂商 API、JSR、JAD/权限、屏幕与设备身份。
2. **跑对**：图形/音频/输入/网络/存储的行为正确性。
3. **跑快**：VM 执行、渲染、启动、音频合成、内存/GC。
4. **好用**：安装、每游戏设置、虚拟键盘、存档、诊断。

---

## 2. 主流模拟器能力对照（粗略）

| 能力 | J2ME Loader | FreeJ2ME | KEmulator nnmod | PSPKVM | 我们 v02.80 |
|---|---|---|---|---|---|
| 执行方式 | 宿主 JVM（ART JIT/AOT） | 宿主 JVM | 原生 VM | phoneME 解释器/JIT | phoneME CLDC-HI **解释器**（JIT 已编入、默认关） |
| CLDC / MIDP | 1.0/1.1 / 1.0/2.0 | 1.0/1.1 / 1/2 | 1.0/1.1 / 1/2 | 1.1 / 2.0 | 1.1 / 2.0（profile 串 MIDP-2.1） |
| 厂商 API | 很广 | 较广 | 最广 | Nokia 部分 | 190 文件，Nokia/Samsung/Motorola/Siemens/Vodafone/J-Phone/JBlend 大部分纯 Java 类已补 |
| JSR-75 FileConnection | 有 | 有 | 有 | 有 | **有**（独立 `jsr75/` 子系统） |
| JSR-75 PIM | 有/部分 | 部分 | 有 | 无 | **无** |
| JSR-120 WMA | 有 | 部分 | 有 | 无 | 有（SMS 走桩，不发真实短信） |
| JSR-135 MMAPI | 有 | 有 | 有 | 有 | **有**（tone-seq / WAV PCM / MIDI SMF） |
| 音频格式 | MIDI/MP3/AAC/AMR/OGG/WAV（Android 解码器） | MIDI/WAV/部分 | 很多 | MIDI/WAV | tone-seq / WAV PCM / SMF / MP3 / AMR-NB / AAC(ADTS) / **OGG Vorbis**；**无 XMF/MMF-SMAF**（不做）、无 AMR-WB、无 AAC-HE |
| 3D | JSR-184 / Mascot Capsule V3（公开特性） | 部分 | JSR-184 / Mascot Capsule V3 | 少 | **JSR-184 已接（31 个类 + 软件/硬件双后端）**；Mascot Capsule V3 与 v10 J3D **无** |
| OpenGL ES (JSR-239) | 有 | 无 | 有 | 无 | 无（可映射到 vitaGL） |
| 屏幕尺寸 | 每应用可选 | 可配置 | 可配置 | 固定+缩放 | **固定 240×320 / 320×240，缓冲上限 320×320** |
| 缩放模式 | fit/stretch/整数等 | 有 | 有 | 有 | 仅 fit（等比居中） |
| 虚拟键盘 | 有 | 有 | 有 | 有 | **无**（物理键重映射 + 系统 IME） |
| 触屏 | 可映射为按键/指针 | 有 | 有 | 无 | 仅指针事件（`MIDP_PEN_EVENT`） |
| 每应用设置 | 有 | 有 | 有 | 有 | 仅 jar/class/orientation/jit |
| 设备身份伪装 | 部分/可配置（未逐版本核实） | 部分 | 有 | 部分 | **固定 `microedition.platform: PS Vita`** |
| 网络 | Android 网络栈，HTTPS 有 | 宿主网络 | 宿主网络 | 有限 | BSD socket + HTTP；**SSL 关闭** |
| 振动 | 有 | 未核实 | 有 | 未核实 | **无**（`anc_*_vibrate` 未实现） |
| 存档/快照 | 部分（RMS） | 未核实 | 有 | 未核实 | RMS 有；VM 快照无 |
| 前端/安装 | 应用列表 + 文件导入 | libretro | 桌面 UI | PSP UI | 列表/网格 + 文件浏览器 + inbox |

---

## 3. 我们现在的家底（带证据）

| 子系统 | 现状 | 证据 |
|---|---|---|
| VM | phoneME CLDC-HI，系统类 ROM 化；ARM 汇编解释器；JIT 后端存在（ARM）但默认关闭 | `phoneme-cldc/src/vm/share/compiler/`、`phoneme-cldc/src/vm/cpu/arm/CodeGenerator_arm.cpp`、`vita-port/src/vita_main.c:99`（`VITA_JIT_DEFAULT 0`）、`phoneme-cldc/build/vita_arm/vita_arm.cfg:288`（`ENABLE_JIT := true`） |
| 启动/预载 | 启动时把 classpath 上的 jar 全量预载；jar 整包读入内存 | `phoneme-cldc/src/vm/share/handles/Universe.cpp:1094`、`phoneme-cldc/src/vm/os/vita/OsFile_vita.cpp`（v02.70） |
| 堆 | Java 堆硬编码 32MB；newlib 池 64MB | `vita-port/src/vita_pcsl.c:2224`、`vita-port/src/vita_main.c:85` |
| 显示 | 虚拟屏固定 240×320 或 320×240；静态 320×320 缓冲；逐像素 RGB565→ABGR 等比缩放 | `vita-port/src/vita_display.c:38`（`VITA_MAX_W/H 320`）、`vita_display.c:104`（`compute_scaling`）、`vita_display.c:292`（`flip_to_display`） |
| 输入 | 12 个物理键可运行时重映射；触摸→`MIDP_PEN_EVENT`；系统 IME | `vita-port/src/vita_input.c:96`（`key_map`）、`vita_input.c:424`（touch）、`vita-port/src/vita_ime.c` |
| 音频 | tone-seq / WAV PCM / MIDI(SMF, TinySoundFont)；tone 线程 | `vita-port/src/vita_audio_javacall.c:247`、`vita_wav.c`、`vita_midi.c` |
| 网络 | BSD socket；HTTP/HTTPS 由 Java 层走 socket；无 SSL | `vita-port/src/vita_net.c:1`、`phoneme-midp/build/vita_arm/Settings.gmk:32`（`USE_SSL=false`） |
| 存储 | RMS + JSR-75 FileConnection | `vita-port/src/vita_pcsl.c`、`jsr75/src/share/native/jsr75_file.c` |
| 厂商 API | 190 个文件（Nokia/Samsung/Motorola/Siemens/Vodafone/J-Phone/JBlend） | `vita-port/nokia_src/vendor/` |
| 3D | JSR-184（M3G 1.1）：31 个 API 类 + `M3gReader` + `Renderer`（1273 行）+ 软件/硬件双后端；已跑通带蒙皮模型的标题 | `jsr184/src/share/core/common/classes/javax/microedition/m3g/`、`jsr184/src/share/native/m3g_native.c`、`vita-port/src/vita_sw3d.c`、`vita-port/src/vita_gl3d.c` |
| 启动器 | 列表/网格、文件浏览器、inbox、图标缓存、3 项全局设置 | `vita-port/src/vita_menu.c:1580`（`SET_VIEW/SET_JIT/SET_KEYMAP`） |
| 运行配置 | `launch.cfg` 四行（jar/class/orientation/jit）；`internal.config` 固定设备属性 | `vita-port/src/vita_main.c:1`、`vita-port/config/internal.config` |
| CPU 频率 | **未调用 `scePowerSetClockFrequency`**（按系统默认跑） | 全仓库无该调用 |

---

## 4. 缺口清单

优先级定义：
- **P0**：不补就会"跑不起来/跑不对"，且影响面大。
- **P1**：影响体验或性能上限，但不阻塞启动。
- **P2**：长尾，按游戏样本决定。

### 4.1 P0：兼容性硬伤

> **进度（2026-10-24）**：P0-1 已于 v02.83 落地，P0-2 已于 v02.84 落地，
> P0-3 已于 v02.85 + v02.89 落地（虚拟键盘 + 左右软键 + 按下反馈），
> P0-4（MP3/AMR-NB/AAC/seek/内存上限/Nokia 铃声/振动）已于 v02.87 + v02.88，
> P0-5（JPEG）已于 v02.89 落地，P0-6（权限默认策略）已于 v02.90 落地，
> P0-7（HTTPS，经 SceHttp）已于 v02.93 落地
> ——**P0 全部代码完成，均待真机验证**。v02.86 把新增开关搬进了菜单「设置」页并引入全局
> `defaults.cfg`（优先级：`game.cfg` > `defaults.cfg` > 编译期内置默认）。
> 实现细节与验证方式见 `PROJECT_MEMORY.md` 对应版本条目。
>
> **P1 进度（2026-10-24）**：P1-3 的第 1、2 项已于 v02.97 落地
> （444/222 MHz + `perf.log` 帧耗时统计；**但 v03.06 发现 444 从未真正生效**，
> 已加三假设取证等真机日志定案），第 3 项（NEON / GPU 缩放）等真机 `perf.log` 数据。
> P1-2 第 1 项（启动 splash）已于 v02.99 落地、v03.00/v03.05 修完残影与闪烁；
> 第 2 项（stderr 行缓冲）**经 log120 数据核实无需做**；第 3 项
> （预载 I/O）v03.03 取证、**v03.08 修掉 jar 句柄泄漏**。P1-1（JIT）另有 v02.94 探针；
> **v03.71 查明 JIT 障碍 ②的根因是工具链把 `__builtin___clear_cache` 折叠成空**，
> 并给出 VM domain 方案与门控探针（`vita_jitmem.c`）。P1-8 诊断浮层 v03.70 落地。
> 渲染侧：v03.59 拆分帧耗时、v03.64 blit 两趟化（真机 14.69→8.03 ms，−45%）。

#### P0-1 可变虚拟屏幕 + 每游戏配置 ✅ v02.83

现状：`gxj_system_screen_buffer` 固定 240×320/320×240，静态缓冲上限 320×320；`internal.config` 的 `microedition.display.*` 也是固定值。

影响：
- 为 128×160 / 176×208 / 176×220 / 360×640 / 480×800 设计的游戏会拿到 240×320，出现布局错位、黑边、UI 出屏、触摸错位。
- 部分游戏用 `getWidth()/getHeight()` 动态布局，反而无法按设计分辨率渲染素材。

目标：
1. 每游戏 `game.cfg`（key=value），支持预设：`128x160`、`176x208`、`176x220`、`240x320`、`320x240`、`360x640`、`480x800`、自定义。
2. 虚拟屏缓冲按尺寸动态分配（上限建议 800×800，RGB565 + alpha 约 3.8MB）。
3. `microedition.display.width/height`、`internal.config` 与虚拟屏同步。
4. 缩放模式：fit / stretch / integer；触摸反变换随之更新。
5. 方向：portrait / landscape / 180° 翻转（最后一项可选）。

证据与改动点：
- `vita-port/src/vita_display.c`（缓冲、缩放、触摸映射）
- `vita-port/src/vita_main.c`（启动前设置尺寸、生成 `internal.config`）
- `vita-port/src/vita_menu.c`（每游戏设置页、写 `game.cfg`）
- `vita-port/config/internal.config`（属性模板）

验收：6 种分辨率下画面不变形、触摸坐标误差 ≤1px；至少 2 个"非 240×320"游戏正常。

#### P0-2 设备画像（platform / 厂商属性 / JAD / 多 MIDlet）✅ v02.84

现状（v02.84 前的真实情况，此处已按源码更正）：
- `System.getProperty()` → `JVMSPI_GetSystemProperty` → `getSystemProperty()` 只查
  **application properties = `lib/system.config`**，与 `internal.config` 无关。
  也就是说游戏看到的 `microedition.platform` 一直是 `system.config` 里的 `j2me`，
  而 `internal.config` 里那两行 `PS Vita` / `Sony` 是**死值**（只影响内部属性查询）。
- 只读 jar 的 `MANIFEST.MF`，**不读 JAD**；只启动 `MIDlet-1`，多 MIDlet 套件无法选择。

已实现（v02.84）：
1. 6 个内建画像（generic / nokia6230 / nokia_n95 / se_k800 / samsung_e250 / siemens_c65），
   每轮从 VPK 模板重建 `system.config`，替换 platform/profiles/locale 并追加 device.*。
2. 画像同时提供默认虚拟屏尺寸（game.cfg 未显式指定时生效）。
3. `game.jad` / `<jarbase>.jad` 作为 `runMidlet` 的 arg0（`CldcMIDletSuiteLoader` 按后缀分流）。
4. `MIDlet-1..N` 从 JAD（优先）或 MANIFEST 解析，菜单里可切换。
5. `game.cfg` 新增 `profile=` / `locale=` / `midlet=` / `jad=`。

未实现（仍待做）：
- 用户自定义画像（目前是编进二进制的 C 表）。
- `MIDlet-Jar-URL` / `MIDlet-Jar-Size` / JAD 签名校验（不安装，直接运行）。
- 按 jar 名/SHA1 自动套用推荐配置的兼容性数据库（见 P1-6）。
4. 多 MIDlet：解析 `MIDlet-1..N`，菜单里让用户选择（默认第一个）。
5. 平台属性与厂商类可用性一致（伪装 Nokia 时 `com.nokia.*` 必须可用，反之亦然）。

改动点：
- `vita-port/src/vita_menu.c`（解析 manifest/JAD、选择 MIDlet、写配置）
- `vita-port/src/vita_main.c`（profile→internal.config、启动参数 `.jad` 或 jar）
- `vita-port/config/profiles/*.cfg`（新增）
- 不改 MIDP 共享源码；`runMidlet` 已支持 `.jad` 作为 arg0（需实测确认 phoneME 的 `CldcMIDletSuiteLoader` 路径）。

验收：同一 jar 在 generic/Nokia/SE 三种 profile 下 `System.getProperty` 返回值正确；多 MIDlet jar 能选第二个启动。

#### P0-3 输入覆盖：虚拟键盘 + 触屏映射 🚧 v02.85（第一片）

现状（v02.85 前）：12 个物理键可重映射（`UP/DOWN/LEFT/RIGHT/CROSS/CIRCLE/SQUARE/TRIANGLE/L/R/START/SELECT` + 右摇杆），数字键只能靠重映射，无法同时覆盖。触摸只发指针事件。

已实现（v02.85）：
1. 原生虚拟键盘覆盖层：3×4 数字面板（0–9、`*`、`#`）与动作面板
   （d-pad、A/B/C/D、S1/S2），绘制在游戏画面之上。
2. **优先使用信箱边栏**：非 16:9 的游戏在 960×544 面板上两侧有边栏，
   键盘放进去即不遮挡游戏；铺满时才 CPU 混合叠加（显示忽略 alpha）。
3. 触摸命中虚拟键 → 转成普通 `KEYMAP_KEY_*` 事件，下游完全透明。
4. 开关：`设置 > 虚拟键盘`（`vkey.cfg`），`game.cfg` 可用 `vkey=on|off` 逐游戏覆盖。
5. `mode`：0 自动 / 1 始终叠加 / 2 仅边栏（目前只能改文件）。

未实现（P0-3 剩余）：
- 多点触控 / 组合键（面板只报主手指，当前一次一键）。
- 用户自定义位置、大小、透明度、布局。
- 触摸→指针 与 触摸→按键 的逐游戏切换。
- 连发/宏、按键重复（长按）。
- 数字面板以外的字母键（部分游戏要 `a`–`z` 做命名输入）。

已补齐（v02.89）：
- **左右软键**：原来是动作块底部两个小 `S1`/`S2` 格子（全屏游戏时 34x23，
  几乎看不见）。现在用独立的 `layout_soft_keys()` 放在游戏矩形的**左上/右上角**
  （功能机经典位置），尺寸 96x44/96x40，标签 `L`/`R`，在所有分辨率与缩放模式下
  都不与其他面板重叠。
- **按下反馈**：被按住的按钮变成琥珀色底 + 白边 + 内嵌亮框 + 字形下沉 1 px
  （叠加模式下 alpha 150→225），触摸立即有可见响应，不用等游戏反应。
  状态由 `vita_input.c` 通过 `vita_vkey_set_pressed()` 推送。

目标：
1. 原生虚拟键盘覆盖层（与 `vita_menu.c` 同一套直接写 framebuffer 的方式），布局：
   - 3×4 数字键盘（0–9、*、#）
   - 方向 + 左右软键 + 确定
   - 游戏键 A–D
   - 自定义布局（每游戏保存）
2. 触摸映射：触屏区域→按键事件；或触屏→指针事件（每游戏切换）。
3. 连发/宏：按住连发、组合键（如 L+R+START 呼出菜单）。
4. 按键重复/长按（文本与列表滚动需要）。
5. 虚拟键盘透明度/位置/大小可调。

改动点：
- 新增 `vita-port/src/vita_vkey.c/.h`（绘制 + 命中测试），或扩展 `vita_menu.c`
- `vita-port/src/vita_input.c`（按键合成、ring 注入、配置持久化）
- `vita-port/src/vita_menu.c`（开关/布局编辑入口）

验收：一个只用数字键的 RPG/菜单类游戏，不改物理映射即可完整操作；所有缩放模式下触摸命中正确。

#### P0-4 音频格式与振动 ✅ v02.87/v02.88（代码完成，待真机验证）

现状（v02.87 前）：只有 tone-seq / WAV PCM / MIDI(SMF)。`g_caps` 与 `javacall_media_get_format` 仅三项（`vita_audio_javacall.c:247`、`:1096`）。`Display.vibrate()` 完全无效（`anc_st_vibrate.c` 的 `anc_start_vibrate()` 恒返回 `KNI_FALSE`）。

已实现：
1. **MP3**（v02.87）：内置 `minimp3`（CC0/public domain，单头文件）。逐帧解码 →
   最近邻重采样到 48 kHz 立体声，复用 WAV 渲染管线。`skip_id3v2()` 处理
   syncsafe size 与 footer。接受 `audio/mpeg`、`audio/mp3`、`audio/x-mp3`、
   `audio/mpeg3`、`audio/x-mpeg`。
2. **振动**（v02.87）：`vita_vibrate.c` 替换 `anc_st_vibrate.o`，
   `sceCtrlSetActuator(port=1, ...)`。**注意** Java 层
   `DisplayDeviceAccess.vibrate()` 第一句就用 `anc_stop_vibrate()` 当能力探测，
   所以它必须在"没在振动"时也返回 TRUE。掌机无马达 → 锁成不支持。
   开关：`设置 > 振动`（`defaults.cfg` 的 `vibrate=`），`game.cfg` 可逐游戏覆盖。
3. **AMR-NB**（v02.88）：3GPP TS 26.104 浮点参考解码器（`src/amr3gpp/`，
   Unlicense）。存储格式 `#!AMR\n`，帧长表逐帧解析；输出 ×6 升采样到 48 kHz。
   vendor 代码打了 4 个补丁（calloc / memmove / cos_table 钳位 / int32_t）。
4. **AAC（ADTS）**（v02.88）：`sceAudiodec` 硬件解码，一次性探针；模块缺失
   （多数 Vita3K）→ 静默退回"不支持"，不崩。
5. **seek**（v02.88）：`setMediaTime()` 对 MP3/AMR/AAC 生效（排队到播放线程
   在块边界应用）。AMR 精确到 20 ms 帧，MP3/AAC 为向前扫描（误差 < 1 帧）。
6. **内存**（v02.88）：压缩流前 512 KB 留内存，其余溢写到
   `ux0:/data/J2ME00001/.pcm<player_id>.tmp`，解码器经 32 KB 窗口读。
   常驻内存与文件大小无关（3 MB BGM 不再占 3 MB 堆）。MMAPI 的
   `nPrefetch()` 语义决定了无法真正边下边播。
7. **Nokia 铃声**（v02.88）：`nokia_src/RingTone.java` 把 OTA（Smart
   Messaging 位流）/ RTTTL 转成 8 kHz 8-bit WAV。不能解析的数据仍保持
   `SOUND_UNINITIALIZED`（与以前一致），不会抛异常。
8. **音量/静音**：`setVolume(mute)` 已生效（静音时仍按实时节奏 sleep）。
9. **OGG Vorbis**（v04.45）：`stb_vorbis` v1.22。与 MP3 同一套 vtable 与重采样路径。
   **整文件进 native 堆**（≤ 8 MB）——v1.22 只有 `open_memory`/`open_filename`/`open_pushdata`，
   而随机 seek 只有 `open_memory` 提供，pushdata 是单向前进。
   宿主端到端实测（维基 `Example.ogg` 104,793 B）：6104 ms / 44100 Hz / 2 ch，
   总输出 292,994 帧（= 6104 ms × 48 kHz），seek/rewind 逐帧可复现。

仍缺：
- **XMF/MMF/SMAF**：**不做**。SMAF 是 Yamaha 的合成器格式，播放需要波形音色库 + 专用合成硬件，
  没有可移植的开源实现；做成"能播"等于重写一个软件合成器 + 抓一份音色库，投入与收益完全不成比例。
  （记在这里是为了不让它每隔几轮被重新提一次。）
- **AMR-WB**、AAC-HE/SBR、AAC 的 MP4 容器。
- OTA 的 pattern(0x00) 指令（当作结束，会丢重复段）。
- WAV 仍整段常驻内存（32 MB 上限）；MIDI 不可 seek。
- 音频焦点（呼出菜单/IME 时压低音量）；振动强度可调；`Display.flashBacklight`。

验收：10 个 MP3/AMR 样本、5 个游戏实机播放无爆音/无欠载；`Display.vibrate()` 生效。
（代码完成、宿主机单测通过；**未上真机**。）


#### P0-5 图片解码：JPEG ✅ v02.89（代码完成，待真机验证）

现状（v02.89 前）：`USE_JPEG=false`（`phoneme-midp/build/vita_arm/Settings.gmk`）。
MIDP 规范只要求 PNG，但厂商 API/游戏资源常用 JPEG，于是
`Image.createImage()` 对 JPEG 直接抛 `IllegalArgumentException`。

已实现：
1. `ENABLE_JPEG=1`（`build_vita.sh` + `Settings.gmk` 的 `USE_JPEG=true`）。
2. `vita-port/src/vita_jpeg.c` 实现 phoneME 的 `JPEG_To_RGB_*` 接口，
   底层 **stb_image v2.30**（public domain，`STBI_ONLY_JPEG`，TEXT 只涨 ~35 KB）。
   上游 C 代码零改动——电话ME 只在 `imgdcd_image_util.c` 里通过
   `#if ENABLE_JPEG` 调用这个接口。
3. 支持 baseline + progressive；输出 RGB565（MIDP 的
   `b | (g<<5) | (r<<11)`）或 32 位；目标矩形与图像尺寸不同时最近邻缩放
   （`DirectUtils.createImage` 用得上）。
4. **JPEG 识别从 `FFD8FFE0` 放宽到 `FFD8FF`**（Java 与 native 两处同步），
   这样 EXIF 起头或没有 APP0 的合法 JPEG 也能解码。
5. 坏图不 `abort()`（`STBI_ASSERT` 置空），失败仍走原有
   `IllegalArgumentException` 语义。

验证：宿主机单测 20 断言 ALL PASS；与 JDK ImageIO（独立 IJG 实现）逐通道
比对 127626 个通道仅 401 个差 1（0.3%，IDCT 舍入）；PNG 路径未触碰。

仍缺：
- 没有图片缓存（`USE_IMAGE_CACHE=false`），每次 `createImage` 都重新解码。
- 不做 JPEG 编码（MIDP 不需要）。
- CMYK JPEG、12-bit 精度不支持（stb_image 的限制）。

验收：含 JPEG 背景/头像的游戏能显示；PNG 无回归。（代码完成、宿主机验证，
**未上真机**。）

#### P0-6 权限与签名 ✅ v02.90（默认策略完成，待真机验证）

现状（v02.90 前）：`system.config` 引用 `_policy.txt` / `_function_groups.txt`，
但 VPK 内没有这两个文件，而且没有任何代码读这两条属性——实际生效的是
`generic_policy_load.c` 的硬编码表。**未签名 jar** 拿到的默认值是
`net_access`=ONESHOT、`read/write_user_data_access`=ONESHOT、
`multimedia_recording`=ONESHOT；`SecurityHandler.checkPermission()` 对
ONESHOT/SESSION 返回 -1（unknown），于是第一次联网/读文件就弹**模态对话框并
阻塞 MIDlet 线程**——正是"权限弹窗卡住游戏"。

已实现：
1. `vita-port/src/vita_permissions.c` 实现 `generic_policy_load.o` 的全部 8 个
   符号；CMake 用 `ar d` 把上游对象从 `libobj.a` 删掉（零上游改动）。
2. **默认 RELAXED**：除 `minimum` 域外，所有域所有组一律 `ALLOW` →
   `checkPermission()` 返回 1，不弹窗。这也是 J2ME Loader / FreeJ2ME /
   KEmulator 的做法（J2ME 游戏是一份自带全部资源的玩具，不是有隐私预算的 App）。
3. **STRICT**：逐值复现上游两张表（`identified` / `unidentified`）；
   `设置 > 权限` 切换，写 `defaults.cfg` 的 `perms=strict`，
   `game.cfg` 也可逐游戏覆盖。
4. 域/组/成员三张表**逐字保留上游**——它们定义权限标识符，改名会让
   `Permissions.getId()` 抛 `SecurityException("Unknown permission")`。
   只有**值**不同。
5. 取证：`permissions_debug.log` 记录生效模式 + 每个安全域第一次被查询时的
   名字（`identified_*` = 签名通过，`unidentified_*` = 未签名/证书无效），
   `crumb.log` 里有一行 `perms: relaxed|strict`。
6. 删掉 `system.config` 里两条指向不存在文件的 `security.*` 属性。

验证：宿主机单测 43 断言 ALL PASS（含把上游两张表抄进测试做
16 组 × 2 域 × 2 级 = 64 个值的全量比对）；ELF 里只剩一份 `permissions_*`
符号，`libobj_no_main.a` 中 `generic_policy_load` 计数为 0。

仍缺：
- **签名 jar / 过期证书未实测**：RELAXED 下签名与否走同一条路（只影响日志里的
  域名），STRICT 下完全交给上游表。
- 严格模式的弹窗文案未本地化（`getGroupMessages` 返回 0 条，Java 回退用组名）。
- `perms` 没有逐游戏的 UI 入口（`game.cfg` 手写 `perms=strict` 有效）。

验收：未签名游戏访问网络/文件不弹窗；开启严格模式时行为符合 MIDP 规范。
（默认策略完成、宿主机验证，**未上真机**。）


#### P0-7 HTTPS ✅ v02.93（代码完成，待真机验证）

现状（v02.93 前）：`USE_SSL=false`，`https://` 根本不存在，所有联网 MIDlet
（浏览器、在线游戏、排行榜上报）在第一次调用就失败。

**核查结论（与原始规划不同）**：路线图建议"优先接 SceSsl"，实测两条路都堵：

1. **SceSsl 没有记录层**。`libSceSsl_stub.a` 只导出 init/term + 证书字段查看
   （SerialNumber/NotBefore/NotAfter/SubjectName/IssuerName/NameEntry*），
   **没有 open/read/write/close**，无法当 TLS 传输。
2. **上游自带 TLS 不能用**。`com.sun.midp.ssl.*`（restricted_crypto，约 3600
   行）只协商 SSLv3/TLS 1.0，公网服务器已全部拒绝。
3. **`sceHttp` 完整导出**（54 个函数），就是浏览器那套栈，做 TLS 1.2/1.3 +
   系统信任库校验，但工作在整笔事务粒度，不给你解密 socket。

已实现（v02.93）：
1. `USE_SSL=true`，上游 **2500 行 `https/Protocol.java` 原样编译**——chunked、
   重定向、keep-alive、header 解析、HttpConnection API 全部继续生效。
2. 只替换接缝 `com.sun.midp.ssl.SSLStreamConnection`（`vendor_src/`）：
   接受"写请求字节 / 读响应字节"的流式调用，底下用 SceHttp 完成事务。
   请求头解析、逐跳头过滤、chunked 请求解码、响应头重建都在 Java
   （`VitaTunnel.java`，可宿主机单测）；native 只搬字节
   （`vita_https.c` + `vita_https_kni.c`）。
3. 证书：SceSsl 拿不到 DER，且现代证书常无 CN，所以
   `getServerCertificate()` 返回合成 cert，SAN = 实际连接的主机名。
   真实链校验由 SceHttp 用系统信任库完成。
4. 降级：SceHttp/HTTPS 模块缺失（多数 Vita3K）→ `available()==0` →
   构造抛 `IOException("HTTPS not supported")`，不崩。

验证：宿主机单测 41 断言 ALL PASS（请求头过滤、chunked 解码、响应头重建、
端到端 GET/POST/chunked 上传、后端不可用、证书 SAN）；`nativeFunctionTable`
的 10 个函数名与 KNI 实现逐一相符；`midp_system.jar` 含
`com/sun/midp/io/j2me/{https,ssl}/Protocol.class` 与 `com/sun/midp/ssl/*`。

仍缺：
- 证书固定/自定义 CA（`sceHttpsLoadCert` 可用但未接）。
- HTTP 代理（上游分支保留，隧道直接连目标）。
- **`ssl://` 原始 TLS socket 协议不可用**（会当成畸形 HTTP → IOException）。
- 协议版本/CipherSuite 是常量（SceHttp 不报告）。

验收：UC/网页类 MIDlet 能打开 HTTPS 页面；HTTP 无回归。
（代码完成、宿主机验证，**未上真机**。）


### 4.2 P1：性能与体验

#### P1-1 JIT：把"已在二进制里"的性能拿回来

现状：
- `ENABLE_JIT := true`，ARM 后端存在；但 `VITA_JIT_DEFAULT 0`，`runMidlet` 注入 `-int`，实际纯解释器。
- 已知问题：**第 2 轮启用 JIT 会崩**，PC 落在 Java 堆内的 compiler area（`PROJECT_MEMORY.md` v01.45 后续）。

**三个障碍的现状（v03.71 取证后）**

| # | 障碍 | 状态 |
|---|---|---|
| ① | compiler area 在 USER_RW 堆内 → 取指 XN 权限异常（`pc==lr`、fault status `0x0d`、`FAR==pc`、WnR=0） | 待解，方案见下 |
| ② | `OsMisc_flush_icache` 是 `bx lr`（空） | **已根因查明**：VitaSDK 的 GCC 把 `__builtin___clear_cache` 整个折叠成空（`-O0` 是 `nop {0}`，`-O1`+ 直接 `bx lr`）。源码从来没写错。自写 `mcr p15` 不可行（PL1 指令，应用在 PL0）；用户态也没有任何缓存 API（`icache/dcache` 导出全是 `ksce*ForDriver/ForKernel`） |
| ③ | VM domain API 从未被本移植使用过 | **探针已就位**（`vita-port/src/vita_jitmem.c`，`jitprobe.cfg` 门控） |

**出路**：`sceKernelAllocMemBlockForVM`（≤16 MiB）+ `sceKernelOpenVMDomain()`
（*"makes all VM domain memblocks executeable"*）+ `sceKernelSyncVMDomain(uid,addr,size)`
（*"flushes Virtual Machine caches for the given memory location"*）——
这一组**同时**解 ①（XN）和 ②（flush_icache）。

探针四问（按序，任一步失败即停）：能否分配 → 关域能否写 → 开域+sync 后 `bx`
能否返回（`mov r0,#42; bx lr`）→ 开域后**还能不能原地 patch**（JIT 必须改写
`CompiledMethod_arm` 的 inline cache）。

目标：
1. 探针 VERDICT 通过 → 用 v03.11 的 overlay 机制替换 `OsMisc_flush_icache` 走
   `SyncVMDomain`，再**把 compiler area 从托管堆挪进 VM 块**（真正的工作量：
   GC 目前把 compiler area 当堆数据扫描/移动，要处理
   `save/restore_compiler_area_top` 与代码对象遍历）。
2. 修复后先做分游戏开关（`jit=0/1/2`），默认仍保守。
3. 调 `-Xjitthreshold` / 编译队列，避免启动期集中编译造成卡顿。
4. 建立性能基准 MIDlet（字节码密集 + 图形密集各一），量化收益。

风险：VM 深水区，时间盒管理；修不好就保持默认关，不能阻塞其他项。
**探针失败即在时间盒内止步**，不要等到投入两周后才确认不可行。

#### P1-2 启动白屏

现状：`round_begin → first_content_flip` 约 7–11 s；预载占大头（v02.70 已把 jar 整包读入内存，真机效果待验证；剩余 resolve + VM 启动 + 首帧）。

> **log120 实测（v02.94，两款游戏）**：preload `total_us=5.87 s / 7.32 s`，代码计时
> `io_open 2.13/4.13 s`、`io_read 0.43/0.60 s`、`resolve 2.05/4.07 s`；而 syscall
> delta 显示 **`io_read n=4121 us=4.78 s` / `n=4462 us=6.42 s`**（每次约 1.2 ms）。
> 同一份日志里 **`io_write n=5 us=31.8 ms`** —— 历史估计的"stderr `_IONBF` 约
> 0.7 s"自 v01.92 删掉每常量池项 CP dump 后就不成立了。真正的成本是**逐 entry 的
> `sceIoRead`**，不是日志。**但详见第 3 项：v02.70 本该已经把它变成内存读，log120
> 显示从第 1 轮起就退回了 stdio。**

目标：
1. ~~启动前绘制原生 splash/"载入中"并 flip~~ **v02.99 完成 + v03.00 修残影**：
   `vita_splash.c` 作为 `flip_to_display()` 的**覆盖层**绘制（深色底 + 游戏名 +
   阶段行 + 缓动进度条 + 「已载入 N 个类」），走**唯一**的
   `sceDisplaySetFrameBuf + swap` 路径；重绘由 `Universe::load_jar_entry` 驱动、
   限流 4 Hz；结束条件两重（预载完成后第一个"非自己请求"的帧 = MIDlet 在画；
   或首帧非白内容），另有 30 s 硬停。v03.00 补上**离开时擦黑边**（否则标题与进度条
   尾巴会永久留在游戏两侧）与超时后的补重绘。真机效果待验证。
2. ~~启动期 stderr 改行缓冲/降级日志（目前 `_IONBF`，历史估计约 0.7s）~~ **撤销**：
   见上，全程 `io_write` 只有 5 次 32 ms，没有可省的开销；改 `_IOLBF` 只会牺牲崩溃
   日志的最后一截（本项目多次靠 stderr 尾部定案）。
3. 预载策略 A/B：验证器已关，测量"预载 vs 懒加载"的首帧与首个卡顿。
   ~~着手点：合并逐 entry 的 `sceIoRead`~~ —— **v03.03 取证、v03.08 修掉**：v02.70 的
   整 jar 内存镜像在 **round 0 之后就失效了**。log123（首次带 v03.03 计数）给出：

   ```
   round 0 post: handles=0/8  refs=0/0
   round 1 pre : handles=8/8  refs=4/4  no_slot=30
   round 2 pre : handles=8/8  refs=4/4  no_slot=38
   ```

   8 个 jar 句柄在 round 0 泄漏且**永不归还**，8 个槽位表随即打满，于是从 round 1
   起每次 `OsFile_open("*.jar","rb")` 都静默退回 stdio（`BufferedFile` 缓冲仅 256 B、
   `find_entry` 与 `open_entry` 在同一 FILE 上交替 ⇒ 每个类约 8 次真实 `sceIoRead`
   ×1.2 ms）。其他计数器全为 0（`bypass/open_fail/skip/malloc`），所以**不是配额或
   malloc 问题，就是句柄表**。
   修法：句柄槽与镜像槽拆分、句柄 8→32（一个 handle 8 字节，共 256 B；内存仍由
   8 MB 镜像上限约束）；报告增加 `closed=` 与最多 4 条去重的
   `leak caller ra=0x…`（`addr2line` 减基址即可命名）；同时修掉
   `JarFileParser::get()` 的两趟重试——原来 `flush_caches()` 排在**最后一次尝试
   之后**，永远救不了当次调用。**泄漏本身是上游 VM 行为（weak 全局引用被回收时
   不调用 `parser().dispose()`），未盲改；等 `leak caller` 指认后再修。**
   预期：round 1+ 的 preload 回落到 round 0 的水平（round 0 的 `io_read` 只有
   0.43–0.60 s，因为镜像在内存里）。待真机验证。

4. 长期：把 `midp_system.jar` 中未进 ROM 的类并入 ROM（高风险，单独评估）。
5. 启动阶段计时保留在 `boot_timing.log`，作为回归指标。

验收：冷启动首内容 ≤3.5s（v02.80 基线的一半以内）；白屏一项（v02.99）已达成，
时间目标待第 3 项。

#### P1-3 渲染与 CPU 频率

现状：
- ~~未设 CPU 频率；默认可能 333MHz。~~ **v02.97 已改**：`vita_perf.c` 在启动早期
  请求 ARM 444 / BUS 222（GPU 默认不动），并把「boot 时钟 → 请求值 → 实际生效值」
  写进 `ux0:/data/J2ME00001/perf.log`。被内核拒绝不是致命错误，只留一行日志并沿用
  旧频率。开关：`ux0:/data/J2ME00001/clock.cfg` 的 `arm=` / `bus=` / `gpu=`
  （`0` = 不动）。**真机收益待实测**。
- 每帧整屏 RGB565→ABGR + 最近邻缩放（约 52 万像素），全在 VM 线程上做。

目标：
1. ~~`scePowerSetClockFrequency(444, 222, 166)`（若未设）~~ **v02.97 完成，
   但 v03.06 发现它从未真正生效**（VitaSDK 只导出分项 setter，故分别调用
   `scePowerSetArmClockFrequency` / `scePowerSetBusClockFrequency`）。
   log123 四次启动的行完全一致：

   ```
   [PERF] boot clock arm=333 bus=222 gpu=0 MHz
   [PERF] arm: set 444 MHz, now 333 MHz (rc=0)      ← 内核接受，频率不动
   ```

   v03.06 的应对：帧行改**实时读**时钟（原先进程内缓存，无法回答"后来有没有变"）；
   boot 行补 `xbar` / `ac` / `wireless`；**只在明文请求确实失败时**才升级尝试
   （A：`scePowerSetConfigurationMode(1)`→`(0)` 各试一次并重发 ARM 请求；
   B：先把 BUS/GPU/GPU-XBAR 抬到耦合值 arm/2 与 166 再重试 ARM）；每次尝试都记录
   rc 与读回值；末尾一行 `VERDICT` 直说成功还是失败及建议。`clock.cfg` 新增
   `mode=` 可强制某个配置模式。**等下一次真机日志点名哪种策略有效。**
2. ~~先加帧耗时统计（渲染 / 逻辑 / 音频），再决定优化点~~ **v02.97 完成（渲染侧）**：
   `flip_to_display()` 打三个时间点，`perf.log` 每 `stats_gap` 帧（默认 300 ≈ 5 s）
   输出一行：
   - `prepare` = 清边框 + 缩放 blit + 覆盖层（我们自己的 CPU 成本）
   - `flip`    = 入口到返回（再加提交与 vblank 等待）
   - `period`  = 相邻两帧间隔（真实 fps）

   判读：`period≈16.67` 且 `prepare` 很小 → 受 vsync 限制，优化 blit 无收益；
   `period≈30` 且 `prepare≈20` → blit 就是瓶颈，值得 NEON。逻辑/音频分项尚未做。
   行数上限 `stats_max`（默认 200）防止长时间运行写满 ux0。
3. 候选优化：
   - **先量（v03.45 已加）**：`vita_membw.c` 在 `lfjport_ui_init()` 里对**真实
     framebuffer** 测六种写法的带宽（32 位填充 / 64 位填充 / 库 memcpy / 手写 4 字
     复制 / 真实 rgb565→abgr 热循环分别写 cached 与 uncached），并换算成"一帧
     884 KB 要多少 ms"。判据：若 `@32b frame time` 接近 `perf.log` 的 `prepare`
     数值（log140 = 17.1 ms ⇒ 52 MB/s），说明 **blit 是内存带宽受限**，那么
     NEON 无用（ALU 不是瓶颈），正解是加宽 store 或先写 cached 中转缓冲再整块
     突发拷贝。
   - NEON 化 RGB565→ABGR 与最近邻采样（编译加 `-mfpu=neon`，热点手写 NEON）。
     **注意**：改 `-mfpu` 会动解释器的硬浮点 ABI（v03.11 的 overlay 专为此存在），
     必须先证伪"内存受限"再考虑。
   - GPU 缩放：把 240×320 RGB565 作为纹理，用 vita2d/GXM 一次绘制到 960×544，彻底移除 CPU 缩放。
   - 脏矩形：只重绘变化区域（对全屏游戏收益小，对 UI 类收益大）。
   - **不可行**：把 framebuffer 换成 cacheable（含 `CDRAM_L1WBWA_RW`）。v01.67 的
     真机黑屏正因如此：显示控制器直接读内存，CPU 脏行留在 cache 里。L1WBWA 名字里
     的 write-back write-allocate 就是同一个陷阱。
4. 注意与 IME 的 GXM 生命周期共存（v02.55/v02.69 已踩过坑）。

验收：2D 游戏 ≥55fps（444MHz 真机）；渲染耗时下降 ≥50%。

#### P1-4 堆与 GC

现状：Java 堆 32MB；48MB 曾在首启 hang（v01.28），根因未定。

目标：
1. 复测 48/64MB，分离 newlib 池与 Java 堆；必要时用 `sceKernelAllocMemBlock` 直接分配堆，绕开 malloc 碎片。
2. 每游戏堆上限配置（大游戏/小游戏）。
3. GC 日志与 OOM 处理：OOM 时给出明确提示而不是白屏。
4. 大 jar 资源的内存上限（jar 内存镜像上限 4MB/8MB，v02.70）。

验收：3 个大型游戏（>2MB jar）稳定运行 1 小时无 OOM/无碎片崩溃。

#### P1-5 音频 CPU 与并发

现状：MIDI 合成占音频预算约 40%（2.9–4.3ms / 10.7ms，32 复音）。

目标：
1. 复音上限/采样率按设备能力自适应；过载时降级而不是爆音。
2. MP3/AMR 解码尽量放 tone 线程，避免与 VM 抢核。
3. 音频优先级与 VM 线程亲和性复核（历史上有同核饿死教训）。

#### P1-6 每游戏设置 UI + 兼容性数据库

目标：
1. 每游戏设置页：屏幕尺寸、缩放、方向、profile、JIT、音量、虚拟键盘布局、触摸模式、locale。
2. 兼容性规则表（按 jar 文件名/SHA1/MIDlet 名匹配），首次启动自动套用推荐配置。
3. 导入/导出配置（备份整目录）。

#### P1-7 字体与本地化

现状：`vita_font.c` 是预烘焙位图字库（J2FB v2），CJK 20px、Latin 抗锯齿；`microedition.locale: en-US`。

目标：
1. `microedition.locale` 每游戏可设（zh-CN/ja-JP/en-US），影响游戏内语言与排序。
2. 字号缩放（小/中/大）与字库覆盖检查（生僻字缺字回退）。
3. 可选自定义 TTF 烘焙流程（离线生成 J2FB），不把 TTF 解析搬上机。

#### P1-8 生命周期与诊断

1. Vita 挂起/恢复：`pauseApp()`/`resumeApp()` 与音频暂停。
2. ✅ **应用内诊断浮层**（v03.70）：fps、帧周期、`prepare`（CPU 准备耗时）、
   ARM 实时时钟，一行屏显；`diag.cfg` 的 `detail=1` 再加 blit 耗时与
   rgb565→abgr 转换带宽。`SELECT+L` 开关，缺省可见。
   放 (2,2) 的 300 px 横条，竖屏下整个落在左侧信箱黑边里，不压游戏像素；
   横屏/拉伸时会压到，所以必须可关。详见
   `vita-port/src/vita_diag.h` 与 `PROJECT_MEMORY.md` 的 v03.70 条。
3. 日志级别开关（默认少写，排障时开），避免 I/O 拖慢（历史上日志风暴多次致卡）。

#### P1-9 性能杠杆排序（先做低成本、高确定性项）

| 杠杆 | 预期收益 | 成本 | 风险 | 备注 |
|---|---|---|---|---|
| `scePowerSetClockFrequency(444,222,166)` | +25–33% 执行速度 | 1 行 | 低 | 先确认当前实际频率；注意发热 |
| GPU 缩放（vita2d/GXM） | 省 2–6ms/帧 | 3–7 天 | 中 | 与 IME 的 GXM 生命周期共存 |
| 启动 splash + stderr 缓冲 | 白屏 −30–50% | 3–5 天 | 低 | 不影响 VM |
| NEON 化 RGB565→ABGR / 绘制热点 | 绘制 1.2–2x | 1 周 | 低 | 先测热点 |
| 堆扩容/分配方式 | 减少 OOM 与 GC 停顿 | 3–5 天 | 中 | 48MB 曾 hang，需复测 |
| 音频流式解码 | 内存与首播延迟 | 1 周 | 低 | 不改 MMAPI Java 层 |
| JIT | **1.5–4x** | 2–4 周 | **高** | 二次启动崩溃未定，设时间盒 |

> 除 JIT 外，其余项都不碰 VM 解释器，适合与兼容性工作并行推进。

### 4.3 P2：长尾

| 项 | 说明 | 价值 |
|---|---|---|
| JSR-256 传感器 | Vita 有加速度计/陀螺仪，可映射到 JSR-256 | 少量体感游戏 |
| JSR-179 定位 | 无 GPS，可返回固定/模拟坐标 | 极少 |
| JSR-82 蓝牙 | 无真实 BT；可做本机 loopback/桩，避免直接抛异常 | 少数联机游戏 |
| server socket / UDP | `USE_SERVER_SOCKET=false`；UDP 模块存在 | 联机/局域网 |
| PushRegistry | 依赖后台/短信，Vita 上意义有限 | 低 |
| JSR-226 SVG | 需要 SVG 渲染器 | 低 |
| JSR-234 高级媒体 | 摄像头/录音不存在 | 低 |
| JSR-172 Web Services | XML/RPC，体积大 | 低 |
| JSR-280 XML | 可复用轻量 XML 解析器 | 低 |
| PIM | 联系人/日程，需自建存储 | 低 |
| 截图/录像/加速 | 前端功能 | 中（体验） |

---

## 5. 分阶段开发规划

> 周数为单人投入的相对估算；带 ★ 的可以并行。

### 阶段 0：度量与回归基线（0.5–1 周）

- 建立 `test/` 游戏矩阵（现有 4 个 + 补 10–15 个，覆盖：MIDP1、Nokia、Siemens、128×160、176×220、360×640、MP3、3D、网络、大 jar）。
- 采集指标：`round_begin→first_content_flip`、首帧 FPS、稳定 FPS、崩溃/挂死、音频欠载、OOM。
- 脚本化：安装→启动→运行 N 秒→退出→收集 `debug/logs`；失败自动留 core。
- 产出：`COMPATIBILITY_MATRIX.md`（游戏 × 能力 × 结果）。

**验收**：对 v02.80 跑完一轮，得到可对比的基线表。

### 阶段 1：显示与设备画像（2–3 周，P0-1/P0-2）✅ 代码完成，待真机验证

1. `game.cfg` 格式与读写（`vita_menu.c` + `vita_main.c`），兼容旧 `launch.cfg`。
2. 动态虚拟屏 + 缩放模式 + 触摸反变换（`vita_display.c`）。
3. profile 目录与 `internal.config` 生成（`config/profiles/`）。
4. JAD 解析、多 MIDlet 选择、安装流程扩展。
5. 每游戏设置页（先放屏幕/方向/profile 三项）。

**验收**：见 P0-1/P0-2；构建 `build.sh` 通过；4 个现有游戏无回归。

### 阶段 2：输入（1–2 周，P0-3）

1. 虚拟键盘覆盖层（绘制/命中/透明/位置）。
2. 触摸→按键 / 触摸→指针切换。
3. 连发、组合键、菜单热键。
4. 配置持久化与每游戏布局。

**验收**：数字键游戏可玩；所有分辨率触摸命中正确。

### 阶段 3：音频/振动/图片/网络（2–3 周，P0-4..P0-7）✅ 完成（v02.87~v02.93）

1. MP3（minimp3）→ PCM，接 `javacall_media_*`。
2. AMR-NB 解码（按样本决定是否首批）。
3. 流式解码与内存上限。
4. 音量/静音/音频焦点。
5. 振动（`anc_*_vibrate` + `sceCtrlSetActuator`）。
6. JPEG 解码。
7. 权限默认策略（v02.90）：未签名 jar 全 `ALLOW`，不再弹模态权限框阻塞游戏；
   `设置 > 权限` 可切严格模式。
8. HTTPS（v02.93）：`USE_SSL=true` + 自实现 `SSLStreamConnection`，底层走
   SceHttp（TLS 1.2 + 系统信任库）。上游 2500 行 HTTP 逻辑原样复用。

**验收**：音频样本 + 5 游戏；振动；JPEG 游戏；联网/读文件不弹权限框；
HTTPS 页面能打。

### 阶段 4：性能（3–6 周，P1-1..P1-5，可与 3 并行）

1. 帧耗时/FPS 浮层与日志（先度量）。
2. 444MHz + 稳定性。
3. JIT 根因（时间盒 2 周），失败则保持默认关并记录。
4. 渲染优化：先 NEON，再评估 vita2d/GXM 缩放。
5. 启动 splash + stderr 缓冲 + 预载 A/B。
6. 堆扩容/分配方式/GC 调优。

**验收**：启动 ≤3.5s；2D ≥55fps；jit=2 连续 10 轮进出不崩（若修成）。

### 阶段 5：3D（4–8 周，P0/P1 之间）

1. ✅ 抽象渲染后端：`vita_gl3d.c`（vitaGL 离屏 FBO）/ `vita_sw3d.c`（软件光栅），
   经 `vita_3d.c` 统一分发，`m3g_native.c` 是 KNI 缝。
2. 🟡 **JSR-184 (M3G)**：31 个 API 类 + `M3gReader`（.m3g 解析）+ `Renderer`。
   已通：多 submesh Mesh、索引、顶点缩放/偏移、SkinnedMesh（Java + 可选原生）、
   Sprite3D、AMBIENT/DIRECTIONAL/OMNI 光照、材质、单层纹理、三种相机、
   AnimationTrack 采样、**CompositingMode 全部状态（v04.02）**、
   **调色板纹理（v04.06）/ 三遍对象排序（v04.08）/ 无 UV 时解除纹理（v04.12）/
   `PolygonMode` 全常量与 `Light.SPOT`（v04.17）**。
   **v04.41–04.44 补齐的**（本节此前一直写着"仍缺"，已过时）：`Fog` 真渲染（两条后端）、
   **硬件路径 alpha test**（v04.42 纠正 v04.02 的误判：GLES1 本来就有 `glAlphaFunc`）、
   `MorphingMesh` 权重（v04.43，Java 层实现，两后端共享）、`Appearance.layer` 绘制顺序 +
   跳过 `layer == -1`（v04.44，稳定排序）。**M3G 1.1 的 API 面到此收尾。**
   三条**作废**项（当时凭记忆写错，M3G 1.1 里没有入口）：`Background.SPHERE/CUBE`（属 2.0）、
   多纹理（1.1 只允许 1 层）、`PolygonMode` 的 POINT/LINE（常量在，但没有 `setFillMode`）。
   **仍然缺**（按对真实游戏的影响排序）：
   - **头号阻塞不是 M3G，而是 JIT 的浮点路径**（2026-11-06 log191）：同一批模型、同一版本，
     14 MB 堆（JIT 开）时 `skin bind` 报 38/26/31/26/21 根奇异骨骼、`drawMesh` 273–469 ms/帧；
     32 MB（JIT 被关）时奇异数全 0、24–38 ms/帧。`skin bind` 是纯 Java（`Matrix4.invert`），
     数据相同结果不同 ⇒ JIT 生成的代码算错浮点。v04.53 修了两处（`ldc2_w` 高低字、
     `double_constant_fold` 符号位）但**未证明能解决**；`JitSelfTest` 的 20 个探针就是为定位它加的。
     **在这一条解决前，"3D 做完了"不成立** —— 模型是对的，画出来是错的。
   - `Light.SPOT` 的**锥形衰减**（常量已有、`setMode` 已接受，着色暂按 OMNI）
   - `Graph` 加载契约（v04.27 已保证单根文件 `[0]` 是根；多根文件按文件顺序）
   - `Object3D.duplicate()` 对 **World** 的处理（v04.25：走 Group 分支，`activeCamera`/
     `background` 丢失；样本集无标题用到）
3. ⬜ **Mascot Capsule V3**（`com.mascotcapsule.micro3d`）：**代码零行**，日本游戏常见。
   需先把 V3 的 API 面（`Figure`/`FigureLayout`/`Effect3D`/`Texture`/`Graphics3D`）照 Javadoc
   补齐，再接现有后端。**这是 3D 剩余工作里最大的一块。**
4. 🟡 **v10 J3D**（`com.vodafone.v10.graphics.j3d`）：v04.40 已落地 11 个类型（API 面完整），
   但**模型格式（Aplix M3D）不公开 ⇒ 只画占位、不能渲真实模型**；规范也没公开"MIDlet 如何
   获得 `Graphics3D`"，用其它获取方式的游戏仍拿不到实例。VSCL 的
   `com.vodafone.v10.graphics.sprite` 同样未做（8×8 tile 硬件引擎 + 私有格式，刻意不做）。
5. ⬜ JSR-239 OpenGL ES 1.x：Java 绑定映射到 vitaGL。

6. ✅ 软件回退：`vita_sw3d.c`，含深度缓冲、五种混合方程、逐像素 alpha test。

**验收**：至少 3 个 3D 游戏能进入并渲染；性能 ≥20fps（先求对，再求快）。

### 阶段 6：网络与长尾（持续）

- HTTPS（SceSsl）、证书、重定向/chunked。
- server socket/UDP、PushRegistry。
- JSR-256/179/82（桩或模拟）、JSR-226/234/172/280。
- 截图/录像/加速/存档。
- 兼容性数据库持续扩充。

---

## 6. 关键设计取舍（推荐方案与理由）

| 议题 | 推荐 | 理由 |
|---|---|---|
| 屏幕尺寸 | 动态缓冲 + 每游戏 `game.cfg` | 不改 MIDP 共享源码；`gxj_system_screen_buffer` 本来就在启动前可设（orientation 已有先例） |
| 设备身份 | `profiles/*.cfg` 生成 `internal.config` | 沿用现有 pcsl 属性读取链，零 MIDP 改动 |
| JAD | 安装时保存，启动时作为 arg0 传入 | phoneME loader 原生支持 `.jad`，避免在 Java 侧伪造属性 |
| 虚拟键盘 | 原生覆盖层 + 合成 `KEYMAP_KEY_*` 注入现有 ring | 不改 MIDP 输入链；与按键重映射共用一套事件 |
| MP3 | 原生解码成 PCM，Java 层当 WAV | `javacall_media` 已有 PCM 管线；避免改 MMAPI Java 类 |
| 振动 | 实现 `anc_jc_vibrate` 的 javacall 后端 | 走 MIDP 既有 `Display.vibrate` 调用链，改动最小 |
| JIT | 先修二次启动，再分游戏开关 | 默认开风险太大；`jit=1` 已能拿回首轮性能 |
| 3D | 一个后端，多 API 前端 | 避免为每个厂商 3D API 写一套光栅化 |
| HTTPS | SceSsl 优先，mbedTLS 备选 | 少一个第三方库，利用系统 TLS；体积/内存可控 |
| 日志 | 运行期级别开关，默认精简 | 历史多次因日志 I/O 造成卡顿/白屏 |

---

## 7. 验收指标（建议写进 CI/回归脚本）

| 指标 | v02.80 基线 | 目标 |
|---|---|---|
| 冷启动首内容 | 7–11 s | ≤3.5 s |
| 2D 游戏稳定 FPS | 未系统测量 | ≥55fps（444MHz 真机） |
| 渲染耗时/帧 | 未测量 | ≤4ms（960×544 输出） |
| 游戏矩阵通过率 | 4 个样本 | 20 个样本 ≥80% 可玩 |
| 崩溃/挂死 | 历史多次 | 连续 8 小时 soak 无崩溃 |
| JIT 稳定性 | 默认关 | jit=2 连续 10 轮进出不崩（若达成） |
| 音频欠载 | 未测量 | 30 分钟 MP3/MIDI 0 欠载 |
| 内存 | 32MB Java 堆 | 大游戏可配 48/64MB，无 OOM |

---

## 8. 风险与边界

1. **JIT 是深水区**：v01.45 的崩溃根因未定，可能牵涉 GC/compiler area 跨轮生命周期。设时间盒；修不好不阻塞其他项。
2. **3D 工作量最大**：JSR-184 规范庞大。先按真实游戏样本裁剪子集，不要追求全规范。
3. **内存天花板**：3D 纹理 + MP3 缓冲 + 大屏缓冲会挤压 128MB 应用分区；优先用 CDRAM，设置硬上限与失败回退。
4. **真机 ≠ Vita3K**：显示/线程/网络/音频在模拟器与真机差异大，所有结论以真机日志为准。
5. **许可证**：厂商类是 Apache-2.0（已核对）；3D/解码器引入前确认许可证兼容（minimp3 CC0、stb_image MIT/public domain、opencore-amr Apache-2.0）。
6. **不要改共享骨架**：新能力优先落在 `vita-port/`（新文件）；必须改 `phoneme-midp`/`phoneme-cldc` 时用最小补丁 + `#if defined(VITA)`，并记录到 `PROJECT_MEMORY.md`。
7. **当前环境无法联网**：本文件对竞品的描述来自公开资料与既有实现认知；涉及具体版本的差异需在上网环境复核。

---

## 9. 建议的下一轮（可直接开工的三件事）

1. **每游戏 `game.cfg` + 动态虚拟屏**（`vita_display.c` / `vita_main.c` / `vita_menu.c`）
   - 先支持 `240x320`、`320x240`、`176x220`、`360x640` 四个预设 + 自定义。
   - 同步 `internal.config` 与触摸反变换。
2. **虚拟数字键盘覆盖层**（新增 `vita_vkey.c/.h` + `vita_input.c`）
   - 3×4 数字 + 四方向 + 两软键；触摸命中注入 `KEYMAP_KEY_*`。
3. **MP3 + 振动**（`vita_audio_javacall.c` + 新 `vita_mp3.c`；`anc_*_vibrate`）
   - 先做 MP3→PCM 复用 WAV 管线，再做振动。

附带零成本项：
- 启动即 `scePowerSetClockFrequency(444, 222, 166)`（若未设），实测稳定性。
- 加 FPS/帧耗时浮层，为后续优化建立数据。

---

## 10. 附：证据索引

| 主题 | 文件 |
|---|---|
| JIT 开关与策略 | `vita-port/src/vita_main.c:99`、`phoneme-cldc/build/vita_arm/vita_arm.cfg:288` |
| JIT 崩溃历史 | `PROJECT_MEMORY.md`（v01.45 后续、log36） |
| 预载与 jar 内存镜像 | `phoneme-cldc/src/vm/share/handles/Universe.cpp:1094`、`phoneme-cldc/src/vm/os/vita/OsFile_vita.cpp` |
| 虚拟屏与缩放 | `vita-port/src/vita_display.c:38`、`:104`、`:292` |
| 按键/触摸 | `vita-port/src/vita_input.c:96`、`:424` |
| 系统 IME | `vita-port/src/vita_ime.c` |
| 音频格式 | `vita-port/src/vita_audio_javacall.c:247`、`:1096` |
| 网络/SSL | `vita-port/src/vita_net.c:1`、`phoneme-midp/build/vita_arm/Settings.gmk` |
| JSR 开关 | `phoneme-midp/build/vita_arm/Settings.gmk`、`phoneme-midp/build_vita.sh` |
| 堆 | `vita-port/src/vita_pcsl.c:2224`、`vita-port/src/vita_main.c:85` |
| 厂商 API | `vita-port/nokia_src/vendor/` |
| 3D 原型 | `vita-port/src/vita_gl3d.c`、`vita-port/src/vita_gl3d.h` |
| 启动器设置 | `vita-port/src/vita_menu.c:1580` |
| 运行配置 | `vita-port/src/vita_main.c:1`、`vita-port/config/internal.config` |
