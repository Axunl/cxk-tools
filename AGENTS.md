# AGENTS.md

本文件面向在此仓库写代码的 AI 代理与协作开发者。内容全部依据当前源码核对得出，
**改了架构、构建方式、约定或平台能力后，请回来同步本文件**。

---

# 第一部分：七条硬性规则

以下七条对**每一次改动**都生效，不是建议。

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
- **PowerShell 脚本（`*.ps1`）同样要全文 ASCII**。它不是编码例外（UTF-8 无 BOM 是合法的），
  但 Windows PowerShell 5.1 会按本地代码页解析无 BOM 的脚本，中文注释会让后续解析出错
  （见踩坑表 OCR 那条）。`scripts/sign-windows.ps1` 因此连注释都是英文 —— 别"顺手翻译"。
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
  find src plugins cmake scripts docs installer .github -type f \
    \( -name '*.cpp' -o -name '*.h' -o -name '*.txt' -o -name '*.cmake' -o -name '*.qss' \
       -o -name '*.qrc' -o -name '*.md' -o -name '*.sh' -o -name '*.bat' -o -name '*.ps1' \
       -o -name '*.yml' -o -name '*.entitlements' \) \
    | while read f; do iconv -f UTF-8 -t UTF-8 "$f" >/dev/null 2>&1 || echo "非 UTF-8: $f"; done
  # 2) 列出带 BOM 的文件
  grep -rlI $'\xef\xbb\xbf' src plugins cmake scripts docs installer .github
  # 3) 列出 .bat/.cmd/.ps1 里的非 ASCII 行
  LC_ALL=C grep -n '[^ -~]' scripts/*.bat scripts/*.ps1
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
  - 签名 / 公证相关变化（环境变量名、签名顺序、CI secret 名单、是否签名）→ 改 README 的
    「代码签名与公证」与「已知边界」，以及本文件「代码签名与公证」一节，三处描述不能互相打架。
  - 命令行参数、配置文件位置或格式、默认快捷键、交互约定变化 → 改对应章节。
  - 对外有意义的目录结构变化（如插件目录、运行时数据路径）→ 改「目录结构」「运行时数据」。
- README 的定位是**给使用者看的**：只写「怎么装、怎么用、能干什么」。实现细节、踩坑、
  内部约定留在本文件，两边不要互相复制成重复维护。
- 同理：插件接口（`src/api/ITool.h`）变化时同步 `docs/PLUGIN.md` 并升 `XTOOLS_API_VERSION`
  （与第七部分检查清单第 5 条一致）。
- 自检方法：把本次 diff 涉及的章节在 README 里搜一遍，凡是 README 还在描述旧行为的，就是没同步完。

## 规则 7 — 颜色只能走主题令牌

- **qss 里不写死颜色**：`style.qss` 是浅/深共用的模板，颜色一律写成 `@bg` / `@text-faint`
  这样的令牌，运行期由 `Theme` 按当前主题替换。写死 `#FFFFFF` 的后果是切到深色后那块
  仍是白的，而且**不会报错**，只有肉眼能发现。
- **C++ 里不写死颜色**：需要色值时用 `xtools::theme::color(Color::X)`（自绘）或
  `xtools::theme::hex(Color::X)`（拼字符串）；设置控件样式用
  `xtools::theme::applyStyle(widget, "…@token…")`，**不要**直接 `setStyleSheet` 拼色值。
  `applyStyle` 会把模板记在控件的 `xtoolsThemeStyle` 属性上，换肤时宿主遍历所有控件重算，
  插件是独立动态库也能跟着变（这是"切换主题不用重启"的关键，别绕过它）。
- 新增颜色先在 `src/api/ThemePalette.h` 的 `table()` 里加一行（令牌名 + 浅色 + 深色），
  再在 qss/代码里引用。**深色值不是浅色值的反相**：文字要抬亮度、底色要压饱和度，
  选中/危险/成功这些语义色要单独挑，保证在深色底上仍能读清。
- 有意保留字面量的例外要就地写注释说明为什么，例如：二维码预览必须白底（扫描器靠对比度）、
  取色器的棋盘格与"建议文字色"、程序图标徽章配色。
- 换肤后的验证：`--shot x.png --theme dark` 与浅色各截一张，确认没有残留白块；
  想验证"运行时切换"就把 `--theme light` 启动后经设置页切到深色，插件面板也必须整块变色。

---

# 第二部分：项目结构与依赖方向

```
cxk-tools/
├── CMakeLists.txt              顶层：Qt 版本、编译选项、输出布局、add_subdirectory 列表
├── CMakePresets.json           IDE 与命令行共用的 debug / release 配置（含本机 Qt 路径）
├── cmake/
│   ├── XToolsHelpers.cmake     xtools_api 接口库 + xtools_add_plugin() 插件工厂（含 install 规则）
│   ├── DeployQt.cmake          Windows 构建后把 Qt 运行库部署到 bin/（让 IDE 里能直接跑）
│   ├── PruneQtRuntime.cmake    三平台共用的运行库裁剪脚本（cmake -P 调用，逐项删并打印字节数）
│   └── FindOnnxruntime.cmake   手写 Find 模块：找 onnxruntime 头/库/dll，产出 onnxruntime::onnxruntime
├── .editorconfig               编码/行尾兜底（规则 5）
├── src/
│   ├── CMakeLists.txt          主程序目标；APP_SOURCES 是显式文件列表
│   ├── api/ITool.h             唯一的 ABI 边界（纯头文件，不编成库）
│   ├── api/ThemePalette.h      颜色令牌表（浅/深两套色值）+ 内联样式解析，宿主与插件共用
│   ├── api/IToolInput.h        可选的「带内容进入」接口（独立 IID，不改 ITool 虚表）
│   ├── api/ExprEval.h          表达式求值器（纯逻辑）：首页就地求值与计算器插件共用
│   ├── core/                   纯逻辑，不 include 任何 ui/ 头文件
│   │   ├── AppController.cpp   组装层：创建对象、连信号、处理命令行
│   │   ├── Settings.*          配置单例，INI 落盘
│   │   ├── Theme.*             主题：跟随系统/浅色/深色；落地调色板 + qss + 控件内联样式
│   │   ├── ToolRegistry.*      工具条目容器 + 搜索评分
│   │   ├── QueryIntent.h       纯逻辑：识别「JSON / 图片 / 英文」三种搜索意图（可单测）
│   │   ├── QuickCalc.h         纯逻辑：判定「这段输入该不该当算式算」+ 就地求值（可单测）
│   │   ├── LocalAppIndex.*     本机应用索引（开始菜单 / .app / .desktop）+ 启动
│   │   ├── Pinyin.*            汉字→拼音/缩写（纯逻辑，可单测）；拼音表见下一条
│   │   ├── PinyinData.*        生成的读音表（U+4E00–U+9FFF，勿手改，见 gen-pinyin-data.py）
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
│   ├── calc/CalcTool.*         求值器在 src/api/ExprEval.h（与首页就地求值共用）
│   ├── ipquery/{IpQueryTool.*, IpQueryLogic.h}
│   ├── procmon/{ProcMonTool.*, ProcMonPanel.*, ProcProbe.*, ProcText.h}
│   ├── filelock/{FileLockTool.*, FileLockLogic.h, FileLockProbe.*, FileLockPanel.*}
│   ├── translate/{TranslateTool.*, TranslatePanel.*, TranslateLogic.h}
│   ├── colorpicker/{ColorPickerTool.*, ColorPickerPanel.*, ColorText.h, ColorProbe.h}
│   ├── screenshot/{ScreenshotTool.*, ScreenshotPanel.*, CaptureOverlay.*, ScrollSession.*,
│   │              ScreenGrab.*, Stitch.h}
│   ├── ocr/{OcrTool.*, OcrPanel.*, OcrEngine.*, OcrOnnx.*, OcrCapture.*, OcrText.h, OcrPostprocess.h,
│   │        models/（ch_PP-OCRv4 det/rec ONNX + ppocr_keys_v1.txt，随程序分发，勿手改）}
│   ├── cron/{CronTool.*, CronPanel.*, CronExpr.h}
│   ├── jwt/{JwtTool.*, JwtLogic.h}
│   ├── md5/{Md5Tool.*, Md5Logic.h}
│   ├── regex/{RegexTool.*, RegexPanel.*, RegexMatch.h}
│   ├── textkit/{TextKitTool.*, TextKitPanel.*, TextLines.h, TextDiff.h}
│   ├── websocket/{WebSocketTool.*, WebSocketPanel.*, WsClient.*, WsServer.*, WsProtocol.h}
│   ├── httpclient/{HttpClientTool.*, HttpPanel.*, HttpSpec.h, HttpStore.*}
│   │                                 API 调试：参数/请求头/请求体/认证、响应、集合历史、cURL 互转
│   ├── share/{ShareTool.*, SharePanel.*, ShareStore.*, ShareServer.*, ShareHttp.h, ShareWeb.h}
│   ├── qrcode/{QrCodeTool.*, QrCodePanel.*, QrEncode.*, QrDecode.*, QrGalois.h, QrLayout.h, QrTables.h}
│   ├── memes/{MemeTool.*, MemePanel.*, MemeTile.*, MemeGrid.*, MemePreview.*, MemeCache.*, MemeSource.h}
│   ├── finance/{FinanceTool.*, FinancePanel.*, FinanceQuote.h}
│   ├── hotsearch/{HotSearchTool.*, HotSearchPanel.*, HotSearchSource.h}
│   ├── screenrec/{ScreenRecTool.*, ScreenRecPanel.*, RecordService.*, RecorderWorker.*,
│   │              AviMjpegWriter.h, ScreenGrabber.h, CursorPainter.h, FrameQueue.h,
│   │              CountdownOverlay.*, RegionSelectOverlay.*}
│   └── jsrun/{JsRunTool.*, JsEngineRunner.*, JsRunConsole.h, JsRunStore.*, JsRunSamples.h}
│                                 QJSEngine 沙箱执行 JS：worker 线程 + 3 秒超时中断 + console 桥
│                                 （唯一需要 Qt Qml 模块的插件，Windows 另部署 Qt6Qml.dll）
├── assets/
│   ├── app.ico                 Windows 图标（PNG 条目的多尺寸 ICO，被 src/app/app.rc.in 引用）
│   ├── app.icns                macOS 图标（icp4~ic10，打包时拷进 .app/Contents/Resources）
│   └── icon-512.png            源图，改图标时从它重新导出上面两个
├── installer/
│   ├── windows/cxk-tools.iss   Inno Setup 6 脚本（AppId 固定，UTF-8 无 BOM）
│   └── windows/lang/ChineseSimplified.isl   第三方简体中文向导翻译，原样入库
├── scripts/{build.bat, build.sh, package.bat, package-macos.sh, gen-release-manifest.py,
│            gen-pinyin-data.py（生成 src/core/PinyinData.cpp，需要 pypinyin）,
│            fetch-onnxruntime.py（从 NuGet 下载解包 onnxruntime 到 build/_third_party）}
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
- 能写成不依赖 `QWidget` 的纯函数就写成纯函数（`src/api/ExprEval.h`、`plugins/jsonfmt/JsonText.h` 就是范例），
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
   搜索建议条（`IntentBanner`）是内容区里的普通控件，同样**不要**做成浮层或弹窗。
5. 工具面板**不缓存**：每次进入工具都 `createPanel()` 新建面板；离开视图（回首页、进设置、
   收起窗口）即销毁面板（`destroyToolPanel()`），输入值随之清空。同一工具可以同时存在
   「内嵌面板」与「独立窗口面板」两份，插件不得假设自己是唯一实例。
   例外是「弹独立窗口」：那是把内嵌面板**搬**过去（`takeCurrentToolPanel()`），不新建第二份。
6. 使用频次与「最近使用」只在用户主动打开工具/应用时记账（`markUsed()` / `launchLocalApp()`），
   启动与唤出不算。工具与应用共用同一份最近使用（上限 8），按时间戳混排。
7. 独立窗口（`ToolWindow`）每个工具最多一个，重复打开只 `raise + activateWindow`；
   主窗口收起时独立窗口继续存活；退出时由 `prepareQuit()` 统一关闭 —— **不能放在
   `closeEvent` 里，`qApp->quit()` 不会给窗口发 closeEvent**。
   从工具页弹出（页头「独立窗口」/ 双击标题栏）走 `popOutTool()`：当前内嵌面板整块移交给
   `ToolWindow`（同一个 QWidget 实例，数据自然跟着走），主窗口随即 `showHome()`。
   已有独立窗口时只前台化，主窗口那份原样保留。
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
scripts\package.bat            :: Windows：构建 -> dist\windows-x64\ -> 签名 -> zip -> 安装器 -> 签安装器
```

```sh
scripts/package-macos.sh       # macOS：构建 -> cxk-tools.app -> strip/prune -> 签名 -> 公证 -> dmg
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

### 代码签名与公证

**签名是可选的，没配证书时行为与以前完全一致**（打一行 `[warn]`，继续出未签名包）。开关只有
环境变量，脚本里不读注册表、不写死证书路径。

| 变量 | 平台 | 说明 |
| --- | --- | --- |
| `XTOOLS_SIGN_PFX` / `XTOOLS_SIGN_PFX_BASE64` + `XTOOLS_SIGN_PFX_PASSWORD` | Windows | PFX 路径或 base64（GitHub secret 只能存文本），密码允许为空 |
| `XTOOLS_SIGN_THUMBPRINT` | Windows | 证书已在 `Cert:\CurrentUser\My` 时直接按指纹签（EV 硬件令牌只能走这条） |
| `XTOOLS_SIGNTOOL` | Windows | 显式指定 signtool.exe；默认在 Windows SDK 的 `bin\<ver>\x64` 里取最新版 |
| `XTOOLS_TIMESTAMP_URL` | Windows | RFC3161 时间戳，默认 `http://timestamp.digicert.com` |
| `XTOOLS_MACOS_SIGN_IDENTITY` | macOS | `Developer ID Application: 名字 (TEAMID)`，启用真签名 + `--options runtime` |
| `XTOOLS_MACOS_ENTITLEMENTS` | macOS | 授权文件，默认 `installer/macos/cxk-tools.entitlements` |
| `XTOOLS_NOTARY_PROFILE`，或 `XTOOLS_APPLE_ID` + `XTOOLS_APPLE_APP_PASSWORD` + `XTOOLS_APPLE_TEAM_ID`，或 `XTOOLS_API_KEY_PATH` + `XTOOLS_API_KEY_ID` + `XTOOLS_API_ISSUER` | macOS | 公证凭据三选一（`package-macos.sh` 的 `NOTARY_ARGS`） |
| `XTOOLS_REQUIRE_SIGNING=1` / `XTOOLS_REQUIRE_NOTARIZATION=1` | 两端 | 没配就失败。CI 上按「secret 是否存在」算出值，本地不设 |

- **Windows 的签名逻辑在 `scripts/sign-windows.ps1`，退出码是契约**：`0` 签好且校验通过、
  `2` 没配证书（本地，可继续）、`1` 配了但坏了（PFX 错、signtool 失败、`verify` 失败）。
  `1` 一律让 `package.bat` 失败。**不要改成「出错也继续」**：悄悄发出未签名包正是这一步要防的。
- **签在压缩之前**（`package.bat` 里紧跟 prune）：签名会改写 PE 头与校验和，压完再签会让 zip
  里的副本仍是未签名的，并且 `latest.json` 的 SHA256 与用户下载到的文件对不上（更新器会直接
  删包报「校验不通过」）。安装器另在 Inno 产出后签一次。
- **只签自己的二进制**：`cxk-tools.exe`、`plugins\*.dll`、`setup.exe`。Qt / MSVC / onnxruntime
  的 DLL 保留厂商签名，重签等于冒名。
- **不用 Inno 的 `SignTool` / `SignedUninstaller`**：那条命令必须携带证书参数（密码会进进程
  列表），且 `/S<name>="含空格的命令"` 在 cmd 里的引号处理属于踩坑表里反复出现的那类坑。
  代价是安装时生成的 `unins000.exe` 未签名，卸载的 UAC 显示「未知发布者」——这个取舍写在
  README「已知边界」里，改之前先说清楚。
- **`sign-windows.ps1` 必须全文 ASCII（注释也是）**：Windows PowerShell 5.1 按本地代码页解析
  无 BOM 的 UTF-8 脚本，中文注释会连带破坏后面的解析（与 `.bat` 同源，见踩坑表 OCR 那条）。
- **macOS 由内向外签**：框架内 Mach-O → 其余 Mach-O → 主程序（带 entitlements）→ `.app` →
  `.dmg`，之后 `codesign --verify --deep --strict` 必须通过。**不要用 `--deep` 去签**：它按目录
  深度猜顺序，对嵌套 helper 不可靠，Apple 也明确说它不用于分发签名。
- **公证顺序**：`.app` 打 zip 提交 → `stapler staple` 进 bundle → 生成 dmg → 签 dmg → 再提交并
  装订。少一次 `staple`，用户第一次离线打开仍会被 Gatekeeper 拦（票据只在服务端，联网才查得到）。
- **授权文件只有 `com.apple.security.cs.disable-library-validation`**：插件体系允许用户把第三方
  `.dylib` 丢进 `Contents/MacOS/plugins`，开着库校验会被 dyld 直接拒绝（表现为插件静默不加载）。
  不要顺手加 `allow-jit`——纯 Widgets 应用没有 QML。改授权文件要重新公证，先确认再动。
- CI 的 secret 名单（`release.yml`，缺任意一个都不阻塞发布，只出一条 `::warning`）：
  `WINDOWS_SIGN_PFX_BASE64`、`WINDOWS_SIGN_PFX_PASSWORD`、`MACOS_SIGN_P12_BASE64`、
  `MACOS_SIGN_P12_PASSWORD`、`APPLE_ID`、`APPLE_APP_PASSWORD`、`APPLE_TEAM_ID`。
  macOS 侧必须建独立钥匙串并 `set-key-partition-list`（少了它 codesign 会卡在
  "User interaction is not allowed"），任务结束用 `if: always()` 删钥匙串。
- 本机自测签名链路：造一张自签证书，`XTOOLS_SIGN_THUMBPRINT=<指纹>` 跑 `scripts\package.bat`，
  出现 `[sign] done` 即链路通。`signtool verify /pa` 会因「根证书不受信任」失败，这是自签的
  必然结果，不是脚本错；测完 `Remove-Item Cert:\CurrentUser\My\<指纹>` 清掉。

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
| `translations/` `qml/` `generic/` | ≈2 MB | `--no-translations` + prune 兜底 | 界面文案自备；`generic` 是 TUIO 触摸协议；`qml/` 只是 import 模块，**`Qt6Qml.dll` / `QtQml.framework` 必须保留**（`jsrun` 的 QJSEngine 在里面），删了它插件加载失败、工具静默消失 |
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

**下载链路的四条硬约束**（2026-11 修「更新老是失败」时定下的，别拆）：

1. **必须跟随系统代理**。`UpdateChecker` 构造时对**自己这个** `QNetworkAccessManager` 调
   `systemProxyForQuery()`，不要改成全局 `QNetworkProxyFactory::setUseSystemConfiguration(true)`
   ——那会顺带改掉所有插件的网络行为。反例就是这次的真实故障：用户开着 `127.0.0.1:7890`
   的代理，浏览器能开 GitHub，程序直连，于是每次下载都卡死；日志里表现为
   `下载失败：Operation canceled`，看不出跟代理有关。
2. **Qt 的传输超时 = `OperationCanceledError` + `"Operation canceled"`**。这个字符串绝不能直接给用户
   （`describeNetworkError()` 必须逐码翻成中文），否则"网络卡住了"会显示成一句谁也看不懂的英文。
   注意 `setTransferTimeout` 是**空闲**口径（多久没新数据才掐），不是总时长；实测 1 秒超时拦不住
   一个数据一直在流、总时长 3.5 秒的下载。
3. **断点续传优先于重下**：安装包先写 `xxx.part`，断流就带 `Range` 续传（最多 5 次），校验通过才改名。
   服务器不认 Range 会回 `200` + 整包，此时必须 `resize(0)` + 重置 hash，否则拼出来的是
   "前段旧、后段新"的坏包，SHA256 永远不过。
4. **写盘失败与网络失败要分开**：写盘失败会 `abort()`，错误码被掩盖成 `Operation canceled`，
   所以用 `m_localError` 单独记账并优先上报；写失败的 `.part` 不能留（磁盘上的字节和 hash 已经不一致）。

同一个坑在 `plugins/` 里联网的插件上同样存在，改网络代码时一并检查。

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

四个必须记住的坑（都实际踩过）：

1. 直接 `call vcvars64.bat` 在受限环境里探不到 Windows SDK（`reg.exe` 被策略拦），`cl` 报找不到头文件
   —— 必须带 `build.bat` 里的 SDK 兜底段。
2. 只 `find_package(Qt6 COMPONENTS Core)` 会因为 `QKeySequence` 缺 `Qt6::Gui` 编译失败。
3. 测试程序缺 Qt DLL 时**不报错、直接退出且无输出**，运行前把 Qt 的 `bin` 加进 `PATH`。
4. 有 Qt DLL 但 `qInfo()` / `qWarning()` 仍然**一个字都不输出**（退出码却是 0）：
   Windows 上进程没有控制台窗口时（从 Git Bash 用管道跑就是这样），Qt 把日志改走
   `OutputDebugString`。加 `QT_ASSUME_STDERR_HAS_CONSOLE=1` 就能重新落到 stderr。

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
- 想接首页带进来的内容（JSON / 图片 / 待翻译文本），额外实现 `IToolInput`（独立 IID，
  不影响 `XTOOLS_API_VERSION`）。宿主在 `createPanel()` 之后立刻调 `setToolInput()`，
  所以插件要自己记住刚创建的面板（`QPointer`），不适用的类型静默忽略。见 `docs/PLUGIN.md` 第 4 节。

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
| 拼音/缩写（`ewm` → 二维码，`weima` → 二维码） | 36~48 |
| 描述包含 | 30 |

- 命中后再加 `min(usage, 20)` 频次加成，**封顶 20，压不过一级匹配**。
- 拼音梯度（`ToolRegistry.cpp` 的 `pinyinScore`，只对纯拉丁查询生效，字面没命中才试）：
  缩写相等 48 / 全拼相等 46 / 缩写前缀 44 / 全拼前缀 42 / 全拼包含 36；次读音整体 −2。
  名称与 `keywords` 都参与（插件常把中文名写在 keywords 里）。
- 空查询按 `1000 - min(usage, 500)` 排序（= 常用工具靠前），上限防止某个工具永远霸榜。
- 同分按名称 `localeAwareCompare` 排序，保证结果稳定（否则会随插件加载顺序抖动）。

## 首页搜索建议（`core/QueryIntent.h` + `IToolInput`）

搜索框除了关键词匹配，还会识别三类「意图」，在结果最上方顶出一条可整行点击的建议条
（`IntentBanner`），回车 / 点击即把内容带进对应工具：

| 触发条件 | 目标工具 | 带进去的内容 |
| --- | --- | --- |
| 输入能解析成 JSON 对象 / 数组（`{...}` `[...]`，≤64 KB） | `jsonfmt` | `ToolInput::Kind::Text`，面板里直接排版 |
| 剪贴板里是图片时按下 `Ctrl+V` / `Shift+Insert` | `ocr` | `ToolInput::Kind::Image`，面板里立刻识别 |
| 工具与本机应用**都没有命中**且输入是英文 | `translate` | `ToolInput::Kind::Text`，面板里立刻翻译 |

- 判定顺序：图片 > JSON > 翻译。词里只要出现非拉丁字母（含中文、西里尔、代理对字符）
  就不算英文；裸标量（`123`、`"abc"`、`true`）不算 JSON —— 它们与普通搜索词无法区分。
- **建议优先于第一个搜索结果**：`m_currentIndex < 0`（还没按方向键挑磁贴）时回车走建议，
  按过方向键就按选中的磁贴走。这条要保住，否则用户回车会莫名打开别的东西。
- 目标插件没加载时**不显示建议**（去掉 `m_intentData`），宁可什么都不给也不要摆一条回车没反应的建议。
- 粘贴图片会清空搜索框里的文字；之后用户再打字会自动丢掉待处理图片（文字优先）。
  `✕`、`Esc`、热键呼出、`--search` 都会同时清掉图片。
- 内容交接：`HomePage::toolActivatedWithInput` → `MainWindow::showTool(id, true, input)` →
  `createToolPanel()` 里 `qobject_cast<IToolInput *>(dynamic_cast<QObject *>(entry->tool))`。
  `ITool` 不是 QObject，必须先 dynamic_cast；插件没实现该接口是合法状态，只记一条日志。
- 纯逻辑（`looksLikeJson` / `isEnglishText` / `forText` / `forImage`）在 `QueryIntent.h` 里，
  不依赖 QWidget，改判定规则时按规则 2 用临时断言程序验证。

## 首页就地求值（`core/QuickCalc.h` + `api/ExprEval.h`）

输入 `12*8` 时结果以「= 96」的小徽标贴在**搜索框内部**右侧（不弹面板、不占网格、不抢焦点），
回车或点徽标把结果复制到剪贴板，并短暂显示「已复制」。实现要点：

- **判定与求值是两件事**：`quickcalc::looksLikeExpression()` 先挡掉绝大多数关键词
  （要求含数字 + 含二元运算符/括号；开头的 `+/−` 是一元符号，不算），再交给
  `calc::ExprEval` 求值。别把这两步合并成「解析失败就当作不是算式」—— 每敲一个键都
  对 `"3d"`、`"json"` 跑一遍完整解析器没有必要。
- **求值器只有一份**：`src/api/ExprEval.h`，计算器插件与首页共用。改求值规则必须同时
  跑两边的验证，否则会出现「首页算出 96、面板算出别的」。
- 徽标是 `QLineEdit` 的子控件，几何（含宽度、`✕` 位置、文字右边距）全部由
  `HomePage::positionSearchOverlays()` 按像素手算。**不要给 `#homeSearchResult` 加
  qss 的 `padding` / `font-size`**：宽度是 C++ 算的，qss 一改就会裁字。
- 回车优先级：就地求值（`m_currentIndex < 0` 时）> 建议条 > 磁贴。求值命中时会把
  `hasToolHits` 置真再传给 `updateIntent()`，否则 `pi*2` 这种输入会同时冒出「翻译」建议。
- 有求值结果时不显示「没有找到匹配的工具或应用」：答案就在框里，再喊一句没找到只会让人困惑。

## 本机应用搜索（`LocalAppIndex::search`）

| 命中方式 | 分值 |
| --- | --- |
| 名称完全相等 | 100 |
| 名称前缀 | 90 |
| 逐词前缀（`studio` → Visual Studio Code） | 80 |
| 名称包含 | 70 |
| 拼音缩写完全相等（`wx` → 微信） | 68 |
| 拼音全拼完全相等（`weixin`） | 66 |
| 首字母缩写（`vsc`） | 65 |
| 拼音缩写前缀（`wxd` → 微信读书） | 63 |
| 拼音全拼前缀（`weix`） | 61 |
| 可执行文件名包含 | 50 |
| 拼音全拼包含（`yinyue` → QQ音乐） | 45 |

- 同样加 `min(usage, 20)`；**空查询直接返回空** —— 应用不铺满首页，只出现在搜索结果
  与「最近使用」里，否则上百个开始菜单项会把网格淹掉。
- 名称用 `toCaseFolded()` 比较，中文原样参与；字面没命中时才比拼音（`core/Pinyin.cpp`）：
  拼音分值整体压在字面匹配之下、可执行文件名之上，**多音字**的次读音再 −2 分。
- 拼音串在 `addEntry()` 时算好存进 `Entry`（`initials` / `pinyin` / `altInitials` / `altPinyin`），
  **不要在 `search()` 里现算** —— 每敲一个键都要扫全表，现算会明显拖慢输入。
- 拼音是「按字」取音，不认词组：多音字只有主读音 + 一个次读音（`音乐` 的 `乐` 有 `le`/`yue`，
  但两个字都多音时只能换一个）。CJK 扩展区（U+3400 起）与生僻字不在表内，只按字面命中。
- 拼音表 `src/core/PinyinData.cpp` 是**生成产物**（`scripts/gen-pinyin-data.py`，数据源 pypinyin），
  约 100 KB、覆盖 U+4E00–U+9FFF 每个码位。要改读音规则就改脚本重跑，别手改表体；
  首字母/全拼的切词规则在 `Pinyin.cpp`，那边才是手写逻辑。
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
- **「注册成功」不等于「按键到得了」**：`RegisterHotKey` 只表示系统占位成功，按键还要先过一遍
  `WH_KEYBOARD_LL` 钩子链才进入热键匹配。链上任何一环（远程控制 ToDesk、剪贴板/截图工具、
  键盘增强、部分输入法、外设驱动）吃掉这次按键，就永远等不到 `WM_HOTKEY`，日志里却还是
  「已注册」。典型指纹：用 `SendInput` 合成的按键能正常派发 `WM_HOTKEY`，物理按键没反应 ——
  因为这些钩子通常只对真实输入生效、放过 `LLKHF_INJECTED` 事件。
- 所以 Windows 侧多挂一个 `WH_KEYBOARD_LL` 兜底（`installKeyboardHook()`）：看到目标组合键就
  延迟 `kHookFallbackDelayMs`（150ms）再判断，这段时间内 `WM_HOTKEY` 到了就什么都不做，
  避免一次按键唤起两次。设计约束：
  - 只在 `RegisterHotKey` **成功**之后才挂：组合键真被别的程序占走时仍如实报「已被占用」，
    不去抢别人的热键。
  - 钩子回调运行在 GUI 线程，但挡在整机输入通路上，**只做比对 + 时间戳 + 起定时器**，
    绝不做重活；并且永远 `CallNextHookEx` 放行，吞键会让所有程序（含输入法）都收不到。
  - 不按 `LLKHF_INJECTED` 过滤：远程桌面/自动化注入的按键也该能唤起窗口。
  - `kHookRepeatGuardMs`（400ms）抑制长按自动重复；换绑时 `unregister()` 会把钩子匹配条件清零。
  - 兜底命中会写一行「WM_HOTKEY 未到达，由键盘钩子兜底唤起」，下次再报热键失灵先搜这行。
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
- 标题栏：按住拖动 = `startSystemMove()`；**双击 = 把当前工具连同内容弹成独立窗口**
  （`MainWindow::popOutCurrentTool()`，首页/设置页静默忽略）。双击判定有两条路：
  ① 标准 `MouseButtonDblClick`；② 自己在 `MouseButtonPress` 里按「时间差 ≤
  `QApplication::doubleClickInterval()` + 位移 ≤ 8px」再判一次 —— 第一次按下已经把
  拖动交给系统移动循环（Windows 会接管鼠标），第二次按下有可能只是普通 Press。
- 页面是三层栈：`HomePage`（首页网格，页头隐藏）/ `SettingsPage` / 动态工具面板。
  页头在工具页与设置页显示，含「← 首页」、标题/描述、「独立窗口」（设置页隐藏）。
- 首页网格：磁贴固定 `104×100`（图标 48），列数 = `(视口宽 - 2*28 - 12) / (104 + 8)`。
  **恒定扣掉 12px 滚动条预留**，见踩坑表。键盘：`←→` 移一格、`↑↓` 移一行、`Enter` 开当前项。
- 搜索时工具与本机应用分两组（「工具」「本机应用」），磁贴顺序 = 键盘导航顺序：
  最近使用 → 工具结果 → 应用结果。空关键词时应用不展示（只可能出现在「最近使用」里）。
- 独立窗口 `ToolWindow`：原生边框（不加 `FramelessWindowHint`、不用 `Qt::Tool`），
  `WA_DeleteOnClose`，面板随窗口销毁；去重表 `QHash<QString, QPointer<ToolWindow>>` 在 `MainWindow`。
  构造函数第二参是「接管的面板」：非空就用它（从主窗口搬过来的那份），空才自己 `createPanel()`。
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
- 全局 `QPushButton` 带 `padding: 6px 14px`，**小按钮统一打 `compact=true` 属性**（
  `QPushButton[compact="true"] { min-height: 0px }`），不要再各写一遍 `padding:0; min-height:0`。
  收敛边界：`compact` 只清 `min-height`（14 个按钮唯一共性）；`padding` 各按钮不统一、仍留在
  各自 objectName 选择器里；`:checked`/`:hover` 的状态色也绝不吞进 compact。
- **圆角三档**：卡片 10 / 内嵌 8 / 小控件 6。装饰性圆形（状态点、进度条）不并入三档。
  **字号刻度**：11 / 12 / 13 / 15 / 17 五档（`#procPercent` 的 30px 大数字是特例保留），
  新控件别再造出 10 / 14 / 16 这种中间档。
- **深色卡片用 `@card` 令牌**（不是 `@bg-alt`）：`@bg-alt` 是「下沉表面」（只读字段、表格头、
  禁用态），深色下提亮会连它们一起抬升、语义颠倒。`@card` 专做卡片抬升（深色比页面稍亮），
  卡片选择器（`QGroupBox`、`#procCard`、`#flCard`、`#cpCard`、`#fsCard`、`#procTable`）用它。
- **页面级左右留白用 `xtools::layout::kPageMargin`**（`src/api/LayoutMetrics.h`），不要在各文件
  再写死 `28` —— 之前 MainWindow / HomePage / 24 个插件各写一份，改起来必然漏。
- **按钮焦点态**：全局 `QPushButton:focus` 用 `@accent` 边框表达；`accent` 按钮用 `@accent-hover`
  底色；无边框按钮（标题栏三个）只能用 `@bg-subtle` 底色（`border:none` 画不出边框）。
- **主窗口投影**：无边框窗口靠 `QGraphicsDropShadowEffect` + 透明边距容器（`#mainShadowHost`）实现，
  颜色走 `@shadow` 令牌、透明度在 `MainWindow::shadowColor()` 里按主题调；换肤时 `Theme::themeChanged`
  触发刷新。投影宿主是 centralWidget，`QSizeGrip` 挂宿主上、`resizeEvent` 定位要扣掉投影边距，
  见踩坑表。
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

## 进程监控（`plugins/procmon`，插件内部）

- 分三层：`ProcProbe`（唯一碰平台 API 的地方，只暴露 Qt 值类型）、
  `ProcText.h`（纯逻辑：单位换算、占用率、**采样差值算 CPU/磁盘速率**、筛选排序、回收候选，
  全部 inline）、`ProcMonPanel`（界面）。纯逻辑抽成 header 是为了能在临时工程里直接断言，
  不必真的去刷别人的工作集。
- **探针无状态、只给累计值**：CPU 用累计 CPU 时间、磁盘用累计读写字节，占用率由
  `proctext::computeRates(上次采样, 本次采样, 间隔毫秒, 核数)` 做差算出。
  好处是同一份数据可以既显示又排序，坏处是**第一次采样没有基准，CPU/磁盘显示 `—`**（别改成 0%，
  那会让人以为进程没在跑）。间隔 < 500ms 时不算速率：释放内存后会立刻 refresh，那种 20ms 的差值全是噪音。
- CPU 口径是**占整机**（差值 ÷ 墙钟 ÷ 核数），与任务管理器一致；单核吃满在 8 核机上显示 12.5%。
  计数器变小（进程重启、macOS 32 位网卡计数器回绕）一律按 0 算，不能让差值回绕成天文数字。
- **按端口搜索靠"监听端口"表**：Windows 走 `GetExtendedTcpTable(OWNER_PID_LISTENER)` +
  `GetExtendedUdpTable`（只有内核 owner 表里才有 pid）；Linux 先建 `/proc/net/{tcp,tcp6,udp,udp6}`
  的 inode→端口表，再扫 `/proc/<pid>/fd` 的 `socket:[inode]` 反查；macOS 用 `/usr/sbin/lsof -nP -iTCP -sTCP:LISTEN -iUDP`。
  只收 **LISTEN / 已绑定的 UDP**：已建立连接会带出几十个随机临时端口，把搜索淹掉。端口 0 是占位条目，必须过滤。
- Windows 侧三个原生库：`psapi`（内存/回收）、`iphlpapi`（端口表 + `GetIfTable2`）、`shell32`（`CommandLineToArgvW`）。
  **`ws2ipdef.h` 必须在 `iphlpapi.h` 之前引** —— `netioapi.h` 里 `MIB_IF_TABLE2` / `GetIfTable2`
  整段包在 `#ifdef _WS2IPDEF_` 里，而 `iphlpapi.h` 自己不引它，漏了只会报"未知类型"，看不出是头依赖缺失。
- **重启进程 = 读命令行 → 结束 → 用同样的 program/args/cwd 重新拉起**。读不到命令行就直接报错，
  绝不"按空参数重启"（带参数的进程会变成空壳，比不重启更让人困惑）。Windows 读命令行是读 PEB：
  `NtQueryInformationProcess(ProcessBasicInformation)` → PEB 偏移 `0x20`(x64)/`0x10`(x86) 取
  `ProcessParameters` → `CommandLine` 在 `0x70`/`0x40`、`CurrentDirectory` 在 `0x38`/`0x24`。
  这套偏移没有公开头文件，**只在同位数进程之间读**（`IsWow64Process` 两边比一次），跨位数直接放弃。
  `argv[0]` 常是裸名（"ping"），能拿到真实 exe 路径就换成绝对路径，免得重启时按 PATH 找错版本。
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
  由 `ProcMonPanel::eventFilter` 在 Resize / EnabledChange 时重摆与改色。qss 无法可靠画这个箭头，
  原因见下面踩坑表；别再往回去改 `::down-arrow`。
- 自动释放的开关与累计释放量都是**面板实例状态**，没有落盘：宿主不缓存面板，离开工具即重置，
  与「面板不缓存」的既有约定一致（也避免两份面板互相串状态）。开关**默认开启**、阈值默认「占用 ≥ 80%」：
  下拉里存的是"占用阈值"（用户直觉），判定函数吃"可用阈值"，由 `proctext::availableThreshold()` 换算一次。
- **网络列固定显示 `—`**，这是刻意的：按进程统计网络流量三大系统都没有免管理权的公开接口
  （Windows 要 ETW + 管理员，Linux 内核只按网络命名空间记账）。整机上下行速率给在概览卡里，
  单元格与表头的悬浮提示都写明了原因 —— 别为了"看起来完整"编一个数字出来。
- 表格可点表头换排序列（`proctext::SortKey`），网络列没有数字所以点它不响应；
  指标列把读不到数值的进程沉底，否则一堆 `—` 会夹在中间像排序坏了。

## 翻译（`plugins/translate`，插件内部）

- 分三层：`TranslateLogic.h`（纯逻辑：语言表、源语言检测、请求构造、四家响应解析、分段/拼接）、
  `TranslatePanel`（界面 + 异步调度）、`TranslateTool`（插件入口）。纯逻辑做成 header 是为了
  临时断言程序能直接 include；面板也放头文件，临时工程开 AUTOMOC 就能做端到端断言。
- 源语言默认「自动检测语言」，**目标语言默认英语**（按展示名 `findText` 定位，别硬编码语言表
  下标，顺序调整后默认值会悄悄漂走）；自动翻译默认开启，但初始空输入不会发请求。
- 目标语言有两种默认值，靠「有没有走 `setInputText()`」区分：主动点磁贴打开不调它，保持
  默认英语；首页推荐（识别到英文后带内容进来）会调 `setInputText()`，此时先把目标语言
  `findText("中文")` 切到中文再翻译——推荐进来的语义是「把英文翻成中文」。
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
  两个坑：① `OCR_NORMAL` 等常量在 WinUser.h 里被 `#ifdef OEMRESOURCE` 保护，而 OEMRESOURCE 在
  `WIN32_LEAN_AND_MEAN` 下不自动定义，需在 include windows.h 前显式 `#define OEMRESOURCE`；
  ② `SetSystemCursor` 会 destroy 传入句柄，`LoadCursor` 返回的共享句柄不能直接传，必须
  `CopyImage(..., IMAGE_CURSOR, 0, 0, 0)`（flags=0 复制副本、不删原图）先复制一份。
- **恢复光标绝对不能用「备份原句柄再 SetSystemCursor 写回」**（2026-11-10 实修）：SetSystemCursor
  改的是**系统级持久设置**，和进程生命周期无关。① 进程没走到 release()（宿主关工具、退出应用、
  插件卸载、被 taskkill）时 OCR_NORMAL 永远是十字，整个桌面鼠标都变成 +，只能重启或外部复位；
  ② 一旦发生过，下一次 `LoadCursor(nullptr, IDC_ARROW)` 备份到的「标准箭头」其实已经是十字，
  release() 写回去等于把十字写回系统——表现为「取完色鼠标一直是十字」。
  正确做法是 `SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE)`：让系统按
  注册表**整体重载**光标方案（用户自定义光标也原样重载），不依赖备份、能自愈任意残留。
  同时铺三道复位：`release()`（Windows 上**不按 s_active 挡门**，顺手清掉上次强杀的残留）、
  `~ColorPickerPanel()`（关工具/卸载）、`qApp->aboutToQuit`（退出应用），外加构造时复位一次做自愈。
  另注：`GetCursorInfo()` 不跟踪 OCR_NORMAL 的替换，别拿它当验证手段（实测始终返回桌面窗口
  自己的箭头句柄）；验证只能看 SPI 调用返回值 + 肉眼观察。
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

- 分层：`OcrText.h` / `OcrPostprocess.h`（纯逻辑，可临时断言）、`OcrOnnx.*`（PP-OCR 推理，
  PIMPL 藏 Ort:: 类型）、`OcrEngine.*`（唯一碰平台差异的地方）、`OcrCapture.*`（纯 Qt 全屏框选
  浮层）、`OcrPanel.*`（界面）。分层理由同 procmon/colorpicker：纯逻辑抽成 header 才能在临时
  工程里直接 include 验证。
- **首选后端是 PP-OCRv4（ONNX Runtime）**，三端统一、离线、识别率最高；模型缺失/加载失败/推理
  失败时 `recognize()` 用 `error == "__FALLBACK__"` 占位回退到平台原生引擎。三个后端最终都
  归一化成 `ocrtext::Word`（带坐标的词），排版（归行、拼接、分列）只有一份实现。
- PP-OCR 管线：QImage 读图 → det（DBNet 出概率图）→ `OcrPostprocess::detBoxes` 阈值/8 邻域连通域/
  外接矩形/unclip 膨胀 → 逐框 crop → rec（SVTR_LCNet 出 CTC）→ `ctcDecode` → 组 Word。det 与 rec
  共用一个 `OcrOnnxEngine`，Session 只建一次（复用，避免重复加载模型）。
- `OcrOnnx.cpp` 是**唯一 include onnxruntime 头文件**的地方（PIMPL 把 `Ort::Session` 藏进 Impl），
  `OcrOnnx.h` 只暴露 Qt 值类型；未定义 `XTOOLS_HAVE_ONNX` 时整个文件退化成「`isAvailable()` 恒
  false 的桩」，三端都能编译。onnxruntime 是可选依赖：`-DOnnxruntime_ROOT=<解包目录>` 启用
  （FindOnnxruntime.cmake 找不到不报错），发布构建必须显式给（否则打出的包没有 PP-OCR）。
  运行库与模型都要随程序分发：onnxruntime.dll + onnxruntime_providers_shared.dll 拷到 `bin/`，
  模型（det 4.7MB + rec 10.8MB + keys 26KB）在 `plugins/ocr/models/` 由 `cmake --install` 落位。
- **det 后处理三要素（踩坑结论，别改回）**：① det 输入必须 pad 到 32 的倍数，否则 Add 节点广播
  失败「25 by 26」；② DB-Net 概率图是「收缩后的文字核心」，必须按 `offset = area × ratio / perimeter`
  做 unclip 膨胀（否则框只有真实高度 40%，rec 切掉上下边缘、认错字）；③ 概率图文字区域与 pad 前
  缩放图 1:1 对齐，**坐标不做缩放**（按 origW/mapW 缩放会整体缩小、切字），只 clamp 到缩放图尺寸。
  连通域必须用 8 邻域（scipy ndimage.label 默认），4 邻域会把「体/衡」这类对角笔画切成多个框。
- **rec 字典映射**：输出 6625 类，只取前 6624（keys 6622 + blank 索引 0 + 空格索引 6623 + 1 对齐
  padding 忽略）。`ctcDecode` 顺序是「逐帧 argmax → 先去连续重复 → 再去 blank → keys 映射」，
  不能反过来（否则 `a - a` 会错成 `aa` 而非 `a`）。
- Windows 兜底用系统自带 PowerShell 调 `Windows.Media.Ocr`（WinRT），**不写 C++/WinRT**：
  那样要 windowsapp.lib / winmd 投影头 / 异步初始化，会把三端构建拖进平台依赖。
  macOS 与 Linux 兜底走外部 `tesseract`（先试 `eng+chi_sim`，失败降级默认语言），缺引擎返回可读原因。
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

## 截图（`plugins/screenshot`，插件内部）

- 分五层：`ScreenGrab.*`（抓屏与裁剪，只走 `QScreen::grabWindow`）、`Stitch.h`（纯逻辑：位移检测
  + 纵向画布，header-only，可直接在临时工程里断言）、`CaptureOverlay.*`（覆盖整块虚拟桌面的框选
  遮罩，回调式、无 `Q_OBJECT`）、`ScrollSession.*`（抓帧循环 + 不抢焦点的控制条）、
  `ScreenshotPanel.*`（界面）。
- **滚动截图不注入滚轮事件**：`SendInput` / `XTestFakeButtonEvent` / `CGEventCreateScrollWheelEvent`
  是三套原生代码，且 macOS 要辅助功能授权、Wayland 根本做不到（规则 3：没有实机就不要写平台原生
  代码）。改成「用户自己滚、程序按重叠区域算位移」，三端一份实现。将来若要做自动滚动，三端都得有。
- 高 DPI 是这里最容易错的地方：`grabWindow()` 返回的缩放比各平台并不统一，一律用「返回图宽 /
  请求逻辑宽」反推 DPR，并把每一帧归一成「设备像素 + dpr=1」再比较与拼接，只在 `crop()` / 出图
  那一刻把 dpr 设回去。少了归一化就会出现「某一帧被隐式缩放成 1/2」的错位。
- 拼接的正确性靠**绝对偏移**而不是「每帧往尾部追加」：`VerticalCanvas` 记录最新帧顶端在画布里的
  y，只有画布范围真的越界才取新行。这是「来回滚不会重复贴」的唯一原因，改这里要想清楚。
- 位移检测的已知退化：内容高度重复时多个 dy 同分，取 |dy| 最小的那个，长图可能重复；
  一帧内滚动超过选区高度的 80% 会匹配失败（当作「没动」）。
- 会话有三个结束条件（点完成 / 停手 2.5 秒 / 长图到 20000 像素），都在 `ScrollSession::tick()`
  里；改阈值必须同步 README「截图」小节写的 2.5 秒与 20000 像素。
- 控制条必须 `WA_ShowWithoutActivating` + `WindowDoesNotAcceptFocus`：少了这两条，一弹出就抢走
  焦点，用户会以为「滚不动了」。

## 文件占用释放（`plugins/filelock`，插件内部）

- 三层同 procmon：`FileLockLogic.h`（纯 Qt Core：路径解析 / 展开 / 归一化 / 匹配 / 按 pid 归并，
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

## 二维码（`plugins/qrcode`，插件内部）

- 分层：`QrTables.h`（版本 / 容错 / 分块 / 校正图形位置表，由 python-qrcode 的 `base.rs_blocks`
  导出生成，**改表要同步核对注释里每版的 data 字节数**）、`QrGalois.h`（GF(256) 与 RS，纯函数）、
  `QrLayout.h`（画布规则：功能图形、保留位图、格式 / 版本信息、蛇形填充顺序）、
  `QrEncode.*` / `QrDecode.*`（两个方向）、`QrCodePanel.*`（生成 / 识别两页）。
- 编解码**共用** `QrLayout` 的 `reservedMap()` 与 `forEachDataModule()`：两边对「哪些模块属于功能图形、
  数据按什么顺序填」的理解只要差一点，就会得到「自己生成的码自己解不开」这种最难查的错。
  要改画布规则，只改 `QrLayout.h`，别在编码器里另写一份。
- GF(256) 的表放在**函数内 static**，不要做命名空间级常量：插件是 DLL，跨翻译单元的静态初始化
  顺序不保证，早于表初始化去查表会拿到全零。
- **两个致命 bug 都出在码字生成 / 纠错上**，症状是「表面正常、内容全错」：
  - `generator()` 取系数写成 `poly[degree - i]`：`poly` 有 degree+1 项、最高次恒为 1，这样取到的是
    不存在的最高次并整体错位，生成的多项式全废。正确是 `poly[degree - 1 - i]`。
    定位办法：把码字每 8 位一组打印，与 `python-qrcode` 的 `rs_blocks` 逐字节比对，「后半段全错」就是它。
  - Forney 多乘了一个 `X_i`：fcr=0 时该因子已抵消，再乘就整组纠错失败（哪怕只翻了 1 个比特也报
    「超出容错范围」）。用随机翻转 1 / 3 / 8 / 20 比特的用例一测即可定位。
- 识别是「先粗后精、逐组合裁决」：1:1:3:1:1 游程扫候选 → 垂直 / 水平交叉复核 → 聚类 →
  `shortlistCandidates()`（命中次数前 8 + 模块尺寸最小前 5 + 四方向极值）→ `rankTriples()` 枚举
  **全部**几何可行组合并按跨度排序 → `decodeBinary()` 最多试 24 组 / 40 次，最终由格式信息 + RS 校验裁决。
  **不要退化回「只取前 8 个候选点」**：v40 这种密集码的数据区里，1:1:3:1:1 片段可能比真定位图形更多、
  更「标准」（实测命中 24 次），只取前几个必然漏掉真点。
- 校正图形必须**质心精修**：粗搜落点偏 3px，透视校正会把误差放大到整个右下角（v2 以上全解不开）。
  做法是取最接近粗定位点的黑像素做连通域填充，面积限制在 `期望面积 × 2.5` 以内（否则会连到外框），
  再取质心。
- 解码前把长边缩到 2000px 以内：v40 在 2000px 下每模块仍有 11px，够分辨；不缩的话手机照片
  4000px 逐像素扫描太慢。
- 段解析覆盖数字 / 字母数字 / byte / ECI / 结构化追加头 / FNC1；byte 段按 UTF-8 → GB18030 → Latin-1
  依次尝试；**Kanji（Shift-JIS）明确报不支持**，不要默默当乱码输出。
- UI：预览必须画在白底上——`QLabel#qrPreview` 直接写死 `background:#FFFFFF`，深色主题下跟着变深
  的话扫描器可能读不出来。识别页投放区宽度由 `setMaximumSize(240, 320)` 定住（宽度撑左栏、
  高度留一点弹性），缩略图在 `updateThumbnail()` 里按投放区现算，**不放大**（小图放大只会掩盖它糊）。

## 表情包（`plugins/memes`，插件内部）

- 分层：`MemeSource.h`（纯逻辑 header：两个数据源的 URL 构造与 HTML / JSON 解析、URL 归一化、
  SHA-256 缓存键、图片格式魔数嗅探 —— 不依赖 QWidget，可单独 include 做断言）、`MemeCache.*`（磁盘缓存）、
  `MemeTile.*` / `MemeGrid.*` / `MemePreview.*`（瓦片、网格、面板内浮层）、`MemePanel.*`（总装 + 网络）。
- **两个数据源都要带对头**：发表情（`fabiaoqing.com`）的图床校验 `Referer`，不带就整片 403；
  360 图片搜索的 JSON 接口不校验。降级链固定为 发表情 → 360，两家都失败时状态行把两个原因都写出来。
- 主源失败后要**冷却**（`kFabCooldownMs`，5 分钟）：实测发表情会一次性挂起十几秒到半分钟，
  不冷却的话每次搜索都先等满 8 秒超时才出结果。只有「超时 / 403 / 结构解析失败」才冷却，
  解析出 0 条（站点通、只是没这个词）不冷却，否则一个冷门关键词会把主源停用 5 分钟。
  冷却状态是面板成员变量（`QElapsedTimer`），不能做成静态/全局——同一工具可能同时有两个面板。
- 发表情的「搜不到」和「页面结构变了」必须分开：有 `searchbqppdiv` → 正常结果；只有 `bqppdiv`
  → 是「无结果时的推荐流」，当 0 条；两个都没有 → 结构异常，报解析失败。混在一起会让改版看起来像「没结果」。
  解析按 `searchbqppdiv` 的**位置切块**再逐块取字段，不要写嵌套 div 的正则（发表情的 DOM 嵌套很深）。
- 复制走「文件通道」：剪贴板里同时放 `setImageData()`（首帧）、`setUrls()`（本地路径）与动图字节
  （Windows 用 `application/x-qt-windows-mime;value="GIF"`，Linux 用 `image/gif`，macOS 只放前两项）。
  **必须有磁盘缓存**：`setUrls` 存的是路径，图片只活在内存里的话粘贴时目标程序读不到任何东西。
  `outbox/` 是给剪贴板用的稳定副本，**不参与容量清理**——剪贴板里的路径可能几分钟后才被粘贴。
  组装逻辑抽成了自由函数 `buildClipboardMime()`，测试直接断言它而不是读系统剪贴板（见踩坑表）。
- 动图播放：`QMovie` 用 `CacheNone` + `setScaledSize()`，`frameChanged → update()` 重绘；
  只播视口内的（按视口中心距离排序、上限 16 个），滚出视口只停播不清图，点开大图时网格全停。
  停播要 `disconnect(this)` → `stop()` 后**同步 delete**，异步删会留着定时器继续跑。
- 瓦片是自绘的、**没有 `Q_OBJECT`**（用 `std::function` 回调，测试靠 `dynamic_cast` 识别）：
  152×184 固定尺寸，常态边框用 `Border` 而不是 `BorderSubtle`（白底表情包在浅色主题下会糊成一片）。
- 面板不缓存（宿主约定），所以每次进来都要重建缓存对象与播放状态；同时存在内嵌与独立窗口两份时
  两边的关键词、播放、下载队列必须完全独立，不能有任何静态 / 全局状态。
- 主题色只能走 `xtools::theme::applyStyle()` / `color()`：`color()` 内部是 `QColor(QString)`，
  解析不了 `rgba(255,255,255,235)` 这种 CSS 函数式写法（拿到无效色，`fillRect` 等于什么都没画）。

## JS 代码运行（`plugins/jsrun`，插件内部）

- 分工：`JsRunConsole.h`（console 安装片段 + 输出阈值，纯数据）、`JsEngineRunner.h/.cpp`（引擎 +
  worker 线程 + 中断 + 结果格式化）、`JsRunTool.cpp`（面板）、`JsRunStore.*`（`jsrun.ini`）。
  内核不依赖任何界面，所以能搭临时工程直接断言（超时中断、循环引用、Store 读写都是这么验的）。
- **引擎只能在创建它的线程里用**：`JsRunWorker::run()` 里栈上建 `QJSEngine`，`run()` 返回即销毁。
  跨线程唯一安全的调用是 `QJSEngine::setInterrupted()`（官方文档明确线程安全、就是为打断死循环
  设计的），这就是「3 秒超时」的全部基础。
- **`RunContext` 的锁契约（改之前必读）**：`engine` 指针只在持 `mutex` 时读写 —— worker 负责
  建/摘指针，主线程持锁调 `setInterrupted`。`evaluate()` 期间**绝不持锁**，否则中断会被自己挡住。
  另有粘滞标记 `interrupted`：中断可能在引擎还没建好时就到达（刚点运行立刻切走页面），worker
  建好引擎后必须补打一次 —— 这条是 200 次竞态压测里真实踩出来的，漏了就是死循环跑到进程退出。
- **每次运行都新建引擎**：`setInterrupted(true)` 会一直生效到显式设回 false，被中断引擎里的
  `var` / 全局状态也会污染下一次；新建开销在 1 ms 量级，不值得复用。
- **console 桥的所有权**：`JsConsoleBridge` 是 `run()` 里的栈对象，`newQObject()` **之前**必须
  `setObjectOwnership(&bridge, CppOwnership)` —— 默认的 JavaScriptOwnership 会让引擎 GC 时
  delete 这个栈对象（双重释放）。桥只暴露一个 `log(level, text)` 槽，脚本拿不到别的宿主能力。
- **V4 没有 `globalThis`**（Qt 6.8.3 实测）：`console` / `print` 由 C++ 侧
  `engine.globalObject().setProperty()` 挂上去；在 JS 片段里写 `globalThis.xxx` 会直接
  `ReferenceError`（早期版本靠非严格模式下的 `this` 碰巧能用，那是实现细节，别退回去）。
- **销毁路径**：面板析构先 `interrupt()` 再 `wait(2s)`；等不到就与线程脱钩（线程对象在 finished
  时自删，`RunContext` 由 `shared_ptr` 保活），**绝不 terminate、也绝不 delete 正在跑的 QThread**。
  实测「运行中销毁面板」在 20 ms 内返回。
- Windows 上 QJSEngine 要 `Qt6Qml.dll`：插件目标自己 `POST_BUILD` 拷到 `bin/`（windeployqt 只看
  主程序 exe 的导入表），`install(FILES ...)` 负责 staging，`package.bat` 另有存在性校验。

## 屏幕录制（`plugins/screenrec`，插件内部）

- 分层：`AviMjpegWriter.h`（**只依赖 QtCore**，MJPEG-in-AVI 流式写入）、`ScreenGrabber.h`（抓屏，
  唯一碰原生 GDI 的地方）、`CursorPainter.h`（光标位图与落图）、`FrameQueue.h`（有界队列）、
  `RecorderWorker`（写盘线程体，无 `Q_OBJECT`）、`RecordService`（**模块级单例**，编排与状态）、
  两个浮层（`CountdownOverlay` / `RegionSelectOverlay`，无 `Q_OBJECT`）、`ScreenRecPanel`（纯视图）。
  纯逻辑做成纯 header 才能在临时工程里直接 include 验证（规则 2）。
- **录制器必须是模块级单例，不能挂在面板上**。宿主不缓存面板（回首页 / 切设置 / 切工具 / 关主
  窗口进托盘都会 `deleteLater()`），挂面板上等于「用户点一下首页，录了三分钟的视频就被截断」。
  面板只订阅 `stateChanged/statsChanged/finished/failed`，析构时**什么都不做**；新面板构造时读一次
  `RecordService::instance()` 的状态即可恢复「录制中 00:23」（幂等只读）。
- 单例用**函数内 static**，不 `new` 也不挂 `qApp` 名下：宿主 `PluginManager::unloadAll()` 会
  `FreeLibrary`，如果 qApp 持有它，析构时会去 delete 一段已经 unmap 的 vtable。
- 退出收尾只能用 `aboutToQuit`（`Qt::DirectConnection` → `stopSync()`），**不能用 `qAddPostRoutine`**：
  它登记的是本 DLL 里的函数指针，unload 之后悬空，而 post routine 恰恰在那之后才跑。
- **AVI 结构的三个死穴**（改 `AviMjpegWriter.h` 前先看 `avi::` 里的偏移常量，两份独立校验脚本都对过）：
  ① `idx1` 的 `dwChunkOffset` **相对 `'movi'` 四字符**，所以第一帧恒为 4（不是 0 也不是 224）；
  ② RIFF 的奇数长度 chunk 必须补 1 字节，补位**计入** `movi`/`RIFF` 的 size，**不计入** chunk size
  与 idx1 的 `dwChunkLength`；③ `RIFF` 的 size 必须**最后**写（追加 idx1 之后），否则播放器读到
  的文件长度是错的。头部固定 224 字节到第一帧数据，偏移全在 `avi::kOff*` 里，不要在别处写魔数。
- 每 `qMax(8, fps/2)` 帧做一次 `checkpoint()`：回填 `dwTotalFrames` / `dwLength` / `movi` size /
  `riffSize`，**刻意不 flush**（write 已进 page cache）。这样进程被强杀留下的文件「没有索引但能
  顺序播放」；不 checkpoint 的话强杀得到的文件 `riffSize` 还是 0，播放器直接拒绝打开。
- **抓屏重入保护不能用互斥锁**。`QScreen::grabWindow()` / `BitBlt` 在 Windows 上会转消息泵，
  嵌套的定时器 tick 会并发改写同一个隐式共享 `QImage` 的引用计数 → 堆损坏 `0xC0000374`。
  用 `GrabbingGuard`（`bool` 标志 + RAII）挡住第二次抓屏，返回 `busy=true` 让调用方只计数。
- Windows 抓屏走 **`GetDC(NULL)` + `CreateDIBSection` + `BitBlt(SRCCOPY|CAPTUREBLT)`** 快路径
  （实测 2560×1600 约 20ms，`QScreen::grabWindow` 约 41ms）；多屏 DPR 不一致时回退 Qt 路径，
  因为快路径假定所有屏同一缩放比。
- 光标：`hbmColor` 为空时是**单色光标**（只有 AND/XOR 掩码，位图高度是光标的两倍），必须自己
  按「AND=1 & XOR=0 → 透明、0&0 → 黑、其余 → 白」合成，否则这台机器上取不到任何光标。
  落图用 `drawImage(pos - hotspot)` 且关掉 `SmoothPixmapTransform`（放大插值会糊边）。
- 恒定帧率 + **补重复帧**：MJPEG AVI 没有逐帧时间戳，视频时长严格等于「帧数 / fps」。所以 tick
  按「此刻总共该写几帧」算欠账（`elapsed*fps/1000 - m_scheduled`），一次最多补 4 帧，超过就清账
  跳过并计入 `skipped`；队列满时丢**新**帧（丢旧帧会让画面时间倒退）。
- 开始录制前**先预抓一帧**：同时解决三件事 —— Wayland / macOS 未授权会在这一步立刻暴露（而不是
  录完才发现是黑屏）、用实际抓到的尺寸当视频尺寸（比按 DPR 推算可靠）、这一帧直接作为第一帧写进
  去不浪费。`beginRecording()` 靠一次性准入票 `m_beginArmed` 进门，**不能拿 `m_state` 当门票**：
  没开倒计时时状态是 `Idle`，倒计时被取消后也是 `Idle`，两者区分不了（前者必须开始、后者必须不做）。
- 倒计时浮层**必须 `WA_TranslucentBackground(true)`**。写成 `false` 时那层 alpha=110 的压暗会叠在
  窗口自己的不透明调色板底色上，整个屏幕变成一块灰板，倒计时那 2.4 秒里用户完全看不见要录的画面。
  它也不抢焦点（`WindowDoesNotAcceptFocus` + `WA_ShowWithoutActivating`，被录窗口标题栏不会变灰），
  代价是收不到键盘，取消方式只能是单击。走完到 `beginRecording()` 之间留 260ms 让合成层撤干净。
- 框选矩形**不要写 `QRect(m_start, m_current).normalized()`**：`QRect` 存闭区间端点，这个构造 +
  `normalized()` 会在反转的两边各内缩 1 像素（从 (400,300) 拖到 (900,700) 得 501×401，反过来拖只有
  499×399，同一块区域给出两个尺寸）。用 `qMin/qMax` 显式构造，两个方向都是 `|d|+1`。
  浮层上显示的尺寸要乘 DPR（用户看到的数字必须等于视频的真实像素）。
- 转 MP4 走外部 `ffmpeg`（`-c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p`），
  `PATH` 里没有就**隐藏按钮**而不是给个点了报错的入口。MJPEG 解出来的尺寸可能是奇数，
  yuv420p 要求偶数，所以滤镜链必须是 `pad=ceil(iw/2)*2:ceil(ih/2)*2`。
- 不做音频、暂停 / 续录、摄像头画中画、多屏同时录制、跨屏框选（一次只录一块屏，与 OCR 一致）。
  面板不注册任何全局热键（全局热键只做「唤出首页搜索」）。

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
| 配置阶段报 `Qt6QmlConfig.cmake not found` | 安装的 Qt 缺 Qt Declarative 模块（根 `find_package` 现在要 `Qml` 组件，`jsrun` 用其中的 QJSEngine） | 官方安装器补勾 Qt Declarative；Linux 装 `qt6-declarative-dev`；CI 若报同样错，给 `install-qt-action` 加 `modules: qtdeclarative` |
| 首页少了「JS 代码运行」，`app.log` 里 `jsrun` 加载失败 | `bin/` 缺 `Qt6Qml.dll`：QJSEngine 在 QtQml 里，而 `windeployqt` 只按主程序 exe 的导入表部署，看不到插件依赖 | 重新构建 `jsrun` 目标（`POST_BUILD` 会拷 `$<TARGET_FILE:Qt6::Qml>`）；也可手动从 Qt 的 `bin` 拷一份进 `build/bin`。发布包由 `package.bat` 的 staging 校验兜底 |
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
| 表情包的图在浏览器里能打开，插件里整片 403 / 裂图 | 发表情的图床校验 `Referer`（防盗链），Qt 默认不带 | 请求时把搜索页地址作为 `Referer` 发出；`403` 单独翻译成一句人话，别混进「网络错误」 |
| 同一个插件在自己的临时测试工程里请求全被挡，套在宿主里正常 | `QCoreApplication::applicationVersion()` 为空，UA 拼成 `cxk-tools/`（带空尾巴）被服务端当垃圾请求 | 组 UA 时判空，退回不带斜杠的 `cxk-tools` |
| 复制出来的动图粘到聊天窗口里不动，只贴出首帧 | 各程序认的动图剪贴板格式不同，`setImageData()` 只放得下一帧 | 三端互补：Windows 额外塞 GIF 原始字节（`CF_GIF`）、Linux 塞 `image/gif`，同时始终放首帧 + 文件地址，并在状态行说明「不动就另存为再按文件发」 |
| 剪贴板相关断言在 CI / 无窗口会话里全挂，连 `setText` 都读不回来 | 这类会话下 `QClipboard` 拿不到系统剪贴板，不是代码错 | 把 MIME 组装拆成自由函数 `buildClipboardMime()` 直接断言，自动化里不要读系统剪贴板 |
| 预览浮层的遮罩时而是全黑、时而完全不生效 | `QColor(QStringLiteral("rgba(...)"))` 解析失败得到无效色，`fillRect` 等于什么都没画 | 半透明遮罩 / 卡片统一走 `applyStyle(...)` + `WA_StyledBackground`，别在 `paintEvent` 里手拼 CSS 颜色函数 |
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
| 从工具页弹出去的独立窗口一片空白（但 `same-widget` 检查又证明面板确实在窗口里） | `QStackedWidget::removeWidget()` 之前会 `hide()` 面板，`WA_WState_Hidden` 记在控件上，父窗口 `show()` 带不出来 | `ToolWindow` 接管面板后显式 `m_panel->show()` 一次 |
| 同一秒内开的两个工具，「最近使用」顺序不对 | 时间戳只有秒级精度，退化成按 id 排序；且 `toSecsSinceEpoch()` 会把毫秒抹掉 | 写 `ISODateWithMs`，排序用 `toMSecsSinceEpoch()` |
| `--shot` 截出来的永远是首页 | `showMainWindow()` 会 `showHome()`，切页动作排在它前面就被覆盖 | 先 `showMainWindow()`，再 `--tool` / `--settings` / `--popout` |
| 折叠后读出来的全文少了内容 | `QPlainTextEdit::toPlainText()` 对隐藏块的处理没有文档保证 | 用 `JsonEditor::fullText()`（逐块拼） |
| 行号栏位置偏、压住输入框圆角 | 行号栏按 `frameWidth()` 摆：它把 qss 的 border **和 padding** 一起算进去了 | 用 `contentsRect()` 定位，另给 `setViewportMargins()` 多留 8px 当"分隔线到正文"的留白 |
| 给编辑器 `sendEvent` 鼠标事件没反应 | `QAbstractScrollArea` 的鼠标事件是从 **viewport** 转发上来的，直接发给控件本身会被 `QAbstractScrollArea::event()` 丢掉 | 事件发给 `editor->viewport()`（坐标本来就是 viewport 坐标系） |
| 自动格式化后光标跳到别的 token 上 | `QJsonDocument` 输出时按键名排序，字符流不再一一对应，"非空白字符锚点"会失效 | `mapCursorPosition()` 里先校验锚点、校验不过退回保行号映射 |
| 粘贴走了一条没被监听的路径（右键菜单粘贴没格式化） | 各路粘贴在 Qt 里最终都落到 `insertFromMimeData()`，只管快捷键一定会漏 | 在 `JsonEditor::insertFromMimeData()` 里发信号，别在 `keyPressEvent` 里匹配 Ctrl+V |
| 自动缩进把输入法的拼字打断/吞掉按键 | 拼字期间的按键还不是最终文本 | `handleAutoIndent()` 开头检查 `isComposing()` 直接返回 |
| 进程表每 2 秒闪一下、选中的行也丢了 | 每次采样都 `clear()` 整表重建 | pid 序列没变就只改文字，变了才重建并恢复选中行与滚动位置（`ProcMonPanel::rebuildTable`） |
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
| 日志写「已注册 Alt+Space」，物理按键却毫无反应；同一台机器上 `SendInput` 合成同一个组合却能唤起 | 按键要先过 `WH_KEYBOARD_LL` 钩子链才进热键匹配，链上某个常驻程序（实测机器上同时跑着 ToDesk / Snipaste / Ditto）把真实按键吃掉，而这类钩子普遍放过 `LLKHF_INJECTED` 事件，所以注入能过、物理按不过 —— 于是「注册成功」和「按键到得了」被解耦了 | Windows 侧再挂一个自己的 `WH_KEYBOARD_LL` 兜底（`installKeyboardHook()`），命中后延迟 150ms，若这期间 `WM_HOTKEY` 到了就不管（防重复唤起）。排查时先搜日志「由键盘钩子兜底唤起」：出现了说明系统热键链路被别家拦了，没出现才是我们自己的问题 |
| 断言程序里大量 `qInfo` 一行都不打印 | Release 构建下控制台 sink 被摘掉 | 验证程序统一用 `std::printf` + `fflush(stdout)`，别依赖 Qt 日志分类 |
| 点「下载并安装」每次都失败，状态栏只有 `下载失败：Operation canceled` | ① Qt 把传输超时统一报成 `OperationCanceledError`，`errorString` 就是这句英文，用户完全看不出是网络问题；② 更新请求没读系统代理，用户开着 `127.0.0.1:7890` 的代理、浏览器能开 GitHub，程序却直连 | 错误码逐条翻中文（`UpdateChecker::describeNetworkError`）+ 更新专用的 manager 套 `systemProxyForQuery()`；断流改 `Range` 断点续传。四条硬约束见「更新链路」 |
| 界面上的 `▾` 渲染成方块 | 字体缺字形（中文回退字体里没有这个符号） | 换成纯文字的 `QPushButton("切换")`，跨平台别再赌字形 |
| `--check-update` 跑完进程不退出，被 SIGTERM 杀掉 | 在 `app.exec()` 之前调 `qApp->quit()`，Qt 会忽略这次请求 | 用 `QTimer::singleShot(0, qApp, [code]{ QCoreApplication::exit(code); })` 把退出排进事件循环 |
| 改完插件链接报 `LNK1104`「无法打开 …\plugins\xxx.dll」 | 上一次 `--shot` 或手动启动的 cxk-tools 还在后台跑着，DLL 被占用（先 `taskkill` 再删 DLL 会"成功"，但链接仍然写不进去） | 构建前 `tasklist \| findstr cxk-tools` 确认没有残留进程，有就结束掉；不要靠删 build 目录绕 |
| 二维码生成的码自己解不开，别的解码器也读不出 | RS 生成多项式系数下标错位：`poly[degree - i]` 取到了恒为 1 的最高次，整条多项式错位 | 改成 `poly[degree - 1 - i]`；定位办法是把码字与 python-qrcode 的 `rs_blocks` 逐字节比对 |
| 二维码纠错连翻 1 个比特都报「超出容错范围」 | Forney 里多乘了一个 `X_i`，而 fcr=0 时该因子已抵消 | `e_i = Ω(X_i⁻¹) / Π_{j≠i}(1 + X_j/X_i)`，不要再额外乘 `X_i` |
| 空状态的提示文字在窗口缩放后自己消失 | `QLabel::setPixmap()` 会连 text 一起清掉，而 resize 钩子里调了 `setPixmap(QPixmap())` | 清空只放在 `clearDecoded()`；`updateThumbnail()` 里"没有图就直接 return" |
| 用户开着程序时，脚本里的 `--check-update` / `--shot` 直接退出、什么都不输出 | 单实例互斥把它当成第二个实例，唤出旧窗口后自己结束 | 诊断命令（`--shot` / `--check-update`）在 `main.cpp` 里跳过 `tryLock()` 与 `setActivateHandler` |
| 更新对话框里说明文字下面多出一根"横线" | `QTextBrowser` 默认按需横向滚动，长行触发横向滚动条 | `setLineWrapMode(QWidgetText::WidgetWidth)` + `setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff)` |
| `package.bat` 打包时报 `vc_redist.x64.exe` 拒绝访问 | `windeployqt` 默认塞 25 MB 的 redist 安装器，且该文件被占用 | `--no-compiler-runtime` + 拷 app-local CRT DLL，prune 脚本里再兜底删一次 |
| 安装器向导中文乱码 | `.iss` 被某次编辑存成了 GBK 或带 BOM 的 UTF-8 | 保持 UTF-8 无 BOM（Inno 6 能识别）；仍乱码就改存 UTF-8 with BOM，**不要改文案**（规则 5） |
| 深色下改了卡片背景，分组框标题后挂一块色斑 | `QGroupBox::title` 用 `background` 压在边框上「割」开边框，这个底色必须等于**盒子自身底色**（不是页面底色）。旧代码盒子=页面同色时两者相等，一旦给盒子换 `@card` 而标题仍是 `@bg`，标题后那块就露馅 | 盒子和 `::title` 的 `background` 必须同步改（现在都走 `@card`），记住不变量是「title == box」 |
| 主窗口加了投影后，缩放抓手飘到透明阴影区、或投影被裁掉 | ① `QGraphicsDropShadowEffect` 画在控件矩形外，超出窗口的部分直接不可见，四周不留透明边距阴影就出不来；② `QSizeGrip` 若还挂在带投影的 root 上会被投影「糊」一圈，且原定位 `width()-18` 会把抓手贴到透明阴影区而不是圆角卡片右下角 | 投影宿主（centralWidget）四周留 `kShadowMargin`（≥ blurRadius 的一半）；抓手挂宿主、`resizeEvent` 定位扣掉 `kShadowMargin` |
| 小按钮收敛到 `compact` 后，某个按钮的高度突然不对了 | `compact` 只清 `min-height`，但个别小按钮的 `padding` 各不统一（0 / 2px / 3px 6px …），若把 padding 也塞进 compact 会把不同按钮的 padding 抹成一致、高度失配 | compact 收敛边界：只清 `min-height`；`padding`、`font-size`、`:checked`/`:hover` 状态色一律留在各 objectName 选择器 |
| 打包报 `'C:\Program' is not recognized` | `%ProgramFiles(x86)%` 写在 `if (...)` 块内，`(x86)` 的 `)` 提前闭合块，剩余文本被当成命令执行 | 把这段逻辑挪出块（标签子程序 + `goto`），块内不要出现带括号的路径变量 |
| 装好的 ISCC 找不到，或报 `"" 不是内部或外部命令` | 同一个块里先 `set` 再 `%VAR%`：整块在解析期就展开完了，读到的是空值 | 同上；确实要在块内取值就 `setlocal enabledelayedexpansion` 后用 `!VAR!` |
| CI 上安装器步骤莫名失败、日志只有一行 err | 没有把「必须有安装器」变成硬约束 | CI 里设 `XTOOLS_REQUIRE_INSTALLER=1`，`package.bat` 缺 ISCC 即 `PACKAGE_FAILED` |
| Linux 编译报 `cannot call member function 'QGuiApplication::nativeInterface()' without object` | `nativeInterface<T>()` 是**成员**函数（模板成员），写成 `QGuiApplication::nativeInterface<T>()` 看着像静态调用；MSVC 与 clang 对这类写法更宽松，只有 GCC 会在 CI 上把它拦下来 | 用 `qGuiApp->nativeInterface<T>()`（或 `qobject_cast<QGuiApplication *>(qApp)`）。别用 `QGuiApplication::instance()`：静态类型是 `QCoreApplication*`，拿不到这个模板成员 |
| 只改了 Linux 分支，本机（Windows）却「编过」了 | 平台分支在另外两个平台上根本不参与编译，本机通过不代表 CI 通过 | 改 `#if defined(Q_OS_LINUX)` 里的代码，交付说明必须写「本机无法验证，等 CI 首次运行」；沙箱禁 WSL/Docker 时没有本地替代方案 |
| `build (macos)` 一步都没跑就 cancelled，annotation 只有 `The configuration 'macos-14-arm64-us-default' is not supported`，日志是空的 | `macos-14` 镜像已于 2026-11-02 从 GitHub Actions 退役：标签还在，但没有机器 | 换 `macos-15`（arm64）。看 `actions/runner-images` 的 Available Images 表确认当前标签，**别用 `-latest`** |
| macOS 包在用户的旧系统上双击没反应，`Info.plist` 却写着 12.0 | 没设 `CMAKE_OSX_DEPLOYMENT_TARGET`，clang 按构建机系统版本当部署目标（runner 是 15/26 就要求 15/26） | 在 `project()` **之前**设 `CMAKE_OSX_DEPLOYMENT_TARGET`；`package-macos.sh` 会打印 `MINOS=`/`PLIST_MIN=` 并在不一致时告警 |
| macOS 任务报 `line 1: ./scripts/build.sh: Permission denied` | 仓库在 Windows 上开发（`core.fileMode=false`），`.sh` 以 `100644` 提交，runner 上就没有可执行位 | 两处都做：`git update-index --chmod=+x scripts/*.sh` 补权限位；CI 里统一用 `bash scripts/x.sh`，不依赖权限位 |
| 打包报 `file SIZE requested of path that is not readable: …/QtSvg.framework/Resources` | macOS framework 内部全是符号链接（`Resources` → `Versions/Current/Resources`），`IS_DIRECTORY` 会跟随链接，把链接当目录去算体积就硬失败 | `_xtools_prune` 里先判 `IS_SYMLINK` 跳过：链接不占体积，删的时候跟着父目录走 |
| 对一张校验不过的文件跑签名脚本，却打印 `[sign] verified` | `signtool verify /pa /q` 仍把 File/Index/Timestamp 表格打到 stdout，函数把这串输出一并返回，`if (Test-Signature …)` 拿到的是非空数组（恒为真） | 调用处写 `$null = & signtool verify …`，只用 `$LASTEXITCODE` 判断（见 `sign-windows.ps1` 的 `Test-Signature`） |
| 签完名再打包，zip 里 / 用户装上的 exe 仍没有签名 | 顺序被打乱：签名会改写 PE 头，事后签只改了 `dist\windows-x64` 里那一份 | `package.bat` 的签名必须在 prune 之后、`Compress-Archive` 之前；安装器在 ISCC 之后单独签一次 |
| macOS 公证通过，用户第一次打开仍被 Gatekeeper 拦 | 只提交了公证没有 `stapler staple`：票据留在 Apple 服务端，离线首启查不到 | 对 `.app` 与 `.dmg` 各 staple 一次（`package-macos.sh` 已做），`xcrun stapler validate` 可复查 |
| `codesign` 报 `a sealed resource is missing or invalid` / `code object is not signed at all` | 用了 `--deep`，或先签容器后签内部文件：容器封条算完又被改写 | 由内向外签（`sign_bundle`）：Mach-O → 主程序 → `.app` → `.dmg`，签名时不要 `--deep` |
| CI 上 `codesign` 卡在 `User interaction is not allowed` | 私钥没授权给 codesign：缺 `set-key-partition-list`，或钥匙串不在搜索列表里 | 导入后 `security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k <pwd> <keychain>`，并把钥匙串加进 `security list-keychains -d user -s` |

---

# 第七部分：提交前检查清单

1. `scripts\build.bat`（或 `scripts/build.sh`）跑通，末行 `BUILD_OK`，**零警告**。
2. 改动涉及界面 / qss：用 `--shot` 出图并**真的看过**，随后删除截图。
3. 改动涉及纯逻辑：临时断言程序跑过，贴出通过数量，然后 `rm -rf build/_verify`。
4. 改动涉及平台分支：Win / Linux / macOS 三段都能编译，兜底分支给出可读失败原因。
5. 改动涉及打包 / 更新：本地跑一遍 `scripts\package.bat`，确认 `dist\` 下的产物名与体积基线相符
   （见「裁剪清单与取舍」），并用 `XTOOLS_UPDATE_FEED` 指向本地假清单验一遍
   `--check-update`；改过资产名就先确认 `gen-release-manifest.py` 能把它归到正确的 `kind`。
   改动涉及签名：确认「没配证书只告警 / 配了但失败必须中断」这条语义没被改掉，本地用自签证书
   跑一遍签名步骤（看到 `[sign] done`），测完删掉证书与临时产物。
6. 同步文档（规则 6）：改架构、构建、约定、平台能力 → 改本文件；改用法、支持工具、环境要求 → 改 `README.md`；
   改插件接口 → 改 `docs/PLUGIN.md` 并升 `XTOOLS_API_VERSION`。
7. 编码自查（规则 5 的三条命令），应无输出。
8. `git status --porcelain` 只剩预期文件：无 `build/`、`dist/`、截图、临时工程、`aqtinstall.log`。
9. 交付说明写清「跑了什么、结果如何」，不要只写"应该没问题"。