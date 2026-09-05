# J2ME/MIDP on PS Vita - Project Memory
> Last Updated: 2026-08-30

## 2026-08-30 缺口分析与 vita-port 重建（重要！）

### 功能缺口分析结论（当天完成，详见对话记录）
游戏三链路断裂的根因全部定位：
- **渲染链唯一断点**: `libobj.a` 的 `lfjport_fb_export.o` 里 `lfjport_refresh()` 是空 TODO——
  整条 gxj 渲染管线画进 `gxj_system_screen_buffer` 后无人上屏。
  链路: Graphics native → gxj_system_screen_buffer → DisplayDevice.refresh0 → lcdlf_refresh → lfjport_refresh(空)。
- **输入链唯一断点**: `libobj.a` 的 `mastermode_export.o` 里 `checkForSystemSignal()` 是空函数——
  VM 周期回调 `midp_check_events()` 轮询到它什么都不做。
  正规链路: checkForSystemSignal 填 MIDP_KEY_EVENT+UI_SIGNAL → midpStoreEventAndSignalForeground → Java EventQueue。
  键码用 `keymap_input.h` 的 KEYMAP_KEY_* 值（'0'-'9'、UP=-1..SELECT=-5、SOFT1/2=-6/-7、GAMEA-D=-13..-16）。
- **旧自研代码是死代码**: `phoneme-midp/src/porting/vita/*` 的 KNI 名与 Java 侧 native 声明不匹配，
  且双 framebuffer（vita_fb vs gxj_system_screen_buffer）从未汇合。已放弃，不再演进。
- 其余缺口: 无音频（JSR-135 关+无 SceAudio 代码）、仅 5x7 ASCII 字体、网络全 stub、
  internal.config 的 display 尺寸配置（240x320 虚拟屏需同步三处：lfjport、config、gxj buffer）。

### vita-port/（新的权威移植层目录）
**所有新代码只写 `samples/j2me/vita-port/`，phoneme 两树零改动**。构建方式：链接前
`ar d libobj_no_main.a lfjport_fb_export.o mastermode_export.o runMidlet_md.o`，
再链入本目录实现（`--allow-multiple-definition` 下先到的 .o 赢）。
- `src/vita_display.c`: lfjport 22 符号全覆盖；240x320 虚拟屏 → 960x544 最近邻缩放居中 blit（RGB565→A8B8G8R8）
- `src/vita_input.c`: 真 checkForSystemSignal + 16 槽 SPSC 事件环 + SceCtrl 边沿检测
- `src/vita_main.c`: 启动器，读 `ux0:/data/J2ME00001/launch.cfg`（首行 jar 路径、次行 MIDlet 类名）运行任意游戏
- `src/vita_pcsl.c`: 复制自旧 vita_stubs.c（sceIo 文件 I/O 真实现，socket 仍 stub）
- `config/internal.config`: 改为 display 240x320 后随 VPK 打包
- `midlets/`: HelloMIDlet + CanvasTest（渲染验证）+ InputTest（按键验证）
- 构建: `./build.sh`（`clean`/`midp` 参数）；产物 `build/cmake/midp_vita.vpk`
- 构建已验证: 链接成功、覆盖符号已确认在最终 ELF（checkForSystemSignal=0x160 新实现、
  lfjport_refresh 尾调用 flip_to_display）、VPK 含 240x320 config；**Vita3K 运行验证待做**
- **Vita3K 首次运行反馈 (8-30)**: 能启动，屏幕只显示 Form 的 softbar "Exit"，标题/正文空白。
  根因: `midp_system.jar` 里没有任何 PNG——chameleon 皮肤资源
  (`src/highlevelui/lcdlf/lfjava/resource/`，178 张 skin PNG + indicator) 没进 classes/ 目录，
  CMake 的 `jar cf -C classes .` 自然打不进去。皮肤加载失败 → 布局字段未初始化 → Form 主体不可见。
  修复: vita-port/CMakeLists.txt 的 jar 命令追加
  `-C ${MIDP_DIR}/src/highlevelui/lcdlf/lfjava/resource .`（jar 878KB→1.09MB，183 张 PNG）。
  渲染链已被此次运行间接验证（softbar 上屏=字体+blit+refresh 都工作）。

### 当前构建管线事实（2026-08-30 起，取代早前 ROM 化描述）
- 现行链路是**非 ROM 化**的：CLDC 库 `dist/lib/libcldc_vm*.a`（8-29 20:01）+ MIDP `obj/arm/libobj.a`
  （145 成员）→ midp-vita 式 CMake 链接 → 类从 VPK 内 `midp_system.jar` 文件系统加载
- 旧 8.2MB `libcldc_vm_midp_g.a` 已不存在；`USE_ROM=true`/ROMImage 路线当前未启用
  （`build/vita_arm/ROMImage_*.cpp` 8-30 00:02 生成过，但未链入现行库）

### 网络层落地（2026-08-30，vita_net.c，已构建验证）
- **`src/vita_net.c`**（~800 行）: pcsl socket/server-socket/network/datagram 全量真实现，
  替换原先 vita_pcsl.c 里的 stub 段（已删）。
  - Vita 侧 BSD socket 走 newlib libc.a 里的包装（内部是 sceNet），无 `sys/ioctl.h`，
    `available()` 用 MSG_PEEK 1 字节探测实现
  - 非阻塞 connect: EINPROGRESS→PCSL_NET_WOULDBLOCK；配套 `vita_net_poll()`（select 全 fd 集）
    在 `vita_input.c` 的 `checkForSystemSignal()` 里被周期调用，就绪时发
    NETWORK_READ/WRITE/EXCEPTION_SIGNAL 唤醒 `midp_thread_wait` 阻塞的协议线程
  - NetInit: `vita_net_early_init()`（sceSysmoduleLoadModule(NET)→sceNetInit 64KB→sceNetCtlInit），
    在 vita_main.c 最先调用；幂等（g_net_up）
  - 错误码映射 EWOULDBLOCK/EAGAIN→WOULDBLOCK，其余→IOERROR；`pcsl_lastNetworkError` 对齐上游
  - datagram 读写签名为 `char *buffer`(pcsl_datagram.h 原样)；addrToString 用 plain malloc
    （libpcsl_memory.a 只导出 pcsl_memory_alloc/free，与 pcsl_mem_malloc 同义不同名）
- CMake: 加 `src/vita_net.c` + `SceNet_stub SceNetCtl_stub`；libpcsl_network.a 实际零导出符号，
  无重复定义风险；velf nm 验证: 114 个 `pcsl_*` T 符号全部来自 vita_net.o
- **HTTP 即免费获得**: com.sun.midp.io.j2me.http 是纯 Java，跑在 socket:// Protocol 上
- **NetTest MIDlet**（midlets/NetTest.java，MANIFEST MIDlet-4）: HttpConnection GET
  http://example.com/，线程化，状态+响应头+正文前 128 字节上屏；build_jar.sh 已纳入
- 构建状态: exit=0，VPK 含 Hello.jar(NetTest.class)+midp_system.jar+178 PNG；**Vita3K 真跑待测**

### 按键 repeat + 音频结论（2026-08-30 续）
- **vita_input.c 增加 key-repeat**: PRESSED 后 400ms 起发 KEYMAP_STATE_REPEATED(3)，
  每 60ms 一次，RELEASED 清状态；vita_input_poll 两条路径（无变化/有变化）都调用 repeat_tick。
  构建过、velf 内 checkForSystemSignal/vita_input_poll 确认生效。
- **MMAPI 评估结论（重要，勿重复调研）**: 本构建是 CSF-lite，`javax.microedition.media`
  的 Manager 是**编译期死的**——javap 确认 createPlayer 无条件抛 MediaException、
  playTone 只做参数校验后 return、getSupportedContentTypes 返回空数组；
  libobj.a 中无任何 Java_com_sun_mmedia native。做音频 ≈ 从零移植 MMAPI native+Java，
  成本远超收益。**结论: 放弃 phoneME 体系内音频，游戏音效若需要走 future 原生 hack**。
  可按下 L/R+START 的原生菜单（vita_menu.c）承担声音开关。

---

## 项目概述

**目标**: 在 PS Vita 上运行 J2ME/MIDP 应用（Java ME）

**核心组件**:
- phoneME CLDC - Java VM 实现
- phoneME MIDP - MIDP 2.0 实现  
- Vita SDK 交叉编译工具链

**工作目录**: `/home/zyb/vitasdk/samples/j2me/`

---

## 项目结构

```
j2me/
├── phoneme-cldc/          # Java VM (CLDC 1.1)
│   ├── src/vm/
│   │   ├── share/handles/Universe.cpp    # HiddenPackage bypass
│   │   ├── share/ROM/                    # ROM 生成相关
│   │   └── os/vita/                       # Vita 原生代码
│   └── build/vita_arm/
│       ├── dist/lib/libcldc_vm_g.a         # 编译好的 VM 库
│       ├── dist/lib/cldc_classes.zip      # CLDC 类
│       └── romgen/generated/              # ROMImage_*.cpp
├── phoneme-midp/          # MIDP 2.0 实现
│   └── build/vita_arm/
│       └── classes/       # 编译好的 MIDP 类
│           ├── com/
│           ├── java/
│           └── javax/
└── midp-vita/             # Vita 启动器
    ├── src/main.c         # 入口点
    ├── build/             # VPK 输出
    └── build_midp_vita.sh # 构建脚本
```

---

## 技术细节

### 工具链
- 编译器: `arm-vita-eabi-gcc`
- 构建系统: CMake + 自定义 Makefile
- ROM 生成: `romgen` (32-bit x86 Linux binary)
- 环境: `/home/zyb/.local/vitasdk`

### ROM 生成流程
```
cldc_classes.zip + MIDP classes (com/, java/, javax/) + midp_system.jar + Hello.jar
    → min_rom.jar (合并，包含所有系统类)
    → dist/bin/romgen
    → ROMImage_*.cpp (19,567 objects, ~1.2MB)
    → libcldc_vm_g.a (链接，UseROM=true)
```

### UseROM 标志
- **关键**: `UseROM = true` 从 ROMImage 加载系统类，不再需要文件系统上的 midp_system.jar
- 配置位置: `phoneme-cldc/build/vita_arm/vita_arm.cfg`
- 启用条件: `ENABLE_ROM_GENERATOR = true`, `ROMIZING = 1`

### VPK 打包路径
```
data/J2ME00001/
├── midp_system.jar      # (嵌入 ROM，不再需要)
├── Hello.jar            # 用户 MIDlet
└── lib/
    ├── internal.config  # MIDP 内部配置
    └── system.config    # MIDP 系统配置
```

### 类加载路径
- ROM 包含: 所有 CLDC + MIDP + midp_system.jar + Hello.jar 类
- VM 启动: 从 ROM 加载 `com.sun.midp.main.MIDletSuiteLoader`
- 不再需要从文件系统加载

---

## 已解决问题 ✅

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| HiddenPackage error | JVM class 未标记 hidden | 修改 `Universe.cpp` 添加 VITA bypass |
| NoClassDefFoundError | midp_system.jar 未打包 | 嵌入 ROM |
| 多 JAR classpath | `:` 分隔符问题 | 修改 `getClassPathPlus()` |
| ROM skeleton | 未生成真实 ROM | 实现 romgen 集成 |
| 构建脚本混乱 | 单文件 | 拆分为模块化 |
| `app0:/u/` 路径错误 | `UseROM = false` 导致从文件系统加载 | 启用 `UseROM = true`，ROM 包含所有类 |
| ClassFormatError:154 | 用户 jar 未做 CLDC preverify | `build_jar.sh` 加 preverify 步骤（javac → preverify → jar） |
| IAE "does not support title" | `lfjport_get_display_capabilities` 返回 0；`lfjport_get_display_device_ids` 返回 NULL 但 KNI 会解引用 | `lfjport_fb_export.c` 返回 255（全能力位）；ids 返回静态数组 `{0}` |
| **VPK 内 ELF 陈旧（本次白屏反复的元凶）** | `libobj_no_main.a` 只经 `target_link_options` 传入，CMake 未注册文件级链接依赖，归档更新后 make 判定无需重链；`vpk` 目标链只依赖 `.self/.sfo` 也探测不到 | CMakeLists 给 midp_vita 加 `LINK_DEPENDS "${LIBOBJ_NO_MAIN}"`；**每次构建后必须 `arm-vita-eabi-objdump` 验证 ELF 内实际代码**。教训：改完源码后 ELF mtime 不变 = 链接没发生，产物不可信 |

---

## 当前状态 🔄

### 2026-09-05 v01.09 重发：上次归档拷错文件，用户从未运行过混合库
- **事故**：0904 归档 `midp_vita_v0109.vpk` 时 `cp` 的源是 `vita-port/build/midp_vita.vpk`（旧布局残留，Sep 4 08:57 = b159/fc3c534 世代），而真正 17:16 的混合库构建产物在 `vita-port/build/cmake/midp_vita.vpk`。**用户装的其实是 v01.08 世代二进制**——日志版本串 `b159 (fc3c534)` + CP tags 转储洪水与 v01.08 行为完全吻合（能进游戏、音乐开关卡死）。
- **教训两条**：
  1. **SELF/VPK 内的 eboot.bin 是压缩封装，strings 验证无效**——只有链接前的裸 ELF（`build/cmake/midp_vita`）可做字符串验证
  2. **版本串是防呆底线**：VITA_VERSION 停在 01.06 长期未 bump，导致用户日志无法区分二进制世代，事故漏网。以后**每次发布必须 bump VITA_VERSION 并核对日志版本串**
- **修复**：VITA_VERSION → 01.09；删除旧路径残留 `build/midp_vita.vpk`；build.sh 输出注明唯一产物路径。重链后版本串 `v01.09 b162 (5c96440)`（提交 5c96440 后重链）。
- **产物（最终版）**：`samples/j2me/midp_vita_v0109.vpk`，md5 `24d1033da5edda5c08ce022691c9f873`（14295657B）。ELF 终验：`81381b58 D jvm_fast_globals`、C 解释器符号 0、转储串 0。
- **测试判据**：装新包后日志第 4 行必须是 `version: J2ME Player v01.09 b162 (5c96440)`。若显示其他版本 = 装错/没覆盖。预期游戏正常进、音乐开关不再卡死、stderr 无 CP tags 洪水。

### 2026-09-04 v01.09：音频开关卡死根因 = KG 库带回类加载转储洪水；库合成（KG 骨架 + 无转储新世代 004/005 + 独立补丁成员）
- **用户实测 v01.08**：✅ 能正常进游戏（GP 表双定义根因闭合确认）；❌ 音乐开关处卡死无响应——"回到前几天的问题"。
- **根因（实测证据链）**：
  1. 音乐开关触发 MMAPI 类链首次加载 → **KG 库（8-31 世代）的 `_MergedSrc004.o`/`_MergedSrc005.o` 里类加载 stderr 转储还活着**（strings 实测：004 含 `CP tags`/`PRE idx`/`NOT in heap`，005 含 `var_oops_do`；9-4 世代库 = 0）→ Vita3K 慢速 I/O 下每类数千行 = "卡死"。这正是 9-2 cldc 2959a08（`VITA_CP_DEBUG` 门控三处转储）修掉的老病——**修复在源码里但不在 KG 二进制里**（KG 备份于 9-2 16:15，早于该源码修复进库的世代）。
  2. v01.08 恢复 KG 库 = 连同修复一起回退，老病复发。**源码树当前是干净的**（`ClassFileParser.cpp`/`ConstantPoolDesc.cpp` 转储均已被 `#ifdef VITA_CP_DEBUG` 门控）。
- **修复：库合成而非整库回退**（既保留 KG 世代"无 C 解释器"的正确骨架，又拿到无转储的类加载代码）：
  1. **KG 库为基础**（D 版 GP 表、无 C 解释器、事件泵修复都在，反汇编确认）
  2. **替换 `004`/`005` 为 9-4 世代的无转储版本**（来自 `libcldc_vm.a.cinterp_broken` 提取——这两个成员自身干净：0 转储串、0 C 解释器符号）
  3. **世代分组漂移补齐**：新世代把 SNI_*/StackmapGenerator*/fplib 从 004/005 挪进了 006（006 带 C 解释器不能整拿）→ 用 `tools/build_extra_o.sh`（006 头文件链 hpp-only + include 源文件 + 完整 PRODUCT 旗标）独立编译 7 个补丁成员补回：`sni.o`、`StackmapGenerator.o`、`Cosine/Sine/Tangent_kernel.o`、`JFP_lib_sin/cos.o`，外加 `vita_vm_clean_shutdown` 空 shim（真身在 Interpreter_c.cpp，仅设看门狗标志；KG 世代无看门狗，no-op 安全）
  4. **闭合验证**：71 个缺失定义全部闭合；库级符号闭合检查（`tools/symcheck.sh`）泄漏 111 个全是预期外部（JVMSPI/libc/_rom_*）；最终 ELF `81381b58 D jvm_fast_globals` ✅、C 解释器符号 0 ✅、转储字符串 0 ✅
- **产物**：`samples/j2me/midp_vita_v0109.vpk`（md5 `ed0d9563b2e0258f3134549d566f1f3e`，14308809B）。库双备份：`/tmp/libcldc_vm.a.V0109_HYBRID` + `dist/lib/libcldc_vm.a.v0109_hybrid`。
- **rebuild_vm.sh 已修正（本轮）**：①删除 `ENABLE_C_INTERPRETER=true`（根源矛盾开关，注释说明双定义机理）②`pack_lib` 加硬校验：`jvm_fast_globals` 必须恰好 1 个 D 定义、库内 0 个 C 解释器符号，违反即 die——防再犯。禁用令解除，但**运行后必须跑 `tools/symcheck.sh` 复验**。
- **工具沉淀**：`tools/build_extra_o.sh`（独立 TU 补丁成员编译：006 头链 hpp-only + PRODUCT ABI 旗标）、`tools/symcheck.sh`（库符号闭合检查）——从 /tmp 固化进项目。
- **测试预期**：v01.09 = v01.08 全部 + 类加载转储清除。音乐开关应不再卡死（Vita3K stderr 也不再刷 fd 0x7 洪水）。若音乐开关仍卡但无日志洪水 → 剩余嫌疑是 MMAPI Player 状态机（vita-port 层 `vita_audio_javacall.c`），届时看 `midp_stderr.log` 定位。

### 2026-09-04 v01.08：★ 回归根因闭合——C 解释器混入导致 GP 表双定义被静默消解（已恢复 8-31 世代 VM 库）
- **用户的判断是对的**：不是"音频改动"本身，也无需逐症状打补丁。8-31 可玩 → 后续崩溃的**真正回归点 = 9-2 01:20 的 `rebuild_vm.sh`（samples 0ff302d）**。
- **根因链（全部实测证据）**：
  1. **8-31 可玩世代（KG 库，`/tmp/libcldc_vm.a.KNOWN_GOOD`，9-2 16:15 备份）没有 C 解释器**——其 `_MergedSrc006.o` 无 `g_jpc`/`g_jpc`/`interpreter_dispatch_table` 符号；VM 跑 **ARM 汇编快速解释器**（`Interpreter_arm.o`），它定义 **`jvm_fast_globals` D 版（1036 字节 .data，r10 基址访问的 GP 指针表）**——这是唯一定义 ✅
  2. **9-2 `rebuild_vm.sh` 从第一版就自相矛盾**：`ENABLE_C_INTERPRETER=true` 把 `Interpreter_c.cpp`（定义 **B 版 `JVMFastGlobals jvm_fast_globals`**，176 字节零初始化 bss，`Interpreter_c.cpp:4860`）编进 `_MergedSrc006`，又从旧库提取 `Interpreter_arm.o`（D 版）塞回库 → **同名双定义**
  3. `--allow-multiple-definition`（8-30 8a为修双定义链接错误加的，当时无害）把冲突静默消解 → **最终 ELF 绑定 B 版**（曾实测 `81390564 B`，r5=0x8139255c 现场离它 +0x2000 越界）→ 快速解释器 GP 表全是零/垃圾 → 野跳进 ROM 字节码区滑行 → 崩。9-3~9-4 诊断的"jsr/ret 无实现"（0e8c406）、"dispatch 表污染"（ae48cdb）、"PC=0"（900dd85）、"ROM text 冒充 handler"（d206ff5）**全是这个双定义的下游症状**——C 解释器本来就不该在构建里，jsr/ret 在快速解释器里本来就有实现。
  4. 崩溃寄存器 ASCII 碎片（"skin"/"btn_"/"srcx"/"srcy"/"pngP"）= 滑行路过的 chameleon 皮肤字符串表（.rodata 0x812ee280+，"keyboard.btn_"、"btn_mid_sel"），佐证执行流失控范围。
- **修复**：恢复 `/tmp/libcldc_vm.a.KNOWN_GOOD` 为 `phoneme-cldc/build/vita_arm/dist/lib/libcldc_vm.a`（混装问题代备份为 `libcldc_vm.a.cinterp_broken`）；vita-port `./build.sh` 重链。**验证**：最终 ELF `81381ae4 D jvm_fast_globals`（D 版 .data 指针表胜出 ✅），`g_jpc`/`interpreter_dispatch_table` 符号数为 0（C 解释器不在 ✅）。
- **产物**：`samples/j2me/midp_vita_v0108.vpk`（md5 `0827c5011d9c9a198edd51197f5f7add`，14289566B）。
- **构建坑（再次踩中，务必牢记）**：
  - **CMake 库文件级依赖缺失**：库归档更新后 make 判定无需重链，ELF 陈旧——`rm build/cmake/midp_vita` 强制重链后必须 `arm-vita-eabi-nm` 验证绑定（v01.07 就是没验证发了陈旧包）
  - **javac 不在 PATH**：`build_jar.sh` 需要 `export PATH=/home/zyb/tools/jdk8u502-b07/bin:$PATH`
- **rebuild_vm.sh 必须修正后才能再用**（本轮未改，防再犯）：`ENABLE_C_INTERPRETER=true` 与"从旧库提取 Interpreter_arm.o"的组合 = 必然双定义。要么去掉 C 解释器（回到快速解释器，当前 KG 世代的做法），要么全量用重新生成的 Interpreter_arm.o（需先恢复 f2i/d2i stub）。**在修正前禁止运行 rebuild_vm.sh**。
- **9-3~9-4 的 C 解释器侧改动全部无效但无害**（jsr/ret 实现、undef_bc 安全网、面包屑环、看门狗、handler 窗口）——它们编在 `_MergedSrc006` 里，但该成员现在来自 KG 世代（无这些代码）；源码保留，不回滚（若未来切回 C 解释器可复用）。但注意 **OS_vita.cpp 的看门狗（900dd85）也不在了**（KG 世代的 OS_vita.o 无 watchdog 符号）——挂起型崩溃将无转储，只能靠 Vita3K 日志。
- **测试预期**：v01.08 = 8-31 世代 VM + 之后全部 vita-port/MIDP 层改进（音频 playTone 状态机、media 事件桥、RMS 隔离 groundwork、日志修复）。若仍崩，则根因在 vita-port/MIDP 层（音频线程、事件桥、RMS 隔离），需用 git 在 vita-port 提交线上二分（`b29d725` 之后的提交都只动 vita-port 层，二分成本低）。

### 2026-09-04 v01.07：dispatch 校验窗口改为表自身采样（拦截 ROM-text 冒充 handler）
- **01.06 实测崩溃形态（用户 vita3k.log）**：进游戏 35ms 后硬崩，PC=0x73726378（"srcx"）、r12=0x73726379（"srcy"+Thumb 位）、r4=0x50676e70（"pngP"）、LR=0x8112bc18（ROM text 内）、Thumb:true——**执行流滑进 ROM 字节码数据当指令执行，间接跳转目标被字符串碎片冒充**。
- **01.04 校验的漏洞（本轮核心发现）**：旧窗口 `[0x81000000,0x81400000)` **包含 ROM text 块（.rodata 内 0x810fbe58 起）**，被污染成 ROM 字节码地址（r2=0x8112bbf0）的表项顺利通过校验 → blx 跳进数据区滑行。01.03 的 0x125d1784（Java 堆）能拦，01.06 的 ROM 地址拦不住。
- **修复（cldc d206ff5）**：`Interpreter_c.cpp` 增 `interpreter_handler_min/max`，init 后从初始化完的表采样 min/max 作为唯一合法窗口。实测验证：bc_impl_* 全在 .text `0x8108fb54..0x810a519c`，ROM text 起 0x810fbe58——ROM 地址必然被拒，且无需硬编码链接边界（链接器可自由排布）。污染表项会在 blx 前触发 `bc_crash_report`（64 条面包屑 + slot/handler/legal window 全套诊断），而不是无声硬崩。
- **构建**：`rebuild_vm.sh` 全代重编（romgen 宿主端仍报错——ROMImage 无需重生成，Java 类未变；31 objects + 21 成员库 OK）。VPK md5 `d38a6c7fe1ab58219f1889b465fe4cb2`。
- **测试预期**：装 01.07 跑口袋灵兽。**两种结果都有信息量**：
  - 若出 `dispatch table entry outside handler window` 报告 → 表污染被拦，日志含 slot/handler/窗口——回传即可定位污染源；
  - 若直接正常进游戏 → 说明 01.06 的硬崩就是 ROM 地址骗过旧校验所致，窗口法闭环。
- **遗留**：若表项反复被污染，说明有越界写源头未除（01.03 的 0x5d1784 低 24 位与 01.06 皆指向同一污染源）——需在拦截报告后追查写入者（候选：某 bc_impl 的越界 store、或 GC/ROM 化类的静态初始化写穿）。

### 2026-09-04 v01.06：PC=0 崩溃机制修正 + 解释器看门狗线程（cldc 900dd85，待用户复测）
- **01.05 崩溃形态再定位（推翻 01.04 的"opcode 越界"结论）**：用户日志显示校验通过后 PC=0（取指错误自刷 53 万行，Vita3K 对 PC=0 只刷日志不杀进程）。反汇编证明分发循环 `blx r2` 前的 `ldrb r2,[r3,#-512]` 取 handler 正常——**表项未被破坏**；PC=0 是 handler 内部间接跳 0（r2 残留为证），即 bc_impl 内部控制流损坏，非分发表越界。01.04 的 handler 范围校验因此拦不住（handler 本身合法）。
- **看门狗方案（cldc 900dd85，3 文件 +147 行）**：PC=0 后解释器线程彻底失联，任何解释器内巡检都无法执行 → 改由独立线程兜底：
  - `Interpreter_c.cpp`：主循环心跳 `vita_interpreter_heartbeat++`（volatile）；`undef_bc_stop`/`bc_crash_report` 开头置 `vita_fatal_reported_flag`（防看门狗误报）；`vita_vm_clean_shutdown()` 供 JVM::stop() 正常退出时通知看门狗。
  - `OS_vita.cpp`：`Os::initialize()` 末尾启动看门狗线程（2000ms 检查间隔、30000ms 超时）；心跳冻结超时 → 调 `vita_interpreter_dump()` 转储面包屑环 64 条到 stderr（设备侧 `ux0:/data/J2ME00001/midp_stderr.log`）→ `sceKernelExitProcess(-3)`。
  - `JVM.cpp`：`stop()` 调 clean_shutdown（文件级 extern "C" 声明——函数内 extern "C" 不合法）。
- **误杀风险（如实）**：>30s 无字节码执行的合法暂停（如长加载、阻塞 IO）会触发看门狗转储+退出。若误杀，调大 OS_vita.cpp 的 `WATCHDOG_TIMEOUT_MS`。
- **构建系统新认知（本轮核心教训，防再犯）**：
  - **ABI flavor 必须单代**：`Deterministic`/`jvm_perf_count` 是非 PRODUCT 专属符号（develop flag 在 `#ifdef PRODUCT` 下宏为空；`ENABLE_PERFORMANCE_COUNTERS` 在 `ENABLE_MINIMAL_ASSERT_BUILD` 分支被清 0——但该分支 PRODUCT 下不生效，实际由 jvmconfig 的 ENABLE 表决定：非 PRODUCT=1/PRODUCT=0）。`JVM_TRAPS` 在 PRODUCT 下展开为空（GlobalDefinitions.hpp:1244 `#ifndef PRODUCT`）→ 符号 mangled 名都不同（`new_symbolEP12JVMTypeArrayPci` vs `...PciP8JVMTraps`）。**基线库是 PRODUCT 代；任何手动重编对象必须带 rebuild_vm.sh 的完整 PRODUCT 旗标**，否则混装必链接失败（本轮先后报 Deterministic/jvm_perf_count undefined、再报 JVMUniverse 三方法 undefined，全是同一根因的两个侧面）。
  - **手动重编单个合并对象的正确命令**（rebuild_vm.sh 同款旗标，在 target/release 下）：`make _MergedSrc003.o BUILD_DIR_NAME=vita_arm IsLoopGen=true LOOP_GENERATOR_DIR=../../linux_arm/loopgen/app ENABLE_ENABLING_CHECK=false ENABLE_C_INTERPRETER=true FORCE_GCC= GNU_TOOLS_DIR=$VITASDK CPP_DEF_FLAGS="-DPRODUCT -DROMIZING=1 -DARM -DVITA -D__PSP2__ -Wno-narrowing -fpermissive -marm -march=armv7-a -mtune=cortex-a9 -mfloat-abi=hard -mfpu=vfpv3 -DSUPPORTS_MEMORY_MAPPED_FILES=0 -DSUPPORTS_ADJUSTABLE_MEMORY_CHUNK=0 -DSUPPORTS_TIMER_THREAD=1 -DSUPPORTS_TIMER_INTERRUPT=0 -DUSE_VM_EXCEPTIONS=0 -DUSE_BSD_SOCKET=1"`——**先 rm 旧 .o**（touch 不够，make 判定依赖未变时直接跳过）。
  - **glue 符号定义在 _MergedSrc006**（Interpreter_c.cpp 的 FUNC_UNIMPLEMENTED 宏），`compiler_glue_code_start/end` 声明在 GlobalDefinitions.hpp:1954（`#if ENABLE_COMPILER && USE_COMPILER_GLUE_CODE`）；ObjectHeap.cpp:1240 引用 glue 的代码在 PRODUCT 目标编译下无错——此前脚本日志里的 ObjectHeap/Natives 编译错误来自 romgen 宿主端（CROSS_GENERATOR），可容忍。
  - **ar 重打包必须在 target/release 目录内以 basename 执行**（`ar r $L $m`，m 不带路径），否则报 "No such file"。
- **产物**：`samples/j2me/midp_vita_v0106.vpk`（md5 b7cc90fd1f9194041c808c178a784d06，14308905B，param.sfo AppVer=01.06）。库 21 成员单 PRODUCT 代（Interpreter_arm 沿用旧库 ARM 汇编成员）。
- **vita_main.c 顺序修复（本轮）**：原 `freopen(midp_stdout/stderr.log)` 在 `sceIoMkdir(DATA_DIR)` **之前**——首次安装时 `ux0:/data/J2ME00001/` 不存在，freopen 静默失败，stderr 停留在 tty 设备（Vita3K `*** TTY:` 逐字符流），导致 midp_stderr.log 为空（8-31 日志丢失事故的根因）。已将三个 `sceIoMkdir`（DATA_DIR/appdb/lib）移到 freopen 之前。注意：**8-31 事故的日志并没有真丢**，它进了 Vita3K 的 TTY 流；此后修复了目录顺序，日志会正常落盘。
- **测试预期**：装 01.06 跑口袋灵兽，若再冻结/崩溃，看门狗会自动转储 64 条面包屑到 `ux0:/data/J2ME00001/midp_stderr.log` 并退出进程（Vita3K 日志出现干净退出而非 53 万行刷屏）——**请回传该日志**，面包屑 bcp/opcode 可直接定位坏 handler。

### 2026-09-04 v01.04：dispatch 表项破坏修复（待用户复测）
- **01.03 崩溃定位（vita3k.log 已闭环）**：`Invalid read at 0x125d1784, PC=0x125d1784, LR=0x8109d7a4`。LR 正是分发调用点 `8109d7a0: blx r2` 的下一条 → PC==跳转目标 → r2=handler=0x125d1784（Java 堆地址）→ **`interpreter_dispatch_table[code]` 表项被越界写破坏成野指针**。tripwire 拦不住（g_jfp/g_jsp 合法，表内容本身坏）；面包屑无法落盘（blx 后 CPU 立即异常）。与 01.02 r4=0xdc5d1784 低 24 位相同（0x5d1784）——同一污染源的多轮表现。
- **修复（ae48cdb）**：Interpret() 分发循环每次 `blx` 前校验 handler ∈ [0x81000000, 0x81400000)，越界先打 slot/handler/表边界到 stderr，再走 bc_crash_report()（面包屑+干净停机）。
- **romgen 排障笔记（未修，暂不需要）**：`romgen/app/ROMImage.o` 只有 576B 空壳——`ROMImage.cpp` 全包在 `#ifdef ROMIZING` 里，而 jvm.make:348 只在 `IsTarget=true` 时定义 `ROMIZING_CFLAGS`，romgen 是宿主工具（IsTarget=false）所以没 -DROMIZING。链接缺 `_rom_data_block` 即此因。**本轮只改 Interpreter_c.cpp，Java 类库未变，ROMImage 无需重生成**，build 阶段（31 objects + repack libcldc_vm.a 21 members）成功即够。若日后要重生成 ROM，需在 romgen 的 CPP_DEF_FLAGS 里补 -DROMIZING=1。
- **注意**：`cmake --build build` 的真产物在 `vita-port/build/` 根下；`build/cmake/` 是旧目录（02:08 的 01.03 包），勿混淆。
- 产物：`samples/j2me/midp_vita_v0104.vpk`（md5 a00017838b809c19d3157e7cacc6fb99）

### 2026-08-31 里程碑：游戏可玩、中文正常显示 ✅
- 口袋灵兽等 J2ME 游戏可在 Vita3K 进入并正常游玩，图像与中文文本渲染全部正常
- 最终根因见"关键文件修改记录"一节（fb_load 无调用点，commit 0fc9e8d）
- 游戏菜单（安装/卸载/启动/横竖屏）可用；诊断日志体系（font_debug.log 走独立 fd）就绪

### 构建成功
- VPK: `vita-port/build/midp_vita.vpk` (~13MB，含 1.4MB fontbitmap.bin + 16MB font.ttf)
- ROM: 19,567 objects, ~1.2MB (包含 midp_system.jar + Hello.jar)
- UseROM: enabled

### 遗留事项
- [ ] RMS 未按游戏隔离（所有游戏共用一个 suite 空间，存档可能互相污染）
- [ ] 音频未实现（游戏无声；需接 sceAudio 或 BGM/PFS stub）
- [ ] 游戏菜单本体仍是 ASCII 5x7 字体（可复用 fontbitmap.bin 做 CJK 菜单）
- [ ] 真机验证（目前仅 Vita3K）
- [ ] 性能观察（解释器执行、绘制帧率）

---

## 构建命令

```bash
cd /home/zyb/vitasdk/samples/j2me/midp-vita

# 完整构建
./build_midp_vita.sh

# 清理后重建
./build_midp_vita.sh clean

# 仅构建 Hello.jar
bash build_jar.sh
```

---

## 关键文件修改记录

### ⭐⭐⭐⭐ 2026-09-04 第三轮：新包确认生效但换形崩溃 → 面包屑环形缓冲+不变量巡检（cldc 3494149，VPK 01.03）
- **版本确认与教训**：用户第三次日志（09:43）确认跑的是 17:02 新包：模块 NID 0xD947F3F4、e_entry 0x36E9E0（旧包 0xE92EFCF7/0x36E930）；此前 09:37 那份日志用户装的是旧包（LR=0x8108ee90=旧 undef_bc 的 svc 地址，与 23:25 现场逐字节相同）。教训：版本号 01.02 不变导致无法从日志分辨包版本——**已把 VITA_VERSION 升为 01.03 并写进 CMakeLists 注释惯例，今后每轮交付必须 bump 版本**。
- **好消息**：`NID 0xE1A00001 not found`（undef→svc）已从日志消失——分发表 0..201 全注册 + undef_bc 安全停机均生效，jsr/ret 补洞被实测确认。
- **新崩溃形态**：PC=0x8109d750（分发循环），LR 同值，SP=0x8043f028；r4=0x810911fc 是**代码地址**（bc_impl_lload_3 函数体内 `pop {r4,pc}` 指令处，0x810911dc）；`Invalid read uint8_t at 0xe8bd8010` 即该指令编码被当数据读；二次执行 0xe58320b4=`str r2,[r3,#0xb4]` 编码。机制：某 bc_impl 尾声 `pop {r4,pc}` 从栈上弹回脏 r4（代码指针）返回分发循环，循环用 r4 当解释器全局基址（gp=0x81390564 时 g_jpc=[r4+0xb4]、g_jsp=[r4+0xb0]）→ 野读。与上一轮 r4=bc_impl_lshr 函数首同病：**栈/帧不平衡类，坏栈底**，非特定 opcode 缺失。
- **本轮修复（cldc 3494149，Interpreter_c.cpp）**：64 项面包屑环形缓冲 bc_ring[]（每条分发前记 code/bcp/jsp）；分发前两条不变量巡检：g_jfp 4 字节对齐、g_jsp>=_current_stack_limit（声明在 GlobalDefinitons.hpp:1917，定义在 InterpreterSkeleton.cpp:218）；命中即 bc_crash_report 打印 g_jpc/g_jsp/g_jfp/总数/方法名（debug 打印版用 Method::print_name_to，release 版无方法名）+ 最近 64 条面包屑到 stderr（设备侧落 midp_stderr.log）与 tty，然后 Universe::set_stopping()+BREAK_INTERPRETER_LOOP(JMP_STOPPED_MANUALLY) 安全停机。该巡检也能覆盖 jsr/ret 等分支改变 g_jfp 后的损坏。
- **验证**：31 对象 OK；velf 内 bc_ring/bc_ring_idx/bc_ring_count 符号在（.bss 0x102714238 区域）、"VITA FATAL"字符串在、undef_bc 仍在 0x8108eef8；param.sfo AppVer=1.03；VPK 14306614B。
- **下一步**：用户装 01.03 跑口袋灵兽，若再异常，Vita3K 日志里应出现干净停机（无野崩二次异常），**拿 ux0:/data/J2ME00001/midp_stderr.log**——"VITA FATAL:" 块给出方法名+64 条面包屑（bcp/op/jsp），bcp 可对照 jar 反汇编定真凶；巡检未触发但 VAN 崩的场景则比较 g_jsp 窗口找异常回卷。
- **遗留风险**：面包屑本身不修根因，是定位手段；若巡检从未触发而崩在 bc_impl 内部（g_jsp 还没低于 limit、g_jfp 恰好对齐），需把巡检加密度（如同时校验 g_jsp<=stack_base）。

### ⭐⭐⭐⭐ 2026-09-03 第二轮：jsr/ret 修复后游戏仍崩 → undef_bc 安全网改造（cldc a9b44c1）
- **背景**：用户装上含 jsr/jsr_w/ret/ret_wide 的 VPK（cldc 0e8c406 / samples 5ec5d45）后实测"口袋灵兽"仍异常。新 vita3k.log 显示**同一 undef_bc→svc 0x1 链再次发生**（新地址：LR=0x8108ee90 恰落在新构建 undef_bc 的 svc 指令上；模块 NID 0xE92EFCF7 也证明用户跑的是新构建）。
- **分发表权威复核结论（排除 jsr/ret 注册问题）**：Python 全量比对（TAGS=0/CPU_VARIANT=1 条件求值）enum 0..201 共 202 项，注册后 MISSING 仅 `[(186,'xxxunusedxxx')]`=保留值；WIDE 表 12 项齐全（含 ret）。**jsr/ret 注册确认无误**。真 opcode 值未出现在 vita3k.log——undef 打印走 fd 0x7 进设备侧 `ux0:/data/J2ME00001/midp_stderr.log`，用户尚未提供该文件。
- **根因（机制层面，与游戏启动 60ms 崩溃同一机制）**：undef_bc 原实现是 tty 打印 + BREAKPOINT。Vita 上 BREAKPOINT=SWI 1，Vita3K 报 "Import function for NID 0xE1A00001 not found" 后**继续执行**——undef_bc 尾声继续跑，pop 回坏 r4 返回分发循环，野指针崩溃远离第一现场。
- **修复**：Interpreter_c.cpp undef_bc 重写（commit a9b44c1，+38/-2）：新增 `undef_bc_stop(code,bcp)`——fprintf+tty 打印 opcode/bcp → `Universe::set_stopping()` → `BREAK_INTERPRETER_LOOP(JMP_STOPPED_MANUALLY)`（与 current_thread_to_primordial 相同的合法 longjmp 退出路径，primordial_to_current_thread 主循环 4593 行已有 JMP_STOPPED_MANUALLY 处理）。debug 打印开启时额外经 undef_bc_report 打方法名（Method::print_name_to 有 PRODUCT 门控，故包在 `#if !defined(PRODUCT) || USE_DEBUG_PRINTING` 内）。release 分支走 undef_bc_stop（无方法名但有 opcode/bcp）。
- **踩坑记录（重要，防再犯）**：
  1. `JVMUniverse`（handles/Universe.hpp）**没有** `stop()` 方法，只有 `set_stopping()`/`is_stopping()`——首次重编报 `'stop' is not a member of 'JVMUniverse'`。
  2. 编辑时误删旧 `#if !defined(PRODUCT) || USE_DEBUG_PRINTING` 的开启行留下孤儿 #endif → `unterminated #if` 波及 JarFileParser/Natives.hpp 假错误。**教训：#if 块改写要整体删旧换新，不能只换中段。**
  3. Interpret() 主循环 guard（查表命中 undef_bc 预拦截）已实现又回退：冗余且若 stop 返回会双重分发同一坏 opcode。undef_bc 自身停机足够。
  4. vita-port/build.sh 需要 JDK：`export JDK_DIR=/home/zyb/tools/jdk8u502-b07 && export PATH=$JDK_DIR/bin:$PATH`（javac 不在默认 PATH）。
  5. romgen 宿主链接报 BSDSocket.o wrong format 是已知无害噪音（VM 31 对象 OK + repack 完成即成功）。
- **验证**：rebuild_vm.sh build 31 对象 OK；库成员 _MergedSrc006.o 反汇编确认 undef_bc 控制流 fprintf→print_cr→longjmp、**无 svc**；新 velf（midp_vita.velf）undef_bc@0x8108eef8（旧构建 0x8108eea4），undef 区域 objdump svc 计数=0。VPK：`vita-port/build/cmake/midp_vita.vpk`（eboot 2417440B，17:02 生成）。
- **待办/下一步**：用户装新 VPK 后 (1) 若崩，预期 vita3k.log 出现干净退出而非野崩，且 midp_stderr.log 必有 `FATAL: Undefined bytecode 0xNN at bcp 0x…`——拿到 NN 即可定点修真 opcode；(2) 请用户提供 ux0:/data/J2ME00001/midp_stderr.log（Windows 侧 Vita3K 目录 `C:/Users/zyb/AppData/Roaming/Vita3K/Vita3K/ux0/data/J2ME00001/`）——当前已装构建的该文件里其实已有旧格式打印 `Undefined bytecode hit: 0x?? at 0x??`，是最快定凶途径。
- **遗留风险**：若坏流来自 JSR 子程序外的其它路径（如宽索引/坏 class），undef_bc 只是把野崩变成可控停机+诊断输出，不解决根因；真 opcode 定位后仍需定点修复。

### ⭐⭐⭐⭐ 游戏启动 60ms 崩溃根因闭合：undef_bc→svc 0x1 野崩溃链 + jsr/ret 无实现（2026-09-03，cldc 0e8c406）
- **症状**：03:46 全编译 VPK 打开游戏即卡死闪退；Vita3K 日志 `unknown NID 0xE1A00001`（~60ms，12:21:55.046）+ `EXCEPTION_ACCESS_VIOLATION Read 0xe3a03000`，PC=0x8109d85c（分发循环 `blx r3`），LR=0x8108ee10，r4=0x81093df0（.text 字节被当指针）
- **根因链（全链条二进制证据闭合，不依赖用户日志）**：
  1. C 解释器分发表 **256+256 槽唯缺 jsr(0xa8)/ret(0xa9)/jsr_w(0xc9)**（Bytecodes.hpp 枚举 vs DEF_BC 全量机械比对，其余全部已注册）
  2. 游戏 jar（2005 前后 javac target<1.5 编译）的 try/finally 产生 jsr/ret；ROM 化系统类已 jsr-free（midp_system.jar 744 类精确流式扫描：此前报的 FloatingDecimal 0xa8@134 是 **ldc2_w 操作数**、CalendarImpl 0xa9@7 是 **iflt 分支操作数**——两次字节扫描均为假阳性，教训：扫描必须线性解码不能裸找字节）
  3. 命中未注册槽 → `undef_bc()`（Interpreter_c.cpp:4154）打印 "Undefined bytecode hit" 后执行 `BREAKPOINT`
  4. **BREAKPOINT 在 Vita = SWI 1**（GlobalDefinitions_gcc.hpp:96 注释即此前 Vita 移植改动："0xe6000010 undefined 会崩，改用 0xEF000001"）——Vita3K 不停机、当未知 syscall 报 NID 0xE1A00001（svc 下一条 `mov r0,r1` 的编码 E1A00001）
  5. svc 返回后 `pop {r4,pc}` 恢复出坏栈 → r4=0x81093df0 → 分发循环把 .text 编码当 g_jpc/表项 → 野指针崩溃（两个变体 0xe3a03000=`mov r3,#0`、0xe3070c9c=`movw r0,#0x7c9c` 均对上）
  6. Hello.jar 不触达 jsr（其 opcode 集全覆盖）——所以 Hello 测试通过而游戏崩
- **修复**：Interpreter_c.cpp +34 行实现全部四个入口：`jsr`（PUSH((jint)(address_word)(g_jpc+3)) + branch(true)，与 goto 同 tick 路径）、`jsr_w`（PUSH(jpc+5)+GET_INT 跳转，对齐 goto_w 不加 check_timer_tick）、`ret` 窄形式（g_jpc=GET_LOCAL(GET_BYTE(0))）、wide `ret`（DEF_BC_WIDE→bc_impl_ret_wide，g_jpc=GET_LOCAL(GET_SHORT(0))——bc_impl_wide ADVANCE(1) 后入口 g_jpc 指向 ret opcode 本身，与 iinc_wide 语义一致）。returnAddress 是裸 jint 绝对 bci，**不用 OBJ_PUSH**（非 oop，GC 不追踪）
- **构建链**：rebuild_vm.sh 全量重编（31 对象全代际）→ libcldc_vm.a 21 成员 → vita-port/build.sh 链接打包。velf 终验：`bc_impl_jsr`@0x102092b3c（push 序列+revsh 分支+tick 调用）、`bc_impl_ret`（ldrb 操作数→GET_LOCAL→写 [r3,#0xb4]）、`bc_impl_jsr_w`（PUSH+rev 全字跳转）逐一反汇编正确
- **构建环境变化**：宿主 g++-11 已不存在，vita_arm.cfg 两处 FORCE_GCC 改为 `/usr/bin/g++-13 -B/usr/bin -m32`（g++-13-multilib 在位）——cfg 与 0e8c406 一起提交
- **路径修正（本轮实证）**：VPK 真正构建入口是 **`vita-port/build.sh`**（vita-port/CMakeLists 含 vm_rom_stubs.c 的 InitFPU/_rom_linkcheck 补符号 + 完整 Vita 移植层）；`midp-vita/build_midp_vita.sh` 是旧路线，其 CMake 链接会报 InitFPU/_rom_linkcheck undefined（vm_rom_stubs 不在其闭包），**不要再用它出包**
- **遗留**：①BREAKPOINT=svc 0x1 加固（改成安全停机）未做——jsr/ret 补齐后 undef 概率已极低，但未来任何 undef 命中仍会野崩 ②游戏 jar 实测待用户确认 ③milestone 崩溃微机制（svc 如何扰栈致 pop 出坏 r4）未完全证明，不影响修复

### ⭐ 事件泵死锁根治：Scheduler 门控放宽（2026-09-02，cldc 83ece23 / vita-port e711a0b，VPK 01.02 b153）
- **症状**：01.01 版（触屏+input_debug.log 日志版）进入游戏后"卡死"无输入响应，但 **input_debug.log 根本不出现**——心跳日志在 checkForSystemSignal 首行，没日志=事件泵从未被调用，这是决定性证据
- **根因（实锤）**：`Scheduler.cpp:653` wake_up_timed_out_sleepers 上游门控 `if (!is_slave_mode() && !Universe::scheduler_async()->is_null())`——主模式下**必须有 Java 线程阻塞在异步 native 调用**才驱动事件泵。我们的音频全同步返回（JAVACALL_MM_ASYNC_EXEC）→ 游戏运行期 scheduler_async 恒空 → checkForSystemSignal 永不执行 → 输入/触摸/媒体事件全部滞留 ring。这也解释了"修好音频同步后反而卡死"的矛盾
- **修复**：`#if defined(VITA)` 分支去掉 scheduler_async 判空，仅保留 `!is_slave_mode()` + `_timer_has_ticked || _estimated_event_readiness > 0`——事件泵改由 10ms timer tick 驱动（ticker 线程→real_time_tick→set_timer_tick→解释器 check_timer_tick→yield 链路已验证完整）
- **构建陷阱（本次最大教训）**：**不能只替换单个 .o 进旧库**。dist/lib/libcldc_vm.a 是 21 成员的既有 ABI 体系（JVM_TRAPS 宏在 PRODUCT 下展开为空、非 PRODUCT 下带 `Traps*` 参数——符号名都不同）。单换 MergedSrc002 必然与其他 20 个成员 ABI 不匹配，链接期缺 JVMScheduler::start/wait/notify 等一大批。正确做法：`rebuild_vm.sh`（VM_BUILD.md 配方）**全量 32 个 target/release 对象统一重编**再打包
- **rebuild_vm.sh EXCLUDE 修正（重要）**：脚本原版漏排 6 个"可执行专属"对象（BSDSocket/Main_vita/NativesTable/ROMImage/ReflectNatives/jvmspi——jvm.make LIB_OBJS 1030-1048 行明确 subst 掉的）。混入主库导致 Main_vita 的 main/module_start、ROMImage 与 MIDP 侧冲突、符号拉取链断裂（pte_osInit/ANI_Initialize 链接失败假象）。~~从 KNOWN_GOOD 基线重播种~~ **2026-09-03 起改为全代际重打包（见下方 03:46 记录）**
- **release flavor 与 jvm_natives_table**：release（非 PRODUCT）+ROMIZING 下 `Natives.cpp:1638` 会引用 `jvm_natives_table`（`#if (!ROMIZING)||(!PRODUCT)`），需把 `NativesTable.o`（纯数据表 R 符号+Java_* 外部引用，无副作用）加进主库；PRODUCT 下无此引用（旧库即 PRODUCT ABI 所以从没暴露）
- **验证**：最终 eboot `wake_up_timed_out_sleepers` @0x81072588 反汇编确认 is_slave_mode 检查后直接读 _timer_has_ticked([r5,#340])/_estimated_event_readiness([r3,#1580/1588])，**无 scheduler_async 判空**；库 21+1 成员、jvm_f2i 浮点 stub 在位；VPK 14530343 字节、版本串 `J2ME Player v01.02 b152 (e711a0b)`
- **测试预期**：装 01.02 后进游戏，`ux0:/data/J2ME00001/input_debug.log` 应出现（[HEARTBEAT] 每 1024 次 checkForSystemSignal + [INPUT] 每次按键变化）——文件出现即事件泵复活；触屏（Vita3K 鼠标点击）走 MIDP_PEN_EVENT 路由到 Canvas.pointerPressed/Released

### ⭐⭐ Thumb/ARM 混编崩溃：rebuild_vm.sh 丢 -marm（2026-09-03，VPK 01.02 b153 修复版）
- **症状**：装上 rebuild_vm.sh 全量重编的 01.02 进游戏即崩。Vita3K 日志：`Undefined instruction at 0x810C0E88, instruction 0x1AFFFFFA` + `Thumb: true` + **同一 PC 无限循环、SP 每轮递减 0x14（无限递归栈下溢）**——与 b139 事故特征同款
- **根因（实锤）**：`rebuild_vm.sh` 的命令行三清 `CPP_DEF_FLAGS=` 把 cfg 注入的 `-marm` 架构标志一并清掉 → 32 个新 C++ 对象全部编成 **Thumb-2**（`f245/b08b/b9a3` 编码），而老库 `Interpreter_arm.o` 是 **ARM 汇编**。崩溃链：新 Thumb `adjust_heap_size`（_MergedSrc003）Thumb `bl fast_memclear` 不切模式 → 跳进 ARM 汇编 → ARM 编码被 Thumb 状态解码 → 未定义指令。LR=0x810861cb 正是 `bl fast_memclear` 调用点
- **误判提示**：崩溃点指令 `1afffffa` 本身是合法 ARM `bne`（链表遍历循环）——"指令合法但 Thumb:true 执行"就是模式错乱指纹，别往坏数据/GP 表方向查
- **正确配方（已被 03:46 全编译路线取代，保留下述宏结论）**：编译命令**必须显式带 `-O2 -DNDEBUG -DPRODUCT -DROMIZING=1 -marm -march=armv7-a -mtune=cortex-a9 -mfloat-abi=hard -mfpu=vfpv3`**（PRODUCT 宏给出无参 JVM_TRAPS ABI + 无 jvm_natives_table 引用，-marm 保证 ARM 编码）。注意：任何路线下这些标志都不能丢——GNU make 命令行变量会压制 makefile 内 `+=`，`CPP_DEF_FLAGS=` 一旦传空，cfg 里 vita_arm.cfg 232-254 行的全部 `CPP_DEF_FLAGS +=`（架构+特性宏）都被清掉
- **入库前必须验证四项**：①反汇编为纯 ARM 编码（e3xx/e59x/e1a 开头，无 f2xx Thumb 前缀）②符号为无参版（`wake_up_timed_out_sleepersEv`，非 `EP8JVMTraps`）③`nm | grep -c jvm_natives_table` = 0（PRODUCT 生效）④`wake_up_timed_out_sleepers` 反汇编无 scheduler_async 门控（VITA 修复在位）
- **CMake 陷阱**：CMake 不把 libcldc_vm.a 当文件依赖——**换库后必须 `rm -f build/midp_vita midp_vita.velf midp_vita.self midp_vita.vpk` 强制重链**，否则只是用旧 self 重打包 VPK（本次 16:47 的 VPK 就是这么混过去的）
- **最终验证**：新 eboot `wake_up_timed_out_sleepers` @0x81071e70 纯 ARM 编码；`adjust_heap_size → fast_memclear` 为 ARM `bl`（eb00ce87）模式一致；版本串 v01.02 b152；HEARTBEAT/checkForSystemSignal/sceTouchPeek 全在位；VPK 14286958 字节（02:24）
- **VM_BUILD.md 待补**：三清中 `CPP_DEF_FLAGS=` 只是为了清 romgen 分支的 `-B/usr/bin -m32 -DCROSS_GENERATOR=1`，但 make 层面 cfg 的 -marm 也在 CPP_DEF_FLAGS 里——**用 rebuild_vm.sh 后新对象是 Thumb，与老库 ARM 成员不兼容**。全量重编路线必须同时验证 `-marm` 在编译命令中（grep 干跑输出）；单对象替换路线用上面的显式命令

### ⭐⭐⭐ 库混代崩溃 + 全编译终局修复：rebuild_vm.sh 两缺陷（2026-09-03 03:46 VPK，验证通过待真机/模拟器实测）
- **症状**：02:24 的 v01.02 修复版进游戏仍崩，Vita3K 日志 `EXCEPTION_ACCESS_VIOLATION, Read violation at 0x13A2C499`，PC=0x8108e6d8（JVMMethod::name()），LR=0x8107c014（Throwable::fillInStackTrace 遍历调用栈）。**注意 Thumb 已不是问题**（日志 `Thumb: false`）——崩溃性质从"指令模式错乱"变成"读野指针"
- **根因 A（库混代，实锤）**：上一版 pack_lib 从 `/tmp/libcldc_vm.a.KNOWN_GOOD`（Sep 2 基线）重播种 20 个旧对象、只换新 `_MergedSrc002.o` → 库内同名成员字节不同、大小差 2 倍级（旧基线混装配产物）→ 异常路径 fillInStackTrace 遍栈时 method/constants oop 为垃圾（0x13a2c499）→ 崩。**触发条件是 Java 异常抛出**，所以此前正常路径的测试全通过、一进游戏就崩
- **根因 B（编译标志丢失）**：`CPP_DEF_FLAGS=` 命令行赋值压制 cfg 的全部 `CPP_DEF_FLAGS +=` → 丢 `-marm/-march/-mfloat-abi=hard/-DPRODUCT/-DARM/-DVITA` 等全套 → FLAVOR=release 本身不产生 `-DPRODUCT`（jvm.make 833-835：`CPP_DEF_FLAGS_release=` 为空、product 才有 `-DPRODUCT`）
- **修复（rebuild_vm.sh，+44/-3 行）**：①build_target() 新增 `CPP_DEF_FLAGS_TARGET`（完整 17 项标志：-DPRODUCT -DROMIZING=1 -DARM -DVITA -D__PSP2__ -Wno-narrowing -fpermissive -marm -march=armv7-a -mtune=cortex-a9 -mfloat-abi=hard -mfpu=vfpv3 -DSUPPORTS_*×4 -DUSE_VM_EXCEPTIONS=0 -DUSE_BSD_SOCKET=1），make 显式传入 ②pack_lib() **废除 KNOWN_GOOD 重播种**，改为全代际重打包：所有 target 对象逐个 `ar r`；`Interpreter_arm.o` 沿用旧库成员（regenerated .s 丢了 jvm_f2i/jvm_d2i 浮点 stub，且其内嵌 GP 全局符号正是链接期所需）③EXCLUDE 清单不变（12 个可执行专属对象）
- **全编译验证（本轮"就该全编译"结论的实证）**：31 个 target/release 对象全新生成（03:28）——**全部纯 ARM**（逐对象统计 0 条 16-bit、共 113753 条 32-bit 指令）、**全部 0 debug-ABI 痕迹**（无 `Traps`/`EP8JVMTraps`）；主库 1169032 字节、21 成员全代际一致
- **强制重链**：删 midp_vita/velf/self/vpk 四产物 → cmake --build 重链 → VPK 14305891 字节（03:46）
- **eboot 终验（nm 地址 0x102… 与 objdump VMA 0x810… 换算：VMA = nm − 0x81000000）**：①Thumb=0 ②jvm_f2i@0x810a60fc / jvm_d2i@0x810a6154 在位 ③GP 全局在位（_old_generation_end@0x1027019d4、_task_class_init_marker@0x102701bfc 等，由旧代 Interpreter_arm.o 供给——GPSkeleton.cpp 那套只在 ROM 生成器里链入）④修复版 `wake_up_timed_out_sleepers`@VMA 0x81062308（0x19C 字节）全部 6 个 bl 解析确认：JVMScheduler::start+0x98、Protocol::writeByte+0xac（编译器布局巧合，非语义调用）、JVMOs::sleep、add_sync_thread、JVMSynchronizer::enter、gc_epilogue——函数体特征与 09-02 门控放宽修复版一致
- **产物**：`vita-port/build/midp_vita.vpk`（14305891B, 03:46）；`phoneme-cldc/build/vita_arm/dist/lib/libcldc_vm.a`（1169032B, 03:28，21 成员全代际）；`.bak` 为旧混代库勿再使用
- **构建期链接报错（预期，无害）**：rebuild_vm.sh 末尾 exe 链接 crt0 失败 `|| true`，38 个 undefined reference（_task_class_init_marker/_old_generation_end/_primordial_sp 等）本就由链接期 Interpreter_arm.o/vita-port 供给，属脚本设计内行为
- **遗留风险**：异常路径（fillInStackTrace 深栈遍历）在本库代际下是**首次完整实测**；若仍崩，下一步查①事件投递真空（mastermode checkForSystemSignal 空 stub 时机）②C interpreter 帧布局与 oop 遍历一致性

### 启动即崩真因：e_entry 被 patch 成 _start，破坏 SCE module_info 定位器（2026-09-02）
- **症状**：新 VPK 打开即卡死、Vita3K 闪退。日志：`Loaded module segment 0/1`（尺寸与 velf 完全一致）→ `Linking SELF app0:eboot.bin` → `eboot.bin module NID: 0xAC3E25AB` → **同毫秒** `EXCEPTION_ACCESS_VIOLATION, Read violation at 0x4A6F84D2F`
- **根因（实锤）**：SCE ELF 的 `e_entry` **不是程序入口**，是 `sce_module_info` 定位器：`(段号<<30) | 段内偏移`（Vita3K `load_self.cpp:492` `module_info_offset = e_entry & 0x3fffffff`、`:689` `module_info_segment_index = e_entry >> 30`；真正的程序入口在 `module_info->module_start`）。vita-elf-create 生成 `e_entry=0x364740` 本来就正确——精确指向 `.sceModuleInfo.rodata`（0x81364740 = 0x81000000+0x364740）。而 `tools/patch_velf_entry.py`（b141 引入）把它改写成 `_start` 偏移 0x6e9 → Vita3K 把段0+0x6e9 处的 .text 机器码当 `sce_module_info_raw` 解析，`import_top/import_end` 全是代码垃圾 → `load_imports` 的 `for (imports=begin; imports<end; imports+=imports->size)` 走野指针 → 读违规闪退
- **误诊链条（教训）**：入口补丁源于"JVM 秒退/入口错误"误判，此后每次构建症状变化都被归因到 entry，一路强化错误修复；`2f578a6` 的 whole-archive ani 同样基于"ANI_Initialize 链接失败"误诊（实测普通 `-lcldc_vm_ani` 完全能解析）
- **修复**：①删除 CMakeLists 的 entry patch POST_BUILD（CMake 留注释说明 SCE e_entry 语义防再犯）②`patch_velf_entry.py` 改名 `.DANGEROUS-DISABLED` ③撤掉 whole-archive ani。b148 的 pte_osInit 保留（无害且有防御价值）
- **排查中排除项（以后别再查一遍）**：reloc 表完整（0x11004c 字节按 SCE 格式精确走完+4 pad）；`.init_array` 4 个构造（register_fini/pthread_setup/frame_dummy/JVMThrow GLOBAL__sub_I）均安全——`pthread_init` 自带守卫幂等调 `pte_osInit`；ani 成员无 init_array；RW 段 1088KB（含新增 jts events 48KB）不是问题；`vita-elf-create` 输出确定性；`vita-make-fself` 输出含时间戳非确定（仅元数据）
- **验证**：`make` 全绿；产物 velf `e_entry=0x364740` 与 `.sceModuleInfo` 对齐；`ANI_Initialize`/`PoolThread_InitializePool`/`javanotify_on_media_notification`/`jts_expand`/`checkForSystemSignal` 全部在位
- **方法论**：崩溃在 loader 阶段（"Linking SELF" 后同毫秒）⇒ 查 ELF/SCE 结构与链接脚本，别查应用代码；Vita3K 源码（load_self.cpp/relocation.cpp）是 loader 崩溃第一手资料

### 启动闪退修复：pte_os pthread 初始化缺失（2026-09-02，samples 93372ca，VPK b148）
- **症状**：启动闪退，Vita3K 日志刷 `Invalid write at 0x4` + `pte_osAtomicExchange` + `pthread_mutex_unlock`——newlib FILE 锁（freopen/printf）触发 pthread_mutex_unlock → pthread_self() → pte_os 线程跟踪未初始化 → NULL+4 崩溃
- **根因**：vitasdk pthread 模拟层（pte_os）需要至少一次 pthread 调用初始化内部线程跟踪。旧 libcldc_vm.a 某些代码隐式触发了初始化；VM 重编去转储后不再调用，首次 pthread_mutex_unlock 即崩
- **修复**：vita_main.c 的 main() 最前面加 `PTHREAD_MUTEX_INITIALIZER` 的 lock/unlock 周期。**以后新增任何 pthread 使用时都不能删除这段初始化**

### ⭐ VM 编译权威文档（2026-09-02，samples 0ff302d）
**改 VM 源码（phoneme-cldc/src/**）后：先读 `VM_BUILD.md`、用 `rebuild_vm.sh`**——完整配方（宿主工具/ARM 目标/打包三阶段）+ 十条陷阱表已固化。要点：命令行三清（FORCE_GCC=/GNU_TOOLS_DIR=/CPP_DEF_FLAGS=）+ `_release` 子目标 + AsmStubs 预置 + Interpreter_arm.o 用旧库成员 + ani.o 等 anilib 文件不得混入主库 + ar r 后逐个验证成员非空。

### ANI 链接回归修复 + 入口补丁恢复（2026-09-02，samples 63015e9，VPK b146）
- **ANI_Initialize 链接失败根因**：重造的 `libcldc_vm_ani.a` symbol index 损坏/过期——ld 扫描 archive 拉入 os_port.o 但不拉 ani.o（定义 ANI_Initialize 的成员）。ranlib 修 index 后最小链接过了但完整链接仍失败（不明 archive 边角案例）
- **解法**：PRE_LINK 提取 ani.o/ani_bsd_socket.o/os_port.o/poolthread.o 四个对象，以普通对象直接链入（对 archive 边角免疫）；从 group 移除 `-lcldc_vm_ani`
- **入口补丁恢复**：`tools/patch_velf_entry.py` POST_BUILD 把 velf e_entry 从 0x395520（无关函数）修回 _start（0x1471，**Thumb 位保留**）。此前 b143 闪退=Thumb 位被 strip，b141 秒退=入口指向普通函数
- **VM_BUILD.md 陷阱 11 补充**：链接期症状 jvm_f2i undefined=库成员坏；运行期症状 Undefined instruction + GP 表地址=坏 GP 表成员；秒退=入口指向普通函数；闪退=Thumb 位丢失

### 音乐开关"卡死"根治：VM 重编去转储（2026-09-02，cldc 2959a08 / samples 357f014，VPK b137）
- **真相**：音乐开关触发 MMAPI 类链**首次加载**（音频集成前这些类 CNF 根本不走加载）→ VM 三处调试转储（ClassFileParser 的 CP dump、ConstantPoolDesc 的 var_oops_do、Universe 的 hidden 警告）每类倾倒数千行 stderr → Vita3K 慢速 I/O 下几分钟出不来 = "卡死"；17403 行日志零异常。fd 0x7 洪水、suite 反复 startSuite 都是伴生现象
- **修复**：全部转储 gate 在 `VITA_CP_DEBUG` 后（默认关）；libcldc_vm.a 重编（eboot 中三组格式串清零）
- **VM 重编配方（血泪总结，cldc commit 2959a08 message 有完整版）**：① loopgen/romgen 按 cfg 原设置编 ② target 用 `IsLoopGen=true LOOP_GENERATOR_DIR=... FORCE_GCC= GNU_TOOLS_DIR=... CPP_DEF_FLAGS=（命令行三清）... _release` + JVMWorkSpace/JVMBuildSpace/JDK_DIR/TOOLS_DIR 环境变量 ③ AsmStubs_x86_64.o 从 romgen/app 预置 touch 旧 ④ **release flavor**（debug 引 AZZERT 符号、product 缺编译器成员）⑤ Interpreter_arm.o 用旧库成员（重生成 .s 丢浮点 stub）⑥ ar r 后必须 ar p 验证成员非空（&&链会静默断）⑦ 37 个 `_rom_check_*` 由 vita-port `src/vm_rom_stubs.c` 补
- **Interpreter_c.cpp:1506**：`invoke_native_entry(..., NULL, ...)` → `va_list()`（ARM EABI va_list 是结构体，NULL 只在 32 位 x86 可编）

### device://tone Player 全链路打通：tone 状态机 + MMAPI 事件桥（2026-09-01）
- **背景（"开启音乐"冻结根因）**：游戏 `Manager.createPlayer("device://tone")` → `javacall_media_create` 对 tone URI 返回 INVALID_ARGUMENT → `nInit` 抛 `MediaException("Unable to create native player")` → 主线程异常/重试 → UI 冻结。此前 vita_audio_javacall.c 只有 playTone 快捷路径，无 Player 状态机
- **`vita_audio_javacall.c`（+681 行）实现完整 tone Player 状态机**（create/realize/prefetch/start/stop/resume/close/destroy + get/set_volume/mute + JTS 缓冲与播放）：
  - `create` 按 UTF-16 精确匹配 `device://tone`，命中则 `javacall_malloc` 分配 `vita_player`（~32KB，内含 32KB JTS 缓冲），未命中返回 INVALID_ARGUMENT
  - **`get_format` 返回 UNKNOWN 是关键设计**：Java 侧 `HighLevelPlayer` 构造器对 UNKNOWN+`TONE_DEVICE_LOCATOR` 自动映射为 DEVICE_TONE 且 `handledByDevice=true`（HighLevelPlayer.java:291-294），跳过 source.connect/download；若误返回 TONE 会走 MediaDownload 路径
  - **全部 mandatory API 同步返回 OK**：CLDC green-thread VM 下 KNI 阻塞会冻结全部 Java 线程；`JAVACALL_MM_ASYNC_EXEC` 宏（mm_async_exec.h）在同步 OK 时直接返回，不设 reentry、不阻塞
  - JTS 播放：`start` 把 `g_jts_req=pl; g_jts_seq++` 交给常驻 `j2me_tone` 线程（复用 playTone 线程），线程内 `jts_expand` 展开为扁平事件表 → 逐事件 `generate_tone_pcm` 方波合成（16k 单声道）→ 3 倍重采样 → SceAudioOut；无 JTS 数据时立即发 END_OF_MEDIA
  - **REPEAT 语义按规范实现（4 字节 `REPEAT multiplier tone_event`）**：校验器（jts_check_sequence）、pass1 定位（pos+=4）、pass2 展开（读紧随其后的 note/dur 发射 multiplier 次）、PLAY_BLOCK 块体（块尾扫描 +4 偏移、块内 REPEAT 展开）四处一致。**上游 win32 mmtone.c 校验器只消费 2 字节、播放循环把 REPEAT 落 default 当 note 播——是 win32 bug，勿效仿**；权威依据 ToneControl.java（`repeat_event = REPEAT multiplier tone_event`）+ DirectTone.jpp 校验器（pos+=4）
  - END_OF_MEDIA 的 data 单位是**秒**（Java 侧 MMEventListener `intParam2 * 1000` 转 ms），native 发 `(void*)(long)(duration_ms/1000)`
- **新建 `vita_media_notify.c`（~90 行，javanotify 事件桥）**：`javanotify_on_media_notification` 在 vita_arm 构建中无实现（唯一上游实现 javanotify_midp_jsr.c 属 javacall_application 子系统，不参与链接；midp_jc_event_send 管线也不在闭包）。仿 vita_input.c 模式：SPSC ring（16 个 MidpEvent，满丢最旧）→ `vita_media_poll`；字段映射与上游 midp_slavemode_javacall.c 对 MIDP_JC_EVENT_MULTIMEDIA 的处理一致（MM_PLAYER_ID/MM_DATA/MM_ISOLATE/MM_EVT_TYPE/MM_EVT_STATUS，MMAPI_EVENT=45）
- **`vita_input.c` checkForSystemSignal 排水媒体事件**：`vita_media_poll` 命中则 `waitingFor = MEDIA_EVENT_SIGNAL` 返回（媒体事件优先于输入事件，防 END_OF_MEDIA 饿死）；未命中再走原有 UI ring
- **事件链闭环**：native `javanotify_on_media_notification` → MidpEvent 入 ring → VM 线程 `checkForSystemSignal` 排水 → `midp_master_mode_events.c` `StoreMIDPEventInVmThread` + `eventUnblockJavaThread` → Java `MMEventListener` 收 END_OF_MEDIA/STARTED/STOPPED → `PlayerListener` 回调正常派发，游戏状态机不再卡在等待
- **编译验证**：`vita-port/build && make` 全绿，`[100%] Built target midp_vita.vpk-vpk`；改动仅 vita-port 三文件 + 新文件，符合最小 diff
- **已知边缘限制**：PLAY_BLOCK 嵌套 PLAY_BLOCK 不支持（上游校验器同样禁止）；REPEAT multiplier 上限钳到 JTS_MAX_REPEAT(8)（规范上限 127，防事件表溢出）；JTS 序列上限 32768 字节（`nGetJavaBufferSize` 返回值，DirectTone.setSequence 一次性整段传输）

### WMA/SMS(JSR 120) 接入 Vita（2026-09-01）
- **目标**：让 `sms://`、`cbs://` 协议被 GCF 识别（`Connector.open("sms://:1234")` 不再抛 InvalidArgumentException；WMA API 类进 ROM）
- **构建接线（核心发现）**：`build_vita.sh` 用命令行覆盖一切 makefile 赋值——启用 JSR 只需在它里面加 `USE_JSR_120=true JSR_120_DIR=/home/zyb/vitasdk/samples/j2me/phoneme_source/phoneME/jsr120`（jsr120 在 phoneme_source 里，同 jsr135 模式）。jsr120 的 `common_defs.gmk` 会强制 `USE_ABSTRACTIONS=true`，而 `build_vita.sh` 本来就传了 `USE_ABSTRACTIONS=true + ABSTRACTIONS_DIR`，刚好满足
- **首编 60 个 javac 错误的真相**：不是源码问题——`alljavalist.txt` 是增量文件列表（appendfiles 写 `$?`，即比 classes.zip 新的文件），jsr120 源码时间戳（8/25）早于旧 classes.zip（8/31）导致只有 4 个 JPP 生成文件（其规则让其必"新"）被编译。**修复：`touch` jsr120 全部 .java/.jpp + 删 classes.zip**
- **javacall_sms_* 符号缺失**：native 层 `smsProtocol_javacall.c` 调 `javacall_sms_*`/`javacall_cbs_*`/`javacall_strdup`，无实现则 `libmidp.so` 链接失败。新增 `vita-port/src/vita_sms_javacall.c`（stub：send/listen 返回 FAIL，`get_number_of_segments` 按 GSM 03.40 真实计算，strdup 用 javacall_malloc）
- **把 Vita 源文件编进 phoneME 构建的通用机制（本项目原创）**：`build/vita_arm/vita_overlay.mk`（vpath % → vita-port/src；JTWI_NATIVE_FILES += vita_sms_javacall.c），经 `export MAKEFILES=<overlay>` 在 `build_vita.sh` 里注入。GNU make 的 MAKEFILES 预加载文件可追加 vpath 与变量，**零修改上游 gmk**，符合最小 diff 原则
- **验证方法**：① `jar -tf classes.zip | grep wireless` 显示 5 个接口类 ② `nativeFunctionTable.cpp` 有 `Java_com_sun_midp_io_j2me_sms_Protocol_*` 全部 7 个 KNI ③ `ROMImage_01.cpp` 中 grep 到 romize 后的 sms native 指针 ④ `libmidp.so` 无 undefined reference
- **运行时行为**：`sms://` 连接可创建；发短信返回失败（Vita 无蜂窝）；收听端口注册失败。游戏内 SMS 功能会优雅降级而非崩溃
- 注意：`vita_overlay.mk` 在 `phoneme-midp/build/vita_arm/`（生成目录，重建不丢）；若频繁 clean build 建议移到 `midp-vita/` 并调整 build_vita.sh 路径

### 游戏卡死真因：VM 每类 stderr 转储洪水（2026-09-01，samples 27d7ef0）
- **症状**：游戏"卡死不动"且无异常——实为 `ClassFileParser.cpp` 里遗留的两段调试转储（`CP tags`/`PRE idx`/`Utf8 idx ... NOT in heap`，诊断 UTF8/hidden 问题时加的）**每加载一个类向 stderr 倾倒几百行**（小游戏 17k 行），Vita3K 模拟 sceIoWrite 慢，I/O 洪水即"卡死"。Vita3K 日志特征：`sceIoWrite: fd 0x7, size: 5x` 高频重复
- **即时缓解**：vita_main.c 把 stderr 重定向 /dev/null（freopen 失败则 `sceIoClose(2)` 走廉价 EBADF）。Java 输出走 `JVMSPI_PrintRaw` 自己的 fd（vm_output.log）不受影响
- **正确修复（已提交待生效）**：cldc commit `291ab15` 用 `VITA_CP_DEBUG` 宏 gate 掉 dump + 静音 Universe 的每类 hidden 警告——**待 VM 重编环境恢复后重编生效**
- **⚠️ CLDC VM 重编配方问题（未解决）**：完整 `make debug` 会失败——romgen 分支的 `CPP_DEF_FLAGS += -B/usr/bin -m32 ...` 与 `-DCROSS_GENERATOR=1` 泄漏进 target 构建上下文（root.make 的 tools/loopgen/_romgen/_debug 多阶段在同一 make 里共享变量，且 jvm.make 1462 行 `FORCE_GCC` 非空时所有编译角色都被替换）。曾试：`IsTarget=true`+`FORCE_GCC=`+`GNU_TOOLS_DIR=`+`CPP_DEF_FLAGS=` 命令行覆盖→卡在 AsmStubs_x86_64.s / Interpreter_c.cpp（解释器兼容）。重编前需理清 root.make 的多阶段变量流
- **版本号陷阱**：GenVersion 取 HEAD commit，先改代码后 commit 会导致 VPK 版本串不反映二进制内容——**务必先 commit 再 build**

### 游戏卡死修复：playTone 同步阻塞冻结 VM（2026-09-01，samples 64652d4）
- **症状**：ANI 修复后的 VPK，游戏运行中卡死不动、无任何异常日志
- **根因**：`javacall_media_play_tone` 原实现是**同步阻塞**——生成 PCM 后逐块 `sceAudioOutOutput` 播完整段才返回。CLDC 是绿色线程模型，nPlayTone 阻塞 = 所有 Java 线程冻结。游戏主循环高频调 tone（按键音/BGM 序列），表现为卡死
- **修复**：专职播放线程（`sceKernelCreateThread`，原生线程；libpthread 链接有玄学问题不用）+ 无锁序列号握手：VM 线程只写 note/dur/vol 并 `g_req_seq++` 即返回；播放线程 4ms 轮询，播新前比对 seq 实现抢占，`stop_tone` 置标志中断输出；audio port 由播放线程打开消除竞态
- **下一嫌疑（未实证）**：若修复后仍"启动即永久卡死"，则指向 http→socket 挂起：`CommonDS` 对 `http:` locator 阻塞式 `Connector.open`+`getResponseCode`，而 Vita 的 pcsl 网络是 53 字节 stub（`pcsl/vita_arm/lib/libpcsl_network.c`）。届时可二分验证（临时禁 http 子系统重编 ROM）

### 游戏启动崩溃修复：ANI 线程池未初始化（2026-09-01，samples 152d350）
- **症状**：jsr135 集成后游戏启动即死循环，Vita3K 日志刷 `Invalid read of uint32_t at 0x4`，`ldrex/strex` 原子指令重试，PC=0x810aca7a
- **根因链**：MMAPI 可用后游戏 `Manager.createPlayer(http://...)` 走通 → 媒体下载进 ANI 阻塞框架 → `PoolThread` 的静态事件（全局零初始化=NULL）被 `Os_SignalEvent/Os_WaitForEvent` 使用 → `pthread_mutex_unlock(&NULL->mutex)`（偏移 4）→ pteos 原子操作死循环。**`ANI_Initialize()` 在 CLDC-HI 启动流程中无任何调用者（上游同样），线程池从未初始化**
- **修复**：vita_main.c 在 `runMidlet` 前显式调 `ANI_Initialize()`（符号未修饰，C 可直接链接）
- **诊断方法论**：Vita3K 的 PC/LR → `arm-vita-eabi-addr2line -e build/midp_vita -f` 定位到函数 → objdump 全量反汇编 + grep `bl <目标地址>` 枚举调用点 → addr2line 每个调用点还原调用图。这套路子可复用

### 中文字库扩容 + 自描述 header（2026-09-01，samples b593d80/a9c1049）
- fontgen 新增 4 段：带圈数字 0x2460、希腊 0x0386、CJK 尾段 0x9FA6-0x9FFF、半角片假名 0xFF61（共 21429 字形，1.41MB）
- bank header 升级为自描述（magic "J2FB" v1：nsec/gw/gh/stride/data_off/段表），**vita_font.c 加载时动态解析并校验**——以后扩段只改 fontgen.c，运行时零改动
- **构建资产已入库**：`config/font.ttf`（16MB，fontgen 唯一输入，此前被顶层 `*.bin`/缺失规则挡在库外！）和 `config/fontbitmap.bin`；tools/font.ttf 重复副本已删；重生成用 `tools/gen_font.sh`（必须 `/usr/bin/gcc-11 -B/usr/bin`，PATH 里的 vitasdk gcc 是 ARM 交叉工具）
- Vita 端已知空缺：半角 ｱ(U+FF71) 等个别字形该字体缺失（62/63 覆盖）；CJK Ext-A（0x3400 段）未纳入，需要时在 fontgen.c 加段即可

### 音频/MMAPI 集成（2026-09-01，samples b29d725 / phoneme-midp 9240db2 / phoneme-cldc 4000337）
- **JSR-135 已进构建**：`build_vita.sh` 开 `USE_JSR_135=true`；javax.microedition.media + com.sun.mmedia 共 228 类已 ROM 化（ROMLog 3442 处引用）；jsr135 KNI 对象（31 个）进 `obj/arm/libobj.a`；nativeFunctionTable 含 nPlayTone 等 247 条
- **javacall 层**：`vita_audio_javacall.c` 目前只实现 playTone（SceAudioOut 合成方波）+ 24 个符号；头文件声明的 87 个 media 函数中 ~63 个未实现（realize/prefetch/start 等 DirectPlayer 路径）——但链接零错误，因当前 caps 只声明 tone，Java 层不会走未实现路径。**WAV 文件播放是下一迭代**
- **工具链事故与修复**：系统 g++-13 被卸载 → 8/29 的 loopgen/romgen 二进制要求 GLIBC_2.38 全废。修复：cfg 改 `FORCE_GCC = /usr/bin/g++-11 -B/usr/bin -m32`；另发现 romgen/loopgen 的 vpath 错拿 MIDP 的 ROMImage.cpp（应链 ROMSkeleton stub），解法是把 Skeleton 副本放到 `romgen/app/`、`loopgen/app/` 下（当前目录优先于 vpath）。**若重编宿主工具，先确认这两个目录里的 ROMImage.cpp 副本还在**
- **构建陷阱记录**：①`unzip` 从 WSL 环境消失导致 zip 内容检查假阴性（用 python zipfile）；②MIDP java 编译按 alljavalist.txt 增量（mtime），上游 jsr135 文件 mtime 老会被跳过——启用新子系统后要删 `tmpclasses/`+`classes.zip` 强制全量；③顶层 `GNUmakefile` 的 build 输出是 `classes.zip`（不是 midp_classes.zip）

### VPK 图标与版本体系（2026-08-31，samples commit 10b4fac）
- LiveArea 资源在 `vita-port/assets/sce_sys/`（icon0 128x128 / bg 840x500 / startup 280x158 / template.xml），由 `vita-port/tools/gen_assets.py`（Pillow）再生成
- 版本自动生成：`vita-port/cmake/GenVersion.cmake` 构建时产出 `vita_version.h`：APP_VER（手动，CMake `VITA_VERSION`）+ 构建号（samples 仓库 commit 数）+ 短哈希，形如 `J2ME Player v01.00 b120 (0fc9e8d)`
- 版本显示在菜单标题栏，并写进 boot_log.txt 首行——**从截图或日志即可定位用户装的是哪个构建**
- 迭代规则：功能里程碑手动升 `VITA_VERSION`（如 01.00→01.01）；其余改动构建号自动递增，无需维护

### 仓库基线整理（2026-08-31）
- `phoneme-cldc` commit `98b12b5`：103 个修改的上游文件 + 23 个新增（`src/vm/os/vita/`、`src/anilib/vita/`、`AsmStubs_x86_64.s`、`build/vita_arm/{Makefile,vita_arm.cfg,vita_arm.mk,embed_midp.py}`、`.gitignore`）
- `phoneme-midp` commit `8a04358`：16 个修改的 gmk/native + 50 个新增（`src/porting/vita/`、各 `vita_arm/` 平台目录、顶层 `GNUmakefile`/`build_vita.sh`/`PORTING.md`、`build/vita_arm` 平台 makefile、`.gitignore`）
- `.gitignore` 策略：`build/vita_arm/*` 全忽略 + 白名单放行手写配置（ROMImage_*.cpp、ROMImageGenerated.hpp、classes/、obj/、pcsl/、*.o、*.a 均为产物）；忽略便利符号链接（cldc 的 `src/midp`、midp 的 `src/src`）
- 已删除垃圾：`Main.java.bak`、`Defs.gmk.original`、`null.o`、`libcldc_vm_midp_g.a.backup_*`
- 之后判断"我改了什么"直接 `git diff 98b12b5`（cldc）/ `git diff 8a04358`（midp）

### vita-port/src/vita_font.c — 文字全空白的最终根因（commit 0fc9e8d）
- **症状**：font_debug.log 中所有 draw 调用 `pixels_drawn=0`，连屏幕中央的 CJK 都是 0；字母和汉字全都不显示。
- **根因**：多轮改写后，`fb_load()` 函数定义存在但**全文件无任何调用点**，`fb_ready` 永远为 0，`fb_glyph()` 永远返回 NULL → ASCII 走"未找到且 <0x2E80"直接不画，CJK 只画豆腐框。
- **修复**：新增 `fb_ensure()` 懒加载（先 `ux0:/data/J2ME00001/fontbitmap.bin` 覆盖，再 `app0:/data/J2ME00001/fontbitmap.bin` VPK 内副本），在 gxjport 三个入口（get_font_info / get_chars_width / draw_chars）调用。
- **验证结论（PC 端）**：fontbitmap.bin 1,397,920 字节布局正确：header 40B + 5 段（ASCII 95 / CJK 20902 / punct 64 / full 95 / gen 24）× 66B 字形，CJK 20902 个字形全部非零。
- **教训**：加日志必须让日志能区分各分支（豆腐框分支此前不计数，导致无法区分"画了豆腐"和"根本没画"）；per-call 日志要节流（此前 7000 行重复）。

### Universe.cpp
```c
#if defined(VITA) || defined(VITA_SDK)
    if (!jvm_class()->access_flags().is_hidden()) {
        fprintf(stderr, "[VITA] Warning: JVM class not marked as hidden, skipping check\n");
    }
#else
    if (!jvm_class()->access_flags().is_hidden()) {
        JVM_FATAL(jvm_class_must_be_hidden);
        return false;
    }
#endif
```

### main.c
```c
char *midp_args[] = {
    "runMidlet",
    "-classpathext",
    "ux0:/data/J2ME00001/midp_system.jar:ux0:/data/J2ME00001/Hello.jar",
    "ux0:/data/J2ME00001/Hello.jar",
    "HelloMIDlet"
};
```

### midp_run.c (getClassPathPlus)
支持 `:` 分隔的多个 JAR 路径

### vita_arm.cfg (启用 UseROM)
```makefile
# Enable ROM (loads system classes from ROMImage.o, embedded in VPK)
# This eliminates the need for midp_system.jar on filesystem
export ENABLE_ROM_GENERATOR := true
export ENABLE_ROM_GENERATOR__BY := vita_arm.cfg
export ROMIZING := 1
export USE_ROM := 1
```

### build_midp_vita.step1_5_rom
ROM jar 现在包含:
- CLDC classes
- MIDP classes (com/, java/, javax/)
- midp_system.jar 内容
- Hello.jar 内容

---

## 调试日志

Vita3K 日志位置:
- `C:/Users/zyb/AppData/Roaming/Vita3K/Vita3K/ux0/data/midp_debug.log`
- `C:/Users/zyb/AppData/Roaming/Vita3K/Vita3K/ux0/data/debug_log.txt`

---

## 参考资料

- phoneME CLDC: `src/vm/share/ROM/` 目录
- MIDP 构建: `phoneme-midp/build/vita_arm/GNUmakefile`
- romgen 配置: `dist/lib/cldc_rom.cfg`
- UseROM 配置: `src/vm/share/utilities/Globals.hpp`
- ROM 初始化: `src/vm/share/ROM/ROM.cpp`
- **8-30 第二轮修复（用户反馈"只有 Exit"+ 截图分析）**:
  1) 缩放公式反了：旧代码把 240x320 拉伸成 720x544（截图白区宽~720 证实），改为等比
     fit: scale=min(PW/VW,PH/VH)，竖屏 407x543 / 横屏 726x544 居中。
  2) **Java 堆仅 1.25MB（Form 空白根因）**: `getInternalPropertyInt("JAVA_HEAP_SIZE")`
     在复制来的 vita_pcsl.c stub 里返回 0 → 默认 1280KB，装载 183 张皮肤图 OOM
     （大图失败→标题/正文不画，softbar 小图幸存；GC 风暴→3FPS）。stub 现返回 32MB。
  3) **方向**: Vita 横持，默认虚拟屏改为 320x240 landscape；launch.cfg 第三行
     portrait|landscape 可切（vita_display_set_orientation 须在 VM 启动前调）。
     internal.config 与 internal.landscape.config 两份随 VPK 打包，启动器按方向拷贝。
  待确认: 用户重测 Form 是否显示、FPS 是否恢复。
- **8-30 第三轮修复（闪退）**: JAVA_HEAP_SIZE=32MB 后 VM 报 "Could not allocate VM heap"
  直接退出。根因: CLDC 堆走 jvm_malloc→newlib，vitaSDK newlib malloc 池默认仅 32MB
  （sbrk.c 的 `_newlib_heap_size`），32MB Java 堆塞不进。修复: vita_main.c 定义
  `unsigned int _newlib_heap_size_user = 64MB`（覆盖弱符号，app 分区 MEMSIZE=128MB 内）。
  链接验证: nm 显示 _newlib_heap_size_user 已从 w 变为 D 81251f48。
  经验: Vita3K 日志 tty 输出（vm_output.log: "Could not allocate VM heap"）定位此类问题最快。
- **8-30 第四轮（用户要求）**: 默认方向改回竖屏 240x320（J2ME 游戏主流为竖屏设计，
  320x240 默认会让竖屏游戏布局错乱）。vita_main.c 默认 orient="portrait"，
  VITA_DEFAULT_LANDSCAPE=0。landscape 仍可经 launch.cfg 第三行选择。
- **8-30 第五轮（日志确诊，Form 空白真根因）**: vm_output.log 抛
  `IllegalArgumentException: This display does not support title` @ Display.setCurrent(bci=45)
  → startApp 失败，Form 从未真正显示（白屏+Exit 只是显示系统初始层）。
  根因: 我写的 lfjport_get_display_capabilities() 返回 0 = 什么都不支持。
  修复: 返回 0x3FF（DisplayDevice.java 位: 1=input,2=commands,4=forms,8=ticker,
  16=title,32=alerts,64=lists,128=textboxes,256=tabbedpanes,512=fileselectors）。
  教训: 参考 lfjport_fb_export.c 复制来的"返回 0"stub 不是无害默认值，是硬故障源。
  另: debug_log.txt 显示 SkinResources 会先找 ux0:/data/J2ME00001/lib/*.png 再回退 jar
  （lib 无自定义皮肤属正常回退路径）。
- **🎉 8-30 里程碑: 首次完整运行成功**。Form 标题/正文/Exit 全部正常显示（竖屏 240x320）。
  渲染链+高级UI+皮肤+字体+32MB堆+能力位修复全部验证通过。
  待验证: InputTest（按键链）、CanvasTest（drawImage/PNG）、真实游戏。
- **8-30 游戏管理菜单 (vita_menu.c v1)**: JVM 启动前的原生菜单（C 直接画 960x544 fb + 5x7 字体）。
  目录约定: games/<名>/game.jar+game.cfg（首行类名/次行方向）、inbox/*.jar 待安装。
  键位: ↑↓选择 ✕启动 ○切方向 □×2卸载 START装inbox SELECT重扫；列表空时 ✕ 退回内置 demo。
  MIDlet 类名自动解析: 复用 libobj.a 的 midpOpenJar/midpGetJarEntry 读 MANIFEST "MIDlet-1:" 第三段
  （注意 midpFree 是 midpMalloc.h 的宏；pcsl_string 可栈上构造 {jchar*,len,0}，路径走 vita_pcsl 直通）。
  launch.cfg 降级为手动覆盖方式（菜单无游戏时 ✕ 退出后生效）。
  已知限制: 所有游戏共享 internal 套件 RMS（appdb 单一空间），存档互通；按套件隔离待做。
  用户自行在 CMakeLists 加了 VITA_SKIN_FILE_ARGS 把皮肤 PNG 同时打入 VPK 的 lib/（皮肤加载双保险）。
- **8-30 菜单重写为 vita2d（修闪烁/4058FPS）**: 用户反馈菜单疯狂闪烁（Vita3K 显示 4058FPS）。
  原因: 手画 framebuffer 循环无 vsync，全速 SetFrameBuf(IMMEDIATE)。按用户建议改用 Vita 专门
  UI 接口 **vita2d**（SDK 已内置 libvita2d.a）: vita2d_init/load_default_pgf(系统 PGF 字体)/
  start_drawing/clear/.../end_drawing/swap_buffers(带 vsync)。输入连发改时间基
  (sceKernelGetProcessTimeWide, 300ms 首延 100ms 连发)，原帧计数连发在千 FPS 下会爆炸。
  链接: vita2d + ScePgf_stub + SceAppUtil_stub + SceCommonDialog_stub
  （无 SceFont/SceCommonGui stub；sceCommonDialogUpdate 由 CommonDialog 提供）。
  vita2d_fini 后 JVM/裸 framebuffer 显示接管，无冲突。
- **8-30 修菜单 jar 路径 bug**: debug_log 显示 midpOpenJar 路径成 "u0/aaJM001gms壢絅/aejr"
  （= ux0:/data/J2ME00001/games/.../game.jar 隔字节丢字符）。根因: vita_menu.c 把 UTF-8
  字节指针直接强转 pcsl_string.data (jchar*=16位单元)，读取按 16 位步长隔字节取。
  修复: make_pcsl_string() 把每字节扩展为一个 jchar 再传，用完 free。
  教训: pcsl_string.data 是 jchar 数组不是字节串，构造时必须逐字节扩展。
- **8-30 菜单交互重构（用户反馈"装完不知道怎么打开"）**: 列表 → 点击/✕ 打开游戏 → 弹出
  操作对话框（启动/方向/卸载×2确认/返回），对话框可物理键或触摸直接点选项，点外部关闭。
  新增前触屏支持: sceTouchSetSamplingState(START)+sceTouchPeek（本 SDK 无
  sceTouchPeekBufferPositive/sampling_mode，是 state 命名），坐标 1920x1088→/2 映射，
  tap=按下抬起沿。链接加 SceTouch_stub。
  注意: 菜单中文文案依赖 Vita3K/系统 PGF 字体的 CJK 覆盖，真机可能豆腐（Vita 官方无简中），
  到时改英文文案或加载自定义字体。
- **8-30 菜单中文渲染修复（用户截图: 中文全是"_"）**: Vita 系统 PGF 字体无简体字形。
  方案: vdpm 装 freetype/zlib（SDK 原缺），链接链 vita2d→freetype→png→bz2→z
  （freetype 开了 png+bzip2 支持），打包 Windows Deng.ttf(16MB) 到 VPK
  data/J2ME00001/font.ttf，vita2d_load_font_file + vita2d_font_draw_text 渲染
  （ui_text/ui_textf 封装，PGF 回退）。VPK 12MB。
  字体版权: Deng 为微软字体，仅本地使用；分发需换开源字体（Noto Sans SC 子集等）。
  触摸: 代码已用 sceTouchPeek（tap=抬起沿），Vita3K 需鼠标左键=前触摸；物理键为主。
  按键布局已 Vita 风格: ✕确认/打开、○返回。
- **8-30 修自动解析失败的真根因**: vita_pcsl.c 的 pcsl_string_utf8_length 返回 length*3
  （粗略上限），而 pcsl_string_get_utf8_data 产出 length 字节——midpGetJarEntry 用该
  长度做 jar 条目名匹配（20 字节名 vs 60 字节 length）永远失败，MANIFEST 读不出 →
  安装时 game.cfg 写 "-" → "没有 MIDlet 类"。修复: 两个 stub 改为一致的真 UTF-8
  编码/计数（ASCII 1 字节、BMP 2-3 字节）。口袋灵兽的类名 = GameMidlet（用户手动
  填 cfg 亦可立即游玩）。
- **8-30 启动游戏闪退（选中游戏后、JVM 前死亡）**: boot_log 只有首行 = 死在菜单返回前。
  嫌疑: vita2d_free_pgf/vita2d_fini 在 Vita3K 上的 GXM teardown。处理: 跳过菜单的
  vita2d 卸载（进程单次运行泄漏无碍，JVM 用裸 sceDisplay 与 vita2d 的 GXM 上下文无关），
  并在启动链路加 [menu] stderr trace（parse 结果/launching/handing over）定位下一次问题。
  注意: 用户并行修改了 vita_display.c（加 get_virtual_size/get_viewport 供触摸映射），
  dst_* 声明已上移修编译；后续编辑前先读文件避免冲突。

- **8-30 功能补全批次（P2触摸/P4生命周期/P3中文字体）**: 全部构建通过，velf nm 验证生效。
  *P2 触摸*: vita_input.c 加 touch_poll()——sceTouchSetSamplingState(START)+sceTouchPeek
  （采样起点修复: 菜单代码有、input 此前漏，peek 前 must START），4095 坐标经
  vita_display_get_viewport() (dst_x/dst_y/dst_w/dst_h) 映射 240x320 虚拟坐标，
  状态机 PRESSED/DRAGGED(坐标变)/RELEASED(抬手) → MIDP_PEN_EVENT (X_POS/Y_POS/ACTION
  用 KEYMAP_STATE_*)，与 qvfb 参考一致走 UI_SIGNAL；vita_display.c pen_supported/
  motion_supported 翻成 KNI_TRUE。
  *P4 生命周期*: lifecycle_poll() 轮询 sceAppMgrReceiveSystemEvent，任意事件→
  midp_suspend()（libobj.a 已有，发 PAUSE_ALL_EVENT→AMS→MIDletPeer.pauseApp()），
  ON_RESUME→midp_resume()（ACTIVATE_ALL_EVENT→startApp()）。PS 键切后台/回来即触发。
  *P3 中文字体*: 新文件 vita_font.c——CMake 删 libobj 的 gxjport_text.o（空 stub），
  本文件供 gxjport_draw_chars/get_chars_width/get_font_info 三符号；
  stb_truetype v1.26 运行时光栅化 TTF（src/stb_truetype.h，PUBLIC DOMAIN），
  字体在 VPK data/J2ME00001/font.ttf（用户已放 Deng.ttf 16MB；ux0:/data/J2ME00001/font.ttf
  可同名覆盖）。glyph 缓存 1024 桶 per (codepoint,size,style)，MIDP SIZE 8/16/24→
  16/20/24px，bold=alpha 右移浸染，CJK 缺字形画 tofu 框；返回 KNI_TRUE 接管渲染，
  无字体/无 glyph 返回 KNI_FALSE 回退内置 5x7（ASCII 兜底）。gxj_screen_buffer
  字段名注意是 pixelData/alphaData 不是 pixels/alpha。
  *测试 MIDlet*: FontTest（MIDlet-5）中英混排+三字号+加粗 Canvas 直绘；NetTest(MIDlet-4)
  上轮已有。MANIFEST 同步 5 个。
  构建链注意: build_jar.sh javac -source 1.3 不能用枚举/泛型/静态嵌套类以外的
   JDK5+ 语法；FontTest 的内部类须 static class（CLDC preverify 要求）。
  *遗留*: vita_menu.c 中文渲染走 vita2d+freetype 独立于本字体系统（互不影响）；
  FontTest 的 chameleon 高级 UI 路径未测（Canvas 直绘已覆盖 gxjport 链）；
  触摸 Vita3K 需鼠标左键模拟；P5 横屏 launch.cfg 第三行 portrait|landscape 上轮已
  实现（vita_main.c read_launch_cfg + vita_display_set_orientation）。
- **8-30 游戏空白诊断（口袋灵兽）**: boot_log 全链路 OK、GameMidlet 自动解析成功、VM 启动、
  Java LCDUI init 执行（绿点出现）、但 refresh_count 恒 1（游戏从未 paint）、无任何异常输出
  （USE_CLDC_RELEASE=true 关闭 phoneME 内部日志；startApp 挂起而非异常——异常会走
  MIDletStateHandler→stdout）。结论: 游戏 startApp 挂起，头号嫌疑 MMAPI 空壳阻塞
  （UnsatisfiedLinkError 是 Error 穿透普通 catch 也可能）。对策: 新增 ProbeMIDlet
  （props/canvas/game包/offscreen image/PNG解码/RMS/MMAPI tone/wav/threads 逐项
  PASS/FAIL 打印到 System.out→midp_stdout.log），launch.cfg 类名填 ProbeMIDlet 运行。
  VM stdout 通路: JVMSPI_PrintRaw→jvm_printf(=printf)→stdout→midp_stdout.log ✓。
- **8-30 彻底移除 vita2d（最终方案）**: 实验证实 vita2d_fini/free_pgf 在 Vita3K 必崩
  （调用→闪退；跳过→不崩但 GXM display queue 占着显示→后续 MIDlet 全空白）。
  菜单最终方案: 手绘 960x544 fb + **freetype 渲染 Deng.ttf 中文**（纯 CPU，无 GXM）
  + sceDisplayWaitVblankStart 限速 60fps。显示路径全程单一（裸 SetFrameBuf），
  JVM 接管零冲突。移除链接里的 vita2d/ScePgf/SceAppUtil/SceCommonDialog stub。
  中文菜单文案改回英文（避免引freeterm复杂度）……（实际: 菜单文案英文，游戏名 UTF-8 经
  freetype 正常渲染中文）。

- **8-30 RMS 验证 MIDlet (RmsTest, MIDlet-6)**: P1 收尾任务。调研确认 RMS 链路:
  Java RecordStore -> com/sun/midp/rms native -> rmsdb_record_store_open
  (rms/record_store/file_based/native/rms.c:515) -> midp_file_cache_open ->
  storage_open (core/storage/reference/native/midpStorage.c:463) ->
  pcsl_file_open = vita_pcsl.c sceIo 实现——理论上开箱即用，RmsTest 用于实测。
  行为: 启动计数器存 RecordStore "vita_rms_test" 记录 1，第 2 次以后运行显示
  [PERSIST OK]; SCREEN 键=Wipe 清档重测。
  *修复一路障*: vita_menu.c 编译 fatal error ft2build.h + FB_H 未定义——
  根因是 cmake 缓存陈旧（增量 make 未感知 CMakeLists 加的 freetype2 include;
  之前菜单中文渲染改造时引入的依赖），touch 重编即过，同时确认 CMakeLists 需
  include_directories($ENV{VITASDK}/arm-vita-eabi/include/freetype2)。
  MIDlet 总清单(Hello.jar 内 6+类): HelloMIDlet/CanvasTest/InputTest(上上轮)/
  NetTest(网络)/FontTest(中文字体)/RmsTest(RMS 持久化)。exit=0，VPK 186 条目。
- **8-30 Hello 空白回归（绿点仅 ~10 帧、画面纯白、19fps 持续刷新）**: 显示链路正常
  （blit 区域=407 竖屏 ✓ 绿点 ✓），chameleon 画的是空内容且持续 repaint；jar 皮肤 183PNG
  完好。对照手段: ProbeMIDlet 增强——加 skin 资源加载测试 + 结尾显示皮肤无关的纯红 Canvas：
  红屏=渲染/flush 链好、问题锁定 chameleon 皮肤/UI；无红屏=Canvas 路径问题。
  midp_stdout.log 的 [PROBE] 逐项给出子系统答卷（含 MMAPI tone/wav、RMS、PNG、线程）。
- **8-30 用户确认回归点=菜单引入后 chameleon 全空（纯白无 Exit，连里程碑的内容也没了）**。
  最大嫌疑: 菜单的 freetype 16MB 字体缓冲常驻 64MB newlib 池（FT Memory_Face 要求 buffer
  存活，一直未释放），加上 VM heap 32MB 把运行期分配挤爆 → chameleon 静默失败。
  修复: 菜单返回前 ft_shutdown()（FT_Done_Face/FreeType + free 字体缓冲），同步菜单
  之后字体无用途。待验证。
- **8-30 重要发现（用户确认回归点后重新对照）**: 里程碑 Form 成功显示时的配置 =
  **虚拟屏 960x544 全屏**（internal.config 原版 960、gxj buffer 960x544）！240x320
  虚拟屏是里程碑之后引入的，chameleon 从此空画（纯白持续 repaint）。皮肤图为小 tile
  （27x27 等）无越界问题。当前实验: 恒定 960x544 虚拟屏（游戏自适应 getWidth/getHeight，
  全屏可玩），config 恢复原版。若 960 恢复显示→240x320 虚拟屏破坏 chameleon 的具体
  机制待查（怀疑 chameleon/skin/gxj 某处对非原生屏尺寸有隐含假设）；虚拟屏/缩放功能
  需在 960 基线上重新设计。
- **8-30 用户回退**: 用户自行将 vita-port 回退到无菜单、240x320 竖屏直启版本
  （删除 vita_menu.c，vita_display/main/input 回退早期版）。已按回退后代码重新编译
  VPK（2.4MB）成功。菜单功能移除；游戏启动暂回 launch.cfg/默认 Hello 方式。
  待办: 基于此干净基线重新验证 Hello 显示，再逐步 reintroduce 功能。
- **8-30 空白回归定案（用户回退验证）**: 回退版（pcsl=旧截断语义）Hello 正常 → 空白真凶 =
  **pcsl utf8 精确化改动**（get_utf8_data 对非 ASCII 输出 2-3 字节真 UTF-8，phoneME 内部
  按 pcsl_string_length（字符数）准备缓冲的调用点被溢出 → 堆破坏 → chameleon 空白/游戏
  挂起）。触发物=口袋灵兽 jar 的中文条目名。"与菜单同轮构建"造成菜单连坐假象。
  **合并决策**: pcsl utf8 保持旧截断语义（不合并精确化——需先审计全部调用点缓冲分配）；
  heap 32MB/皮肤双打包/240x320 竖屏/Probe 等 MIDlet/launch.cfg 保留（回退版已含且验证正常）；
  菜单暂缓（重新设计：确定性资源释放 + utf8 安全前提）。240x320 破坏 chameleon 的旧假设
  同时被推翻（回退版=240x320 且正常）。
  经验: **改 pcsl_string 这类底层字符串原语的语义 = 全局风险**，任何精确化都必须配套
  审计所有调用点的缓冲约定；"同轮多改动"出回归时优先用回退+二分定位。
- **8-30 合并 0001-save-change.patch**（另一会话的完整功能版，基于回退基线）:
  - vita_menu.c: freetype 手绘版（无 vita2d，ft_shutdown 释放）✓
  - **vita_font.c + stb_truetype**: 替换 libobj 的 gxjport_text.o 空壳（ar d gxjport_text），
    游戏 drawString 经 stb_truetype 渲染 app0:/data/J2ME00001/font.ttf（用户可用
    ux0:/data/J2ME00001/font.ttf 覆盖）→ **游戏内 CJK 文字可显示**
  - **vita_net.c**: 真 BSD socket（SceNet/SceNetCtl）替换 pcsl socket stub +
    vita_net_early_init(VM 前) + vita_net_poll(select 挂 checkForSystemSignal 路径)
  - vita_input.c: +PEN 事件(触摸 viewport 映射) + suspend/resume(appmgr) + net_poll
  - vita_display.c: 恒 960x544 pin + viewport 导出
  - midlets: FontTest/NetTest/RmsTest 诊断 MIDlet
  - **合并时排除**: pcsl utf8 精确化两处已回退为旧截断语义（空白真凶，见上）
  构建 ✓。测试顺序建议: Hello(基线) → FontTest(游戏内 CJK) → RmsTest → NetTest → 口袋灵兽。
- **8-30 合并版 Hello 空白 → 二分开始**: 合并 patch 后 Hello 空白（utf8 已排除）。
  Vita3K 日志: font.ttf(16MB) 在 VM 启动 0.1s 被读入（vita_font 惰性加载），之后 14s
  静默直到关闭 → 白屏 = paint 卡死在第一次 drawString（stb_truetype 渲染 16MB Deng.ttf）
  或 16MB font_data 挤爆 64MB newlib 池。二分: 本轮构建已排除 vita_font（ar d 列表恢复
  gxjport_text.o stub、vita_font.c 移出编译）+ vita_net_poll 事件泵旁路
  （ENABLE_VITA_NET_POLL 条件编译）；vita_net.c 保留编译（pcsl 网络符号需要，
  net_poll 被旁路，early_init 无害）。待用户验证 Hello。
  若恢复 → 逐个开回 font/net 定位；font 的修复方向 = 换小子集字体 + 查 stbtt 渲染路径。
- **8-30 找到事件风暴真凶（lifecycle_poll）**: config 一致性假说也被推翻（960+960 一致
  仍空白）。最终定位: patch 版 vita_input.c 的 lifecycle_poll()——Vita3K 的
  sceAppMgrReceiveSystemEvent 恒返回成功（零填充事件），while 循环无限轮转且事件非
  RESUME → **midp_suspend() 风暴** → PAUSE_ALL 事件灌爆 → MIDlet 启动即被暂停 →
  永不 paint → 白屏（19fps = suspend 事件风暴的泵频）。修复: lifecycle_poll 整体旁路
  （注释说明，真机+正确过滤后才能启用）。cfg "-" 占位说明: scan 时 manifest 兜底成功
  （boot_log class 正确），后续加 scan 成功回写 cfg 优化。
- **8-30 修 scan_games cfg 解析 bug**（用户手动改 cfg 未生效）: 重写版读 cfg 第二行时
  path 未初始化（sceIoOpen 垃圾路径）→ 手写的方向/类名可能被忽略。已修: 正确路径 + 两行
  完整解析（首行类名、次行方向）+ manifest 解析成功后 save_cfg 回写。
  等待用户反馈: lifecycle_poll 旁路版 Hello 是否恢复显示。
- **8-30 suspend 风暴修复后的新状态**: Form 管线走通大半（softbar "Done" 出现——里程碑后
  首次有非白内容），4FPS 偏低、标题/body 仍未画。已按用户要求切回 240x320 竖屏默认
  （landscape 320x240 可选），internal.config/landscape 双份同步。
  下一步诊断工具: ProbeMIDlet（含 skin 资源加载测试 + 纯红 Canvas 对照）——若红屏显示
  则 Canvas 链 OK、问题锁定 chameleon skin；[PROBE] 答卷在 midp_stdout.log。
- **🎉 8-30 里程碑: 真实游戏链路跑通**。用户在 games/Hello/game.cfg 手填 HelloMIDlet 后
  成功运行（此前自动解析的类名疑似带控制字符污染 → Class.forName CNFE，异常消息不可见
  尾字符）。修复: parse_manifest_class 只拷贝可打印字符（>=0x20）。
  待验证: 口袋灵兽（GameMidlet）实际运行表现（画面/按键/速度）。
  已知遗留: ①MMAPI 音频未实现（游戏静音，某些游戏可能异常）②RMS 全游戏共享未隔离
  ③性能 4FPS 待优化 ④240x320 小屏 chameleon 空白待查（当前 960x544 全屏直出可玩）
  ⑤菜单功能已回合并（freetype 版）。
- **8-30 CNFE 随机回归（HelloMIDlet ClassNotFound 再次出现）**: 用户手填类名成功一次后
  再次 CNFE → 随机性指向**内存水位**: CLDC JarFileParser 的 BufferedFile 缓存/类字节
  读取走 64MB newlib 池，池被 font 16MB(菜单)+VM heap 32MB+杂项瓜分后类加载分配可能
  失败→CNFE。对策: _newlib_heap_size_user 64→96MB（MEMSIZE=128 分区内）。
  备选嫌疑: ①game.jar 内容（确认用户 games/Hello/game.jar 是最新构建的 Hello.jar，
  含 HelloMIDlet.class 1587 字节）②JarFileParser 的 CacheJarEntries 缓存。
  若扩池后仍 CNFE → 在 JarFileParser 加载失败处加 trace 打印具体失败点。
- **8-30 Plan B 落地（绕开 VM jar 加载）**: CNFE 持续（多次运行均 ClassNotFound HelloMIDlet，
  jar 本地验证含该类）→ CLDC 从 classpath 第二个 jar 加载应用类不可靠。利用 CLDC 原生
  目录 classpath 支持（ClassPathAccess::open_entry_from_file jar 打不开即按目录找文件）:
  启动器把游戏 jar 全条目解压到 ux0:/data/J2ME00001/appclasses/（midpIterateJarEntries
  收集名字[jchar→正确UTF-8] + midpGetJarEntry 解出 + 逐级 mkdir 写文件），classpath 第二项
  指向目录。解压失败自动回退 jar 模式。中文条目名在 GetJarEntry 匹配（截断语义）下可能
  skip（日志计数）——类（ASCII）必出。boot_log 有 [extract] 统计。
- **8-30 全面回退（用户要求）**: 发现 samples/ 是 git 仓库且 HEAD=288337c "j2me success run
  demo"=用户确认 Hello 正常的版本。git checkout + clean 完整恢复：src 仅 4 文件
  （display 283/input 160/main 223/pcsl 1205 行），无菜单/无net/无font/stb，CMake 极简。
  构建成功。**当前 VPK = 已验证可跑 Hello 的版本**。
  教训（重要）:
  1. patch 合并轮的多项改动（菜单/utf8/lifecycle/net/font）从未逐项验证就叠加，
     出问题时无法定位——以后每加一项必须单独构建+用户验证
  2. samples/.git 的存在一直在 AGENTS.md"非 git 仓库"认知之外，导致走了大弯路
  后续路线（如继续）：以 288337c 为基线小步走，每步：改一项→构建→用户确认→git commit。
- **8-30 功能回归第一步: 游戏菜单（提交 447177d）**: 基于干净基线 288337c 只加一个功能。
  设计（吸取全部教训）: 手绘 fb + 内置 5x7 ASCII 点阵（零新库/零链接变化）、每帧
  sceDisplayWaitVblankStart、退出 free(menu_fb)、不碰 display/input/pcsl、菜单期间独立
  轮询 SceCtrl、MANIFEST 解析（make_pcsl_string 逐字节扩展 + 最后逗号 + 只拷贝可打印字符）。
  功能: games/ 列表、✕ 弹窗（launch/orientation/uninstall×2/back）、START 装 inbox、
  SELECT 重扫、空列表 ✕ 退回 launch.cfg/Hello。中文游戏名第二步（freetype 或点阵中文）。
  测试要点: 菜单正常 + Hello 照常显示 + inbox 装游戏 → 启动。
- **8-31 修 "no MIDlet class in jar"（含 Hello，提交见 git）**: 定案——
  findJarEntryInfo 要求 entry.nameLen == 传入 nameLen（精确相等），而基线 pcsl 的
  pcsl_string_utf8_length = length*3（粗略上限）→ 任何条目查找必失败（与 jar/类名无关，
  Hello 同样中招）。菜单改为自带 mini zip 解析（sceIo 读文件 + EOCD/central directory
  遍历 + zlib raw inflate（inflateInit2 -15），大小写不敏感匹配），不再调用 midpJar/
  pcsl 链。CMake 仅加 zlib 链接。
- **8-31 MIDlet 生命周期插桩**: MIDletStateHandler.createAndRegisterMIDlet 加
  System.out.println（creating/OK/FAILED+栈→midp_stdout.log），编译后类已替换进
  classes/ 与 midp_system.jar。javac 1.3 无 Throwable 重抛→按 CNFE/IE/IAE/RE/Error
  分类重抛。诊断流程: 跑游戏→midp_stdout.log 看 [MIDLET] 行 + vm_output.log 异常栈。
- **8-31 zip 解析诊断插桩**: zip_read_entry 每步（open/size/EOCD/entries/前4条目名/
  匹配/method）fprintf(stderr)→midp_stderr.log；JVMSPI_PrintRaw 发现于 midp_run.c——
  VM 全部输出（Java println/异常栈）都写 ux0:/data/vm_output.log（追加）。MIDletStateHandler
  已插桩（[MIDLET] creating/OK/FAILED+栈 → 同文件，经 stdout）。
  注意: 之前"System.out 被 release 裁剪"推断不成立——JVMSPI_PrintRaw 在 midp_run.c 有
  文件实现，Java 输出必然落 vm_output.log。
- **8-31 终极根因修复（截图日志实锤）**: classpath "ux0:..." 的冒号被 CLDC 分隔符切断
  （entries: "ux0" + "/data/..."），系统类加载成功纯因相对残段碰巧命中 app0:；games/ 下
  的游戏 jar 相对残段不存在 → CNFE → 空白 + skin 加载失败 + Alert 空白崩溃连环。修复:
  main.c chdir(DATA_DIR)（newlib chdir，SceIofilemgr 传递）+ classpath 全相对
  （"midp_system.jar:games/x/game.jar"，条目内零冒号）。960 全屏 + 菜单保留。
  此修复同时应解决 skin 图标加载失败（同 classpath 根因）。
- **8-31 修 midp_system.jar 不更新问题（用户日志: NCDFE FullCanvas + 目录方式查类）**:
  ux0:/data/J2ME00001/midp_system.jar 是运行时文件（无任何代码维护它）——用户机器上是
  旧版（无 Nokia stub 类）→ NoClassDefFoundError: FullCanvas。同时确认: 冒号修复已生效
  （HelloMIDlet 加载成功，推进到 Nokia 类），Nokia stub 类已进 VPK 的 midp_system.jar。
  修复: main.c 每次启动 copy_file(app0 midp_system.jar → DATA_DIR)。
- **8-31 CJK 文字渲染（提交 80f5a12）**: 游戏能跑后用户报告中文不显示——gxjport_text.o
  是空壳（KNI_FALSE→回退 5x7 ASCII）。合并 patch 的 vita_font.c + stb_truetype.h:
  ar d gxjport_text.o + vita_font.c 提供同符号，stb_truetype 渲染 config/font.ttf
  (Deng.ttf 16MB, VPK data/J2ME00001/font.ttf, 可被 ux0:/data/J2ME00001/font.ttf 覆盖)。
  状态: 游戏能进（GameMidlet 全屏 960x544）+ 中文待用户验证。
- **8-31 文字渲染修复（yoff 语义）**: vita_font 恢复（cherry from 80f5a12）+ 修字形垂直
  定位: stbtt yoff 是基线相对（上正），gxj_text 传 line-box top（TOP anchor 原样/
  BASELINE 预调整），正确字形顶部 = y + ascent - yoff。原实现 pen_y+yoff 把字形推出
  clip 框 → 全部不可见。font.ttf 曾被 git clean（未跟踪）删除——已 git add 跟踪。
  提交: 见 git log（CJK glyph vertical position fix）。
- **8-31 文字渲染诊断插桩**: vita_font.c 全路径 trace（try_load_font 路径/字节数/init、
  draw_chars 进入[fallback 或 n+首码点+clip]、每字形渲染尺寸、完成 pen_x）→
  midp_stderr.log。等待用户一轮运行定位文字消失的确切断点。
- **8-31 关键诊断数据（用户 TTY 日志）**: [font] draw_chars n=3 first_cp=0x88C5("装")
  clip=(0,0,240,302) 打印 3 次但 **无 [font] glyph 行** → glyph_get 在 stbtt_GetGlyphBitmap
  前后未到达打印 → stbtt 渲染挂死/极慢/异常返回。已加计时 trace（首次渲染 + 异常值打印
  耗时 ms）。下轮日志见 [font] glyph ... took=XXms 即知 stbtt 是否卡/慢。
- **8-31 修 CJK 宽度 0（提交 8c7e0a4）**: 日志实锤 glyph bitmap=0x18（宽 0 高 24）。
  根因: vita_font.c:166 stbtt_GetGlyphBitmap(&font_info, 0, scale, ...)——x 方向 scale 传 0！
  → 每个字形宽度 0 → draw_chars 里 mask 非 NULL 但 aw=0 → 被跳过 → 文字全消失。
  修复: x/y 同用 scale_for(sc)。经验: **签名 (info, scale_x, scale_y, glyph,...)，
  位置参数 0 恰好落在 scale_x 上，编译器无警告（0 是合法 float）**。
- **8-31 vita_font v2 = 离线点阵字库**: stbtt 在 Vita ARM 渲染字形宽度恒 0（PC 正常），
  放弃运行时渲染。tools/fontgen.c（PC, stbtt 已验证路径）生成 1bpp 点阵库
  （ASCII 95 + CJK 20902 字, 20x22, 1.38MB）→ VPK data/J2ME00001/fontbitmap.bin。
  vita_font.c 加载 bank 查表 blit（前景色, clip 裁剪, CJK tofu 标记）。
  get_chars_width 等宽 20px。fontbitmap.bin 也可被 ux0 覆盖。
- **8-31 修 fontbitmap 头解析错位（v2 不显示的确定根因）**: fb_load 按错位偏移读头
  （fb_gw 读成 0 → 绘制循环零次迭代 → 无任何文字像素）。已按 fontbitmap.bin 实测字节
  布局重写解析：u16@12=GW、u16@16=GH、u16@20=STRIDE、u32@24=data_off、
  u32@28=ascii_count、u32@32=cjk_first、u32@36=cjk_count。
  下轮运行 midp_stderr.log 应出现 "[font] bitmap bank loaded ... (gw=20 gh=22 ...)"。
- **8-31 vita_font 诊断插桩**: draw_chars 进入打印（fb_ready/n/首码点/clip/dest 尺寸）+
  前 40 字符的字形命中打印（glyph=ok/MISSING）。一轮运行即可区分:
  ①无 [font] 行=draw_chars 未被调用（问题在 gxj_text 之前的调用链）
  ②glyph=MISSING=点阵库覆盖/码点问题
  ③全 ok 但屏幕无字=blit 坐标/颜色问题。
- **8-31 日志丢失根因（用户点破）**: CLDC VM 启动时重新绑定 stdio 到 tty0（Vita3K 日志的
  *** TTY: 逐字符输出即证据）→ vita_font 内 fprintf(stderr) 全被劫走，midp_stderr.log 空。
  修复: vita_font 自带 flog()（sceIoOpen ux0:/data/J2ME00001/font_debug.log 直写，
  独立 fd，不经过 stdio）。**重要经验: VM 线程内的日志必须自开 fd，不能依赖
  main.c 的 freopen**。
- **8-31 文字消失终极根因（font_debug.log 实锤）**: dest=320x-2128011192 ——
  gxj_text.c 在调 gxjport_draw_chars 前用 getScreenBuffer() 宏把 NULL dst 映射为
  &gxj_system_screen_buffer（gxj_intern_graphics.h），vita_font.c 未做此映射，
  从 NULL dst 重新推导出未初始化栈结构 → 全部字形画进垃圾内存。修复: 与 gxj_text.c
  相同的 getScreenBuffer 规则。日志链修复也同批提交（flog 独立 fd）。
- **8-31 类型混淆修复（真正的修复）**: font_debug.log 仍显示 dest=320x-2128011192 →
  根因是 gxjport_draw_chars 的 dst 参数**已经是 gxj_screen_buffer\***（gxj_text.c
  自己完成 imagedata→screen_buffer 转换 + getScreenBuffer 映射后才调用），
  vita_font.c 却将其当 java_imagedata* 二次解析 → 读垃圾字段 → 画进野内存。
  修复: 直接 (gxj_screen_buffer*)dst 强转。上一轮 getScreenBuffer 补丁是治标。
- **8-31 决定性诊断（用户要求停止猜测）**: vita_font draw_chars 一次性输出:
  ① dst 与 &gxj_system_screen_buffer 的指针同一性
  ② dest 尺寸/pixelData/alphaData 指针
  ③ **直接向 dest 四角画 40x40 色块**（白/红/绿/蓝 RGB565）——屏幕上看到角标 =
  缓冲即显示缓冲，缺字=字形内容问题；看不到=缓冲不对
  ④ 每字形非空白行计数 ⑤ 首个字形像素写入的物理地址/颜色。
  一轮运行 + 一张截图即可完全判定。
- **8-31 修 fontbitmap 头偏移**: fontgen 输出的头字段在偏移 6(GW u16)、8(GH u8)、
  10(STRIDE u16)、12(data_off u32)、16(ascii_first)、20(ascii_count)、24(cjk_off)、
  28(cjk_first)、32(cjk_count)。vita_font 之前的偏移全错。
- **8-31 fontbitmap 加载修复**: CLDC VM 启动时把 stdio 重绑到 tty0（Vita3K 日志的
  *** TTY: 输出即证据），font.c 内 fprintf(stderr) 全被劫走 → midp_stderr.log 空。
  修复: font.c 内所有日志改用独立 fd 直写 ux0:/data/J2ME00001/font_debug.log。
- **8-31 回退到 288337c 干净基线**: CJK 字体渲染多轮修复仍未解决，回退到已验证
  Hello 正常显示的提交。CJK 字体将作为独立、经过完整测试的功能单独实现。
  基线状态: 无菜单（launch.cfg/默认启动）、240x320 竖屏、5x7 ASCII 文字、
  皮肤已修复、heap 32MB、newlib 64MB、classpath 冒号修复、能力位修复。
  所有这些修复在基线里工作正常。
- **8-31 vita_font 完全重写**: 移除所有残留 vita_font_init 调用（旧 stbtt 版遗留），
  硬编码 GW=20 GH=22 STRIDE=3 DATA_OFF=40，fb_glyph 查 5 个区段返回 1bpp 位图指针，
  draw_chars 直接 blit。fontbitmap.bin 由 tools/fontgen.c 生成（覆盖 ASCII+CJK+punct+
  fullwidth+gen 共 21180 字形），打包进 VPK。
