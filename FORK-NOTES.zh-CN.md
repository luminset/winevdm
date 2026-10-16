# 本 fork 的改动说明（中文）

> 这是**本 fork 自有**的内容：描述我们在上游 `otya128/winevdm` 之上做了什么，**不会同步回上游**。
> 上游原始说明见 [README.md](README.md)。

## 一、这个 fork 是什么

- **上游基线**：`otya128/winevdm` 的 `master` @ `a31d1c2`（2026-09-22）
- **我们的工作分支**：`otvdm-patched` = 上游基线 **+ 12 个合流补丁**（并修好其中 3 条自身的缺陷）**+ 一处 MIDI 修复 + `-O1` 构建守卫**
- **预编译包**：本仓库 **Releases** 页的 `patched-o1-midi-20261016`（MinGW-W64 i686、`-O1` Release、已去掉符号表与构建机路径、平铺布局、解压即用；157 个条目 / 2,443,216 B，**不含 DOS 载荷**）
- **DOS 支持**：单独的扩展包 **`ext-dos-20261016`**（`otvdm-ext-dos-20261016.zip`，3,057,248 B）：解压进 otvdm 部署目录 → 双击 `install-dos.cmd` 装上，双击 `uninstall-dos.cmd` 连自己一起撤干净。主包与它互不依赖，**本仓库里没有它的任何文件**（只加了几行 `.gitignore` 防止将来误提交）
- `master` 保持为**上游纯镜像**，可随时同步；我们的东西都在 `otvdm-patched` 上

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

- Releases 页的 `patched-o1-midi-20261016`：MinGW-W64 i686 `-O1` Release、平铺布局、带运行时 DLL，解压即用（解压后得到一层 `otvdm-patched-o1-midi-20261016-nodos\` 目录）；157 个条目 = 156 个内容文件 + `SHA256SUMS.txt`，**2,443,216 B**，**不含 DOS 载荷**（DOS 走扩展包 `ext-dos-20261016`）。
- **注意**：MinGW 版**每个 16 位程序会出现两个 `otvdm.exe` 进程**，这是正常的（靠 `--fix-compat-mode` 子进程真正运行应用）。**判断是否死转要看 CPU 时间，不要看进程数。**
- 当前包已**去掉 COFF 符号表与内嵌的构建机路径**（链接 `-Wl,-s`，编译 `-ffile-prefix-map` + `-g0`）：带符号表与路径的早期版本是 5,921,317 B；这一版的 156 个内容文件与上一版**逐个字节相同**（只有包内那份中文说明按「DOS 改走扩展包」重写过）。官方 v0.9.0 为 1.46 MB。

## 四、验证到什么程度（请不要过度宣称）

- ✅ **16 个公版 Windows 3.11 中文附件**：15 个正常出窗口（9 秒墙钟内 CPU 均 < 1.3 秒）；与"未打补丁的同 `-O1` 构建"和"官方 v0.9.0（MSVC）"逐项 A/B 对比，**无回归**。`REGEDIT.EXE` 三个版本都不出窗口，属既有问题。
- ✅ **MCI waveaudio 驱动链路**：`MPLAYER.EXE CHIMES.WAV` 能通过重定向后的 `WINDOWS\SYSTEM.INI` 打开 16 位驱动；声音控制面板能枚举目录里的 WAV 文件。
- ✅ **MIDI 流输出修复（`midiStreamOut16`）**：编译通过，16 程序 A/B 回归矩阵与基线 IDENTICAL；**无端到端验证**（包内没有调用 `midiStream*` 的 Win16 程序），详见上面 §三.3。
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
