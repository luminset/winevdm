# 本 fork 的改动说明（中文）

> 这是**本 fork 自有**的内容：描述我们在上游 `otya128/winevdm` 之上做了什么，**不会同步回上游**。
> 上游原始说明见 [README.md](README.md)。

## 一、这个 fork 是什么

- **上游基线**：`otya128/winevdm` 的 `master` @ `a31d1c2`（2026-09-22）
- **我们的工作分支**：`otvdm-patched` = 上游基线 **+ 12 个提交**（合流社区 fork 里的 12 个补丁，并修好其中 3 条自身的缺陷）
- **预编译包**：本仓库 **Releases** 页的 `patched-o1-slim-20261014`（MinGW-W64 i686、`-O1` Release、已去掉符号表与构建机路径、平铺布局、解压即用；156 个文件 / 2,415,833 B）
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

- 本 fork 另有一条**未合并**的主题分支 `mingw-o1-guard`：在根 `CMakeLists.txt` 加了守卫 —— 当 MinGW + GCC 且**当前配置**的旗标里含 `-O2`/`-O3`/`-Os` 时，自动降为 `-O1` 并打印一行原因。它按当前 build type 取变量名（`CMAKE_<LANG>_FLAGS_<CONFIG>`），所以 `Release`、`RelWithDebInfo`、`MinSizeRel` 三种配置都**真的**生效；显式给的 `-O2`/`-O3`/`-Os` 同样降级，显式 `-O1` 不动。该分支**不并入 `otvdm-patched`**，留给上游 PR。

### 2. MinGW 版交付包的两个必要调整

- **必须平铺目录**：`otvdm.exe` 静态导入 `krnl386.exe16`，官方包的 `dll/` 子目录布局只对 MSVC 版有效。
- **必须随包带 MinGW 运行时**：`libgcc_s_dw2-1.dll`、`libstdc++-6.dll`、`libwinpthread-1.dll`，否则报 "error while loading shared libraries"。

### 3. 预编译包

- Releases 页的 `patched-o1-slim-20261014`：MinGW-W64 i686 `-O1` Release、平铺布局、带运行时 DLL，解压即用（解压后得到一层 `otvdm-patched-o1-slim\` 目录）。
- **注意**：MinGW 版**每个 16 位程序会出现两个 `otvdm.exe` 进程**，这是正常的（靠 `--fix-compat-mode` 子进程真正运行应用）。**判断是否死转要看 CPU 时间，不要看进程数。**
- 当前包已**去掉 COFF 符号表与内嵌的构建机路径**（链接 `-Wl,-s`，编译 `-ffile-prefix-map` + `-g0`）：2,415,833 B（上一版带符号表与路径的是 5,921,317 B，**−59 %**），二进制里不再有构建目录或工具链的绝对路径。官方 v0.9.0 为 1.46 MB。

## 四、验证到什么程度（请不要过度宣称）

- ✅ **16 个公版 Windows 3.11 中文附件**：15 个正常出窗口（9 秒墙钟内 CPU 均 < 1.3 秒）；与"未打补丁的同 `-O1` 构建"和"官方 v0.9.0（MSVC）"逐项 A/B 对比，**无回归**。`REGEDIT.EXE` 三个版本都不出窗口，属既有问题。
- ✅ **MCI waveaudio 驱动链路**：`MPLAYER.EXE CHIMES.WAV` 能通过重定向后的 `WINDOWS\SYSTEM.INI` 打开 16 位驱动；声音控制面板能枚举目录里的 WAV 文件。
- ✅ **时钟动画（定时器路径）**：三个版本都在动（连续截图像素哈希互不相同）。
- ⚠️ **有 6 个提交只做了源码审查 + 编译 + 无回归验证，没有正向功能验证**：`cd17af3`（需 Morphman 一类复用 `MMCKINFO` 缓冲的程序）、`d8a0b84`（需 Borland 编译的程序）、`a6ce5a3`/`5a6a99f`（需带 `OFN_ENABLEHOOK` 的 16 位应用）、`c3861a8`（需 OLE 客户端）、`043df7c`（需故意传坏指针的程序）、`61bc422`（需直接调 `mmioSendMessage` 的程序）。**在此之前请勿对外宣称这些已修复。**
- ⚠️ MPlayer 按空格播放不出声，在**三个版本上都复现**（含官方 v0.9.0）→ 上游/环境限制，不是本分支造成的。
- ❌ 本仓库**不含**任何 Windows 3.x 附件二进制（微软版权），测试请自备安装介质。

## 五、与上游的关系

- 本分支**未提 PR**，只存在于本 fork；上游只走 PR，绝不直接推 `upstream`。
- `master` 保持为**上游纯镜像**，可随时同步（GitHub 的 "Sync fork"，或本地 `git fetch upstream && git merge --ff-only upstream/master`）。
- **本文件与 `README.md` 顶部的提示属于本 fork 自有内容**，不打算同步回上游。将来若向上游提 PR，请从**干净的 12 提交处**另开分支（tag `patched-o1-20261014` / 提交 `4df7863`），不要带上这些说明提交。

## 六、许可

GPL-2.0，与上游一致。源码 = 本仓库的 `otvdm-patched` 分支，或对应 tag 附带的自动源码包。
