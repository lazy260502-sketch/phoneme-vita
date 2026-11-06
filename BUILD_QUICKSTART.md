# 编译速查(给所有 AI 工具/新会话:先读这一页再动手)

只列"改了什么就跑什么"和"已经踩过的坑"。原理和细节见 `VM_BUILD.md`、`BUILD.md`、`PROJECT_MEMORY.md`。

## 1. 改了什么 → 跑什么

| 你改了 | 命令(仓库根 `/workspace/psvita/phoneme-vita`) | 说明 |
|---|---|---|
| `vita-port/src/*.c`、`CMakeLists.txt`、`nokia_src/*.java` 以外的 port 层 | `vita-port/build.sh` | 只链接+打包 |
| `phoneme-midp` 的 Java、`vita-port/nokia_src/*.java` | `vita-port/build.sh midp` | 重建 midp 库和 ROM(含 vendor 类) |
| **`phoneme-midp` 的 native（`.c`/`.cpp`）**，例如 `i18n/i18n_port/vita/*`、`timezone/*` | 先 `MIDP_EXTRA_GOAL=$PWD/phoneme-midp/build/vita_arm/obj/arm/libobj.a bash phoneme-midp/build_vita.sh`，再 `vita-port/build.sh midp` | **`all` 目标不依赖 `libobj.a`**，见坑 17；漏了这一步 ELF 还是旧对象 |
| `phoneme-cldc/src/**.cpp/.hpp`、`src/vm/os/vita/*`(含 FP overlay) | `./rebuild_vm.sh` 然后 `vita-port/build.sh` | **先改库再链接**;不重编库,ELF 里还是旧代码 |
| 想全量验证 | `vita-port/build.sh clean` | 删 `build/cmake` 并强制重建 midp |
| 动了浮点/Math/解释器调用约定 | 上面之后再跑 `debug/tools/fpmodel/run.sh` | 校准 0 不一致、40/40、42/42、50/50 才算过 |
| 改了字符集表/生成器 `tools/charsetgen/*` | `python3 tools/charsetgen/gen_charmap.py` 然后 `python3 tools/charsetgen/verify_charsets.py` | 全表 decode+encode 必须 0 不一致；`gen_charmap.py --check` 只校验不入盘 |
| 改了 `vita-port/jsr_src/**.java`（JSR-179/256 等自实现可选包） | `bash vita-port/build_nokia_stubs.sh` 然后 `vita-port/build.sh midp` | 装进 `classes/` → `midp_system.jar`；**新增 `javax` 包要加进脚本的 `JSR_PKGS` 白名单**，**新增 `com.sun` 协议处理器要加进 `SUN_ALLOW`**（`sensor:` 就是这么装的），否则不会安装 |

产物:`vita-port/out/vpk/midp_vita_v<版本>.vpk`。版本号在 `vita-port/CMakeLists.txt` 里最后一个 `set(VITA_VERSION`,
选号前 `grep -n 'set(VITA_VERSION' vita-port/CMakeLists.txt`(版本号被多个会话共用,别撞号)。

## 2. 目录与"谁负责什么"

- `phoneme_source/`:上游参考树,**只读**。
- `phoneme-cldc/build/share/`:骨架,**不改**。平台开关写 `build/vita_arm/vita_arm.cfg`。
- `phoneme-cldc/src/vm/os/vita/`:Vita 专属新增文件放这里(含 overlay:`vita_fp_overlay.S` + `patch_interp.sh`)。
- `rebuild_vm.sh`:重编 CLDC 对象并重新打包 `libcldc_vm.a`(`pack_lib`)。**overlay 的替换步骤在这里**。
- `vita-port/CMakeLists.txt`:只管链接最终 ELF 和打包 VPK,**不放 overlay 逻辑**。
  也只放构建内容(工具链/版本/源列表/链接顺序/打包);版本历史论证写 `PROJECT_MEMORY.md`
  (v04.35 已把 7114 行精简到 ~700 行,代码逐行未变)。版本号只在 Configuration 块末尾写一次。

## 3. overlay(覆盖/替换)怎么做

新增文件 + 在 `pack_lib` 里替换库成员,不改原文件。要求见 `AGENTS.md` 第 4 条。范例就是 FP overlay:

```
rebuild_vm.sh pack_lib
  └─ 从 libcldc_vm.a.bak 取出 Interpreter_arm.o(私有 mktemp 目录)
  └─ patch_interp.sh  in.o  out.o
        ├─ 已含 jvm_d2i_isoft → 原样复制(幂等)
        ├─ objcopy --redefine-sym jvm_X=jvm_X_isoft (28 个 + d2i + f2i)
        ├─ 汇编 vita_fp_overlay.S,ld -r 合并
        └─ nm 校验:每个符号恰好定义一次
  └─ ar r libcldc_vm.a  (在含该成员的目录里,用 basename)
  └─ 库级校验:jvm_d2i/jvm_f2i/28 个 _isoft 在整个库里各恰好一份
```

写新 overlay 时照这个结构:幂等、改名而非靠链接顺序、`nm` 校验、临时文件放 `mktemp -d`、`.gitignore` 加白名单。

## 4. 已踩过的坑(别再踩)

1. **库没重编,ELF 还是旧的。** 改了 `phoneme-cldc/src` 后必须 `./rebuild_vm.sh`,再 `vita-port/build.sh`。
2. **midp ROM 陈旧。** 默认 make 目标不依赖 `libobj.a`;vendor 类要先进 `classes.zip` 再生成 ROM。`build.sh midp` 已处理,
   别绕过它手工 make。验证 Java 改动用 ELF 里的 UTF-16 字符串或 ROMLog,**不要看 `midp_system.jar`**(ROM 化的类不从 jar 加载)。
3. **`nm … | grep -q` + `set -o pipefail` = 随机失败。** `grep -q` 命中即退出,`nm` 被 SIGPIPE 杀(rc 141),管道判假。
   这条让 `patch_interp.sh` 的幂等检测随机失效,把 overlay 合并两次报 `multiple definition of jvm_l2f_isoft`。
   统一用 `grep -c … || true` 或先把输出存变量。
4. **`/tmp` 里固定文件名会被旧文件静默复用**(别的会话、上一次失败的运行都会留下)。用 `mktemp -d`,`trap` 里清理;
   `trap` 要用双引号让变量立刻展开(函数内 `local` 变量在 EXIT 时已不存在)。
5. **`ar r` 带路径会把路径写进成员名。** 先 `cd` 到成员所在目录再 `ar r $L basename`。
6. **`rebuild_vm.sh` 日志里的 `ld returned 1`(缺 `vita_splash_*`、`_current_thread`、`fast_memclear`)是预期的**:
   宿主 loopgen 链接缺 port 层符号。判断成功看 `== library packed: N members ==` 和退出码 0,不看这条。
7. **PRODUCT / 非 PRODUCT ABI 不能混**:手工重编对象必须带 `rebuild_vm.sh` 里的完整旗标。
8. **同名符号被两种调用约定共用时,改名要连定义一起改。** 解释器用整数寄存器调 FP 辅助函数,C++ 用硬浮点;
   所以 `jvm_d2i/jvm_f2i` 的两份定义必须分开命名(`_isoft` 是整数协议,无后缀是硬浮点)。
9. **复制构建树去别处验证前,先删 `loopgen/ romgen/ target/ dist/`**:里面的 Makefile/Dependencies 写死原绝对路径,
   不删会悄悄编译原目录源码,验证等于没做。事后用日志检查源码路径是否都在副本里。
10. **`Interpreter_arm.s/.o` 可以重新生成**(`loopgen -generateoptimized +GenerateGNUCode -outputdir <dir>`,与冻结版逐字节一致)。
    丢浮点桩的是 `+GenerateGPTableOnly`。不要再把它当黑盒,也不要把 `GPTableOnly` 的产物拿去比较。
11. **模型/脚本的 ABORT 不是通过。** `fpmodel` 缺指令时会中止;补指令后必须重跑校准(0 不一致)。拿不到对照时,
    先用 GCC 编译同一份 C 源码做参照,再怪 ELF。
12. **宿主上编译/运行验证程序**:PATH 前部是 vitasdk,`gcc` 会变成 ARM 编译器、`as` 不认 `--64`。用
    `/usr/bin/gcc -B/usr/bin ...`。例:`debug/tools/wavq/test_resample.c`(用的是要出货的 `vita_wav_resample.h`)。
13. **ROMized 的 Java(`nokia_src/`、`com.sun.midp.*`)改了要 `build.sh midp`,验证看 ELF 不看 jar。**
    字符串常量会被 romgen 合并去重,数 `strings` 次数没用;可靠办法是在 ELF 里搜**方法字节码**(例如 `parseOta` 的
    `10 6B 36 0D 03|04 36 0E 10 07 36 0F` 里 `03`=style 0、`04`=style 1),并对比上一版 ELF 的同一模式。
14. 编辑器不能覆盖已存在文件、单次写入过长会被拒:大文件用 `cat >>` 分段写。
15. **`build.sh midp` 曾经不重编 Java 却退出 0。** 它在调 `build_vita.sh` 前对 `classes.zip` 做 `jar uf`,mtime 被刷新,
    make 认为所有 `.java` 都比 zip 旧,日志是 `no need to recompile .java files`,ROM 沿用旧类。已在 `build.sh` 里
    用 `touch -r` 还原时间戳。验证办法:看 `/tmp/midp_build.log` 里有 `compiling N .java files`,并核对 `classes/` 下对应
    `.class` 的 mtime;只看返回码不算数。另外宿主需要 `gawk`(只有 mawk 时 `reldir` 报 `gawk: not found`,Settings.gmk 之后不继续)。
16. **KNI 原生方法返回 float/double 必须走整数寄存器。** 解释器从 r0(float)/r0:r1(double,r0=高字)取原生返回值,
    而 `-mfloat-abi=hard` 编出来的 C 函数把结果放在 s0/d0。`Float.intBitsToFloat`/`Double.longBitsToDouble` 曾因此返回 r0 里的残留
    指针(log143:全是 0x813f3258),整个 M3G 加载器的 float 都是垃圾。修法是 overlay:`vita_kni_fp_overlay.S` + `patch_kni_fp.sh`
    (`rebuild_vm.sh pack_lib` 调用)。新增任何返回 float/double 的 KNI 函数都要同样处理,并在 `debug/tools/fpmodel/bitsnat.py` 加用例。
17. **`build.sh midp` 的 `all` 目标不依赖 `libobj.a`。** 改了 MIDP 的 native(`.c`/`.cpp`)后只跑
    `vita-port/build.sh midp`,日志会是 `no need to recompile .java files` 然后直接链接 —— **ELF 里还是旧对象**
    (v04.32 的 `vitaGenConv.c` 就这样白跑了一次,`ar t libobj.a` 里根本没有它)。必须显式:
    ```bash
    MIDP_EXTRA_GOAL=$PWD/phoneme-midp/build/vita_arm/obj/arm/libobj.a \
        bash phoneme-midp/build_vita.sh
    ```
    再 `vita-port/build.sh midp`。核对: `arm-vita-eabi-ar t .../libobj.a | grep <你的>.o`。
18. **`libobj.a` 的成员是增量 ADD 列表**(`build/vita_arm/obj/arm/libobj.a-list`): 一个 `.o` 只要"没重新编译过"
    就不会被 ADD,哪怕它躺在 `obj/arm/` 里。v04.32 因此丢了 `getLocaleInfo`(`defaultLocale.o` 780 B 一直在,
    库里没有,链接报 `undefined reference to getLocaleInfo`)。修法: `rm obj/arm/<x>.o` 强制重编让它重新入列。
    **换 i18n 实现目录时尤其要检查**: `defaultLocale.c` 必须和 `vitaGenConv.c` 一起编(见 `i18n_port/vita/lib.gmk`)。
19. **`conv.c` 的 `NUM_LCCONV` 是"已请求编码数"上限,不只是缓存大小。** 上游非日文配置是 1,第二个编码会被
    静默判成 unsupported;本移植在 `__vita__` 下改成 8。
20. **打开"新的 Java 类集合"(如 `USE_SERVER_SOCKET=true`)必须按序:先重编 Java → 重生成 ROM → 再编 native。**
    顺序错会得到 `invalid use of incomplete typedef 'struct Java_com_sun_midp_io_j2me_serversocket_Socket'`:
    `serverSocketProtocol.c` 依赖 `ROMStructs.h` 里的结构体,而 romgen 只对**已在 classes.zip 里**的类生成结构。
    v04.32 直接以 `libobj.a` 为目标,Java 编译根本没跑,于是沿用旧 `ROMStructs.h`。可用配方:
    ```bash
    cd phoneme-midp
    find src/protocol/ssocket -name '*.java' -exec touch {} +     # 让增量编译看到新类
    rm -f build/vita_arm/ROMStructs.h build/vita_arm/ROMImage.cpp \
          build/vita_arm/obj/arm/ROMImage.o
    bash build_vita.sh                                            # 默认 all: Java + romgen + native
    MIDP_EXTRA_GOAL=$PWD/build/vita_arm/obj/arm/libobj.a bash build_vita.sh
    cd .. && vita-port/build.sh midp
    ```
    核对三件事: `grep -c serversocket build/vita_arm/ROMStructs.h` ≥1、
    `ls build/vita_arm/classes/com/sun/midp/io/j2me/ | grep serversocket`、
    `arm-vita-eabi-ar t build/vita_arm/obj/arm/libobj.a | grep serverSocketProtocol`。
21. **这台构建机上 `gcc` 就是 vitasdk 的交叉编译器**（`gcc -dumpmachine` → `arm-vita-eabi`）。
    用它"在宿主上试编一下"实际编的是 ARM 目标文件（不能执行，但**等于顺手做了 ARM 验证**）。
    真要跑宿主测试用 `/usr/bin/gcc -B/usr/bin`——`-B` 不能省，否则它仍调用 vitasdk 的 `as`，
    报 `as: unrecognized option '--64'`，产物是 ARM 的却以为能跑（`Exec format error`）。
22. **`stb_vorbis` v1.22 没有 `stb_vorbis_open_callbacks`**（只有 `open_memory` / `open_filename` /
    `open_pushdata`）。凭记忆按"回调式"写会编不过；随机 seek 只有 `open_memory` 支持，
    所以 OGG 走整文件入堆（上限 8 MB，见 `vita_ogg.c` 头注释）。
23. **改了 `phoneme-cldc/src/vm/**` 之后必须用 `./rebuild_vm.sh`，不能直接在 CLDC 构建目录里 `make`。**
    直接 `cd phoneme-cldc/build/vita_arm && make` 会因为 `JVMBuildSpace`/`JDK_DIR`/`TOOLS_DIR`
    没 export 而把 `OUTDIR` 拼成 `/build/vita_arm`，报
    `mkdir: cannot create directory '/build': Permission denied`。
    `rebuild_vm.sh` 自己 export 这些变量、只重编需要的 ARM 对象并重打包
    `dist/lib/libcldc_vm.a`（v04.47 实测：VM_OK → 库 14:43:30）。
    它日志里那句 `collect2: error: ld returned 1 exit status` 是 `cldc_vm_g` **可执行文件**的链接
    失败，`VM_BUILD.md` 明确"忽略即可"——我们只用库。判据是库里成员的 mtime，不是这条错误。
24. **`vita-port/CMakeLists.txt` 会被并发会话覆盖 —— 丢的往往是"源文件清单"和"链接库"，不只是版本号。**
    v04.48 实测：文件在两次构建之间变回"HEAD..04.31"状态，症状是链接失败：
    ```
    undefined reference to `vita_dec_ogg'          ← src/vita_ogg.c 那行丢了
    undefined reference to `vita_lifecycle_init'   ← src/vita_lifecycle.c
    undefined reference to `vita_motion_start'     ← src/vita_motion.c
    undefined reference to `sceRtcGetCurrentClock' ← SceRtc_stub 丢了
    undefined reference to `sceMotionStartSampling'← SceMotion_stub 丢了
    ```
    **危险点：链接失败时 `build.sh` 仍可能把旧的 midp_vita 打包成新版本号的 VPK**，
    所以"文件存在且时间新"不能当作构建成功的证据 —— 必须看 `BUILD_EXIT=0`。
    恢复方法（不要一个个撞）：
    ```bash
    for f in vita-port/src/*.c; do b=$(basename $f); \
      grep -q "src/$b" vita-port/CMakeLists.txt || echo "MISSING: $b"; done
    # stb_vorbis.c 是故意不列的（被 vita_ogg.c include）
    ```
    改完前后各 `grep -n 'set(VITA_VERSION' vita-port/CMakeLists.txt | tail -1` 核对生效版本号。

    **改完 VM 还要走完整链路**：`rm ROM 产物` → `build_vita.sh` → `MIDP_EXTRA_GOAL=…libobj.a` →
    `vita-port/build.sh midp`；`phoneme-midp/build_vita.sh` 用的是**预编译的 CLDC dist**，
    它不会替你重编 VM。

    另外 `stb_vorbis.c` **不要**加进 `CMakeLists.txt`：它是声明+实现同文件，由 `vita_ogg.c` include，
    单独编译会重复定义所有符号。


## 5. 改完要做的

- `git diff -- <file>` 自查;只增不乱改(不重排格式、不动尾随空格)。
- 把改动、原因、验证结果追加到 `PROJECT_MEMORY.md`("关键文件修改记录")。
- 如实报告:哪些在模型/宿主上验证过,哪些**没上真机**。
25. **改了 `phoneme-cldc` 的浮点/字序相关代码后,`vita-port/build.sh` 的 Step 4b FP 闸门必须全绿。** `MSW_FIRST_FOR_DOUBLE=1` 是 `rebuild_vm.sh` 给链接自检(`_rom_linkcheck_mffd_true`)强加的,**不是 C double 在内存里的字序**;C 代码读写 double 的字用 `JDOUBLE_MEM_HI_WORD/LO_WORD`(`GlobalDefinitions.hpp`)。v04.29-v04.49 因为混用,`sqrt/floor/sin/cos/doubleToLongBits` 全错了 6 个版本。闸门跑 fpmodel(校准+mathfull+bitsnat+d2ihf,约 11 秒),`FP_GATE=0` 才能跳过;`ABORT` 不算通过。`debug/tools/fpmodel/` **已入 git**（v04.54，14 个文件；`soft_ref.o` 由 `armcal.py` 自动编译，工具路径走 `VITASDK_BIN`）。
