# MCLC — 用 C 写的 Minecraft Java 版启动器

## 项目简介

MCLC（Minecraft C Launcher）是一个面向 Windows 的轻量级 Minecraft Java 版启动器，
使用 C11 从零实现。它不依赖 .NET、第三方启动器框架、libcurl 或 zlib，直接通过
WinHTTP 与 Mojang 官方服务通信，自动下载并校验指定版本的客户端 jar、libraries、
natives 和 assets，再根据版本元数据构造 Java 启动命令。

项目的目标是提供一个体积小、行为透明、便于学习和二次开发的启动器实现：它同时覆盖
旧版 `minecraftArguments` 和新版 `arguments`、版本继承、Windows 平台规则、离线
UUID、SHA-1 校验、增量补全以及 Java 运行时查找。仓库包含 GitHub Actions，推送后
会自动用 MSVC 和 MinGW-w64 各构建一套图形版和命令行版。

> 当前版本仅支持 **Windows** 和**离线登录**。它不会要求或保存 Microsoft 账号密码，
> 也不会自动下载 Java 运行时。

## 功能

- 原生 Win32 图形界面（`mclc_gui.exe`）：版本选择、用户名、内存、Java 多版本
  下拉框、游戏目录、演示模式和实时日志
- “下载游戏”窗口按正式版、预览版、远古版、愚人节版分类选择版本
- 独立下载进度窗口；已有文件按 SHA-1 校验，不一致时重新下载
- GUI 使用后台线程执行下载和启动，界面不会卡死；不创建控制台窗口
- 保留命令行版（`mclc.exe`）用于自动化、CI 和排错
- 从 `%APPDATA%\.minecraft` 读取/安装版本，也支持 `--dir` 指定任意游戏目录
- 从 Mojang 官方版本清单下载版本 JSON
- 依据 `rules` 处理 Windows 平台、系统架构和 feature 规则
- 下载客户端 jar、libraries、natives、logging 配置和资源索引
- 下载资源对象到 `assets/objects/xx/<sha1>`，支持旧版的 `virtual` /
  `map_to_resources` 资源映射
- 下载游戏时按 SHA-1 校验已有文件，不一致就删除并重新下载
- 命令行可用 `--verify` 对已有文件做完整校验
- 解析旧版 `minecraftArguments` 和新版 `arguments.game` / `arguments.jvm`
- 还原版本继承（`inheritsFrom`），支持常见的 Forge / Fabric / NeoForge 版本
- 生成原版离线 UUID（`UUID.nameUUIDFromBytes("OfflinePlayer:<name>")`）
- 自动寻找 `--java`、`MCLC_JAVA`、`JAVA_HOME`、官方启动器 runtime 和常见 JDK
- 纯 C 实现 ZIP/DEFLATE 解压，不需要 zlib、libcurl 或 .NET
- 两个独立的静态链接 exe，除 Windows 系统 DLL 外无运行时依赖

## 构建

需要 Windows 10/11 和以下任一工具链（MSVC 建议 VS 2019 16.7+，以支持 `/std:c11`）：

### 1. MSVC

打开 **x64 Native Tools Command Prompt for VS**，进入项目目录：

```bat
build.bat
```

或手动：

```bat
cl /nologo /std:c11 /O2 /utf-8 /MT /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
  src\main.c src\util.c src\json.c src\hash.c src\http.c src\inflate.c ^
  src\zip.c src\version.c src\install.c src\remote.c src\launch.c ^
  src\progress.c /Fe:mclc.exe /link winhttp.lib shell32.lib

cl /nologo /std:c11 /O2 /utf-8 /MT /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
  src\gui.c src\util.c src\json.c src\hash.c src\http.c src\inflate.c ^
  src\zip.c src\version.c src\install.c src\remote.c src\launch.c ^
  src\progress.c /Fe:mclc_gui.exe /link /SUBSYSTEM:WINDOWS winhttp.lib shell32.lib ^
  comdlg32.lib ole32.lib user32.lib gdi32.lib comctl32.lib
```

### 2. MinGW-w64 / MSYS2

```bash
make
```

或者在 MSYS2 UCRT64 终端：

```bash
gcc -std=gnu11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -DUNICODE -D_UNICODE \
    src/main.c src/util.c src/json.c src/hash.c src/http.c src/inflate.c \
    src/zip.c src/version.c src/install.c src/remote.c src/launch.c \
    src/progress.c -static -static-libgcc -lwinhttp -lshell32 -o mclc.exe

gcc -std=gnu11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -DUNICODE -D_UNICODE \
    src/gui.c src/util.c src/json.c src/hash.c src/http.c src/inflate.c \
    src/zip.c src/version.c src/install.c src/remote.c src/launch.c \
    src/progress.c -mwindows -static -static-libgcc -lwinhttp -lshell32 -lcomdlg32 -lole32 \
    -luser32 -lgdi32 -lcomctl32 -o mclc_gui.exe
```

### 3. CMake

```bat
cmake -S . -B build
cmake --build build --config Release
```

产物在 `build\` 或 `build\Release\` 下：

- `mclc_gui.exe`：图形界面（推荐）
- `mclc.exe`：命令行版（CI/自动化用）

## GitHub Actions 自动编译

本目录就是仓库根目录，已包含 [`.github/workflows/build.yml`](.github/workflows/build.yml)。
把它作为 GitHub 仓库根目录推送即可；不要把 `mclc` 再套一层子目录，否则 Actions
找不到 workflow。

推送到 GitHub 后会自动编译：

- push 到 `main` / `master`
- Pull Request
- 手动触发 `workflow_dispatch`
- 推送 `v*` 标签时，在编译完成后创建 GitHub Release

CI 会并行完成两套构建：

| Job | 工具链 | 产物 |
|---|---|---|
| `MSVC x64` | Visual Studio 2022 + MSVC | `mclc-msvc-x64` artifact |
| `MinGW-w64 x64` | MSYS2 UCRT64 + GCC | `mclc-mingw-x64` artifact |

每个 artifact 都包含 `mclc_gui.exe` 和 `mclc.exe`。在 GitHub 的
**Actions → Build → 对应运行 → Artifacts** 下载。创建 Release 时会附加：

- `mclc-gui-msvc-x64.exe`
- `mclc-cli-msvc-x64.exe`
- `mclc-gui-mingw-x64.exe`
- `mclc-cli-mingw-x64.exe`
- 两个包含 `README.md`、`LICENSE`、GUI 和 CLI 的 zip 包

### 提交到 GitHub

在项目根目录执行（已经初始化过 Git 可跳过 `git init`）：

```bash
git init
git config user.name "Your Name"
git config user.email "you@example.com"
git add .
git commit -m "Initial commit: MCLC Minecraft launcher"
git branch -M main
git remote add origin https://github.com/<你的用户名>/<仓库名>.git
git push -u origin main
```

发布一个版本：

```bash
git tag v1.0.0
git push origin v1.0.0
```

随后 Actions 会编译 MSVC 和 MinGW 两个版本并自动生成 Release。

## 使用

### 图形界面（推荐）

双击：

```text
mclc_gui.exe
```

窗口里可以设置：

- **游戏目录**：默认 `%APPDATA%\.minecraft`，可浏览选择
- **版本**：下拉列表显示本地已下载版本，也可以手动输入已安装的版本号
- **用户名**：离线玩家名
- **内存 MB**：最大 Java 堆内存
- **Java**：下拉列表自动列出本机检测到的 Java，格式为
  `Java 17.0.9 (C:\...\java.exe)`；也可以用“浏览...”手动添加
- **演示模式**
- **下载游戏**：打开版本选择窗口
- **启动游戏**：只启动已下载的版本，不再检查或下载文件
- **打开游戏目录**
- **日志**：显示下载、校验、安装和启动过程，不需要额外控制台

图形界面在后台线程执行下载和安装，因此界面不会卡死。启动 Java 时使用
`CREATE_NO_WINDOW`，不会弹出命令行窗口。

### 下载游戏

点击“下载游戏”后会立即弹出独立窗口，版本清单在后台加载，主界面不会因网络
请求而卡死。

1. 左侧/顶部选择版本类型：
   - **正式版**
   - **预览版**
   - **远古版**（old_beta / old_alpha）
   - **愚人节版**
2. 在列表中选中具体版本
3. 点右下角“下载”
4. 点右上角叉号取消，不会开始下载

确认后会弹出下载进度窗口，显示当前文件/阶段和进度条。下载期间：

- 已存在的文件会按云端 SHA-1 校验
- 大小或 SHA-1 不一致的文件会先删除，再重新下载
- 新下载的文件也会再次校验
- 进度窗口不能关闭，下载完成后自动关闭

下载完成后回到主窗口，选中刚下载的版本后点“启动游戏”即可。启动流程不再做
“检查并下载”，只负责加载版本并启动。

典型流程：

1. 双击 `mclc_gui.exe`
2. 确认游戏目录
3. 点“下载游戏”，选择版本并确认
4. 下载完成后在版本下拉框中选择该版本
5. 点“启动游戏”

### 命令行（可选）

`mclc.exe` 仍然保留，用于自动化、CI 和排错：

```text
mclc                          交互式启动
mclc launch <版本>            启动（自动下载缺失文件）
mclc install <版本>           只安装/校验，不启动
mclc list                     列出本地已安装版本
mclc remote [--snapshots]     列出 Mojang 官方版本
```

版本可以是 `1.21.4`、`1.20.1`、`latest` 或 `latest-snapshot`。

常用选项：

```text
--dir <路径>        游戏目录（默认 %APPDATA%\.minecraft）
--user <名字>       离线用户名（默认 Steve）
--uuid <UUID>       覆盖离线 UUID
--java <路径>       java.exe 或 JAVA_HOME 目录
--memory <MB>       最大堆内存（默认 2048）
--width / --height  窗口尺寸
--demo              演示模式
--verify            校验已有文件的 SHA-1（较慢）
--refresh           强制重新下载版本 JSON
--dry-run           补齐文件后只打印 Java 命令行
--detach            启动后不等待游戏退出
--jvm <参数>        追加 JVM 参数（可重复）
--game-arg <参数>   追加游戏参数（可重复）
--                  其后的参数全部传给游戏
-h, --help          帮助
-V, --version       启动器版本
```

示例：

```bat
mclc.exe install latest
mclc.exe launch 1.21.4 --memory 4096
mclc.exe launch 1.20.1 --user Alex --java "C:\Program Files\Eclipse Adoptium\jdk-17\bin\java.exe"
mclc.exe launch latest --dry-run
mclc.exe launch 1.20.1 --jvm -Dorg.lwjgl.util.Debug=true
```

### 第一次运行

图形界面：双击 `mclc_gui.exe`，点“下载游戏”，选择版本类型和具体版本后确认。
下载完成后在版本下拉框中选择该版本并启动。

命令行：

```bat
mclc.exe launch latest
```

如果本地没有版本 JSON，程序会：

1. 拉取 `version_manifest_v2.json`
2. 下载对应版本 JSON 到 `versions\<id>\<id>.json`
3. 补全 `versions\<id>\<id>.jar`
4. 下载 `libraries\...` 和 `versions\<id>\natives`
5. 下载 `assets\indexes\<assetIndex>.json` 以及所有 `assets\objects`
6. 查找 Java，构造参数并启动

首次安装需要下载数百 MB 的资源，耗时取决于网络。之后的启动只补齐缺失文件。

### 离线登录

MCLC 使用离线模式：

- 图形界面中的“用户名”或命令行 `--user` 决定玩家名
- `--uuid` 可覆盖 UUID；不指定时按 Mojang 的离线 UUID 算法生成
- 启动参数中的 `accessToken` 为 `0`，`userType` 为 `legacy`

因此它适用于单人游戏、离线服务器和局域网。**不能加入开启了正版验证的服务器。**

## Java 要求

启动器会按以下顺序查找 Java：

1. `--java`
2. `MCLC_JAVA`
3. `JAVA_HOME`
4. 官方启动器内置 runtime（`Minecraft Launcher\runtime\...`）
5. `Program Files` 下的常见 JDK 目录
6. `PATH` 中的 `java.exe`

图形界面启动时会扫描这些位置，把检测到的 Java 加入下拉框，格式如：

```text
Java 17.0.9 (C:\Program Files\Eclipse Adoptium\jdk-17.0.9\bin\java.exe)
```

第一项是“自动选择（推荐）”。也可以点“浏览...”手动加入其他 `java.exe`。
如果本机有多个 Java，下拉框会分别列出。

版本 JSON 里的 `javaVersion.majorVersion` 只用于提示和选择目录。MCLC 不会下载 Java
运行时，也不会严格校验 Java 主版本；如果启动时报 `UnsupportedClassVersionError`，
请在 Java 下拉框中选择正确版本，或用 CLI 的 `--java` 指向正确版本，例如：

- Minecraft 1.17–1.20.4：Java 17
- Minecraft 1.20.5+：Java 21
- 旧版本（1.12.2 及更早）：Java 8

MCLC 会按 64 位 Windows 选择 x64 natives，并假定使用 64 位 Java。32 位 Java
需要不同的 native classifier，当前版本不自动处理。

## 目录结构

```text
<game_dir>\
  versions\<id>\
    <id>.json
    <id>.jar
    natives\
  libraries\<group>\<artifact>\<version>\...
  assets\
    indexes\<assetIndex>.json
    objects\<xx>\<sha1>
    virtual\<assetIndex>\...      # 旧版资源
    log_configs\...
  resources\...                  # map_to_resources 旧版资源
```

MCLC 不修改官方启动器的 `launcher_profiles.json`，可以与官方启动器共用同一个
`.minecraft` 目录；但不要让两个启动器同时运行同一个版本。

## 代码结构

| 文件 | 作用 |
|---|---|
| `src/gui.c` | Win32 图形界面、后台任务、日志管道 |
| `src/main.c` | 命令行解析、交互界面、命令编排 |
| `src/util.c` | UTF-8/UTF-16、路径、目录、动态字符串 |
| `src/json.c` | 无依赖 JSON 解析器 |
| `src/hash.c` | SHA-1、MD5、离线 UUID |
| `src/http.c` | WinHTTP 下载、连接复用、进度显示 |
| `src/inflate.c` | RFC 1951 DEFLATE 解码器 |
| `src/zip.c` | ZIP 中央目录、CRC32、natives 解压 |
| `src/version.c` | 版本 JSON、继承、规则、libraries、参数变量 |
| `src/install.c` | 客户端/库/资源/natives 安装与校验 |
| `src/remote.c` | Mojang 版本清单和版本 JSON 下载 |
| `src/launch.c` | 参数拼装、Java 查找、`CreateProcessW` 启动 |
| `src/progress.c` | 下载/校验进度回调 |

启动流程：

```text
GUI 读取目录/版本/用户名/内存
        -> 后台线程
        -> 版本清单 -> 版本 JSON -> Version(layers/libraries)
        -> install_version() 补齐文件
        -> build_arguments() 变量替换、rules、classpath
        -> launch_find_java()
        -> CreateProcessW(java.exe ..., CREATE_NO_WINDOW)
        -> 日志管道 -> 图形界面日志框
```

CLI 和 GUI 共用同一套 `Version` / `install_version` / `launch_version` 核心。

## 已知限制

- 仅支持 Windows；代码使用 WinHTTP、Win32 路径和 `CreateProcessW`
- 仅支持离线登录，没有 Microsoft OAuth/设备码登录
- 不自动下载 Java 运行时
- 图形界面保持轻量，没有新闻、皮肤、账号切换等官方启动器功能
- 不支持 ZIP64；Minecraft 的 natives jar 不会触发该限制
- 不处理超长路径（> 260 字符）的系统级开关；建议把游戏目录放在较短路径下
- 校验已有文件 SHA-1 时资源多会明显变慢
- Forge/NeoForge 等大型整合包的元数据变体很多；若某个版本参数特殊，可用
  `--dry-run` 查看命令行后再调整 `--jvm` / `--game-arg`

## 排错

**`无法加载版本 ...`**

确认 `versions\<id>\<id>.json` 存在；可以先在图形界面点“下载游戏”完成安装，
或执行 `mclc.exe install <id>`。

**`找不到指定的 Java` / `UnsupportedClassVersionError`**

在图形界面的 Java 下拉框选择正确的版本，或点“浏览...”添加 `java.exe`，也可以安装
对应 JDK。命令行可用
`--java`。

**`HTTP 404`**

版本 JSON 中的库可能需要第三方 Maven（常见于整合包）。先用官方启动器安装一次该
版本/整合包，让库文件落到 `libraries`，再用 MCLC 启动。

**下载很慢**

资源是逐个下载的，首次安装可能很久。中断后重新运行会跳过已存在的文件。

**资源校验失败**

删除报错的文件后重试；也可以加 `--verify` 做完整校验。

## 安全说明

- 所有元数据和文件都通过 HTTPS 从 Mojang 官方域名下载
- 下载完成后校验 SHA-1（元数据提供时）
- ZIP 解压拒绝绝对路径、`..` 和 `META-INF`，并校验 CRC32
- MCLC 不上传账号信息，不保存密码
- 离线 UUID 仅用于本地/离线服务器，不具备正版身份认证能力

## License

MIT。见 `LICENSE`。
