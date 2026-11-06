# 上游源码与归档来源（phoneME / phoneME Feature）

> 用途：**查实现、补缺口、核对基线**时，知道该去哪找、找到后怎么验。
> 核验日期：2026-11-02（下述链接、commit、HTTP 状态均当日实测）。
> 相关：`README.md`（本仓库来源）、`PROJECT_MEMORY.md`（决策与踩坑）、
> `COMPATIBILITY_ROADMAP.md`（同类模拟器能力对照）。

---

## 0. 结论先行

| 本仓库对象 | 实际来源 | 固定基线 | 形态 |
|---|---|---|---|
| `phoneme_source/phoneME/`（只读参照树） | https://github.com/magicus/phoneME | `5a13f65cff07b2211b7b867c51ada4bb9c7d4f32` | git submodule（约 155 MB） |
| `phoneme-cldc/`（工作副本） | https://github.com/tberthel/phoneme-components-cldc | `aae56e14a7e3`（电话 ME Feature mr2-rel-b23） | 完整源码 + Vita 移植 |
| `phoneme-midp/`（工作副本） | https://github.com/tberthel/phoneme-components-midp | `8d44713992fd` | 完整源码 + Vita 移植 |

取基线：

```sh
# A. 只读参照树（子模块，已在 .gitmodules 登记）
git submodule update --init phoneme_source/phoneME
git -C phoneme_source/phoneME rev-parse --short HEAD      # 期望 5a13f65cf

# B. 两个工作副本的上游组件仓
git clone https://github.com/tberthel/phoneme-components-cldc
git -C phoneme-components-cldc checkout aae56e14a7e3
git clone https://github.com/tberthel/phoneme-components-midp
git -C phoneme-components-midp checkout 8d44713992fd
```

> 这三个基线是判定"我们改过什么"的唯一参照。不要用目录级 diff 代替
> `git diff`（见 `AGENTS.md` 第 2 条）。
>
> **注意三者不同源**：`phoneme_source/phoneME` 是 magicus 的 umbrella 树，
> `git describe` 为 `midp-mr2-promo-b05@1016-5099-g5a13f65cf`；两个工作副本来自
> tberthel 组件仓的 mr2-rel-b23 基线。它们只用于"查阅原始机制"，不能互相替代，
> 也不能拿参照树去反推我们的改动清单。

---

## 1. 官方源码去哪了

Sun 的 `phoneme.dev.java.net` / `download.java.net` 早已下线（`phoneme_source/phoneME/www/`
里全是死链，只作历史文档），官方按 J2ME 模块规范把工程**拆成 20 多个独立仓**，
当前最完整的社区镜像是 GitHub 组织 **https://github.com/PhoneJ2ME**（29 个仓，实测）：

```
abstractions  builds   cdc    cldc   cyg4me  javacall  jpeg   jsr120  jsr135
jsr172        jsr177   jsr211 jsr239 jsr280   jump      kdp    legal   lime
midp          midp_abb pcsl   phonej2me.github.io      pisces  preverifier
releases      restricted_crypto  tools  www     xmlparser
```

- 每个仓对应 `phoneme_source/phoneME/` 下的一个**同名顶层目录**；把这些仓按目录名
  克隆到一个空目录里，就得到与本仓库参照树结构等价的完整工程。
- 本仓库的参照树（magicus/phoneME）就是这样一个已经拼好的 umbrella 树，**优先用它**，
  不必逐个克隆。
- 注意：PhoneJ2ME 组织里**没有** `jsr184`（M3G）仓。`jsr75/`、`jsr184/` 在本仓库是
  独立子系统，上游 phoneME 树无对应完整实现（JSR-75 上游只有 `USE_JSR_75` 空钩子与
  权限/清理桩，见 `PROJECT_MEMORY.md` 关于 JSR-75 的记录），其来源按目录内注释与
  `PROJECT_MEMORY.md` 追踪，不要指望从官方树里"翻到"。

### 官方离线压缩包：只剩发布清单，不能再下载

- 归档主页：https://phonej2me.github.io/downloads_page.html （页面 HTTP 200）
- MR3 说明页：https://phonej2me.github.io/content/mr3/download_feature.html （HTTP 200）

**坑**：页面上的 zip 链接仍指向 `download.java.net`，实测
`phoneme_feature-mr4-rel-src-b01-20_apr_2009.zip` 等为 `301 → 404`，**下载不下来**。
这些页面只能当作"版本/文件名/构建流程"的历史参考，不要写进任何"一键获取源码"的步骤。
需要源码请走上面的 GitHub 组织 / 组件仓。

---

## 2. 容易搞错的上游

| 仓库 | 实际是什么 | 能不能当上游用 |
|---|---|---|
| `PhoneJ2ME/cldc`、`PhoneJ2ME/midp`、`PhoneJ2ME/pcsl` … | Sun 原工程按模块拆出的镜像 | ✅ 官方镜像，查实现首选 |
| `tberthel/phoneme-components-cldc` / `-midp` | 同一份 Sun 源码按组件拆成 Git 仓（提交说明如此） | ✅ **本仓库工作副本的基线** |
| `sympower/phoneme-midp` | GitHub 描述：*"Fork of Phone-J2ME MIDP JAD tool in order to build a (more) modern Java ME JadTool"* | ❌ **是 JadTool 分支，不是 MIDP 全量源码** |
| `KiddRwxSsj/j2vita` | GPL-2.0，PS Vita 上的 J2ME 运行器（同类项目） | 仅作**同类实现对照**，不作上游 |
| `magicus/phoneME` | umbrella 只读参照树 | ✅ 已作为 `phoneme_source/` 子模块 |

> `j2vita` 与本项目是同一目标平台上的平行实现，可以用来对照目录组织、按键/屏幕策略；
> 但它不是 phoneME 官方基线，**不要用它替换或混入本仓库的工作副本**。

---

## 3. 规范 / API 文档（Javadoc 镜像，查签名首选）

**https://nikita36078.github.io/J2ME_Docs/**

维护者就是 J2ME Loader 的作者 nikita36078，镜像了 Sun/Nokia/厂商的官方 Javadoc 与
规范页。查"这个类的签名/常量/异常到底是什么"时**先查这里**，比在上游源码里翻
mangled 名字快得多。

### 坑：首页链接是反斜杠，点了会 404

首页 HTML 里写的是 Windows 路径（`docs\jsr184`），浏览器解析后 404。
手工拼正斜杠即可（实测全部 200）：

```
https://nikita36078.github.io/J2ME_Docs/docs/<目录名>/
```

### 与本仓库相关的目录（实测标题，2026-11-02）

| 目录 | 内容 | 对应本仓库 |
|---|---|---|
| `cldc-1.1` | CLDC 1.1.1 | `phoneme-cldc/` |
| `midp-2.0` | MID Profile | `phoneme-midp/` |
| `midp-3_0-fr-javadoc` | MIDP 3.0 | 兼容性参考 |
| `jsr75` | FileConnection Optional Package 1.0 | `jsr75/` |
| `jsr75-pim` | PIM Optional Package 1.0 | 未实现（见 `COMPATIBILITY_ROADMAP.md`） |
| `jsr184` | Mobile 3D Graphics API (M3G) | `jsr184/src/share/native/m3g_native.c`、`vita-port/src/vita_3d.c` |
| `Mascot_Capsule_API`、`Motorola3D_API` | Mascot Capsule V3 / EXT3D1.0 | 3D 路线图 |
| `jsr135` | MMAPI | `vita-port/src/vita_audio_javacall.c`、`vita_midi.c` |
| `jsr120`、`jsr205` | WMA 1.1 / 2.0 | JSR-120 桩实现 |
| `jsr211` | CHAPI（内容处理） | 未实现 |
| `jsr239` | OpenGL ES 绑定 | 未实现（可映射 vitaGL） |
| `jsr179-1_1-mrel-javadoc` | Location API | 未实现 |
| `jsr177`、`jsr82_1.1.1_javadoc`、`jsr172` | SATSA / Bluetooth / Web Services | 未实现，优先级低 |
| `jsr226`、`jsr256`、`jsr257`、`jsr280_1.0_spec` | SVG / Sensor / 非接触 / XML | 未实现 |
| `Nokia_UI_API_1_1`、`nokiaapi2` | Nokia UI API 1.1 / 2.0 | `vita-port/nokia_src/` |
| `Siemens_API`、`Samsung_API_1_0`、`Vodafone_API_1_0/2_0`、`Motorola_*`、`LG_MMPP_API` | 各厂商 API | `vita-port/nokia_src/vendor/` |
| `DoJa_5_1`、`Star_API_2_0`、`ezplus_2.5/3`、`VibeLight`、`Maps_API`、`BlackBerry_API_7_1_0` | 日系/其他平台 API | 长尾参考 |

> 用在什么地方：①新增/修改厂商类时核对成员签名与返回值约定；②`jsr184/` 的
> M3G 行为（`SkinnedMesh.applySkin`、`Transformable` 层级等）以 `jsr184` 文档 + 
> 上游 `midp`/`jsr184` 源码为准；③写兼容性桩时确认"该抛什么异常"。
> 文档版权归原发布者，只作查阅，不整站复制进本仓库。

---

## 4. 找实现的标准顺序

1. `phoneme_source/phoneME/<模块>/` —— 上游原始实现（**只读**）。
2. 本仓库对应工作副本（`phoneme-cldc/`、`phoneme-midp/`、`vita-port/`、`jsr75/`、
   `jsr184/`）——看我们已有的移植与 `#if defined(VITA)` 分支。
3. `PhoneJ2ME/<模块>` —— 上游树缺文件/缺分支时补查。
4. **J2ME_Docs 镜像** —— 先确认 API 签名/常量/异常（见第 3 节）。
5. 同类项目（J2ME Loader / PSPKVM / FreeJ2ME / KEmulator nnmod / j2vita）——
   只借公开 API 签名、行为语义与设计，见 `.github/agents/jvm-dev.agent.md`
   「外部参考实现」。

查到结论后：改动按 `AGENTS.md` 的最小 diff 规则进行；与上游的差异记录到
`PROJECT_MEMORY.md`；新增的外部来源和核验日期补到本文。

---

## 5. 归档维护规则

- **只登记、不入库**：上游 zip、大体积源码树（含 `phoneme_source/phoneME/` 内容本身）
  不进本仓库提交，submodule 指针固定 commit 即可。
- 登记条目必须写：URL、commit/tag、核验日期、许可证。
- 链接失效不要直接删除历史记录，标注"已失效（日期）"并给出替代来源。
- 许可证注意：phoneME 为 GPLv2 + Classpath Exception；`j2vita`、PSPKVM 为 GPL 系；
  J2ME Loader 为 Apache-2.0。跨项目只借签名/语义/设计，确需引用代码必须满足许可并
  在文件头写明来源。
