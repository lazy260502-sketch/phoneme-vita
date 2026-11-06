# m3g 宿主探针 (HostProbe)

用**移植代码自己的 `M3gReader`** 在宿主 JVM 上加载真机同款 `.m3g`，然后按标题
字节码里那组相机参数算一遍裁剪空间，把"设备端 `[m3g] ndc ...` 会出现什么数字"
提前算出来。避免再靠猜。

## 构建与运行

```sh
WORK=/tmp/m3gprobe
rm -rf $WORK && mkdir -p $WORK/src $WORK/out
cp -r jsr184/src/share/core/common/classes/* $WORK/src/
cp debug/tools/m3gprobe/HostProbe.java $WORK/src/javax/microedition/m3g/
cd $WORK/src && javac -nowarn -encoding UTF-8 \
    -cp /workspace/psvita/phoneme-vita/phoneme-midp/build/vita_arm/classes \
    -d $WORK/out $(find . -name '*.java')
java -cp $WORK/out javax.microedition.m3g.HostProbe /path/to/hunter.m3g
```

`.m3g` 可从游戏 jar 里解出来（`python3 -c "import zipfile;zipfile.ZipFile(...).extractall(...)"`）。

## 2026-10-28 的结论（v03.39 依据）

`BioCtrl` 的 `setCamera` 只用了一个平移：
`postTranslate(-9,90,180)` + `setPerspective(70, 1.714, 1, 10000)`。
把它当 **camera→world**（视图矩阵 = 逆）时：

| 模型 | behind | inside | NDC x | NDC y |
| --- | --- | --- | --- | --- |
| jill_han (1073 v) | 0 | **1073 / 1073** | [-0.55, 0.39] | [-0.80, 0.66] |
| zombie (675 v) | 0 | **675 / 675** | [-0.40, 0.49] | [-0.79, 0.72] |
| hunter (1523 v) | 84 | 122 | [-17.6, 18.0] | [0.34, 36.9] |

当 **直接使用**（不取逆）时，`hunter` 变成 `behind=1523 / 1523`——与 log138 设备端
`behind=472/472` 完全一致。这是"v03.36 那次改动是错的"的决定性证据。

同理，`posArea` / `negArea` 是**混合**的（如 543 / 467），说明绕向没有整体反向，
`windingFix` 自动判定为 0（不翻转）——即"GL 正面/背面反了"这个假设在这三个模型上
**不成立**，自动翻转只作保险存在。

---

# 加载链路回归探针（log183/log184 之后新增）

上面那组是"渲染数值对不对"的探针。下面这组解决的是**另一类**问题：标题在
`Loader.load()` 之后拿到的对象**结构**对不对——`find(userID)` 能不能命中、
`duplicate()` 有没有丢东西、按位置寻址的索引会不会落到非 Node 上。

这类缺陷的共同症状是 **`[TIMEREXC] ... NullPointerException` 然后画面停住**
（TimerThread 静默取消任务，没有其它痕迹），已经发生过 5 次：

| 版本 | 缺陷 | 症状 |
| --- | --- | --- |
| v03.29 | `Texture2D.FUNC_BLEND` 常量缺失 | `IllegalArgumentException` |
| v04.17 | `PolygonMode.WINDING_*` 值错 | `IllegalArgumentException: bad winding` |
| v04.22 | `Appearance.setLayer` 非 public | `java.lang.Error:  95`（= IllegalAccessError） |
| v04.25 | `duplicate()` 丢 userID / 不复制 children | NPE @ bci=135、bci=855 |
| v04.27 | `Loader.load()` 返回内部节点而非根 | NPE @ `getTransformTo` |

## 一次性跑完的必要检查

```sh
W=/tmp/m3g; J="$PWD/debug/3d_game/极品飞车13之变速英文3D版.jar"
# （路径含中文时把 jar 复制成 /tmp/jars/g2.jar，JDK 8 的 ZipFile 不接受 UTF-8 路径）
```

| 探针 | 命令 | 通过标准 |
| --- | --- | --- |
| `NfsSim` | 无参数 | `ALL PASS`（这是最终门禁：它复现游戏的两条真实调用序列） |
| `RegChk` | `RegChk /tmp/jars/*.jar` | `m3g files=28 ok=28 fail=0` |
| `StructChk` | `StructChk /tmp/jars/g2.jar` | 单根文件的 `objs[0]` 必须是根（v04.27 起） |
| `OrderDump` | `OrderDump /tmp/jars/g2.jar` | 和改动前 `diff`：**只允许单根文件变化**，多根文件必须逐字节一致 |
| `Pos2` | `Pos2 /tmp/jars/g2.jar` | `非 Node: 0 / 19` |

`OrderDump` 的 `diff` 是防止"修一个坏十个"的关键：极品飞车对**多根**文件
（`/bl` `/bm` `/af` `/aq` …）是按**数组下标**取对象的，重排它们会立刻让
`checkcast Node` 抛 ClassCastException（log177 就是这么来的）。

## 已固化的结论（别再重新推导）

- `ac.a(res)` = `Loader.load(o.a[res])[0].duplicate()`，取 **下标 0**。
- `ac.b(raw)` = 按位置取，`group = (raw>>8)&0xFF`，`idx = raw & 0xFFFF00FF`；
  `group` 先经 `n.b_[]` 映射到资源（`6 -> /bl`、`0 -> /p`、`10 -> /l` …）。
- `ac.b` 有**三个重载**：`b(int)[B`（取原始字节）、`b(int)LNode`（取节点）、
  私有 `b(int)V`。查调用点时**必须看描述符**，只看方法名会把取字节误判成取节点。
- `.m3g` 里 `userID` 是"每个对象体前 4 字节"，绝大多数对象是 0；一个文件通常只有
  1~2 个非零 ID，`find()` 找的就是它们。
