# CLDC VM (libcldc_vm.a) 正确编译流程 — 2026-09-02 实战验证版

> 本文档由 2026-09-01 ~ 09-02 的重编攻坚总结而成（g++-13 消失事故 + 六种失败配方
> 的教训）。**改 VM 源码后按本文流程走**；跳步必踩坑。配套脚本：`rebuild_vm.sh`。
> 原理：构建体系有三个互斥形态（cfg 的 `ifeq` 分支），变量互相污染是万恶之源：
>
> | 形态 | 产物 | 编译器 | 关键变量 |
> |---|---|---|---|
> | IsLoopGen=true | `loopgen/app/loopgen`（宿主工具） | `/usr/bin/g++-11 -B/usr/bin -m32`（cfg 的 FORCE_GCC） | FORCE_GCC 生效 |
> | IsRomGen=true | `romgen/app/romgen`（宿主工具） | 同上 | FORCE_GCC 生效 |
> | IsTarget=true¹ | `target/<flavor>/*.o`（ARM 对象） | `arm-vita-eabi-g++` | FORCE_GCC 必须为空 |
>
> ¹ 实测用 `IsLoopGen=true` + 命令行三清也可编出正确 ARM 对象（quick-native 宏环境
> 需要它），见 Step 3。
>
> vita-port **只需要** `dist/lib/libcldc_vm.a`（21 个成员），**不需要** `cldc_vm_g`
> 可执行文件（其链接会报 crt0 路径错，忽略即可）。

## 环境（每个 make 会话都要 export，bash 工具的 shell 不保留环境变量）

```bash
export JVMWorkSpace=/home/zyb/vitasdk/samples/j2me/phoneme-cldc
export JVMBuildSpace=$JVMWorkSpace/build
export JDK_DIR=/home/zyb/tools/jdk8u502-b07
export TOOLS_DIR=/home/zyb/vitasdk/samples/j2me/phoneme_source/phoneME/tools
```

缺任何一个会在 root.make 的 sanity 检查处以 `exit: Illegal number: -1` 失败。

## Step 1/2：宿主工具 loopgen / romgen（仅当二进制损坏或缺失时）

`romgen`/`loopgen` 是 32 位 x86 宿主工具。系统 g++-13 已被卸载，现存二进制由
`vita_arm.cfg` 的 `FORCE_GCC = /usr/bin/g++-11 -B/usr/bin -m32` 编出（cldc commit
4000337）。健康检查：直接运行 `romgen`，应打印 `Error: class not specified`；
报 `GLIBC_2.38 not found` 即二进制已坏，需重编：

```bash
cd $JVMWorkSpace/build/vita_arm
make BUILD_DIR_NAME=vita_arm IsLoopGen=true CLDCBUILD=fp ENABLE_ENABLING_CHECK=false \
     -j8 loopgen          # 产 loopgen/app/loopgen
make BUILD_DIR_NAME=vita_arm IsLoopGen=true ROMGENERATOR_DIR=romgen/app \
     CLDCBUILD=fp ENABLE_ENABLING_CHECK=false -j8 romgen   # 产 dist/bin/romgen
```

注意：romgen/app 与 loopgen/app 下各有一个 **ROMImage.cpp（ROMSkeleton 副本）**，
是修复 vpath 污染的关键（见陷阱 7），**不要删除**。

## Step 3：ARM 目标 VM（核心步骤，改了 .cpp/.hpp 后执行）

```bash
cd $JVMWorkSpace/build/vita_arm

# (a) 预置 x86 AsmStubs：target 构建的 vpath 会试图用 ARM as 编这个 x86 汇编
mkdir -p target/release
cp romgen/app/AsmStubs_x86_64.o target/release/
touch -d "2026-08-25" target/release/AsmStubs_x86_64.o   # 骗过时间戳，跳过重编

# (b) 编译（命令行三清是灵魂，见下）
make BUILD_DIR_NAME=vita_arm IsLoopGen=true \
     LOOP_GENERATOR_DIR=../../linux_arm/loopgen/app \
     ENABLE_ENABLING_CHECK=false ENABLE_C_INTERPRETER=true \
     FORCE_GCC= GNU_TOOLS_DIR=/home/zyb/.local/vitasdk CPP_DEF_FLAGS= \
     -j8 _release
```

**命令行三清（缺一必败，症状各不相同）**：
- `FORCE_GCC=`：不清 → cfg 的 romgen 分支把所有编译角色换成宿主 g++-11，
  用 x86 编译器吃 ARM 源码，报 `unrecognized option '-marm'`
- `GNU_TOOLS_DIR=<vitasdk>`：不清 → 交叉编译器前缀丢失，报裸 `g++` 找 crt0
- `CPP_DEF_FLAGS=`：不清 → romgen 分支的 `-B/usr/bin -m32 -DCROSS_GENERATOR=1`
  泄漏进 ARM 编译，报 `-m32 unrecognized`（arm g++）

**其余关键点**：
- `IsLoopGen=true` 必须带：它设置 `arch=arm` 和 quick-native 宏环境（缺了
  `JVMClassFileParser::get_u2` 等符号无人定义）
- 直接调 **`_release`** 子目标：`debug`/`release`/`product` 顶层目标都会 FORCE
  重跑 tools/loopgen/romgen（三清会穿透子 make 把宿主工具编坏）
- **flavor 必须 release**：`debug` 引入 AZZERT 断言符号（`_rom_check_*` 等大批
  缺口）；`product` 需要 Quick 编译器成员（`JVMMethod::compile`，本树编不过）
- 结尾链接 `cldc_vm_g` 报 crt0 路径错是**预期内**——对象已全部产出于
  `target/release/*.o`（约 31 个），Step 4 只需要它们

## Step 4：打包 libcldc_vm.a（最易翻车，每步必须验证）

```bash
L=$JVMBUILDSPACE/vita_arm/dist/lib/libcldc_vm.a
cp $L $L.bak                      # ① 永远先备份（含浮点 stub 的旧 Interpreter_arm.o）

cd $JVMBUILDSPACE/vita_arm/target/release
for m in *.o; do                  # ② 全部新对象入库（例外见下）
  case "$m" in
    AsmStubs_x86_64.o|Interpreter_arm.o) ;;      # x86 对象 / 需用旧成员
    ani.o|ani_bsd_socket.o|os_port.o|poolthread.o) ;;  # anilib 专属，在
                                                  # libcldc_vm_ani.a 里，混入
                                                  # 主库会引入 ANI_Initialize
                                                  # 等符号解析混乱
    *) arm-vita-eabi-ar r $L "$m" ;;
  esac
done

# ③ Interpreter_arm.o（GP 表 + jvm_f2i/d2i 浮点 stub）必须用旧库成员：
#    loopgen 重新生成的 .s 丢了浮点 stub（生成上下文不全），重编版本无这些符号
cd /tmp && arm-vita-eabi-ar p $L.bak Interpreter_arm.o > Interpreter_arm.o
arm-vita-eabi-ar r $L Interpreter_arm.o

# ④ ⚠️ ar r 的 && 链会静默断，逐个验证成员非空：
arm-vita-eabi-ar p $L Interpreter_arm.o | wc -c    # 必须 > 0（约 97KB）
arm-vita-eabi-ar t $L | wc -l                      # 成员数
```

## Step 5：vita-port 链接验证

```bash
cd /home/zyb/vitasdk/samples/j2me/vita-port
cmake --build build -j8
```

- 未解析符号清零 = 成功。`_rom_check_*` 等 37 个链接校验符号由
  `src/vm_rom_stubs.c` 提供（非 PRODUCT 构建引用它们）
- **先 git commit 再 build**：版本串取 HEAD，顺序反了 VPK 版本不反映内容
- 验证转储等字符串是否进入 eboot（.self 段是压缩的，直接搜 eboot.bin 会误报）：
  `python3 -c "print(open('build/midp_vita','rb').read().count(b'CP tags (len'))"`

## 陷阱速查表（每条都是实战踩过）

| # | 陷阱 | 症状 | 规避 |
|---|---|---|---|
| 1 | g++-13 已从系统卸载 | 旧工具报 GLIBC_2.38；构建报 g++-13 不存在 | cfg 已改 g++-11；勿改回 |
| 2 | PATH 前部是 vitasdk（ARM gcc/as） | 宿主编译报 `as: --32/--64 unrecognized` | 宿主编译一律 `-B/usr/bin` 或绝对路径 |
| 3 | make 命令行变量优先级最高且穿透子 make | 三清传给顶层会弄坏 romgen/loopgen 子构建 | 宿主工具与 target 分开跑 |
| 4 | tools/loopgen/_romgen 目标带 FORCE | 顶层 debug 目标无条件重编宿主工具 | target 直接调 `_release` |
| 5 | vpath 污染 | ARM as 编 AsmStubs_x86_64.s / 链接错误 ROMImage.cpp | 预置 .o + romgen(loopgen)/app 下保留 Skeleton 副本 |
| 6 | flavor 选错 | debug=AZZERT 符号缺口；product=编译器成员缺失 | release |
| 7 | 重生成的 Interpreter_arm.s 缺浮点 stub | 链接期：jvm_f2i/jvm_d2i undefined；**运行期：GP 表数据被当代码执行——Vita3K 报 Undefined instruction 且地址落在 Interpreter_arm.o 区域、伴随无限递归栈下溢（b139 崩溃实例）** | 库成员用旧版（备份提取），打包后 `nm` 验证 jvm_f2i 存在 |
| 8 | ar r && 链静默断 | 库成员为空、链接莫名缺符号 | 每步 ar p 验证非空 |
| 9 | va_list 传 NULL | ARM EABI 编译错（x86 32 位能过） | 用 `va_list()` 值初始化 |
| 10 | MIDP Java 增量按 mtime | 改 jsr135/上游源后 classes.zip 不更新 | 删 tmpclasses/ + classes.zip 强制全量 |
| 11 | vita-elf-create 入口选择 | **eboot 直接闪退**（ARM 模式解码 Thumb）；或 JVM 秒退（入口=无关函数被当 main） | 工具默认找 `module_start` 符号（无则 fallback 错误地址）；`-m` 参数不修正 e_entry。修复=`tools/patch_velf_entry.py` 在 POST_BUILD 把 velf e_entry 重写为 ELF 真实入口（**必须保留 Thumb 位 bit0**，strip 掉就是闪退） |
