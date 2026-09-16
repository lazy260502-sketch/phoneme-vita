# 构建产物 / 日志 / 交付物 位置规范

> 目的：结束"文件生成位置到处都有，谁知道用哪个"的状态。本文件是唯一权威。

## 一、目录约定（宿主侧）

```
samples/j2me/
├── out/vpk/        # ★ 所有对外交付的 VPK（唯一交付位置）
├── debug/logs/     # ★ 真机拉回的日志，按会话建子目录 logYYYYMMDD/
├── vita-port/      # 源码 + CMakeLists（构建用 out-of-source，见下）
└── ...
```

### 规则z

1. **VPK 交付**：构建产物一律拷贝到 `out/vpk/`，命名 `midp_vita_v<版本>[_<标签>].vpk`
   （如 `midp_vita_v01.71_tonediag.vpk`）。`vita-port/` 下的 `midp_vita.vpk` 只是构建输出，不算交付物；不要在别处再放第二份。
2. **真机日志**：拉回后放 `debug/logs/<会话名>/`（如 `log3/`），一次测试一个子目录，不混放。coredump（`*.psp2dmp`）也放这里。
3. **构建方式**：vita-port 一律 **out-of-source**：
   `cmake -S vita-port -B vita-port/build-cmake`，产物全在 `build-cmake/`，源码树零污染。
   （历史原因当前是 in-source；新构建逐步迁移，旧 in-source 产物已随 .gitignore 屏蔽）
4. **`.gitignore`** 已加：CMake in-source 产物、`out/`、`debug/logs/`——git status 保持干净，构建产物永不入库。

## 二、真机侧日志（ux0:/data/J2ME00001/）权威清单

| 文件 | 写入者 | 打开方式 | 内容 |
|---|---|---|---|
| `boot_log.txt` | vita_main.c | 追加 | 启动横幅、版本串、菜单 jar、RMS 路径——**每次先核对此文件确认版本** |
| `crumb.log` | vita_crumb.c | 追加 | 原生面包屑：launcher/ANI/媒体/菜单心跳/round/jsr75 面包屑 |
| `net_log.txt` | vita_net.c | 追加 | 网络子系统初始化与 socket 事件 |
| `vm_boot.log` | midp_run.c (VM 侧) | 追加 | JVM_Start 每轮 classpath/主类 |
| `vm_output.log` | jvm_printf→stdout 重定向 | 追加 | **Java 层 stdout**（System.out / MIDLET/TONE 前缀输出） |
| `vm_stderr.log` | stderr 重定向 | 追加 | Java 层 stderr + getClassPathPlus 等诊断 |
| `midp_stdout.log` / `midp_stderr.log` | midp 层 freopen | 追加/覆盖见代码 | midp 层输出（注意与 vm_* 是两条不同链路，见 PROJECT_MEMORY） |
| `runmidlet_debug.log` | runMidlet.c | 追加 | runMidlet 入口参数、midpInitialize 结果 |
| `midp_debug.log` | midp_run.c | 追加 | midp_run 各阶段 |
| `input_debug.log` | vita_input.c | 追加 | 事件泵心跳（v01.71 起：`#N ms= d= CE= ani=`）、按键/触摸 |
| `audio_debug.log` | vita_audio_javacall.c | **追加**（v01.71 前 TRUNC，历史数据已毁） | tone req/done、AudioOut 错误码 |
| `ani_mark.log` | vita_checkevents.c | **覆写槽**（watchdog 用途，故意 TRUNC） | ANI 等待进/出最后状态：`IN `=卡在等待内，`OUT`=卡在等待前 |
| `watchdog.log` | vita_watchdog.c (v01.72) | 追加 | 泵停转 >3s 时的取证快照：VM/tone 线程的内核等待类别（io/mutex/cond/delay/semaphore）+ 双时钟采样 |

### 使用守则

- **判版本**：看 `boot_log.txt` 的 `version:` 行（含 git 短哈希）。
- **防 FTP 缓存**：拉取前在**机内改名**（加时间后缀）再拉，历史教训见 PROJECT_MEMORY v01.69 节。
- **追加式文件**（绝大多数）：跨会话可信，读时注意按 `==== launcher start ====` / `[MIDLET] startSuite` 分段。
- **非追加式**：仅 `ani_mark.log`（watchdog 槽设计如此）；`audio_debug.log` v01.71 起已改追加。
