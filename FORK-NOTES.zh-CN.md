# 本 fork 的改动说明（中文）

> 这是**本 fork 自有**的内容：描述我们在上游 `otya128/winevdm` 之上做了什么，**不会同步回上游**。
> 上游原始说明见 [README.md](README.md)。

## 一、这个 fork 是什么

- **上游基线**：`otya128/winevdm` 的 `master` @ `b88d7f87`（2026-11 同步：包含上游 5 个提交 / 2 个 PR 合并，已合进 `nohafix`；上一个基线是 `a31d1c2`）
- **我们的工作分支**：`nohafix` = 上游基线 **+ 12 个合流补丁**（并修好其中 3 条自身的缺陷）**+ MIDI 修复两处**（映射器设备号符号扩展与 `[otvdm] midiconfig=`、`midiStreamOut16` 的表头大小）**+ 16 位 MIDI 的兜底发声通路（内置 MCI 序列器）+ 按需开启的虚拟 C 根 + `-O1` 构建守卫**
- **预编译包**：本仓库 **Releases** 页的 `patched-o1-midip2-20261016`（MinGW-W64 i686、`-O1` Release、已去掉符号表与构建机路径、平铺布局、解压即用，**不含 DOS 载荷**）及其上一版 `patched-o1-midip1-20261016`——两版的可复现性说明见 §三.4。**虚拟 C 根的 `vcroot` 版暂未发布**（尝试性修改，见 §三.6 与 §4 末尾）。
- **DOS 支持**：单独的扩展包 **`ext-dos-20261105`**（`otvdm-ext-dos-20261105.zip`，**3,058,623 B**，932 条目，**zip 无顶层目录**，解压直接铺在 otvdm 部署目录根）：双击 `install-dos.cmd`；安装时按系统位数**直接指向** `msdos\x64\msdos.exe` 或 `msdos\x86\msdos.exe`，**不再复制根 `msdos.exe`**；双击 `uninstall-dos.cmd` 连自己一起撤干净。**旧包 `ext-dos-20261016` 已被本版替代并从 Releases 删除**（旧包是「单顶层目录 + 安装时复制根副本」的结构）。主包与它互不依赖，**本仓库里没有它的任何文件**（只加了几行 `.gitignore` 防止将来误提交）
- `master` 保持为**上游纯镜像**，可随时同步；我们的东西都在 `nohafix` 上；**对上游暂不提交**（2026-10-17：PR #1627 已关闭、通道分支已删除，只在确实必要时重建，见 §五）

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

- 根 `CMakeLists.txt` 里**已经带上了守卫**（原主题分支 `mingw-o1-guard` 的两个提交已并入 `nohafix`）：当 MinGW + GCC 且**当前配置**的旗标里含 `-O2`/`-O3`/`-Os` 时自动降为 `-O1` 并打印一行原因。它按当前 build type 取变量名（`CMAKE_<LANG>_FLAGS_<CONFIG>`），所以 `Release`、`RelWithDebInfo`、`MinSizeRel` 三种配置都**真的**生效；显式给的 `-O2`/`-O3`/`-Os` 同样降级，显式 `-O1` 不动。原先那条独立分支（`mingw-o1-guard`）已删除：内容就在 `nohafix` 里（提交 `8ea3a5b9`、`9d1073c4`），将来若要单独提给上游，从这里拣这两条即可。

### 2. MinGW 版交付包的两个必要调整

- **必须平铺目录**：`otvdm.exe` 静态导入 `krnl386.exe16`，官方包的 `dll/` 子目录布局只对 MSVC 版有效。
- **必须随包带 MinGW 运行时**：`libgcc_s_dw2-1.dll`、`libstdc++-6.dll`、`libwinpthread-1.dll`，否则报 "error while loading shared libraries"。

### 3. 16 位 MIDI 流输出的一处真实缺陷（我们自己查出来并修好的）

- 症状：16 位程序调 `midiStreamOut`（`MMSYSTEM.254`）时**静默无声、不报错**。
- 根因：`midiStreamOut16` 把应用给的 16 位 `MIDIHDR16` **直接当 32 位 `MIDIHDR` 交给宿主**。两者布局不同 —— 16 位版里的 `lpData` 仍是 16 位指针，尾部还少 `dwOffset`/`dwReserved` —— 宿主于是读到一个坏指针。**不是"漏了 `MapSL`"**：`.spec` 里标 `ptr`/`str`/`wstr` 的参数，convspec 生成的 thunk 已经把它换算成 32 位线性地址，只有 `segptr`/`segstr` 才需要实现自己 `MapSL`。
- 修法沿用本树自己的约定：`MODM_PREPARE` 早就为这个 16 位表头建过 32 位影子、并把地址停在 `reserved` 字段（注释原文就是 "for reuse in unprepare **and write**"）。于是取回影子、按 `MODM_LONGDATA` 的做法刷新 `dwBufferLength`，长度用本树既有的 `offsetof(MIDIHDR, dwOffset)`（=28）；没 prepare 过的头返回 `MIDIERR_UNPREPARED`（先判 `MHDR_PREPARED`，因为 `reserved` 是应用可写字段、未清零时可能是垃圾值）。改动 3 文件 / +30 −2。
- **验证范围照实说**：源码级核对 + 编译 + 16 程序 A/B 回归矩阵与基线 **IDENTICAL**（无回归），但**没有端到端验证** —— 交付包里没有会调 `midiStream*` 的 Win16 程序。「放 MIDI 没声音」这个原始观感已定案为 **MPLAYER 自动播放没触发**（手动点播放有声），与这处缺陷无关。

### 4. 预编译包

- Releases 页的 `patched-o1-midip1-20261016`：MinGW-W64 i686 `-O1` Release、平铺布局、带运行时 DLL，解压即用（解压后得到一层 `otvdm-patched-o1-midip1-20261016\` 目录）；157 个条目 = 156 个内容文件 + `SHA256SUMS.txt`，**2,445,266 B**（sha256 `6d2ccc1a…`），**不含 DOS 载荷**（DOS 走扩展包 `ext-dos-20261105`）。
- **注意**：MinGW 版**每个 16 位程序会出现两个 `otvdm.exe` 进程**，这是正常的（靠 `--fix-compat-mode` 子进程真正运行应用）。**判断是否死转要看 CPU 时间，不要看进程数。**
- 包已**去掉 COFF 符号表与内嵌的构建机路径**（链接 `-Wl,-s`，编译 `-ffile-prefix-map` + `-g0`）：带符号表与路径的早期版本是 5,921,317 B；官方 v0.9.0 为 1.46 MB。

### 5. 16 位 MIDI 的兜底发声通路（内置 MCI 序列器，随 `patched-o1-midip2` 发布）

- **为什么需要**：16 位程序通过 MCI 命令接口请求"MIDI 映射器"（设备号 `-1`）时，本树把它截断成 `0xFFFF` 交给宿主 winmm，宿主判为非法设备号；而走 `driverProc` 直调又绕过了 winmm 的返回值翻译，于是 `MPLAYER.EXE` 这类程序会**判定设备不可用并静默无声**。
- **做法**：把 Wine 的 MCI 序列器（`dlls/mciseq/mcimidi.c`，**LGPL 2.1+**，版权头完整保留、文件内容逐字节未改）内嵌为 `mmsystem\mcimidi_wine.inc`，接进 16 位 MCI 分派：默认**先走宿主**，宿主打不开才切内置；`[otvdm] midiseq=internal` 可强制。
- **落到系统默认输出**：内置序列器打开时若宿主存在 MIDI 输出设备，就发 `MCI_SET`/`MCI_SEQ_SET_PORT = 0` 指向**系统默认输出**——不碰第三方驱动、不做 MAPPER 探测。
- **覆盖的入口**：`MCI_OPEN_TYPE_ID`、类型串 `"sequencer"`、以及**按扩展名推导**（清单：`mid` / `midi` / `rmi`）——`MPLAYER` 走的是最后这条。
- **零回归**：默认配置下 `.mid`/`.rmi` 仍由宿主（设备号 `0001`）处理，内置路径一行都不执行；导出表逐字一致，导入只新增 2 条。
- **已知边界**：字符串接口（`mciSendString`）不走这条兜底；兜底只在 `MCI_OPEN` 时刻判断一次；内置序列器**不认 `[otvdm] midiconfig=`**（该键只作用于宿主优先那条路径），也不出现在设备枚举里。

## 四、验证到什么程度（请不要过度宣称）

- ✅ **16 个公版 Windows 3.11 中文附件**：15 个正常出窗口（9 秒墙钟内 CPU 均 < 1.3 秒）；与"未打补丁的同 `-O1` 构建"和"官方 v0.9.0（MSVC）"逐项 A/B 对比，**无回归**。`REGEDIT.EXE` 三个版本都不出窗口，属既有问题。
- ✅ **MCI waveaudio 驱动链路**：`MPLAYER.EXE CHIMES.WAV` 能通过重定向后的 `WINDOWS\SYSTEM.INI` 打开 16 位驱动；声音控制面板能枚举目录里的 WAV 文件。
- ✅ **MIDI 流输出修复（`midiStreamOut16`）**：编译通过，16 程序 A/B 回归矩阵与基线 IDENTICAL；**无端到端验证**（包内没有调用 `midiStream*` 的 Win16 程序），详见上面 §三.3。
- ⚠️ **有 6 个提交只做了源码审查 + 编译 + 无回归验证，没有正向功能验证**：`cd17af3`（需 Morphman 一类复用 `MMCKINFO` 缓冲的程序）、`d8a0b84`（需 Borland 编译的程序）、`a6ce5a3`/`5a6a99f`（需带 `OFN_ENABLEHOOK` 的 16 位应用）、`c3861a8`（需 OLE 客户端）、`043df7c`（需故意传坏指针的程序）、`61bc422`（需直接调 `mmioSendMessage` 的程序）。**在此之前请勿对外宣称这些已修复。**
- ⚠️ MPlayer 按空格播放不出声，在**三个版本上都复现**（含官方 v0.9.0）→ 上游/环境限制，不是本分支造成的。
- ❌ 本仓库**不含**任何 Windows 3.x 附件二进制（微软版权），测试请自备安装介质。

### legacy installer 启动提示（2026-11-06）

- `KBMIDI.EXE` / `MIDIE100.EXE` 这类 16 位外壳的 Wise/InstallShield 老安装器，启动后会解包私有安装引擎；实测其安装目录 `C:\KBMIDI`、`C:\MIDIE100` 仍落到真实 C 盘，虚拟 C 根无法覆盖。
- `krnl386/file.c` 新增 `WarnLegacyInstaller()`：在 `LoadModule16` 启动 EXE 前扫描 `GLBSInstall` / `GLBSSTUB` / `WiseMain` / `InstallShield` / `UNWISE` / `STUB.exe` / `_INS` 标记，**仅当 `[otvdm] VirtualDrive` 显式存在且非空时**弹一次 `otvdm - legacy installer` 警告，并写 `ERR` 日志；未显式配置时按系统集成方式运行，不弹提示。
- 实测：显式配置 `VirtualDrive=C` 时 KBMIDI、MIDIE100 启动即弹警告；默认注释/缺省配置不弹；CALC.EXE 不弹。警告只提示，不改变真实写入；处理方式仍是沙箱/VM，或把安装路径改到 `<otvdm>\C\...`。
- 另保留 `Warn32BitChild()`（`ne_module.c` / `dosexe.c` / `shell.c` / `winoldap.c` 的 32 位子进程入口），作为通用 32 位子进程的补充提示。

## 五、与上游的关系

- **本 fork 只保留三条长期分支**，各有明确用途：

  | 分支 | 用途 |
  | --- | --- |
  | `master` | 上游 `master` 的**纯镜像**，不带任何 fork 内容 |
  | `nohafix` | **我们自己的主线**：上游基线 + 全部 fork 补丁，发布包从这里构建 |


## 六、许可

GPL-2.0，与上游一致。源码 = 本仓库的 `nohafix` 分支，或对应 tag 附带的自动源码包。
