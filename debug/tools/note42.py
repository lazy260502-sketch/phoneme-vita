#!/usr/bin/env python3
"""按 note 类型 dump psp2dmp 的某个注释体（十六进制 + ASCII）。

用法: python3 note42.py <psp2dmp> <type_hex> [max]
常用类型:
  0x102c EVENT_LOG_INFO  - 内核事件日志（进程被杀原因）
  0x102a/0x1031 TTY_INFO - dump 时刻 tty 最后输出
  0x1003 THREAD_INFO     - 线程名/状态
  0x101b STACK_INFO      - 每个线程栈范围
  0x1002 PROCESS_INFO
"""
import gzip
import struct
import sys

data = open(sys.argv[1], 'rb').read()
if data[:2] == b'\x1f\x8b':
    data = gzip.decompress(data)
want = int(sys.argv[2], 16)
maxb = int(sys.argv[3]) if len(sys.argv) > 3 else 4096

e_phoff = struct.unpack_from('<I', data, 0x1c)[0]
e_phnum = struct.unpack_from('<H', data, 0x2c)[0]
found = False
for i in range(e_phnum):
    f = struct.unpack_from('<8I', data, e_phoff + i * 32)
    if f[0] != 4:
        continue
    off, sz = f[1], f[4]
    pos = off
    while pos + 12 <= off + sz:
        namesz, descsz, ntype = struct.unpack_from('<III', data, pos)
        doff = pos + 12 + ((namesz + 3) & ~3)
        if ntype == want:
            found = True
            blob = data[doff:doff + min(descsz, maxb)]
            print(f'note {want:#x} descsz={descsz}')
            for k in range(0, len(blob), 16):
                row = blob[k:k + 16]
                hexs = ' '.join(f'{b:02x}' for b in row)
                asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in row)
                print(f'  {k:04x}  {hexs:<47}  {asc}')
            # 顺带打印里面所有可读的连续字符串
            print('  --- strings ---')
            cur = b''
            for b in blob:
                if 32 <= b < 127:
                    cur += bytes([b])
                else:
                    if len(cur) >= 4:
                        print('   ', cur.decode())
                    cur = b''
            if len(cur) >= 4:
                print('   ', cur.decode())
        pos = doff + ((descsz + 3) & ~3)
if not found:
    print(f'note {want:#x} not present')
