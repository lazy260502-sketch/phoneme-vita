# J2ME/MIDP on PS Vita - Project Memory
> Last Updated: 2026-09-18

## 2026-09-18 v01.80：**已安装标签方向键失效**——v01.78f 的 `else` 分支吞掉了"列表模式 tab 0"（最小逻辑落空）

> 用户复测 v01.79：**真机启动不再崩溃**（v01.79 修复确认有效），但报出新 UX 缺陷——"在已安装游戏那里，没有上下移动切换游戏，只能在设置标签中上下切换"。

### 根因（读代码即定案，无需转储）
方向键光标分发器（`vita_menu_run` 主循环内）在 v01.78f 被写成：

```c
if (tab == 0 && set_list_grid == TAB_GRIDMODE) { psel = &gsel; ... }   /* ← 带了模式条件 */
else if (tab == 1) { ... } else if (tab == 2) { ... }
else if (tab == 3) { psel = &setsel; nent = SET_N; }
else { has_cursor = 0; nent = 0; }                                     /* ← 本意只兜 tab 4 退出 */
```

最后那个 `else` 只是给 tab 4（退出，无列表）准备的，但**第一个 `if` 额外带了 `GRIDMODE` 条件**，于是最常见的组合——**tab 0 + 列表模式（默认视图）**——不匹配任何分支，直接掉进 tab-4 的 `else`，`has_cursor = 0` → UP/DOWN 完全不动。**设置标签能动纯粹因为它有独立分支**。网格模式不受影响（命中自己的 `if`），所以按 SQUARE 切视图看起来像"绕过办法"。

**这是一类错误的教科书样本：用"多条件 if"表达"某个 tab"，再用裸 `else` 兜"另一个 tab"，中间的组合必然漏。** v01.78f 修的是同一类 bug（tab 3 抢 tab 0 的光标），结果自己在隔壁引入了它的镜像。

### 修复（最小 diff，`vita-port/src/vita_menu.c`）
- 分支改为 **`if (tab == 0)`**，列表/网格的选择移到**分支内部**：网格把 `psel/ptop` 换成 `gsel/gtop`，列表保持 `sel/top`；`nent = game_count` 两条路径共用。
- **一个 tab 一个分支**：以后新增 tab 不会再继承 tab 4 的 `else`。
- 1 秒心跳补记 `tab=%d focus=%d view=%c`——"哪个 tab 拥有方向键"这类问题现在从 `crumb.log` 一眼可判，不必再改代码复现。

### 交付 / 验证
- `out/vpk/midp_vita_v01.80_tab0dpad.vpk`（v01.80 **b265**，内嵌 hash `1a1923f`，md5 `20ccfad5b1c1bd9e907e2fdb21b454c5`，3640308 B）。提交 `1a1923f`。
- 真机复测要点：① 已安装**列表**视图 UP/DOWN 移动光标（本次 bug）；② 已安装**网格**视图 UP/DOWN 按行、LEFT/RIGHT 按格（回归）；③ 未安装/文件/设置 三标签方向键（回归）；④ 退出标签方向键应无反应（v01.78f 行为保持）；⑤ 启动/退出多轮（v01.79+v01.78h 回归）。

### 教训（新增）
- **条件分支覆盖要穷举**：分发器的每个 tab 都应有自己的分支；用裸 `else` 兜"特殊 tab"时，前面的分支条件必须与 tab 一一对应，不能夹带模式/状态条件。
- **两个相邻 bug 同源**：v01.78f 与 v01.80 是同一个"tab↔光标"映射问题的正反两面。改这类表驱动分发时，先把 tab × 模式 的全组合列出来再写分支。
- **心跳日志要带决策变量**：本次若 v01.79 的心跳已含 `tab/focus/view`，用户报障时看 `crumb.log` 即可定性，无需猜测。

## 2026-09-18 v01.79：**真机启动崩溃定案**——taiHEN 插件桩砸 r7（-O0 帧指针），`-fomit-frame-pointer` 根治（重要！）

> 用户报告"真机启动报错"并附 `psp2core-1789660610-0x0000142ca1` 转储。**首次真机验证 v01.78h**（此前全部版本只在 Vita3K 验证）。本轮解析 Sony core dump（gzip → ARM ELF core，24 个 NOTE）完成取证。

### 真机转储解析方法（新工具链，务必记住）
- `psp2dmp` = gzip 压缩的 ARM ELF core。解压后 `readelf -l`：43 段 = 15 个 `0xe0xxxxxx/0x81xxxxxx` 内核/插件段 + 2 组重复的用户段（0x81641000 栈 0x40000、0xd0001000）+ 24 NOTE。
- NOTE 按**标准 ELF note 格式**解析（namesz/descsz/type + 名字）：`THREAD_REG_INFO`(0x1004)、`PROCESS_INFO`(0x1002)、`MODULE_INFO`(0x1005)、`THREAD_INFO`(0x1003)、`STACK_INFO`(0x101b)。
- **MODULE_INFO 里找 `midp_vita` 条目**：名字后 +0x30 附近是 `base/size`（本次 `0x8106f000 / 0x2c6efc`）。**真机加载器滑动段**（本次 +0x6f000，v01.67 时代观测过 +0x72000）——**重定位现场地址时必须减去模块基址偏移**！
- THREAD_REG_INFO 布局（实测）：`+0x00` 头、`+0x0c` tid(0x178)、`+0x10` CPSR=0x60010030(abort+Thumb)、`+0x14` r0、`+0x28` r7、`+0x44` sp、`+0x48` lr(内核 abort 句柄 0xe00100bf)、`+0x4c` **pc**；`+0x174` 故障状态(0x807=写权限)、`+0x178` FAR。
- 文件名里的异常地址（`0x142ca1`）是野跳转后的 PC，真实现场在 THREAD_REG_INFO 的 pc 里。

### 崩溃链（全部实测）
1. 重定位后故障 PC = **0x81004a1a = `str.w r3, [r7, #812]`**（v01.78h b257 的 `vita_menu_run+0x134`）——**`fe_scan()` 返回后第一条局部初始化**（`tab=0`），即菜单从未画出来，崩在启动五连调用之后。
2. **r7 = 0x81000000**（= 链接基址 = henkaku 用户态系统调用号区间起点 `SYSCALLS_USER`）；FAR=0x8100032c（0x81000000+0x32c，写只读 .text）→ data abort。
3. **全 256KB 栈扫描：0x81000000 仅出现一次**——在 fe_scan 的 `push {r7,lr}` 槽（0x81680648）⇒ **r7 在 fe_scan 入口前已被砸**，且不是从栈恢复的（是直接被写进寄存器）。
4. **MODULE_INFO 显示进程内有 taiHEN 插件 oclockvita、Framecounter**：它们的 hook 桩是 16 字节 ARM `ldr r7,=syscall_id; svc 0; bx lr`——**砸 r7 且从不恢复**。
5. Vita3K 把 hooked 导入走自家 HLE thunk（完整保存 guest 上下文）⇒ **模拟器永不复现**。
6. CMAKE_BUILD_TYPE 为空 → GCC 默认 **-O0** → **每个函数都保留 r7 帧指针** → 菜单启动链（settings_load/menu_touch_init/scan_games/inbox_scan/fe_scan 全是 sceIo*/sceTouch*/sceAppMgr* 重度用户）任一被 hook 调用返回即帧指针报废 → 下一次局部写命中只读 .text。
7. 排除项：字面池扫描（830 个被引用池中无"无重定位的绝对指针"）；veneer 检查（--pic-veneer 仍在且全部 PIC 形式）；fstubs 桩正常。

### 修复
- **`CMakeLists.txt` 给 vita-port 自己的 C/C++ 对象加 `-fomit-frame-pointer`**（v01.79）。反汇编验证 b258+：`vita_menu_run` 序言不再 `add r7,sp,#N`，全部局部经 r3/sp 相对寻址；r7 只在序言/尾声保存恢复——**活动 r7 被砸已无影响**。
- 副作用评估：Thumb 回溯走 `.ARM.exidx` 仍可用；port 层无人读 r7 当 FP；菜单 UX 不变。
- **同时收编 Vita3K 退出卡死家族（v01.78b~h 全系）**：菜单不再依赖 r7 穿过任意 callee 存活。v01.78h 的 longjmp 逃生与哨兵保留（防御纵深：万一还有别的破坏者，哨兵会报 `menu sen BAD`）。

### 与 v01.67 的关系（用户提示"参考 cmakelist 记录"）
- v01.67 = 静态构造期崩（main 之前）：ld 默认 ARM→Thumb veneer 的绝对字面量无重定位条目 → 真机跳到滑动前的旧地址。修复 = `--pic-veneer`。
- v01.79 = 菜单启动期崩（main 之后）：taiHEN 桩砸 r7（-O0 帧指针）。修复 = `-fomit-frame-pointer`。
- **同族规律：真机专有 = (段滑动 | 插件注入) 与 (链接期假设 | ABI 契约) 的交集；Vita3K 两者都不模拟。** 排查顺序：① 模块基址偏移重定位现场 → ② 扫无重定位的绝对地址 → ③ 查进程内插件 → ④ 查 ABI 契约（帧指针/syscall 寄存器）。

### 交付 / 验证
- `out/vpk/midp_vita_v01.79_nofp.vpk`（v01.79 **b259**，内嵌 hash `ed8b2c8`，md5 `4108040449474047bc3c74bcee3d24e2`，3639675 B）。提交 `ed8b2c8`。
- 真机复测要点：① 启动进菜单（本次崩点）；② 菜单操作（浏览/安装/设置——都是被 hook 系统调用的重度路径）；③ 退出菜单→进游戏→退出→再进菜单多轮；④ Vita3K 回归（启动/退出/游戏全流程）。
- 若真机仍有问题：转储在 `samples/j2me/debug/logs/`（本次就是从那里找到的），按"真机转储解析方法"一节处理。

### 教训（新增）
- **真机 coredump 是最高价值证据**：寄存器 + 全栈 + 模块表一次拿全，比 Vita3K 日志强一个量级。用户侧只要把 `psp2dmp` 拷进 `debug/logs/` 即可。
- **-O0 + 帧指针 + 插件环境 = 炸弹**：任何"自己的代码"在别人 hook 的世界里跑，都不能假设 callee 保存 ABI 寄存器。
- 旧转储（9-14 的 3 个，v01.7x 时代）寄存器现场签名不同（pc 在 VM 区），属另外的崩溃，未分析——若 v01.79 后真机还崩，先比对它们的 MODULE_INFO/寄存器。


## 2026-09-17 v01.78h：**退出卡死真定案**——保存寄存器块被会话中途清零，退出改走 longjmp（治标+取证）

> 用户复测 v01.78g："退出，依然卡死"。本轮用 nm/objdump/readelf/od 取证**推翻 v01.78g 的全部归因**（PNG 行距砸堆 + 退出批量 free——那些修复本身没错，但不是本次崩溃的原因），锁定真正崩溃帧并给出确定性修复。

### 取证链（全部实测，非推测）
1. **崩溃 PC = `vita_menu_run` 尾声 `ldmia.w sp!, {r4,r5,r7,r8,r9,sl,fp,pc}`（0x8100682a @ b255）**：`nm -n` 界定函数边界 `0x810048b2..0x8100682d`，objdump 序言 `stmdb sp!,{…lr}` + `sub.w sp,#800`、尾声 `add.w r7,#776; mov sp,r7; ldmia.w sp!,{…,pc}` 全平衡。
2. **崩溃 `SP=0x80000328` = 入口 SP−32 = 保存寄存器块本身**；pop 出的 `r4..r11` 全 0、PC=0 ⇒ **保存块被零填充**。
3. **`LR=0x81010a3b` 是陈旧值**（`crumb_flush` 内 `blx sceIoClose` 的返回地址，落在函数边界之外）——**不可作归因依据**，上轮误用。
4. **栈溢出被推翻**：VELF 文件偏移 0x2d65a0 的模块参数区 `od` 读出 `main_thread_stacksize=0x18000`（96 KB），实际用 ~3 KB。
5. **crumb.log 决定性证据**：心跳健康跑到 30000+ flips（约 8 分钟），`==== menu exit ====`、`menu exit: have_sel=0 games=8` 全部写出，**然后** pop 才读到全零 ⇒ 零写在会话中途某刻发生，pop 只是唯一读点。**只改退出路径永远无效。**

### 根因状态
- **谁写的零未定**（帧内无局部数组能到保存块：`msg[120]` 在帧中下部，帧顶 528..776 全是 4 字节标量槽；所有 memset/memcpy/snprintf 均有界）。嫌疑面：另一线程或 HLE 钉住的钉子（如 `sceAppMgrGetDevInfo` 的 HLE 实现写内存）宜用哨兵下次会话夹逼。
- 崩溃签名口诀（新教训）：**崩溃寄存器全 0 + PC=0 + SP 恰为某函数帧的入口SP−(保存块大小) ⇒ 优先怀疑该函数帧被异步清零，而非调用链下游。LR 须先用 nm 验证是否落在函数边界内，陈旧 LR 不可归因。**

### 修复（确定性，不再依赖找到写者）
- **退出不走尾声 pop**：`vita_menu_run` 所有出口改 `longjmp(vita_menu_escape, 1)`；`main` 调用前 `setjmp(vita_menu_escape)`，返回后读 `.bss` 的 `vita_menu_result`。jmp_buf 在 `.bss`（0x812f6338），清零者够不到；反汇编验证：退出路径 `crumb_flush → str vita_menu_result → bl longjmp`，**无任何 `ldmia …,pc`**。
- **`sel` 改 static**（C99 7.13.2.1p3：setjmp 后经指针修改的非 volatile 自动变量 longjmp 后读取是 UB；同时彻底免疫栈清零）。
- **取证哨兵保留**：帧内 `volatile unsigned sen[8]`（0xC0DE0000+k 图案），心跳每秒校验，一旦被清零打 `menu sen BAD i=… w=… want=…`，下次会话即把清零时刻夹逼到 1 秒窗口，再结合 tab/动作定位写者。

### 交付 / 验证
- `out/vpk/midp_vita_v01.78h_longjmp.vpk`（v01.78 **b257**，内嵌 hash `5ecf5aa`，md5 `7b49312290b132c5b1ab5e1bd98e65c5`，3641291 B）。提交 `5ecf5aa`。
- 复测要点（Vita3K）：① 点"退出"标签；② 焦点在标签栏对"退出"按 X；③ 选游戏→退出→再进菜单循环；④ 若仍异常，看 crumb.log 有无 `menu sen BAD`（有 ⇒ 帧内哨兵被碰，把那 1 秒内的操作告诉开发者；无 ⇒ 写者目标不是本帧，另查）。

### 上轮结论更正
- v01.78g 节的"PNG 行距砸堆 + 退出批量 free ⇒ 退出闪退"归因**不成立**（修复保留，但与本崩溃无关）；`LR=0x81006a04 bl icon_cache_restore_kept` 的归因当时就未验证函数边界。

## 2026-09-17 v01.78g：退出闪退第一轮（归因已在 v01.78h 更正）

> ⚠️ 本节归因已被 v01.78h 取证推翻（崩溃帧实为尾声 pop，与图标释放无关），但 PNG rowbytes 校验、退出不动堆、install 路径越界修复本身正确，全部保留。

### 崩溃现场取证
- Vita3K 日志（Windows 宿主）：`PC: 0x00000000  SP: 0x80000328  LR: 0x81006a09`，`r4-r11` 全 0，`r2=0x3F(63)`；同一帧反复报 `Invalid read of uint32_t at address: 0x0, 0x4, 0x8, ... 0x1774`。
- `LR=0x81006a09` ⇒ 反汇编 `0x81006a04: bl 0x81002dd4 <icon_cache_restore_kept>` ⇒ **崩溃在菜单退出收尾的图标缓存释放序列里**（不是 main、不是 VM）。
- 非法读地址从 0x0 线性递增 ⇒ `free()` 在**已损坏的 malloc 链表**上追垃圾指针；`PC=0` 是链表里取到空函数指针/非法跳转的结果。**先有堆损坏，退出时才爆**。

### 根因（两处叠加）
1. **`vita_icon.c` 假定 libpng 输出恒为 RGBA**：`pixels = malloc(w*h*4)` 且 `rows[y] = pixels + y*w`，但 `png_set_filler()` 对**已经带 alpha 的 PNG 会被 libpng 忽略**，调色板/tRNS/16bit 组合也可能得到别的通道数。rowbytes 与假设不符时 `png_read_image()` 按 libpng 的真实行距写 ⇒ **每行越界、砸坏 malloc arena**，损坏很久之后才在 `free()` 里爆（典型的"退出才崩"）。
   - **修法**：`png_read_update_info()` 后用 `png_get_rowbytes(png, info)` 校验 `== w*4`，不符**直接拒绝**（返回 -1，图标退化为纯文本行），绝不按假设写。目前所有归一化路径（palette/tRNS/gray/filler）都应收敛到 RGBA，因此正常图标不受影响。
2. **v01.78f 的 park/restore 让退出路径去 free 64 个纹理槽**：`icon_cache_clear()` + `icon_cache_restore_kept()` 在退出时批量 free。堆已被 VM 折腾过，这里就是崩溃点（`r2=0x3F` 正是 `ICON_CACHE_MAX-1` 的循环常量）。
   - **修法（本版核心）**：**退出路径完全不动堆**——删掉 `icon_cache_keep()`/`icon_cache_restore_kept()` 与 `cache_hit_pix/w/h[]` 三个数组；释放改到 **`scan_games()` 开头**（每次扫描先把 64 槽全释放再重填），保证槽位永不留悬垂指针，且释放点与 VM/堆活动完全隔离。park/restore 本来也买不到什么：`cache.bin` 已经缓存了 PNG **字节**，重扫走内存解码，不需要"续命纹理"。

### 顺带修掉的真缺陷
- `icon_load()` 里 `if (!vita_icon_decode_png(...))` **判断反了**（该函数 0=成功、-1=失败）⇒ 真失败不打印、每次成功反而打一条 `png decode FAILED`。改为 `!= 0`。
- `install_inbox()` / `install_jar()` 的 `base[blen - 4] = '\0'` **无界下标写**：`ent.d_name`/basename 可长于 `base[128]`，`snprintf` 截断后仍按原长索引 ⇒ 栈越界。改为用**拷贝后** `strlen(base)` 重新判断并截断。

### 新增退出埋点（保险）
- 退出收尾加 `crumb_marker("menu exit")` + `crumb_printf("menu exit: have_sel=%d games=%d")` + `crumb_flush()`。下次若仍崩，`ux0:/data/J2ME00001/crumb.log` 直接说明死在"退出前"还是"退出后"，不用再猜。
- 注意 crumb 走自己的 sceIo 通道，不进 midp_stderr.log。

### 交付 / 验证
- `out/vpk/midp_vita_v01.78g_tabui.vpk`（v01.78 **b255**，内嵌 hash `aca83cc`，md5 `63893f77362e8cabde3fb29155e74cab`，3640913 B）。
  - 内嵌 hash 是**构建时的树状态**（提交前）：本版改动先提交为 `aca83cc`，随后因补录本条记忆 `--amend` 成 `8f5ea66`——VPK 里的 hash 与最终提交号不一致是 amend 的正常副作用（同 v01.78e/`1d39c08` 的情形），代码内容一致。
  - 注意 `--amend` 会改提交号：**若在乎内嵌 hash 与提交号一致，就"先提交、再构建、不 amend"**（本次教训）。
- 复测要点（Vita3K）：① 点"退出"标签；② 焦点在标签栏对"退出"按 X；③ 退出后重启游戏确认菜单可重入；④ 图标仍正常显示（若某图标变纯文本，看 midp_stderr.log 的 `[icon] png decode FAILED`，说明该 PNG 行距不是 w*4，属预期防御）。

### 工具链经验（本轮新增，务必记住）
- `midp_vita.velf` **已剥符号且无 DWARF** ⇒ `addr2line` 全 `??:0`、`nm` 为空。必须用同目录**未剥离**的 `midp_vita` 配 `arm-vita-eabi-nm -n midp_vita`；反汇编必须 `arm-vita-eabi-objdump -D -M force-thumb`（默认按 ARM 解码 Thumb 出乱码）。
- `vita_version.h` **未被 git 跟踪**，构建前必须 `rm -f vita_version.h && touch CMakeLists.txt`，否则版本号/ hash 与提交不符。
- **崩溃日志在 Windows 宿主 `C:\Users\zyb\Downloads\windows-latest\vita3k.log`，容器内无法访问（`/mnt/c` 不存在）** ⇒ 取证要么让用户粘贴日志，要么在代码里自证（`crumb.log` / `midp_stdout.log` / `midp_stderr.log`）。日志链路：`tty->print*` → stdout → midp_stdout.log；`fprintf(stderr)` → midp_stderr.log。
- 教训：**"退出瞬间崩"优先怀疑堆损坏**（越界写把 arena 砸了，触发点在很久之后的某个 free），而不是"退出路径本身写错了"。**（v01.78h 补注：本条对本菜单纯退出场景不成立——实测是尾声 pop 读到被清零的保存块；但结论对 free 序列崩溃场景仍有效。）**

## 2026-09-17 v01.78：TV 风格标签菜单——五标签 + 触摸 + 文件浏览器（未上机）

> 用户需求："菜单界面做下优化…类似 TV 样式，左侧标签栏、右侧内容栏，点击或按不同标签显示不同内容；标签：已安装（单列表/网格可切换）、未安装（自动扫描+缓存记录）、文件（文件管理）、设置、退出"。`vita_menu.c` 主循环重写（+661/-55），扫描/安装/对话框内部逻辑不动。

- **布局**：顶栏 52px（标题+版本+计数）+ 左栏 220px 五标签（中文，选中高亮+竖条）+ 右侧内容区。`TAB_N 5`，L/R 肩键或点左栏切标签；点"退出"标签直接退出。
- **已安装**：SQUARE 列表↔4×3 网格（84px 图标），选择持久化 `ux0:/data/J2ME00001/menu.cfg`（单字符 'l'/'g'）；网格方向键按行移动（上下 ±cols），tap 格子=选中+开对话框；对话框索引前先 `sel=gsel` 同步（对话框用 `games[sel]`）。
- **未安装**：`inbox_scan()` 菜单启动+每次安装后重扫（dopen 遍历很轻，"缓存"就是磁盘上的 jar 本身）；X 全装（`install_inbox`），tap 单装（`install_jar`）。
- **文件**：沙盒根 `ux0:/data/J2ME00001`，`fe_insert` 插入排序保持".."→目录→.jar→其他文件分组序；X/tap 目录=进入、jar=安装（与 inbox 同一 rename+cfg 路径）；O 回上级（".." 条目也覆盖，冗余保留）；SELECT 刷新。
- **设置**：游戏视图切换（X/tap，持久化）+ `sceAppMgrGetDevInfo` 的 ux0 容量 + games/inbox 计数 + JIT 提示 + 数据目录。
- **触摸**：`menu_touch_init()` 开采样（`sceTouchSetSamplingState START`，不开则 reportNum 恒 0）+ `sceTouchGetPanelInfo` 动态 max（真机 1920×1088 / Vita3K 960×544），`poll_tap` down→up 沿触发；归一化同 `vita_input.c`。
- **消息**：4 秒自动过期（`msg_us` + `sceKernelGetProcessTimeWide`），不再永久驻留。
- **自查修复 5 缺陷**：网格对话框光标不同步；网格滚动单位混用（gtop 行单位独立 clamp）；".." 只画不可选（改为 fe_scan 虚拟槽 0 真条目）；inbox tap 误全装改单装；DATA_ROOT 宏序 + fe_parent 前向声明。**另修两次编辑事故**：tab2 CROSS 体误删（恢复 X=打开/安装）；旧变量声明块残留（重定义编译错）；`fe_scan` 首尾双向填充紧缩循环目录/文件交错序（重写为 `fe_insert` 插入排序）。
- 交付：`out/vpk/midp_vita_v01.78_tabui.vpk`（v01.78 b247→e4b8440, md5 13a622d9b41a3a56dc409375e2b2903f）。**未上机**——触摸归一化已按 PanelInfo 动态适配，但真机手感（tap 命中区、网格格子大小）待验。
- 遗留小疵：网格名称超宽不截断（画到下一格）；对话框无 tap 操作（仅按键）；`vita_menu.c` 内有几处 `-Wformat-truncation` 警告（snprintf 截断安全，非错误）。

## 2026-09-17 v01.77：内存/输入/图形审查轮——display flip 补 vblank 同步（未上机）

> 规范审查第二轮（内存+输入+图形）。**内存全绿**：每函数 malloc/free 扫描 7 处失衡命中逐一复核，全部是契约式返回（pcsl_string 三兄弟由上游 `pcsl_string_free` 释放、`javacall_malloc` 配 `javacall_free`、`openfilelist` 配 closefilelist、`addrToString` 调用方释放、`fb_load` 错误路径全走 `fb_fail`（free+NULL）成功路径进程持有）。CDRAM 块（menu 2MB + display 4MB）分配/粒度/释放路径全合规。**输入合规**（DIGITAL 采样模式合法、触摸归一化正确、主指单点符合 MIDP）；input_debug.log 每条 open/write/close 保留不动——挂死取证还要用。

- **`flip_to_display()` 补 `sceDisplayWaitVblankStart()`**（vita_display.c）：NEXTFRAME 要到下个 vblank 才生效，此前扫描输出仍读旧前台；Java 快速连环 repaint 时下一次 blit 可能在换页落地前写完刚交换的后备帧 → 撕裂。menu 侧 `menu_flip()` 一直有成对等待，display 侧没有。顺带把刷新率规约到 60Hz（软件 blit 无所谓）。
- **`sceDisplaySetFrameBuf` 返回码记账**：失败打一次 stderr（`static int reported` 限流）——v01.69 就是靠 menu 侧这个 rc 发现 IMMEDIATE 在真机 3.65 报 0x80290006 的；失败时不交换缓冲（旧前台继续显示）。
- 注释两处（不动代码）：`lfjport_ui_finalize` 释放 CDRAM 块的至多一帧扫描输出窗口（只在 VM 关机时发生，可接受）；`ring_push` 写死 SPSC 前提（生产者=消费者=VM 线程，加第二个生产者必须改临界区）。
- 交付：`out/vpk/midp_vita_v01.77_gfxsync.vpk`（v01.77 b245, a0d153e, md5 89f8d3d6c1c187d758666afe08bbe519）。**未上机**——下次真机会话直接用这版（覆盖 v01.75/76 的验证计划不变）。

## 2026-09-17 v01.76：音频路径审查修复（未上机，与 v01.75 一并等真机验）

> 全端口 SDK 规范审查（每函数 fd 配对扫描 + 人工复核）后的 3 处修复。fd 审计结论：全部配对（持久 fd 是设计，pcsl 句柄走 closefile 收尾）。

- **`alog()` 自旋锁原子化**：旧 `while (lock); lock=1` 是 check-then-set，Vita 3 用户核下真竞态（VM 线程 + tone 线程并发调 alog）——交错行会污染挂死取证证据。改 `__sync_lock_test_and_set/__sync_lock_release`（ARMv7 ldrex/strex）。
- **删除 `src/vita_audio.c` 死文件**：从未进构建；且其 `sceAudioOutOpenPort` 第 2 参数误当"声道数"传 1（实为每调用采样数），谁加回构建就是潜伏 bug。git 留档。
- **`j2me_tone` 亲和裸 0 → 宏**：与 watchdog 统一 `SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT`。
- 低优先已记录不动：`vita_net.c` 4 处 strcpy（inet_ntoa 最长 16B + PCSL 缓冲 ≥256B，实际安全）；`vita_main.c` mkdir 返回值未查（freopen 链会连锁暴露）。
- 交付：`out/vpk/midp_vita_v01.76_review.vpk`（v01.76 b244, 33d42f2）。**未上机**——下次真机会话用这版，watchdog.log 预期不变（alive 首行 + HANG 双时钟行 + 三线程快照）。

## 2026-09-17 v01.75：看门狗规范审查版（未上机，等真机一并验）

> 对照 vitasdk 头文件 + 本项目真机已验证实践的 code review，4 处修复，正常路径无行为变化。

- **栈 0x2000 → 0x4000**：dump 链路（栈上 SceKernelThreadInfo ~200B + snprintf + sceIo*）8KB 偏紧；溢出 = 内核杀线程 = 看门狗无声消失。0x4000 对齐 `j2me_tone`（项目验证过的下限）。
- **双时钟取证**：HANG 报告同时采 `gettimeofday` 与 `sceKernelGetSystemTimeWide()`——若 H2（时钟冻结）为真，**哪一个冻住**直接点名故障层（newlib/sceRtc 链 vs 内核时间子系统）。新格式：`[WD] HANG ce= polls= clock t0/t1/d= | kclock k0/k1/kd=`（kd≈100000us=内核钟健康）。
- **亲和掩码用宏**：`SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT` 替代裸 0。
- **删死包含**：`<psp2/kernel/processmgr.h>` 无对应调用。
- 交付：`out/vpk/midp_vita_v01.75_watchdog4.vpk`（v01.75 b242, 2792319）。**未上机**——下次真机会话直接用这版，watchdog.log 预期：`[WD] alive` 首行 + 挂死后 HANG 行 + vm/tone/wd 三线程 wait 快照。

## 2026-09-16 v01.74：看门狗优先级非法——真机与 Vita3K 的又一差距

> log6 判定：`[WD] create FAILED rc=0x80028023` **每次启动必现**（6/6）= `SCE_KERNEL_ERROR_ILLEGAL_PRIORITY`。

- **根因**：看门狗优先级 0x10000300 在**真机**用户态非法；Vita3K 接受它（模拟器差距 +1：上一个是 freopen 前目录不存在）。本项目真机长期验证过的合法优先级只有 **0x10000100**（`cldc_ticker`、`j2me_tone`）。修复：降到 **0x10000150**（数值越大优先级越低，仍保持"进程内最可抢占"）。
- **v01.73 的存活行设计立竿见影**：`[WD] alive` 行缺失 + FAILED 行存在，一眼定案——"静默失败不可接受"再次被证明。
- **log5 附带收获**：挂死是**概率性**的——8 轮 round 里文件管理器、ToneTest、封神榜全部正常进出多轮，最后挂在文件管理器第 2 轮。**不是首次必现**，复现要多次进出。
- 经验沉淀：**真机线程优先级合法区间以本项目已验证值为准**（0x10000100 一档），新线程优先级不要超出已验证区间；模拟器过 ≠ 真机过。
- 待回收：v01.74 真机 watchdog.log——这次 `[WD] alive` 应出现，挂死后应有 `[WD] HANG ce= polls=12 clock t0/t1/d=` + vm/tone/wd 三线程 wait 快照。

## 2026-09-16 v01.73：看门狗 v2——免时钟检测 + 启动存活行（log5 教训）

> v01.71 (b235) 真机日志（debug/logs/log4/）判定结果：**H3 出局、音频无罪、泵彻底停转**。

- **铁证**：`ani_mark.log` 末态 `CE #115 ANI OUT`——最后会话（封神榜）泵在启动 ~4.4s 后永久停转，且不在 ANI 等待内；`audio_debug.log` `tone done: chunks=38`（400ms×16k×3=19200 帧=37.5 块，数学吻合）——音频线程完整播完无错误。
- **判读修正**：v01.71 心跳每 1024 次泵调用（≈40s）才采样一次，而挂死发生在 2~10s——**H2（时钟冻结）从未被真正排除**。
- **剩余二选一**：(a) VM 线程阻塞在某内核等待（sceIo/互斥/cond/延迟）；(b) VM 线程满转（Java/GC 死循环，永不阻塞所以永不进泵）。
- **v01.72 新增 `vita_watchdog.c`**：低优先级线程盯 `vita_ce_count`，MIDlet round 内停转 >3s 即快照 VM/tone 线程的 `sceKernelGetThreadInfo`（status+waitType+waitId——wait 类别直接点名阻塞原语）+ 双 gettimeofday 采样（d==0=时钟冻结实锤）。每次挂死只报一次，追加到 `watchdog.log`。
- **接线**：`vita_tone_tid` 由 `vita_audio_javacall.c` 导出；`vita_main.c` 在 round begin/end 置 `vita_wd_round_active`、记录 `vita_wd_vm_tid = sceKernelGetThreadId()`（主线程即 VM 解释线程）。
- **附带确认**：`vm_output.log` 链路（JVMSPI_PrintRaw→crumb_append）无缓冲崩溃安全，"最后一行=挂死点"前提成立；封神榜挂点在 `[pcsl] open NEZHA_DOWN.db` 入口行之后。

## 2026-09-16 v01.71：ToneTest 挂死诊断版——三判定点探针 + 音频日志毁证据修复

> 用户回报：**"运行 ToneTest 的时候，会发出一下声响后，就没反应了"**。vm_output.log 止于 `stg1: playTone returned`，之后 2.5s stage 定时、1Hz 心跳、60s 看门狗全灭；但另一会话挂死时仍有 `[INPUT] buttons=` 记录（泵可能还活着）。

- **已排除**（静态取证结论）：playTone 本身（异步，bump `g_req_seq` 即返）；EOM 通知死锁（简单 playTone 路径不发 END_OF_MEDIA）；AudioBridge/vita_audio.c 路径（ROM 未用）；native 链接断链（ELF 符号全闭合）。
- **三互斥假设**：H1=VM 主线程卡在 ANI 等待**之前**的阻塞 native（repaint→`sceDisplaySetFrameBuf` 等）；H2=墙上时钟（`gettimeofday`=`Os::java_time_millis` 源）冻结→所有定时等待者永不超时；H3=卡死在 `ANI_WaitForThreadUnblocking`（pthread_cond_timedwait）内。
- **发现的独立 bug**：`vita_audio_javacall.c` 的 `alog()` 用 `SCE_O_TRUNC` 打开 audio_debug.log——每次进程启动截断，**上一会话（ToneTest）的音频记录被下一会话（UC）启动毁掉**。已改 `SCE_O_APPEND`。
- **探针布局**（全在 `ux0:/data/J2ME00001/`，判读表见 CMakeLists v01.71 注释）：
  1. `input_debug.log` 心跳行升级为 `#N ms=... d=... CE=n ani=e/x`——一行同时证明**泵活性**（N/CE 推进）与**时钟活性**（ms 推进）；ms 冻结+d 不变=H2。
  2. `ani_mark.log` 是 watchdog 槽：ANI 进/出各 `sceIoPwrite` 覆写偏移 0——心跳停 + 末态 `OUT`=H1（卡泵前段）；末态 `IN`=H3。
  3. `audio_debug.log`（追加式）+ tone 线程 `tone req`/`tone done: chunks=` 锚点 + `sceAudioOutOutput` 负返回码记录。
- **改动文件**：`vita-port/src/{vita_audio_javacall.c, vita_input.c, vita_checkevents.c}` + CMakeLists 版本块。
- **遗留疑点（未修，待判定后处理）**：`vita_pcsl.c:2169` `JVM_monotonicTimeMillis` 返回 0 stub——无法解释前 2.5s 时间正常，存疑。

## 2026-09-15 v01.70：真机闪烁定案——单缓冲直写扫描输出帧，双缓冲修复

> 用户回报 v01.69 **"画面显示了，但是一直在闪烁，感觉像是一直在刷新，之前 Vita3K 偶尔会这样"**。

- **根因**：菜单（每帧全屏重绘）和 LCDUI blit 都是**单缓冲**——直接画在显示控制器正在扫描输出的那块帧上，面板读到半成品帧 = 持续闪烁/撕裂。Vita3K 偶尔出现的同现象是同一个竞态（只是很少输）；v01.68 改 CDRAM 后写入变慢（非缓存），真机上竞态从"偶尔"变成"必然"。
- **修复**（提交 d4fe4c8）：两处 framebuffer 各自改为**单 CDRAM 块双帧 ping-pong**——画后备帧 → NEXTFRAME 提交 → 交换指针：
  - `vita_menu.c`：`menu_fb` 别名后备帧，`menu_flip()` 提交+交换；
  - `vita_display.c`：`flip_to_display()` blit 进后备帧、提交、交换；`lfjport_ui_finalize` 释放整块。
  - CDRAM 占用：菜单 4MB + 显示 4MB（256KB 粒度）。
- **产物**：`midp_vita_v01.70.vpk`（b232 (d4fe4c8)，MD5 `086d50d8912751d542426faaa11e7d5f`）。
- **真机五连坑（累计）**：PIC 跳板 / 非缓存 fb / NEXTFRAME / 双缓冲 / （诊断）crumb 心跳+防 FTP 缓存。下一个真机风险点不变：**音频 ring buffer 的缓存一致性**（"有画面没声音"就查它）。

## 2026-09-15 v01.69：真机黑屏真根因定案——`SetFrameBuf(IMMEDIATE)` 被真机拒绝（0x80290006）

> v01.68（CDRAM）后真机仍黑屏。**注意那两轮"还是黑屏"的 log 其实是 FTP 传输缓存了旧文件**（boot_log 显示 v01.67 b226、且无 crumb.log）——真机侧早已在跑 v01.68 b229。教训：**核对版本串，别信文件内容；取日志用改名法（复制成新文件名再拉）绕开 FTP 缓存**。

- **crumb.log 心跳数据（b229 诊断版）一步定性**：
  - `menu: fb=0x61000000 block=0x4001015f` → CDRAM 分配成功（0x61000000 正是 CDRAM 映射基址）；
  - `menu hb: flips=60/120/179...` 每秒 +60 → 菜单循环完全健康；
  - **`rc=0x80290006` 出现在每一次 flip** → `SCE_DISPLAY_ERROR_INVALID_UPDATETIMING`（psp2/display.h:24）。
- **根因**：两个 flip 点（`vita_menu.c menu_flip`、`vita_display.c flip_to_display`）都用 `SCE_DISPLAY_SETBUF_IMMEDIATE`。真机 fw 3.65 拒绝该更新时机、面板从未拿到 framebuffer。**官方三个样例（debugScreen/camera/ime）全部用 `SCE_DISPLAY_SETBUF_NEXTFRAME`**；Vita3K 不校验 IMMEDIATE——模拟器正常的第三个理由。
- **修复**（提交 a79ff3b）：两处统一改 `SCE_DISPLAY_SETBUF_NEXTFRAME`。菜单循环里每次 flip 后已有 `sceDisplayWaitVblankStart()`，NEXTFRAME 语义匹配。
- **产物**：`midp_vita_v01.69.vpk`（v01.69 b230 (a79ff3b)，MD5 `601a7ece827246df5078b0a7fef81a32`）。
- **真机四连坑（最终版）**：
  1. 段滑动 → 跳板必须 PIC（v01.67 `--pic-veneer`）；
  2. CPU 写/硬件读的缓冲必须非缓存（v01.68 CDRAM framebuffer）——**音频 ring buffer 是下一个待查点**；
  3. `SetFrameBuf` 更新时机必须 NEXTFRAME（v01.69）；
  4. **诊断日志是唯一可信源**：crumb 心跳 + 返回值记录一步定性；同时警惕 FTP 缓存伪造"修复无效"。

## 2026-09-15 v01.68：真机黑屏定案——framebuffer 用了缓存内存，显示控制器看不见（CDRAM 修复）

> 用户回报 v01.67 真机 **"启动后黑屏"**；boot_log.txt 出现并停在 `[media] tone player thread created`——pic-veneer 修复已生效（静态构造活下来了、日志能写了），崩溃变成了显示问题。

- **日志解读**：`[media]` 之后 main() 的下一批 dlog 只会在**菜单做出选择之后**才写（菜单自身不写 boot_log），所以日志停在这里 ≠ 卡死，恰好说明执行流进入了 `vita_menu_run()`。黑屏 = 菜单在跑但画面上不去。
- **根因**：`vita_menu.c` 的 `menu_fb` 和 `vita_display.c` 的 `vita_fb` 都是 `memalign()` 的**普通缓存堆内存**。真机上 Cortex-A9 的写回式 D-cache 把 CPU 的画面写留在缓存里，显示控制器扫描的是主存 → 面板永远黑。**Vita3K 不模拟缓存**（与 veneer 那次同类"模拟器专用正常"），所以模拟器一直 looks fine。全 port 层 grep 不到任何 Dcache 回写调用佐证。
- **修复**（提交 fc0008e）：新 helper `src/vita_fbmem.h`——`sceKernelAllocMemBlock(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW)`（256KB 粒度向上取整）分配 framebuffer，CPU 侧**非缓存**映射，对显示控制器即时可见；这正是官方样例（common/debugScreen.c、camera、ime）的做法。两处替换：
  - `vita_menu.c`：menu_fb 走 `menu_fb_blk`，块随进程存活（与旧所有权一致）。
  - `vita_display.c`：vita_fb 走 `vita_fb_blk`，`lfjport_ui_finalize` 里释放。
  - CMakeLists 链接 `SceSysmem_stub`（MemBlock API 属 SceSysmem 组）。
- **产物**：`vita-port/build/cmake/midp_vita.vpk`，v01.68 b227 (fc0008e)，3,634,904 B。
- **教训（真机三连坑，务必记住）**：
  1. 段滑动 → 跳板必须 PIC（v01.67 `--pic-veneer`）；
  2. 显示路径内存必须非缓存（CDRAM 或回写）；任何"CPU 写、其它硬件读"的缓冲（音频 ring、未来 GPU 纹理上传）同样适用——**下一步真机没声音的话先查 `vita_audio.c` 的 ring buffer 是不是同一类问题**；
  3. Vita3K 不模拟缓存也不滑动段——"模拟器正常"不能作为任何真机结论的依据。

## 2026-09-14 v01.67：真机启动即崩定案——ld 默认 ARM→Thumb 跳板无重定位（`--pic-veneer` 修复）

> 用户回报 **"启动即崩,菜单都没显示出来，vm_output,vm_stderr,net_log 还没创建"**。

- **症状**：真机（fw 3.65 + oclockvita/Framecounter 插件）一启动就崩，菜单不出现；`ux0:/data/J2ME00001/` 下**任何**日志文件（boot_log.txt / crumb.log / net_log.txt / vm_output.log / vm_stderr.log）都不存在。Vita3K 完全正常。两份 psp2dmp（v01.65 的 1789400876、v01.66 的 1789403835）现场完全相同。
- **根因**（证据链全部闭环）：
  1. THREAD_INFO 里崩溃线程 entry = 设备端 `_start+1`，对比本地 ELF 的 0x81001269 → **真机代码段被滑动 +0x72000**（插件注入量决定，两台机/两次启动可不同：v01.65 那份滑 +0x3d000）；RW 段独立滑动 **+0x120000**（本地 0x812e0018→设备 0x81400018、0x814a8170→0x815c8170，与转储 r2/r3 逐字吻合）。**Vita3K 恒按 0x81000000 装载，这就是模拟器永不复现的原因。**
  2. 加载器确实应用了 ELF 重定位（否则 movw/movt 装的地址不可能全是正确滑移值），前 3 个 `.init_array` 构造函数都跑完了（栈上残留 `__sinit`/`_malloc_r`/`pte_osTlsAlloc` 帧）。
  3. 但 ld 默认生成的**长距离 ARM→Thumb interworking 跳板**（`____X_from_arm`：`ldr pc,[pc,#-4]; .word 绝对地址`）的**字面量没有重定位记录**——readelf 验证 v01.66 ELF：38 个跳板、0 条重定位。
  4. 第 4 个构造函数（`_GLOBAL__sub_I__ZN8JVMFrame12_in_gc_stateE`，Frame.cpp 的静态对象析构注册）尾部 `b ____aeabi_atexit_from_arm` → 跳到**过期的链接期绝对地址** → 野代码 → data abort。dmp1 的 PC 恰好 = `__aeabi_atexit` 的链接期地址 0x810be194（铁证）；dmp2 落到 `str r2,[r7,#4]`、r7=0（写 0x4）。**崩在 main() 之前**，所以没有任何日志文件。
- **修复**：`CMakeLists.txt` 链接选项加 `-Wl,--pic-veneer`（提交 6b49df3）。38 个跳板全部变为 pc 相对序列（`ldr ip,[pc,#4]; add ip,pc,ip; bx ip`），随段滑动自适应；R_ARM 重定位总数不变（99653→99653），模拟器行为不受影响。
- **教训**：
  - 真机 coredump 符号化**必须先从 THREAD_INFO 的 entry 算出段滑动量再换算**，直接拿本地符号表解设备地址全盘皆错（这次先错解成 png/read_file/net 路径，浪费了一轮分析）。
  - "日志文件一个都没有" 本身就是最强证据：说明崩在 `main()` 之前，只有静态构造/加载器层面的问题才会这样。
  - 上游 CLDC 用 `-marm` 编 ARM 态代码、port 层是 Thumb，这种混合态在**段会被滑动的平台**上必须 `--pic-veneer`。

## 2026-09-14 v01.66：汉字的行盒适配（CJK 18px → 20px）

> 用户回报 **"文件根目录修改成功了，但是字体，英文显示还行，中文显示效果反而不太好了"**。
> v01.65 换字体库后拉丁变好、汉字反而更糟。**根因是几何而非字形**：只改 `tools/fontgen.c` 的一个常量，
> 重新生成银行并重打 VPK；**未重编 MIDP ROM**（字体库是打包资源，不进 ROM）。

### 一、根因：汉字坐在基线上填不满 22 行行盒

v01.65 给 CJK 用 `CJK_PX 18`，而行盒是 `CH 22` / `ASCENT 18`；汉字墨迹约占 em 的 0.85：

| | `CJK_PX 18`（v01.65） | `CJK_PX 20`（本版） |
|---|---|---|
| `中` 的墨迹行 | **5..19**（15 行） | **3..20**（18 行） |
| 墨迹上方空格 | **5 行** | 3 行 |
| 墨迹列 | 2..14 | 3..16 |
| section advance | **18 px** | **20 px**（= cell 宽） |

- 汉字**墨迹本身几乎没变**（13×15 → 13×16），变的是**位置与间距**：18px 的墨迹坐在基线上只够 15 行，
  上方空出 5 行、下方 1 行——中文看起来**又小又沉**；笔尖只前进 18px，比 20px 字身还紧 2px，密排时发挤。
- **拉丁不受影响**：它的墨迹本来就贴着基线（`A` rows 5..17），所以用户看到的是"英文还行，中文变糟"。
- **读旧库的坑（勿拿它当基线）**：v1 银行头部 offset 24 是"位图基准"字段，装机的 v01.64 加载器却把基准算成
  `off + nsec*8`，而旧银行真正基址就是 `off`——**旧库每个字形都错位了一个字形槽**（72 字节）。
  旧版"中文观感"有一部分来自这个错位。

### 二、修复与验证

- `tools/fontgen.c`：`#define CJK_PX 18` → `20`（注释写清两个字体为何用不同 em）。
  `LATIN_PX 20`、`CW 20`、`CH 22`、`ASCENT 18`、`NSEC 11` 与 v2 布局**全部不动**。
- **不裁字（逐字形扫描量化）**：`bash tools/gen_font.sh` 后扫全部 CJK 各区：`U+4E00..U+9FA5` 墨迹下缘
  **最大 row 20**（15,058 字形）、上缘最小 row 2；`U+FF01..U+FF5F` 下缘最大 row 21——都在 cell 的 0..21 内。
  `WARNING clipped: 0 above, 19 below, 53 sideways` 在 **18/19/20 三个字号下数量相同**（50/54/53），
  是拉丁 `xo = -1` 的正常左伸与既有的 19 个下伸字形，**与本次改动无关**。
- **拉丁零回归（分区字节比对，新库 vs v01.65 库）**：

  | 区域 | 差异字节 |
  |---|---|
  | header（0..32） | 0 |
  | 拉丁 advance 表 | 0 |
  | CJK advance 表 | 21,121（17/18 → 19/20） |
  | 拉丁位图 | **0** |
  | CJK 位图 | 796,659 |

  英文部分**逐字节相同**——正是用户说"还行"的那一半。
- 银行 **1,615,280 B 不变**（布局未动、只有内容变）：MD5 `af032dab…` → `1142aeab2860bb7c99b15f9db0c10aa9`。
- VPK：`cd vita-port && cmake --build build/cmake -j4`（**不需重编 MIDP ROM**）→ **v01.66 b222**、3,634,400 B。
  比 v01.65 大 ~100 KB 属正常：20px 汉字笔画更密、熵更高，压缩率下降。VPK 内
  `data/J2ME00001/fontbitmap.bin` 的 MD5 == `config/fontbitmap.bin` ✓。
- `strings build/cmake/midp_vita | grep "version: J2ME"` → `version: J2ME Player v01.66 b222 (60959ac)`。
- **`vita_version.h` 是 configure 时生成的**（`GenVersion.cmake`）：`VITA_PORT_BUILD` = 提交数、
  `VITA_PORT_HASH` = 当前 HEAD。所以**先提交、再构建**，hash 才指向本版代码提交；
  构建前的 HEAD 会滞后一拍（本版第一次构建得到的是 `b221 (5cc5a9c)`，touch `CMakeLists.txt`
  重新 configure 后才是 `b222 (60959ac)`）。

### 三、遗留风险 / 待验收

- **Vita3K 未上机复测**：汉字应填满行盒、笔画更实、字距回到 20px；英文应与 v01.65 完全一致。
- **`0xFF61..0xFF9F`（半角片假名）整区无字形**（等线字体没有；`sec 8` 无墨迹、advance 全 0）→ 渲染成 tofu。
  `0x2460..0x2487` 只有 10/40、`0x3000..0x303F` 35/64——**既有覆盖缺口**（字体 + section 表两方面），本版未处理。
- **CJK 仍是 1bpp**（无灰度）：20px 后笔画还原更好，但曲线边缘仍有锯齿。改 8bpp 会把银行从 1.6 MB 推到约 10 MB，**不建议**。
- `j2me/debug/*.psp2dmp`（崩溃转储）留在工作区，**未提交**。

## 2026-09-14 v01.65：JSR 75 根目录 = 设备存储卷（多 root）+ UI 字体换 J2FB v2

> 两条用户需求一次做完：(1) **"希望文件管理根目录是 psvita 的根目录，而不是当前应用的 data 目录"**；
> (2) **"字体能否换一个，现在的字体显示英文字母很奇怪，有些符号排版也很难看"**。
> 改动全部在 `j2me/jsr75`（Java + native）与 `j2me/vita-port`（字体链路 + 打包），**未碰 `phoneme_source/`、未碰 `cldc/build/share/`**。

### 一、多 root 模型（旧"单 root = data"彻底退役）

| 位置 | 改动 |
|---|---|
| `FileSystemRegistryImpl.java` | `ROOT_DEVS = {ux0,imc0,uma0,app0}` + `ROOT_ALWAYS = {T,F,F,T}`；`availableRoots()`（惰性 + `synchronized`）；`findRoot()`；`resolve(String) → String[3]`；`resolvePath()` 变成 `resolve(url)[0]` 的一行包装；新增 `trimSlash()` |
| `FileConnectionImpl.java` | 新增 `private final String rootName`，构造器改为 `String[] parts = resolve(url)` 一次拿三件；`getPath() = "/" + rootName + (relPath 空 ? "/" : "/" + relPath)`；`totalSize()/availableSize()` 改传 `absPath` |
| `FileStore.java` | `availableSize(String)`/`totalSize(String)` 改签名；`getRootPath()` javadoc 改为"启动器数据目录，**不是** JSR 75 root" |
| `jsr75_file.c` | 新增 `jsr75_is_dev_root()`；`isDirectory` 判定加一项；`availableSize/totalSize` 改为按参数路径取设备前缀；顶部"Root policy"重写（原有一段**重复粘贴**的段落，一并清掉） |
| `FileSystemRegistry.java` / `Protocol.java` | 仅 javadoc：root 是"每个已挂载存储卷的根"，示例 URL 改 `file:///ux0/data/x` |

- **root 名就是设备名**（`"ux0/"`、`"imc0/"`、`"uma0/"`、`"app0/"`），拿到就能直接拼 `file://localhost/" + root`，和 `C:/`、`E:/` 的既有平台习惯一致。
- `file:///ux0/data`（与 `file://localhost/ux0/data`）→ `resolve()` 返回 `{"ux0:/data", "ux0", "data"}`；`{"ux0:", "ux0", ""}` 表示卷根。
- **数据目录不再是 root，但它照常可达**：`ux0/data/J2ME00001`。这正是需求 1 的要点——文件管理器现在能浏览整机，而不是被锁进移植自己放 jar/config 的那个目录。
- `availableRoots()` **必须惰性**，不能写成静态初始化块：探测是 native 调用，而 ROMizer 在**构建期**就链接这些类，类初始化不能依赖 native 代码。
- 旧常量 `ROOT_NAME` / `ROOT_LIST_NAME` / `ROOT_URL` 全部删除（`grep` 已确认无残留引用；`FileConnectionImpl.class` 对 `getRootPath` 的引用数为 **0**，说明旧耦合真的断了）。

### 二、native 两处关键点

- **`jsr75_is_dev_root(path)`**：`sceIoGetstat()` 对**卷根**不保证置 `IFDIR`（和 `jsr75_is_root_path()` 早就在绕的是同一个固件怪癖），所以改为**直接开卷根**（`sceIoDopen("ux0:/")` 成功即存在且已挂载）。这同时回答了"卡插没插"，Java 层拿它决定 `imc0`/`uma0` 要不要列进 `listRoots()`。
- **`availableSize`/`totalSize` 带路径**：`jsr75_dev_of(cpath)` 取设备前缀 → `sceAppMgrGetDevInfo(dev)`，于是每张卡报自己的容量，而不是永远报 data 目录所在卷的。
- 卷根的绝对路径是 **`ux0:`（无尾斜杠）**，因为 `resolve()` 走 `trimSlash()`；这**安全**：`listOpen()` 本来就会把目录路径补成 `"dir/"` 再 `sceIoDopen`（`ux0:/`），而 `parentPath()` 的 `slash <= 0 → null` 分支正好吃掉无斜杠的情况（不会 `substring` 越界）。
- `pending`：`jsr75_is_root_path()` 在多 root 模型下已成死代码，按最小 diff 原则**保留未删**。

### 三、字体：J2FB v2（比例拉丁 + 抗锯齿 + 基线对齐）

- **格式**：32 字节头（`"J2FB"`、version=2、`nsec`@8、`dataOff`@12、ascent/descent/leading@16/18/20、`nglyphs`@24）+ `nsec`×28 字节 section 记录（first/count/gw/gh/stride/bpp/advOff）+ advance 表（1 字节/字形，0 = 无字形）+ 4 字节对齐位图。stride：8bpp → `gw`，1bpp → `(gw+7)/8`。加载器仍兼容 v1。
- **11 个 section / 21,653 码点**：Latin（`0x0020×95`、`0x00A0×96`、`0x0100×128`、`0x0386×56`、`0x2010×24`、`0x2460×40`）= **DejaVu Sans 20px 8bpp 比例宽度**；CJK（`0x3000×64`、`0xFF01×95`、`0xFF61×63`、`0x4E00×20902`、`0x9FA6×90`）= **等线 18px 1bpp**。字身 `CW20×CH22`，基线 `ASCENT 18`，descent 4。银行 **1,615,280 B**。
- **坑 1：stb `yo` 是屏幕坐标（向下为正）**，必须 `cy = ASCENT + yo`。第一版按 `cy = yo` 摆，所有字形整体下移 24 行——回归时先看这个。
- **坑 2：CJK 墨迹宽度会超过字面 advance**（全角框、`0xFF01` 组），section advance 归一到 `max(墨迹上限, adv 上限)`，否则字与字粘一起。
- **坑 3：编译 `fontgen` 必须 `-B/usr/bin`**（`gen_font.sh` 已固化）——PATH 前面是 VitaSDK 的 ARM `as`/`ld`，会拒绝 x86 目标文件。
- **打包瘦身**：`config/font.ttf`（16 MB 等线）**不再进 VPK**，只发 `fontbitmap.bin`。**VPK 13.7 MB → 3.6 MB**。`config/font-latin.ttf` = DejaVu Sans 副本 + `font-latin.LICENSE.txt`（保留来源与许可），`.gitignore` 对这两个新增文件做了白名单。

### 四、构建与验证（本轮实际命令与结果）

- MIDP：`bash phoneme-midp/build_vita.sh` → `EXIT=0`。尾部 `jsr75_file.c: undefined reference to 'sceAppMgrGetDevInfo'` 是**已知无害项**：只出现在 `libmidp.so` 的检查链接里（那条路径不含 `SceAppMgr_stub`），最终可执行文件链接 `libobj.a` 时符号齐全；而且这个调用**旧版 `availableSize` 就在用**，不是本次引入。
- 级联确认（时间戳）：`FileSystemRegistryImpl.java` 11:13:13 → `.class` 11:14:19 → `classes.zip` 11:14:19 → `obj/arm/jsr75_file.o` + `obj/arm/ROMImage.o` → `libobj.a` 11:14:41。
- **反汇编核验**（`arm-vita-eabi-objdump -dr .../jsr75_file.o`）：`FileStore_isDirectory` 内联了 `sceIoDopen`/`sceIoDclose`/`sceIoGetstat`——新的卷根探测确实编进去了。
- **类/ROM 级核验**：`FileSystemRegistryImpl.class` 常量池含 `ux0`/`imc0`/`uma0`/`app0`/`unknown root: `/`localhost`；`FileConnectionImpl.class` 对 `getRootPath` **0** 引用；ELF 中 `imc0`/`uma0`/`unknown root: `/`localhost` 以 UTF-16LE ROM 字符串存在（1 个）；`Java_com_sun_midp_jsr075*` native 符号 **21** 个（未变）。
- 产物：`vita-port/build/cmake/midp_vita.vpk` = **v01.65 b220 (c568dcc)**，3,534,892 B；VPK 内 `data/J2ME00001/fontbitmap.bin` 与 `config/fontbitmap.bin` **MD5 相同**（`af032dab2d62574dccc4e10d2f12acd8`），`font.ttf` 已不在包内；`version: J2ME Player v01.65 b220 (c568dcc)`。
- 提交：`samples` 主仓 **c568dcc**（`phoneme-cldc`/`phoneme-midp` 本轮无改动）。
- **提醒：版本号在 CMakeLists 里改完必须重打 VPK**，否则 `strings` 还是旧 build 号（v01.48 已踩过一次）。

### 五、未实测 / 遗留风险

- **Vita3K 未上机复测**（本轮用户未装包）：预期 `listRoots()` 给出 `ux0/`（插卡时另有 `imc0/`/`uma0/`）与 `app0/`，`file:///ux0/data/J2ME00001` 可进，`availableSize()`/`totalSize()` 随路径变卡。
- **字体视觉未验收**：拉丁应为比例宽度 + 灰度抗锯齿，下伸部/标点落在基线上。`vita_menu.c` 里写死的 x 坐标（最大 908）在拉丁变窄后只会更宽松，不会溢出。
- `FileConnectionImpl.open()` 对 `relPath == ""` 仍然拒绝（"Cannot open the root"）——**卷根同理**，若某 MIDlet 直接 `open()` 根目录会抛异常（与旧单 root 时代行为一致，未改）。
- `getName()` 对卷根返回 `"ux0:"`（`lastElement()` 找不到 `/`），不是 `""`；设备名 root 的固有小事，无害。
- `FileStore.exists()` 仍走 `pcsl_file_exist()`，其 `app0:` 兜底（`vita_pcsl.c:1408`）对不存在的路径可能报 true（v01.60 起已知）。
- `usedSize()`/`directorySize()` 现在可能对整个卷递归（`MAX_DEPTH 12` 限深不限宽），在 `ux0:/` 上调会很慢——文件管理器真去算根目录体积时注意。
- `jsr75_is_root_path()` 已成死代码（保留未删）。

## 2026-09-14 v01.63：MiniXplorer 目录打不开定案——`resolvePath()` 不认 `file://localhost/...`

> 实测（Vita3K，v01.62 + `[ALERT]`/`[MIDLET]`/`[JSR75]` 诊断）后新的、明确的失败点。
> 症状：MiniXplorer 能正常启动、UI 出现，但一进目录就失败。

### 一、日志给出的完整证据链

```
[MIDLET] startApp: MiniXplorer
[JSR75] listRoots -> data/                     <- listRoots 修好了（v01.61 的修复生效）
[MIDLET] startApp returned: MiniXplorer
[JSR75] open file://localhost/data/ mode=3     <- MiniXplorer 按规范拼 URL
Uncaught exception: java.lang.IllegalArgumentException: not a file URL: file://localhost/data/
  com.sun.midp.jsr075.FileSystemRegistryImpl.resolvePath(), bci=38
  com.sun.midp.jsr075.FileConnectionImpl.<init>(), bci=76
  com.sun.midp.io.j2me.file.Protocol.openPrim(), bci=130
  javax.microedition.io.Connector.open() x3
  MiniXplorer.OpenDir(), bci=73
  MiniXplorer.traverseDirectory(), bci=116
```

之后是 `file://localhost/data/data/`、`/data/data/data/`……**无限级联**——那是 MiniXplorer
自己的错误处理把 root 名反复追加，不是我们的 bug。

### 二、根因：URL 语法只认一种写法

- JSR 75 语法是 `file://<host>/<root>/<path>`，本机 host = `localhost`；空格权威
  （`file:///data/`）是同一件事的公认简写。两种写法都在野外存在。
- 原 `resolvePath()` 硬编码前缀 `"file:///"`，于是**规范的 `file://localhost/...` 一律被拒**。
  MiniXplorer 用的正是 `"file://localhost/" + listRoots()` 这条文档推荐写法。
- 讽刺的是 v01.61 刚把 `listRoots()` 改成返回 root **名**（`data/`），正是为了让调用方能拼出
  `file://localhost/data/` —— 结果 `resolvePath()` 不认这个形式，两处假设没对上。

### 三、修复（`jsr75/src/share/core/common/classes/com/sun/midp/jsr075/FileSystemRegistryImpl.java`）

- `resolvePath()` 改为解析权威部分：`scheme = "file://"` → 取到下一个 `/` 之前作为 host，
  **接受 `""` 与 `localhost`（大小写不敏感）**，其它 host 仍报 `unknown host`（本移植只暴露一个
  root，不能假装认识别的设备）。`file:///data/x` 与 `file://localhost/data/x` 现在归一到同一路径。
- 顺带更新类头注释与 javadoc（原文写"Accepted form: file:///data"）。
- 其余路径全部未动：`getPath()` 仍返回 `/data[/...]`（规范要求含 root），`getURL()` 原样返回
  打开时的 URL，`FileConnectionImpl.listEntries()` 已保证目录带尾 `/`（对应原生
  `jsr75_file.c:listNext` 的 `SCE_S_ISDIR` 分支），`Connector` → `Protocol.openPrim` 未改。

### 四、副产品：那块 "Done" 屏的最终解释

- `[ALERT] new Alert title=null text=This application does not use the screen and runs in the
  background.` 是 phoneME **自己的** `HeadlessAlert`（`ResourceConstants.LCDUI_DISPLAY_HEADLESS`）。
- `CldcForegroundController.registerDisplay()` 把 `new HeadlessAlert(...)` 作为
  `getCurrent()==null` 的**占位显示对象预先构造**——所以 MIDlet 一调 `Display.getDisplay()`
  就会看到这条 `[ALERT]` 日志，**与是否真的弹屏无关**。
- 真正弹屏的条件在 `CldcMIDletStateListener.midletActivated()`：`startApp` 返回后
  `Display.getDisplay(midlet).getCurrent() == null` 才 `requestForegroundForDisplay()`。
- 结论：之前的 "Done" 屏是 MIDlet **没留下任何可显示对象**的症状（当时 `startApp` 后半段
  已经因为 JSR 75 抛异常而中断），不是病因。本次运行 MIDlet 正常留下 UI，故不再出现。

### 五、产物与验证

- `vita-port/build/cmake/midp_vita.vpk` = **v01.63**，13,692,084 B（10:46:50），
  `strings midp_vita | grep "version: J2ME"` = `J2ME Player v01.63 b218 (cf48224)`，
  `Java_com_sun_midp_jsr075*` 原生符号 21 个。
- `classes.zip` 10:45:46、`ROMImage_01.cpp` 10:46:03、`obj/arm/ROMImage.o` 10:46:06（同代）。
  **注意：本机没有 `unzip`**，验证类内容用 `python3 -c "import zipfile..."`；
  已确认 `FileSystemRegistryImpl.class` 常量池含 `file://` / `localhost` /
  `equalsIgnoreCase` / `unknown host: `。
- romgen 通过（无 `illegal immediate` / `Error 133`）。

### 遗留

- 未实测：MiniXplorer 打开目录后的 `list()` 渲染、以及 `availableSize/totalSize`
  （`libmidp.so` 链接仍报 `sceAppMgrGetDevInfo` 未定义——该符号只在旧 `libmidp.so` 路径上，
  VPK 走 `libobj.a`，不影响；但 JSR 75 的 `availableSize()` 在真机/模拟器上可能返回失败）。
- 诊断打印仍在（`[JSR75]` / `[MIDLET]` / `[ALERT]`），稳定后删除。

### 六、实测结果：**通过** ✅

用户装 v01.63 在 Vita3K 实测——**MiniXplorer 正常启动，目录可以打开了**。JSR 75
（`javax.microedition.io.file`）在 PS Vita 上从零到可用，至此完成。

## 2026-09-14 v01.64：JSR 75 收尾——诊断打印全部撤除（最小 diff 还原）

> 承接 v01.63 的实测通过。功能已稳定，按最小 diff 原则把为定位而加的临时诊断全部删除。

### 一、撤除内容（`git checkout --` 整体回退，均为纯诊断文件）

| 文件 | 撤除内容 |
|---|---|
| `jsr75/.../com/sun/midp/io/j2me/file/Protocol.java` | `[JSR75] open file:... mode=` |
| `jsr75/.../com/sun/midp/jsr075/FileConnectionImpl.java` | `[JSR75] connect ...`、`listEntries` 的 3 处 |
| `phoneme-midp/.../MIDletStateHandler.java` | `[MIDLET]` 的 startApp / startApp returned / THREW / destroyApp / destroyed / state loop |
| `phoneme-midp/.../Alert.java` | `[ALERT]` 的构造 + setString |

- **重要澄清**：`MIDletStateHandler` 里 `[MIDLET] creating` / `created+registered OK` /
  `FAILED(CNFE|IE|IAE|RE|Error)` / `startSuite` 这 8 处**不是临时诊断**，它们来自
  **2026-08-31 的移植基线提交 `8a04358`**，是项目固有启动日志 → **保留**。
  （用 `git log -S` 定位时注意：双引号里的 `\[` 会被当成字面反斜杠，要用
  `git log -S'[MIDLET] creating'` 单引号写法，否则会搜不到而误判为"未提交"。）
- `FileSystemRegistryImpl.java` 只删掉 `[JSR75] listRoots ->` 一行，**真实修复全部保留**。

### 二、最终保留的真实改动（JSR 75 全部改动清单）

| 文件 | 改动 | 版本 |
|---|---|---|
| `jsr75/.../FileSystemRegistryImpl.java` | `listRoots()` 返回 root **名**（`data/`）；`resolvePath()` 解析权威部分，接受 `""`/`localhost` | v01.61 / v01.63 |
| `jsr75/.../javax/microedition/io/file/FileSystemRegistry.java` | javadoc：root 是**名**不是 URL | v01.61 |
| `vita-port/config/system.config` | `microedition.io.file.FileConnection.version: 1.0` | v01.62 |
| `phoneme-cldc/src/vm/cpu/arm/BinaryAssembler_arm.cpp` | 字面量池绑定复用窗口 off-by-8 修复 | v01.62 |
| `rebuild_vm.sh` | `set -e` + `grep -c` 假失败修复（`\|\| true`） | v01.62 |

（另有 v01.60 起新增的整棵 `jsr75/` 树与 native 层 `src/share/native/jsr75_file.c`。）

### 三、产物与验证

- `vita-port/build/cmake/midp_vita.vpk` = **v01.64**，13,690,011 B（10:52:39），
  `version: J2ME Player v01.64 b218 (cf48224)`，jsr75 原生符号 21 个。
- `classes.zip` 10:52:00 → `ROMImage_01.cpp` 10:52:18 → `obj/arm/ROMImage.o` 10:52:21（同代），
  romgen 无 `illegal immediate` / `Error 133`。
- 用 `python3` zipfile 逐类核验常量池：
  - `FileSystemRegistryImpl.class` → 含 `localhost`、`unknown host`，**不含 `[JSR75]`** ✅
  - `FileConnectionImpl.class`、`Protocol.class` → **不含 `[JSR75]`** ✅

### 四、遗留（下一轮候选）

- `availableSize()` / `totalSize()` 未在设备上验证（`sceAppMgrGetDevInfo`）。
- `list()` 结果在 MiniXplorer 里的渲染、以及 >1 层的深层目录遍历未逐一验证。
- 若要继续调试其他 MIDlet，可再次临时加诊断——**但务必避开 AOT 大方法**（见 v01.62 第六节）。

## 2026-09-14 v01.62：romgen SIGTRAP 定案（ARM 字面量池 off-by-8，上游潜伏 bug）+ JSR75 属性补全

> 承接 v01.61。给 `MIDletStateHandler.startSuite` 加 `[MIDLET]` 诊断 println 后，
> romgen 变成 SIGTRAP(133)，构建阻断。本轮把根因查到"1 字节"级别并修掉。

### 一、症状与定案过程（可复现）

- `phoneme-midp/build_vita.sh` 在 `cldc_vm.gmk:234` 报 `Error 133`；手动跑 romgen
  （只看 stdout）才见到真正的断言：`size 4096 too big for 12 bits` →
  `assert(has_room_for_imm(imm, size), "illegal immediate value")`
  （`phoneme-cldc/src/vm/cpu/arm/Assembler_arm.hpp:73`，`#ifndef PRODUCT` 下）。
- gdb（romgen **未 strip、带 debug_info**）符号化栈：
  `imm_index` ← `access_literal_pool (BinaryAssembler_arm.cpp:248)` ← `ldr_from` ←
  `ldr_literal` ← `ldr_oop` ← `invoke` ← … ← `JVMCompiler::compile` ←
  `JVMMethod::compile` ← `ROMOptimizer::precompile_methods (ROMOptimizer.cpp:3626)`。
- 关键局部量：`#2 pos=7188, target=3100` ⇒ `target-(pos+8)` = **-4096**（12 位合法范围 ±4095）；
  `#19 i=165, compiled_count=147, impossible_count=18, precompile_size=364` ⇒ 崩溃项 =
  AOT 编译队列第 165 项（0 基）。对照上一次成功构建的 `ROMLog.txt` 的
  `[AOT compilation report]` 第 165 项 = **`com/sun/midp/midlet/MIDletStateHandler.startSuite`**
  —— 正是本轮加打印的那个大方法。

### 二、根因：VFP 分支的绑定字面量复用窗口多 1 字节（上游 bug）

- `write_literal()` 写池时 `literal->set_bci(position)` ⇒ `_bci` = 池在代码里的位置。
- `access_literal_pool()` 用 `imm_index(pc, target - (pos + 8))` 生成 PC 相对偏移，
  要求 `|offset| ≤ 4095` ⇒ 可复用字面量的**最远回距** = `pos - 4087`。
- `find_literal()`（`ENABLE_ARM_VFP=1` 走的那支）：`offset = 4088 - 8`，
  `min_offset = _code_offset - offset` = `pos - 4088`，判据 `ptr->_bci >= min_offset`
  ⇒ **多接受 1 字节**（`_bci == pos-4088` → 偏移 -4096）。
  debug 构建断言崩溃；**PRODUCT 构建会静默把 4096 截断成 0 → 载入错误地址**（潜在错码）。
- 非 VFP 分支用 `ptr->_bci <= position` 本来就把该边界正确排除，所以这是 VFP 侧独有的
  off-by-one，不是"设计如此"。
- 触发条件：只有 AOT 大方法（`startSuite` 约 7.2KB 代码 + 大字面量池）才会出现
  "恰好 4088 字节之前的绑定字面量"。普通方法永远碰不到，所以上游一直没暴露。

### 三、修复

- `phoneme-cldc/src/vm/cpu/arm/BinaryAssembler_arm.cpp`（VFP 分支 `find_literal`）：
  `const int min_offset = _code_offset - offset + 1;` + 11 行推导注释。
  **不做 VITA 条件编译**：这是跨平台真实 bug，`#if defined(VITA)` 反而会把它藏起来。
- 重编宿主工具：`bash rebuild_vm.sh tools`（loopgen 需先建好；`-j` 下 romgen 会抢跑
  半成品 loopgen → `Permission denied`/Error 127，**再跑一次即可**）。改 VM 代码后
  **必须**重建 romgen，否则 ROM 生成物与源码不一致。产物
  `phoneme-cldc/build/vita_arm/dist/bin/romgen`（重建后 6,421,100 B）。
- 运行库同步：`bash rebuild_vm.sh build` ⇒ 只有含该文件的 `_MergedSrc003.o` 同代重编
  （10:35:12）；重打包后 29 成员、`jvm_fast_globals` 唯一 `D`、无 C-interpreter 符号。
  （运行期 JIT 关闭 `-int`，此修复对当前运行是惰性的，纯粹为源码/库一致。）

### 四、两个排查/脚本陷阱（务必避免重复踩）

1. **romgen 的两条输出链**：VM 崩溃报告走 **stdout**；`tty->print_cr` 诊断
   （`ClassFileParser.cpp:2157` 的 `PRE_NATIVES/POST_NATIVES`，4 万余行）走 **stderr**。
   用 `2>&1` 会把断言淹没 ⇒ 用 `> /tmp/out.txt`（或 `2>/dev/null`）。
   另：`gdb -ex 'run > file'` 会破坏 romgen 的 argv（报 `class not specified`），
   重定向必须在 shell 层做。
2. **`rebuild_vm.sh` 的 `set -e` + `grep -c` 假失败**：`ccount=$(... | grep -cE ...)`
   在 0 匹配时返回状态 1，`set -e` 让脚本在**重打包成功之后**静默 `exit 1`
   （日志无 FAIL、库却已更新 ⇒ 极易误判为"半成品库"）。已在两处加 `|| true`
   （2026-09-14）。若看到 `EXIT=1` 且最后一行是 `(full-generation repack...)`，就是它。

### 五、v01.62 交付内容与状态

- JSR75：`FileSystemRegistryImpl.listRoots()` 按规范返回**根名**
  （`ROOT_LIST_NAME = ROOT_NAME + "/"`）；
- `vita-port/config/system.config` 新增 `microedition.io.file.FileConnection.version: 1.0`
  （属性系统只搜 `applicationProperties` = system.config，须随包分发）；
- **临时**保留诊断打印用于 MiniXplorer 定位：`[ALERT]`（Alert 4 参构造 + setString）、
  `[MIDLET]`（startSuite/startApp/destroyApp/state loop）、`[JSR75]`（Protocol/
  FileConnectionImpl/FileSystemRegistryImpl）。**稳定后必须删除**（最小 diff 原则）。
- 产物：`vita-port/build/cmake/midp_vita.vpk` = **v01.62**，13,691,870 B（10:37:12），
  `arm-vita-eabi-nm midp_vita | grep -c Java_com_sun_midp_jsr075` = **21**；
  `ROMImage_01.cpp` 10:36:54、`obj/arm/ROMImage.o` 10:36:57（含新诊断）。

### 六、教训

- **不要给 AOT 大方法加 println**：新增字符串常量 + 代码会移动字面量池布局，可能正好踩到
  12 位边界（本例就是 10:18 那次 println 直接触发）。诊断打印优先放小方法，或用开关隔离；
  修完 off-by-one 之后仍要遵守，因为边界是"位置敏感"而非"长度敏感"。
- 编译脚本退出码不等于结果：先 grep 日志里的 `illegal immediate` / `VM Error` / `Error 133`。

### 遗留风险

- 字面量池**前向**窗口（`set_delayed_literal_write_threshold`）未审：按构造约有 15 字节余量，
  本轮未触发；`BinaryAssembler_thumb.cpp:497` 的同名函数未审（Thumb 路径未启用）。
- `microedition.io.file.FileConnection.version` 是否被运行期 `System.getProperty` 读到，
  待 MiniXplorer 实测确认。

## 2026-09-14 v01.60：JSR 75 (`javax.microedition.io.file`) 落地——独立子系统 `samples/j2me/jsr75`，单 root = `ux0:/data/J2ME00001`

> 承接 v01.59 的"地基已备、Java 层下一轮做"。本轮把 Java 层 + native 层 + 构建接线一次做完，**不碰任何共享 phoneME 文件**（新增 `jsr75/` 树 + `build_vita.sh` 两处开关）。

### 一、许可证 / 来源纪律（用户明确要求）

- 上游 phoneME **没有 JSR 75 实现**（只有 `USE_JSR_75` 空钩子 + 权限/清理接口桩），所以只能参考外部实现。
- 决定：**只取 JSR 75 规范的公开 API 签名**（`javax.microedition.io.file.*` 的 5 个类型，规范公开、签名本身不受版权保护），`com.sun.midp.*` 侧的一切 Impl（`FileConnectionImpl` / `FileStore` / `FileSystemRegistryImpl` / `Protocol`）**全部自己写**，写在现有 `pcsl_file_*` 之上。**没有从任何 GPL 项目复制代码**（`freej2me-plus` 的 `FileConnectionImpl.java` 是 GPL 系，只用于核对行为，未复制）。
- `phoneme_source/` 全程只读查阅，未复制任何文件进工作副本。

### 二、Root 策略（唯一 root，可读写）

- 唯一暴露的根就是启动器 `chdir()` 的那个目录：`vita_main.c:63 #define DATA_DIR "ux0:/data/J2ME00001"`。
- native 侧 `jsr75_root()` 用 `getcwd()` 取（fallback 硬编码同一字符串），因此**与 `vita_pcsl.c` 的 `vita_resolve_path()` 天然一致**，不需要重复 `#define`。
- Java 侧 `FileSystemRegistryImpl.ROOT_NAME = "data"`、`ROOT_URL = "file:///data/"`；`FileStore.getRootPath()` 返回 `ux0:/data/J2ME00001/`，Java 把 `file:///data/<x>` 翻成 `ux0:/data/J2ME00001/<x>` 后下传，路径直接命中 `vita_resolve_path()`。

### 三、关键机制发现（改之前必须知道）

1. **native 绑定全自动**：`romgen` 侧 `SourceObjectWriter::put_c_function()` 直接为 `KNIDECL(m)` 生成 `extern "C" Java_<class>_<method>`，**不需要重生成 `NativesTable.cpp`**。所以新增 native 只要 C 侧函数名与 Java 侧 `native` 方法名对上即可（本轮 C 侧 21 个 ↔ Java 侧 21 个，`nm` 逐一核对）。
2. **`pcsl_directory.h` 那一组（v01.59 补的 8 个）在 `libmidp.so` 里拿不到**——它们实现在**可执行文件**（`vita-port/src/vita_pcsl.c`），不在 MIDP 共享库链接的那套 PCSL 里。第一版 native 直接调 `pcsl_file_is_directory/mkdir/rmdir/get_time/getfreesize/gettotalsize`，链接期全部 undefined。**结论：`libmidp.so` 内的代码必须自洽**，目录/时间/容量这类事只能走 `sceIo*` / `sceAppMgrGetDevInfo`（`sceIo*` 与 `sceAppMgr*` 的 stub 在最终可执行文件链接里已由 CMake 提供）。
   - 另注：`phoneme-midp/src/vita_stubs.c` 里的 `pcsl_file_*` 是**死代码**（v01.59 已定案，CMakeLists 注释：old vita_stubs.o is NOT linked），别照它写。
3. **`preprocess_jpp.py` 的 DEFINES 为空**⇒ JSR 75 的 Java 文件是**普通 `.java`**，不能写 `#ifdef ENABLE_JSR_75`。
4. 构建钩子（`Subsystems.gmk:240`）：`USE_JSR_75=true` → `JPP_DEFS += -DENABLE_JSR_75` + `include $(JSR_75_DIR)/build/cldc_application/subsystem.gmk`；`SubsystemDefs.gmk:390` 用 `$(SUBSYSTEM_JSR_75_JAVA_FILES)` 进 `classes.zip` 规则、`$(SUBSYSTEM_JSR_75_NATIVE_FILES)` 进 `JTWI_NATIVE_FILES`。**include 顺序是对的**（`SubsystemDefs.gmk:31` 先 include `Subsystems.gmk`），所以 `subsystem.gmk` 里的变量在被消费前就已定义，不需要 jsr120 那套 `subsystem_defs.gmk`/`subsystem_rules.gmk`。

### 四、改动清单（全部新增，共享文件零改动）

- **新增 `samples/j2me/jsr75/`**（Vita 本地子系统）
  - `src/share/core/common/classes/javax/microedition/io/file/{FileConnection,FileSystemRegistry,FileSystemListener,ConnectionClosedException,IllegalModeException}.java`
  - `src/share/core/common/classes/com/sun/midp/jsr075/{FileConnectionImpl,FileStore,FileSystemRegistryImpl}.java`
  - `src/share/core/common/classes/com/sun/midp/io/j2me/file/Protocol.java`（GCF `file:` 协议，`Connector.open("file:///data/...")` 的入口）
  - `src/share/native/jsr75_file.c`（21 个 native，646 行）
  - `build/cldc_application/subsystem.gmk`（9 个 Java 文件绝对路径 + `vpath % $(JSR_75_NATIVE_DIR)` + `SUBSYSTEM_JSR_75_NATIVE_FILES = jsr75_file.c`）
- **修改 `phoneme-midp/build_vita.sh`**（唯一改动的既有文件，+6/-2）：两处 `USE_JSR_75=false` → `true` + `JSR_75_DIR`/`PROJECT_JSR_75_DIR`。
- **修改 `vita-port/CMakeLists.txt`**：`VITA_VERSION` 01.59 → **01.60** + 注释块。
- **未改** `build/vita_arm/Options.gmk`（其 `USE_JSR_75 = false` 被命令行变量覆盖，GNU make 命令行优先）。
- **`vita-port/CMakeLists.txt` 未加 `jsr75_file.c`**：它由 MIDP makefile 编进 `libobj.a`，CMake 只是复用该归档。

### 五、语义决定（记录下来，避免以后推翻）

- **filter**（`list(String filter, boolean includeHidden)`）：只按**条目名**做 `*` / `?` 通配匹配。目录在 native 侧带尾 `/` 上报，匹配前剥掉，所以 `list("*.txt", ...)` 不会因目录把 `dir.txt/` 也匹配进来造成歧义。
- **隐藏语义**：本移植把**名字以 `.` 开头**定义为隐藏。原因：Vita 文件 API 没有"隐藏"属性位（`pcsl_file_get_attribute` 的 HIDDEN 只能恒 0），而 `.` 前缀是唯一**同时驱动** `isHidden()`、`setHidden()`（实现为加/去前导点的改名）和 `list(..., includeHidden)` 的约定，三者天然自洽。
- **权限**：JSR 75 的 `javax.microedition.io.Connector.file.read/.write` 在 `domain: minimum,unsecured` 下**未授予**，因此 `Protocol` 刻意**不做 `AccessController` 检查**（否则 MIDlet 必然 SecurityException）。上游 MIDP 核心 `Connector` 不做 scheme 级权限检查（`grep Connector.file` 只命中 CDC 的 `Permissions.java`），所以这条路是通的——**但未上机验证**。

### 六、踩坑记录（本轮新增）

1. **`strncpy` 触发 `-Werror=stringop-truncation`**：MIDP 这套构建**默认 warnings-as-errors**（`build_vita.sh` 里那一串 `-Wno-error=...` 就是为此）。改成限长 `memcpy` + 手动补 NUL。
2. **KNI 宏展开缺分号**：`JSR75_HANDLE_METHOD(name, body)` 第一版 `body` 后没分号，5 处展开全部 `expected ';' before '}'`。宏内部补 `;` 后调用点写起来像赋值语句。
3. **KNI mangled 名手误**：`seekFile` 写成 `..._FileStore_seekFile` 之前错成 `jsr75` 而非 `jsr075`，链接期才暴露。**`arm-vita-eabi-nm <某个 .o> | grep " T "` 是最快的 KNI 名字自检**。
4. **头注释里写了 `sceIo*/sceAppMgr*`，其中的 `*/` 提前结束块注释**，导致后面 30 行全变成代码，报了一串莫名其妙的 `unknown type name 'PCSL'` / `ptrdiff_t`。注释里不要出现 `*/` 字面量。
5. **make 侧 `libmidp.so` 链接失败是既有无害项**：`_rom_linkcheck_mffd_false` undefined（`libobj.a(_MergedSrc005.o)`，与 JSR 75 无关，v01.48 条目已记录过），CMake 的真实可执行文件链接能解析它。所以判断 JSR 75 是否成功**不能看 `build_vita.sh` 的退出码**（它被 `| tee` 吞掉），必须看 `obj/arm/jsr75_file.o` + CMake 那一步。

### 七、验证（已做）

- `javac -source 1.3 -target 1.3`（bootclasspath = cldc_classes.zip，classpath = MIDP classes）**0 error**，产出 12 个 `.class`（含 `FileConnectionImpl$InnerInputStream/OutputStream`）。
- `jar tf classes.zip`：`javax/microedition/io/file/*`、`com/sun/midp/jsr075/*`、`com/sun/midp/io/j2me/file/Protocol.class` 全在。
- `obj/arm/jsr75_file.o` 编译通过；21 个 `Java_com_sun_midp_jsr075_FileStore_*`。
- `grep -c jsr075 ROMImage_01.cpp` = 21；`nativeFunctionTable.cpp` 45 处。
- `cmake --build build/cmake -j4` 链接通过；`arm-vita-eabi-nm build/cmake/midp_vita | grep -c Java_com_sun_midp_jsr075` = **21**。
- 产物：`vita-port/build/cmake/midp_vita.vpk` 13,689,448 B（2026-09-14 07:29）。

### 八、遗留 / 下一步

- **完全未上机/未进 Vita3K**：`FileConnection` 的运行时行为（root 枚举、`list` 过滤、`setHidden` 改名、流读写、`availableSize`）全是设计预期，没有任何实测证据。
- `com.sun.midp.jsr075.FileConnectionCleanupImpl` **不存在**：上游 `ams/.../Installer.java:1803` 会 `Class.forName` 它，会抛 `ClassNotFoundException`（该路径只在套件删除时走，暂判无害，待实测确认）。
- `availableSize/totalSize` 走 `sceAppMgrGetDevInfo`：CMake 可执行文件链接能解析，但 make 侧 `libmidp.so` 那条线缺 `SceAppMgr_stub`——如果将来真要让 `libmidp.so` 独立链接，需要补 `LIBS += -lSceAppMgr_stub`。
- 记忆里的"JIT 二次启动"、"物理尾巴字节留盘"等旧遗留不变。

## 2026-09-14 v01.59：JSR 75 地基——补齐 `pcsl_directory.h` 的 8 个 native

> 承接 v01.58 的"文件操作能力盘点"结论：`pcsl_file.h` 的 21 个 API 已全实现，缺口在 JSR 75。本轮先补**它依赖的最后一层 native 空洞**，Java 层下一轮做。

### 一、盘点修正：`vita_stubs.c` 是**死代码**

- `phoneme-midp/src/vita_stubs.c`（1195 行，`pcsl_file_*`/`pcsl_socket_*` 桩、`[file_open]` 调试串、写 `debug_log.txt`）**没有进链接**：`vita-port/CMakeLists.txt` 第 11 行注释明写 "old vita_stubs.o is NOT linked (replaced by src/vita_pcsl.c)"。
  ⇒ 查"某个 pcsl 函数有没有实现"**只能看 `vita_pcsl.c`**，改 `vita_stubs.c` 没有任何效果（本轮曾差点改错文件）。
- `gen_stubs.py` 生成的 30 个 Java 桩类（chameleon / orientation / `com.sun.midp.main.MIDletSuiteLoader`/`Configuration`/`CommandState` / GCI / `javax.microedition.media.*`）**不含 JSR 75**，所以"JSR75 的打桩代码"并不存在，无从"补全"。

### 二、真实缺口：`pcsl_directory.h` 8 个入口（`grep pcsl_directory_ vita_pcsl.c` = 0）

`phoneme-midp/build/vita_arm/pcsl/vita_arm/inc/pcsl_directory.h` 声明、而本移植从未实现，任何调用都会**链接期失败**：

| 函数 | Vita 实现 |
|---|---|
| `pcsl_file_is_directory` | `sceIoGetstat` + `SCE_S_ISDIR(st_mode)`，stat 失败返回 0，路径非法 -1 |
| `pcsl_file_mkdir` | `sceIoMkdir(path, 0777)`（单级，与 POSIX mkdir 一致） |
| `pcsl_file_rmdir` | `sceIoRmdir(path)` |
| `pcsl_file_getfreesize` | `sceAppMgrGetDevInfo(dev, ...)` → `free_size` |
| `pcsl_file_gettotalsize` | 同上 → `max_size` |
| `pcsl_file_get_attribute` | READ/WRITE 恒 1（Vita 无 per-file 权限位），EXECUTE/HIDDEN 0，未知 type -1 |
| `pcsl_file_set_attribute` | READ/WRITE 当成功 no-op，其余 -1 |
| `pcsl_file_get_time` | `st_mtime` → epoch 秒（见下） |

新增/复用的辅助：

- `vita_dir_path()`：`pcsl_string` → UTF-8（512 缓冲）→ 去尾 `/`、`\` → 空判 → `vita_resolve_path()`（与其它 `pcsl_file_*` 同一条路径解析）。
- `vita_stat_any()`：`sceIoGetstat` 失败时回退 `app0:` 形式——复刻 `pcsl_file_exist` 里 `ux0:/data/` → `app0:/` 的前缀置换，只有真实设备才需要。
- `vita_device_of()`：从绝对路径抽出 `"ux0:"` 式卷前缀，让容量查询按卷走而不是硬编码 `ux0:`。
- `vita_sce_datetime_to_epoch()`：**踩坑记录**——`SceIoStat::st_mtime` 不是 epoch 秒，而是 `SceDateTime` 结构体（`psp2common/types.h:195`：`unsigned short year, month, day, hour, minute, second; unsigned int microsecond;`）。第一版写 `(long)st.st_mtime` 直接编译不过（`aggregate value used where an integer was expected`）。改用 Howard Hinnant 的 `days_from_civil` **纯整数**换算，不引 `psp2/rtc.h`、不加 `SceRtc_stub`、不依赖 libc 时区，Vita3K 与真机结果一致。代价：RTC 按原值读，若主机时钟设的是本地时间，epoch 差一个 UTC 偏移——MIDlet 只做相互比较，无影响。

### 三、改动（最小 diff，只动工作副本）

1. `vita-port/src/vita_pcsl.c`
   - `#include <pcsl_directory.h>`（紧随 `pcsl_file.h`）。
   - 在 `pcsl_file_getpathseparator()` 与 `PCSL Print stubs` 横幅之间新增 "PCSL directory / attribute service (JSR 75 groundwork)" 一节：8 个 `pcsl_directory.h` 入口 + 4 个 static 辅助。
2. `vita-port/CMakeLists.txt`：`VITA_VERSION` 01.58 → **01.59** + 注释块。

### 四、验证

- `cmake --build build/cmake -j4` 通过（无 error/warning）。
- `arm-vita-eabi-nm build/cmake/midp_vita | grep -E " T pcsl_file_"`：**21 → 29**，新增 8 个正是上表函数（`81008788`..`810089f0`）。
- 产物：`build/cmake/midp_vita.vpk` 13,667,710 B，md5 `a973d3f2516243bd591ccf49c0a20f30`。

### 五、遗留 / 下一步

- 这 8 个函数**目前无调用者**（JSR 75 Java 层还没写），因此 01.59 行为与 01.58 **完全等价**，纯地基。
- JSR 75 上游 phoneME **无源码**（只有 `USE_JSR_75` 空钩子 + 权限/清理接口桩）；互联网可参考：`nikita36078/J2ME-Loader`（Apache-2.0，只有 5 个 API 接口）与 `TASEmulators/freej2me-plus`（含 `FileConnectionImpl.java`，许可证 GPL 系）。计划：按 Apache-2.0 那套**只取公开 API 签名**，Impl 自己写在现有 `pcsl_file_*` + 本轮 `pcsl_directory_*` 之上。
- 待办（Java 层）：`javax.microedition.io.file.{FileConnection, FileSystemRegistry, FileSystemListener, ConnectionClosedException, IllegalModeException}` + Impl + `com.sun.midp.io.j2me.file.Protocol`，接到 `vita_overlay.mk` 或新子系统目录（**不改 `phoneme_source/`**），把 `ux0:/data/J2ME00001/` 注册成 root；任一 `.java` 变更会自动触发 romgen 级联重生成 `ROMImage_*.cpp`。

## 2026-09-14 v01.58：JIT 策略开关（`launch.cfg` 第 4 行 `jit=0|1|2`）+ 文件操作能力盘点

> 真机问题按用户要求**只记录、不上机复测**（v01.57 诊断包已就位，见下一节）。
> 本轮转向"补全 J2ME 功能"，先做能力盘点，再交付第一个可用开关：JIT。

### 一、文件操作能力盘点（结论：**native 层无缺口**）

- `vita-port/src/vita_pcsl.c` 中 `pcsl_file.h` 声明的 **21 个 API 全部已实现**：
  `init / finalize / open / close / read / write / unlink / truncate / exist /
  commitwrite / rename / openfilelist / closefilelist / getnextentry / seek /
  sizeofopenfile / sizeof / getusedspace / getfreespace / getfileseparator /
  getpathseparator`。
  ⇒ MIDP 自身（RMS、jar/class 读取、资源加载）所需的文件操作**已经不缺**，
  不需要新增 native 代码。
- 缺口只在 **JSR 75 `javax.microedition.io.file.FileConnection`**，而它在
  **上游 phoneME 里就不存在**：`phoneme_source/phoneME/` 无 `jsr75` 目录，
  只有 `abstractions/` 下的 `FileConnectionPermission` 权限桩与
  `ams_jsr_interface` 里的 `com.sun.midp.jsr075.FileConnectionCleanup`；
  `phoneme-midp/build/vita_arm/Options.gmk` 中 `USE_JSR_75 = false`（与其它 JSR 一致）。
  `phoneme-midp/src/protocol/file/` 是 `com.sun.midp.io.j2me.storage`（RMS 存储后端），
  **不是** JSR75。
  ⇒ 要做 JSR75 得**新写 Java API 类 + native 接线**（可架在现有 `pcsl_file_*` 之上），
  属于新增特性而非补漏，**暂缓**（等真机可用再决策，见"遗留"）。

### 二、JIT 现状调查（结论：JIT **一直在**二进制里，只是被无条件关掉）

- `phoneme-cldc/src/vm/share/utilities/Globals.hpp:231`：
  `product(bool, UseCompiler, true, ...)` ⇒ **默认开**；
  VPK 里可见 29 个 `T .*Compiler` 符号；`Universe.cpp:532-533` 调用
  `CompiledMethodCache::init()` 与 `Compiler::initialize()`。
- 但 `phoneme-midp .../jams/native/runMidlet.c` 里有一句**无条件的**
  `JVM_ParseOneArg("-int")` ⇒ `Arguments.cpp:129` `UseCompiler = false`，
  自 v01.46 起 JIT 从未真正跑过。
- 关掉的原因：**第 2 轮（UC）启用 JIT 会崩**，faulting PC 落在堆内
  `compiler_area`（v01.45 章节），根因未定位。

### 三、改动（最小 diff）

1. `vita-port/src/vita_main.c`
   - 新增 `#define VITA_JIT_DEFAULT 0` + `int vita_jit_policy = VITA_JIT_DEFAULT;`
     （全局，供 MIDP 侧读取）+ 只读 `launch.cfg` 的 `read_jit_policy()`。
   - `for(;;)` 轮循环增加 `int round` 计数，每轮开头重读 cfg 并折算：
     `vita_jit_policy = (cfg_jit == 1 && round > 0) ? 0 : cfg_jit;`
     （`jit=1` = 仅第 1 轮开 JIT），并 `crumb_printf("round %d: jit cfg=%d -> policy=%d", ...)`。
   - `runMidlet` 前打印真实 VM 标志：
     `crumb_printf("vm UseCompiler=%d (jit policy=%d)", ...)`（weak extern 读取
     `UseCompiler`，无编译器子系统时也能链）。
2. `phoneme-midp .../jams/native/runMidlet.c`
   - 把无条件 `-int` 换成**经弱符号读取策略**后才注入：
     ```c
     extern int vita_jit_policy __attribute__((weak));
     if (&vita_jit_policy == NULL || vita_jit_policy == 0) { "-int" }
     ```
     非 Vita 构建 / 独立 `libmidp.so` 链接里该弱符号为 NULL ⇒ 行为与旧版**完全一致**。
3. `vita-port/CMakeLists.txt`：`VITA_VERSION` 01.57 → **01.58** + 注释块。
4. `vita-port/README.md`：`launch.cfg` 说明补第 4 行 `jit=`，修正"默认 orientation"
   描述（代码默认是 **portrait**，原文误写成 landscape），补全日志文件清单。

### 四、关键机制（下次改 JIT 前重读）

- `-int` → `Arguments.cpp:129` `UseCompiler = false`；`-comp` → `MixedMode=false; UseCompiler=true`。
- `UseCompiler` 是**进程级全局**，`JVM.cpp:594-672` 只在**宿主 romization 期**
  保存/恢复它 ⇒ 运行时一旦 `-int`，**后续所有轮次都保持解释器**——
  这正是 `jit=1`（仅第 1 轮）自动变成"第 2 轮起解释器"的原因，无需额外代码。
- `CompiledMethodCache::init()` 每轮由 `Universe::initialize()` 重做（清零
  `Map/weights/upb/size/last_old`），所以 `jit=2`（每轮都开）不需要手工重置缓存。

### 五、验证（二进制级已过）

- 重编 `vita-port/build.sh midp`（触发 `runMidlet.o` 重编）→ **Build successful**。
- `arm-vita-eabi-nm .../obj/arm/runMidlet.o | grep vita_jit_policy` → `w vita_jit_policy`
  （弱未定义，符合预期）；VPK 里 → `812d2c48 B vita_jit_policy`（启动器强定义胜出）。
- **反汇编确认判断没被优化掉**（`objdump -dr runMidlet.o`）：
  `movw/movt r3, vita_jit_policy` → `cmp r3,#0` → `beq 注入-int` →
  `ldr r3,[r3]` → `cmp r3,#0` → `beq 注入-int`：地址检查与值检查都在。
- 产物：`vita-port/build/cmake/midp_vita.vpk` = **v01.58 b215 (143f5cd)**，
  13666113 B，MD5 `a1ca93a299fbdce68595853d7a8bf8c2`。

### 遗留

- **第 2 轮 JIT 崩溃根因仍未修**（v01.45）。默认 `jit=0` 不改变 v01.46 以来的稳定性；
  `jit=2` 是该崩溃的最小复现开关，`jit=1` 可拿回第 1 轮 JIT 性能——两者都**待真机**。
- JSR 75 `FileConnection`：上游无实现，需新写 Java 类 + native 接线；
  **需用户决策**后再动手（真机可用前建议暂缓）。

## 2026-09-14 v01.57：真机 core dump 取证——`pte_osSemaphoreCreate` 空 pHandle 落盘写空指针 + 全链路去 stdio 诊断包（vita-port `vita_crumb.c/.h`，phoneme-midp `midp_run.c`）

### 用户现象

真机跑一次后留下崩溃转储 `psp2core-1789319139-0x00001636c3-eboot.bin.psp2dmp`。
**当前没有可用真机复测**，故本轮只做静态取证 + 交付一个"下一次上机就能指认凶手"的诊断包。

### 取证（**纠正了第一版结论**）

- 第一版结论（已作废）：把 fault 归到 JDWP `VMEvent::exception_event`。错因是拿栈上残留
  地址当现场——SP±16B 与 saved r5 落在 `0x813c0f50/0x813c0f64` 的旧内容上是**陈旧栈残渣**。
- **真实现场**：活动 PC = `0x810bb25a`，指令 `strge r3,[r5]`，`r5 = 0`。
  该地址落在 `pte_osSemaphoreCreate` 内（函数首 `0x810bb234`）：
  `push {r4,r5,lr}; mov r2,r0; sub sp,#12; movs r4,#0; movw r0,#0x1848; mov r5,r1;
   movw r3,#0x7fff; mov r1,r4; mov r4,lr; str r4,[sp,#0]; movt r0,#0x812b;
   blx sceKernelCreateSema; subs r3,r0,#0; itet ge; movge r0,r4; movlt r0,#2;
   strge r3,[r5,#0]`
  → `r5` 就是 `pHandle` 出参。`sceKernelCreateSema` **成功**（r0>=0 ⇒ ge 分支），
  随后把句柄写回 `*pHandle` 时 `pHandle == NULL` ⇒ 空指针写。
- **为什么不是 JDWP**：`_debugger_active` 只由 `JavaDebugger.cpp:1112`
  (`connect_java_debugger`) 置位，而 `Frame.cpp:948` 的异常钩子以
  `if (_debugger_active)` 门控；且 `USE_ON_DEVICE_DEBUG=false`，
  `midp_run.c` 的 `midpInitializeDebugger` 不会自动注入 `-debugger -nosuspend`。
  整条 JDWP 链是死的。
- **主要假设**：某个**VM 工作线程的首次 stdio 写**触发 newlib 惰性 FILE 锁初始化，
  该路径调用 `pte_osSemaphoreCreate` 时传了 NULL 出参。（Vita3K 对此宽容——与
  之前 `g_caps_tone`、PNG 编码器同类的"模拟器盲区"。）

### 修复（本轮 = 诊断包，不是根修）

1. **新增 `vita-port/src/vita_crumb.{c,h}`**：完全绕开 stdio 的面包屑通道。
   - `crumb_append(path, s, len)`：`__atomic_exchange_n` 门闩 + 按路径缓存句柄 +
     `sceIoOpen(O_CREAT|O_WRONLY|O_APPEND)`/`sceIoWrite`，**不碰任何 FILE\***。
   - `crumb_marker/printf/flush`，宏 `CRUMB(...)` / `CRUMB_SEC(l)`。
   - `extern int __real_pte_osSemaphoreCreate(int, void**);` +
     `int __wrap_pte_osSemaphoreCreate(int initialValue, void **pHandle)`：
     每次创建都记 `[pte] sem create init=%d handle=%p -> %p rc=%d from %p`
     （`__builtin_return_address(0)` = 调用者地址）；`pHandle == NULL` 时记
     `[pte] BLOCKED NULL-pHandle sem create from %p` 并直接返回 2
     （= 内核失败分支的同一 pte_osResult），不再走空指针写。
2. **热打印路径全部去 stdio**（这就是"下一次不会再崩"的那一刀）：
   - `phoneme-midp .../native/midp_run.c` 的 `JVMSPI_PrintRaw`：逐字符
     `fopen/fwrite/fclose` → `crumb_append("ux0:/data/vm_output.log", s, length)`，
     之后照旧 `pcsl_print_chars`。文件内加了 `crumb_append` 的 **weak no-op 兜底定义**
     （保证独立链接 `libmidp.so` 时不因缺符号失败；VPK 链接里 `vita_crumb.c` 的强符号胜出）。
   - `vita-port/src/vita_pcsl.c` 的 `pcsl_print_chars`：`fprintf(stderr,...)` →
     `crumb_append("ux0:/data/vm_stderr.log", s, len)`。
3. **启动器锚点** `vita-port/src/vita_main.c`：`crumb_marker` 打在
   launcher start / net early init done / round begin / runMidlet enter / runMidlet exit，
   外加 `crumb_printf("version: %s", ...)`、`crumb_printf("launch: %s / %s", jar, class)`、
   `crumb_printf("runMidlet returned %d", status)` + `crumb_flush()`（块内声明保持 C89 合法）。
4. **链接期拦截** `vita-port/CMakeLists.txt`：源列表加 `src/vita_crumb.c`；
   链接块加 `-Wl,--wrap=pte_osSemaphoreCreate`（必须排在 `-lpthread` 之前）。

### 构建/产物

- 重编：`cd vita-port && export VITASDK=/home/zyb/.local/vitasdk && export PATH=$VITASDK/bin:/home/zyb/tools/jdk8u502-b07/bin:$PATH && rm -f build/cmake/vita_version.h && ./build.sh`
  → **Build successful**（版本号同时 bump 01.55 → 01.57）。
- 产物：`vita-port/build/cmake/midp_vita.vpk` = **v01.57 b214 (199c975)**，
  13666575 B，MD5 `4a488fc550ce579a664e9a3bb22107ab`。

### 验证入口（二进制级已过）

- `strings midp_vita | grep "version: J2ME"` → `v01.57 b214`
- `arm-vita-eabi-nm midp_vita | grep pte_osSemaphoreCreate` →
  `__wrap_pte_osSemaphoreCreate @0x8100b4bc` + `pte_osSemaphoreCreate @0x810bb51c`
  （重定向已生效：`pthread_mutex_init`/`sem_init` 走 wrapper，只有 wrapper 调真函数）
- `nm | grep crumb` → `crumb_append/flush/marker/printf` 全在位。

### 下次上机怎么做（**待真机验证**，本轮无机器）

1. 先删 `ux0:/data/J2ME00001/crumb.log`（避免旧内容混淆）。
2. 装上 v01.57 跑一遍。
3. 打开 `ux0:/data/J2ME00001/crumb.log`：出现
   `[pte] BLOCKED NULL-pHandle sem create from 0x<caller>` 那一行的 `<caller>` 就是凶手，
   用**本次**构建的 `build/cmake/midp_vita`（含符号、`main` 在 `0x81000060`）
   `arm-vita-eabi-addr2line -e midp_vita -f -C 0x<caller>` 即可定位。
   （**注意**：dump 里的 `0x8105e099`/`0x810bb25a` 等地址来自**上一个**二进制，
   重链后全部作废，别再拿旧数字对地址。）
4. 若 `crumb.log` 里连 launcher 锚点都没有 ⇒ 崩在比 `vita_main` 更早的阶段。

### 遗留

- 根因（谁把 NULL 传进 `pte_osSemaphoreCreate`）本轮**未修**，只做了断路 + 取证；
  真机日志到手后再收敛。
- 已知无害项：`libmidp.so` 链接尾部报 `undefined reference to _rom_linkcheck_mffd_false`
  （`MIDP.gmk` 给 ROMImage.o 硬编码 `-DMSW_FIRST_FOR_DOUBLE=1` 而 `libcldc_vm.a` 是 0）；
  `phoneme-midp/build/vita_arm/bin/arm/` 从来没产出过 `libmidp.so`，VPK 走 `libobj.a`
  + 启动器 `vm_rom_stubs.c`（其中已定义该符号），**VPK 链接不受影响**。
- JIT 二次启动根因仍未修（`-int` 掩盖中）——见 v01.45 后续章节。

## 2026-09-13 v01.56：真机安装 0x8010113D 定案——sce_sys PNG 必须是索引色（vita-port `assets/sce_sys/`）

### 定案过程（12 包 A/B 矩阵，真机实测）

历时三轮的排查矩阵（A–Q 包，全在 `build/test_vpks/` 生成后 FTP 实测）：

| 包 | 变量 | 真机结果 |
|---|---|---|
| A 无data / C 完整换ID / D 未压缩SELF / E 修template | 我们 sfo + **RGB PNG** | ❌ 全败（88–99% 漂移） |
| H hello eboot + 我们壳 | **RGB PNG** + 手写 gate template | ❌ |
| F hello+2MB填充 / G hello+16MB填充 / J python重打包hello | 纯 hello + python zipfile 打包 | ✅ 全过 |
| I 我们的 eboot + hello 壳 | eboot 变量 | ✅（eboot 洗清） |
| N 我们的完整 sfo + hello 资源 | sfo/TITLE_ID 变量 | ✅（sfo 洗清，含数字前缀 J2ME） |
| O hello + 仅 TITLE_ID=JMEE00002 | 数字前缀理论 | ✅（该理论作废） |
| Q 我们 RGB PNG + hello 原版 template | **PNG 单变量** | ❌ **定罪** |
| P 我们全内容 + **索引色 PNG** | 修复验证 | ✅ **定案** |

### 根因

- **SceShell/promoter 拒绝真彩色（color-type 2）PNG**，只接受索引色（color-type 3）。hello_world 三个资源全是 palette 型（bg 甚至是 1-bit），我们的全是 8-bit RGB——尺寸合规（128×128/840×500/280×158）但编码不对。
- **Vita3K 不校验** → 模拟器永远能装，掩盖问题。
- 失败点漂移（88/92/99%）= promoter 收尾逐个解码资源，死点随包内内容变化——"固定字段错误失败点应稳定"的反向推理是关键突破口。

### 修复（vita-port 提交 f43ff34）

- `icon0.png`/`bg.png`/`startup.png`：RGB → 8-bit palette 重编码（纯 Python zlib 手写 PNG 编解码，无 Pillow/ImageMagick 可用；与真机验证过的 P 包逐字节同源）。
- `template.xml`：`content-ver="01.00"` → `content-rev="1"`；startup 从 frame2/liveitem 移到标准 `<gate><startup-image>` 写法。
- 正式包重建核验：三 PNG ctype=3、template 含 gate、191 条目 testzip OK（13,664,532 字节，MD5 a5ba9e64）。

### 教训

1. **对照样本要逐字节剖到编码层**——"PNG 尺寸合规"不等于"PNG 合法"，color-type 差异肉眼不可见。
2. **A/B 矩阵是最后手段但极有效**——12 个包把 eboot/sfo/体积/传输/打包器/PNG 六个假设全部一次洗清。
3. **失败点漂移是数据敏感错误的指纹**——固定字段错误失败点稳定，漂移意味着校验器在扫描内容流。
4. 模拟器宽容性 = 排查盲区：Vita3K 不查的东西（MEMSIZE、PNG 编码）恰好都是真机雷区。

## 2026-09-13 v01.55：总根因定案——`pcsl_string` 桩违反上游 NUL 终止语义，所有 RMS 库共享同一个物理文件（vita-port `vita_pcsl.c`）

### 用户报告（v01.54 实测）
> "第一次保存书签，提示保存成功，实际没有，退出的时候提示存储空间少。第二次进卡在初始化。"

### 决定性证据（v01.54 的 `name=` 面包屑 + FFFFFFFF hexdump）
```
[rms] open name='coreA' suite=-1 exists=0 (new store)     ← 新建库，写全新头（signature+next=1+时间戳）
[rms] open name='A'     suite=-1 exists=1 next=1 live=0 ver=0 data=0   ← A 读到的正是 coreA 刚写的头！
[rms] open name='coreA' ... exists=1 next=2 live=1 ver=1 data=0        ← coreA 二开读到别人的写入结果
[rms] open name='G2'/'B'/'P'/'CK'/'R' ... 全部 exists=1 next=2 live=1 ver=1 data=0
[rms] SALVAGE ... scan_end=104  /  64 bytes preserved   ← 所有库完全相同
FFFFFFFF 文件：next=2 live=1 ver=1 data=0 + offset40 一个空闲块(id=-1,size=0x38)，共 104 字节
```
**不同名字的库在读同一个物理文件 `appdb_1A35/FFFFFFFF`——库名与 .db 后缀根本没进路径。**

### 根因（上游语义 vs 我们的桩）
上游不变量（`phoneme_source/phoneME/pcsl/string/utf16/pcsl_string.c`，对读确认）：
1. `pcsl_string.data` **永远以 NUL 结尾**，`->length` **计入这个 NUL**（`:110` "Do not count terminating '\0'" 是访问器的职责）
2. `PCSL_STRING_EMPTY = {&zero_char, 1, 0}`（length=**1**，不是 0）
3. 字面量宏 `PCSL_DEFINE_*_LITERAL`：`length = sizeof(arr)/sizeof(jchar)`（NUL 含在内）
4. `pcsl_string_cat`（`:490`）**吃掉第一个串的结尾 NUL**（"Strip the terminating zero at the end of the first string"）再拼接
5. `midp_suiteid2pcsl_string(-1)` 返回 `{L"FFFFFFFF\0", length=9}`（静态缓冲区，8 位 hex+NUL）

我们的桩（`vita-port/src/vita_pcsl.c`）全部违反：
- `PCSL_STRING_EMPTY = {NULL, 0, 0}`；`pcsl_string_length` 返回 `->length` 原值不减 1
- `cat`/`append`/`append_buf`/`append_char` 原样拷贝 `->length` 个 jchar → **字面量的内嵌 NUL 被拼进路径中部**
- `convert_to_utf8` 按 `->length` 逐字转换（含内嵌 NUL），`snprintf("%s")`/`sceIoOpen` 在第一个 NUL 截断

于是 `root + "FFFFFFFF\0" + "A" + ".db"` 的 C 视图 = `…/rms/appdb_1A35/FFFFFFFF`——**全部库同文件**。这解释了一切症状：
- "保存成功实际没有"：写到共享文件，下次开库 header 又被别的库覆盖
- "二次进全丢"：每次开库都读到别的库的 header → bad chain → salvage
- "存储空间少"：所有库的写入叠在一个文件里，反复 salvage/compact 把它搅成浆糊
- `[pcsl] unlink blocked .../FFFFFFFF`：UC 删自己的库（路径=FFFFFFFF）时句柄还开着
- 上游 `rms.c`/`suitestore_intern.c` **零 bug**（diff IDENTICAL 证实）——问题全在我们自己的 pcsl 桩

### v01.55 修改（`vita-port/src/vita_pcsl.c`，全部标注 v01.55）
1. `PCSL_STRING_EMPTY` → `{&static_zero, 1, 0}`（与上游一致）
2. `pcsl_string_length`/`utf16_length`：末位是 NUL 则减 1
3. `pcsl_string_utf8_length`/`convert_to_utf8`：只转换内容（停在终止零）——消除 C 串截断点
4. `convert_from_utf8`/`from_utf16`：**剥掉全部尾部 NUL**（上游 `:315`"Strip trailing zero characters"）+ 存 `length+1`（含单个终止零）
5. `pcsl_string_cat` 重写：吃掉首串结尾 NUL、保证结果恰好一个终止零、空结果返回 EMPTY 常量（**核心修复**）
6. `pcsl_string_dup`：非堆（常量/字面量）直接结构体拷贝（上游语义，避免 free 静态数据）
7. `append`/`append_char`/`append_buf`：全部改走 cat/convert_from_utf16
8. `equals`/`compare`/`starts_with`/`ends_with`/`index_of*`/`convert_to_jint`/`substring`：全部改为内容长度语义（终止零不参与）
9. `pcsl_file_open` 新增 `/rms/` 路径一次性面包屑 `[pcsl] open path=<abs> flags=0x.. size=<N>`（8 槽去重）——下一轮日志直接看到每个库映射到哪个物理文件

### 构建验证（v01.55）
- `./build.sh midp`：`Build successful`，VPK `build/cmake/midp_vita.vpk` 13,679,669 B，`vita_version.h`=01.55 b211（build 号没变是正常的，计数器按 git hash+配置）
- ELF 内 ASCII 验证：`[pcsl] open path=`=1、`01.55`=2、`01.54`=0
- `libmidp.so` 的 `collect2: error: ld returned 1 exit status`（`_rom_linkcheck_mffd_false` 未定义）是已知无害噪音（v01.53 起就有，最终 ELF 由 CMake 链接成功）
- `git diff --stat`：`vita_pcsl.c` 本轮大幅修改（pcsl_string 系列全量对齐上游）

### 下一轮验证判据（v01.55 实测）
1. **`[pcsl] open path=...` 应当出现多个不同路径**：`…/FFFFFFFF/A.db`、`…/FFFFFFFF/coreA.db`、`…/FFFFFFFF/G2.db`…——每个库一个文件，不再全是裸 `FFFFFFFF`
2. `[rms] open name='X'` 各库头状态**互不相同**（不再全部 `next=2 live=1 ver=1 data=0`）
3. `[rms] salvage` 不应再对每个库反复触发；书签保存后二次进入仍在
4. 旧数据：共享文件 `FFFFFFFF` 里只有 104 字节垃圾（空闲块），无有效数据可救——**首次进 v01.55 后各库从零开始是预期行为**，之后才是真持久化
5. 注意：v01.54 的 `SALVAGE skipped ... 64 bytes preserved` 保护了这个共享文件没被清零（v01.53 的零收获保护立功了）

### 遗留 / 未做
1. `pcsl_esc_attach_string` 仍是 dup 桩（非上游转义编码）。RMS 库名是 ASCII（A/B/G2/coreA）时 dup 与真转义等价；**中文名/特殊字符库名仍可能撞路径**——真转义实现留待需要时从 `phoneme_source/phoneME/pcsl/escfilenames/pcsl_esc.c` 移植
2. `pcsl_string_convert_to_jlong` 仍是 jint 精度桩（无调用方受影响，观察）
3. `[pcsl] unlink blocked .../FFFFFFFF` 调用方定位：v01.55 修复后该路径不再被构建，预计自然消失
4. 上游 `pcsl_string.c` 还有 `pcsl_utf16_convert_to_utf8` 等转换助手未移植——目前手写转换已覆盖需求


## 2026-09-12 v01.54：第二次进 UC 书签/历史全丢——三个真凶：db 头永不落盘、抢救把空闲块当 8 字节跨、抢救"零收获"时毁灭式重写（phoneme-midp `RecordStoreImpl.java` + vita-port `CMakeLists.txt` 版本号）

### 用户报告（v01.53 实测）
> "第二次进 uc，之前保存的书签，历史记录都没了。和首次进入一样。"

v01.53 把"空间不足"压下去了，但**跨次启动持久化**这条线没修好：第一次进能存，第二次进就是空的。

### 日志证据（用户提供的 v01.53 stderr）
```
[RMS] created appdb: ux0:/data/J2ME00001/rms/appdb_1A35
[rms] salvage (bad chain) live=0 data_size=64
[rms] SALVAGE live=1 max_id=1 old_data_size=64 scan_end=104
[pcsl] unlink blocked, file open elsewhere: ux0:/data/J2ME00001/rms/appdb_1A35/FFFFFFFF   (重复多次)
[rms] salvage (bad chain) live=1 data_size=0
[rms] SALVAGE live=0 max_id=0 old_data_size=0 scan_end=5280        ← 5KB 数据被就地抹掉
第二次进 UC:
[rms] salvage (bad chain) live=0 data_size=64        ← 与第一轮开头一字不差
[rms] SALVAGE live=1 max_id=3 old_data_size=64 scan_end=5280
[rms] salvage (empty header, live blocks present) data_size=0
[rms] SALVAGE live=1 max_id=54307759 old_data_size=0 scan_end=6032  ← 垃圾块被收下
[rms] compact skipped: block chain inconsistent
```
`FFFFFFFF` 文件 8464B 的 hexdump 显示 header 自洽（`next_id=0x033CABB0`、`live=1`、`data_size=5248`）但 **offset 40 起全是 0**——头和块对不上，人眼可见。

### 三个根因（都能独立造成"数据看起来没保存"）

**1. db 头的写入只进 `midp_file_cache`，永不落盘（持久化缺失）**
- 上游 `RecordStoreImpl` 里 **10 处 `// dbFile.commitWrite();` 全是注释掉的**（`phoneme_source` 原样如此，见其 215/454/514/714/863/884/1040/1086/1118 行，只有 1228 行抢救路径是活的），上游靠"关库时 cache 落盘"。
- **Vita 上库从不被关**：`vita_main.c` 的 launcher 主循环让 VM/MIDlet 常驻（`runMidlet` 返回后进程不退出），所以：
  - `addRecord` 写的 db 头 = 4~20 B → 进 `midp_file_cache`（`RMS_CACHE_LIMIT=3072`）；
  - 记录 payload 大 → **`uncachedWrite` 直接下盘**（超过 cache limit）。
  - ⇒ 磁盘上出现"**payload 在，header 不在**"的结构：`next_id=1 live=0 data_size=0` 压着一个 5KB 块。这正是每次 `salvage (empty header, live blocks present)` 的形状，也是 MIDlet 每次都拿到"空库"的机制。
- **修复**：在 MIDP 规范规定的**持久化提交点**上恢复 commit——`addRecord`（规范原文 "The record is written to persistent storage before the method returns."）、`deleteRecord`、`setRecord`。子步骤 `addBlock`/`freeBlock`/`writeBlock` 不各自提交（否则一次 addRecord 刷三次），`compactRecords` 末尾本来就有 `truncate()` 带刷。

**2. 抢救扫描把空闲块当"跳过 8 字节 block header"处理（垃圾块的来源）**
- 规范走法是 `currentOffset += currentSize`，**活块和空闲块一样按整块跨**（`RecordStoreIndex.getRecordIDs()` 与 `getFreeBlock()` 均如此，已对读确认）。
- v01.52/v01.53 的 `salvageStore()` 只在"活块"分支 `offset += size`，**`else` 分支一律 `offset += BLOCK_HEADER_SIZE`**——于是扫描器踩进空闲块的 payload 里、按随机字节"重新同步"，把 payload 当成块头收下 → 日志里 `max_id=54307759` 就是这么来的（`1615147117` 同源）。随后按这个垃圾块致密重写 + 截断，**真实记录被覆盖/截掉** = 第二次进书签没了。
- **修复**：新增分支 `id < 0 && dataSize >= 0 && size > 0 && blockFitsInFile(offset, size)` → `offset += size`（健康空闲块按整块跨）；只有真正坏头才走 `+= BLOCK_HEADER_SIZE` 重新同步。

**3. 抢救"零收获"时是毁灭式的（v01.52 起就埋着）**
- 原逻辑：`liveCount==0` → 照样致密重写（无内容）→ 把 `[40, scanEndOffset)` **整段零填** → 写一个 `data_size=0` 的空头 → `truncate(40)`。日志里的 `SALVAGE live=0 max_id=0 ... scan_end=5280` 就是**一个 5KB 库被物理抹掉**，不可恢复。
- 若扫描器本身误判（根因 2 正是），这一步就把用户数据从"没读出来"变成"真没了"。
- **修复**：`liveCount == 0 && scanEndOffset > DB_HEADER_SIZE` 时**直接 return，不写不填不截**，只打一行
  `[rms] SALVAGE skipped name='<store>': no live block found, <N> bytes preserved`。
  代价仅是该库这次仍以空库打开、下次还会再扫一遍；**任何情况下都不能因为"我读不懂"而销毁唯一一份数据**。

### v01.54 修改
**A. `phoneme-midp/src/rms/rms_api/reference/classes/com/sun/midp/rms/RecordStoreImpl.java`**
1. `addRecord`/`deleteRecord`/`setRecord` 三处 `dbFile.commitWrite();` 恢复（带 VITA 说明注释）。
2. `salvageStore(String storeName, byte[] dbHeaderData)` 签名加库名（构造器 3 个调用点同步），日志全部带 `name='...'`——**下一轮日志必须能直接看出是哪个库**。
3. 扫描循环新增"健康空闲块按整块跨"分支（根因 2）。
4. `liveCount==0` 且文件非空时提前 return（根因 3）。
5. 构造器新增开库面包屑（`exists` 两分支各一条，含 `next/live/ver/data/free`）：
   ```
   [rms] open name='' suite=-1 exists=1 next=.. live=.. ver=.. data=.. free=..
   [rms] open name='' suite=-1 exists=0 (new store)
   ```
   **每次开库头状态一次打全**，"上次存的数据这次还在不在盘上"一眼可判，不再靠猜。
**B. `vita-port/CMakeLists.txt`**：`VITA_VERSION` 01.53 → 01.54。

### 构建验证（本轮）
- `./build.sh midp`：`Build successful`，VPK 13,678,862 B（`build/cmake/midp_vita.vpk`），`vita_version.h` = `01.54 b211`。
- ROM 链验证（遵守 v01.53 教训 1，不看日志看字节）：ELF 内 **UTF-16LE** 计数
  `SALVAGE skipped name=`=1、`open name='`=1、`salvage (bad chain) name=`=1、`salvage (empty header, live blocks present) name=`=1、`SALVAGE name='`=1（UTF-8 全 0，符合 Java 串规则）；`01.54` ASCII=2 / `01.53`=0。
- `git diff --stat`（phoneme-midp）：`RecordStoreIndex.java` +13、`RecordStoreImpl.java` +663/-34（累计，含 v01.49~v01.53）。

### 下一轮日志怎么看（v01.54 实测判据）
1. 每个库开库一行 `[rms] open ...`：**第一轮写过数据后，第二轮开库时 `data`/`live` 是否与写入时一致**。
   - 若一致、且没有 `[rms] salvage` ⇒ 持久化修好了（根因 1 成立）。
   - 若 `exists=1` 但 `next=1 live=0 data=0` 压着非空文件 ⇒ header 仍然没落盘，继续查 `commitWrite` 之后的 cache 路径。
2. 出现 `[rms] salvage (bad chain) name='X'` 时看 `X` 是谁——把 8464B 的 `FFFFFFFF` 和"书签库"对上号（空名库才是 `FFFFFFFF`）。
3. `SALVAGE skipped ... bytes preserved` 出现即说明扫描器仍有盲区，**但数据没有被销毁**，可以再迭代抢救器。
4. `max_id` 必须是正常量级（个位数/千位）；再出现 54307759 这种说明还有第二处垃圾来源。

### 遗留 / 未做
1. **`[pcsl] unlink blocked .../FFFFFFFF` 的调用方仍未定位**（候选：`rms.c:215 rmsdb_record_store_delete`、`fileCache.c`、`suitestore_task_manager.c`、`storageFile.c`、`components_storage_kni.c:173`）。若该路径是在"每次启动删库重建"，那它本身就是数据丢失的一环——v01.43 的"held 就拒删"保护了数据，但也可能让 MIDlet 的删除语义失败后走"当作新库"分支。**下一轮用 `name=` 面包屑对照 `unlink blocked` 的时序**。
2. `midp_file_cache_truncate` 的记账仍未与钳后 `size` 对齐（v01.53 观察项，非本次根因）。
3. `midp_file_cache` 仍是**单例且只缓存一个文件**：空名库的 db/idx 共用 `FFFFFFFF` 一条路径时，第二个句柄的 I/O 全部退化为非缓存路径，`cachedFileSize`/`cachedAvailableSpace` 与之无关——**长期项**，涉及共享 native 文件，改动要谨慎（最小 diff + 明确收益再动）。
4. v01.54 **尚未在设备/Vita3K 实测**；上面判据是设计预期。

## 2026-09-11 v01.53：UC 报"RMS 使用空间不足"——v01.52 抢救会接受垃圾块并把 data_size 写爆（phoneme-midp `RecordStoreImpl.java` + vita-port `vita_pcsl.c`）

### 用户报告（v01.52 实测）
> "uc 浏览器打开网页，提示 rms 使用空间不足。"

v01.52 解决了"第二次进 UC 记录/书签丢失"（抢救成功、数据回来了），但引入新症状：**空间永远报不足**。方向一致——数据回来了，但库的账目被写坏了。

### 根因定案（v01.52 的抢救缺少上界）
v01.52 之前那条日志是关键证据：
```
[rms] SALVAGE live=1 max_id=1615147117 old_data_size=0
```
`max_id=1615147117` 是**垃圾块头被当成活块收下**了（`0x6040602D` 之类的随机字节被解析成 `id`）。v01.52 的接受条件是 `id>0 && dataSize>=0 && size>0`，**对 `size`/`dataSize` 没有任何上界**，于是：
1. 致密重写按这个垃圾 `size` 推进 `writeOffset`（`writeOffset += size`，即使实读短了也照样加）；
2. header 重算 `data_size = writeOffset - 40` → **`getSize()` 变成天文数字**；
3. `getSizeAvailable()` = min(200000 − getSize(), storageFreeSpace) 的第一项变成**负数** → 钳到 0 → 每次 `addBlock()` 都抛 `RecordStoreFullException`；
4. 同时 `truncate(writeOffset)` 记下一个巨大的 logical clamp → 该 store 之后所有 size/read 都被钳到一个虚假大小。

**结论：不是磁盘真的满了，是 `data_size` 这一个 int 被垃圾块写爆，导致 200KB/每 suite 的额度恒为 0。** 这也解释了为什么 v01.52 之前"书签丢失"和 v01.53 "空间不足"会先后出现——同一处缺边界。

### v01.53 修改

**A. `phoneme-midp/src/rms/rms_api/reference/classes/com/sun/midp/rms/RecordStoreImpl.java`（抢救加边界 + 空间面包屑）**
1. 新增常量 `SALVAGE_MAX_BLOCK = RMSConfig.STORAGE_SUITE_LIMIT + BLOCK_HEADER_SIZE`（即 200008）、`SALVAGE_MAX_SPAN = 2 * RMSConfig.STORAGE_SUITE_LIMIT`、`PROBE_BYTE[1]`。
2. 新增 `blockFitsInFile(offset, size)`：`size<=0 || size>200008` 直接 false；否则 `seek(offset+size-1)` + `read(PROBE_BYTE,0,1)==1`（payload 末字节必须真实存在）。**垃圾 size 在第一道就被拒**。
3. `physicalFileHasLiveBlocks()` 改用 `blockFitsInFile()`（去掉局部 `byte[] one`）。
4. `salvageStore()` 扫描循环加跨度上界：`offset > DB_HEADER_SIZE + SALVAGE_MAX_SPAN` 即停（`scanEndOffset=offset; break;`），接受条件加 `blockFitsInFile(offset, size)`；日志补 `scan_end=`。
5. `getSizeAvailable()` 新增一次性命中面包屑（`spaceReported` 字段，只打一次）：
   ```
   [rms] space low area=<storageAreaId> fileSpace=<..> limitSpace=<..> data_size=<..> rv=<..>
   ```
   **三项一次打全**，"空间不足"到底是 per-suite 额度负数、还是磁盘 free space 归零，日志直接给答案，不用再猜。
6. 注意：`recordStoreName` 只是构造器**参数**、不是字段，面包屑里用 `RmsEnvironment.getStorageAreaId(suiteId)`（见下方构建陷阱）。

**B. `vita-port/src/vita_pcsl.c`（把"逻辑截断"补成真的物理截断 + 让 used space 认逻辑大小）**
1. 新增 `vita_logical_size_for(abs_path)`：在 `g_open_handles` 里找同路径、`logical_size >= 0` 的句柄，返回最小逻辑大小，否则 -1。
2. 新增 `vita_physical_truncate(abs_path, size)`:先 `truncate(path, size)`（newlib 路径版，Vita 上是 `sceIoChstat` 包装），失败再 `open(O_WRONLY)`+`ftruncate(fd)`+`close`；`size<0` 或 `app0:` 前缀直接返回 -1。
3. `pcsl_file_truncate()` 重写：
   - **先取 `cur_pos` 再 `SEEK_END`**（旧代码先 SEEK_END，"恢复位置"实际是 seek 回 EOF，是潜伏 bug）；
   - 已有 clamp 时取更小值（`size = min(size, logical_size)`），保证物理收缩永不落在给调用方看的大小之上；
   - `!vita_handles_held_ex(path, vf)` 时才真收缩（空名 store 的 db/idx 共用一个路径，**twint handle** 情况下收缩会剪掉对方还相信的数据）；成功后若物理 EOF `<= size` 就清掉 clamp。
4. `pcsl_file_close()`：关闭前取 `pending = logical_size`，`sceIoClose` 后若无人再持有该路径则 `vita_physical_truncate(path, pending)`——**clamp 在关文件时落地**，不留"逻辑小了但磁盘没回收"的尾巴。
5. `pcsl_file_getusedspace()`：逐目录项拼 `abs_dir + "/" + d_name`，用 `vita_logical_size_for()` 取逻辑大小，按 `(logical>=0 && logical<physical) ? logical : physical` 计入；并加一次性诊断 `[pcsl] getusedspace <dir> = <N> bytes`（4 槽去重、`total>1MB` 才打）。**这是"物理 st_size 被当成 used space"的直接修复**：bloated 的物理文件不再把 free space 压到 0。
6. `vita_handles_held_ex()` 去掉 `__attribute__((unused))`（重新被 `pcsl_file_truncate` 使用）。

**C. `vita-port/CMakeLists.txt`**：`VITA_VERSION` 01.52 → 01.53。

### 构建陷阱（本轮新增，务必记住）
1. **javac 失败会静默跳过 ROM 重生成，而 `build.sh` 仍然打印 "Build successful"**：`./build.sh midp 2>&1 | grep ...` 里 `make` 的失败在管道中被吞掉。第一次改完 `RecordStoreImpl.java` 引用了不存在的 `recordStoreName`，`javac` 报 `cannot find symbol` → `classes.zip` / `ROMImage_05.cpp` / `ROMImage.o` / `libobj.a` **全部停在旧时间戳**（11:46），而 VPK 照样产出、脚本照样说成功。**改 Java 后必须用时间戳链证明 ROM 真的重生成**（见下）。
2. **判断"Java 改动进产物没有"的时间戳链**：`build/vita_arm/classes/com/sun/midp/rms/RecordStoreImpl.class` → `classes.zip` → `ROMImage_05.cpp` → `obj/arm/ROMImage.o` → `obj/arm/libobj.a` → `vita-port/build/cmake/midp_vita`，必须**逐级晚于源文件**。本次实测：源 16:32 → class/classes.zip 16:52 → ROMImage_05.cpp/ROMImage.o/libobj.a 16:53 → ELF 16:53。日志中可见 `... searching updated .java files` / `... compiling 1 .java files` / `romgen -romconfig ... -romize`。
3. **字符串验证一分为二**（v01.52 定的 UTF-16LE 规则只对 Java 串成立）：Java 串（`ROM_BL`，UTF-16LE）用 `data.count(s.encode('utf-16-le'))`；C 串（`vita_pcsl.c`）用 `data.count(s.encode())`。本次实测：`[rms] space low area=` utf16le=1 / `scan_end=` utf16le=1 / `[pcsl] getusedspace ` ascii=1。`.self` 是压缩的，查串只能查 `build/cmake/midp_vita`。
4. **Vita newlib 的 truncate 家族（用 objdump 反汇编 `lib_a-truncate.o` 定案）**：`sceIoTruncate` 不存在；`truncate(path,size)` = `sceIoChstat(path, SceIoStat{st_size})` 纯路径版；`ftruncate(fd,size)` 走 `__vita_fd_grab(fd)`，而该函数只认 newlib 自己 fd 表里的 **1..255**，**`sceIoOpen` 拿到的 SceUID 不能直接喂给 `ftruncate`**。⇒ 路径版 `truncate()` 是唯一可靠路线，`ftruncate` 只作为 `open()` 得来句柄的备选。
5. **`midp_file_cache` 是全局单例、且只缓存第一个文件**（`midp_file_cache_truncate` 里 `cachedAvailableSpace += cachedFileSize - size` 用的是调用方给的 `size`，不是 `pcsl_file_truncate` 实际生效的钳后值）。**本轮未改**：实测该差额只在"物理收缩失败（twin handle）"时才出现，方向是少记可用空间（保守），不足以解释本次症状；留作观察项，不再无凭据地动它。

### 实测预期（v01.53）
- 打开 UC 时 stdout 出现 `[rms] space low area=0 fileSpace=<N> limitSpace=<M> data_size=<K> rv=<R>`——**看这三项谁是 0/负数即可定案**：
  - `limitSpace` 为负/0 且 `data_size` 巨大 ⇒ 库头被写爆（v01.52 的垃圾块），需要一次抢救把 `data_size` 收回真实值；
  - `fileSpace` 为 0 ⇒ 磁盘账目问题，看同一轮的 `[pcsl] getusedspace <dir> = <N> bytes` 是否仍然偏大。
- 坏库首次开库：`[rms] SALVAGE live=N max_id=M old_data_size=K scan_end=E`，`max_id` 应当是**正常量级**（不再出现 1615147117 这种）。
- 正常库：无 `[rms]` 输出。

### 遗留 / 未做
1. **`midp_file_cache_truncate` 的记账未与钳后的 `size` 对齐**（见构建陷阱 5）——已评估，非本次根因，未改。
2. **`[pcsl] unlink blocked, file open elsewhere: .../rms/appdb_1A35/FFFFFFFF` 的调用方仍未定位**（空名 store 的 db/idx 共用 `FFFFFFFF` 路径）。候选：`rms.c:215 rmsdb_record_store_delete`、`fileCache.c:84/259`、`suitestore_task_manager.c:452`、`storageFile.c:183`、`components_storage_kni.c:173`。
3. **v01.53 尚未在设备/Vita3K 上实测**，上面"实测预期"是设计预期，不是观察结果。
4. 顺手清理：`vita-port/build/cmake/midp_vita.vpk.out.0nUsn9`（v01.52 jar 自吞噬事故留下的 72GB 临时文件）已删除；`samples/j2me/09:20，理论上用户装的可能是`（误重定向产生的 63B 垃圾文件，09-09 09:58）仍在，待确认后删。

## 2026-09-11 v01.52：v01.51 的"坏链重置"被证实抹掉用户数据——改为抢救（salvage）策略

### 用户报告（v01.51 实测）
> "还是存在问题，第二次进 uc 记录和书签都没了，和首次进入一样。"

用户附的 `FFFFFFFF` hexdump 是决定性证据：`sig=midp-rms` / `next_id=1` / `num_live=0` / **`version=1`** / `data_size=0` / `free_size=0`，**物理文件 5856 字节**、偏移 40 起还有 `id=-1 size=168` 的自由块。`version=1` 只可能由 v01.51 重置块写入（`else` 初始化分支不写 version）⇒ **v01.51 的自愈在含 5856 字节活数据的库上触发了，把库抹成空**。

### 根因定案（为什么"简单写入/seek 搞不好"）
1. **`midp_file_cache` 的写缓存在内存**：小的 header 写（`data_size`/`num_live` 等 4~20 字节）走缓存，块数据大写直通磁盘。UC 退出若未走 `closeRecordStore()`（或进程被杀），磁盘留下"块数据新、header 旧"的错位文件——这就是链校验检测到的不一致来源。**块本身几乎总是完好的，坏的只是 header 的口径。**
2. **v01.51 的应对（重置为空）方向性错误**：把 5856 字节的完好数据当成垃圾扔掉。正确策略是**按物理内容抢救**。
3. `[pcsl] unlink blocked ... FFFFFFFF` 反复出现是框架清理循环（`rmsdb_remove_record_stores_for_suite` / `quietDeleteFile`）对空名 store 的删除尝试被预检挡住，**无害**（失败即保留活数据）。

### v01.52 修改（`RecordStoreImpl.java` 单文件，+1 新方法群替换 2 个重置块）
1. **`salvageStore(byte[] dbHeaderData)`（新）**：
   - 扫描到**物理 EOF**（不信 header 的 data_size——它就是谎言来源；被 v01.51 抹过的库 data_size=0 但记录物理还在，照样能找回）；
   - 坏块头按 8 字节滑步重同步；`id>0 && size>0` 收进活块表（上限 512）；
   - **按 id 去重**（陈旧尾巴里是旧副本；compact 把数据下移 ⇒ 最低偏移处是最新副本，取第一个）；
   - 活块**致密重写**到 `[DB_HEADER_SIZE, ...)`（边读边写、`n<=0` 即断尾丢弃该块及之后）；
   - header 按实际保留内容重算：`next_id = max(旧next_id, maxId+1)`（不复用已删 id，符合 RMS 规范）、`num_live=kept`、`version+1`、`data_size=writeOffset-40`、`free_size=0`；
   - **`commitWrite()` 后再 `truncate(writeOffset)`**（header 是 40 字节小写，会留在 midp_file_cache 里！必须先 flush 再钳制尾巴）。
2. **`physicalFileHasLiveBlocks()`（新探针）**：header 声称空库（`num_live==0`）但物理文件里存在"payload 末字节可读"的活块 ⇒ 抢救。payload 校验防止垃圾头（声称越界 size）每次开库空触发。**用户当前这台设备上被 v01.51 抹掉的书签，装上 v01.52 后首次启动即被此探针救回。**
3. **`isBlockChainConsistent()` 增强**：除原有检查外，交叉核对**扫描到的活块数 == header 的 num_live**（捕获"header 写丢失但块完好"的形态）。
4. 构造器两个重置块 → 改调 `salvageStore()`；面包屑统一 `[rms] salvage (...)` / `[rms] SALVAGE live=N max_id=M old_data_size=K`。
5. `compactRecords()` 的坏链早退（v01.51）保留不变。

### 实测预期（v01.52）
- **本机已被 v01.51 抹掉的书签**：首启 v01.52 时 stdout 出现 `[rms] salvage (empty header, live blocks present)` + `[rms] SALVAGE live=N ...`（N>0），书签应恢复。
- 正常库：无 `[rms]` 输出（探针在第一个活块处即终止，代价可忽略）。
- 若 `SALVAGE live=0` 且数据确实没了：行为等同 v01.51（空库重建），不会再更糟。

### 构建事实（沿袭 v01.51 定案，v01.52 修正两条）
1. ~~ROM 输入是 `classes.zip`~~（正确，保留）；~~`strings | grep` 可验证字符串常量~~（**v01.52 修正：错误**）。
2. **romgen 把字符串常量编码为 UTF-16LE 的 `ROM_BL` 宏打包**（见 `ROMImage_00.cpp` 的 `_rom_text_block0`），**不是明文 ASCII** ⇒ `strings <ELF> | grep '<面包屑>'` 必然为 0，**不能**当"改动没进产物"的证据。正确验证方式：`python3 -c "open('midp_vita','rb').read().count('<字符串>'.encode('utf-16-le'))"`。
3. `Logging.*` 编译期被剥（留痕只能 `System.out`）——仍然正确。
4. ~~CMake 的 jar 依赖 glob `*.class`~~ —— 方向正确但**激活了潜伏的 jar 自吞噬**（见第 5 条）。
5. **`MIDP_SYSTEM_JAR` 输出路径必须在 `classes/` 目录之外**：历史上它是 `${MIDP_BUILD}/classes/midp_system.jar` 而打包命令是 `jar cf <out> -C classes .`——输出在被打包目录内。旧 `DEPENDS` 盯目录 mtime 几乎不重跑（掩盖了 bug）；v01.51 glob `*.class` 让它**每次重跑**，每跑一次把上一代 jar 塞进自己 → 指数膨胀（实测 1.2MB → 138GB，`classes.zip` 又把怪兽打包 → 30GB）。**v01.52 已改为 `${MIDP_BUILD}/midp_system.jar`**。
6. `libmidp.so` 的 `_rom_linkcheck_mffd_false` 报错无害（仍然正确）。
7. 判"改动是否进产物"：时间戳单调链 + `javap -classpath classes.zip` + **UTF-16LE 字节串搜索**（第 2 条）。

## 2026-09-11 v01.51：块链损坏定位 + Java 侧面包屑 + ROM 出货链取证（phoneme-midp / vita-port）

### 用户报告（v01.50 实测）
> "删除 rms，第一次进正常，保存书签，第二次进卡在初始化，进不去。"

首启（空库）正常、存书签后二启卡死 ⇒ 差异只在**落盘的 RMS 内容**，与 v01.50 的"数据丢失"不同：这次头部合法，坏的是**数据区的块链**。

### 定案：v01.50 的守卫维度不够
- v01.49 挡 `calculateBlockSize <= 0`（块循环），v01.50 挡头部字段不可能值（data_size < 0 等），**都看不到"头部合法但数据区字节不是合法块头"**。
- 数据区出现非法块头（Vita3K 丢 O_TRUNC + `pcsl_file_truncate` 只是随句柄消失的逻辑钳制 ⇒ 陈旧尾巴留在 live 区），任何一次行走（`getRecordIDs` / `getRecordHeader_SearchFromTo` / `getFreeBlock`）都会跳过数据区末尾 ⇒ 找不到任何记录 ⇒ MIDlet 恢复状态时永远等不到数据 = "卡在初始化"。
- 更糟：CLOSE 时 `compactRecords()` 按"遇到的每个 8 字节头"搬数据并改写 data_size，遇到假头就搬垃圾并把 data_size 压小 ⇒ 首次启动还读得出的库被压成坏库。

### v01.51 修改
1. **`RecordStoreImpl.isBlockChainConsistent(byte[])`（新增，只读）**：按 `calculateBlockSize(getInt(header,4))` 走 `[DB_HEADER_SIZE, data_size+DB_HEADER_SIZE)`，每块必须 `size > 0` 且 `size <= 剩余`，最终必须**正好落在 dbSize**。只读，不写盘，只动文件位置。
2. **`compactRecords()`**：链不一致 → 直接 return（绝不基于坏链搬数据/改 data_size）。
3. **打开自愈（构造器 `if (exists)` 分支）**：链不一致 → 整店重置为 pristine 空店（next_id=1 / num_live=0 / version=1 / last_modified=now / data_size=0 / free_size=0）＝ 等价于用户手动"删 rms"，这是唯一验证过能让 UC 再启动的操作。
4. **`RecordStoreIndex.getFreeBlock()`**：块越界（`currentSize > size - offset`）→ 打印 + `return 0`（追加到末尾），绝不返回越界偏移覆盖活数据。
5. **`vita-port/src/vita_pcsl.c` `pcsl_file_seek()` 去掉位置钳制**：`storagePosition()` 只把 `-1` 当错误，v01.47 的"钳制到 logical_size 的正数返回值"会被当成成功 ⇒ file cache 位置与句柄位置静默分裂。
6. **Java 侧面包屑（本轮新增，见下）**：`System.out.println("[rms] ...")`。

### 本轮踩到并定案的构建事实（比修复本身更值钱）
1. **ROM 的输入是 `classes.zip`，不是 `midp_system.jar`**：`Defs.gmk:114  MIDP_CLASSES_ZIP ?= $(MIDP_OUTPUT_DIR)/classes.zip`，romgen 规则 `cldc_vm.gmk:227  $(MIDP_OUTPUT_DIR)/ROMImage.cpp: $(MIDP_CLASSES_ZIP) ...`。`midp_system.jar` 只是被塞进 VPK 的数据副本（UseROM 下运行时不用它）。
2. **romgen 去掉方法名**（ROMGEN_ARGS 含 `+EnableAllROMOptimizations`）⇒ 在 ROM/ELF 里 `strings | grep <方法名>` **必然为 0**，不能当"改动没进产物"的证据（本轮据此误判过一次，白白折腾）。可用的标记只能是**字符串常量**（`System.out.println` 的字面量一定进 ROM）。
3. **`Logging.REPORT_LEVEL` 是编译期常量**：release ROM 里 `if (Logging.REPORT_LEVEL <= Logging.WARNING) { ... }` **整块被 javac 删除**（`javap -v classes.zip` 里搜不到该字符串，实测为 0）⇒ Java 侧若想留下痕迹，**只能用 `System.out`**（tty → `JVMSPI_PrintRaw` → stdout/`vm_output.log`）。现有 phoneME 的 `Logging.report` 全部是 no-op。
4. **CMake 盯目录的依赖不可靠**：`add_custom_command(DEPENDS ${MIDP_BUILD}/classes)` —— javac **覆盖**已存在的 `.class` 不改目录 mtime ⇒ `midp_system.jar` 一直停留在 04:36 的旧货，v01.51 的类一度没进 VPK。已改为 `file(GLOB_RECURSE ... ${MIDP_BUILD}/classes/*.class)` + 资源文件作 `DEPENDS`。
5. **`libmidp.so` 报 `undefined reference to '_rom_linkcheck_mffd_false'` 是无害的**：VM 库（`libcldc_vm.a` 的 `_MergedSrc005.o`/`Interpreter_arm.o`）期望 `..._mffd_false`，而 romgen 生成的是 `..._mffd_true`，`vita-port/src/vm_rom_stubs.c` 提供前者补齐；最终 ELF 用 `libobj_no_main.a`（含 `ROMImage.o`）链接，两者共存故能解析。**该错误发生在 ROM 重生成/`libobj.a` 归档之后**，所以 `./build.sh midp` 会在这一步 make 失败但产物齐全，脚本仍打印 "Build successful"（管道吞了非零退出码）。
6. **判"改动是否进产物"的正确姿势**：时间戳单调链 `*.java → classes/*.class → classes.zip → ROMImage.cpp → ROMImage.o → libobj.a → libobj_no_main.a → midp_vita → midp_vita.vpk` + `javap -classpath classes.zip <类>` 看方法是否存在 + `strings <ELF> | grep <字符串常量>`。

### 实测预期（v01.51）
- 首启（空库）：无 `[rms]` 输出，正常。
- 存书签 → 二启：若库坏 ⇒ `midp_stdout.log`/`vm_output.log` 出现 `[rms] RESET store (bad chain) ...`，UC **应能启动**（库被重置为空，书签丢失但不再卡死）。
- 若二启**仍然卡初始化**且**没有** `[rms]` 输出 ⇒ 说明卡死与 RMS 块链无关，需要另抓 `midp_stderr.log` + `FFFFFFFF` 头 64B 重新定位。

## 2026-09-11 v01.50：RMS 存储砖死定案与自愈——data_size 负值头部被写盘（phoneme-midp 提交 a3fee44、vita-port 提交 60a2947）

### 用户报告（v01.49 实测）
1. UC 记录（历史/书签）没保存或无法恢复，第二次进和第一次一样。
2. 书签卡顿应该与 IO 无关，可能是网页加载中造成的。

### 法证闭环（用户提供 FFFFFFFF 文件 hexdump 3104B + midp_stderr.log + vita3k.log）
头部逐字段解析（布局见 AbstractRecordStoreImpl.java:48-87）：
- sig `midp-rms` ✅ / next_id=2 / **num_live=0**（零存活记录=用户看到"全丢"）/ version=4 / last_modified 2026-09 合法 / **data_size=0xFFFFFED8=-296** ❌ / free_size=0
- 偏移40 块头：id=-1(空闲), size=0xB38=2880 → 算术铁证：**compact 前 data_size=2584，2584-2880=-296 分毫不差**
- 第二次启动 `getSize()=40+(-296)=-256` = stderr 里 `[pcsl] seek with negative offset -256`（v01.49 拦截生效：仅 1 条、无崩溃、三次启动 MAIN_EXIT=2001 干净退出、23 秒空转消失）
- addBlock 走 `blockOffset=getSize()=-256` → 保存全部静默失败 → 报告#1 完全解释

### 坏文件产生机制（回答"每次装新版都删 rms 为什么还会坏"）
删 rms 是对的——坏文件不是旧版带入，是**本次会话运行时自己写坏的**：
1. Vita3K 丢 `O_TRUNC`（v01.41 实锤）→ compact 的 truncate 从不物理收缩 → 陈旧尾巴永存
2. 空白名 store 的 db/idx **共用一个文件**（buildSuiteFilename: nameLen>0 才加 .db/.idx 后缀）→ 互相踩
3. compactRecords 走块把垃圾字节当成 2880B 空闲块（`currentId<0 && currentSize>0`，v01.49 的 `<=0` 守卫拦不住）
4. 走完**无条件**执行 `data_size -= moveUpNumBytes` 并写回头部 → -296 上盘 → store 永久砖死
5. 用户每次删 rms 后 UC 一保存书签又确定性复现同一链条

### 修复（双守卫，phoneme-midp a3fee44）
- **compactRecords 写回前**：`compactedSize < 0` → 直接 return 放弃本次 compact（绝不写不可能的头部）；下次打开由开盖自愈兜底
- **openRecordStore(exists 路径)**：next_id<1 / num_live<0 / data_size<0 / free_size<0 任一命中 → 重置为干净空库（putInt 1/0/1/0 + 写回 DB_HEADER_SIZE）→ 曾砖死的文件复活，MIDlet 恢复保存能力；尾巴垃圾留在盘上但 data_size=0 不可见，首次 append 即覆盖

### 构建（Java 变更 → ROM 重生成链再次验证）
- `./build_vita.sh` EXIT=0，RecordStoreImpl.class 10490B 与 libobj.a 同批 04:24；`libmidp.so` _rom_linkcheck 失败照旧无害
- vita-port：需 `export PATH=/home/zyb/tools/jdk8u502-b07/bin:$PATH`（javac）+ `rm build/cmake/vita_version.h` 破版本缓存
- 产物 `midp_vita.vpk`，`strings` 验证 `v01.50 b209`

### 实测预期
- 第一次启动 UC：开盖自愈把 -296 头部重置为空库（日志可观察 seek -256 不再出现）
- 保存书签/历史后退出重进：**数据应存活**（compact 守卫阻断 -296 再产生）
- 若仍复现：抓 `rms/appdb_1A35/FFFFFFFF` hexdump 头 64B + midp_stderr.log，重点看 data_size 是否再次为负（若为负说明还有第三条写坏路径）

### 教训
- "打开时校验"防不了"运行时写坏"——**写回前的变量必须与写盘值同源校验**（本例 compactedSize 先算后判，而不是先 putInt 再读回判断）
- RMS 砖死的三要素齐了：物理 truncate 缺失（模拟器）+ 路径共用（空白名 store）+ 无守卫的减法（上游代码假设存储栈可靠）
- v01.49 守卫只挡 `<=0` 的**块循环**，挡不住 `>0` 的**幻影空闲块**——守卫要覆盖"值合法但内容是垃圾"的情况，只能靠算术不变量（data_size ≥ 0）

### 遗留
- 幻影 2880B 空闲块的精确写入者未定位（0x6D393001 掩码在代码中无来源，疑为 db/idx 同文件互踩的 idx 写入）；开盖自愈使其失去危害性，观察优先
- 报告#2（书签卡顿疑为网页加载）：本次 stderr 全程无 seek 风暴无空转，v01.49 已解决 IO 侧；网页加载卡顿属 UC 自身单线程解释器性能范畴，暂不动

## 2026-09-10 v01.49：UC 书签保存 23 秒无响应 + PC=0x0 崩溃——calculateBlockSize 负块大小无界循环（phoneme-midp 提交 5eea6a1、vita-port 提交 508ae93）

### 用户报告（v01.48 实测）
- UC 浏览器保存书签时界面很卡（约 23 秒无响应），随后退出；再次启动 UC 后 Vita3K 崩溃刷屏（EXCEPTION_ACCESS_VIOLATION PC=0x85f05fd8 → PC=0x0 循环）。
- **用户已先删除 `ux0:/data/J2ME00001/rms/appdb_1A35` 再测**（日志 `Creating new dir rms/appdb_1A35` 佐证）——排除存量脏数据假设，根因在代码路径自身。

### 日志全量定位（vita3k.log，642,731 行二分）
- 10:58:54 新建 rms/appdb_1A35 → 10:59:58 书签写入本身很快（fd 0x11D 写 2863B，~2ms，**保存动作不卡**）。
- 10:59:58 起 fd 0x12E 反复 open→`seek_file 0x80010051` 失败（同 fd write 8B 正常 → fd 是好的，是偏移量错）。
- 11:00:13.6→11:00:36.9 **23 秒无 IO 空窗** = 用户感知的"很卡"；11:00:37 UC 退出。
- 11:01:16 第二次启动 UC，boot 部署阶段 11:01:16.302 首异常（**崩溃发生在第二次启动，非保存时**）。

### 根因机制（本轮定案）
- `RecordStoreUtil.calculateBlockSize`：remainder==0 时返回 `dataSize+8`，否则 `dataSize+(8-remainder)+8`。垃圾 dataSize 为 8 的负倍数（如 -16）→ 返回 -8；dataSize=-8 → 返回 0。
- 块大小 ≤0 → `currentOffset` 不前进/倒退 → `compactRecords`/`getRecordIDs`/`getRecordHeader_SearchFromTo`/`getFreeBlock` 块循环无界空转（23 秒）→ 算出垃圾 offset → seek 失败链 → 进程状态破坏。
- Vita3K 对垃圾/负偏移 `sceIoLseek` 报 0x80010051 且**文件位置保持不变**，后续操作在陈旧偏移上进行，进一步放大损坏。

### 修复（phoneme-midp 5eea6a1 + vita-port 508ae93）
- **Java 4 处守卫**（守卫失败契约与坏 header 读一致）：
  - `RecordStoreImpl.compactRecords`：`currentSize <= 0` → throw IOException
  - `RecordStoreIndex.getRecordIDs`：break
  - `RecordStoreIndex.getRecordHeader_SearchFromTo`：return INVALID_OFFSET
  - `RecordStoreIndex.getFreeBlock`：throw IOException
- **C 层拦截**（`vita-port/src/vita_pcsl.c`）：`pcsl_file_seek` 对 `offset < 0` 记一次 stderr（去重）并返回 -1，借 storagePosition 既有 -1→getLastError 链路上抛到 Java。
- 版本 `CMakeLists.txt` 01.48→01.49。

### ROM 重生成链路（Java 系统类改动必须走全，否则白改）
- phoneme-midp 的 `ROMImage.cpp: $(MIDP_CLASSES_ZIP) $(ROMGEN_CMD) rom.config` 规则（`build/common/makefiles/cldc_vm.gmk:227`）会自动级联：改 .java（比 classes.zip 新）→ 重编该类 → 更新 classes.zip → `romgen -romize`（phoneme-cldc/build/vita_arm/dist/bin/romgen，32 位 x86 宿主工具）→ 重新生成 ROMImage_*.cpp → 重编 ROMImage.o 进 libobj.a。
- **增量构建即可**，无需 rm classes.zip；验证法：`classes/` 下改动类 .class 时间戳 == 本次构建时间 + `grep "RecordStoreImpl.java" alljavalist.txt` 命中。
- `libmidp.so` 链接失败（`_rom_linkcheck_mffd_false`）已知无害，VPK 走 libobj.a。
- vita-port 构建需 JDK：`export PATH=/home/zyb/tools/jdk8u502-b07/bin:$PATH`（build_jar.sh 要 javac；MIDP 构建本身已带）。
- 版本缓存陷阱复现：v01.49 时 `vita_version.h` 仍是旧值，需 `rm build/cmake/vita_version.h` 再 cmake；验证 `strings midp_vita | grep "version: J2ME"`。

### 验证
- MIDP 构建 EXIT=0，ROM 15.43s 重新生成（20,195 对象），RecordStoreIndex/Impl .class 均为本次构建产物，libobj.a/ROMImage.o 15:41 同批。
- VPK：`vita-port/build/cmake/midp_vita.vpk`（14.4MB，15:53），`strings midp_vita` 确认 `v01.49 b207`。
- 待用户实测：删 appdb_1A35 后 UC 保存书签应快速返回（守卫触发时该次保存抛 IOException 而非卡死）；二次启动不再崩溃。

### 遗留（观察项）
- 崩溃 PC=0x85f05fd8 落在堆内 JIT 区与 `-int` 纯解释器矛盾——推测进程级状态污染延续或 LR 巧合；Java 守卫落地后大概率消失，若复现需单独排查。

## 2026-09-09 v01.43：消除残留的「占用」日志三连——unlink 前查注册表跳过注定失败的 remove（vita-port 提交 547a471）

### 用户报告（v01.42 实测）

- "文件占用问题还是存在"。实测 VPK 已是 b192（stderr 横幅 `v01.42 b192 (503cd4b)` + `[pcsl] unlink blocked, file open elsewhere: ux0:/data/J2ME00001/rms/appdb_1A35/FFFFFFFF` 面包屑生效），UC 全程正常（联网、vmStatus=2001 干净退出）。
- 注意：用户附的 vita3k.log 是旧文件（Downloads\windows-latest 遗留副本：17:49→00:02 那次 6 小时会话、Version: 1.28、debug_log.txt 写入全是 b188 特征），不要拿它对照新构建。

### 现状定论

- 唯一残留的"占用" = `rms/appdb_1A35/FFFFFFFF*`（internal suite -1 的活 RMS 数据）被 phoneME 的 suite 清理逻辑 unlink，而 UC 自己的句柄还开着 → Windows/Vita3K 无 FILE_SHARE_DELETE 必失败（Error 32 三行噪音，v01.36 已实锤）。
- v01.37 的"如实报失败"在功能上是对的（数据活着、UC 正常），代价是每轮三行装饰性日志。

### 修复（v01.43）

- `pcsl_file_unlink` 在调 sceIoRemove **之前**先查 `g_open_handles` 注册表（新 `vita_handles_held()`）：本进程有同路径活句柄 → 直接跳过注定失败的 syscall，`vita_report_held` 打一条面包屑后返回 -1。
- 语义与 v01.37 完全一致（调用方拿到同样的失败、文件照样保住），只是 Vita3K 再也看不到这次删除——三行噪音从源头消失。
- 注册表由 open/close 维护，路径是 resolve 后的绝对路径，与 unlink 的 abs_path 同源，不会误判。

### 验证

- v01.43 b194 (547a471) 构建成功。预期：下一轮日志中 `[pcsl] unlink blocked` 面包屑仍在（每路径一条），Vita3K 日志中 `Cannot remove file / Error code: 32 / io_error_impl` 三连消失。
- 若用户仍报"占用"：需要用户提供新的 vita3k.log 确认三连是否消失，以及面包屑出现次数。

## 2026-09-09 v01.42：6 小时长会话 Vita3K 闪退——菜单循环累积内存泄漏（vita-port 提交 503cd4b）

### 用户报告（b188 实测）

- "vita3k 直接闪退了"。时间线：17:49 启动 Vita3K → 安装 VPK → UC **完整正常一轮**（联网 OK、vmStatus=2001 MAIN_EXIT 干净退出、回菜单）→ 用户构建为 04:30 的 b188（v01.41 核心但**无 v01.41a fd 泄漏修复**）→ vita3k.log 尾部停在 [00:02:20.513] input_debug.log 打开（约 6 小时 13 分后），闪退。

### 根因排查（坐实）

1. **`menu_fb` 每轮 2MB 泄漏**（vita_menu.c:1380）：`vita_menu_run` 每次进菜单 `memalign(2MB)` 直接覆盖 static 指针，旧块永不释放。注释声称"每进程 2MB 可接受"——但代码是**每轮**执行，菜单↔游戏切换 N 轮 = N×2MB。修复：`if (menu_fb == NULL)` 才分配，进程生命周期复用。
2. **icon 纹理覆盖泄漏**：`icon_cache_restore_kept()` 把上一轮的解码纹理放回 `icon_pix[]` 槽位后，`scan_games()` 的 cache-hit/miss 两条路径的 `vita_icon_decode_png(&icon_pix[idx],...)` 直接覆盖指针不 free——每游戏每轮漏一张 icon（~16-64KB）。修复：decode 前 free 旧槽位。
3. **次要结论**：b188 无 v01.41a 修复，O_TRUNC 恢复路径的 fd 泄漏（open 成功后 free 未 close）会在多轮后累积孤儿 FILE*——Error 32 与资源消耗的放大器。v01.41a（8a4db28）已修。
4. **debug_log.txt 之谜定论**：用户 b188 日志有 debug_log.txt 写入（open/write/close 每次重开模式）但 git 历史中 b188/b189/b190 的 `VITA_FILE_DEBUG` 都是 0——04:30 构建发生在 1204cac 提交前，构建时工作区临时开着 `=1`，提交时改回 0。**构建产物与提交内容可能不一致，横幅只保证 HEAD，不保证工作区干净**。教训：构建前 `git status` 确认干净。
5. **image_wash 双重前缀疑云排除**：摘要中的 "app0:ux0:/..." 证据实为 09-05 v01.32 时代的老分析残留；b188 的 `pcsl_file_exist` 已正确做前缀交换（本轮日志唯一 Missing file 是 vm_boot.log 探测，正常）。
6. **vita_stubs.c 旧双实现确认不碍事**：`phoneme-midp/src/vita_stubs.c` 是旧 pcsl 实现（无条件 debug_log、path[256]、`app0:%s` 裸拼接），其 .o 在 phoneme-midp 构建树存在但**不在任何链接的 .a 里**（libobj.a/libpcsl_file.a 均不含）；当前 velf 的 pcsl 符号单一定义且为新代码。

### 附带修复

- **build.sh pipefail**：`make | tail -20 || {...}` 的退出码来自 tail——make 失败也打印 "Build successful"（05:16 事故根因）。已加 `set -o pipefail`。

### 验证与遗留

- v01.42 b192 构建成功，版本横幅与 HEAD 一致；待用户长会话复测（菜单↔游戏多轮 + 挂数小时）。
- 闪退若仍复现：向用户要 vita3k.log 被省略段（592-18790 行）的尾部 ~50 行 + 是否在 [00:02] 启动了第二轮游戏。
- 6 小时菜单空转的 Vita3K 侧资源累积（host 侧）仍是候选——若 b192 仍闪退且时间点与游戏轮次无关，转向模拟器本体。

## 2026-09-09 v01.41：I/O 适配层系统性重审——Vita3K O_TRUNC 盲区是占用/EBADFD/慢的共同根因

### 用户反馈（v01.40 实测无效）

- 占用（Error 32）仍在、seek EBADFD 警告仍在——path 600 + truncate 兜底没治本。用户要求"好好分析下 io 是不是没适配好"。

### 系统性对照结论（上游 pcsl posix/win32 vs 我们的 vita_pcsl.c vs Vita3K HLE 源码）

逐函数读完三层后的**决定性发现**：

1. **Vita3K `translate_open_mode()`（io/src/filesystem.cpp）没有 SCE_O_TRUNC 分支**——所有 open 都映射 `"rb+"`（master 与历史提交 345d807488 确认一致，用户 v0.2.1 同样中招）。`O_TRUNC` 被**静默丢弃**。
2. 因此 v01.40 的 truncate（close→reopen(O_TRUNC)→写回）在 Vita3K 上**从未真正截断过**：保存的前缀覆盖了文件头部，**旧尾巴永久残留**；RMS compact 报告成功但垃圾记录头活在新逻辑尺寸之后 → 二启读损坏数据/慢；`pcsl_file_sizeofopenfile` 一直返回旧尺寸。**真机上 O_TRUNC 是生效的——这是模拟器特有的静默数据损坏**，也是 v01.40 日志分析看不见它的原因。
3. Vita3K `close_file` 是同步 `std_files.erase(fd)`（shared_ptr<FILE> 即时 fclose）——排除"句柄延迟释放"假说；Error 32 = remove 时**确有另一个 Vita3K FILE\* 仍开着**（`_wfopen` 无 FILE_SHARE_DELETE，Windows 下进程内任何打开句柄都阻止删除）。我们的 truncate close→reopen 窗口 + `OPEN_READ_WRITE_TRUNCATE` 路径都是制造这种窗口的源头。
4. `FileStats` 自带 `truncate()`（`_chsize_s`）且 io.cpp 有 `truncate_file()`，但**未挂到任何 sceIo 导出**（SceLibKernel 导出表无 sceIoTruncate）——真机也无此 syscall，不能依赖。
5. 上游 win32 pcsl：truncate=`_chsize(fd)`（不动 fd）、close 先 commitwrite——语义参考；Vita 无对应 syscall，只能重建文件。
6. RMS 栈事实（排除项）：`storage_open` 返回 `(int)handle`（指针截断，ARM32 无损，仅日志噪音）；`ENABLE_RECORDSTORE_FILE_LOCK=1` 已启用；Java 侧 `RecordStoreFile.close` 有 `handle=-1` 防护（396-401 行）；`storageCleanup` finalize 依赖该字段防 double-close——Java 层句柄生命周期健康，问题全在 pcsl 语义层。

### v01.41 修复（vita-port 提交 1204cac）

- `pcsl_file_truncate`：close→**remove→create(O_RDWR|O_CREAT)→校验新文件为 0 字节→写回**。校验失败（remove 被共享冲突挡掉）就诚实报错，不再静默损坏。真机/模拟器行为一致（remove+create 在两边都真正清空）。
- `pcsl_file_open`：带 `SCE_O_TRUNC` 打开成功且文件非空时，同样 close→remove→create 仿真（`storage_open` 的 `OPEN_READ_WRITE_TRUNCATE` 路径，suitestore 临时文件复用场景）。
- `pcsl_file_seek/read/write/sizeofopenfile`：fd<0 快速失败 + 一次性 stderr 面包屑（`[pcsl] seek on dead handle '<path>'`）——EBADFD 系统调用噪音在源头掐断，且下次复现能直接定位肇事路径。

### 预期与验证点（等用户实测）

- 二启不再卡数据初始化（RMS compact 真正生效，垃圾尾巴没了）。
- seek EBADFD 警告应消失或变成一条可定位的 stderr 面包屑。
- 占用（Error 32）应大幅减少：truncate 不再有 O_TRUNC reopen 窗口；若仍有，剩下的就是 midp_file_cache flush 时序，届时再查。
- 慢：本地缓存恢复后 UC 应明显变快（不再每次全量重下）。

### 教训（补充进方法论）

1. **模拟器的 syscall 语义要用它的源码验证，不能按 POSIX 直觉假设**——`translate_open_mode` 一共 20 行，读了就绝不会写出依赖 O_TRUNC 的代码。
2. "修了报错但没好"时，下一步不是再修报错，而是**找没报错的静默失败**（O_TRUNC 被丢弃时不报任何错）。
3. 跨平台文件操作的正确性锚点是"**这个序列在两个平台都产生相同的磁盘状态**"，不是"这个调用在 POSIX 语义下正确"。

## 2026-09-09 v01.40：EBADFD(0x80010051) seek 风暴 = 句柄路径截断；真机 0x8010113D 仍开放

### v01.39 实测结论（用户确认）

- ✅ 不再卡死、二启正常启动——调度器忙转修复生效。
- ⚠️ 还是慢；vita3k.log 出现 `seek_file (_sceIoLseek) returned 0x80010051`，UC 无法读本地缓存。
- ❌ 真机安装仍报 0x8010113D（v01.30 的 MEMSIZE 修复没解决真机问题）。

### EBADFD 根因（v01.40 修复）

- 0x80010051 = errno 81 = EBADFD。Vita3K seek_file 只在 fd<0 或 fd 不在打开表时返回它——**phoneME 拿着死 fd 在 seek**。
- 链条：`VitaFileHandle.path[256]` 太短 → UC 深层缓存路径被 strncpy 截断 → RMS compact 走 `pcsl_file_truncate`（close→reopen O_TRUNC）→ reopen 打开**不存在的截断路径**失败 → `vf->fd=-1` 但 handle 仍被 Java 层持有 → 之后每次 seek/read 都是 `sceIoLseek(负fd)`=EBADFD → **UC 本地缓存永久失效，每次启动全走网络**（"太慢"的真因之一）。
- 修复（vita-port 提交 6e9be4b）：path 扩到 600（与 abs_path 同宽）；truncate reopen 失败时用无 O_TRUNC 重开兜底恢复 fd（truncate 本身仍报失败，但句柄不再半死）。

### 真机 0x8010113D 排查记录（未解决，需分流实验）

- 已排除：①eboot.bin 与 `vita-make-fself -s -c`（无 -m）参考输出**逐字节一致**（CMP 验证）；②param.sfo 与 VitaSDK 默认输出一致（ATTRIBUTE=32768 是所有 homebrew 的 mksfoex 默认值，非异常）；③VPK 结构完整（livearea/icon/template 齐全，13.8MiB 压缩）。
- 待做的 A/B 实验（按成本排序）：
  1. 用 VitaSDK 官方 samples 的 hello_world.vpk 装真机——若也失败，是**真机环境问题**（Hen 版本/存储/安装方式），与本 VPK 无关；
  2. 若 hello_world 能装：做一个"hello 壳+我们的 eboot.bin"混装 VPK 定位是 eboot 还是资源问题；
  3. 检查真机 Hen（h-encore 版本对 fself -c 的兼容性）与安装途径（VitaShell 复制 ux0:/vpk 后安装 vs FTP 直装）。
- 教训：模拟器能装 ≠ 真机能装；但**先验证对照样本**再怀疑自己，避免又是 MEMSIZE 式的误判。

## 2026-09-08 v01.39：调度器忙转根因——os_port.cpp 超时条件抄反（ms == 0 应为 ms != 0）

### 症状与诊断路径

- UC 症状链：一启正常（能开 m.baidu.com）→ 退出卡住 → 强关窗口 → 二启卡在数据初始化、fps=1、极卡。
- 决定性证据组合：
  - `runmidlet_debug.log`：**2 次 ENTRY、0 次 RETURNED**——`midp_run_midlet_with_args_cp` 从未返回，VM 轮次没正常走完；
  - `input_debug.log` 二启会话：心跳 #1→#8193（约 840 万次事件泵调用）无触摸——**VM 活着但全速空转**；
  - `midp_stderr.log`：二启网络全通（连到 m.baidu.com/ms.bdstatic.com），说明 UC 引导走完，卡在初始化阶段。

### 根因（确定 bug，非猜测）

- `phoneme-cldc/src/anilib/vita/os_port.cpp` 的 `Os_WaitForEventOrTimeout`：上游 `anilib/linux/os_port.cpp` 是 `if (ms != 0)`（基于 gettimeofday 算绝对超时），**Vita 移植时抄成 `if (ms == 0)`**——`ms > 0` 时 timespec 恒为 `{0,0}`（1970-01-01）→ `pthread_cond_timedwait` 立即 ETIMEDOUT。
- 后果链：`JVMSPI_CheckEvents`（vita_checkevents.c）把 ANI 等待 clamp 到 50ms → `ANI_WaitForThreadUnblocking` → `PoolThread_WaitForFinishOrTimeout(…, 50)` → 立即返回 → 调度器**永不真正睡眠**，50ms 等待变成 0ms 忙循环 → CPU 烧满、fps=1、心跳计数狂奔、UC 初始化被拖死。基线提交 98b12b5 就带此 bug（git show 确认）。

### 修复（v01.39）

- `os_port.cpp`：`if (ms == 0)` → `if (ms != 0)`（+注释说明来历），phoneme-cldc 提交 5956a99。
- 重编：`cd build/vita_arm/target/release && make CLDCBUILD=fp ENABLE_ENABLING_CHECK=false os_port.o`（用体系自身的 release 配方，`arm-vita-eabi-g++`，注意 build/vita_arm/dist/lib 下的 `g++` 同为交叉编译器符号链接）。
- 重打包：在 `dist/lib` 内 `arm-vita-eabi-ar r libcldc_vm_ani.a <新os_port.o>`、同法 `_r` 变体（vita-port 链接的是 `libcldc_vm_ani`/`_anix`）。
- 验证：velf `.text` 内 `Os_WaitForEventOrTimeout` 指令流为 `bl gettimeofday; orrs r3,r6,r8; streq…; beq`——与库内修复版逐指令一致。VPK=build/cmake/midp_vita.vpk（APP_VER 01.39）。

### 教训

1. **移植平台代码时逐字符对照上游**：一个 `==`/`!=` 抄反在 pthread 超时路径上不报错、不崩，只表现为"性能差/卡顿"，极难归因。凡从上游复制的函数，落地前 diff 一遍原版。
2. **心跳计数是免费的性能仪表**：input_debug 心跳每 1024 次泵一条，速率异常（百万级 vs 预期 ~2万/分钟）直接暴露忙转。
3. 待观察：退出卡死（一启退出挂住）是否同根因（忙转下 ANI/pthread 竞态）或另有独立原因——v01.39 实测后若退出仍卡，下一嫌疑是 SVM 模式 MIDletDestroyTimer 失效（上游注释明说 timer 在 SVM 不工作）与 EventQueue.shutdown 等待链。

## 2026-09-08 v01.38：UC 数据初始化慢的 I/O 放大器（debug_log）+ 网络修复实测确认

### 网络线关闭（实测证据）

- 用户 v01.37 实测 net_log：`gethostbyname` 全部秒回、`connect immediate OK fd=15/16`（连 uc.ucweb.com/ucus.ucweb.com 均直连成功）——**v01.35 的 g_handles 修复实测生效**，select 唤醒链正常（连接走的是 immediate OK，未触发 EINPROGRESS 也无妨）。网络层不再有已知问题。

### UC「卡在数据初始化」的 I/O 放大器（v01.38 修复）

- `pcsl_file_open` 每次被调都 `debug_log()`：**open+write+close 三个 syscall 写 debug_log.txt，成功路径写两行 = 6 个额外 syscall/次**。UC 初始化要开几百个文件（RMS 恢复/配置/皮肤），I/O 放大约 3 倍；Vita3K 还为每个 syscall 打一行 trace 日志，双重放大。
- 修复：`VITA_FILE_DEBUG` 宏开关（默认 0 关闭），所有 debug_log 调用点进 `#if`。
- 附带：`pcsl_file_unlink` 在"ux0 文件存在但被锁"时不再 fallback 删 app0 副本（必然再失败，占用日志翻倍的来源）。

### 「还是报占用」的定性与现状

- v01.37 起"占用报错"= 预期行为：共享冲突在保护 UC 活存档不被 suite 清理循环删掉（见 v01.36/v01.37 章节）。
- v01.38 后噪音减半（不再有成对的 app0 fallback 失败）。剩余：每个受保护文件一组三行（Vita3K 打的），**无害、无法从应用侧消除**（除非模拟器修 FILE_SHARE_DELETE）。

### 教训

1. **调试日志本身会成为性能 bug**：per-call 的 open/write/close 型日志在高频路径（file_open）上等于把 I/O 放大数倍——发布前必须用编译期开关关闭，不是靠"日志文件小"判断影响。
2. 修复网络/文件层后要主动找"实测证据"关闭问题线（本次 net_log 的 immediate OK），避免已修问题继续占用排查注意力。

## 2026-09-08 v01.37：撤销 v01.36 句柄驱逐——共享冲突一直在「意外保护」UC 存档（重要教训）

### 症状（用户 v01.36 实测）

- UC 第一次启动正常，第二次不行（NPE 回归）+ 严重卡顿 fps≈1。
- 时间线对照：v01.35 时代用户只抱怨"占用日志还在"，**从未抱怨二启失败**；v01.36（驱逐）后立即出现。v01.36 相对 v01.35 的唯一行为差异就是驱逐。

### 根因：删除成功才是错的

- `midp_remove_suite` 清理循环删除的目标 `appdb_1A35/FFFFFFFF*` **是正在运行的临时 suite（UC）的活 RMS 数据**。
- 11:28 日志时序实锤：`remove FFFFFFFF 失败(Error 32) → 紧接着又 open fd 0x171 同一路径`——删除失败后马上有代码重新打开它，说明它是活数据。
- **v01.36 之前**：Windows 共享冲突（Error 32）让这些删除**一直失败**——这个"烦人的占用报错"其实一直在**意外保护** UC 存档跨启动存活。
- **v01.36**：驱逐持有句柄（fd→-1）+ 重试 → 删除成功 → 但 Java 层还持着这些 fd 继续写 → 全部落空 → RMS 文件半写损坏 → 二启读坏数据 NPE + 写失败重试风暴烧 CPU → fps≈1。
- **比全删更糟**：全删（用户手动删 appdb）反而等价"首次启动"，能进；半坏数据直接崩。

### 修复（v01.37，已提交）

- `pcsl_file_unlink` 撤销驱逐：真占用时**如实返回失败**，让文件活下来。三行 Vita3K 日志（Cannot remove/Error 32/io_error_impl）是模拟器打的装饰性噪音，接受它。
- 句柄注册表与 `vita_handles_evict`（标 `__attribute__((unused))`）保留，供将来"真卸载时才驱逐"的精细化修复。
- **用户须知**：v01.36 跑过的 `rms/appdb_1A35` 可能已被写坏，装 v01.37 后先手动删一次该目录再测。

### 教训（最高优先级）

1. **"修好报错"前先问：这个失败在保护什么？** 报错 ≠ bug。删除失败的背后可能是数据的生命周期还没结束。尤其清理循环 vs 活数据的竞争。
2. **修改删除/关闭语义的补丁，必须验证"数据跨启动存活"场景**（跑两次 + 检查数据文件完整性），只看单次运行日志不够。
3. 平台语义差异（POSIX unlink-open-file vs Windows）不能机械补偿——补偿方向错了一样是 bug。上游依赖的语义在这台"机器"上恰好被模拟器缺陷满足时，先搞清楚谁依赖它再动手。

## 2026-09-08 v01.36：「FFFFFFFF 被占用」真相 = Windows 共享冲突，v01.35 误判纠正（对照 Vita3K 源码逐行验证）

### v01.35 为什么没修好（误判复盘）

- v01.35 从 `sceIoRemove returned 0x80010002` 推出"ENOENT=文件不存在，Vita3K 映射错误"——**被 Vita3K 骗了**。
- **实锤（拉取用户实际版本 Vita3K v0.2.1 496939b6 的 `io.cpp` 源码对照）**：`remove_file()` 里 `fs::detail::remove()` 失败时，`LOG_ERROR("Error code: {} ({})", error_code.value(), error_code.message())` 打印的是 **Windows API 原话** —— `Error code: 32` = `ERROR_SHARING_VIOLATION`，**这次是真实占用**，不是误报；然后 Vita3K 对"不存在"和"被占用"**一律硬编码返回** `IO_ERROR(SCE_ERROR_ERRNO_ENOENT)` = `0x80010002`。返回码无区分度，v01.35 的"ENOENT=不存在"推理在此场景失效。
- 另一实锤：`open_file()` 的 `FileStats` 用 `create_shared_file`（`_wfopen`，**无 FILE_SHARE_DELETE**）持有 `FILE*`——只要进程内任何 fd 开着该文件，Windows `fs::remove` 必 Error 32。**POSIX 允许 unlink 已打开文件**（phoneME 清理循环依赖此语义），Windows 不允许——平台语义差异是根因。
- 日志三行（`Cannot remove file` / `Error code: 32` / `io_error_impl returned 0x80010002`）**全部是 Vita3K 打的**，应用侧改返回值处理消不掉，必须让 sceIoRemove 真正成功。

### 修复（vita_pcsl.c）：句柄注册表 + 删除前驱逐

- `g_open_handles[64]`：`pcsl_file_open` 成功时注册、`pcsl_file_close` 注销（`vita_handle_register/unregister`）。
- `pcsl_file_unlink` 新流程：remove 失败 → `sceIoGetstat` 仍存在（=真占用，区别于不存在）→ `vita_handles_evict(abs_path)` 把**同路径**的所有句柄 `sceIoClose`+`fd=-1`（模拟 POSIX"名字先消失，fd 随后失效"）→ 重试 remove。"已不存在=成功"逻辑保留。
- `pcsl_file_truncate` 内部 close→reopen 同一 `vf`（注册表指针/路径不变），无需改动。
- 验证：编译通过，APP_VER=01.36，VPK 11:16。

### 教训（重要）

1. **模拟器的错误返回码可能无区分度**：Vita3K 把 ENOENT 和共享冲突都折成 `0x80010002`，判断真实失败原因**不能只看返回码**，要对照模拟器源码的 HLE 实现（这次是 `io.cpp remove_file` 的 `error_code.message()` 帟暴露了 Windows 真错）。
2. **跨平台语义差异要在移植层补偿**：POSIX "unlink open file" vs Windows 共享冲突——上游代码天然依赖 POSIX 语义时，移植层要么提供句柄注册表驱逐，要么保证删除前句柄全关。
3. **教训的教训**：v01.35 的推理链（返回码→ENOENT→"文件不存在"）单看自洽，但没有对照模拟器源码验证就下了结论——**涉及模拟器行为时，先拉对应版本源码核对，再下结论**。

## 2026-09-08 v01.35：破除「FFFFFFFF 被占用」误报 + UC 联网失败根因（g_handles 表未初始化）

### 一、`remove_file appdb_1A35/FFFFFFFF "文件被占用"` ~~是 Vita3K 的误报~~（v01.36 纠正：一半误判，见上章节——ENOENT 部分确是误吞，但 Error 32 部分是真实共享冲突，v01.35 的"纯误报"结论不成立）

- **真错误码**：`io_error_impl` 明确打印 `remove_file (sceIoRemove) returned 0x80010002`。SCE I/O 错误编码 = `0x80010000 + errno`，`0x80010002 - 0x80010000 = 0x2 = ENOENT`（对照新lib `sys/errno.h: ENOENT 2`）。**是"文件不存在"，不是"共享冲突"**。
- **误报机制**：Vita3K 打日志时把 SCE errno 映射到 **Windows errno 表**，错把 `ENOENT(2)` 显示成 `Error code: 32 (另一个程序正在使用此文件)` —— 纯映射错误。用户看到的"被占用"是假的。
- **谁触发**：`midp_remove_suite` 清理循环（`suitestore_task_manager.c:452`）对枚举出的**每个**条目无条件 `storage_delete_file` → `pcsl_file_unlink` → `sceIoRemove`。当目标已不存在（裸 `FFFFFFFF` 只是 suiteId 前缀，真实文件是 `FFFFFFFF<name>.db`；或已被前序删除步骤删净），`sceIoRemove` 返回 ENOENT → 走 `io_error_impl` 打印。**与文件是否被删无关，删不删文件夹都会报**（因为删的是一个本来就不存在的条目）。
- **修复（vita_pcsl.c）**：`pcsl_file_unlink` 在 `sceIoRemove` 失败后，用 `sceIoGetstat` 探测——若路径已不存在则视为"删除已达成"，返回成功。不硬编码错误码，真机/Vita3K 通吃。附带收益：清理循环不再因 ENOENT 提前 `break`，后续文件能删干净。
- **结论**：这条日志是 Vita3K 特有害噪音，真机不会有；修复后归零。

### 二、UC 联网失败根因：`g_handles` 表未初始化，所有非阻塞 connect 静默失败

- **症状**：net_log.txt 中连真实服务器 `uc.ucweb.com`(120.241.3.205)/`u.ucfly.com`(219.133.46.180) 只有 `socket_open`，**无任何 connect 结果**；只有不可达的移动 WAP 代理 `10.0.0.172` 打印 `connect FAILED errno=116`（ETIMEDOUT，UC 内置 CMWAP 回退，当前网络必超时，正常）。
- **根因**：`vita_net.c` 的 `static NetHandle g_handles[VITA_NET_MAX_FDS]` 是 **BSS 全局，默认全 0**。`na_create()` 靠 `g_handles[i].fd == -1` 找空槽——初始全是 0，**永远匹配不到，永远返回 NULL**。于是所有走 `EINPROGRESS` 的非阻塞 connect（NBIO 由 v01.33 的 `vita_set_nbio` 正确设置）都在 `h == NULL` 处静默 `close(fd); return PCSL_NET_IOERROR`——**没有日志**。这就解释了两类主机的日志差异：可达主机走 EINPROGRESS→na_create NULL→静默失败；不可达代理 connect 直接返回 ETIMEDOUT→打 FAILED（在 na_create 前）。
- **修复（vita_net.c）**：`g_handles` 显式初始化为全部 `{ -1 }`（32 个）。na_create 从此能正确注册 fd，select 表生效，EINPROGRESS→select→NETWORK_*_SIGNAL 唤醒链恢复。
- **教训**：依赖"表里某个哨兵值"的空槽查找，表必须显式初始化；BSS 零值（fd=0）恰好是非法 fd，但不是哨兵 `-1`，会使"找空槽"逻辑永久失败。此类 bug 无编译告警、无运行日志（NULL 路径常被上层吞掉），只能靠审查 + 实测日志追。

### 验证

- 两修复均编译通过，APP_VER=01.35，VPK `/home/zyb/vitasdk/samples/j2me/vita-port/build/cmake/midp_vita.vpk`（08:52）。
- 提交：见 `git -C vita-port log`。

## 2026-09-08 v01.34：目录枚举 stub 是 UC 二次启动 NPE 根因（重要教训：stub 的连锁失效模式）

### 症状与误导性线索

- 用户实测：UC 首次能进（v01.33 修复生效），**第二次启动 `java.lang.NullPointerException: 0 at ao.a(), bci=4`**，删除 `rms/appdb_1A35` 后又能进一次。
- 日志里 `remove rms/appdb_1A35/FFFFFFFF` Error 32（文件被占用）一度是头号嫌疑——**不是根因**（那是 suite 关闭时的良性清理尝试），真根因是枚举 stub。

### 真根因：pcsl_file_openfilelist 是 NULL stub

- 调用链（全部源码核实）：`RecordStore.listRecordStores()` → `RecordStoreFile.listRecordStores` (jpp) → native `getNumberOfStores` → `rmsdb_get_number_of_record_stores` → `rmsdb_get_number_of_record_stores_int` → `storage_open_file_iterator` → `pcsl_file_openfilelist` → **NULL** → 返回 `OUT_OF_MEM_LEN` → JNI 层 `KNI_ThrowNew(midpOutOfMemoryError)` → UC 启动状态恢复中断 → `ao.a()` NPE。
- **首启能过的原因**：RMS 创建走 `pcsl_file_open`/`pcsl_file_exist`（真实实现）；只有"列出已有 store"需要枚举。删目录后列表合法为空 → 绕过失败路径 → "又能进一次"。这就是"删 appdb 恢复"症状的机制。
- 受害面不止 UC：`midp_remove_suite` 的文件清理循环、`rmsdb_remove_record_stores_for_suite`、`RecordStore.listRecordStores` 全走此枚举。

### 修复（vita_pcsl.c，+158 行）

- `pcsl_file_openfilelist/getnextentry/closefilelist` 用 `sceIoDopen/Dread/Dclose` 实现，**严格按上游 POSIX 参考契约**（`pcsl/file/posix/pcsl_posix.c` + `pcsl/file/util/pcsl_util_filelist.c`）：
  - 输入串语义 = `root目录 + match前缀`；按最后一个分隔符拆分（`pcsl_string_last_index_of`，jchar 单位）
  - 每次 getnextentry 返回下一个前缀匹配的目录项，结果 = `string[0..rootLength) + entry名` 完整路径（caller free）
  - 跳过 `.`/`..`；目录本体经 `vita_resolve_path` 解析（相对路径防御）
- RMS 文件布局（顺带查实）：suite 存储是 **sRoot 下扁平文件** `root + <suiteId8位hex> + <后缀>`；后缀集合 `.ss/.ii/.ap/.db/.idx/.jar/.ssr/.tmp` + `_suites.dat/_trans.dat/_icons.dat`（`suitestore_constants.xml`）。`FFFFFFFF` = `INTERNAL_SUITE_ID(-1)`，临时 suite（从 jar 直接启动的 UC）的 RMS 基名 = `getSecureFilenameBase(-1)` = `build_suite_filename(-1, EMPTY)`。
- 验证：ELF 三符号 T；`getnextentry` 内 `sceIoDread` 调用确认；APP_VER=01.34。提交 79f5dbc。

### 教训（重要）

1. **stub 的失效模式是"条件性成功"**：创建路径真实、枚举路径 stub → 首启成功、二启失败——极易误判为"持久化数据损坏"。以后遇到"首启 OK 二启挂"先查该功能的**完整 API 面**（创建/读/写/枚举/删除）是否都有实现，而不是先怀疑数据损坏。
2. 上游契约不能凭直觉写：`pcsl_string_last_index_of(const pcsl_string*, jint)` 签名与 POSIX 版 `PCSLStorageDirInfo`（rootLength jchar 单位含分隔符）都从参考实现核对后才动手。
3. 日志里 Error 32/0x80010002 的 remove 失败在 suite 生命周期里大量出现，多数良性——**判断主因要看调用时序与是否有后续读坏数据**，单条 remove 失败不构成根因。

### 遗留风险

- `pcsl_file_getfreespace/getusedspace` 仍返回 0 的风险已在补全提交（b98c165）消除：
  - `getusedspace`：sceIoDopen/Dread 单层遍历 storage root 求和（POSIX 参考语义，不含子目录）
  - `getfreespace`：`sceAppMgrGetDevInfo("ux0:")` 真实空闲字节（long 32 位饱和 LONG_MAX）
  - **关键联动**：`internal.config`(+landscape) 增加 `system.jam_space: 100000000`——`storage_get_free_space = totalSpace - used`，而 `totalSpace` 默认 4MB（`DEFAULT_TOTAL_SPACE`），不设此属性时 RMS 数据超 4MB 会误报 `RecordStoreFullException`。属性单位字节（上游 linux_fb 参照 1000000），读取点在 `midpInit.c:253` `getInternalProperty("system.jam_space")`。
- RMS 的 `.idx` 索引文件（tree_index/linear_index）路径未实测。
- 真机未测；Vita3K 的 sceIoDread 对大目录行为需观察。

## 2026-09-08 v01.33：fcntl NBIO 对 sceNet fd 无效（connect 冻结 VM 27s）+ vm_output 竖排修复

### 用户 v01.32 实测反馈 → 三个现象，一真两良性

1. **connect 卡死 27 秒**（真 bug）：net_log 显示 `socket_open ip=120.241.3.205 port=80` 后无 connect 结果，Vita3K 日志 `sceNetConnect` 从 11:28:32 阻塞到 11:28:59 才返回 `0x8041013C`（= `SCE_NET_ERROR_ETIMEDOUT`），vita_net.c 日志 `errno=116`（= `ETIMEDOUT`）。
2. **vm_output.log 一字符一行**（真 bug）：CLDC 的 `PrintStream.write(int)`（`write(int b) → byteOut.write(b)`）**逐字符**调 `JVMSPI_PrintRaw(s,1)`；v01.28 的逐行钩子每次 `fprintf("%.*s\n")` 强加换行 → 竖排。
3. `sceNetInit 0x80410110`（EBUSY）+ `sceNetResolverCreate` stub 警告（良性）：第二轮启动 newlib `gethostbyname` 内部 lazy 再 init 网络栈；`10.0.0.172` 是 UC 内置 CMWAP 移动代理回退（直连失败才走），当前网络环境必超时，非我方 bug。
4. **v01.32 修复确认生效**：suite 落在 `rms/appdb_1A35/FFFFFFFF`（per-game appdb）；`[MIDLET] created+registered OK: cn.uc.application.app.WebClient`（此前崩溃点已过）。`remove_file FFFFFFFF` 失败是 suite 关闭清理，良性（2026-09-07 已有结论）。

### 根因：fcntl(O_NONBLOCK) 对 sceNet 描述符是 no-op

- 二进制级验证链：newlib `socket()`@810f8e98 → `sceNetSocket`（fd 直通，错误码经 `__vita_scenet_errno_to_errno` 转换）；newlib `select`@810f15f8 → `sceNetEpollCreate/Control/Wait/Destroy`（select 对 net fd 有效）；但 `fcntl` 只作用于文件描述符标志，sceNet fd 的非阻塞模式是**socket 选项**：`sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO=0x1100, &on, 4)`（net.h:388）。
- 后果：socket 一直是阻塞模式，`connect()` 阻塞在 syscall 里，VM 事件泵线程全冻结（`checkForSystemSignal`/`vita_net_poll` 也跑不了），27s 后 ETIMEDOUT。此前 v01.30 时代"网络能用"是因为当时网络调用全 stub 直接失败返回，从没真正 connect 过。
- 修复：`vita_net.c` 新增 `vita_set_nbio(fd)`，在 TCP client（`pcsl_socket_open_start`）、server（`pcsl_server_socket_open_start`）、UDP（`pcsl_datagram_open_start`）创建时统一调用；`vita_socket_available()` 的 MSG_PEEK 依赖 socket 已 NBIO（删掉原来的临时 fcntl 翻转）。
- 恢复的设计链：NBIO connect → `EINPROGRESS` → `PCSL_NET_WOULDBLOCK` → Java 线程 `midp_thread_wait(NETWORK_WRITE_SIGNAL)` → `vita_net_poll()` select 就绪 → `midp_thread_signal` 唤醒 → `getsockopt(SO_ERROR)` 完成握手。

### 修复：JVMSPI_PrintRaw 原样透传（phoneme-midp cf018c2）

- `midp_run.c`：`fprintf("%.*s\n")` → `fwrite(s,1,length)`；换行由 Java println 自带。逐次 fopen/fclose 防强杀丢日志方案不变。最终 ELF 验证 `fopen→fwrite→fclose`。

### 踩坑（新增）

- **build.sh 的 make 失败会被吞**：`make ... 2>&1 | tail -20 || {...}` 里管道退出码是 tail 的（成功），`set -e` 拦不住——脚本继续打出成功横幅但 VPK 是旧的（本次 APP_VER 仍 01.32、VPK 时间戳早于 midp 重编才发现）。判断构建成败要 `ls -la --time-style` 对时间戳 + 查 APP_VER，或手动 `make`。本次 datagram 段漏改的 fcntl 就是手动 make 才暴露的。
- errno/错误码速查：newlib errno 116=ETIMEDOUT、119=EINPROGRESS、120=EALREADY、11=EAGAIN/EWOULDBLOCK；sceNet 0x8041013C=ETIMEDOUT、0x80410110=EBUSY（`psp2/net/net.h`）。

### 验证与产物

- `midp_run.o`：`JVMSPI_PrintRaw` 内 `bl fopen/fwrite/fclose`（无 fprintf 格式串）。
- 最终 ELF：`pcsl_socket_open_start` 序列 `socket(8100766c) → sceNetSetsockopt(810076c0) → connect(810076e0)`；全 ELF 6 处 sceNetSetsockopt 调用。
- VPK：APP_VER=01.33，`build/cmake/midp_vita.vpk`（04:20:15）。
- 提交：phoneme-midp cf018c2（PrintRaw）+ samples c5722f0（vita_net.c NBIO + CMakeLists v01.33 注释）。

### 遗留风险

- 直连 `120.241.3.205:80`（uc.ucweb.com）本身超时与否取决于网络环境（该 IP 是 UC 老服务器的电信出口）；修好 NBIO 后若仍连不上，属服务器/路由问题，换网络验证。CMWAP 代理 10.0.0.172 在当前环境必不通（UC 无设置页时可能反复重试）。
- datagram/available 路径的 NBIO 改动未实测（无 UDP MIDlet）；MSG_PEEK 假设 socket 已 NBIO，若后续有 UDP MIDlet 异常优先查这里。
- 若 UC 在连接阶段表现"卡住但没死"，属预期：EINPROGRESS + select 唤醒链首次真正运转，观察 net_log 是否出现 `connect DONE`/`connect EINPROGRESS` 行。

## 2026-09-08 v01.32：UC 卡初始化真根因（皮肤图 app0 fallback 失效）+ appdb 覆盖修复 + rms/ 目录归整

### 三个用户反馈 → 两个根因 + 一个修复

1. **"UC 还是卡住初始化"** → 直接根因：30 张皮肤 PNG 全部 `FAILED (0x80010002)`（ENOENT）。chameleon 皮肤资源池初始化带缺图进行，LCDUI 初始化链路挂起。位置在 `games/app/game.jar` 与 `appdb/FFFFFFFF` 打开日志之后。
2. **"很多 png 资源没有"** → **资源其实都在**：VPK 内 `data/J2ME00001/lib/` 有全部 178 张 PNG（python3 zipfile 解包验证，`screen.image_wash`/`scroll.*`/`softbtn.*`/`ticker.*`/`alert.*` 一个不缺）。打不开是 fallback bug（见下）。
3. **"为什么数据都访问 appdb/FFFFFFFF，appdb_XXXX 却全空"** → `FFFFFFFF` = `INTERNAL_SUITE_ID(-1)` 的 8 位 hex（`GET_SUITE_ID_LEN=8`，`midp_suiteid2pcsl_string`），suite 存储路径 = `sRoot + suiteId`。`sRoot` 永远是共享 appdb，因为 per-game 设置被上游覆盖（见下）。

### 根因 A：pcsl app0 fallback 拼接非法路径（皮肤图打不开）

- `vita_pcsl.c` 的 `pcsl_file_open/unlink/exist/sizeof` 在 ux0 打开失败后 fallback：`snprintf(app0_path, "app0:%s", path_utf8)`——用**原始输入路径**。输入是绝对路径 `ux0:/data/...` 时产生 `app0:ux0:/...`（恒 ENOENT）；该命中 VPK 的场景只有皮肤图（`lib/<name>.png`，绝对路径），于是全军覆没。
- 路径链：`ResourceHandler.getSystemImageResource` → `getAmsResource(name+".raw"/".png")` → `File.getStorageRoot(INTERNAL_STORAGE_ID)` = `sRoot`（= appdb dir = `ux0:/data/J2ME00001`）→ `lib/...` 由 skin 代码拼上。VPK 内布局是 `data/J2ME00001/lib/*.png`，所以正确 fallback 是把 `ux0:/data/` 前缀换成 `app0:/`。
- 修复：四处 fallback 统一改为基于 `vita_resolve_path` 产物的前缀替换：`strncmp(abs,"ux0:/data/",10)==0 → "app0:/"+(abs+5)`（注意 +5 跳过 `ux0:`，无重复斜杠）。字节级验证：`ux0:/data/` @810ffff4、`app0:/%s` @81100000。
- ROM 化皮肤机制说明：`lfj_load_image_from_rom`（SkinRomizationTool 生成的 178 项查找表，在 `lfj_image_rom.o`）**已生效**，但 Java 侧部分资源（`SkinResourcesImpl` 之外的 `ResourceHandler` 路径）仍走文件系统，两条路都要通。

### 根因 B：runMidlet 无条件覆盖 midpSetAppDir（appdb_XXXX 全空）

- 覆盖链：`vita_main.c` 启动器每轮 `midpSetAppDir(per-game appdb_<TAG>)` → `runMidlet()`（`phoneme-midp/src/ams/example/jams/native/runMidlet.c:134-141`）**无条件** `midpSetAppDir(getApplicationDir(...))` → `commandLineUtil_md.c` 的 `getApplicationDir` 从 `MIDP_HOME` 重建 `MIDP_HOME + "/appdb"`（APPDB_DIR 硬编码）→ sRoot 永远 = 共享 appdb。
- `storageInitialize(config_home, app_dir)` 在 `midpInit(LIST_LEVEL)` 里用 `midpAppDir` 填 `sRoot[0]`；`midp_suite_exists(INTERNAL_SUITE_ID)` 直接返回 OK 不查 `_suites.dat`，所以**空的 per-game appdb 目录可直接用**，无需种子拷贝。
- 修复（phoneme-midp 0767127，3 文件 +29/-8）：新增 `midpGetAppDir()`（midpInit.c + midpAMS.h）；runMidlet 仅当 `midpGetAppDir()==NULL` 才回退 `getApplicationDir`。USE_NATIVE_APP_MANAGER=false，nams 的同类调用点不在链接里。
- 附带修 UB：`midpSetAppDir` 只存指针，vita_main 传的是栈缓冲 `char appdb_path[80]` → 出块悬空。改 `static char appdb_path[96]`。

### 目录归整（用户诉求：数据文件夹收进子目录）

- suite 存储统一移入 `rms/` 子目录：`ux0:/data/J2ME00001/rms/appdb`（共享/Hello）+ `rms/appdb_<TAG>`（per-game）。`get_per_game_appdb` 输出带 `rms/` 前缀。
- 迁移：启动时 `sceIoDopen(DATA_DIR "/appdb")` 存在则 `sceIoRename` → `rms/appdb`（一次性，老存档保留）。旧 `appdb_XXXX` 空目录不迁移（无数据），会残留在原处，可手动删。
- 目录布局变为：`J2ME00001/{lib/, games/, inbox/, rms/, cache 等}`。

### 验证与产物

- `bash build_vita.sh`（phoneme-midp）exit=0：`libobj.a` 已含 `midpGetAppDir` T 定义 + runMidlet.o U 引用。**注意**：构建尾部 `libmidp.so` 链接失败（`_rom_linkcheck_mffd_false` 未定义）是**预先存在**的中间产物问题——`CREATE_MIDP_SHARED_LIB=true` 的检查性链接需要 `vm_rom_stubs.c`（在 vita-port 侧），不影响 `libobj.a` 生成与最终链接。
- `bash build.sh`（vita-port）exit=0；`midpGetAppDir` T @8103d95c 在最终 ELF。
- VPK：178 张 lib PNG 在包内；param.sfo APP_VER=01.32。
- 提交：phoneme-midp 0767127 + samples 145a382（CMakeLists 版本注释含完整因果链）。

### 遗留风险

- UC 实测若仍卡：下一个埋点是 `SkinResourcesImpl_ifLoadAllResources0`（AMS isolate 分支，`ENABLE_MULTIPLE_ISOLATES=false` 时返回 FALSE 走 lazy load）与 chameleon `ResourceHandler` 的 `/raw` 路径；另需看 `vm_output.log`/`midp_stderr.log`。
- `libmidp.so` 链接噪音每次 `build_vita.sh` 都会报 Error 1（exit 码却为 0 的原因是脚本最后一步 cp/chmod 成功）——判断成败要看 `libobj.a` 时间戳，不要只看脚本退出码。
- 若用户嫌弃旧 `appdb_XXXX` 空目录残留：下次可加"检测 rms/ 存在则清理旧 appdb_*"。

## 2026-09-08 v01.31：LCDUI 初始化崩溃修复（UC 进不去死因）+ 菜单扫描缓存

### 崩溃根因（Vita3K EXCEPTION_ACCESS_VIOLATION，UC 等 MIDlet 启动即死）

- 现象：Vita3K 崩溃 `EXCEPTION_ACCESS_VIOLATION 0xC0000005`，PC=0x810346c6，"Invalid read at 0x0"；net_log.txt 只有 3 行 init 成功日志（网络调用从未发生）。
- 归因（二进制级）：`arm-vita-eabi-addr2line -e midp_vita -f -C 0x810346c6` → `DisplayDeviceContainer.getDisplayDevicesIds0`。反汇编确认：native 侧拿 `lfjport_get_display_device_ids` 返回的 ids 指针（r5）→ `SNI_NewArray` → 循环 `ldr r2,[r5,#4]!` 解引用 NULL。
- 根因：`vita-port/src/vita_display.c` 的 `lfjport_get_display_device_ids` 违反契约——`*n=1` 但 `return NULL`。正确契约见上游 `phoneme-midp/src/highlevelui/fb_application/reference/native/fbapp_export.c:398`：返回 `static jint display_device_ids[] = {0}`。
- Java 链路：`DisplayDeviceContainer` 构造（LCDUI 初始化即触发）→ `getDisplayDevicesIds0()` → 循环 `ids.length` 读元素 → 死。HTTP 任何调用之前。
- 修复：vita_display.c 返回静态数组 `{0}`；反汇编验证新实现 `str 1->*n; ldr r0,[pc,#4]`（返回 0x813bf4b8 非 NULL）。提交 3c30ee8。
- 为何此前游戏能跑：待查（可能 v01.30 前该函数路径不同，或游戏走不同初始化顺序）；不影响本修复正确性。

### 次要理论排查（closed，不成立）

- `DisplayEventListener` type=54 未知 id NPE 理论：ROM 化 midp_system.jar 里**没有任何类调用 `MMEventHandler.setListener`**（JSR135 Player 实现 BasicPlayer 不在构建里，`USE_JSR_135` 实际 false）；MMAPI 事件即使到达 Java 队列也因无注册 listener 被丢弃。vita_media_notify.c 的 push 侧字段布局与上游 javacall 桥（midp_msgQueue_md.c:197）一致，无害。

### 菜单扫描缓存（用户诉求：首次启动慢，是否逐一解析 jar → 是）

- 原状：`scan_games()` 每游戏开 jar 4-5 次（MANIFEST x3 + 中央目录 class 验证 + icon PNG）+ PNG 解码，每游戏几百 ms，每次启动/重扫重复。
- 方案：每游戏目录 `cache.bin`（magic 'J2CB' v1），键 = jar size + mtime 双字（`sceRtcGetTick`）。命中：不读 jar，name/cls/icon 从缓存取，game.cfg 只刷新人工 class 覆盖与 landscape；icon 用缓存里的 PNG 原始字节解码（免 jar IO）。不匹配：全量重解析 + 重写缓存。
- rescan 纹理保留：`icon_cache_keep/restore_kept` 让未变游戏已解码纹理跨扫描存活（SELECT 重扫零解码）。
- 踩坑记录：①`g->dir "/" CACHE_NAME` 字符串拼接不能直接进 `sceIoOpen` 参数（要 snprintf）；②`sceRtcGetTick(const SceDateTime*, SceRtcTick*)` 出参型，不是返回 tick 值；③multi_replace 衔接处又吞了一次 `icon_load` 函数签名行（第二次发生，**多段替换后必须编译**）；④git commit -m 中文长消息会被终端回显污染，用 `git commit -F <file>`。
- 提交：27d56b8（cache.bin）+ 3c30ee8（崩溃修复）+ 55c2a17（APP_VER 01.31）。

### 验证状态

- 增量构建 EXIT=0，midp_vita.vpk（APP_VER=01.31）已重新打包，待用户 Vita3K/真机装新 VPK 实测 UC。
- 若 UC 仍进不去：net_log.txt 现在有全链路埋点，可定位网络阶段卡点。

## 2026-09-07 v01.30 修订 2：真机安装 0x8010113D + 网络日志全链路埋点

### 真机 0x8010113D（安装失败）
- **根因**：`vita_create_self(... MEMSIZE 128)`——fself `-m` 是 **KiB
  内存预算**，Normal app 合法值仅 0（0x1000-0x12800 仅 System mode），
  128(0x80) 写入 SELF control info 后真机 SceShellSync 拒绝安装；
  Vita3K 不校验该字段所以模拟器一直没事。128 来自初始 commit 288337c
  （作者可能误以为单位是 MB）。
- **修复**：CMakeLists 去掉 MEMSIZE（留注释说明）；字节级验证 VPK 内
  eboot.bin 与 `vita-make-fself -s -c`（无 -m）输出完全一致。
- **教训**：`vita.cmake` 的 MEMSIZE 直接透传 fself -m，语义是 KiB 预算
  而非 MB；模拟器宽松真机严格，**首次真机部署前应 diff 一次 fself
  参数**。

### 网络日志全链路（vita_net.c）
- 之前 `vita_net.c` 一行日志都没有 → "网络不可用但看不到 log"。
- 新增 `vnet_log()`：stderr（midp_stderr.log）+ `ux0:/data/net_log.txt`
  追加式双写（stderr 每次启动被截断，net_log 跨启动留存；每行
  fopen/fclose 防强杀丢日志，同 vm_output.log 方案）。
- 埋点覆盖：early_init 三步返回码、getLocalIP、gethostbyname 成败、
  socket open（ip/port）、connect 即时/异步/失败+errno、SO_ERROR、
  recv/send 失败+errno、EOF。任一环失败都能定位。

### 其它
- sceNetInit 内存池 64KB → **1MB**（DNS resolver 也从池分配；官方
  net_http_bsd 示例用 1MB，64KB 极易饿死 DNS）。
- net_poll 从 vita_input.c 移到 vita_checkevents.c（JVMSPI_CheckEvents
  step 0，MIDP 泵之前）：输入泵不应知道网络存在；调用频率/时机不变。
- `multi_replace` 一次吞掉了 `VITA_NET_MAX_FDS` 定义（替换串衔接处
  消费未补回）→ 编译报 undeclared 才发现。多段替换后必须编译验证。
- build.sh 依旧吞 make 错误码照报 successful（v01.28 已知坑，再次
  实锤：本次 Error 2 被包成 "Build successful"）。

## 2026-09-07 v01.30 网络层恢复：UC 浏览器进不去 → socket 全 stub 所致

### 现象与根因
用户反馈 UC 浏览器一直进不去。排查确认：**当前树的网络是纯 stub**
（`vita_pcsl.c` 的 `pcsl_socket_open_start` 直接 return -1），任何
`Connector.open("http://...")` 在 TCP 层即失败 → UC 启动即挂/退出。
8-30 曾有真实现 `vita_net.c`（~845 行，BSD socket 全量），但该版从未
单独 commit，8-30 全面回退时被 git clean 掉，之后所有版本网络均为
stub。

### 恢复来源（git 考古）
`vita_net.c` 找回自 **dangling commit d93fc0a3**（8-30 "save change"，
`git fsck --lost-found` 发现）。内容：pcsl socket/server-socket/
network/datagram 全量真实现——newlib BSD socket 包装（内部 sceNet）、
非阻塞 connect（EINPROGRESS→WOULDBLOCK）、gethostbyname（getaddrinfo）、
`vita_net_early_init()`（sceSysmoduleLoadModule(NET)+sceNetInit+sceNetCtlInit）、
`vita_net_poll()`（select 全 fd 集→NETWORK_READ/WRITE/EXCEPTION_SIGNAL
唤醒阻塞协议线程）。HTTP 是纯 Java（com.sun.midp.io.j2me.http）跑在
socket:// 上，TCP 通即 HTTP 通。

### 改动（4 文件 +16/-311 + 新增 845 行）
1. `src/vita_net.c`（恢复）：全部 pcsl 网络函数真实现。
2. `src/vita_pcsl.c`：整段删除网络 stub（原 1008-1317 行，37 函数 +
   htons/htonl 辅助——这些在 net.c 有同名真实现，**必须删否则重复
   定义**）。
3. `src/vita_main.c`：main 里 `vita_net_early_init()`（VM 启动前）。
4. `src/vita_input.c`：checkForSystemSignal 里 `vita_net_poll()`
   （input/touch poll 之后、media 之前）。
5. `CMakeLists.txt`：+`src/vita_net.c`、+`SceNet_stub SceNetCtl_stub`
   （SceSysmodule_stub 原已有）。

### 经验
- **功能合并回退时必须先把每个功能的文件单独 commit**——8-30 的教训
  重演：vita_net.c 只存在于 patch/dangling commit 里，恢复靠 git fsck
  考古。此后每完成一个功能立即 commit。
- pcsl 网络函数在 stub 与真实现间切换时，htons/htonl/getRawIpNumber
  这类小辅助也是重复定义点，别只看 socket_* 大函数。
- dangling commit 是救命稻草：`git fsck --lost-found` 找回未提交工作。

### 验证
- 构建成功；`nm midp_vita`：`vita_net_early_init/vita_net_poll/
  pcsl_socket_open_start/pcsl_network_gethostbyname_start` 全部 T 符号
  唯一（无重复定义）。
- 产物 `vita-port/build/cmake/midp_vita.vpk`（16:13）。
- 待用户实测：UC 浏览器联网、NetTest MIDlet（HttpConnection GET
  example.com）。**注意**：DNS（getaddrinfo）与 sceNetCtlInit 在
  Vita3K 的支持度未知——若 UC 仍进不去，看 midp_stderr.log 的
  `[net]`/errno 定位是 DNS 还是连接阶段。

## 2026-09-07 v01.29 修复第 2 轮：zip 大小写比较单向折叠 + 触摸坐标系 2 倍（用户日志实锤）

### 用户日志（midp_stderr.log）关键证据
- `口袋灵兽`：`entry[2] len=6 '40.png' match=0` + `[icon] zip entry
  not found: '40.png' (from '/40.png')` —— 条目就在 jar 里（len 精确
  相等）却失配 → 排除前导 '/' 单一原因，暴露比较 bug。
- 其余 jar：`'icon.png' match=0`、`'resource/icon.png' match=0` 全灭；
  唯一能匹配的是全大写 `META-INF/MANIFEST.MF` —— 模式指向单向折叠。

### 根因 2 个（都是确定性 bug）
1. **`zip_read_entry` 的"大小写不敏感"只把条目名转大写，`want` 没转**：
   条目 `'p'→'P'` ≠ want `'p'` → 所有小写 want 失配。修复：两边都折叠
   （vita_menu.c，比较循环加 b 折叠）。
2. **前触摸板 report 坐标是 1920x1088 空间，不是 960x544**：SDK 官方
   touch 示例用 `report[0].y >= 1000` 做判定即为实锤。原代码按
   960x544 边界判定 → 屏幕右半/下半触摸全被当 letterbox 丢弃，左上
   区域坐标也错 2 倍。修复：`vita_touch_init` 用 `sceTouchGetPanelInfo`
   读 maxAaX/maxAaY（默认 1919/1087），采样处归一化到 960x544 再进
   `vita_display_map_touch`（vita_input.c）。
   前轮的 SetSamplingState(START) 修复仍然必要（两个 bug 叠加）。

### 经验
- **带调试日志发布是正确的**：本轮两个根因都是用户回报 midp_stderr.log
  一次定位（match=0 + len 相等 = 比较逻辑 bug；y>=1000 = 2x 坐标系）。
- **zip 大小写折叠必须双向**；写这类比较时用 "want 里没有大写字母"
  的用例自测（小写 want + 混合大小写条目）。
- **SceTouch report 空间 ≠ 显示空间**：真机 1920x1088 vs 960x544。
  永远用 sceTouchGetPanelInfo 的 active area 归一化，别硬编码。
- 官方 samples/（如 touch/src/main.c）是坐标系/API 行为的权威参考。

### 验证
- `./build.sh` 成功（rm .obj 后重建 + make 显式检查退出）。
- zlib 解压 self 段确认 `[TOUCH] init: state=%d set=%d max=(%d,%d)`
  新格式串在产物内。产物 `vita-port/build/cmake/midp_vita.vpk`（15:55）。
- 待用户实测：图标应能显示（40.png/icon.png 类）；触摸全屏区域命中、
  坐标正确（input_debug.log 的 down INSIDE 行 virt 坐标应在 0..239/
  0..319 内且与手指位置成比例）。

## 2026-09-07 v01.29 修复：触摸无效（sceTouch 采样未启动）+ 图标不显示（前导 '/'）

### 用户实测反馈
中文显示 OK；触摸无效果；游戏列表不显示应用图标。

### 根因（两个都是确定性 bug，非模拟器限制）
1. **触摸：`vita_touch_init()` 从未调用 `sceTouchSetSamplingState(START)`**。
   Vita 前触摸面板上电默认 STOP，`sceTouchPeek` 返回成功但 `reportNum`
   恒为 0 → 永远走不进事件分支 → 静默"不支持"。与 MIDP 事件链无关
   （按键走同一链路正常即为反证）。
2. **图标：MIDlet-1 图标路径常带前导 '/'（如 "/icon.png"）**，而 zip
   条目名无前导 '/'，`zip_read_entry` 按 `name_len==strlen(want)` 精确
   长度匹配必然失败 → icon_pix 全 NULL → `draw_icon_scaled` 静默 no-op。

### 改动（仅 2 文件）
1. `src/vita_input.c`：
   - `vita_touch_init()`：先 `sceTouchGetSamplingState`，非 START 则
     `sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
     SCE_TOUCH_SAMPLING_STATE_START)`。**注意枚举是 STOP/START，没有
     "OFF"**（首版编译错即此）。
   - `vita_touch_poll()` 加状态迁移日志（仅状态变化时写，不刷屏）：
     init 结果 / peek FAILED / up / down OUTSIDE(物理坐标) /
     down INSIDE(物理+虚拟坐标) → `input_debug.log`。用于区分
     "事件没产生" vs "被 letterbox 丢弃" vs "Java 层丢弃"。
2. `src/vita_menu.c` `icon_load()`：查 zip 前 strip 前导 '/'；失败时
   fprintf(stderr)（`[icon] zip entry not found` / `[icon] png decode
   FAILED`，菜单阶段 stderr 已 freopen → 进 midp_stderr.log）。

### 经验
- **sceTouch 必须 SetSamplingState(START)**——这是 Vita SDK 隐式要求，
  不调用则 Peek 永远空数据，且无错误返回。
- zip 条目查找一律 strip 前导 '/'（JAD/manifest 资源路径书写惯例 vs
  zip 命名规则的经典差异）。
- build.sh 会吞 make 错误码照报 "Build successful"——验证必须看 make
  输出或检查产物符号/字符串。
- VPK 的 eboot.bin/.self 中 ELF 段被 zlib 压缩，strings 直接 grep 查
  不到新代码；需先 zlib 解压段再查（见验证方法）。

### 验证
- `./build.sh` 成功；python zlib 解压 midp_vita.self 的 0x1000 段确认
  `[TOUCH] init`、`[icon] zip entry` 字符串在内（产物确为新代码）。
- 产物：`vita-port/build/cmake/midp_vita.vpk`（12:30）
- 待真机/Vita3K 验证：触摸日志链（init state=1）、图标行显示。

## 2026-09-07 v01.29 触摸启用 + 菜单中文 + 游戏列表真实名称/图标（三个新功能）

### 需求
1. 游戏列表显示 jar 内真实名称（MIDlet-Name）与图标（MIDlet-1 第 2 字段）
2. J2ME 支持中文显示、触摸功能

### 关键调研结论（改动前先读，避免误判）
- **触摸链路本来就已通**：vita_input.c 的 `vita_touch_poll()` 早已采样
  `sceTouchPeek(SCE_TOUCH_PORT_FRONT)` 并发 `MIDP_PEN_EVENT`（ACTION=
  KEYMAP_STATE_PRESSED/DRAGGED/RELEASED，X_POS/Y_POS 为虚拟屏幕坐标，
  经 `vita_display_map_touch` 从物理 960x544 反算）。事件分发链
  `DisplayEventListener` PEN_EVENT → `Display.handlePointerEvent` →
  Canvas `pointerPressed/Released/Dragged` **不检查 pen_supported**；
  `Constants.POINTER_SUPPORTED` 在 generated/classes 里已是 true。
  唯一卡点是 `lfjport_is_display_pen_supported/motion_supported` 返回
  KNI_FALSE（只影响 `DisplayDevice.hasPointerEvents()` 查询 API）。
- **Java 层中文渲染本来就已通**：vita_font.c 的 gxjport_draw_chars 走
  J2FB v1 字库（fontbitmap.bin，GB2312 全集 + 假名/希腊/全角），Java
  的 drawString 一直能画中文。**只有原生菜单**（vita_menu.c draw_text
  5x7 ASCII）不支持：idx>126 画 '?'。
- **游戏列表名称原来用目录名**（`strncpy(g->name, ent.d_name)`），
  图标从未解析。

### 改动（全部在 vita-port，最小 diff，新增文件优先）
1. `src/vita_display.c`：pen_supported / pen_motion_supported 改返回
   KNI_TRUE（各 1 行 + 注释）。触摸至此完全启用。
2. `src/vita_font.c`（文件尾追加 ~100 行，不改既有代码）：新增
   `vita_menu_draw_utf8()` / `vita_menu_font_gw/gh()`——菜单侧 UTF-8
   解码 + fb_glyph blit 到 32bpp framebuffer（0xAABBGGRR），缺字画
   tofu box。复用 fb_ensure/fb_glyph，无重复字库代码。
3. `src/vita_menu.c`：
   - `draw_text()` 开头探测多字节：含 >=0x80 字节则改走
     vita_menu_draw_utf8（字库加载失败时画 '?' 占位，行仍可见）；
     纯 ASCII 走原 5x7 快速路径（零回归）。
   - 新增 `manifest_get()`（大小写不敏感 + manifest 续行规则）、
     `parse_manifest_name()`（MIDlet-Name，回退目录名）、
     `parse_manifest_icon()`（MIDlet-1 第 2 个逗号字段）。
   - **注意调用顺序**：`g->jar` 必须先 snprintf 再 parse_manifest_*，
     否则 jar 路径为空解析必败（曾踩，自查 diff 时发现修正）。
   - GameEntry 加 `icon[128]` 字段；scan_games 时 `icon_load()` 解码
     并缓存（ICON_CACHE_MAX=MAX_GAMES，菜单退出时 icon_cache_clear）。
   - 列表行/对话框画图标：`draw_icon_scaled()` 按框适配（等比最近邻
     采样，含 alpha 混合；首版整数 scale 溢出 bug 已修）。
4. `src/vita_icon.c/.h`（新增）：libpng 解码（VitaSDK 自带
   arm-vita-eabi/lib/libpng16.a），内存 reader + setjmp 错误路径，
   归一化到 8bpp RGBA（png_set_palette_to_rgb/strip_16/gray_to_rgb/
   filler）。RGBA 字节序按小端读即 0xAABBGGRR，无需重排。
5. `CMakeLists.txt`：源文件加 vita_icon.c；链接 `png`（必须在 `z`
   之前，pngwrite 的 deflateEnd 需要 z——顺序反了会 undefined
   reference）；版本 01.28 → 01.29。

### 验证
- `./build.sh` 成功；`arm-vita-eabi-nm build/cmake/midp_vita | grep
  vita_icon_decode_png|vita_menu_draw_utf8` 确认符号进 ELF。
- 产物：`vita-port/build/cmake/midp_vita.vpk`
- 待真机/Vita3K 验证：中文游戏名列表显示、图标显示、游戏内触摸。

### 遗留风险
- MIDlet 图标常为 16x16~32x32 小 PNG，最近邻放大到 22/44px 框会有
  颗粒感（可接受，未做平滑）。
- 菜单 draw_text 的多字节路径 1:1 blit（20x22px），与 ASCII 3x 缩放
  视觉尺寸相近但混排时基线略有差异（列表场景几乎不混排）。
- JVM 内 GBK 编码的 MANIFEST（少数老 jar）：MIDlet-Name 会以 GBK 字节
  被 UTF-8 解码画成乱码/tofu。多数国产 jar 为 UTF-8，暂不做 GBK 转换。
- pen_supported 改 true 后，`Display.hasPointerEvents()` 为 true，
  某些游戏会切到触摸 UI 模式（这正是需求，但个别游戏按钮布局可能
  依赖触摸坐标校准，需真机观察）。

## 2026-09-07 v01.28 回归修复：堆回退 + 日志加固 + watchdog 体系移除（重要！）

### 背景：v01.27 回归
- 用户实测 v01.27：打开 UC 没画面、强制退出后 Vita3K 卡死、日志全空
- v01.27 把堆从 32MB→48MB、_newlib 64MB→96MB（为 UC 二次启动 OOM 扩容），疑似回归根因

### 关键实证（推翻 v01.27 归因，本段最大发现）
1. **watchdog 从未进过任何产品二进制**：
   - `vita_interpreter_heartbeat`/`vita_interpreter_dump`/`vita_vm_booted_flag`/`vita_vm_stopping_cleanly` 的**定义全在 `Interpreter_c.cpp`**（romgen 宿主工具专用，`ENABLE_C_INTERPRETER` 仅 IsRomGen=true 分支设 true）
   - ARM target 的 `_MergedSrc*.incl` 不含 Interpreter_c.cpp（v01.28 前），好库（出厂代 v0109_hybrid_pre_gcfix）与 midp 侧 libobj.a（173 成员）全量扫描**零命中**
   - 出厂 v0110 VPK（09-05 03:37）velf strings 无 `cldc_watchdog` → **v01.27"强退→Vita3K 卡死"与 watchdog exit(-3) 无关**，归因作废
2. **`Main_vita.cpp` 的 tty 镜像不会生效**：产品里 `JVMSPI_PrintRaw` 由 **midp 侧 `midp_run.o`**（`midp_run.c:491`）提供，cldc 库不带；`Main_vita.o` 恰在 rebuild_vm.sh 的 EXCLUDE 列表 → 镜像必须落到 `midp_run.c`
3. **`JVM.cpp` 的 `vita_vm_clean_shutdown` 是无定义 extern 调用点**：900dd85 加进 JVM::stop()，但定义在 romgen-only 的 Interpreter_c.cpp → 好库 005 已含 `U vita_vm_clean_shutdown`，靠 `shim_clean_shutdown.o` 成员兜底解析（v01.27 能链接成功的原因）

### 决策：回退 watchdog 体系（它在 ARM 面无定义点，从未生效也无从生效）
- 若按原计划把带 watchdog 引用的新 OS_vita.o 打进库：心跳恒 0 + booted 无置位点 → 死代码线程，一旦 booted 被置 true 就变真杀手
- 回退范围（phoneme-cldc，全部回到基线 98b12b5）：
  - `JVM.cpp`：`git checkout 98b12b5 --`（移除 clean_shutdown 调用点）
  - `OS_vita.cpp`：删除 watchdog 线程/start/宏/initialize 调用（95 行）
  - `Main_vita.cpp`：`git checkout 98b12b5 --`（PrintRaw 镜像迁移到 midp_run.c）
  - `Interpreter_c.cpp`：剥离 v01.28 加的 `vita_vm_booted_flag` 定义+置位（保留 900dd85+d206ff5 的心跳/面包屑/窗口验证，romgen-only 无害）

### v01.28 保留/新增改动
- `vita_pcsl.c`：堆 48MB→**32MB**（回退，回归根因候选）
- `vita_main.c:54`：_newlib 96MB→**64MB**（回退，与堆同步）
- `JVM_vita.cpp`（+49 行）：新增 `vita_boot_log()`（fopen("vm_boot.log","a")+vfprintf+fclose，**逐行钩子式**，幸存 Vita3K 强杀）；JVM_Start/JVM_Start2 打 classpath/main/argc、set_arguments done、每次 JVM::start() loop enter/returned
- `midp_run.c`（midp 侧）：`JVMSPI_PrintRaw` 改为**逐行 fopen/fclose 钩子式**写 `ux0:/data/vm_output.log`（原常开 FILE* 宿主侧缓冲强杀全丢，8-31 空日志事故）
- `vita_menu.c`：free(menu_fb) 改为保留分配（防 VM 堆复用后扫频花屏/"没画面"）
- `CMakeLists.txt`：VITA_VERSION 01.27→**01.28**

### 构建修复（本段第二个大坑）
- **陈旧 incl 雷**：`target/generated/incls/__MergedSrc006.cpp.incl`（09-03 03:28）比 loopgen 版多一行 `#include ".../vm/cpu/c/Interpreter_c.cpp"`（09-03/09-04 cinterp 实验遗留）→ 删除该行（恢复 34 成员）
- **Skeleton 泄漏雷**：repack 把宿主工具 stub `InterpreterSkeleton.o`/`OopMapsSkeleton.o`（空 `interpreter_dispatch_table()` 等）打进库 → 守卫拦下 → 加入 rebuild_vm.sh EXCLUDE
- **教训**：永远不要删除 `_MergedSrc*.o`（会踩 09-03 遗留坏 incl）；target 编译真实 incl 在 `target/generated` 不在 `loopgen/generated`；库成员抽取验证（ar p|nm）是发现"守卫之外静默陈旧"的唯一手段

### 验证
- `rebuild_vm.sh`：33 对象编译 OK，守卫双检通过（C 解释器符号 0、jvm_fast_globals 单 D），库 29 成员
- `build.sh`（需 `export PATH=/home/zyb/tools/jdk8u502-b07/bin:$PATH` 供 javac）：全绿，产物 `midp_vita_v0128.vpk`（v01.28 b166）
- velf 验证：`vita_boot_log` T 已解析；`vm_boot.log`/`ux0:/data/vm_output.log` 字符串在；**无** cldc_watchdog/vita_vm_booted/vita_interpreter_heartbeat/vita_vm_clean_shutdown
- 待用户 Vita3K 实测：①UC 首启是否恢复（堆回退）②强退是否还卡死 ③`ux0:/data/J2ME00001/vm_boot.log` 给出精确 breadcrumb

### 遗留风险
- 堆回退到 32MB 后 UC 二次启动 OOM 可能复发（v01.27 扩容的原始动机）——若 32MB 首启正常但二次 OOM，再二分 48MB
- 若实测仍"没画面"，优先看 vm_boot.log 的 [JVM_Start] 行与 vm_output.log 的 [tty] 行定位卡点

## 2026-09-07 用户实测确认 + `appdb/FFFFFFFF` 删除失败警告调查（良性，无需处理）

### 用户实测结果（v01.28）
- **UC 能正常进入了**（堆回退 32MB 生效，v01.27 回归修复成功）
- 启动时有一条 `remove_file: Cannot remove file: ux0:/data/J2ME00001/appdb/FFFFFFFF` 警告（`Error code: 32` 文件被占用），不影响运行

### `appdb/FFFFFFFF` 删除失败调查结论（良性，无需处理）
- `FFFFFFFF` = `INTERNAL_SUITE_ID`（-1，`suitestore_common.h:65`），`appdb/FFFFFFFF` 是 MIDP 内部 suite 的存储目录
- **MIDP 侧不可能删除它**（证据链闭合）：
  1. `midp_remove_suite`（`suitestore_task_manager.c:324`）对内部 suite 有双重保护：`get_suite_data` 找不到内部 suite（`suitestore_intern.c:327` 只匹配 `COMPONENT_REGULAR/PREINSTALLED`）→ 返回 NOT_FOUND 提前 break；即便走到 `remove_from_suite_list_and_save`（`suitestore_intern.c:1341`）也直接 `return 0` 跳过
  2. `midp_run.c`/`midpInit.c`/`main.c` 均无删除 `appdb/FFFFFFFF` 的调用
  3. "Cannot remove file"/"Error code: 32" 字符串**不在 phoneme-midp 源码**（`pcsl_file_unlink` 只返回负 Vita 错误码，不打印该文案）
- **结论**：该警告来自 **Vita3K 模拟器自身**（app 启动时同步/清理 ux0 文件系统，遇到被占用文件打印），与 MIDP/VM 代码无关。`Error 32`=文件被占用（可能是模拟器对 `appdb/FFFFFFFF` 目录内句柄未释放），删除失败后 MIDP 继续运行，UC 正常进入 → **纯良性，无需处理**
- 若日后想消除噪音：可在 `main.c` 启动时对 `appdb/FFFFFFFF` 目录做一次 `sceIoRemove` 清理（但需确认模拟器句柄已释放，否则仍失败）——当前不建议，属低优先级

## 2026-09-06 启动器循环化（游戏退出回菜单）+ UC 二次启动 OOM 堆扩容（v01.27）

### 需求与方案
- 用户需求：①游戏（MIDlet）退出后回到游戏菜单，整个 VPK 进程不退出；②UC 浏览器第二次启动报 `java.lang.OutOfMemoryError`
- 方案：`vita_main.c` main() 重构为 `for(;;)` 菜单循环——每轮 `vita_menu_run` 选游戏 → 播种 config/appdb/heap → `runMidlet`（VM 完整 start/cleanup 一轮）→ 回菜单。**进程永不退出**，同时规避 Vita3K 的 `sceKernelExitProcess → request_process_exit → on_game_closed` GUI 竞态崩溃（此前实锤的模拟器闪退链）

### phoneME 同进程 VM 重启的可行性依据（全源码确认）
- `JVM_Initialize` 注释"OK to call more than once"；`runMidlet.c` 每次调用走 `midpInitialize → midp_run_midlet_with_args_cp → midpFinalize` 完整配对
- `Universe::apocalypse()`：dispose Scheduler/Thread、flush jar caches、memset persistent handles、重置全部 bootstrap 状态标志、`ObjectHeap::dispose` free 堆 chunk（Vita 的 chunk 走 jvm_malloc=malloc，真释放）
- `midpInit.c` 注释明确"midpInitialize should be called again ... such as running MIDP in a loop"
- **pcsl 内存池再初始化安全**（压缩时遗留的最后疑点，已闭环）：vita_arm 的 `pcsl_memory_impl.h` 把 `pcsl_mem_initialize_impl(x,y)` 定义为 `(0)`、`pcsl_mem_finalize_impl` 同为 no-op 宏——分配直通 `pcsl_malloc_port`=malloc。即** vita 配置根本没有 pcsl 私有池**，midpFinalize→pcsl_mem_finalize 后再 initialize 是空操作，无泄漏无双重初始化风险

### 关键改动（3 文件）
1. `vita-port/src/vita_main.c`（核心）：
   - 循环外（一次性）：pte_osInit、mkdir/freopen/chdir、Hello.jar 拷贝、**ANI_Initialize + javacall_media_initialize**（后者每调一次创建一个常驻 tone 线程且无幂等守卫——放循环内会每轮泄漏一个线程，实测源码确认）
   - 循环内（每轮）：默认值重置 → vita_menu_run → 方向+internal.config/system.config/midp_system.jar 播种（midpFinalize 会拆属性存储，必须每轮重拷）→ setenv MIDP_HOME + midpSetConfigDir → per-game appdb（midpSetAppDir）→ setHeapParameters（Arguments::finalize 清配置，每轮重设）→ runMidlet → 回菜单
   - 移除原 `fclose(g_log); return status`——g_log 保持打开持续追加，多轮日志连续
2. `vita-port/src/vita_pcsl.c`：`JAVA_HEAP_SIZE` 32MB→**48MB**（`getInternalPropertyInt`；UC 首次能跑 32MB 但二次启动 OOM，循环模式下还需容纳 VM 重启路径）
3. `vita-port/src/vita_main.c:51`：`_newlib_heap_size_user` 64MB→**96MB**（预算：MEMSIZE 128MB 分区，48MB Java 堆 + native/VM/ROM 对象/帧缓冲 < 96MB；若仍 OOM 可再升）
4. `CMakeLists.txt`：VITA_VERSION 01.26→**01.27**

### 验证
- `bash build.sh` 全绿，产物 `vita-port/build/cmake/midp_vita.vpk`（v01.27）
- 反汇编实锤两处改动编入：`getInternalPropertyInt` 内 `mov.w r0, #50331648 ; 0x3000000`（48MB）；strings 见 `runMidlet returned %d - back to menu`
- 待用户 Vita3K 实测：①游戏退出→回菜单→再进游戏 ②UC 重进是否仍 OOM

### 遗留风险（已知未处理）
- **watchdog**：`OS_vita.cpp:326` 解释器心跳超时会 `sceKernelExitProcess(-3)`——循环模式下会杀整个启动器（连带触发 Vita3K 退出竞态）。若实测中出现"回菜单前闪退"，优先查这里
- **ROM 二次 bootstrap**：`ROM::dispose()` 后 persistent handles 重填路径未经实测（apocalypse 有 memset+标志重置，理论安全）
- **runMidlet 内部 appdb 覆盖疑点**：`runMidlet.c:134` 内部 `midpSetAppDir(getApplicationDir())` 可能用 MIDP_HOME/appdb 覆盖我们的 per-game 路径——但历史日志显示 appdb_1A35 生效，需实测 RMS 隔离确认

## 2026-09-05 音乐开关卡死第三轮根因：ENABLE_JSR_135 未生效的旧 .o（重要！）

### 根因链（反汇编实锤）
- v01.08/v01.09/v01.10 三轮"音乐开关卡死"的最终根因：`midp_master_mode_events.o`
  是 **08-28 03:12** 编译的——当时构建配置还是 `USE_JSR_135=false`。
- 09-01 提交 9240db2 启用 `USE_JSR_135=true`（jsr135 subsystem.gmk 会注入
  `-DENABLE_JSR_135=1`），但 **GNU make 不跟踪 CFLAGS 变化**，旧 `.o` 原样打入 libobj.a。
- 结果：`midp_check_events` 的 switch 里 `#if ENABLE_JSR_135 case MEDIA_EVENT_SIGNAL:`
  分支被编译剔除，跳转表第 36 项（signal-1 索引）指向 default 丢弃分支。
- 完整死锁链：音乐开关 → tone Player 播完 → `javanotify_on_media_notification(END_OF_MEDIA)`
  → vita_media_notify.c SPSC ring → `vita_media_poll` 取出 → `checkForSystemSignal` 设
  `waitingFor=MEDIA_EVENT_SIGNAL` → **midp_check_events switch 丢弃** → Java
  EventDispatcher 永远收不到 END_OF_MEDIA → 卡死。
- 验证方法（以后排查同类问题的模板）：
  `arm-vita-eabi-objdump -d midp_master_mode_events.o`，数 `StoreMIDPEventInVmThread`
  调用点（旧 .o 只有 1 处=UI_SIGNAL；新 .o 2 处），读 switch 跳转表
  `ldrls pc, [pc, r3, lsl #2]` 的第 36 表项是否指向 memcpy→StoreMIDPEvent 分支。

### 修复
1. `rm build/vita_arm/obj/arm/midp_master_mode_events.o` 后 `bash build_vita.sh` 重编
   ——同类问题还有 `midpServices.o`（`ENABLE_JAVA_DEBUGGER` 的 `midp_isDebuggerActive`
   同样缺失，链接报 undefined 才暴露），一并删除重编了 midpEvents/midpEventUtil/midpInit/
   midpMidletSuiteUtils/heap/lfj_cskin。
   **教训：构建配置里的条件编译宏变更后，必须删所有受影响 .o 强制重编，make 不会替你做。**
2. libmidp.so 链接仍失败（`_rom_linkcheck_mffd_false` 未定义 + `_in_gc_state` 重复定义，
   混合 VM 库与 .so 链接方式不兼容）——**不影响我们**：vita-port 只消费 `obj/arm/libobj.a`，
   该归档已含修复后的 .o（已用 ar tv 验证成员时间戳）。
3. **教训：make 单对象重编要先 rm 旧 .o** 这条旧规则同样适用于"配置变更"场景，且
   `USE_*` 布尔开关的变更影响面要用 `grep -rln "ENABLE_XXX" src/ --include="*.c"` 排查。

### 2026-09-05 romgen 修复（同日连环事故）
- `build/vita_arm/dist/bin/romgen` 被写坏成全零文件（09-04 05:00 事故残留），
  08-31 时代的 `romgen/generated/ROMImage.cpp` 也被某次 romize 输出覆盖成 include 壳
  （1509 字节，丢失 ROMSkeleton 的 `romgen_check_oopmaps`/`_rom_constant_pool` 定义）。
- 修复步骤（可复用）：`export PATH=/usr/bin:/bin:$PATH`（关键！PATH 里的 vitasdk
  gcc/as 是 ARM 交叉工具，会污染宿主构建）→ `export JVMWorkSpace=$CLDC` →
  `cd build/vita_arm/romgen && cp src/vm/share/ROM/ROMSkeleton.cpp generated/ROMImage.cpp`
  → `cd app && rm -f ROMImage.o && make`。产物为 32 位 i386 ELF（pie），与 32 位 ARM
  目标指针宽度一致，romize 的 slice 算法才不会断言（64 位 romgen 在
  ObjectHeap.cpp:3511 断言崩溃——指针宽度不兼容，不可用 64 位版替代）。
- 注意：`romgen/app/` 里曾混入 ARM 对象（Interpreter_arm_arm.o/ani.o/os_port.o/
  poolthread.o/_MergedSrc006.o 旧代），手工 `g++ -m32 *.o` 重链会失败，正确方式是
  走 jvm.make 的正规 `$(ROM_GENERATOR)` 规则（它会重编缺失 TU 并按 Obj_Files 链接）。

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

### 2026-09-05 v01.10：音乐开关卡死真凶 #2——GC encode 无门控转储洪水（最后一个）
- **用户实测 v01.09 重发版（b162）**：✅ 能进游戏、CP tags 洪水消失（上轮修复生效）；❌ 音乐开关仍卡死。
- **根因**：`ObjectHeap.cpp:2134` 的 **GC 压缩编码转储无任何门控**——每次 GC 压缩对**每个存活对象**打一行 `GC encode:`。音乐开关 → MMAPI 类链加载 → 分配压力触发 GC → 数千对象 = 数千行 stderr → Vita3K 慢 I/O = "卡死"。与 CP tags 洪水**同一故障模式、不同源头**。用户日志铁证：洪水内容从 CP tags 变成了 GC encode。
- **不是存储问题**：日志里 RMS/文件系统零异常，卡死瞬间恰好是 GC encode 洪水开始处。
- **修复**：①`ObjectHeap.cpp` 加 `#ifdef VITA_GC_DEBUG` 门控（cldc b82a81f，与 2959a08 同款最小补丁）②`tools/build_merged_o.sh` 新工具：按库内同款 PRODUCT ABI 旗标重编单个 `_MergedSrcNNN.cpp` 作替换成员③重编 003（GC encode 所在成员）移植进库。
- **全库终验**：转储串（GC encode/CP tags/PRE idx/NOT in heap/var_oops_do）全 0、`D jvm_fast_globals` 唯一、C 解释器符号 0。ELF：`81381ae0 D jvm_fast_globals`。
- **产物**：`samples/j2me/midp_vita_v0110.vpk`，md5 `08ab999d33e9bdbbe440fce9bc312e2e`，版本串 `v01.10 b164 (8d02608)`。
- **源码 dump 审计（本轮已做）**：`grep fprintf(stderr` 全量过一遍，ClassFileParser 全部门控、JavaDebugger 仅 KDWP 异常路径（不可达）、ObjectHeap 已修——**已知无门控洪水源清零**。若音乐开关仍卡，stderr 应已干净，届时转向 vita-port 层 MMAPI 状态机排查（`vita_audio_javacall.c`，看 `midp_stderr.log` 定位）。
- **库备份**：`libcldc_vm.a.v0109_hybrid_pre_gcfix`（GC 修复前）、`.v0109_hybrid`（混合库初版）、`.kg_puredumps`（纯 KG）。

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

### 2026-09-05 音频链路深挖：Manager 双版本矛盾 + ToneTest v3
- **midp_system.jar 是陈旧存根**（8-31 构建，早于 9-01 的 USE_JSR_135=true）：
  `src/media/reference/classes/.../Manager.java` 的 createPlayer 直接 throw
  MediaException、playTone 空操作。jar 中 com/sun/mmedia 只有 10 个内部视频类。
  **但 ROM（9-05 重建）已含完整 jsr135 实现**（DirectTone/HighLevelPlayer/
  NativeTonePlayer/DirectPlayer…，最终 ELF strings 验证）——ROM 化类优先于
  classpath jar，运行时系统类实际来自 ROM，音频 Java 链路是好的。jar 存根
  被 ROM 遮蔽，不构成运行时 bug，但掩盖排查视线；重建 classes.zip 时需注意
  `Options.gmk`（USE_JSR_135=false）与 `Settings.gmk`（true）的覆盖顺序。
- **v01.12 用户实测"按键没反应"再分析**：日志证实菜单写入 game.cfg（18B =
  "ToneTest\nportrait\n"）且 VM 打开 games/Hello/game.jar——**新 jar 确实在跑、
  类名正确**，替换操作成功。midp_stdout.log 为空是 v2 设计盲区：第一句日志要
  等按 CROSS 才产生，"ToneTest 没起来"与"VM 冻结/事件泵死"无法区分。
- **ToneTest v3（自驱动）**：ctor+startApp 即打 banner（"[TONE] ctor v3"）、
  1Hz 心跳（hb 计数器，每 10 次落 stdout）、t=2.5s 起每 2.5s 自动推进一阶段
  （无需按键）、TRIANGLE/CROSS 仍可干预、60s 总看门狗自动退出。一次运行即可
  分流：无 banner=类加载失败；banner 停、hb 停=VM 冻结；hb 走而按键无
  lastKey=输入链路断；stg1-5 最后一条日志=音频卡点。
- VPK v01.13（b166, 1c4730e 链）产物 vita-port/build/cmake/midp_vita.vpk，
  含 v3 ToneTest（ctor v3 / alive # 标记已验证在 class 内）。
- **v01.14：默认启动类改为 ToneTest**（vita_main.c 默认 class_name，音频排障
  期间临时）。理由：启动器每次启动都把 VPK 内 Hello.jar 重新复制到数据目录
  （copy_file(app0:...)），默认路径永远跑最新构建，用户操作简化为
  "装 VPK → 启动 → 三角退出菜单"，不再涉及 games/Hello/ 副本（VPK 升级不
  覆盖已安装 games/ 副本的坑被整个绕开）。注意 launch.cfg 若存在仍会覆盖
  默认值——排障期间确认用户机器无 ux0:/data/J2ME00001/launch.cfg。
  排障结束后改回 "HelloMIDlet"。

### 2026-09-05 音乐卡死真根因：链接顺序遮蔽 vita 音频实现（v01.15 修复）
- **v01.14 实测**：ToneTest v3 完美运行（ctor banner → 60s 心跳全活 →
  60s 看门狗正常退出），**VM 无冻结、事件泵正常**；此前"按键没反应"实为
  v2 盲区。stg1 稳定抛 `MediaException: Failed to play tone`（非存根消息，
  证明 ROM 里的 jsr135 真实现已在跑）。
- **根因**：`phoneme-midp/build/vita_arm/pcsl/vita_arm/lib/libpcsl_multimedia.a`
  是 `gen_stubs.py` 生成的**全桩 archive**（所有 javacall_media_* 直接
  return JAVACALL_NOT_IMPLEMENTED）。CMake `target_link_options()` 永远排在
  对象文件**之前**，导致链接顺序 = whole-archive(libobj) → libpcsl_multimedia
  → 端口层 .obj；`--allow-multiple-definition` 下未定义符号从先出现的桩
  archive 解析，`vita_audio_javacall.c` 的真实现被静默丢弃。KNI 把
  NOT_IMPLEMENTED 映射为 RESULT_FAIL → "Failed to play tone"。
  **audio_debug.log 从未出现同因**——真实现代码从未执行。
- **验证手段（可复用）**：`nm` + `objdump -d` 看 ELF 中 `javacall_media_play_tone`
  的函数体：桩 = 7 行直接 `mvn r3,#1`（return -2）；真实现 = push r4-r6 +
  movw 取字符串地址（ALOG 日志调用）。
- **修复**：CMakeLists.txt 把 whole-archive 块 + 库 group 从
  `target_link_options` 移入 `target_link_libraries`（CMake 自身对象永远
  在 libraries 前），链接顺序变为 端口层 .obj → libpcsl_multimedia.a。
  修复后 play_tone/create/start/stop 全部落在 0x8100xxxx 端口层区域。
  **不能删除桩 archive**：libobj 的 KNI 对象还引用其中的 javautil_* 字符串
  工具与录音/MIDI 函数（126 个符号中 vita 实现只覆盖 31 个）。
- 遗留：ToneTest 异常路径不推进 stage（stg1 失败后重试到 60s 看门狗），
  属测试代码行为非缺陷；游戏音乐是否依赖 EOM 桥待 v01.15 实测。
- **v01.15 实测：链接修复完全生效**——stg1 playTone 返回 OK（不再抛异常）、
  stg2 createPlayer OK、stg3 realize/prefetch OK，native 音频链路全通。
  stg4 抛 `IllegalStateException: cannot set seq after prefetched` 是
  ToneTest 自身 bug（MMAPI 要求 setSequence 在 realize 后、prefetch 前，
  v3 放在了 prefetch 后）。
- **v01.16**：ToneTest 修正调用顺序（realize → getControl+setSequence →
  prefetch → addPlayerListener → start → 等 EOM）。待实测 stg4/5。
- **v01.16 翻车：全类 CNFE**——同一启动路径 v01.15 能解析 jar、v01.16 抛
  `ClassNotFoundException: ToneTest`。本地与 VPK 内 jar md5 一致、zip 结构
  校验全过（EOCD 校验/CD 遍历/CRC），两版仅差 8 字节 deflate 输出。
  根因判定：JDK `jar` 工具写的是 **streamed zip**（LFH crc/sizes=0 +
  data descriptor，flag=0x0808），phoneME `JarFileParser` 对该布局的偏移
  处理有脆弱边界，特定压缩输出恰好踩中（open_entry 用 CD 的 locOffset+
  CENOFF 寻址 LFH 后按 CENSIZ 解压——任何 DD 吞吐错位即崩）。
  **修复（v01.17）**：`build_jar.sh` 打包后追加 python 规范化步骤——丢弃
  目录桩条目、以固定时间戳重写为**非 streamed** zip（LFH 带 crc/sizes，
  flag=0x0000，无 DD）。此类布局 phoneME 上游 romgen/JAR 工具链即默认格式。
  **教训：交付给 phoneME 的 jar 一律规范化，勿直接用 JDK jar 输出。**
- **v01.17 仍 CNFE → 规范化假设被证伪**。停止猜测改分流实验（v01.18）：
  ①默认类改回 HelloMIDlet（自 v01.13 未变）——若它也 CNFE 则是 jar/环境
  问题，若正常则问题特定于 ToneTest 类字节；②启动日志加
  "runtime Hello.jar size=N (expected M)"（HELLO_JAR_SIZE 烘进
  vita_version.h，CMake file(SIZE)），排除复制截断。
  注意：GenVersion.cmake 新增 HELLO_JAR_SIZE 参数；CMake 中 file(SIZE)
  必须在 gen_version 目标之前（MIDLET_JAR 定义相应上移）。
- **v01.18 决定性数据：runtime jar=9460B ≠ VPK jar=9204B → 根因找到**。
  HelloMIDlet 也 CNFE（排除类字节问题）。9460 恰是 v01.15 jar 尺寸：
  `copy_file` 用 `SCE_O_TRUNC` 覆盖写，但 Vita3K 不实现截断——新 jar
  (9204) 比旧 (9460) 短，写入后旧文件尾巴残留，长度仍 9460。
  JarFileParser 的 EOCD 校验（endpos+22+com==len）对 corrupted 长度
  全部失败 → 整个 jar 不可解析 → 全类 CNFE。
  时间线完全吻合：v01.14→15 新 jar 更长（全量覆盖无残留）→ 能跑；
  v01.15→16 起新 jar 更短 → 残留尾巴 → CNFE。
  **修复（v01.19）**：`copy_file` 改为先 `sceIoRemove(dst)` 再
  `O_CREAT` 打开，不依赖 O_TRUNC；默认类改回 ToneTest（继续 stg4/5
  EOM 桥验证）。
  **教训：Vita/newlib 的 sceIo O_TRUNC 不可靠（Vita3K 实测不截断），
  覆盖写文件必须先 remove 再 create。**
- **v01.19 实测：CNFE 修复确认**（size=9204 匹配、ToneTest 注册 OK、
  stg1-4 全过）。新发现：`playerUpdate: started/stopped/closed` 事件
  **已到达 Java 监听器**——MMEventHandler 派发机器是通的。
  但 EOM 环节测试无效：stage 4 也被 2.5s 自动推进跳过（音序 ~2.25s，
  跳过与 EOM 到达赛跑）→ "stg5: skipped, eom=false" 不说明任何问题。
  **v01.20**：自动推进只覆盖 stage 0..3；stage 4 只能由 EOM 到达或
  15s 看门狗结束；CROSS 仍可手动跳过。
- **v01.20 实测：音频出来了**（链接修复彻底生效），但 **EOM 仍 STUCK、
  按键无响应、60s 退出时 Vita3K 闪退**。症状指纹：Java 内部事件
  （started/stopped/closed，不跨 native）全通；native→Java 注入事件
  （按键、EOM，都走 checkForSystemSignal）全灭。
  **根因（第三次链接遮蔽！）**：`libcldc_vm_ani.a` 的
  `anilib/share/ani_bsd_socket.cpp` 定义了**强符号** `JVMSPI_CheckEvents`，
  函数体只处理调试器 + `ANI_WaitForThreadUnblocking`，**从不调
  midp_check_events**；而 `midp_run.c` 里桥接事件泵的版本是 weak ——
  强符号静默获胜，`checkForSystemSignal` 从未运行。这也是按键从
  ToneTest v1 起就"没反应"的真正原因。
  **v01.21 修复**：新增 `src/vita_checkevents.c` 强符号版：先
  `midp_check_events(bt, n, 0)` 泵事件，再 `ANI_WaitForThreadUnblocking`
  （timeout 钳到 ≤50ms 保证响应）。CMake 对象先于库 →
  `--allow-multiple-definition` 先到先得。ELF 已验证：
  0x81006544 先 blx midp_check_events 再 b.w ANI_Wait。
  **教训：电话本移植三大链接坑——gen_stubs 桩 archive、mastermode_export
  桩、ANI 强符号，全部用同一手段（项目对象放前面）压制。**
  遗留：60s 退出闪退（EXCEPTION_ACCESS_VIOLATION）待 EOM 验证后再查。
- **v01.21 实测全绿**：按键（TRIANGLE 退出到达）、EOM（`EOM RECEIVED -
  bridge OK`）、音频全部工作。事件桥闭环。
- **游戏启动报错分析（三例）**：GameMidlet OK；UC WebClient NPE；
  FileManagerMIDl CNFE。两个根因：
  1. **internal 套件无属性**：`CldcMIDletSuiteLoader.createMIDletSuite()`
     只在 `args[0]` 以 .jar/.jad 结尾时才调 getJarProps/getJadProps 装载
     MANIFEST 属性，否则空 Properties → `getAppProperty()` 全 null →
     框架型应用（UC 等）startApp 读属性即 NPE。
  2. **MANIFEST 类名与 jar 不符**：`FileManagerMIDl`（结尾小写 l）在
     jar 里根本不存在 → Class.forName CNFE。国产 jar 常见。
  **v01.22 修复**：
  1. `vita_main.c` run_argv 增加第 6 参数 jar 路径（arg0 槽位：
     -classpathext 摘除后落在 argv[3]）→ internal 套件装载 MANIFEST
     属性。注意路径必须相对（cwd=DATA_DIR），JarReader native 走
     midpOpenJar 与类装载器同源。
  2. `vita_menu.c` 新增 `zip_walk_names`（中央目录名遍历回调）、
     `verify_class_in_jar`（<class>.class 存在性校验）、
     `resolve_class_in_jar`（兜底挑选：*MIDlet=3 > *Midlet=2 > 顶层类=1，
     排除 $ 内部类与 META-INF，同分短 FQN 优先）、`validate_game_class`
     （扫描+安装两处接线，修正后写回 game.cfg）。宿主侧等价测试
     三项全过。
- **v01.23（属性装载的安全时序修复）**：v01.22 实测 UC 仍 NPE。日志
  分析：`CldcMIDletSuiteLoader.createMIDletSuite()`（runMIDletSuite 第
  286 行）读 jar MANIFEST 发生在 `initSuiteEnvironment()`（第 303 行，
  AccessController.setAccessControlContext）**之前**——上游 internal
  套件从不带属性所以无此路径；我们的 arg0=.jar 修复触发了
  `JarReader.readJarEntry → AccessController.checkPermission`（读
  javax.microedition.io.Connector.http 权限）在 context==null 时抛
  SecurityException，被 getJarProps 的 catch(Throwable) 吞掉 →
  props=null → 空属性 → NPE 依旧。
  **修复**：`phoneme-midp` 的
  `src/security/access_controller_cldc_port/classes/com/sun/j2me/security/AccessController.java`
  checkPermission(name,resource,extraValue) 的 `context == null` 分支
  从 throw SecurityException 改为 return（附 VITA 注释：bootstrap 窗口
  只有受信 loader 代码运行，MIDlet 代码必在其套件 context 设置之后，
  安全性不受影响——MIDlet 代码不可能在 createMIDletSuite 阶段执行）。
  **ROM 再生链（本次跑通）**：`phoneme-midp/build_vita.sh`（make
  GNUmakefile 默认目标）依赖链自动完成：
  AccessController.java → midp_classes/classes.zip（javap 验证
  `ifnonnull; return`，异常字符串已消失）→ `$CLDC_DIST_DIR/bin/romgen`
  → build/vita_arm/ROMImage.cpp → obj/arm/ROMImage.o → obj/arm/libobj.a。
  注意：构建尾部 `bin/arm/libmidp.so` 链接报 multiple definition（库内
  _MergedSrc002/003 重复符号）属**已知无害失败**——vita-port 用的是
  obj/arm/libobj.a（已成功再生），不用 libmidp.so。
  **产物**：`vita-port/build/cmake/midp_vita.vpk`（v01.23，1430 万字节）。
  **FileManagerMIDl 的 JSR-75 结论**：类名兜底已生效（日志显示解析为
  com.app.filemanager.FileManagerMIDlet），但启动报
  NoClassDefFoundError: javax/microedition/io/file/FileConnection——
  JSR-75（FileConnection）在 phoneme_source 全树无源码（只有
  jsr120/135/172/177/211/239/280），phoneme-midp/src/protocol/file 也
  只有 storage 辅助类，无 FileConnection 接口。build_vita.sh 一直
  USE_JSR_75=false（源码不存在）。**实现 JSR-75 是大工程，本阶段
  搁置**；该游戏无法简单修复，需向用户说明。
- **v01.24（"JAR not found"修复——pcsl 相对路径 vs sceIo）**：v01.23
  实测 UC 仍 NPE，但异常已换形：`JarReader.readJarEntry → IOException:
  JAR not found`（SecurityException 消失证明 AccessController 放行
  生效，暴露下一层）。根因是**两条文件通路的相对路径语义不一致**：
  类装载走 `ClassLoader → OsFile_vita.cpp → jvm_fopen`（newlib
  fopen，用进程 cwd——vita_main.c chdir(DATA_DIR) 后能解析
  `games/app/game.jar`）✅；而 JarReader 走
  `midpOpenJar → storage_open → pcsl_file_open（vita_pcsl.c）→
  裸 sceIoOpen("games/app/game.jar")`——**Vita 的 sceIo* 不支持
  相对路径**（无 cwd 概念，必须 ux0:/app0: 绝对前缀）→ 打开失败
  → IOException → props 仍为空 → NPE 依旧。
  **修复（vita-port/src/vita_pcsl.c）**：新增 `vita_resolve_path()`
  （含 `:` 或以 `/` 开头视为已绝对，否则前缀 `VITA_DATA_ROOT =
  ux0:/data/J2ME00001`，与 vita_main.c 的 DATA_DIR 保持同步），
  应用于 `pcsl_file_open`（主修复点）与 `pcsl_file_unlink` /
  `pcsl_file_exist` / `pcsl_file_sizeof`（同病症预防）。
  app0: 兜底逻辑保留（VPK 内只读资源仍走 app0 前缀）。
  产物：vita-port/build/cmake/midp_vita.vpk（v01.24，14296404B）。
  **教训：vita-port 的 pcsl_file_* 层是唯一不经 newlib cwd 的文件
  通路，任何相对路径入口都必须显式解析到 ux0: 绝对路径。**
  调试便利：pcsl_file_open 有 debug_log（ux0:/data/debug_log.txt），
  实测可看 [file_open] path= 行确认路径解析。
- **v01.25（"JAR Corrupt"修复——pcsl_string UTF-8 桩语义全错）**：
  v01.24 实测异常换形为 `JAR Corrupt`——路径修复已生效（open +
  getJarInfo 的 EOCD 定位/大小校验/PK 头验证全过，那些失败会报
  "not found"），死点推进到 midpGetJarEntry。
  **根因（我们自己写的桩）**：`vita_pcsl.c` 的
  `pcsl_string_utf8_length` 返回 `str->length * 3`（"粗估"）——
  `midpGetJarEntry` 用它做条目名长度，与 `findJarEntryInfo` 的
  `entry.nameLen == nameLen` 精确比较：`META-INF/MANIFEST.MF`
  20 字符被算成 60，永远匹配不上 → CD 遍历越过最后一个条目 →
  在 EOCD 位置读 46 字节（CENHDRSIZ）只剩 22 → JAR_CORRUPT。
  同时 `pcsl_string_get_utf8_data` 返回静态 buffer（非上游契约的
  malloc 缓冲），`pcsl_string_release_utf8_data` 不 free → 泄漏。
  **修复（vita-port/src/vita_pcsl.c）**：
  1. `pcsl_string_utf8_length`：真实 UTF-8 字节数（1/2/3 字节
     按码位分类，ASCII 1:1），NULL 返回 -1（上游契约）。
  2. `pcsl_string_convert_to_utf8`：真 UTF-16→UTF-8 编码（多字节
     序列、NUL 结尾、BUFFER_OVERFLOW 判定），替换原 & 0xFF 截断。
  3. `pcsl_string_get_utf8_data`/`release_utf8_data`：malloc 配对
     free，符合上游契约。
  4. `pcsl_file_open/unlink/exist/sizeof` 路径转换统一走
     convert_to_utf8（中文目录名不再被 & 0xFF 弄坏）。
  5. 用户指正：vita_resolve_path 的 VITA_DATA_ROOT 硬编码改为
     运行时 getcwd()（跟 vita_main.c 的 chdir 联动，仅 getcwd
     失败时才回退字面量）。
  **教训：vita-port 的 pcsl_string 桩不是"能编过就行"——上游
  API 有精确语义契约（长度精确性、缓冲所有权），中游代码
  （jar 条目匹配、installer、suitestore）依赖这些契约。写桩
  前先读上游 pcsl/string/utf16/pcsl_string.c 的对应实现。**
  产物：vita-port/build/cmake/midp_vita.vpk（v01.25，14296493B）。
- **v01.26（RMS 持久化修复——"首启正常、之后一直失败"根因）**：
  v01.25 实测 UC 的 "JAR Corrupt" 异常消失（属性链全通），但 NPE
  仍在 `ao.a() bci=4 ← aa.c() ← aa.b() ← aa.h() ← WebClient.startApp()`
  与 v01.24 完全相同。用户提供决定性时间线："**一开始安装的时候有
  正常打开启动过，后面就一直不行**"。
  **根因（RMS 持久化两个实锤桩缺陷）**：
  1. `pcsl_file_truncate` 空操作但返回 0（伪成功）：
     `RecordStoreImpl.compactRecords()`（RecordStoreImpl.java:886）
     删记录后靠 `dbFile.truncate(getSize())` 收缩 db 文件——空操作
     导致文件头 data size 与物理内容错位、陈旧块残留 → 二启
     `getRecordIDs()/getRecordHeader()` 按偏移遍历读出脏数据 →
     UC 混淆代码 `ao.a()` 拿到 null → NPE。**首启无 RMS 数据不需
     truncate，所以首启正常**。
  2. `pcsl_file_rename` 恒返回 -1：suite 存储写路径
     `write_file()`（suitestore_intern.c:497）用"写临时文件→改名"
     模式提交——rename 恒失败导致数据滞留 .tmp、目标文件永远
     不更新。
  连带发现：`vita_storage.c` 的 `get_per_game_appdb` 只匹配
  `"/games/"`，而 vita_main.c 传入相对路径 `games/app/game.jar`
  （无前导斜杠）→ 匹配失败 → 所有游戏退回共享 "appdb"（日志实证
  `[RMS] appdb: ux0:/data/J2ME00001/appdb`），per-game RMS 隔离
  从未生效。
  **修复（vita-port/src/vita_pcsl.c + vita_storage.c）**：
  1. `pcsl_file_truncate`：读-重写方案（Vita 无 sceIoFtruncate）——
     读出前 size 字节到 malloc 缓冲 → sceIoClose → 同路径
     sceIoOpen(SCE_O_RDWR|SCE_O_CREAT|SCE_O_TRUNC) → 写回 →
     文件位置恢复（截到 size）。重开依赖 `vf->path`（open 时记录
     的解析后绝对路径），已补 strncpy 后的显式 NUL 终止。
  2. `pcsl_file_rename`：双路径 convert_to_utf8 → vita_resolve_path
     → sceIoRename（fcntl.h:166 存在）。
  3. `vita_storage.c`：新增 `find_games_seg()` 同时匹配 "games/"
     开头（相对）与 "/games/"（绝对），per-game RMS 隔离生效。
  排查中确认无害的链：free space（totalSpace 默认 4MB、used 恒 0
  → free=4MB，RMS 创建不被挡）；`vita_stubs.c` 里的旧 pcsl 副本
  是死代码（未链接，修复时勿改错文件——objdump 验证 ELF 中是
  vita_pcsl.c 的新实现）。
  **验证**：构建成功；objdump 确认 pcsl_file_truncate 内含
  sceIoLseek/Read/Close/Open/Write 完整读-重写链、
  pcsl_file_rename 内含 sceIoRename。
  **用户验证步骤**：装 v01.26 后**先删除 Vita3K 侧**
  `ux0:/data/J2ME00001/appdb` **目录**（清除旧损坏 RMS 数据），
  再启动 UC——预期首启正常且二次启动也正常。若删 appdb 后仍
  NPE，则 RMS 假设被削弱，需把 game.jar 复制进容器做字节码级
  分析（javap -c 反汇编 ao.a() 的 bci=4）。
  **教训：桩函数"返回 0"不等于无害——truncate 伪成功比直接失败
  更危险（错误被静默吞掉，损坏延迟到下一次读取才爆发）。写桩
  前必须对照上游消费者（RecordStoreImpl/suitestore_intern）的
  语义契约。**
  产物：vita-port/build/cmake/midp_vita.vpk（v01.26）。

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

## 2026-09-09 v01.44：Error 32 终局定案（b195, 22f5ffc）

### 用户 vita3k.log 全量取证结论（17:49-00:02 会话，~2 万行）
1. **设备上的二进制不是 b194**。指纹：日志中每次文件 open 前后出现
   `debug_log.txt` 的 64B+22B 写入 = vita_pcsl.c `VITA_FILE_DEBUG=1` 的
   `[file_open] path=ux0:/data/J2ME00001/appdb/FFFFFFFF flags=0x2`（64B）
   + `[file_open] ok direct\n`（22B）。v01.38 (44e7e71, 9-08 12:12) 才把
   该开关置 0，b194 ELF 无 "debug_log.txt" 字符串。用户 17:49/20:17 装的
   VPK 是 pre-v01.38 的 debug 构建（8-31 04:30 时代）。
2. **Error 32 的真实删除目标是目录本身**：`ux0:/data/J2ME00001/appdb/FFFFFFFF`
   （suite id 目录，无文件名后缀）。链路：midp_remove_suite 清理循环 →
   storage_get_next_file_in_iterator → 我们的 pcsl_file_getnextentry
   不跳过子目录 → FFFFFFFF 目录被当文件返回 → storage_delete_file →
   pcsl_file_unlink(目录)。Windows 下目录被 RMS 句柄锚定必然拒绝 →
   Error 32 ×2（ux0 + Vita3K 内部 app0 fallback）。
3. **崩溃日志也是旧二进制的**：00:01:37.746 EXCEPTION_ACCESS_VIOLATION
   PC=0x8103350e（用 b194 addr2line 解析为 Graphics_drawArc，但旧二进制
   地址空间不同，仅供定位参考）。Vita3K exception_handler 记录后进程继续，
   崩溃后句柄生命周期错乱 → remove 执行 → Error 32。v01.42 后未再复现 6h
   闪退，b194/v01.44 上是否仍崩需复测。
4. **正常退出路径（17:56 会话）同一旧二进制无 Error 32**：v01.37 evict 语义
   命中后 remove 重试成功。Error 32 只在崩溃/异常路径出现。
5. b194 的 v01.43 预检查本身工作正常（拦截活句柄路径），但覆盖不到
   "目录被当文件删" —— 这是 v01.44 (22f5ffc) 修的：getnextentry 加
   `entry.d_stat.st_attr & SCE_SO_IFDIR` 跳过（与 getsizeoffilelist 同款过滤）。

### 验金方法（防再犯）
- 判定设备二进制版本：看菜单右上角 `vXX.XX bNN (hash)`，或 vita3k.log 中
  有无 per-open `debug_log.txt` 写入（有 = pre-v01.38）。
- b194 产物验证：`strings build/cmake/midp_vita | grep debug_log.txt` = 0。

## 2026-09-10 v01.45：b195 残余 Error 32 定案（truncate 换file踩孪生句柄）

### 用户 vita3k.log 铁证（23:55:33）
```
|E| [remove_file]: Cannot remove file: .../rms/appdb_1A35/FFFFFFFF
|E| [remove_file]: Error code: 32
|W| [io_error_impl]: remove_file (sceIoRemove) returned 0x80010002
```
sceIoRemove **真的执行了** → 不可能来自 pcsl_file_unlink（v01.43 预检查
拦截时不发 syscall）。穷举全部 7 个 sceIoRemove 调用点后锁定：
**pcsl_file_truncate 的 remove+create 换file**（pcsl_file_open 的 O_TRUNC
仿真同款问题）。此前的"b195 不会再有 Error 32"判断是错的。

### 根因：空名 store 的 db/idx 孪生句柄同路径
- `buildSuiteFilename`（rms.c）只在 store 名非空时追加 `.db/.idx` 后缀；
  nameLen==0 时**忽略 extension 参数** → db 句柄和 idx 句柄打开的是
  **同一条路径** `appdb_XXXX/FFFFFFFF`（RecordStoreImpl 构造 dbFile 后
  立即构造 RecordStoreIndex → idx 文件；POSIX 下同文件双开合法）。
- truncate 换file流程只关自己的 fd → sceIoRemove → **孪生 idx 句柄仍
  锚定名字** → Vita3K（_wfopen 无 FILE_SHARE_DELETE）拒绝 → Error 32。
- v01.41 的后验检查已把 swap 判失败返回 -1（fd 落回旧文件），syscall
  却已发出 → 日志噪音；且面包屑去重只按路径 → 该路径启动时已报过
  "unlink blocked"，23:55 的 "truncate blocked" 被**静默吞掉**，
  stderr 与 vita3k.log 对不上号。

### v01.45 修复（71a6ce0）
1. 新增 `vita_handles_held_ex(path, self)`（排除自己的 held 检查）：
   `pcsl_file_truncate` 与 O_TRUNC 仿真在换file**前**预检查，孪生句柄
   存在 → 直接 -1 + 面包屑，不发注定失败的 syscall，不再空关 fd。
2. `vita_report_held` 去重键改为 **op+path**（原只按 path，会吞掉
   同路径不同操作的证据）。
- 对调用者返回值与 v01.41~44 一致（-1）；真机 POSIX 语义下该 swap 本可
  成功，现被预检查拦下属保守策略——真机实测若有 truncate 失败再议。

### 教训（防再犯）
- **"不会再有 Error 32" 类断言前必须穷举 syscall 出口**：预检查只护住
  pcsl_file_unlink，truncate 换file / O_TRUNC 仿真两处直发 sceIoRemove。
- 面包屑去重维度必须包含操作类型，否则吞掉后续不同操作的证据。
- stderr 面包屑与 vita3k.log 时间线**对不上号本身就是线索**（本案：
  23:55 有 syscall 无面包屑 → 去重吞了或来源未埋点 → 两样都查）。

## 2026-09-10 v01.45 后续：第二次进 UC 崩溃定案（JIT 二次启动，-int 隔离）

### 崩溃现场（用户装 b200 复测，11:01:16.302）
第一次进 UC 正常，退出回菜单后**第二次进** → Vita3K
`EXCEPTION_ACCESS_VIOLATION 0xC0000005, Read 0x4E5060004`（35 位地址），
PC=0x85f05fd8（**JIT 代码缓存区 = Java 堆 compiler_area**），fault 指令
`ldr r4,[r5,#0x404]`；随后 PC=0x0 无限刷屏 → 闪退。

### 指令链解析（全部坐实，非猜测）
- r5=0x813b17d4=`gp_base_label`（nm 直读运行时地址），+0x404=
  `_compiler_stack_limit`(0x813b1bd8，合法地址)。
- `ldr r4,[r5,#0x404]` 出自 `CodeGenerator_arm.cpp:1190` JIT 序言
  "stack overflow + timer tick" 检查；GP 访问宏在
  `SourceAssembler_arm.hpp`（`ldr_gp_base`→`ldr_label("gp_base_label")`）。
- **fault 地址 0x4E5060004 ≠ r5+0x404** → 寄存器转储是损坏后状态；
  真相是执行流已跳进坏代码（垃圾字节把自己解码成同样的 ldr 形态）。
- JIT 编译代码不是独立 code heap：`Compiler::allocate_and_compile` →
  `Universe::new_compiled_method` 分配在**堆内 compiler_area**
  （`Compiler.cpp:1073` reserve_compiler_area），PC=0x85f05fd8 落在
  堆范围完全吻合 → "跳进已失效/被覆盖的 JIT 代码"。

### 跨轮状态排查结论
- `Universe::apocalypse()` 不复位 Compiler 静态状态，但每轮
  `Universe::bootstrap()` 会 `CompiledMethodCache::init()` +
  `Compiler::initialize()`（memset 全部 _state），跨轮复位基本完整。
- 第二轮 JIT 代码是新编译的，PC 指向第二轮的堆——所以不是"第一轮
  悬空缓存被复用"，而是 JIT 代码在堆里**被 GC/压缩搬移后失效**或
  二次启动时序触发的堆状态异常（未最终定论，属 VM 深水区）。

### 处置（phoneme-midp 6525f4a）：-int 纯解释器隔离
`runMidlet.c` 在 `JVM_Initialize()` 后注入
`JVM_ParseOneArg(1, {"-int"})` → `Arguments.cpp: UseCompiler=false`
（进程级静态，覆盖所有后续轮次）。标准 VM 选项，零新平台代码，
 phoneme-midp 改动规模 +1 文件。**性能换稳定；根因（JIT 代码在
二次启动场景下的失效机制）未修，待 UC 流程跑通后回头深挖。**
- 重编：`phoneme-midp/build_vita.sh`（增量，runMidlet.o 重编成功；
  尾部 libmidp.so multiple definition 仍为已知无害失败）→
  `vita-port/build/cmake` cmake --build 重打 VPK。
- 验证手段：`arm-vita-eabi-objdump -d runMidlet.o | grep -c JVM_ParseOneArg`
  = 3（原 2 + 新 1）。

## 2026-09-10 v01.47：truncate 改惰性逻辑截断（-int 隔离生效后的根修）

### 用户双测试结论（b201）
1. 退出 UC 再进**不再闪退** → v01.46 的 -int JIT 隔离有效
   （JIT 崩溃未再复现；根因仍在深水区，暂不回头）。
2. 删 rms/appdb_1A35 → UC 可进；**退出 Vita3K 重进 UC 卡初始化**。
   第二轮 stderr 在 `created+registered OK` 后零输出（无 [NET]、
   无任何 [pcsl] 面包屑）；第一轮出现过 `truncate blocked`。

### 根因链（v01.45 拦截 → compact 半途而废 → 脏数据 → 下轮卡死）
- compactRecords（RecordStoreImpl.java:782）先把数据块前移、更新
  header 的 RS6_DATA_SIZE（此时新大小已写盘），最后才 dbFile.truncate。
- v01.45 的预检查让 truncate 永远返回 -1 → IOException → compact
  中止，但 header 已改。物理文件仍保留旧尾块。
- 下一轮（新进程也一样）按 header 的新 DATA_SIZE 遍历，块边界与
  物理内容错位 → 读到垃圾 record 头 → UC 初始化流程卡死。
  与 v01.41 记载的 "first launch OK, every launch after broken"
  是同一数据破坏类。

### 根修：lazy logical truncation（vita_pcsl.c，j2me 658f744）
- Vita 无 ftruncate syscall；Vita3K 丢弃 SCE_O_TRUNC；物理截断只剩
  remove+create 换file，而空名 store（FFFFFFFF）的 db/idx 孪生句柄
  同路径锚定 → 换file必败。此路彻底放弃。
- VitaFileHandle 加 `logical_size`（-1=无钳制）。truncate(size) 只记
  钳制（多次截断取小者）；`pcsl_file_read` 过界返 0（EOF）、
  `pcsl_file_seek` 结果钳到 logical_size、`pcsl_file_sizeofopenfile`
  报逻辑大小；`pcsl_file_write` 写过界即解除钳制（文件重新变大）。
- 契约依据：phoneME RMS 全部读边界来自 db header（RS6_DATA_SIZE，
  RecordStoreImpl:799 `while (currentOffset < getSize())`）与 idx
  offset 表（RecordStoreIndex.getRecordHeader），**从不依赖物理
  EOF** → 逻辑截断对上层完全透明。
- O_TRUNC 仿真（storage_open 的 temp 文件路径）同改：打开即置
  logical_size=0，不再 remove+create，不再有"remove 被孪生句柄挡住
  → create 落在旧文件 → 交出内容矛盾句柄"的坑。
- v01.45 的 truncate/O_TRUNC 两处 vita_handles_held_ex 预检查随之
  移除（换file本身不存在了）；held_ex 函数保留备用。

### 遗留
- 物理尾巴字节留在盘上（RMS 库 KB 级，可接受）；真机可换真 ftruncate。
- JIT 二次启动根因未修（被 -int 掩盖）；若后续恢复 JIT 需先复现并
  定位"JIT 代码在堆内被失效"的机制。
- 验证入口：`strings midp_vita | grep "version: J2ME"` 应显示
  v01.47 b202 (658f744)。

## 2026-09-10 v01.48：RMS 文件缓存悬空句柄根修（数据不持久 + 书签卡死）

### 用户现象（b203 复测反馈）
- 二次进入 UC 正常（v01.47 生效）、`truncate blocked` 消失
- **但数据不持久化：每次启动都是全新开始**
- **保存书签直接卡死，无法操作**

### 根因：midp_file_cache.c 单例缓存悬空句柄（phoneme-midp f979b68）
`mFileCache` 是进程级单例，缓存"当前文件"的 handle。close 时只 flush
不释放单例，`mFileCache->handle` 永久指向已 free 的 VitaFileHandle。
UC 多 store 并存（空名设置库 FFFFFFFF + 书签库 + 缓存库），open/close
交错后：下次 `midp_file_cache_open` malloc 大概率复用同一块内存（新
文件合法 fd 占住了 fd 字段），走 else 分支对悬空 handle 执行
`finalize(stayOpen=TRUE)` → `storagePosition(悬空handle, 旧cachedPos)`
→ **把新文件 seek 到旧文件残留偏移**：
- 新开 store 读 db header 错位 → DB_SIGNATURE 校验失败 →
  "invalid record store contents" → UC 视为损坏重置（每次全新开始）
- 写路径位置污染 → 书签保存流程死循环卡死

### 排除项（本轮分析定案）
- **`unlink blocked .../FFFFFFFF` 良性**：FFFFFFFF = INTERNAL_SUITE_ID(-1)
  的 8 位 hex 目录名，UC 的空名设置库 db/idx 共用同一路径。退出时的
  unlink 来自 UC 自身清理逻辑，被 v01.43 held 预检查挡住反而保护数据。
  Windows/Vita3K 共享语义下打开的文件不可删，不存在"句柄先关则误删"。
- **v01.47 逻辑截断无涉**：写路径 sceIoWrite 直写落盘 ✔，commitWrite
  → flush → 真实写穿 ✔，读边界/sizeof 钳制正确 ✔。数据其实一直在盘上，
  坏在读回时的 header 校验。
- **RMS 文件锁未启用**：TARGET_VM=cldc_vm 时 lib.gmk 不编译
  rms_file_lock.c，RecordStoreLock 用 no-op stub，无死锁嫌疑。
- 空间计算正常：getusedspace 真实现（v01.34+）+ jam_space=100MB。

### 修复（最小 diff，16 行）
`midp_file_cache_close`：finalize 后 `midpFree(mFileCache); mFileCache
= NULL;`，恢复上游不变量"mFileCache->handle 必须指向打开中的文件"。

### 构建/产物
- 重编：`rm build/vita_arm/obj/arm/midp_file_cache.o && ./build_vita.sh`
  （exit=0，尾部 libmidp.so `_rom_linkcheck` 链接失败为已知无害项，
  VPK 走 libobj.a 不受影响；libobj.a 已含新对象）
- VPK：vita-port/build/cmake/midp_vita.vpk = **v01.48** (f53174e)
  14458298 B
- 提交：phoneme-midp f979b68（+16）；j2me 主仓 8f512a0 + f53174e

### 验证入口
- `strings midp_vita | grep "version: J2ME"` 应显示 v01.48
- 复测预期：UC 二次启动数据仍在（书签/设置保留）、保存书签不卡死、
  `unlink blocked .../FFFFFFFF` 仍会出现（预期，良性）

### 遗留
- JIT 二次启动根因未修（-int 掩盖中）
- 物理尾巴字节留盘（v01.47 定案，可接受）

### 修正（同日）
- 首次 VPK 重编未触发 gen_version（版本头缓存 b203 d841b85），重建后
  正确：**v01.48 b206 (eb9b719)**，14458185 B。装 VPK 前先核对
  `strings midp_vita | grep version`。
