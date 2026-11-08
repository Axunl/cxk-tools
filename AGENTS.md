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
│   ├── XToolsHelpers.cmake     xtools_api 接口库 + xtools_add_plugin() 插件工厂
│   └── DeployQt.cmake          Windows 构建后把 Qt 运行库部署到 bin/（让 IDE 里能直接跑）
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
│   │   ├── WindowGeometry.h    窗口几何的恢复/保存 + 屏幕外失效保护
│   │   └── Log.*               日志落盘 + 消息处理器
│   ├── ui/                     只向下依赖 core/api
│   │   ├── MainWindow.*        自绘标题栏 + 页头 + QStackedWidget（首页/设置/工具）
│   │   ├── HomePage.*          首页：搜索框 + 「最近使用 / 全部 / 本机应用」磁贴网格
│   │   ├── ToolWindow.*        单个工具的独立窗口（每工具最多一个）
│   │   ├── SettingsPage.*      热键录制、开机自启、插件加载状态
│   │   └── TrayController.*    托盘图标与右键菜单
│   └── app/
│       ├── main.cpp            启动顺序（见文件头注释，改动前先读）
│       └── resources/          style.qss + resources.qrc
├── plugins/
│   ├── jsonfmt/{JsonFmtTool.*, JsonFmtPanel.*, JsonEditor.*, JsonHighlighter.*, JsonText.h}
│   └── calc/{CalcTool.*, ExprEval.h}
├── scripts/{build.bat, build.sh, package.bat}
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

插件目录固定为「可执行文件同级 `/plugins`」，三平台一致（macOS 上是 `xxx.app/Contents/MacOS/plugins`）。

## 打包

```bat
scripts\package.bat          :: 重新构建 -> dist\（exe + Qt 运行库 + plugins\）
```

macOS 用 `macdeployqt`，Linux 把可执行文件与 `plugins/` 放同一目录。`dist/` 与 `build/` 都已被忽略，不要提交。

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

- 默认 `Ctrl+Alt+Space`（`Settings::defaultHotkey()`）。**不要改成 `Alt+Space`**：Windows 把它留给窗口系统菜单，
  `RegisterHotKey` 必定失败。
- Windows：`RegisterHotKey` 注册到 message-only 窗口（`HWND_MESSAGE`），`WndProc` 收 `WM_HOTKEY`。
- Linux：X11 `XGrabKey` 抓根窗口，要连带 `CapsLock`/`NumLock` 的 4 种 lock mask；
  占用检测靠 `XSetErrorHandler` + `XSync` 捕 `BadAccess`；Wayland 拿不到 `Display*`，直接返回可读失败文案。
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

## 日志

- 位置：`QStandardPaths::AppLocalDataLocation` 下的 `logs/app.log`（不可漫游目录，避免企业漫游配置同步日志）。
- 处理器在 `main.cpp` 的 `Log::init()` 装好，之后的 `qInfo/qWarning/qCritical` 同时落盘。
- Windows 大致在 `%LOCALAPPDATA%\cxk\cxk-tools\`；配置在 `AppConfigLocation` 下的 `cxk-tools.ini`。

---

# 第六部分：踩坑表

这些都是本项目实际踩过并已修好的，改代码时别把它们改回去。

| 现象 | 原因 | 正确做法 |
| --- | --- | --- |
| 新写的 `.cpp` 不参与编译，改了半天没效果 | `src/CMakeLists.txt` 的 `APP_SOURCES` 是**显式列表、没有用 GLOB** | 新增/删除源文件必须同步改列表 |
| 界面一片裸控件，qss 完全不生效 | `CMAKE_AUTORCC` 没开，qrc 没被编译 | 保持 `set(CMAKE_AUTORCC ON)`，确认资源路径 `:/style.qss` |
| 配置写不进 ini，或者 `setPath` 没用 | `QSettings` 默认构造在 Windows 上走注册表 | `QSettings(path, IniFormat)` + `setFallbacksEnabled(false)` |
| ini 里出现 `[%General]` | `[General]` 是 INI 保留段名，Qt 会转义 | 组名用 `settings` |
| `QLocalServer` 相关链接失败 | 它属于 Qt Network 模块 | `find_package(... Network)` + 链接 `Qt6::Network` |
| 退出时崩在释放 QString/QWidget | 插件与主程序 CRT 不一致 | 插件统一 `MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"`（已在 `xtools_add_plugin` 里） |
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

---

# 第七部分：提交前检查清单

1. `scripts\build.bat`（或 `scripts/build.sh`）跑通，末行 `BUILD_OK`，**零警告**。
2. 改动涉及界面 / qss：用 `--shot` 出图并**真的看过**，随后删除截图。
3. 改动涉及纯逻辑：临时断言程序跑过，贴出通过数量，然后 `rm -rf build/_verify`。
4. 改动涉及平台分支：Win / Linux / macOS 三段都能编译，兜底分支给出可读失败原因。
5. 同步文档（规则 6）：改架构、构建、约定、平台能力 → 改本文件；改用法、支持工具、环境要求 → 改 `README.md`；
   改插件接口 → 改 `docs/PLUGIN.md` 并升 `XTOOLS_API_VERSION`。
6. 编码自查（规则 5 的三条命令），应无输出。
7. `git status --porcelain` 只剩预期文件：无 `build/`、`dist/`、截图、临时工程、`aqtinstall.log`。
8. 交付说明写清「跑了什么、结果如何」，不要只写"应该没问题"。