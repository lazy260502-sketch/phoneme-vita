# log40 之后的 A/B 实验（v02.16）

目的：判定停摆是**与具体 jar 内容相关**，还是**所有 MIDlet 都要走的通用路径**。

## 为什么需要 `menu=0`

log36..log40 五次复现的冻结都发生在 `midp_run_midlet_with_args_cp` →
`JVM_Start+0xa4` → `JVM::start()+0x178` → `primordial_to_current_thread` →
`start_lightweight_thread_asm+0x18` 这条路径上（即"第一次切入 MIDlet 的 Java
代码"）。但每轮都先进了**原生菜单**（2 MB framebuffer、触摸初始化、
文件系统扫描），这是一个共同变量。

v02.16 起 `launch.cfg` 支持一行 `menu=0`：launcher 读到它就**完全跳过**
`vita_menu_run()`，直接启动 cfg 第 1/2 行指定的 jar/class。这样一轮 A/B 里
不再有菜单的任何残留状态可以被归咎。

无 `menu=0` 行时行为与 v02.15 完全一致（菜单优先）。

## 部署

把对应样例改名成 `launch.cfg`，放到真机的 `ux0:/data/J2ME00001/`：

```
ux0:/data/J2ME00001/launch.cfg
```

**⚠ jar 路径必须是相对的**（不能写 `ux0:/data/...`）：main 已 `chdir` 到数据
目录，且 VM 的 classpath 用 `:` 分隔，绝对路径会在第一个冒号处被切坏
（变成 `ux0` + `/data/...`）。

## 判读

开机后不再出现菜单，直接进 MIDlet。跑完（冻结后看门狗会自动 dump）看：

- `boot_log.txt` / `midp_stderr.log` 应有：
  ```
  launch.cfg jar (menu=0): Hello.jar
  launch.cfg class (menu=0): HelloMIDlet
  ```
  → 确认这轮**没有**走菜单（不会有 `menu jar:` 行）。

- 冻结判据（与 log36..log40 逐项一致）：
  ```
  [WD] VMPROG verdict exec=0 tick=0 -> NEITHER
  vm runClocks d=0 ; cpu1 idleN/switchN d=0 ; KICK no effect
  ```
  死点应在 `start_lightweight_thread_asm+0x18` 附近。

- **若 HelloMIDlet 冻结签名与 game.jar 完全相同** ⇒ 冻结是**通用路径**问题
  （与 MIDlet 内容无关），方向 A 的"换 jar"分支关闭，转方向 B / 调度 A/B。
- **若 HelloMIDlet 不冻结** ⇒ 与 jar 内容/类初始化相关，转去分析 game.jar 的
  启动路径差异。

## 样例文件

| 文件 | jar / class | 用途 |
|---|---|---|
| `launch.cfg.hello` | `Hello.jar` / `HelloMIDlet` | **首选**：最小 LCDUI，不碰音频/网络/RecordStore |
| `launch.cfg.tonetest` | `Hello.jar` / `ToneTest` | 自编 MMAPI 测试（会主动播放音频） |
| `launch.cfg.canvas` | `Hello.jar` / `CanvasTest` | 图形 / Canvas |
| `launch.cfg.game` | `games/app (1)/game.jar` / `com.app.filemanager.FileManagerMIDlet` | 复现 log40 的对照组 |
