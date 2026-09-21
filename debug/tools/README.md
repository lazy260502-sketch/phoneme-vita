# parse_psp2dmp.py 使用笔记（2026-09-21，任务 2 交付）

一键解析 Vita3K psp2dmp coredump。用法、判读要点、日志链路、收集清单
见技能文件：`samples/j2me/.instructions/psp2dmp-forensics.instructions.md`

## 验证记录

- log21 样本：窗口+0x10、slide=0x44000（anchor 自动解出）、PC=invoke_pending
  入口、LR=start_light 入口、栈走含 JVM::start —— 与手工分析全吻合
- log19 样本回归：窗口+0x10、slide=0x15000 自动解出
- 符号源：vita-port/build/midp_vita（0x810 基址未 strip 链接产物）；
  v01.95 起每次构建自动归档 out/vpk/midp_vita_v<版本>.sym

## PSVita core note 布局（实测）

- ELF32（EI_CLASS=1），phoff@0x1c
- note name 段**无 4 字节对齐填充**（COREFILE_INFO=15 字节实长，desc 紧跟）
- THREAD_REG_INFO (type=0x1004)：descsz=0x760，寄存器 r0-r15 窗口基址
  +0x10（log19/log21 两样本一致；脚本保留 +0x08/+0x04/+0x0c 自适应兜底）
- 其他有用 note：0x1003 THREAD_INFO / 0x102a TTY_INFO（Vita3K *** TTY: 流）
