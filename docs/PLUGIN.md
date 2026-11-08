# 写一个自己的工具

一个工具 = **一个 QWidget 面板** + **一份元信息**，不到 30 行代码。
工具以插件动态库的形式存在：宿主启动时扫描 `plugins/` 目录，丢进去就装、删掉就卸。

## 1. 接口长什么样

`src/api/ITool.h`（宿主与插件共享的唯一头文件）：

```cpp
struct ToolInfo {
    QString     id;            // 唯一标识，小写英文，如 "jsonfmt"
    QString     name;          // 展示名，如 "JSON 格式化"
    QStringList keywords;      // 搜索关键词，如 {"json", "格式化"}
    QString     description;   // 一句话说明，显示在页头与磁贴悬浮提示
    QString     iconPath;      // 可选，相对插件目录的图标；留空自动生成字母徽章
                               // 建议 ≥96×96（首页磁贴按 48 逻辑像素 × DPR 取图）
};

class ITool {
public:
    virtual int apiVersion() const { return 1; }
    virtual ToolInfo info() const = 0;
    virtual QWidget *createPanel(QWidget *parent = nullptr) = 0;  // 每次返回新面板
};
```

## 2. 新建目录 `plugins/mytool/`

`MyTool.h`

```cpp
#pragma once
#include "ITool.h"
#include <QObject>

class MyToolPlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)   // 这两行不能少
    Q_INTERFACES(xtools::ITool)
public:
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};
```

`MyTool.cpp`

```cpp
#include "MyTool.h"
#include <QTextEdit>
#include <QVBoxLayout>

xtools::ToolInfo MyToolPlugin::info() const
{
    xtools::ToolInfo i;
    i.id = QStringLiteral("mytool");
    i.name = QStringLiteral("我的工具");
    i.keywords = QStringList{QStringLiteral("my"), QStringLiteral("测试")};
    i.description = QStringLiteral("一句话说明");
    return i;
}

QWidget *MyToolPlugin::createPanel(QWidget *parent)
{
    auto *panel = new QWidget(parent);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(28, 0, 28, 20);   // 与宿主 28px 留白对齐（独立窗口里上方由宿主补 12px）
    layout->addWidget(new QTextEdit(panel));
    return panel;
}
```

`CMakeLists.txt`

```cmake
xtools_add_plugin(mytool SOURCES MyTool.cpp MyTool.h)
```

## 3. 挂进构建

在根 `CMakeLists.txt` 末尾加一行：

```cmake
add_subdirectory(plugins/mytool)
```

重新构建（Windows `scripts\build.bat`，macOS / Linux `scripts/build.sh`），
产物出现在 `build/bin/plugins/`（`.dll` / `.dylib` / `.so`），重启应用即可在首页网格里看到它。

## 4. 注意事项

- **不要给面板设 parent 归属**：`createPanel` 返回的 QWidget 由宿主接管，传进来的 `parent` 可能为空。
- **面板会被创建多次**：每次进入工具宿主都会调用 `createPanel`（不缓存面板，输入值不留存），
  每次都必须返回全新面板，不要在构造函数里做「假定只执行一次」的全局副作用。
- **不要假设只有一个面板实例**：宿主可能同时把同一个工具的面板放进主窗口和「独立窗口」，
  也可能因为切换页面反复创建。所有 UI 状态（输入内容、历史列表）都放在面板实例里，
  不要用静态/全局变量缓存 —— 否则两份面板会互相串状态。
- **加 `#include "MyTool.moc"`**：如果 cpp 里还定义了带 `Q_OBJECT` 的类（比如面板类），
  文件末尾必须加这一行，否则链接期找不到元对象。
- **接口版本**：宿主只加载 `apiVersion() == 1` 的插件。改了 `ITool.h` 的虚函数或 `ToolInfo` 字段
  就要升 `XTOOLS_API_VERSION` 与默认 `apiVersion()`，否则旧插件会被拒绝加载并记日志
  （这是刻意的，避免虚表错位直接崩溃）。
- **样式**：主程序已有全局 QSS，`QPushButton` / `QLineEdit` / `QPlainTextEdit` 直接用就是统一风格；
  想要强调按钮可以 `btn->setProperty("accent", true)`。
  但 **`QComboBox` 例外**：全局 qss 为消掉原生 `::drop-down` 甩到圆角框外的边框写了规则，
  Qt 因此不再画下拉三角。每个用到 `QComboBox` 的面板都要自己补一个自绘箭头子控件
  （`WA_TransparentForMouseEvents`），并在 `eventFilter` 里跟着 Resize 摆到右侧 18px 区域
  （参考 `MemMonPanel.cpp` / `TranslatePanel.cpp` 的 `ComboArrow`）；否则用户看不出那是可展开的
  下拉框，会以为面板里根本没有可选项。
- **弹窗类浮层**（补全候选、日期选择等）要自己给 popup 设 `objectName`：这类控件是顶层窗口，
  全局 qss 只能按 `QAbstractItemView#<objectName>` 命中，不设就是系统默认外观
  （参考 `plugins/jsonfmt/JsonEditor.cpp` 里的 `jsonCompleterPopup`）。
  另外**不要给 item view 本身加 `padding`**，它会把 viewport 撑得比内容还小，导致常驻滚动条。
- **需要联网的插件要显式链 `Qt6::Network`**：`xtools_api` 只带 Core/Gui/Widgets，
  `QNetworkAccessManager` / `QNetworkInterface` 少了这一行会链接失败。在插件的
  `CMakeLists.txt` 里加 `target_link_libraries(mytool PRIVATE Qt6::Network)`
  （参考 `plugins/ipquery` 与 `plugins/translate`）。翻译这类依赖第三方服务的工具还要在
  README 里写清「需要联网」「文本会发给谁」「失败时怎么降级」，不能静默失败。
- **不要自己弹独立窗口**：宿主已经提供「独立窗口」（页头按钮 / 磁贴右键），插件再弹自己的顶层窗口
  会出现两套窗口语义；普通对话框（文件选择等）不在此列。
- **外部网页跳转用 `QDesktopServices::openUrl()`**：某家在线服务若没有可匿名自动调用的接口，
  要做成跳转入口就用它（可用剪贴板把文本带过去），不要内嵌 WebEngine 或伪装成数据源；
  状态栏要写清「译文不会自动回填、需手动操作」。注意这类入口仍会被用户当成多余按钮，
  加之前先确认有人需要，别拿跳转凑「接入了某家服务」的数。

## 5. 调试与验证

加载失败不会崩主程序，原因写进日志，同时显示在「设置 → 工具与插件」：

- 日志：`QStandardPaths::AppLocalDataLocation/logs/app.log`
  （Windows 通常是 `%LOCALAPPDATA%\cxk\cxk-tools\logs\app.log`）
- 界面上：设置页「已加载 N 个插件，失败 M 个」下方会逐条列出失败原因

想把求值、解析这类纯逻辑单独验证，别把它埋在 `.cpp` 的匿名命名空间里 ——
抽成一个 header（参考 `plugins/calc/ExprEval.h`、`plugins/jsonfmt/JsonText.h`），
再用一个临时 CMake 工程 include 它做断言测试，测完把临时工程删掉即可。

面板类本身也可以放头文件（参考 `plugins/jsonfmt/JsonFmtPanel.h`）：临时工程把它和
`JsonFmtTool.cpp` 一起编译，就能直接构造面板做端到端断言（点按钮、发按键事件），
比只测纯逻辑覆盖得多。**纯逻辑 header + 面板 header** 是这套插件结构里最好测的形态。
