#!/usr/bin/env python3
"""列出 psp2dmp 的全部 PT_NOTE 条目，并 hexdump 0x102e(summary)。

用途: 判断 coredump 是被"哪个线程/哪个异常"触发的 —— 0x102e 里
往往存着触发 dump 的信号/故障地址，这是区分
"内核 kill / 用户数据中止 / 外部结束进程" 的唯一直接证据。
"""
import gzip
import struct
import sys

data = open(sys.argv[1], 'rb').read()
if data[:2] == b'\x1f\x8b':
    data = gzip.decompress(data)

e_phoff = struct.unpack_from('<I', data, 0x1c)[0]
e_phnum = struct.unpack_from('<H', data, 0x2c)[0]

for i in range(e_phnum):
    f = struct.unpack_from('<8I', data, e_phoff + i * 32)
    if f[0] != 4:
        continue
    off, sz = f[1], f[4]
    pos = off
    while pos + 12 <= off + sz:
        namesz, descsz, ntype = struct.unpack_from('<III', data, pos)
        doff = pos + 12 + ((namesz + 3) & ~3)
        name = data[pos + 12:pos + 12 + namesz].rstrip(b'\0').decode(
            'latin1', 'replace')
        print(f'note type={ntype:#010x} name="{name}" namesz={namesz} '
              f'descsz={descsz} off={doff:#x}')
        if ntype in (0x102e, 0x102f, 0x1030):
            blob = data[doff:doff + descsz]
            print(f'  --- {ntype:#x} hexdump ({len(blob)} B) ---')
            for k in range(0, len(blob), 16):
                row = blob[k:k + 16]
                hexs = ' '.join(f'{b:02x}' for b in row)
                asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in row)
                print(f'  {k:04x}  {hexs:<47}  {asc}')
        pos = doff + ((descsz + 3) & ~3)
