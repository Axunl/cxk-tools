# AGENTS.md

本文件面向在此仓库写代码的 AI 代理与协作开发者。内容全部依据当前源码核对得出，
**改了架构、构建方式、约定或平台能力后，请回来同步本文件**。

---

# 第一部分：六条硬性规则

以下六条对**每一次改动**都生效，不是建议。

## 规则 1 — 代码必须有详细注释

- 每个文件顶部说明职责；每个类/结构体用 `///` 写用途与关键约定；每个非平凡函数说明它做了什么、
  参数与失败语义。
- 注释写**why 和约束**，不写"给变量赋值"式的复述。
- 平台分支、魔法数字、绕坑写法必须就地注明原因（例：为什么热键抓 key 要连带 4 种 lock mask）。
- 注释语言用中文（与现有代码一致），源码是 UTF-8；`.bat` 文件内注释必须纯 ASCII（cmd 按 GBK 读会乱码）。
- 新增的 `// TODO` 必须带上条件，不要留裸 TODO；不允许提交被注释掉的旧代码。

## 规则 2 — 必须用测试验证，测试用例事后删除

- 任何代码改动在交付前都要有**实际执行过的验证**，不允许"看起来对"就交付。
- 仓库**不保留测试工程**（当前没有 `tests/`、没有 CTest、没有 GoogleTest）。
  因此测试一律写成**临时物**：跑通、贴出结果、然后删除，不留痕迹。
- 验证方式按改动类型选择（详见第三部分）：纯逻辑写临时断言程序；界面用 `--shot` 出图；
  插件链路看产物与运行日志。
- 交付说明里要写清"跑了什么、结果如何"，不能只说"应该没问题"。

## 规则 3 — 必须跨平台：Windows + macOS + Linux

- 目标平台是 **Windows、macOS、Linux** 三个，缺一不可。任何改动都要在三端"能编译、行为可解释"，
  不能只保证当前开发机（Windows）通过。
- 不允许出现只有某个平台能编译的代码：任何原生调用都必须包在对应宏里，且三段齐全——

  ```cpp
  #if defined(Q_OS_WIN)
      // Win32：RegisterHotKey / HKCU Run
  #elif defined(Q_OS_LINUX)
      // X11：XGrabKey / XDG autostart
  #elif defined(Q_OS_MACOS)
      // macOS：暂无实现时给出明确提示
  #else
      // 其余平台兜底：编译通过、返回"当前平台不支持"
  #endif
  ```

  已按此写好的参照实现：`src/core/HotkeyManager.cpp`（热键）、`src/core/Settings.cpp`（开机自启）。
- 不允许写死平台细节：不写 `.dll`/`.dylib`/`.so`、`.exe` 字面量（插件过滤用 `QLibrary::isLibrary()`），
  不拼 `\` 或 `/` 路径分隔符，不假设大小写不敏感，不假设窗口/托盘/菜单行为一致。所有路径走
  `QDir` / `QStandardPaths`，可执行文件走 `QCoreApplication::applicationFilePath()`。
- **Linux 专属约束**（写 Linux 分支时必须遵守）：
  - 全局热键只能走 X11：`XGrabKey` 抓根窗口，事件从 Qt 的 native event filter 里取 `xcb_generic_event_t`，
    CMake 在 `if(UNIX AND NOT APPLE)` 下 `find_package(X11 REQUIRED)` 并链接 `X11::X11`(+`X11::xcb`)。
  - 抓键要连带 `CapsLock`/`NumLock` 的 4 种 lock mask，否则用户开着大写锁定就失灵。
  - 占用检测靠 `XSetErrorHandler` + `XSync` 捕获 `BadAccess`（X 协议的错误是异步上报的，没有返回值可看）。
  - **Wayland 会话拿不到 `Display*`**，这是环境限制：必须返回"当前不是 X11 会话（Wayland 下不支持全局热键）"
    这类可读文案，不允许静默失败。
  - 开机自启写 XDG：`~/.config/autostart/cxk-tools.desktop`，关闭时删除该文件；`Exec=` 里的路径要加引号，
    否则装在有空格的路由下会启动失败。
  - 新增 Linux 依赖时同步更新 `README.md` 的环境要求表（当前是 `libx11-dev`、`libxcb1-dev`）。
- 构建入口按平台分开：Windows 用 `scripts/build.bat`，macOS / Linux 用 `scripts/build.sh`（支持 `QT_DIR`）。
  改 CMake 时两个脚本都要能跑通，不要只验证其中一个。
- 某能力在某平台确实做不到时（如 macOS 全局热键、Wayland 全局热键）：**返回明确的错误文案**让上层提示用户，
  绝不允许静默失败或假装成功。
- 无 macOS / Linux 环境时，改动仍要保持平台中立并注明"未在 macOS（或 Linux）实机验证"，
  不要凭猜测写 Carbon/AppKit 或 Xlib 代码；平台专属代码宁可留空分支加提示，也不要塞未经验证的实现。

## 规则 4 — 临时文件用完即删

- 构建产物、截图、临时测试工程、日志、脚手架文件，验证结束立刻删除。
  例外只有仓库里已存在的资产（`docs/main-preview.png`）。
- 临时物一律放在 `build/`（已被 `.gitignore` 忽略）或系统临时目录，不要放仓库根、`src/`、`plugins/`。
- 交付前跑一遍 `git status --porcelain`，确认只剩**你打算提交的**文件。

## 规则 5 — 代码文件统一 UTF-8 编码

- 仓库内所有文本文件（`*.cpp` `*.h` `CMakeLists.txt` `*.cmake` `*.qss` `*.qrc` `*.md` `*.sh` 等）
  一律 **UTF-8（无 BOM）+ LF 行尾**。源码里有大量中文字面量（界面文案、日志、错误提示），
  编码不统一会直接变成乱码或编译报错。
- MSVC 侧由 `CMakeLists.txt` 的 `/utf-8`（source 与 execution charset 都用 UTF-8）保证；
  **新增编译器/平台时同样要显式指定 UTF-8**，不要依赖系统默认代码页。
- **唯一例外：`.bat` / `.cmd`**。cmd.exe 按本地代码页（中文 Windows 是 GBK/936）逐字节解析脚本，
  存成 UTF-8 会吞换行、整个脚本崩掉（本项目已踩过两次）。这两个文件**只能纯 ASCII**，
  需要中文文案就放到 `.cpp` 里，脚本里只留英文注释与逻辑。
- 安装器脚本：`installer/windows/cxk-tools.iss` 保持 **UTF-8 无 BOM**（Inno Setup 6 会自动识别
  合法 UTF-8）。如果哪天向导里的中文变乱码，第一件事是把该文件改存成「UTF-8 with BOM」并在
  `.editorconfig` 里为 `*.iss` 记这一条例外，**不要去改文案**。
  `installer/windows/lang/ChineseSimplified.isl` 是第三方翻译文件（Inno Setup 官方
  「unofficial translation」，维护者信息保留在文件头），原样入库，不要翻译或重排它。
- 编辑器由仓库根的 `.editorconfig` 兜底（默认 `charset = utf-8`，`*.bat`/`*.cmd` 锁 `latin1`），
  VS Code / CLion / Qt Creator 打开即生效，不用配个人设置。
- 改完自查（应无输出）：

  ```sh
  # 1) 列出非 UTF-8 的文本文件（只扫文本后缀，别把 png 之类的二进制算进来）
  find src plugins cmake scripts docs -type f \
    \( -name '*.cpp' -o -name '*.h' -o -name '*.txt' -o -name '*.cmake' -o -name '*.qss' \
       -o -name '*.qrc' -o -name '*.md' -o -name '*.sh' -o -name '*.bat' \) \
    | while read f; do iconv -f UTF-8 -t UTF-8 "$f" >/dev/null 2>&1 || echo "非 UTF-8: $f"; done
  # 2) 列出带 BOM 的文件
  grep -rlI $'\xef\xbb\xbf' src plugins cmake scripts docs
  # 3) 列出 .bat/.cmd 里的非 ASCII 行
  LC_ALL=C grep -n '[^ -~]' scripts/*.bat
  ```

## 规则 6 — 任何改动同步更新 README.md

- 凡改动涉及**用户可见的行为或能力**，必须在**同一轮改动里**同步更新 `README.md`，
  不允许"代码先改完、README 以后再说"。交付时 README 描述必须与代码实际行为一致：
  不能只改代码留下过时文档，也不能文档写了而代码没有。
- 触发清单（命中任意一条就要动 README）：
  - 新增 / 删除 / 修改工具 → 改「支持的工具」能力表。
  - 构建、打包方式变化（命令、脚本名、生成器、产物位置）→ 改「环境要求」「构建」「打包」章节。
  - **改发布资产名 / `latest.json` 字段 / feed 地址 / 安装器静默参数** → 必须同时改
    `scripts/gen-release-manifest.py`（`SUFFIX_RULES`）、`src/core/UpdateInfo.h`（`pickAsset`）、
    `src/core/UpdateChecker.h`（`feedUrl`）、`installer/windows/cxk-tools.iss` 与 README 的
    「自动更新」章节。这五处只要有一处漏改，更新链路就断，而且**不会在构建期报错**，
    只会在用户点「检查更新」时表现为「已是最新」或「暂不提供该平台的安装包」。
  - 依赖或环境要求变化（Qt 版本、编译器、`libx11-dev` 等系统包）→ 改环境要求表。
  - 平台支持状态变化（某能力从"待补"变"已实现"，或反过来）→ 同步「平台差异」表与「已知边界」。
  - 命令行参数、配置文件位置或格式、默认快捷键、交互约定变化 → 改对应章节。
  - 对外有意义的目录结构变化（如插件目录、运行时数据路径）→ 改「目录结构」「运行时数据」。
- README 的定位是**给使用者看的**：只写「怎么装、怎么用、能干什么」。实现细节、踩坑、
  内部约定留在本文件，两边不要互相复制成重复维护。
- 同理：插件接口（`src/api/ITool.h`）变化时同步 `docs/PLUGIN.md` 并升 `XTOOLS_API_VERSION`
  （与第七部分检查清单第 5 条一致）。
- 自检方法：把本次 diff 涉及的章节在 README 里搜一遍，凡是 README 还在描述旧行为的，就是没同步完。

---

# 第二部分：项目结构与依赖方向

```
cxk-tools/
├── CMakeLists.txt              顶层：Qt 版本、编译选项、输出布局、add_subdirectory 列表
├── CMakePresets.json           IDE 与命令行共用的 debug / release 配置（含本机 Qt 路径）
├── cmake/
│   ├── XToolsHelpers.cmake     xtools_api 接口库 + xtools_add_plugin() 插件工厂（含 install 规则）
│   ├── DeployQt.cmake          Windows 构建后把 Qt 运行库部署到 bin/（让 IDE 里能直接跑）
│   └── PruneQtRuntime.cmake    三平台共用的运行库裁剪脚本（cmake -P 调用，逐项删并打印字节数）
├── .editorconfig               编码/行尾兜底（规则 5）
├── src/
│   ├── CMakeLists.txt          主程序目标；APP_SOURCES 是显式文件列表
│   ├── api/ITool.h             唯一的 ABI 边界（纯头文件，不编成库）
│   ├── core/                   纯逻辑，不 include 任何 ui/ 头文件
│   │   ├── AppController.cpp   组装层：创建对象、连信号、处理命令行
│   │   ├── Settings.*          配置单例，INI 落盘
│   │   ├── ToolRegistry.*      工具条目容器 + 搜索评分
│   │   ├── LocalAppIndex.*     本机应用索引（开始菜单 / .app / .desktop）+ 启动
│   │   ├── PluginManager.*     扫描 plugins/ 并用 QPluginLoader 加载
│   │   ├── HotkeyManager.*     全局热键（Win32 / X11；macOS 待补）
│   │   ├── SingleInstance.*    QLocalServer 单实例与二次激活
│   │   ├── IconBadge.*         字母徽章图标（插件没给图标时的兜底）
│   │   ├── UpdateInfo.h        纯逻辑：版本号比较、latest.json 解析、资产挑选（可单测）
│   │   ├── UpdateChecker.*     拉 latest.json、比对版本、下载并校验 SHA256
│   │   ├── WindowGeometry.h    窗口几何的恢复/保存 + 屏幕外失效保护
│   │   └── Log.*               日志落盘 + 消息处理器
│   ├── ui/                     只向下依赖 core/api
│   │   ├── MainWindow.*        自绘标题栏 + 页头 + QStackedWidget（首页/设置/工具）
│   │   ├── HomePage.*          首页：搜索框 + 「最近使用 / 全部 / 本机应用」磁贴网格
│   │   ├── ToolWindow.*        单个工具的独立窗口（每工具最多一个）
│   │   ├── SettingsPage.*      热键录制、开机自启、插件加载状态、检查更新
│   │   ├── UpdateDialog.*      更新对话框：版本对比 + release notes + 下载进度
│   │   └── TrayController.*    托盘图标与右键菜单
│   └── app/
│       ├── main.cpp            启动顺序（见文件头注释，改动前先读）
│       ├── app.rc.in           Windows 版本资源 + 图标模板（configure_file 生成 app.rc）
│       └── resources/          style.qss + resources.qrc
├── plugins/
│   ├── jsonfmt/{JsonFmtTool.*, JsonFmtPanel.*, JsonEditor.*, JsonHighlighter.*, JsonText.h}
│   ├── calc/{CalcTool.*, ExprEval.h}
│   ├── ipquery/{IpQueryTool.*, IpQueryLogic.h}
│   ├── memmon/{MemMonTool.*, MemMonPanel.*, MemProbe.*, MemText.h}
│   ├── filelock/{FileLockTool.*, FileLockLogic.h, FileLockProbe.*, FileLockPanel.*}
│   ├── translate/{TranslateTool.*, TranslatePanel.*, TranslateLogic.h}
│   ├── colorpicker/{ColorPickerTool.*, ColorPickerPanel.*, ColorText.h, ColorProbe.h}
│   ├── ocr/{OcrTool.*, OcrPanel.*, OcrEngine.*, OcrCapture.*, OcrText.h}
│   ├── cron/{CronTool.*, CronPanel.*, CronExpr.h}
│   ├── jwt/{JwtTool.*, JwtLogic.h}
│   ├── regex/{RegexTool.*, RegexPanel.*, RegexMatch.h}
│   ├── textkit/{TextKitTool.*, TextKitPanel.*, TextLines.h, TextDiff.h}
│   ├── websocket/{WebSocketTool.*, WebSocketPanel.*, WsClient.*, WsServer.*, WsProtocol.h}
│   └── share/{ShareTool.*, SharePanel.*, ShareStore.*, ShareServer.*, ShareHttp.h, ShareWeb.h}
├── assets/
│   ├── app.ico                 Windows 图标（PNG 条目的多尺寸 ICO，被 src/app/app.rc.in 引用）
│   ├── app.icns                macOS 图标（icp4~ic10，打包时拷进 .app/Contents/Resources）
│   └── icon-512.png            源图，改图标时从它重新导出上面两个
├── installer/
│   ├── windows/cxk-tools.iss   Inno Setup 6 脚本（AppId 固定，UTF-8 无 BOM）
│   └── windows/lang/ChineseSimplified.isl   第三方简体中文向导翻译，原样入库
├── scripts/{build.bat, build.sh, package.bat, package-macos.sh, gen-release-manifest.py}
├── .github/workflows/{ci.yml, release.yml}
└── docs/{PLUGIN.md, main-preview.png}
```

## 分层规则

| 层 | 目录 | 可以依赖 | 禁止 |
| --- | --- | --- | --- |
| 接口 | `src/api` | 仅 Qt 值类型与 QWidget 指针 | 依赖 core / ui |
| 逻辑 | `src/core` | api + Qt Core/Gui/Widgets | `#include "ui/..."` |
| 界面 | `src/ui` | core + api + Qt Widgets | 被 core 反向引用 |
| 入口 | `src/app` | 全部 | 承载业务逻辑 |
| 插件 | `plugins/*` | 只能 `#include "ITool.h"` | 引用宿主内部类 |

- **对象的创建顺序与信号连接只写在 `AppController::start()` 里**，不要在构造函数里互相 new，
  也不要在 ui 里直接访问 `PluginManager` 之外的东西。排查"谁触发了谁"时只看这一个文件。
- 能写成不依赖 `QWidget` 的纯函数就写成纯函数（`ExprEval.h`、`JsonText.h` 就是范例），
  这样规则 2 要求的临时测试才能直接 include 验证，而不是复制粘贴代码去测。

## 交互约定（已定型，改之前先问）

1. 启动、热键、托盘「呼出搜索」、单实例二次激活**统一落在首页网格**（`MainWindow::showHome()`），
   不再恢复「上次使用的工具」（`settings/last_tool` 已随改版删除）。
2. `Esc` 分三层，走 `MainWindow::keyPressEvent` 按键传播实现（只认裸 `Esc`）：
   搜索框有关键词 → `HomePage` 的 eventFilter 先 `clear()` 并吞掉；工具页/设置页 → 回首页；
   首页 → `close()` 收进托盘。子控件先拿到按键、谁用谁吞掉 —— 设置页热键捕获中的 `Esc`
   由 `HotkeyButton::keyPressEvent` 接受，不会传到窗口层误回首页。
   关闭按钮与首页 `Esc` 语义相同：只是 `hide()`，进程不退出；真正退出必须先 `prepareQuit()`，
   否则会被 `closeEvent` 拦下。
3. 全局热键与托盘「呼出搜索」都只做一件事：`MainWindow::focusSearch()`（显示 + 回首页 + 聚焦搜索框并全选）。
4. 首页就是磁贴网格（`HomePage` + `LauncherTile` + qss），不要另起 `QListView` + delegate 实现；
   也不需要独立浮层。磁贴固定 104×100，列数由 `columnsFor()` 按视口宽度算。
   工具与本机应用共用同一种磁贴，靠 `LauncherItem::kind` 区分点击行为与右键菜单。
5. 工具面板**不缓存**：每次进入工具都 `createPanel()` 新建面板；离开视图（回首页、进设置、
   收起窗口）即销毁面板（`destroyToolPanel()`），输入值随之清空。同一工具可以同时存在
   「内嵌面板」与「独立窗口面板」两份，插件不得假设自己是唯一实例。
6. 使用频次与「最近使用」只在用户主动打开工具/应用时记账（`markUsed()` / `launchLocalApp()`），
   启动与唤出不算。工具与应用共用同一份最近使用（上限 8），按时间戳混排。
7. 独立窗口（`ToolWindow`）每个工具最多一个，重复打开只 `raise + activateWindow`；
   主窗口收起时独立窗口继续存活；退出时由 `prepareQuit()` 统一关闭 —— **不能放在
   `closeEvent` 里，`qApp->quit()` 不会给窗口发 closeEvent**。
8. 本机应用只从系统入口索引（`LocalAppIndex`），**不做全盘扫描**；启动统一交给系统
   （Windows/macOS `QDesktopServices`，Linux `gio launch` 再退回 `Exec`）。
   启动成功后主窗口 `close()` 收进托盘（启动器语义，别改成留在原地）。

---

# 第三部分：构建与验证

## 构建

```bat
scripts\build.bat            :: Release
scripts\build.bat Debug
```

```sh
scripts/build.sh             # macOS / Linux，Release
scripts/build.sh Debug
QT_DIR=~/Qt/6.8.3/macos scripts/build.sh   # Qt 不在默认搜索路径时
```

- `scripts/build.bat` 顶部**写死了本机工具链路径**（`VSDIR` / `QTDIR` / `CMAKE` / `NINJA`），
  换机器要改这四行；`build.sh` 用 `QT_DIR` 或 `CMAKE_PREFIX_PATH`，不写死。
- 成功标志：脚本末行打印 `BUILD_OK`（失败是 `BUILD_FAILED`）。构建应**零警告**，有警告先修掉。
- 纯命令行重建（不经脚本）：

  ```sh
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<Qt 路径>
  cmake --build build --parallel
  ```

- **IDE（CLion）走 `CMakePresets.json`**：`cmake --preset debug` / `cmake --build --preset debug`，
  产物在 `build/clion-debug/bin`，与 `build/bin` 互不干扰。presets 里写的是本机 Qt 路径
  （`C:/Qt/6.8.3/msvc2022_64`），换机器/换平台要改；macOS 与 Linux 仍用 `scripts/build.sh`。
  IDE 里点运行前先确认 Toolchain 是 **Visual Studio**（Qt 是 MSVC 版，用 MinGW 会在链接期炸）。
  IDE 的「运行」只构建运行配置选中的目标，但 `cxk-tools` 已在根 `CMakeLists.txt` 收尾处
  `add_dependencies` 全部插件（清单由 `xtools_add_plugin` 写入全局属性 `XTOOLS_PLUGIN_TARGETS`），
  所以点运行会连带构建插件到 `bin/plugins/`，**不需要手动 Build All**。

## 产物布局

```
build/bin/cxk-tools(.exe)          主程序（build.bat / build.sh 产出）
build/bin/plugins/<id>.dll         插件（Windows）
build/bin/plugins/<id>.dylib       插件（macOS）
build/bin/plugins/<id>.so          插件（Linux）
build/clion-debug/bin/…            CMakePresets 的 Debug 产物（IDE 用，互不覆盖）
```

插件目录固定为「可执行文件同级 `/plugins`」，三平台一致（macOS 上是 `xxx.app/Contents/MacOS/plugins`）。

Windows 上构建结束会自动跑 `cmake/DeployQt.cmake`，把 Qt 运行库（`Qt6*.dll`、`platforms/` 等）
放到 `bin/` 里，所以 **`bin/` 下的 exe 可以直接双击/在 IDE 里启动**，不需要手工改 `PATH`。
脚本按「构建配置」写 stamp（`.qt-deploy-<Config>`），只有首次构建才真正调用 `windeployqt`；
删掉整个 `bin/` 或切换 Debug/Release 后会重新部署。想跳过可加 `-DXTOOLS_DEPLOY_QT=OFF`。

## 打包与发布

### 版本号只有一个来源

- `CMakeLists.txt` 顶部的 `project(cxk-tools VERSION x.y.z)` 是**唯一**版本定义。configure 时它会被写进
  `build/xtools-version.txt`，`package.bat` / CI 都读这个文件 —— **不要去 grep 源码或另外维护一份**。
- 该值经 `APP_VERSION` 宏进 `QApplication::applicationVersion()`，同时进 `app.rc` 的 `FILEVERSION`、
  安装器的 `AppVersion`（`ISCC /DAppVersion=`）、`latest.json` 的 `version`。四处不一致用户会看到
  「关于里是 0.1.0、安装器是 0.0.9」这类问题。
- 发版 tag 必须是 `v<同一个版本>`：`release.yml` 的 `prepare` 任务会把 tag 去掉 `v` 后与
  `CMakeLists.txt` 比对，**不一致直接 fail**，不允许带着错版本号发出去。

### 三平台入口

```bat
scripts\package.bat            :: Windows：构建 -> dist\windows-x64\ -> zip -> 安装器
```

```sh
scripts/package-macos.sh       # macOS：构建 -> cxk-tools.app -> strip/codesign -> dmg
                               # Linux 由 CI 直接 tar 打包（cmake --install 出来的目录 + plugins/）
```

三平台走同一条骨架：`cmake --install --prefix <stage>` → 部署 Qt 运行库 → 裁剪 → 压缩。
**`cmake --install` 是唯一的 staging 出口**，安装规则写在 `install(TARGETS ...)` 与
`xtools_add_plugin()` 里（新增插件自动带上，不需要改打包脚本）。所以新增源文件/资源时若忘了加
install 规则，症状是「本机能跑、装完找不到插件」。

### macOS：部署目标必须显式设

- 根 `CMakeLists.txt` 在 `project()` **之前**设 `CMAKE_OSX_DEPLOYMENT_TARGET=12.0`，与
  `installer/macos/Info.plist.in` 的 `LSMinimumSystemVersion` 对齐。不设的话 Apple clang 按
  「构建机所在系统」当部署目标：在 `macos-15` runner 上产出的包要求 macOS 15+，用户在
  macOS 12/13 上双击「毫无反应」，而 Info.plist 还写着 12.0 —— 声明与实际不符最难查。
  变量必须在 `project()` 前设，之后设只对后续 target 生效（本项目等于没生效）；
  且条件要用 `CMAKE_HOST_APPLE` 而不是 `APPLE` —— `APPLE` 派生自 `CMAKE_SYSTEM_NAME`，
  在 `project()` 之前可能还没定义，写成 `if(APPLE)` 会静默跳过。
- `scripts/package-macos.sh` 会打印 `MINOS=` 与 `PLIST_MIN=`，不一致就告警（不中断）。
  看到这条 warn 先查上面两处是否被改歪，别去怀疑 `codesign`。

### 产物命名契约（更新器依赖它）

```
cxk-tools-<ver>-windows-x64.zip          便携版（解压即用）
cxk-tools-<ver>-windows-x64-setup.exe    Windows 安装器
cxk-tools-<ver>-macos-arm64.dmg          macOS 安装包
cxk-tools-<ver>-macos-x64.dmg
cxk-tools-<ver>-linux-x64.tar.gz
latest.json / SHA256SUMS.txt             清单（不作为下载资产）
```

- 名字由 `scripts/gen-release-manifest.py` 的 `SUFFIX_RULES` 反查 `(platform, kind)`，
  再由 `src/core/UpdateInfo.h` 的 `pickAsset()` 按平台选资产。**改名必须同时改这三处**，
  否则更新器会「找到 release 但没有一个能用的资产」（表现为 `parseLatestJson` 失败或
  `pickAsset` 返回空，用户看到"暂不提供该平台的安装包"）。
- `kind` 只有四种：`installer` / `portable` / `dmg` / `tarball`。Windows 优先挑 `installer`
  （能静默升级 + 覆盖安装），没有就退回 `portable`（只下载、交给用户手动解压）。

### `latest.json` 契约

Feed 地址写死在 `UpdateChecker::feedUrl()`：

```
https://github.com/Axunl/cxk-tools/releases/latest/download/latest.json
```

`releases/latest/download/...` 会自动指向最新一个**非预发布**的 release，所以 `latest.json`
只作为资产挂在 release 上，不需要另建 gh-pages 分支。环境变量 `XTOOLS_UPDATE_FEED` 可覆盖
（本地假清单测通链路就靠它，别为测试改死地址）。

```json
{
  "version": "0.2.0",
  "tag": "v0.2.0",
  "notes": "## 变更\n- …",
  "pageUrl": "https://github.com/Axunl/cxk-tools/releases/tag/v0.2.0",
  "assets": [
    { "platform": "windows-x64", "kind": "installer",
      "name": "cxk-tools-0.2.0-windows-x64-setup.exe",
      "url": "https://github.com/.../cxk-tools-0.2.0-windows-x64-setup.exe",
      "sha256": "…", "size": 10485760 }
  ]
}
```

- `notes` 是 Markdown，对话框直接 `QTextBrowser::setMarkdown()` 渲染；CI 用 `git log` 生成，
  所以提交标题就是给用户看的变更说明，别写「wip」「fix typo」。
- `version` 缺省或无任何有效 asset 时 `parseLatestJson()` 判失败并清空输出，界面报「检查失败」而不是
  「已是最新」—— 把解析失败当成功是最容易蒙过测试的坑。
- 版本比较只按段做数值比较、缺段补 0，**预发布后缀（`-beta.1`）不参与比较**。目的只是判断"有没有新版"，
  不做完整的 semver 语义。

### 安装器（Inno Setup 6）

- 编译要传版本：`ISCC.exe /DAppVersion=0.2.0 installer\windows\cxk-tools.iss`（忘了传，脚本里
  `#ifndef AppVersion` 会直接 `#error`，不会静默产出 0.0.0 的安装包）。
- **ISCC 的定位顺序**（`package.bat` 的 `:find_iscc` 子程序）：环境变量 `ISCC` → `PATH` →
  `%LOCALAPPDATA%\Programs\Inno Setup 6` → `%ProgramFiles(x86)%\Inno Setup 6` →
  `%ProgramFiles%\Inno Setup 6`。CI 里 Inno 装到 `%RUNNER_TEMP%\InnoSetup6` 并把绝对路径写进
  `ISCC`，所以 CI 不依赖任何猜测。
- **找不到 ISCC 时的行为由 `XTOOLS_REQUIRE_INSTALLER` 决定**：CI 设成 `1`（缺了就
  `PACKAGE_FAILED`，绝不允许发出一个没有 setup.exe 的 release）；本地不设，只
  `[warn] ... portable zip only` 继续出 zip。
- **定位与调用必须写在标签子程序 / 顶层的 `goto` 流程里**，不能塞进 `if (...)` 块：
  ① `%VAR%` 在整块解析时就展开，块内 `set` 的值当轮读不到，命令会变成 `""` 去执行；
  ② `%ProgramFiles(x86)%` 里的 `)` 会提前闭合块，剩下的文本被当命令跑（实测报
  `'C:\Program' is not recognized`）。这两条是第一次 CI 发版失败的真正原因。
- `AppId` 是**固定 GUID**，覆盖升级靠它；换掉会让新版本装成第二个程序、控制面板里出现两条。
- 压缩 `lzma2/ultra64` + `SolidCompression=yes` + `LZMAUseSeparateProcess=yes`（后者防止 Inno 的
  压缩线程在 CI 上吃满内存失败）。实测 28 MB 的 staging 压出 **10.1 MB** 的 setup.exe。
- **不做** `[UninstallDelete]`：用户配置（`cxk-tools.ini`、`logs/`）不随卸载删除，重装后设置还在。
- 应用内更新调用安装器的参数是固定的：

  ```
  /SILENT /CLOSEAPPLICATIONS /NORESTART
  ```

  `[Run]` 里刻意**不加** `skipifsilent`，这样静默升级结束后新版本会自动被拉起。
  `CLOSEAPPLICATIONS` 靠 Restart Manager 关掉正在运行的旧实例，所以新进程要等安装器退出后再起，
  不能反过来抢文件锁。
- **安装范围要和升级参数对上**：`.iss` 里 `PrivilegesRequired=admin`，但向导允许改成「仅为我安装」。
  按用户装（exe 在 `%LOCALAPPDATA%` 下）时，`launchInstaller()` 会补 `/CURRENTUSER`，
  否则静默升级会去提权，用户拒一次 UAC 就变成「安装包启动了、版本没变」。
  这个判断是 `update::isPerUserInstallDir()`（纯函数，比较时带路径分隔符，
  否则 `...\AppData\LocalBackup\x` 会被误判成在 `Local` 下）。

### 裁剪清单与取舍

`cmake/PruneQtRuntime.cmake` 逐项删并打印字节数，每项都有「为什么能删」的注释。
**注意分工**：体积最大那几项是先用 `windeployqt` 的参数**根本不让它拷进来**，prune 只是兜底
（某些版本的 windeployqt 不认参数还是会拷）。所以正常一轮 prune 只删到 5 项、约 1.39 MB ——
如果哪天 prune 打印出删了 15 MB，说明 windeployqt 的开关失效了，去看那行命令。

| 项 | 典型体积 | 谁负责挡掉 | 为什么能删 |
| --- | --- | --- | --- |
| `dxcompiler.dll` + `dxil.dll` | ≈15.8 MB | `--no-system-dxc-compiler` + prune 兜底 | RHI 的 D3D 着色器编译器，纯 Widgets 光栅渲染用不到 |
| `opengl32sw.dll` | ≈20 MB | `--no-opengl-sw` + prune 兜底 | 软件 OpenGL 兜底实现，有显卡驱动就走不到它 |
| `vc_redist.x64.exe` | ≈25 MB | `--no-compiler-runtime` + prune 兜底 | 改由 app-local CRT DLL 满足，不必再跑一遍安装程序 |
| `d3dcompiler_47.dll` | ≈4 MB | prune | D3D/ANGLE 才要 |
| `Qt6Svg.dll` + `imageformats/qsvg.dll` + `iconengines/` | ≈1 MB | prune | 仓库无 `.svg`，图标是 `IconBadge` 运行期画的 |
| `tls/qopensslbackend.dll` `tls/qcertonlybackend.dll` | ≈1.4 MB | prune | Windows 用系统 SChannel 后端，实测 HTTPS 正常 |
| `translations/` `qml/` `generic/` | ≈2 MB | `--no-translations` + prune 兜底 | 界面文案自备；无 QML；`generic` 是 TUIO 触摸协议 |
| `vccorlib140.dll` `concrt140.dll` | 小 | prune | C++/CX 与并发运行时才用，拷 CRT 时顺带进来的 |

- **VC 运行库用 app-local 而不是 `vc_redist`**：`windeployqt` 加 `--no-compiler-runtime`，
  再从 `VCToolsRedistDir`（找不到就用 PowerShell 在 VS 目录里搜最新的 `Microsoft.VC*.CRT`）
  拷 `msvcp140.dll` / `vcruntime140.dll` / `vcruntime140_1.dll`。省掉 25 MB 的 redist 安装器，
  代价是这三个 DLL 跟着程序走（正常，绝大多数 Qt 应用都这么发）。
- 裁剪脚本是**逐项删、逐项打印**，不用 `file(GLOB)` 遍历删除：漏删一项只是多几 MB，
  误删一个必需 DLL 会让程序静默退出，必须能一眼看出删了什么。
- 曾经把 `tls/qopensslbackend.dll` 删掉后担心 HTTPS 失效，实测 `--check-update` 拉到真实地址
  返回 `HTTP 404`（证明请求发出去了、只是路径不存在），所以 SChannel 后端在起作用。
  **再动 TLS 相关文件必须重跑这条验证**。
- 体积基线（0.1.0，Windows x64 Release）：构建产物 `build/bin` 约 43 MB → staging **29.36 MB**
  → zip **12.66 MB** → setup.exe **10.6 MB**。如果哪天 zip 突然涨了 10 MB 以上，先怀疑
  `windeployqt` 又把 dxcompiler / redist / opengl32sw 塞回来了（看 prune 的删除条数就知道）。
- `package.bat` 每轮先删掉 `dist` 里同名的 zip 与 setup.exe 再产出：否则本轮跳过安装器时，
  dist 里那个**上一轮的 setup.exe** 会让「跳过」看起来像「成功」，体积报告也会读到过期数字。

### CI 与发版

| 文件 | 触发 | 做什么 |
| --- | --- | --- |
| `.github/workflows/ci.yml` | push / PR | 三平台只编译（不打包），确认没有平台专属编译错误 |
| `.github/workflows/release.yml` | 推 `v*` tag，或手动 `workflow_dispatch` | 校验版本 → 三平台打包 → 生成清单 → `gh release create` |

- `ci.yml` 的 Linux 任务要装 `libx11-dev libxcb1-dev libgl1-mesa-dev`（热键走 X11），
  Windows 任务用 `ilammy/msvc-dev-cmd` 进 MSVC 环境，macOS 用 `macos-15`（arm64 runner）。
- **macOS runner 标签会随镜像下线而失效**：`macos-14` 已于 2026-11-02 从 GitHub Actions 全面退役，
  标签还在但分配不到机器，报 `The configuration 'macos-14-arm64-us-default' is not supported`，
  job 一步都不跑就 cancelled（annotations 里只有这一行，日志是空的）。
  升级前先看 `actions/runner-images` 的 Available Images 表：目前 arm64 是
  `macos-15` / `macos-26`（`macos-latest` 现在等于 macOS 26），x64 要写 `macos-15-intel` /
  `macos-26-intel`。**别用 `-latest`**：Qt 6.8.3 配新 SDK 没有验证过，会莫名其妙地编不过或打不开。
- `release.yml` 里 Inno Setup 直接下官方 release 的 6.7.4，装到 `%RUNNER_TEMP%\InnoSetup6`
  （路径确定），随后把 `ISCC=<绝对路径>` 写进 `GITHUB_ENV`；找不到 `ISCC.exe` 就让这一步失败。
  **不要**改回「顺手装个最新版」：7.x 的消息表与 `lang\ChineseSimplified.isl` 不兼容。
- 打包步骤带 `XTOOLS_REQUIRE_INSTALLER=1`，所以 CI 里不会出现「悄悄只发了个 zip」的 release。
- release 已存在时用 `gh release upload --clobber` + `gh release edit` 覆盖，允许「同一个 tag 重跑」，
  不需要先删 release。
- 清单脚本只依赖 Python 标准库，本地也能跑：

  ```sh
  python scripts/gen-release-manifest.py --version 0.1.0 --tag v0.1.0 --dir dist
  ```

  `--dir` 是产物所在目录（按 `SUFFIX_RULES` 识别），`--out` 省略时与 `--dir` 相同，
  `--notes` 可传一个 markdown 文件作为 `latest.json` 的 `notes`（CI 用 `git log` 生成）。

### 更新链路（改打包前先看这里）

`AppController::checkUpdate()` 是三处触发（设置页按钮 / 托盘菜单 / 启动静默检查）的唯一收敛点，
启动检查还会尊重 `autoCheckUpdate()` 与 24 小时节流。下载走临时目录，边下边算 SHA256，
结束再比对 `size`，不符就删文件报错 —— **不要改成「下完再说」**，半个安装包被静默放行是最坏的结果。

- `--check-update` 是诊断入口：打印结果后立刻退出（`QTimer::singleShot(0, ...)` + `QCoreApplication::exit`，
  在 `app.exec()` 之前 `quit()` 会被 Qt 忽略）。它和 `--shot` 一样**跳过单实例锁**，否则用户开着程序时
  脚本永远拿不到结果。
- 静默检查只用托盘气泡提示，不弹窗抢焦点；气泡被点击才打开 `UpdateDialog`。
- 装完的包才有升级能力（`isInstalledBuild()` 判断同目录有无 `unins000.exe`）；便携版退化成
  「打开发布页」让用户自己换。

## 界面验证（改 ui/ 或 qss 必做）

```sh
build/bin/cxk-tools --shot build/shot-home.png                    # 首页网格
build/bin/cxk-tools --shot build/shot-tool.png --tool jsonfmt     # 工具页（页头 + 面板）
build/bin/cxk-tools --shot build/shot-settings.png --settings     # 设置页
build/bin/cxk-tools --shot build/shot-popout.png --popout calc    # 独立窗口
build/bin/cxk-tools --shot build/shot-search.png --search chrome  # 搜索（含「本机应用」分组）
```

`--shot` 会等 500ms 让布局与样式生效，把窗口 `grab()` 成 PNG 后自动退出，是**无头环境下唯一的界面回归手段**。
**切页参数必须排在 `--shot` 的判断之后执行**：`showMainWindow()` 会落到首页，顺序颠倒会永远截到首页。
用例注意：

- 运行库：Windows 上构建已自动把 Qt DLL 部署到 `bin/`，直接运行即可；若之前把
  `bin/` 里的 Qt DLL 删了（或用 `-DXTOOLS_DEPLOY_QT=OFF` 构建），先重新配置构建一次，
  或临时 `PATH=/c/Qt/6.8.3/msvc2022_64/bin:$PATH`，否则进程静默退出、图片不生成。
- **先确认没有残留实例**：单实例互斥会让第二个进程立刻退出，截图/日志全是旧状态。
  `Get-Process cxk-tools` 查、`Stop-Process -Id <pid> -Force` 杀。
- 出图后**用看图工具真的看一遍**，检查文字截断、控件重叠、图标裁切、大片空白。
- 截图是临时物，看完删除（规则 4）；仓库只保留 `docs/main-preview.png`。

## 临时测试（规则 2 的落地方式）

仓库没有测试工程，所以测试一律在 `build/_verify/` 里现搭现拆：

```cmake
# build/_verify/CMakeLists.txt
cmake_minimum_required(VERSION 3.21)
project(verify LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# QKeySequence 在 QtGui 里，只找 Core 会编译失败
find_package(Qt6 REQUIRED COMPONENTS Core Gui)

add_executable(verify main.cpp)
target_link_libraries(verify PRIVATE Qt6::Core Qt6::Gui)

# 要测仓库里的 .cpp 时：把文件与头文件目录显式加上
# target_sources(verify PRIVATE ${REPO}/src/core/ToolRegistry.cpp ${REPO}/src/core/Settings.cpp)
# target_include_directories(verify PRIVATE ${REPO}/src/core ${REPO}/src/api)
```

Windows 下**不要自己重写 vcvars 调用**——把 `scripts/build.bat` 里 `call vcvars64.bat` 到 `:sdk_ok`
那一段（含 Windows SDK 路径兜底）整段复制到临时 `run.bat` 里再用：

```bat
rem run.bat：环境准备照抄 build.bat，然后
cmake -S build\_verify -B build\_verify\out -G Ninja -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --build build\_verify\out
build\_verify\out\verify.exe
```

三个必须记住的坑（都实际踩过）：

1. 直接 `call vcvars64.bat` 在受限环境里探不到 Windows SDK（`reg.exe` 被策略拦），`cl` 报找不到头文件
   —— 必须带 `build.bat` 里的 SDK 兜底段。
2. 只 `find_package(Qt6 COMPONENTS Core)` 会因为 `QKeySequence` 缺 `Qt6::Gui` 编译失败。
3. 测试程序缺 Qt DLL 时**不报错、直接退出且无输出**，运行前把 Qt 的 `bin` 加进 `PATH`。

约定：不把带 `Q_OBJECT` 的类拉进临时工程（要么开 AUTOMOC，要么改用 `--shot` 看界面）；
涉及 `Settings::instance()` 的用例会读写真实配置目录，测完把改过的键还原；**跑完 `rm -rf build/_verify`**。

---

# 第四部分：增加一个新工具

## 四步

**1. 新建目录并实现接口**（`plugins/mytool/MyTool.h`）：

```cpp
#pragma once
#include "ITool.h"
#include <QObject>

class MyTool : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)   // 三行缺一不可，少一行宿主 qobject_cast 失败
    Q_INTERFACES(xtools::ITool)

public:
    int apiVersion() const override { return XTOOLS_API_VERSION; }  // 显式对齐宿主版本
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};
```

**2. 写 `plugins/mytool/CMakeLists.txt`**，一行：

```cmake
xtools_add_plugin(mytool SOURCES MyTool.cpp MyTool.h)
```

**3. 根 `CMakeLists.txt` 末尾加一行**：

```cmake
add_subdirectory(plugins/mytool)
```

**4. 重新构建**，`build/bin/plugins/` 下出现 `<id>.dll/.dylib/.so` 即成功；重启应用即可在首页网格里看到。
若把带 `Q_OBJECT` 的面板类写在 `.cpp` 里，**`.cpp` 末尾要 `#include "MyTool.moc"`**（见 `CalcTool.cpp`）。

## 契约清单（违反会崩或内存泄漏）

- `id` 是全应用唯一键：最近使用、使用频次都用它。重复 id 会被 `ToolRegistry` 丢弃并告警。
  留空时宿主回退成文件名。
- `keywords` 直接决定搜索命中率，把中英文与常见缩写都写进去（如 `{"json","格式化","format","jf"}`）。
- `iconPath` 是**相对插件目录**的路径，文件不存在会退回字母徽章并在日志里告警。
- `createPanel()` **每次进入工具都会被调用**（宿主不缓存面板，输入值不留存），每次都要返回**全新**的 `QWidget`；面板**不要设 parent**，
  宿主会 `setParent` 塞进 `QStackedWidget` 或独立窗口；插件不要自己 `delete`。
- **插件不得假设自己的面板是唯一实例**：同一工具可以同时有「主窗口内嵌」与「独立窗口」两份
  （用户点了页头「独立窗口」）。状态放面板实例里，别用静态/全局变量。
- 异常情况返回 `nullptr` 即可（宿主会显示占位页），绝不能让宿主崩。
- **改 `ITool.h` 的虚函数或 `ToolInfo` 字段必须把 `XTOOLS_API_VERSION` +1**，
  否则旧插件会以错误的虚表布局被加载，崩溃点在 `info()` 第一行，堆栈毫无意义。
- 加载失败不会崩主程序：原因写进 `logs/app.log`，同时显示在「设置 → 工具与插件」。
  **不支持热重载**：插件是进程内模块，换 `.dll` 必须重启（托盘菜单「重新扫描插件（重启）」）。

完整走查与典型写法见 [docs/PLUGIN.md](docs/PLUGIN.md)。

---

# 第五部分：实现细节速查

改这些地方前先看表，别凭印象改。

## 搜索评分（`ToolRegistry::search`）

| 命中方式 | 分值 |
| --- | --- |
| 名称完全相等 | 110 |
| 名称前缀 | 100 |
| 关键词完全相等 | 95 |
| 名称包含 | 85 |
| 关键词前缀 | 75 |
| 关键词包含 | 65 |
| id 包含 | 60 |
| 首字母缩写命中（长度 >1 的查询才启用） | 40 |
| 描述包含 | 30 |

- 命中后再加 `min(usage, 20)` 频次加成，**封顶 20，压不过一级匹配**。
- 空查询按 `1000 - min(usage, 500)` 排序（= 常用工具靠前），上限防止某个工具永远霸榜。
- 同分按名称 `localeAwareCompare` 排序，保证结果稳定（否则会随插件加载顺序抖动）。

## 本机应用搜索（`LocalAppIndex::search`）

| 命中方式 | 分值 |
| --- | --- |
| 名称完全相等 | 100 |
| 名称前缀 | 90 |
| 逐词前缀（`studio` → Visual Studio Code） | 80 |
| 名称包含 | 70 |
| 首字母缩写（`vsc`） | 65 |
| 可执行文件名包含 | 50 |

- 同样加 `min(usage, 20)`；**空查询直接返回空** —— 应用不铺满首页，只出现在搜索结果
  与「最近使用」里，否则上百个开始菜单项会把网格淹掉。
- 名称用 `toCaseFolded()` 比较，中文原样参与，不做拼音（与工具搜索一致）。
- 索引在启动时扫一次（`AppController::start`），运行期不热更新。

## 配置（`Settings`，INI 明文）

| 键 | 含义 | 说明 |
| --- | --- | --- |
| `settings/hotkey` | 全局热键 | 存 `PortableText`（`Ctrl+Alt+Space`），跨平台可读 |
| `settings/start_with_system` | 开机自启 | 真正落系统项见下 |
| `recent/<id>` | 最近使用时间戳 | UTC ISO8601（**带毫秒**，`ISODateWithMs`），最多 8 条，只在用户主动打开时写；`id` 是工具 id 或 `app:<hash>` |
| `usage/<id>` | 使用频次 | 参与搜索加成，工具与应用共用同一套键 |
| `ui/main_geometry` | 主窗口位置/大小 | `QWidget::saveGeometry()` 的字节流 |
| `ui/tool/<toolId>/geometry` | 独立窗口位置/大小 | 同上；`name` 里的 `/` 归一化成 `_` |

- 「最近使用」不能用秒级精度：同一秒内连开两个工具时，「最新」会退化成按 id 排序；
  排序也必须用 `toMSecsSinceEpoch()`，`toSecsSinceEpoch()` 会把毫秒抹掉。
- 本机应用的 id 存 **路径的 FNV-1a 哈希**（`app:<16 hex>`），不存路径本身：
  INI 的键里带 `/` 会被当成组层级、带盘符冒号也不好读；哈希还让「同一入口换大小写写法」
  在 Windows 上得到同一个 id。**不要改用 `qHash`**（Qt 可能加随机种子，重启后 id 全变）。
- 几何恢复要校验窗口是否还在某块屏幕内（`WindowGeometry.h`），否则换显示器后会「窗口消失」。
- 已废弃的 `settings/last_tool`（旧版恢复上次工具用）不再读取，老 ini 里残留的键无害。

- 组名是 **`settings`，不是 `general`**：`[General]` 是 INI 保留段名，Qt 会转义成 `[%General]`，
  手工编辑时极其费解。
- 必须 `QSettings(filePath, IniFormat)` 构造 —— 默认构造在 Windows 上走注册表，`setPath()` 对它无效。
- 必须 `setFallbacksEnabled(false)`，否则没写的键会去注册表找同名值，出现「配置里没有却读出值」的灵异现象。

## 全局热键

- 默认 `Ctrl+Alt+Space`（`Settings::defaultHotkey()`）。`Alt+Space` 是**能用的**：实测 `RegisterHotKey`
  注册成功、`WM_HOTKEY` 正常派发、能唤出窗口。它不做默认值只是因为会抢掉所有窗口的系统菜单
  （用户主动改过去是他的选择，不要去拦）。
- **`Alt+Space` 在 Windows 上有个 Qt 层的坑，录制和注册是两个独立环节**：
  系统把它当窗口菜单处理，Qt 收到 `WM_SYSKEYDOWN(VK_SPACE)` 后直接转成 `SC_KEYMENU`，
  **根本不生成 `QKeyEvent`**，于是 `HotkeyButton::keyPressEvent` 永远等不到那一次按键 ——
  表现为「点了按钮按 Alt+Space 毫无反应」，系统菜单弹出还可能抢走焦点触发 `focusOutEvent`，
  把值恢复成旧的。`HotkeyButton::nativeEvent` 专门补了一条原生消息通道：只在捕获态且
  `VK_SPACE` 时介入，自己用 `GetKeyState` 读修饰键，并吞掉消息避免真的弹菜单。
  两条入口共用 `acceptSequence()`，别在 `keyPressEvent` 里再复制一份逻辑。
- Windows：`RegisterHotKey` 注册到 message-only 窗口（`HWND_MESSAGE`），`WndProc` 收 `WM_HOTKEY`。
- Linux：X11 `XGrabKey` 抓根窗口，要连带 `CapsLock`/`NumLock` 的 4 种 lock mask；
  占用检测靠 `XSetErrorHandler` + `XSync` 捕 `BadAccess`；Wayland 拿不到 `Display*`，直接返回可读失败文案。
  `Display*` 从 `qGuiApp->nativeInterface<QNativeInterface::QX11Application>()` 取
  （需含 `<QtGui/qguiapplication_platform.h>`）。**这是成员函数**，写成 `QGuiApplication::nativeInterface<T>()`
  在 GCC 上直接编译失败（MSVC/clang 不报），见第六部分踩坑表。
- macOS：未实现，`registerSequence()` 返回明确原因，**不允许静默失败**。
- 失败不阻止启动：原因回显到设置页 + 托盘气泡，用户在设置里换一个组合键即可。

## 单实例

- socket 名 `cxk-tools-single-instance`；`tryLock()` 先探测 300ms，连上就把已运行实例唤出并退出。
- 没有实例时先 `QLocalServer::removeServer()` 清理崩溃残留（Unix 下 `kill -9` 会留下 socket 文件）。
- `listen` 失败时**退化为允许多开**并告警，不静默失败。

## 窗口

- 主窗口：无边框 + 自绘标题栏；最小 `920×580`，默认 `1120×720`（首次启动；之后从
  `ui/main_geometry` 恢复），标题栏高 `46`，左右内容留白 `28`。右下角 `QSizeGrip`，
  在 `resizeEvent` 里重新定位（恢复几何后也靠这里重摆）。
- 页面是三层栈：`HomePage`（首页网格，页头隐藏）/ `SettingsPage` / 动态工具面板。
  页头在工具页与设置页显示，含「← 首页」、标题/描述、「独立窗口」（设置页隐藏）。
- 首页网格：磁贴固定 `104×100`（图标 48），列数 = `(视口宽 - 2*28 - 12) / (104 + 8)`。
  **恒定扣掉 12px 滚动条预留**，见踩坑表。键盘：`←→` 移一格、`↑↓` 移一行、`Enter` 开当前项。
- 搜索时工具与本机应用分两组（「工具」「本机应用」），磁贴顺序 = 键盘导航顺序：
  最近使用 → 工具结果 → 应用结果。空关键词时应用不展示（只可能出现在「最近使用」里）。
- 独立窗口 `ToolWindow`：原生边框（不加 `FramelessWindowHint`、不用 `Qt::Tool`），
  `WA_DeleteOnClose`，面板随窗口销毁；去重表 `QHash<QString, QPointer<ToolWindow>>` 在 `MainWindow`。
- `Esc` 分层见「交互约定」第 2 条；热键捕获中的 `Esc` 由 `HotkeyButton` 吞掉只取消捕获。

## 样式（`src/app/resources/style.qss`）

- 全部按 **objectName 选择器**生效：`#mainRoot` `#titleBar` `#titleText` `#titleHotkey`
  `#titleBtn` `#titleBtnSettings` `#titleBtnClose` `#homeRoot` `#homeTop` `#homeSearch`
  `#homeSearchClear` `#homeScroll` `#homeContent` `#sectionTitle` `#launcherTile` `#tileName`
  `#homeFooter` `#homeHint` `#homeEmptyTitle` `#homeEmptyDesc` `#pageHeader` `#pageBack`
  `#pagePopout` `#toolTitle` `#toolDesc` `#toolStack` `#toolWindowRoot`；按钮用 `[accent="true"]`。
  插件内的 `#jsonCompleterPopup`（QCompleter 的补全弹窗）也在这份全局 qss 里 —— 弹窗是
  顶层 `QAbstractItemView`，只能靠 objectName 命中，见踩坑表。
- 磁贴的 hover / 键盘选中走**动态属性**（`#launcherTile[hovered="true"]`、`#launcherTile[current="true"]`），
  不用 `:hover` —— QWidget 子类的 `:hover` 依赖样式引擎主动打 `WA_Hover`，跨版本不保证触发。
- 全局 `QPushButton` 带 `padding: 6px 14px`，**30px 宽的小按钮必须显式 `padding: 0`**，
  否则文字被挤没、只剩一个小点（标题栏的最小化/关闭曾因此显示成两个点）。
- qss 通过 `.qrc` 编进二进制（`CMAKE_AUTORCC ON`），路径固定 `:/style.qss`；改完不用改代码，重编即可。

## JSON 编辑器（`plugins/jsonfmt`，插件内部）

- **折叠**：Qt 没有公开的折叠 API，做法是把区块内部的 `QTextBlock::setVisible(false)`
  （Qt Creator 也是这么干的）。隐藏行仍留在文档里 —— 块号、字符位置都不变，
  所以行号是绝对行号（折叠第 9 行后下一个可见行就是 19），光标也不会错位。
- 可折叠区块由 `jsontext::foldRegions()` 扫出来（纯逻辑，可单测）：只认**跨行**的 `{}`/`[]`，
  字符串里的括号不算，括号没配齐时静默跳过（用户正在敲）。区域是延迟重算的
  （`m_foldTimer` 200ms），所以 `m_foldDirty` 为真时 `foldEnd()` 一律返回 -1、不画箭头。
- 折叠状态存的是"起点行 → 当时算出的终点行"。文本一变，只有两端都对得上的折叠才保留，
  对不上就丢掉：沿用旧行号会把另一段内容莫名藏起来。
- 光标落进隐藏行（撤销、跳到出错行、程序化设置）会**自动展开**那个折叠，否则光标藏起来了。
- 折叠占位符（`⋯ N 行`）画在 `paintEvent` 里叠在正文之上，点它即展开；点行号栏的箭头同样切换。
- 行号栏是子控件 `JsonGutter`（`JsonEditor` 的 friend，转调私有钩子）。**它不能放进匿名命名空间**：
  头文件里的 `friend class JsonGutter;` 认的是全局作用域那个类。
- **读全文一律用 `JsonEditor::fullText()`**，不要 `toPlainText()`：隐藏块是否计入没有文档保证
  （Qt 6.8 实测会计入，但不该赌）。格式化、复制、扫描补全键都走 `fullText()`。
- **进插件就聚焦输入框**（`JsonFmtPanel::showEvent` 里 `QTimer::singleShot(0, ...)` 再 `setFocus`）。
  同步设会被宿主的焦点链覆盖，所以必须延后一拍。
- **粘贴即格式化**（`JsonFmtPanel::formatPasted`）：`JsonEditor::insertFromMimeData()` 是
  Ctrl+V / 右键粘贴 / 拖放 / 中键**所有入口的汇合点**，在那里发 `textPasted()` 信号；
  面板收到后若整篇能解析就整篇重排，不能解析（粘贴的是片段或根本不是 JSON）就静默不动。
  刻意**不做**"边打字边整篇重排"：光标会被甩走、撤销栈被打散，输入体验更差。
- **手动输入只做自动缩进**（`JsonEditor::handleAutoIndent`，排在 `handleAutoPair` 之前）：
  回车按当前行结构给缩进（上一行以 `{`/`[` 结尾就 +1 级，否则与上一行对齐；
  光标卡在自动补齐的 `{}` 中间时一次拆成三行）；行首敲 `}`/`]` 先退一级；
  后面紧跟着同一个闭括号就直接跳过去（否则会出现两个闭括号）。
  缩进单位默认 4 空格（与 `QJsonDocument::Indented` 一致），当前行用 Tab 时跟着用 Tab。
  输入法拼字（`isComposing()`）期间一律不介入。
- 粘贴后光标的三级映射见 `mapCursorPosition()`：文末 → 文末；非空白字符锚点（需校验）；
  都对不上（`QJsonDocument` 输出会按键名排序）才退回"保行号 + 列号夹住"。

## 内存监控（`plugins/memmon`，插件内部）

- 分三层：`MemProbe`（唯一碰平台 API 的地方，只暴露 Qt 值类型）、
  `MemText.h`（纯逻辑：单位换算、占用率、筛选排序、回收候选，全部 inline）、`MemMonPanel`（界面）。
  纯逻辑抽成 header 是为了能在临时工程里直接断言，不必真的去刷别人的工作集。
- **采样只在面板可见时进行**：`showEvent` 起定时器、`hideEvent` 停。内嵌面板被页栈藏起来后
  还每 2 秒枚举几百个进程（每个都要 OpenProcess）是纯浪费。
- **回收能力只有 Windows 有**（`EmptyWorkingSet`）。Linux 内核与 macOS 都没有普通用户可用的
  「刷出指定进程工作集」接口，两个平台返回明确失败文案（`unsupportedTrimReason()`），
  把「自动释放」勾选框自动弹回并在状态栏说明原因 —— 不允许留一个"看着开着其实什么都不做"的开关。
- `EmptyWorkingSet` 需要 `PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA`，**不能用** `QUERY_LIMITED`：
  后者只够读统计，刷工作集会被拒绝。列表采样则相反，用 `QUERY_LIMITED` 能多打开一批受保护进程。
- 回收语义是"把工作集写进页面文件 / 待机列表"，进程不退出、数据不丢，但**短期会变慢**。
  因此：手动批量只挑 64 MB 以上、最多 40 个；自动释放只挑 128 MB 以上、最多 6 个，且有 30 秒冷却
  （`kAutoCooldownSecs`）—— 连续刷同一批进程只会让系统更没效率。
- 列表刷新做了两档：pid 序列不变就**只改文字**（`updateRow`），变了才整表重建（`rebuildTableFully`），
  重建时按 pid 恢复选中行、夹住滚动位置。每次刷新都 `clear()` 重建会每 2 秒闪一下、选中的行也丢。
- 表格行高由 `RowHeightDelegate::sizeHint` 给最小值（30px）。qss 的 `::item padding` 只管左右，
  竖向高度来自 delegate，写 qss 调不动。
- 阈值下拉框的箭头是 `ComboArrow`（`QComboBox` 的自绘子控件、`WA_TransparentForMouseEvents`），
  由 `MemMonPanel::eventFilter` 在 Resize / EnabledChange 时重摆与改色。qss 无法可靠画这个箭头，
  原因见下面踩坑表；别再往回去改 `::down-arrow`。
- 自动释放的开关与累计释放量都是**面板实例状态**，没有落盘：宿主不缓存面板，离开工具即重置，
  与「面板不缓存」的既有约定一致（也避免两份面板互相串状态）。

## 翻译（`plugins/translate`，插件内部）

- 分三层：`TranslateLogic.h`（纯逻辑：语言表、源语言检测、请求构造、四家响应解析、分段/拼接）、
  `TranslatePanel`（界面 + 异步调度）、`TranslateTool`（插件入口）。纯逻辑做成 header 是为了
  临时断言程序能直接 include；面板也放头文件，临时工程开 AUTOMOC 就能做端到端断言。
- 四家数据源按枚举顺序即降级顺序：有道演示接口（默认）→ 搜狗网页接口 → MyMemory →
  Google gtx（海外兜底）。JSON 形态完全不同，统一在 `parseResponse()` 里归一化成
  `ParseResult`，面板不感知差异；`RequestSpec.error` 让纯逻辑能在**不发请求**的前提下判
  「这家不支持当前方向」，面板直接走降级。
- **搜狗要先握手**：`GET https://fanyi.sogou.com/` 下发 cookie，SSR 的
  `window.__INITIAL_STATE__` 里有 `secretCode` / `uuid`；没有 cookie 时接口固定返回
  `translate.errorCode = "s10"`。表单签名是 `md5(from + to + text + secretCode)`，
  握手与翻译请求必须用同一个浏览器 UA。会话参数只缓存在面板实例里（`m_sogouSecret`），
  s10 时清掉、下次重新握手；不落盘、不做全局缓存，避免内嵌/独立窗口两份面板串会话。
- **搜狗的坑方向**：繁体中文不是它的目标语言（原样返回原文）、`en→ko` 稳定返回
  `status=-1`、源=目标时它会擅自反转方向（`en→en` 返回中文）。三类都由
  `sogouDirectionError()` 在发请求前拦掉并降级；自动模式用本地检测码参与同语言判断。
  0.1 秒连发也会偶发 `status=-1`，所以握手也计入同一个节流窗口。
- **腾讯不接数据源**：官方 API 已于 2025-09-15 关闭，`api.fanyi.qq.com/api/v1/*` 要
  TCaptcha 换 Token、`transmart.qq.com/api/imt` 要登录态，桌面端无法匿名调用。曾做过
  「腾讯网页 ↗」跳转按钮（剪贴板带文本 + `QDesktopServices::openUrl()`），应用户要求已移除：
  面板只保留四家真正可用的服务，搜索关键词里的「腾讯」也一并去掉，别留找不到入口的死文案。
- **引擎入口必须看得见**：全局 qss 写了 `QComboBox::drop-down` 后 Qt 不再画默认三角，
  源/目标语言与引擎三个下拉都要用自绘 `ComboArrow`（objectName `transComboArrow`，
  `WA_TransparentForMouseEvents`）+ `TranslatePanel::eventFilter` 跟着 Resize 摆位；漏补时
  「自动（4 家）」看着只是一行静态文字，用户会以为整个工具只有一个引擎。除了箭头，还加了
  `transEngineLabel`（「引擎」标签）与 `transEngineHint`（常驻服务链，直接列出四家名字，
  每项在下拉里另带 ToolTipRole 说明），后者设 `QSizePolicy::Ignored`，窄窗口裁字也不撑大
  窗口最小宽度。状态栏只报本次真实来源，不能拿它当「有哪些服务」的说明书。
- **限流是常态**：有道连发几条就返回 411，部分 `Auto→小语种` 方向返回 102，文本超过约
  1000 字符返回 103；MyMemory 单次 q 上限 500 字符（403，且把错误文案塞进 `translatedText`）。
  所以统一按 400 字分段、串行请求、任意两次请求间隔 ≥1000ms；任一引擎失败就**整篇**换下一家
  重译（保证译文来源一致），全部失败时把每家原因逐条列在状态栏，不允许静默失败。
- **源语言靠本地检测**：MyMemory 的 `langpair=autodetect|xx` 会把原文原样返回（实测
  "Hello world" 仍是 "Hello world"），有道 `Auto→ja` 直接报 102。`detectSource()` 按 Unicode
  区段判断（假名/谚文优先于汉字，汉字压过拉丁）。有道/搜狗在自动模式用各自的 `Auto`/`auto`
  （服务端检测对拉丁语系更准），MyMemory/Google 则传本地推断出的明确码。
- `QUrlQuery` 构造 MyMemory 的 `langpair` 里带 `|`，必须走 FullyEncoded（`%7C`）；有道与搜狗
  的 POST 表单也要 `query.toString(QUrl::FullyEncoded).toUtf8()`，别手工拼字符串
  （签名用的四个参数必须与表单逐字一致）。
- 所有 `errorCode` / `responseStatus` / `status` 都可能是字符串或数字（有道 `"0"` 是字符串、
  411 是数字，搜狗 `status` 是数字），统一走 `scalarText()`；只按 `isString()` 读会把数字
  形态全判成失败。
- 异步回复一律带 `generation`：面板不缓存且可同时存在内嵌/独立窗口两份，重新翻译、清空或
  析构时用 `cancelInFlight()` 让旧回复失效；`connect` 的上下文必须是 `this`。
- 「输入后自动翻译」默认开启且防抖 1100ms：公开接口有频率限制，必须保留防抖与请求间隔，
  避免边打字边请求撞 411；复选框在信号连接前置为选中，空面板不会因此发请求。
  状态三态走 `setProperty("state")` + `unpolish/polish`，颜色只在全局 qss 里定义。
- 只做纯文本翻译，不做词典释义/发音/文档翻译；待翻译文本会发给第三方服务，README 已提示
  不要用于敏感内容。接口是公共服务，随时可能变更；搜狗页面还带 `transOfflineTime:
  2026-11-27` 下线标记，届时该引擎可能失效，别把可用性当成不变前提。

## 取色器（`plugins/colorpicker`，插件内部）

- 分三层：`ColorText.h`（纯逻辑：颜色文本解析、7 种格式输出、WCAG 对比度与建议文字色）、
  `ColorProbe.h`（屏幕取样 + 平台能力说明，只用 Qt API）、`ColorPickerPanel.*`（界面）。
  纯逻辑抽成 header 是为了能在临时工程里直接断言，不必拉起整个界面。
- **屏幕取色不 `grabMouse()`**：`grabMouse()` 会吃掉其他应用的鼠标事件。改成取色态下用
  `QTimer`（40ms）轮询 `QCursor::pos()` + `leftButtonDownNow()`，全程不干扰别的应用。
  吸管浮窗对鼠标事件透明（`WA_TransparentForMouseEvents`），点击穿透、不挡取样点。
- **左键状态必须实时查询，不能用 `QGuiApplication::mouseButtons()`**：它不是实时查询，
  只反映 Qt 应用**最近一次收到的鼠标事件**里的按键状态。吸管模式下宿主窗口隐藏、浮窗对
  鼠标透明，光标在其他应用上时本应用收不到鼠标事件，mouseButtons() 永远停在「未按下」——
  表现就是「预览跟着光标走、按下左键死活不锁定」（2026-11-08 实修）。Windows 用
  `GetAsyncKeyState(VK_LBUTTON) & 0x8000` 查物理按键实时状态（全局、无需焦点），
  其他平台退回 mouseButtons() 兜底（macOS/Linux 下该问题同样存在，属已知边界）。
- **打开工具即进入「吸管模式」**：`showEvent` 首次显示时用 `QTimer::singleShot(0)` 延一轮调
  `showEyedropper()`（隐藏本面板所属顶层窗口、显示跟随鼠标的 `EyedropperWindow` 浮窗）+
  `startPicking()`，等宿主布局/绘制完成再抓屏。浮窗是**独立顶层窗口**（`Qt::FramelessWindowHint |
  Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus` + `WA_ShowWithoutActivating` +
  `WA_TransparentForMouseEvents`），脱离宿主窗口、跟随光标移动，实时显示放大镜 + HEX 色值。
  宿主窗口隐藏后光标不再落在「本应用窗口内」，全屏任意位置都能取色。`m_autoPicked` 挡门保证只
  自动触发一次；`m_eyedropperMode` 让 `hideEvent` 知道「窗口是面板自己藏的、别停采样」。
  状态机两段：`ignoreUntilRelease`（忽略首次按下的那一下）→ 实时预览 → **按下左键即锁定**。
  **锁定即自动复制 HEX**：`onPickTick` 的 `leftDown` 分支调 `lockAndCopy()`（`recordColor` +
  写 HEX 到剪贴板并回读校验），随后浮窗显示「已复制 #XXXXXX」停留约 1.2 秒（`EyedropperWindow::
  showResult`）再 `hideEyedropper()` 关浮窗、整体收起宿主窗口，取色动作就此结束、不再弹回面板。
  裸 `Esc` 也取消取色并走 `hideEyedropper()`。
- **取色期间系统鼠标光标变十字准星（+）**：`CrosshairCursor` 类封装——Windows 用原生
  `SetSystemCursor(CopyImage(LoadCursor(IDC_CROSS)), OCR_NORMAL)` 替换系统级 OCR_NORMAL 光标
  （跨应用生效，Qt 的 `setOverrideCursor` 只对本应用窗口有效，鼠标移到别的应用就变回箭头）。
  备份原箭头句柄、结束后 `SetSystemCursor(s_backup, OCR_NORMAL)` 恢复；`grab()`/`release()` 幂等。
  两个坑：① `OCR_NORMAL` 等常量在 WinUser.h 里被 `#ifdef OEMRESOURCE` 保护，而 OEMRESOURCE 在
  `WIN32_LEAN_AND_MEAN` 下不自动定义，需在 include windows.h 前显式 `#define OEMRESOURCE`；
  ② `SetSystemCursor` 会 destroy 传入句柄，`LoadCursor` 返回的共享句柄不能直接传，必须
  `CopyImage(..., IMAGE_CURSOR, 0, 0, 0)`（flags=0 复制副本、不删原图）先复制一份。
  macOS/Linux 无等价的「全局替换系统光标」Qt 公开接口，退而用 `setOverrideCursor(Qt::CrossCursor)`
  （仅本应用窗口内生效），不假装成功。
- **光标在本应用窗口内不采样**：`cursorOverOwnWindow()` 遍历 `QApplication::topLevelWidgets()`
  的可见 `frameGeometry()`。否则刚点开时光标还在磁贴/面板上，会把窗口背景当成取到的颜色，
  用户点输入框/复制按钮的那次按下+松开还会被误判成锁定。吸管模式下宿主窗口已隐藏、浮窗对鼠标
  透明，不会命中此分支；它兜底覆盖「手动点按钮取色」的普通路径。窗口内只更新坐标标签、整个 tick
  跳过；`startPicking()` 的状态文字一次写全，tick 不再覆盖状态，红色取样失败文案才留得住。
  实时预览时只在输入框**无焦点**才同步 HEX。
- **抓屏瞬时失败不立刻退出取色**：切屏、锁屏、远程桌面重连时 `grabWindow()` 可能只抖一下，
  连续 3 次（约 120ms）失败才 `setStatus(error)` + `stopPicking()`；否则取色会因为一次
  抖动就停下、逼用户再点一次按钮。成功一次即清零，最终失败仍给明确原因（规则 3）。
- 取样走 `QScreen::grabWindow(0, ...)`，坐标是「`QCursor::pos()` 减去该屏 `geometry().topLeft()`」，
  请求区裁剪到屏幕内（光标贴边时）。**不要假设返回图与请求区 1:1**：Qt 在高 DPI 下会返回放大的
  设备像素图（本机 13×13 的请求拿到 16×16），中心像素必须用 `image.size()/area.size()` 的实际比例
  反推；放大镜直接显示这张原图，天然保持正确密度。
- 三平台差异只体现在 `ColorProbe::platformHint()` 的文案：macOS 需屏幕录制授权（未授权系统返回
  全黑且 Qt 不报错）、Linux Wayland 拿不到像素、Windows 远程桌面可能黑屏。抓图失败一律给可读原因，
  **不静默返回黑色**；手动输入 / 剪贴板读入是权限受限时的兜底路径。
- `parseColor()` 结果统一经 `detail::normalized()` 转成整数 RGBA 规格：`fromRgbF(..., 0.5)` 与
  `QColor(r,g,b,128)` 的 alpha 内部定点值不同，`operator==` 会判不等，历史色块去重会因此失效。
- 复制走 `copyText()`（手动「复制」按钮与「复制全部格式」）并**回读剪贴板校验**：`QClipboard::setText()`
  没有返回值，Windows 上剪贴板被别的进程占用时 Qt 只打一条 COM 警告；不回读就会把失败当成功报给用户。
- **`onTick` 必须有重入锁（踩坑，曾致堆损坏崩溃）**：`QScreen::grabWindow()` / `QClipboard::setText/text()`
  在 Windows 上会内部转消息泵，使 40ms 定时器在本 tick 未返回时又触发一次 `onPickTick`；嵌套 tick
  会并发改写 `m_color` / `m_image` / `m_fieldEdits` 这些隐式共享对象的引用计数 → 堆损坏
  （`0xC0000374`，实测「点开取色器卡住→进程崩溃」）。入口 `if (m_inTick) return;` + RAII `TickGuard`
  出口复位。另外把锁定后的剪贴板复制用 `QTimer::singleShot(0, this, ...)` **挪出 tick**：剪贴板读写会
  转消息泵，同步做要么被重入锁挡住、要么面板销毁时 `this` 悬空（绑 `this` 为 context 则自动取消）。
  改这段时务必复测：长时间连续取样（约 150 次抓屏）不崩、锁定后 clipboard==hex。
- CMYK 是 Qt 的 `cyanF()` 等屏幕色近似换算，不是印刷分色；「最近颜色」是面板实例状态，不落盘。

## OCR 文字识别（`plugins/ocr`，插件内部）

- 四层：`OcrText.h`（纯逻辑，可临时断言）、`OcrEngine.*`（唯一碰平台差异的地方）、
  `OcrCapture.*`（纯 Qt 全屏框选浮层）、`OcrPanel.*`（界面）。分层理由同 memmon/colorpicker：
  纯逻辑抽成 header 才能在临时工程里直接 include 验证。
- **两端输出统一成「带坐标的词」**：Windows 后端输出 `W|x|y|w|h|text`，tesseract 输出 TSV
  （取 `level==5` 的 word 行），都由 `OcrText.h` 归一化成 `Word{x,y,width,height,text}`，
  排版（归行、拼接、分列）只有一份实现。
- Windows 用系统自带 PowerShell 调 `Windows.Media.Ocr`（WinRT），**不写 C++/WinRT**：
  那样要 windowsapp.lib / winmd 投影头 / 异步初始化，会把三端构建拖进平台依赖。
  `OcrEngine.cpp` 的平台分支仍是 Win / macOS / Linux / 兜底四段齐全，macOS 与 Linux 走外部
  `tesseract`（先试 `eng+chi_sim`，失败降级默认语言），缺引擎返回可读原因。
  **macOS / Linux 没有实机验证**，不要往这两段塞未经验证的 Vision / 原生 OCR 代码。
- 嵌入的 PowerShell 脚本**必须纯 ASCII（含注释）**：PS 5.1 按本地代码页（GBK）读无 BOM 的
  `.ps1`，实测中文注释会让后面的 WinRT 类型解析成 null（「不能对 Null 值表达式调用方法」）。
  `runWindowsOcr()` 开头有逐字节 ASCII 防呆；识别结果里的中文不受影响（stdout 显式 UTF-8）。
- 临时脚本与临时 PNG 都用 **`QTemporaryDir` + 普通 QFile / `QPixmap::save`**，不用
  `QTemporaryFile`：后者在 Windows 上一直持有底层句柄，外部进程打开时报「正由另一进程使用」。
- WinRT OCR 会把每个汉字切成独立词并用空格相连，只取 `result.Text` 会得到「最 近 使 用」。
  所以取词级 `BoundingRect`，拼接时只有「两边都是 CJK」才连写，中文标点一律紧贴；
  归行容差取 3/4 行高（逗号/句号的基线比同行汉字低约半个字高），不写死像素以适配高 DPI。
- 截图浮层**不设 parent**：调用方要先 `hide()` 宿主窗口，挂成子窗口会被一起藏掉。
  面板用 `QPointer` 兜住浮层，析构时 `deleteLater()` 并恢复宿主可见性；`hide()` 到
  `grabWindow()` 之间留 260ms 让桌面重绘，否则截到主窗口残影。
- 识别是同步阻塞调用：宿主随时可能销毁面板（面板不缓存），异步回调容易踩悬空指针。
  期间只用 `m_status->repaint()` 刷新状态，**不用 `processEvents()`**（会进事件循环导致重入销毁）。
- 不注册第二个全局热键：全局热键只做「唤出首页搜索」是已定型的交互约定（第二部分第 3 条），
  改它要先问。翻译 / 公式识别 / 结构化表格也不做，「表格文本」只是按空隙分列的 TSV 近似。

## 文件占用释放（`plugins/filelock`，插件内部）

- 三层同 memmon：`FileLockLogic.h`（纯 Qt Core：路径解析 / 展开 / 归一化 / 匹配 / 按 pid 归并，
  可单独断言）、`FileLockProbe`（唯一碰原生 API 的地方，只出 Qt 值类型）、`FileLockPanel`（界面）。
- **平台能力**：检测三端都有 —— Windows Restart Manager（官方接口，按文件查占用者）、
  Linux `/proc/<pid>/fd` 符号链接、macOS 系统 `lsof -F pcn`（未在实机验证，失败返回原始错误）。
  **「解除占用」只有 Windows**：`NtQuerySystemInformation(64)` 枚举句柄 → 按选中 pid 过滤 →
  `DuplicateHandle` 复制到本进程 → `GetFinalPathNameByHandleW` 拿 DOS 路径 → 命中后
  `DuplicateHandle(DUPLICATE_CLOSE_SOURCE)` 关源句柄。Linux/macOS 没有「替别的进程关 fd」的
  普通用户接口，按钮置灰并给原因，不偷偷降级成杀进程。「强制结束」三端都有
  （`TerminateProcess` / `SIGTERM`，刻意不发 `SIGKILL`，不替用户决定丢数据）。
- **为什么检测与解除用两套机制**：RM 只给进程不给句柄，但比逐个 `NtQueryObject` 快几个数量级；
  解除时才枚举句柄且按 pid 过滤，规模小得多。`NtQueryObject(ObjectNameInformation)` 会在同步
  管道句柄上永久阻塞，`GetFinalPathNameByHandle` 不会，且直接给 DOS 路径（省掉卷映射）。
- 路径比较统一走 `pathKey()`：手动把 `\` 换成 `/`（`QDir::fromNativeSeparators` 在 Unix 不转）、
  解析符号链接、`cleanPath`，`caseInsensitive` 由调用方按平台传，header 内无 `#if`。
  `GetFinalPathNameByHandle` 给的 `\\?\C:\...` / `\\?\UNC\...` 必须过 `fromWin32FinalPath()`
  再去比，否则全不匹配。Linux 的 fd 目标会带 ` (deleted)` 后缀，要先 chop(10)。
- 文件夹展开上限 512 个文件（`kMaxExpandedFiles`），超出置 `truncated` 并在状态栏提示缩小范围，
  不静默只查前几个；递归与否由 `matchesPath()` 判断剩余路径里是否还有 `/`，避免「只查顶层」
  在 procfs 上退化成隐式递归。
- 检测放 `QThread::create` 工作线程：worker 只写一份 `QSharedPointer<ScanOutcome>`，结果回调
  以 `this` 为 context（面板销毁后 Qt 丢弃投递）；线程对象 `finished → deleteLater`，
  面板析构里 `wait()` 兜住正在跑的 worker。别把 RM / procfs 扫描放回 UI 线程。
- 宿主自身 pid、Windows 的 pid 0/4、POSIX 的 pid ≤ 1 一律从可操作对象里排除；
  「解除占用」与「强制结束」都走 `QMessageBox` 二次确认，前者要明确说明「进程不退出但
  下次读写可能报错 / 崩溃 / 丢数据」。
- 表格用 `QTableWidget` + 第一列 `ItemIsUserCheckable`；重建期间 `m_building` 吞掉
  `itemChanged`，否则会误刷「已选择 N 个程序」。空态与表格放在 `QStackedWidget` 里切换，
  **选页面不能用三元表达式**（`QLabel*` 与 `QTableWidget*` 无隐式转换，MSVC C2446）。

## 共享（`plugins/share`，插件内部）

- 四层：`ShareHttp.h`（纯函数：请求行/头解析、`percentDecode`、keep-alive 判定、
  `Content-Disposition`、MIME、`humanSize`、`looksLikeVirtualNic`、`localAddresses`，
  可单独断言）、`ShareStore`（数据 + 落盘）、`ShareServer`（HTTP 服务）、`SharePanel`（界面）。
  `ShareWeb.h` 是内联单页 HTML，`indexHtmlData()` 直接返回 `const char*`，不额外落盘。
- **刻意破例的单例**：`ShareStore::instance()` / `ShareServer::instance()` 在插件里是进程级
  单例，且**不注册析构**（`new` 出来放着）。原因是宿主的 `IPanel* createPanel()` 不缓存面板，
  同一个工具可以同时存在内嵌面板和独立窗口两份 —— 若面板持有服务，会变成「开两个窗口占两个
  端口、各有一份自己的列表」。单例保证"一个端口 + 一份数据"，所有面板看同一份模型。
  面板通过 `ShareStore::changed()` 重建视图，网页端靠 `/api/state` 2 秒轮询。
- 不用 `QHttpServer`：Qt6 的 `QHttpServer` 在 6.4 之前只有技术预览、且不同发行版打包状态不一，
  跨平台稳不住。这里只手写 HTTP/1.1 的一个子集（`QTcpServer` + `QTcpSocket`），
  每个连接一个无 `Q_OBJECT` 的 `HttpConnection` 对象，三段式状态机
  `parseHead → consumeBody → dispatch`，`Content-Length` 决定 body 何时读完。
- **上传**走 `QSaveFile`（写临时文件 + `commit()` 原子替换），文件名先净化（去路径分隔符、
  去 `..`、控制字符）再落 `uploads/`，重名自动加 `(1)`；`X-File-Name` 用 percent-encoded
  传中文，不塞进 header 明文。**下载**按 `kDownloadChunk = 64KB` 分块，
  `pumpDownload()` 盯着 `bytesToWrite() > kSocketHighWater` 就停手等 `bytesWritten`，
  避免把 2GB 文件一次性灌进 socket 缓冲把内存打爆。
- 上限集中在 `ShareStore`：文件 200 / 文本 50 / 单条文本 16KB / 一次展开目录 300 个文件 /
  单文件上传 2GB。超限在接口层直接返回 4xx 或 `truncated` 提示，不静默截断。
- **局域网地址排序**是这块最容易踩的坑：`QNetworkInterface` 会把 VMware / Hyper-V / WSL /
  Docker 的虚拟网卡一起列出来，实测 VMnet8 会抢首位，手机扫到的是个连不通的地址。
  所以 `looksLikeVirtualNic()` 按名字关键词降权，再按网段加权（192.168 / 10 / 172.16-31 优先），
  最后 `.1` 结尾（多半是网关位）降权。断言程序里有「真实网卡优先」的用例，改权重先跑它。
- 端口从 5421 起顺延，最多试 20 个；被占就换下一个并打日志，界面上显示实际端口。
  桌面端 5 秒重新枚举一次网卡（插拔网线 / 换 WiFi 后地址会变），网页端不管这个。
- **没有鉴权**，只适合可信局域网。别在 README 之外的地方暗示"安全"，也不要加"远程访问"
  之类的引导；`uploads/` 是网页上传的副本，删除条目只删记录不删原文件（引用语义）。

## WebSocket 调试（`plugins/websocket`，插件内部）

- 四层：`WsProtocol.h`（纯逻辑：地址解析、客户端/服务端握手、帧编解码、关闭码，全 inline 可临时断言）、
  `WsClient.*`（客户端连接层）、`WsServer.*`（服务端会话层）、`WebSocketPanel.*`（客户端页 / 服务端页 / 容器）。
  **不用 Qt WebSockets**：锁定的 Qt 6.8.3 安装里没有该模块（`lib/cmake` 与 `bin` 都查过），
  两端都用 `QTcpSocket` / `QSslSocket` / `QTcpServer` 自实现 RFC 6455，零新依赖、无平台分支。
- **掩码方向由角色决定，写在一处**：`FrameReader(expectMasked)` 服务端=true（必须带掩码，解析时解掩码）；
  客户端=false（带掩码即判错）。`encodeFrame(op, payload, mask)` 同理，服务端发帧统一 `mask=false`。
  方向判反的表现是"每组载荷都变乱码"，排查先看这两处。
- `WebSocketPanel` 里两页**同时存活**（QStackedWidget）：切页不打断正在跑的连接 / 监听是刻意的，
  调试时常要一边当服务端一边用别的客户端连。代价是两页的 socket 常驻，面板销毁时一起收。
- 服务端默认只绑 `127.0.0.1`，对外暴露必须用户勾「允许局域网访问」（绑 `0.0.0.0`）。
  子协议留空 = 回显客户端提出的第一个（任何客户端都能连）；填了 = 只接受交集，无交集拒绝握手。
  握手校验只查必须项（GET / HTTP 1.1 / `Upgrade` / `Connection` / `Version: 13` / Key 16 字节），
  **不限制路径与 Origin** —— 这是本机调试服务端，接谁进来由用户决定。
- **`~WsServer()` 必须 `blockSignals(true)` 后再收尾**：QObject 析构是先删子对象、后断自己的信号连接，
  而 `m_server` 是页面的子对象；此时若发出 `clientDisconnected`，页面的槽照样被调用，
  槽里的 `m_log` 已经先一步 delete → 退出时段错误（实测踩到，别去掉）。
- 握手被拒走 `disconnectFromHost()`，**不要 `abort()`**：abort 丢弃未送达字节，客户端只会看到
  "连接被重置"而拿不到 4xx 原因。会话清理交给 `disconnected`（open == false 时静默销毁）。
- 超时全是 `QTimer::singleShot` + `sessionFor(id)` 复检：握手 8 秒、关闭 2 秒。
  会话销毁后回调找不到 id 就自动失效，不需要给每个会话配 QTimer 成员。
- 服务端**不做 TLS 监听**（要证书与私钥输入，超出"简易"范围）、不主动 ping（但必定回 pong，
  载荷原样带回）、最多 64 个客户端、不协商扩展。这些都写进了 README 的已知边界。
- **临时测试的坑**：测同线程里的服务端不能用 `waitForConnected` / `waitForReadyRead` ——
  它们只阻塞在自己那个 socket 上，服务端收不到 `newConnection`，会误判成"服务端没响应"。
  用轮询 + `QEventLoop` 的等待函数（`waitFor(pred, ms)`）。服务端另用独立实现的裸 socket
  Python 客户端验证过互操作（握手 / 掩码 / 分片 / ping-pong / 1002 拒绝），别只用自己的客户端自证。

## 日志

- 位置：`QStandardPaths::AppLocalDataLocation` 下的 `logs/app.log`（不可漫游目录，避免企业漫游配置同步日志）。
- 处理器在 `main.cpp` 的 `Log::init()` 装好，之后的 `qInfo/qWarning/qCritical` 同时落盘。
- Windows 大致在 `%LOCALAPPDATA%\cxk\cxk-tools\`；配置在 `AppConfigLocation` 下的 `cxk-tools.ini`。

---

# 第六部分：踩坑表

这些都是本项目实际踩过并已修好的，改代码时别把它们改回去。

| 现象 | 原因 | 正确做法 |
| --- | --- | --- |
| 翻译连续请求返回 411、长文本返回 103 | 有道演示接口有同 IP 频率限制与约 1000 字上限；MyMemory 单次只有 500 字 | 按 400 字分段串行、两次请求间隔 ≥1000ms，失败自动降级；自动翻译虽默认开启，仍必须防抖并限制请求间隔（见第五部分翻译一节） |
| 译文里出现 `QUERY LENGTH LIMIT EXCEEDED`，或原文被原样返回 | MyMemory 超限时把错误文案塞进 `translatedText`；`langpair=autodetect\|xx` 会命中记忆库返回原文 | 解析时先查 `responseStatus` 再查错误文案；源语言一律由本地 `detectSource()` 推断后传明确码 |
| 第三方 JSON 的数字状态码被判成失败 | 有道 `errorCode` 有时是 `"0"` 有时是数字 411，MyMemory `responseStatus` 同理 | 用 `scalarText()` 兼容字符串/数字/布尔，不要只按 `isString()` 读 |
| 搜狗接口一律返回 `s10`，签名怎么算都不对 | 没先访问首页建立会话（缺 cookie），或握手与翻译请求用了不同 UA | 先 GET `fanyi.sogou.com/` 解析 `secretCode` / `uuid`，cookie 由 `QNetworkAccessManager` 自动保存；两处共用同一个浏览器 UA（见第五部分翻译一节） |
| 搜狗把原文原样返回，或把译文翻成反方向 | 繁体目标不支持、源=目标时服务端自动反向、`en→ko` 返回 `status=-1` | `sogouDirectionError()` 在发请求前拦掉并降级，绝不把错译文当成功结果 |
| 想「接腾讯翻译」却拿不到译文 | 腾讯翻译君免费 API 已关闭，网页接口需要 TCaptcha 滑块 Token 或登录态 | 不要伪装成数据源，也别硬塞跳转按钮凑数（原「腾讯网页 ↗」已按用户要求移除）；说明原因并保持四家真实服务即可 |
| 新写的 `.cpp` 不参与编译，改了半天没效果 | `src/CMakeLists.txt` 的 `APP_SOURCES` 是**显式列表、没有用 GLOB** | 新增/删除源文件必须同步改列表 |
| 界面一片裸控件，qss 完全不生效 | `CMAKE_AUTORCC` 没开，qrc 没被编译 | 保持 `set(CMAKE_AUTORCC ON)`，确认资源路径 `:/style.qss` |
| 配置写不进 ini，或者 `setPath` 没用 | `QSettings` 默认构造在 Windows 上走注册表 | `QSettings(path, IniFormat)` + `setFallbacksEnabled(false)` |
| ini 里出现 `[%General]` | `[General]` 是 INI 保留段名，Qt 会转义 | 组名用 `settings` |
| `QLocalServer` 相关链接失败 | 它属于 Qt Network 模块 | `find_package(... Network)` + 链接 `Qt6::Network` |
| 退出时崩在释放 QString/QWidget；或 Debug 构建一点插件就卡住后崩 `0xC0000374` | 插件与主程序 CRT 不一致——注意是「与宿主**逐配置一致**」而不是固定 /MD：固定 `MultiThreadedDLL` 在 Debug 里会与宿主 /MDd（`MSVCP140D`/`ucrtbased`）和 Qt debug DLL（`Qt6Cored`）各用各的堆，跨 DLL 传 `QString`/`QWidget` 直接堆损坏（Release 两边都是 /MD 所以长期没暴露） | `xtools_add_plugin` 里用 `MultiThreaded$<$<CONFIG:Debug>:Debug>DLL`（与 CMake 默认一致，Debug=/MDd、Release=/MD）；验证方法 `dumpbin /dependents` 看插件与 exe 的 CRT DLL 后缀（`D` 与否）是否相同 |
| 高 DPI 下图标被裁或被放大到溢出 | 设了 `devicePixelRatio` 却按设备像素画 | 按**逻辑像素**坐标绘制，并给 1x/2x 两份 pixmap（见 `IconBadge`） |
| 标题栏按钮只剩两个小点 | 继承了全局 `padding: 6px 14px` | 小按钮显式 `padding: 0; min-height: 0;` |
| Windows 构建报找不到 `rc.exe` | 受限环境 vcvars 探不到 Windows SDK（`reg.exe` 被拦） | `build.bat` 里有 SDK 路径兜底，别删 |
| 运行程序"秒退"，什么都没发生 | 单实例互斥：已有实例在跑，第二个只唤醒旧实例然后退出 | 调试前先查并结束残留进程 |
| 运行 `--shot` 没生成图片、也没报错 | `bin/` 里没有 Qt DLL（被删过，或用 `-DXTOOLS_DEPLOY_QT=OFF` 构建） | 重新配置构建触发 `DeployQt.cmake`，或临时把 Qt 的 `bin` 加进 `PATH` |
| 补全弹窗是系统默认外观，和卡片界面完全不搭 | popup 是 `QCompleter` 内部创建的**顶层** `QAbstractItemView`，没有 objectName 就命中不了全局 qss | `completer->popup()->setObjectName("jsonCompleterPopup")`，qss 按 `QAbstractItemView#jsonCompleterPopup` 写（见 `JsonEditor`） |
| 只三五条候选的补全弹窗也常驻一根竖向滚动条 | 给 item view **本身**加 `padding` 会把 viewport 撑得比内容还小 | view 不设 `padding`，留白放到 `::item` 的 `padding` / `min-height` |
| `QCompleter::setCurrentRow(0)` 之后弹窗里一条都不高亮 | `complete()` 会按新前缀重建候选并重置行号，且 `setCurrentRow()` 在这条路径上拿到空索引 | 在 `complete()` **之后**用 `popup()->setCurrentIndex(completer->completionModel()->index(0, 0))` |
| IDE（CLion）里点运行没反应、编辑器一片红 | CMake 工程没加载成功（没有可用 run configuration），常见于没配 Qt 路径或工具链选了 MinGW | 用 `CMakePresets.json` 的 `debug` 配置、Toolchain 选 Visual Studio，再 `Tools → CMake → Reset Cache and Reload Project`；运行配置选 `cxk-tools` 目标，**不要用单文件「Run 'main.cpp'」** |
| 编辑器顶部提示「此文件不属于任何项目目标」且侧栏文件全灰 | 工程在仓库还没有 `CMakeLists.txt` 时被打开过，CLion 把「普通文件夹」的判断持久化在 `%LOCALAPPDATA%\JetBrains\CLion*\projects\<工程名>.*\project-model-cache\cache.data` 里，之后重启也不会重新识别 CMake | 关掉 IDE → 删 `.idea/` 与该 `projects\<工程名>.*` 缓存目录 → 重新打开。删 `.idea` 时**必须连 `modules.xml` + `.iml` 一起补回**（模块=内容根），否则 CLion 眼里的工程是 0 个文件（日志 `project model has 0 sources ... 0 total files in content`），同样不会配置 CMake |
| 从终端 `start clion64.exe` 启动的 IDE 里 CMake 配置静默失败 | 子进程继承了启动终端的限制：受限账号下 `vcvarsall.bat` 内部调 `reg.exe` 探测 VS/SDK 被拦，日志表现为 `WindowsEnvironmentReader` 的 `InterruptedException` 与 `CMakeRunner - No output` → cmake 找不到编译器 | IDE 一律通过开始菜单/桌面图标正常启动；排查时看 `%LOCALAPPDATA%\JetBrains\CLion*\log\idea.log` 里的 `CMakeRunner` 与 `WindowsEnvironmentReader` |
| 在 IDE 里启动后立刻退出、无窗口 | 运行目录缺 Qt DLL（旧版 `bin/` 只有 exe 和 plugins） | 现在由 `DeployQt.cmake` 自动部署；若仍出现，检查 `bin/` 下是否有 `Qt6Core[d].dll` 与 `platforms/` |
| 插件目录里没东西但也不报错 | 首次构建 `plugins/` 可能不存在 | `loadAll()` 会 `mkpath` 并在日志里告警；`xtools_add_plugin` 也有 POST_BUILD 建目录 |
| IDE 里点运行后界面「已加载 0 个工具」、日志「发现 0 个候选文件」 | 运行配置只构建 `cxk-tools` 目标，插件是独立 MODULE 目标、不在依赖链上 → `bin/plugins` 为空 | 根 `CMakeLists.txt` 收尾处已 `add_dependencies(cxk-tools 全部插件)`（清单由 `xtools_add_plugin` 写全局属性 `XTOOLS_PLUGIN_TARGETS`），点运行即连带构建；仍出现则检查 `bin/plugins` 是否被清过，并**重启应用**（插件只在启动时扫描一次） |
| 本机应用在「最近使用」里重启后全失效 | 直接把路径当 QSettings 键（`/` 被当组层级），或用了 `qHash`（Qt 可能加随机种子） | 用 `LocalAppIndex::idForPath()` 的 FNV-1a 哈希 id（`app:<16 hex>`），路径只存在内存里 |
| `QProcess::startDetached` 打不开开始菜单里的 `.lnk` | `.lnk` 不是可执行文件，CreateProcess 不会解析快捷方式 | Windows/macOS 交给 `QDesktopServices::openUrl`（ShellExecute / LaunchServices）；Linux 用 `gio launch` 或 `Exec` |
| `.bat` 改了中文注释后脚本整体崩 | cmd 按本地代码页（GBK）逐字节解析 | `.bat`/`.cmd` 只能纯 ASCII（规则 5） |
| IDE 运行控制台里日志中文变 `???` 乱码（文件日志却是好的） | `qPrintable()` 实际是 `toLocal8Bit()`，中文 Windows 下向 stderr 写 GBK 字节，而 IDE 控制台按 UTF-8 解码 | 日志写 stderr 一律 `toUtf8()`（见 `Log.cpp` handler）；Windows 下有控制台时 `SetConsoleOutputCP(CP_UTF8)` 兜底 |
| `QLineEdit` 自带清空按钮压在搜索框视觉圆角边框上 | 其内置 clear button 按控件右缘定位，qss 的 `margin`/`padding` 都挪不动它 | 自建 `QToolButton#homeSearchClear` 浮在框内，在 `eventFilter` 的 `Resize` 里显式摆位；别再试 qss 属性 |
| `QWidget` 子类设了 qss `background` 却不显示 | QWidget 子类默认不画样式背景 | `setAttribute(Qt::WA_StyledBackground, true)`（见 `LauncherTile`） |
| 首页磁贴只排一列、之后不再重排 | 列数按滚动区视口宽度算，首帧视口还没拿到真实宽度；只监听 `HomePage::resizeEvent` 会漏掉视口自己的尺寸变化 | 视口也装 `eventFilter`，`Resize` 时 `reflow()`；列数没变时直接 return |
| 网格列数在滚动条出现/消失时反复抖动 | 按真实 viewport 宽度算列数，滚动条占掉 10px → 少一列 → 变矮 → 滚动条消失 → 多一列 … | 列数公式恒定扣 `kScrollbarReserve`(12px) |
| 退出程序后独立窗口还留着、几何没存 | `qApp->quit()` 不会给窗口发 `closeEvent` | 清理放 `MainWindow::prepareQuit()`（存几何 + `closeAllToolWindows()`） |
| 同一秒内开的两个工具，「最近使用」顺序不对 | 时间戳只有秒级精度，退化成按 id 排序；且 `toSecsSinceEpoch()` 会把毫秒抹掉 | 写 `ISODateWithMs`，排序用 `toMSecsSinceEpoch()` |
| `--shot` 截出来的永远是首页 | `showMainWindow()` 会 `showHome()`，切页动作排在它前面就被覆盖 | 先 `showMainWindow()`，再 `--tool` / `--settings` / `--popout` |
| 折叠后读出来的全文少了内容 | `QPlainTextEdit::toPlainText()` 对隐藏块的处理没有文档保证 | 用 `JsonEditor::fullText()`（逐块拼） |
| 行号栏位置偏、压住输入框圆角 | 行号栏按 `frameWidth()` 摆：它把 qss 的 border **和 padding** 一起算进去了 | 用 `contentsRect()` 定位，另给 `setViewportMargins()` 多留 8px 当"分隔线到正文"的留白 |
| 给编辑器 `sendEvent` 鼠标事件没反应 | `QAbstractScrollArea` 的鼠标事件是从 **viewport** 转发上来的，直接发给控件本身会被 `QAbstractScrollArea::event()` 丢掉 | 事件发给 `editor->viewport()`（坐标本来就是 viewport 坐标系） |
| 自动格式化后光标跳到别的 token 上 | `QJsonDocument` 输出时按键名排序，字符流不再一一对应，"非空白字符锚点"会失效 | `mapCursorPosition()` 里先校验锚点、校验不过退回保行号映射 |
| 粘贴走了一条没被监听的路径（右键菜单粘贴没格式化） | 各路粘贴在 Qt 里最终都落到 `insertFromMimeData()`，只管快捷键一定会漏 | 在 `JsonEditor::insertFromMimeData()` 里发信号，别在 `keyPressEvent` 里匹配 Ctrl+V |
| 自动缩进把输入法的拼字打断/吞掉按键 | 拼字期间的按键还不是最终文本 | `handleAutoIndent()` 开头检查 `isComposing()` 直接返回 |
| 进程表每 2 秒闪一下、选中的行也丢了 | 每次采样都 `clear()` 整表重建 | pid 序列没变就只改文字，变了才重建并恢复选中行与滚动位置（`MemMonPanel::rebuildTable`） |
| 表格行高怎么调都是 20px | qss 的 `::item padding` 只管左右，行高来自 delegate 的 `sizeHint` | 用 `RowHeightDelegate` 设最小行高，别再和 qss 较劲 |
| 下拉框右侧出现深色 L 形线 / 箭头消失 / 两个箭头压住文字 | 原生 `::drop-down` 的边框会被 qss 的 `padding` 挤到圆角框外；写规则修它又会让 Qt 停画默认箭头；`::down-arrow { image }` 会把图片重复画到当前项文字后，`background-image` 则整片平铺 | `::drop-down` 只写定位 + `border: none; background: transparent`（并给 `QComboBox` 右侧留 24px padding），箭头用面板里的自绘子控件 `ComboArrow`，位置在 `eventFilter` 里跟着 Resize 摆；每个用到 `QComboBox` 的面板都要各自补一份（翻译页三个下拉曾整页漏补，用户因此找不到引擎入口）；不要试图用 qss 画这个箭头 |
| 读进程内存统计返回 0 / 打不开 | 用 `PROCESS_QUERY_LIMITED_INFORMATION` 读统计没问题，但刷工作集要 `QUERY_INFORMATION + SET_QUOTA` | 采样与回收分别开句柄，别指望一个权限位通吃 |
| 调用系统 OCR 报「不能对 Null 值表达式调用方法」，WinRT 类型全是 null | `.ps1` 里写了中文注释：PowerShell 5.1 按本地代码页（GBK）解析无 BOM 的 UTF-8 脚本，注释乱码后连带破坏后面的类型解析（与 `.bat` 只能纯 ASCII 同源） | 脚本**连注释都保持纯 ASCII**；`runWindowsOcr()` 逐字节检查非 ASCII 并提前报错。识别结果里的中文由脚本显式设 `Console.OutputEncoding = UTF8` 保证，不受影响 |
| 外部进程打不开刚生成的 `.ps1` / PNG，报「文件正由另一进程使用」 | `QTemporaryFile` 在 Windows 上会一直持有底层句柄，`close()` 之后也不释放 | 用 `QTemporaryDir` + 普通 `QFile` 写入（或 `QPixmap::save(path)`），写完即完全释放，目录析构时统一清理 |
| OCR 中文结果变成「最 近 使 用」这种散字 | WinRT OCR 把每个汉字切成独立词并默认以空格相连，直接取 `result.Text` 没法还原 | 取词级 `BoundingRect`，拼接时只有两边都是 CJK 才连写、中文标点紧贴；表格模式按词间空隙插 Tab |
| OCR 结果里「，．」单独占一行 | 中文标点基线比同行汉字低约半个字高，按行高一半聚类会被拆出去 | 归行容差取 3/4 行高（夹在 6..24），既不拆标点也不会把行距正常的两行并起来 |
| 截图浮层跟着主窗口一起消失，或截完主窗口再也不出现 | 浮层设了 parent，宿主 `hide()` 时会连带隐藏；取消/销毁路径没恢复可见性 | 浮层不设 parent，面板用 `QPointer` 跟踪，成功/取消/析构三条路径都调 `restoreHostWindow()`；`hide()` 后延时 260ms 再抓屏 |
| `QStackedWidget` 切页面那行报 C2446「无法从 QTableWidget\* 转换为 QLabel\*」 | 用三元表达式在两个不同 QWidget 子类指针里选一个，MSVC 找不到公共隐式转换 | 用 if/else 分别 `setCurrentWidget()`；同理 `QTextStream` 里三元出的 `QLatin1String` / `QString` 也会编译失败，统一用 `QStringLiteral` |
| Windows 上「解除占用」明明找到了进程，却一个句柄都没关掉 | 拿 `GetFinalPathNameByHandleW` 的 `\\?\C:\...` 直接和 `QFileInfo` 路径比，设备前缀导致全部不匹配 | 先过 `fromWin32FinalPath()` 去 `\\?\`（UNC 转 `//host/share`），再 `pathKey()` 归一化后比较 |
| 「只查文件夹顶层」在 Linux 上把深层文件也算进去了 | `/proc/<pid>/fd` 给的是完整真实路径，只判目录前缀会顺带命中所有子孙 | `matchesPath()` 非递归时要求前缀之后不含 `/`；递归时才放开 |
| 手机上打开的共享地址转圈连不上，电脑上同一个端口却是通的 | `QNetworkInterface` 把 VMware / Hyper-V / WSL / Docker 的虚拟网卡一起列出来，实测 VMnet8 会抢首位 | 用 `looksLikeVirtualNic()` 按名字降权 + 网段加权 + `.1` 网关位降权，见第五部分共享一节 |
| 网页收到的中文文件名是乱码，`+` 变成了空格 | 上传时文件名塞进 header，中文过不去；改用 `QUrlQuery::queryItemValue()` 解码又按 form 规则把 `+` 还原成空格 | 文件名走 percent-encoded（`X-File-Name`），解码用自己写的 `percentDecode()`，**保留 `+`** |
| 共享开两个窗口就变成两个端口、两份列表 | 宿主 `createPanel()` 不缓存面板，把服务挂在面板上就等于每个面板一个服务 | `ShareStore` / `ShareServer` 用进程级单例且不析构，见第五部分共享一节 |
| 设置页里按 `Alt+Space` 毫无反应，`hotkey` 还是旧值 | Windows 把它当窗口系统菜单：Qt 拿到 `WM_SYSKEYDOWN(VK_SPACE)` 直接转 `SC_KEYMENU`，**不生成 `QKeyEvent`**，`keyPressEvent` 永远等不到；菜单弹出还可能抢走焦点让 `focusOutEvent` 恢复旧值 | 录制不能只靠 `keyPressEvent`：`HotkeyButton::nativeEvent` 里补原生消息通道（`GetKeyState` 读修饰键 + 吞掉消息），见第五部分「全局热键」 |
| 设了某个热键后，别的程序里同名快捷键全失效（如 `Ctrl+A` 不能全选） | `RegisterHotKey` 是系统级独占，单修饰键 + 字母这种「常见编辑快捷键」被抢走就全局生效 | 这不是 bug 是全局热键的固有语义：默认值只给 `Ctrl+Alt+Space`，交付时提醒用户别拿单修饰键 + 字母当热键 |
| 断言程序里大量 `qInfo` 一行都不打印 | Release 构建下控制台 sink 被摘掉 | 验证程序统一用 `std::printf` + `fflush(stdout)`，别依赖 Qt 日志分类 |
| 界面上的 `▾` 渲染成方块 | 字体缺字形（中文回退字体里没有这个符号） | 换成纯文字的 `QPushButton("切换")`，跨平台别再赌字形 |
| `--check-update` 跑完进程不退出，被 SIGTERM 杀掉 | 在 `app.exec()` 之前调 `qApp->quit()`，Qt 会忽略这次请求 | 用 `QTimer::singleShot(0, qApp, [code]{ QCoreApplication::exit(code); })` 把退出排进事件循环 |
| 用户开着程序时，脚本里的 `--check-update` / `--shot` 直接退出、什么都不输出 | 单实例互斥把它当成第二个实例，唤出旧窗口后自己结束 | 诊断命令（`--shot` / `--check-update`）在 `main.cpp` 里跳过 `tryLock()` 与 `setActivateHandler` |
| 更新对话框里说明文字下面多出一根"横线" | `QTextBrowser` 默认按需横向滚动，长行触发横向滚动条 | `setLineWrapMode(QWidgetText::WidgetWidth)` + `setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff)` |
| `package.bat` 打包时报 `vc_redist.x64.exe` 拒绝访问 | `windeployqt` 默认塞 25 MB 的 redist 安装器，且该文件被占用 | `--no-compiler-runtime` + 拷 app-local CRT DLL，prune 脚本里再兜底删一次 |
| 安装器向导中文乱码 | `.iss` 被某次编辑存成了 GBK 或带 BOM 的 UTF-8 | 保持 UTF-8 无 BOM（Inno 6 能识别）；仍乱码就改存 UTF-8 with BOM，**不要改文案**（规则 5） |
| 打包报 `'C:\Program' is not recognized` | `%ProgramFiles(x86)%` 写在 `if (...)` 块内，`(x86)` 的 `)` 提前闭合块，剩余文本被当成命令执行 | 把这段逻辑挪出块（标签子程序 + `goto`），块内不要出现带括号的路径变量 |
| 装好的 ISCC 找不到，或报 `"" 不是内部或外部命令` | 同一个块里先 `set` 再 `%VAR%`：整块在解析期就展开完了，读到的是空值 | 同上；确实要在块内取值就 `setlocal enabledelayedexpansion` 后用 `!VAR!` |
| CI 上安装器步骤莫名失败、日志只有一行 err | 没有把「必须有安装器」变成硬约束 | CI 里设 `XTOOLS_REQUIRE_INSTALLER=1`，`package.bat` 缺 ISCC 即 `PACKAGE_FAILED` |
| Linux 编译报 `cannot call member function 'QGuiApplication::nativeInterface()' without object` | `nativeInterface<T>()` 是**成员**函数（模板成员），写成 `QGuiApplication::nativeInterface<T>()` 看着像静态调用；MSVC 与 clang 对这类写法更宽松，只有 GCC 会在 CI 上把它拦下来 | 用 `qGuiApp->nativeInterface<T>()`（或 `qobject_cast<QGuiApplication *>(qApp)`）。别用 `QGuiApplication::instance()`：静态类型是 `QCoreApplication*`，拿不到这个模板成员 |
| 只改了 Linux 分支，本机（Windows）却「编过」了 | 平台分支在另外两个平台上根本不参与编译，本机通过不代表 CI 通过 | 改 `#if defined(Q_OS_LINUX)` 里的代码，交付说明必须写「本机无法验证，等 CI 首次运行」；沙箱禁 WSL/Docker 时没有本地替代方案 |
| `build (macos)` 一步都没跑就 cancelled，annotation 只有 `The configuration 'macos-14-arm64-us-default' is not supported`，日志是空的 | `macos-14` 镜像已于 2026-11-02 从 GitHub Actions 退役：标签还在，但没有机器 | 换 `macos-15`（arm64）。看 `actions/runner-images` 的 Available Images 表确认当前标签，**别用 `-latest`** |
| macOS 包在用户的旧系统上双击没反应，`Info.plist` 却写着 12.0 | 没设 `CMAKE_OSX_DEPLOYMENT_TARGET`，clang 按构建机系统版本当部署目标（runner 是 15/26 就要求 15/26） | 在 `project()` **之前**设 `CMAKE_OSX_DEPLOYMENT_TARGET`；`package-macos.sh` 会打印 `MINOS=`/`PLIST_MIN=` 并在不一致时告警 |
| macOS 任务报 `line 1: ./scripts/build.sh: Permission denied` | 仓库在 Windows 上开发（`core.fileMode=false`），`.sh` 以 `100644` 提交，runner 上就没有可执行位 | 两处都做：`git update-index --chmod=+x scripts/*.sh` 补权限位；CI 里统一用 `bash scripts/x.sh`，不依赖权限位 |
| 打包报 `file SIZE requested of path that is not readable: …/QtSvg.framework/Resources` | macOS framework 内部全是符号链接（`Resources` → `Versions/Current/Resources`），`IS_DIRECTORY` 会跟随链接，把链接当目录去算体积就硬失败 | `_xtools_prune` 里先判 `IS_SYMLINK` 跳过：链接不占体积，删的时候跟着父目录走 |

---

# 第七部分：提交前检查清单

1. `scripts\build.bat`（或 `scripts/build.sh`）跑通，末行 `BUILD_OK`，**零警告**。
2. 改动涉及界面 / qss：用 `--shot` 出图并**真的看过**，随后删除截图。
3. 改动涉及纯逻辑：临时断言程序跑过，贴出通过数量，然后 `rm -rf build/_verify`。
4. 改动涉及平台分支：Win / Linux / macOS 三段都能编译，兜底分支给出可读失败原因。
5. 改动涉及打包 / 更新：本地跑一遍 `scripts\package.bat`，确认 `dist\` 下的产物名与体积基线相符
   （见「裁剪清单与取舍」），并用 `XTOOLS_UPDATE_FEED` 指向本地假清单验一遍
   `--check-update`；改过资产名就先确认 `gen-release-manifest.py` 能把它归到正确的 `kind`。
6. 同步文档（规则 6）：改架构、构建、约定、平台能力 → 改本文件；改用法、支持工具、环境要求 → 改 `README.md`；
   改插件接口 → 改 `docs/PLUGIN.md` 并升 `XTOOLS_API_VERSION`。
7. 编码自查（规则 5 的三条命令），应无输出。
8. `git status --porcelain` 只剩预期文件：无 `build/`、`dist/`、截图、临时工程、`aqtinstall.log`。
9. 交付说明写清「跑了什么、结果如何」，不要只写"应该没问题"。