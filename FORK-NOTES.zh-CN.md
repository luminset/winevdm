# 本 fork 的改动说明（中文）

> 这是**本 fork 自有**的内容：描述我们在上游 `otya128/winevdm` 之上做了什么，**不会同步回上游**。
> 上游原始说明见 [README.md](README.md)。

## 一、这个 fork 是什么

- **上游基线**：`otya128/winevdm` 的 `master` @ `a31d1c2`（2026-09-22）
- **我们的工作分支**：`otvdm-patched` = 上游基线 **+ 12 个合流补丁**（并修好其中 3 条自身的缺陷）**+ MIDI 修复两处**（映射器设备号符号扩展与 `[otvdm] midiconfig=`、`midiStreamOut16` 的表头大小）**+ 16 位 MIDI 的兜底发声通路（内置 MCI 序列器）+ `-O1` 构建守卫**
- **预编译包**：本仓库 **Releases** 页的 `patched-o1-midip2-20261016`（MinGW-W64 i686、`-O1` Release、已去掉符号表与构建机路径、平铺布局、解压即用，**不含 DOS 载荷**）。**上一版 `patched-o1-midip1-20261016` 保留**——它的可复现性说明见 §三.4。
- **DOS 支持**：单独的扩展包 **`ext-dos-20261016`**（`otvdm-ext-dos-20261016.zip`，3,057,248 B）：解压进 otvdm 部署目录 → 双击 `install-dos.cmd` 装上，双击 `uninstall-dos.cmd` 连自己一起撤干净。主包与它互不依赖，**本仓库里没有它的任何文件**（只加了几行 `.gitignore` 防止将来误提交）
- `master` 保持为**上游纯镜像**，可随时同步；我们的东西都在 `otvdm-patched` 上

### 现成方案调研（"能用现成的就不自研"）

- **结论：没有可用的现成替代品；复用 Wine 的 `mcimidi.c` 是唯一合理路径。** 分三层：
- **① 那个时代的"MIDI 播放器"都是程序，不是驱动**。GitHub 上能查到的 DOS 侧播放器包括 DOSMID（★35）、SOSPLAY（★10）、mindi（★7）、MPUmid（★4）、dmx-midi-player、dos-midi-player-for-speaker 等 —— 它们是**独立程序**，装进 otvdm 也救不了 MPLAYER：MPLAYER 缺的不是"某个播放器"，而是"**能被 MCI 调用的 sequencer 驱动**"。
- **② 现成的 Win16 MCI sequencer 实现不存在**：GitHub 搜索 `midi sequencer win16` = **0 个仓库**，`mciseq` 仅 1 个无关命中。而且即便找到某个 16 位 `.DRV`，按 otvdm 的架构它**也无法自己出声**（16 位侧没有通往现代 Windows 音频的通道，发声必须由宿主侧完成）—— 所以"装个现成的 16 位驱动"这条路在架构上就不成立。
- **③ 于是"复用"落在唯一可行的形态**：逐字节内嵌 Wine 的 `dlls/mciseq/mcimidi.c`（LGPL-2.1+，保留原始版权头，作为 `mmsystem/mcimidi_wine.inc`），自己只写胶水把 MCI 分派接过去。本次"自研"的全部内容就是 **352 行胶水 + 1622 行上游代码**（外加两处缺陷修复）。
- **④ 日本下载站（Vector）逐条核实过**：`Windows3.1 → 画像＆サウンド → 音楽関係` 下 **MIDI関係 28 本**（子分类：MIDIプレイヤ、**MIMPI関係 2 本**、RCPプレイヤ 7 本、ゲームミュージック、ファイル変換），其中标注 **Windows 3.1 的 MIDI 播放器 14 本**、**MS-DOS 的 MIDI 播放器 16 本**。代表条目：**CD/MIDI PLAYER**（Win3.1 免费软件，作者 綾部光男，可连播 CD 与 MIDI、显示 MIMPI 的 WRD 歌词）、**Lemon 2.51a**（MID/RCP/R36/G18/G36 播放器）、**美人 16bit版**（WAV/MID/**RMI** 批量登记与 BGM 连播）、**SELSONG/WIN**（"把 MIMPI 或 **MEDIA PLAYER** 连起来演奏"的选曲器）、Sequence Studio Lite（音序器）、GSR/Win（MIDI 监视器）。**结论没变**：这些全是**应用程序**，都要靠系统里已有的 MIDI 驱动（Windows 3.1 日文圈走的是 **MIMPI** 或声卡自带驱动）才发得出声 —— 和 MPLAYER 是同一套架构；`SELSONG/WIN` 这种"给 MEDIA PLAYER 选曲"的软件恰好说明当年 MPLAYER 就是那个播放器。
- **⑤ 真正自称"驱动"的只有两个，都帮不上忙**：**RS232C MIDI ドライバ 1.50**（"Microsoft Windows 3.1 RS232C-MIDI driver"，免费，6K）驱动的是**外接串口 MIDI 硬件**；**MIDI PatchBay Driver 1.1**（"多个软件同时使用 Windows 的 MIDI 设备"）是在**已有 MIDI 设备之上**做复用。两者都不提供"在现代 Windows 上出声"的通路。
- **⑥ "可复用的 SMF 代码"只到解析层**：搜到的现成件（`midifile`（C 的 SMF 解析库）、`MStefan99/Midi-Parser`、以及若干日文教程与示例）都只做**文件解析**；而 Wine 的 `mcimidi.c` 是**完整的 MCI 播放器**（解析 + 拍子/速度调度 + `midiOutShortMsg` 输出 + MCI 命令语义），且同样 LGPL 兼容 —— 所以复用它是正确取舍。
- **过程记录**：以上日本站与检索结果是通过**局域网代理**访问得到的（本机直连时这些域名被解析到非公网地址、无法访问）；代理地址属于本机环境信息，只记录在团队记忆里，不写进仓库。

## 二、改了什么：12 个提交（按主题分组）

### 1. 16 位多媒体 / 驱动（本次重点）

| 提交 | 做了什么（白话） |
|---|---|
| `ef4f43e` | 把 16 位驱动调用（`DrvOpen`/`SendDriverMessage`）从 `user.exe16` 转发到 `mmsystem.dll16`，并重定向 `SYSTEM.INI` —— 音频兼容性的主补丁 |
| `d152621` | **修上面这条的两个缺陷**：`SYSTEM.INI` 路径改为从 `GetWindowsDirectoryA()` 拼（原写法硬编码 `c:\windows\system.ini`、绕过重定向，导致 16 位侧永远读不到包内配置）；并在 `strcat` 前补长度检查 |
| `cd17af3` | `mmioDescend16` 遇到"父块顺序不对"时忽略父块，对齐 Win16 行为（Win32 会直接报错返回） |
| `61bc422` | 修 `mmioSendMessage16` 的 thunk 指针传参、以及 `pfn16` 为空时的处理（两行笔误级修复） |

### 2. 定时器

| 提交 | 做了什么 |
|---|---|
| `0c70123` | 照 wine 的做法，用 32 位 `SetTimer` 实现 `SetSystemTimer16` |
| `4df7863` | **配套的那一步**：让 `SetSystemTimer16` 的回调收到 Win16 语义的 `WM_SYSTIMER`（而不是带标志位的 `WM_TIMER`）。**这两条必须成对**——只打第一条，Win16 应用会收不到 `WM_SYSTIMER` |

### 3. 文件对话框（thunk 层同步）

| 提交 | 做了什么 |
|---|---|
| `a6ce5a3` | 在调用 16 位 `OFN` 的 `FileNameOK` 钩子**之前**，把 32 位的 `OPENFILENAMEW` 同步回 16 位结构体（原来钩子读到的是过期结构体，属这类 thunk 层的典型 bug） |
| `5a6a99f` | 同一作者同一主题的收尾：统一 `OPENFILENAME16` 的同步点；单独看它依赖 `a6ce5a3` |

### 4. NE 加载器 / 文件 API / OLE / 安全

| 提交 | 做了什么 |
|---|---|
| `d8a0b84` | NE 加载器：磁盘长度为 0、但 `minalloc` 非 0 的数据段其实是 BSS，不该再从文件读 `size` 字节填进去（原来会把无关文件字节灌进已清零的段，Borland 程序会触发 #GP 0D） |
| `043df7c` | `_lopen`/`_lcreat` 遇到非法文件名指针时返回错误，而不是崩溃 |
| `c3861a8` | `olecli`：加载流期间释放 thunk 锁，避免重入死锁 |
| `ef82d7b` | 消除静态扫描报的不安全 `strncat` 用法与 use-after-free |

> **3 组补丁不能拆开带走**：`ef4f43e`+`d152621`、`0c70123`+`4df7863`、`a6ce5a3`+`5a6a99f`。

## 三、我们自己加的东西（上游没有）

### 1. MinGW 构建必须用 `-O1`（本 fork 最重要的发现）

- MinGW-W64 i686 在 `-O2`/`-O3` 下会**误编译** `krnl386/resource.c`：GCC 判定某条路径不可达，生成 `jmp <self>`（机器码 `EB FE`）自跳转陷阱，而它正好落在每个 GUI 程序启动必经的 `LoadIcon16 → LoadImage16 → FindResource16` 上 —— 结果是**任何** 16 位程序一启动就 100% CPU 死转、不出窗口。
- 自跳转计数实测：`-O0`=0、`-O1`=0、`-O2`=2、`-O3`=3；整个工程在 `-O3` 下有 16 个 TU 含自跳转。
- **`-Os`（MinSizeRel）与 `-O2` 同类不安全**，误编译**也不限于 `resource.c`**：工具链端实测 MinSizeRel 产物与 `-O2` 的自跳转数量完全一致（krnl386.exe16 5 处、user.exe16 6 处；同口径下 `-O1` 为 0），命中点还包括 krnl386 的 `_AddAtom16@4`、`_FindAtom16@4` 与 user 的 `_BuildCommDCB16@8`、`_OpenComm16@12` 等。`resource.c` 只是启动必经路径上最显眼的那个，所以**别只给这一个文件打补丁，要按整个工程降级优化**。
- **这不是源码 bug，是编译器 bug**；上游 CI 里的 GCC 任务没有传 `-DCMAKE_BUILD_TYPE`（CMake 因此不加任何 `-O`，实际是 `-O0`），MSVC 任务更看不见 —— 所以上游一直没踩到，只有显式做 `Release` 构建的人才会中招。
- 同类误编译还让构建期的 `widl.exe` 在 `-O2/-O3` 下挂死（表现为"编译卡在 94%"）。
- 正确的旗标写法（**注意**：`CMAKE_C_FLAGS` 会被 Release 默认的 `-O3` 覆盖，光写它没用）：

```bash
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="-w"   -DCMAKE_CXX_FLAGS="-w" \
      -DCMAKE_C_FLAGS_RELEASE="-O1 -DNDEBUG" \
      -DCMAKE_CXX_FLAGS_RELEASE="-O1 -DNDEBUG" \
      ../src

# 核实有没有真的生效（别看命令行，看生成的文件）
find . -path '*krnl386.dir/flags.make' | head -1 | xargs grep 'C_FLAGS = '
# 期望： C_FLAGS = -w -O1 -DNDEBUG
```

- 根 `CMakeLists.txt` 里**已经带上了守卫**（原主题分支 `mingw-o1-guard` 的两个提交已并入 `otvdm-patched`）：当 MinGW + GCC 且**当前配置**的旗标里含 `-O2`/`-O3`/`-Os` 时自动降为 `-O1` 并打印一行原因。它按当前 build type 取变量名（`CMAKE_<LANG>_FLAGS_<CONFIG>`），所以 `Release`、`RelWithDebInfo`、`MinSizeRel` 三种配置都**真的**生效；显式给的 `-O2`/`-O3`/`-Os` 同样降级，显式 `-O1` 不动。原先那条独立分支仍留在远端（`mingw-o1-guard`），作为这段改动的历史位置，将来若要单独提交给上游从那里取即可。

### 2. MinGW 版交付包的两个必要调整

- **必须平铺目录**：`otvdm.exe` 静态导入 `krnl386.exe16`，官方包的 `dll/` 子目录布局只对 MSVC 版有效。
- **必须随包带 MinGW 运行时**：`libgcc_s_dw2-1.dll`、`libstdc++-6.dll`、`libwinpthread-1.dll`，否则报 "error while loading shared libraries"。

### 3. 16 位 MIDI 流输出的一处真实缺陷（我们自己查出来并修好的）

- 症状：16 位程序调 `midiStreamOut`（`MMSYSTEM.254`）时**静默无声、不报错**。
- 根因：`midiStreamOut16` 把应用给的 16 位 `MIDIHDR16` **直接当 32 位 `MIDIHDR` 交给宿主**。两者布局不同 —— 16 位版里的 `lpData` 仍是 16 位指针，尾部还少 `dwOffset`/`dwReserved` —— 宿主于是读到一个坏指针。**不是"漏了 `MapSL`"**：`.spec` 里标 `ptr`/`str`/`wstr` 的参数，convspec 生成的 thunk 已经把它换算成 32 位线性地址，只有 `segptr`/`segstr` 才需要实现自己 `MapSL`。
- 修法沿用本树自己的约定：`MODM_PREPARE` 早就为这个 16 位表头建过 32 位影子、并把地址停在 `reserved` 字段（注释原文就是 "for reuse in unprepare **and write**"）。于是取回影子、按 `MODM_LONGDATA` 的做法刷新 `dwBufferLength`，长度用本树既有的 `offsetof(MIDIHDR, dwOffset)`（=28）；没 prepare 过的头返回 `MIDIERR_UNPREPARED`（先判 `MHDR_PREPARED`，因为 `reserved` 是应用可写字段、未清零时可能是垃圾值）。改动 3 文件 / +30 −2。
- **验证范围照实说**：源码级核对 + 编译 + 16 程序 A/B 回归矩阵与基线 **IDENTICAL**（无回归），但**没有端到端验证** —— 交付包里没有会调 `midiStream*` 的 Win16 程序。「放 MIDI 没声音」这个原始观感已定案为 **MPLAYER 自动播放没触发**（手动点播放有声），与这处缺陷无关。

### 4. 预编译包

- Releases 页的 `patched-o1-midip1-20261016`：MinGW-W64 i686 `-O1` Release、平铺布局、带运行时 DLL，解压即用（解压后得到一层 `otvdm-patched-o1-midip1-20261016\` 目录）；157 个条目 = 156 个内容文件 + `SHA256SUMS.txt`，**2,445,266 B**（sha256 `6d2ccc1a…`），**不含 DOS 载荷**（DOS 走扩展包 `ext-dos-20261016`）。
- 这一版相对上一版**只重编了 `mmsystem.dll16` 一个模块**，改动三处：① **`midiStreamOpen16` 的设备号符号扩展**——16 位侧的 MIDI 映射器设备号是 WORD `0xFFFF`，原代码零扩展成 `0x0000FFFF`，宿主 winmm 判为非法设备号（`MMSYSERR_BADDEVICEID`）→ **任何请求映射器的流式打开都会被拒**；现按 `(INT16)` 符号扩展，与本来正确的 `midiOutOpen16` 一致。② **新增 `otvdm.ini` 的 `[otvdm] midiconfig=`**——逐部署选择宿主 MIDI 输出设备（等价 DOSBox-X 的同名配置），值可以是设备序号或设备名子串，**只重定向「映射器」请求**，值非法时静默回退，不写这一行 = 改版前行为。③ `midiStreamOut16` 的 `MHDR_PREPARED` 加固。156 个内容文件里 **153 个与上一版逐字节相同**。
- **源码状态照实说**：改动 ①②③ 的源码**现在都已在 `otvdm-patched` 分支里**（①② 见提交 `eb221be7`，③ 见 `32bf26b1`；`git log -S midiconfig` 现在有命中）。发布当时它们还只存在于工作区，所以这一版的 `mmsystem.dll16` 领先于 tag 所指的提交。
- **可复现性照实说（2026-10-16 补测，此前写的"合流后会变成可复现"不成立）**：这一版的 `mmsystem.dll16` **实测无法逐字节复现**——用 `32bf26b1` 干净重建差 **1674 字节**（换仓库路径重建、构建目录放源码树内/外，结果都一样），用 `eb221be7` 重建差 1677 字节。差异**几乎全部来自构建脚本里 `-ffile-prefix-map` 的目的串**：当时写成 `.`，现在写成空串，内嵌路径串因此是 `.\mmsystem\…` 与 `\mmsystem\…` 两种形式；把目的串改回 `.` 重建后，差异降到 **14 字节**（其中 11 字节是 PE 记账：时间戳两处拷贝 + 由它决定的校验和，另 3 个单字节落在 `.text` 内，未归属）。
- **但功能上确属这一版**：P1 引入的字面量指纹（`midiconfig` 等）与干净重建**完全一致**，P2 专有的标记为 0，导入 DLL 表与未打补丁版**逐项一致**（8 个）。所以差别是**构建参数**而非源码，这一版可以放心用，只是**字节无法自证**。
- **下一版会把可复现性做实**：发布流程改为"打包后必须用同提交、不同路径的干净重建与包内 DLL 逐字节核对"，并把构建脚本纳入版本管理——它此前只存在于构建机上，所以这一版的构建输入已无法从仓库重建。
- **注意**：MinGW 版**每个 16 位程序会出现两个 `otvdm.exe` 进程**，这是正常的（靠 `--fix-compat-mode` 子进程真正运行应用）。**判断是否死转要看 CPU 时间，不要看进程数。**
- 包已**去掉 COFF 符号表与内嵌的构建机路径**（链接 `-Wl,-s`，编译 `-ffile-prefix-map` + `-g0`）：带符号表与路径的早期版本是 5,921,317 B；官方 v0.9.0 为 1.46 MB。

### 5. 16 位 MIDI 的兜底发声通路（内置 MCI 序列器，随 `patched-o1-midip2` 发布）

- **为什么需要**：16 位程序通过 MCI 命令接口请求"MIDI 映射器"（设备号 `-1`）时，本树把它截断成 `0xFFFF` 交给宿主 winmm，宿主判为非法设备号；而走 `driverProc` 直调又绕过了 winmm 的返回值翻译，于是 `MPLAYER.EXE` 这类程序会**判定设备不可用并静默无声**。
- **做法（复用现成实现，没有自研）**：把 Wine 的 MCI 序列器（`dlls/mciseq/mcimidi.c`，**LGPL 2.1+**，版权头完整保留、文件内容逐字节未改）内嵌为 `mmsystem\mcimidi_wine.inc`，接进 16 位 MCI 分派：默认**先走宿主**，宿主打不开才切内置；`[otvdm] midiseq=internal` 可强制。
- **落到系统默认输出**：内置序列器打开时若宿主存在 MIDI 输出设备，就发 `MCI_SET`/`MCI_SEQ_SET_PORT = 0` 指向**系统默认输出**——不碰第三方驱动、不做 MAPPER 探测。
- **覆盖的入口**：`MCI_OPEN_TYPE_ID`、类型串 `"sequencer"`、以及**按扩展名推导**（清单：`mid` / `midi` / `rmi`）——`MPLAYER` 走的是最后这条。
- **零回归**：默认配置下 `.mid`/`.rmi` 仍由宿主（设备号 `0001`）处理，内置路径一行都不执行；导出表逐字一致，导入只新增 2 条。
- **已知边界**：字符串接口（`mciSendString`）不走这条兜底；兜底只在 `MCI_OPEN` 时刻判断一次；内置序列器**不认 `[otvdm] midiconfig=`**（该键只作用于宿主优先那条路径），也不出现在设备枚举里。
- **验证范围见下节**。关于"是否真的响了"：**已实测到信号**——用系统默认渲染端点的 WASAPI 电平表（`p2-mciseq\hosttest\audiometer.cs`）测量内置序列器播放 `CANYON.MID`：**峰值 0.735（-2.7 dBFS）**，且与请求的 6 秒严格同步（第 6 秒起归零）；同一次测量带**阴性对照**（什么都不放 = 0.000000）与**阳性对照**（系统 `Windows Ding.wav` = 0.496 / -6.1 dBFS），端点未静音、主音量 32%。⇒ 在这台开发机上确实发声，不再只能靠人耳。
- **一个继承自 Wine 的小限制**：`CANYON.MID` 的第一条轨是 SMPTE 计时，Wine 的播放器不实现 SMPTE 轨起点（日志 `NIY: SMPTE track start 96:0:0 0.0`），会跳过该轨；音乐照常播放，这不是本版引入的。
- **★ 16 位整条路也实测发声了**（不再是推断）：真实 Win3.1 16 位程序 `MPLAYER.EXE` 打开 `CANYON.MID`、强制 `midiseq=internal` 后点播放，trace 显示 `MIDI_mciOpen wDevID=61440`（即 `0xF000`，宿主 `0x0001` 出现 **0** 次）、`MIDI_mciPlay`、`dwStatus=526`（播放）**3044** 行且 `525`（停止）**0** 行、`Wait until` 从 0 推进到 **47004 ms**（1260 个脉冲）、`not translated` / `MMSYSERR` / `MCIERR` 全 **0**；**与此同时**电平表连续 **16 秒**测到峰值 **0.646（-3.8 dBFS）**。⇒ "16 位程序播 MIDI 真的响"已经是同时观测到的事实。
- **★ 重要更正（用户口径，2026-10-16）：MPLAYER 完全不能自动播放。** 上一轮我据"三次运行（`e2e\A-stock` / `B-p2default` / `C-p2internal`）里我自己没有点击，却出现 `(playing)`"推出"设备可用时它会自己播"，**这个推论是错的**：那几次的播放状态是**用户手动点击**造成的 —— 我的 A/B/C **没有控制住"用户是否点击"这个变量**，所以旧/新 DLL 在"自动播放"上的差异**不成立**，已全部撤回。
- **16 位那段仍然有效的结论只有这些**：在**有人点了 Play 之后**，本版 DLL 下设备确实被打开并驱动（trace 走内置序列器 `wDevID=61440`，位置推进到 **43126 ms**），**同时**电平表测到持续发声（峰值 0.785 / 均值 0.344）；默认宿主路径下同样持续发声（峰值 0.700 / 均值 0.318）且 trace 里 **0** 条内置代码行。⇒ **"点播放之后有声"成立；"是否会自动播放"不作为本版结论。**
- 定向投递窗口消息（`e2e\winclick.cs` + `mplayer-probe.ps1`，投给单个 HWND、不用 `SendKeys`）保留为工具，但它**是否真能启动 MPLAYER 播放尚未验证**（当时无法与用户的手动点击区分）。
- **已知外观差异（不影响发声）**：走默认宿主路径时标题显示 `(playing)`；强制 `midiseq=internal` 时标题的状态字为空（`Media Player - CANYON.MID ()`）。

## 四、验证到什么程度（请不要过度宣称）

- ✅ **16 个公版 Windows 3.11 中文附件**：15 个正常出窗口（9 秒墙钟内 CPU 均 < 1.3 秒）；与"未打补丁的同 `-O1` 构建"和"官方 v0.9.0（MSVC）"逐项 A/B 对比，**无回归**。`REGEDIT.EXE` 三个版本都不出窗口，属既有问题。
- ✅ **MCI waveaudio 驱动链路**：`MPLAYER.EXE CHIMES.WAV` 能通过重定向后的 `WINDOWS\SYSTEM.INI` 打开 16 位驱动；声音控制面板能枚举目录里的 WAV 文件。
- ✅ **MIDI 流输出修复（`midiStreamOut16`）**：编译通过，16 程序 A/B 回归矩阵与基线 IDENTICAL；**无端到端验证**（包内没有调用 `midiStream*` 的 Win16 程序），详见上面 §三.3。
- ⚠️ **第二轮 MIDI 改动（`midiStreamOpen16` 符号扩展 + `[otvdm] midiconfig=`）**：源码级分析 + **宿主侧调用实测**（32 位程序直接调 winmm：`midiStreamOpen(0xFFFFFFFF)` 成功、`(0x0000FFFF)` 返回 `MMSYSERR_BADDEVICEID`(2)）+ 16 程序 A/B 回归矩阵三轮（默认 / `midiconfig=0` / `midiconfig=99`）与基线逐行一致；**没有正向听测** —— 构建机只有一块宿主 MIDI 设备、也没有会调 `midiStreamOpen` 的 Win16 程序，所以「换设备是否真的换音色」要在装了第三方 MIDI 驱动的目标机上确认。**这两处源码现已合流到 `otvdm-patched`（提交 `eb221be7`），但已发布那一版的字节无法复现**（详见 §三.4 的"可复现性照实说"）。
- ✅ **第三轮：16 位 MIDI 的兜底发声通路（内置 MCI 序列器，见 §三.5）**：宿主侧实测（`midiOutGetNumDevs()` = 1、设备 0 为 `SWSYNTH`、open / 短消息 / close 全部返回 0）；MCI 命令接口双向对照（默认端口返回 `MCIERR_SEQ_PORT_NONEXISTENT`(338)、0 ms 就结束，改指系统默认端口后返回 0、按 2000 ms 播完）；强制内置时逐条检查**每个音是否被设备接受**（0 条告警）；**真实 16 位端到端**：Win3.1 `MPLAYER.EXE` 打开 `.MID` 解析出 1 轨 / 480 ppq / 33861 字节、长度 2:52、播放位置持续推进，`.RMI` 同样读出 172000 ms；默认路径 A/B **零回归**（宿主设备号 `0001`，内置零调用）；生命周期 37 项 + 扩展名 17 项 + 宿主 MIDI 接受性 13 项全部通过；发布用构建可逐字节复现；**并在系统默认渲染端点的电平表上实测到真实信号（峰值 -2.7 dBFS，含阴性/阳性对照），即"确实发声"这一步已用软件测量，不再只靠人耳；进一步在真实 16 位程序（MPLAYER，走内置序列器 `0xF000`）播放时同时测到连续 16 秒、峰值 -3.8 dBFS 的信号**。
- ✅ **时钟动画（定时器路径）**：三个版本都在动（连续截图像素哈希互不相同）。
- ⚠️ **有 6 个提交只做了源码审查 + 编译 + 无回归验证，没有正向功能验证**：`cd17af3`（需 Morphman 一类复用 `MMCKINFO` 缓冲的程序）、`d8a0b84`（需 Borland 编译的程序）、`a6ce5a3`/`5a6a99f`（需带 `OFN_ENABLEHOOK` 的 16 位应用）、`c3861a8`（需 OLE 客户端）、`043df7c`（需故意传坏指针的程序）、`61bc422`（需直接调 `mmioSendMessage` 的程序）。**在此之前请勿对外宣称这些已修复。**
- ⚠️ MPlayer 按空格播放不出声，在**三个版本上都复现**（含官方 v0.9.0）→ 上游/环境限制，不是本分支造成的。
- ❌ 本仓库**不含**任何 Windows 3.x 附件二进制（微软版权），测试请自备安装介质。

## 五、与上游的关系

- 本分支**未提 PR**，只存在于本 fork；上游只走 PR，绝不直接推 `upstream`。
- `master` 保持为**上游纯镜像**，可随时同步（GitHub 的 "Sync fork"，或本地 `git fetch upstream && git merge --ff-only upstream/master`）。
- **本文件与 `README.md` 顶部的提示属于本 fork 自有内容**，不打算同步回上游。**目前不向上游提 PR**，改动先在自己仓库维护。将来若提 PR，请从**干净的 12 提交处**另开分支（tag `patched-o1-20261014` / 提交 `4df7863`），不要带上这些说明提交；`-O1` 守卫那两个提交在 `mingw-o1-guard` 上可单独取。

## 六、许可

GPL-2.0，与上游一致。源码 = 本仓库的 `otvdm-patched` 分支，或对应 tag 附带的自动源码包。
