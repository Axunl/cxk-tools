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
    QString     description;   // 一句话说明，显示在页头
    QString     iconPath;      // 可选，相对插件目录的图标；留空自动生成字母徽章
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
    layout->setContentsMargins(28, 0, 28, 20);   // 与主窗口页头对齐
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
产物出现在 `build/bin/plugins/`（`.dll` / `.dylib` / `.so`），重启应用即可在侧边栏搜到它。

## 4. 注意事项

- **不要给面板设 parent 归属**：`createPanel` 返回的 QWidget 由宿主接管，传进来的 `parent` 可能为空。
- **面板可能被创建多次**：不要在构造函数里做「假定只执行一次」的全局副作用。
  （宿主实际只在工具第一次打开时调用一次，但接口语义允许多次。）
- **加 `#include "MyTool.moc"`**：如果 cpp 里还定义了带 `Q_OBJECT` 的类（比如面板类），
  文件末尾必须加这一行，否则链接期找不到元对象。
- **接口版本**：宿主只加载 `apiVersion() == 1` 的插件。改了 `ITool.h` 的虚函数或 `ToolInfo` 字段
  就要升 `XTOOLS_API_VERSION` 与默认 `apiVersion()`，否则旧插件会被拒绝加载并记日志
  （这是刻意的，避免虚表错位直接崩溃）。
- **样式**：主程序已有全局 QSS，`QPushButton` / `QLineEdit` / `QPlainTextEdit` 直接用就是统一风格；
  想要强调按钮可以 `btn->setProperty("accent", true)`。
- **不要自己弹窗**：工具面板是嵌在主窗口里的，弹独立对话框会破坏"一个窗口装下所有工具"的体验。

## 5. 调试与验证

加载失败不会崩主程序，原因写进日志，同时显示在「设置 → 工具与插件」：

- 日志：`QStandardPaths::AppLocalDataLocation/logs/app.log`
  （Windows 通常是 `%LOCALAPPDATA%\cxk\cxk-tools\logs\app.log`）
- 界面上：设置页「已加载 N 个插件，失败 M 个」下方会逐条列出失败原因

想把求值、解析这类纯逻辑单独验证，别把它埋在 `.cpp` 的匿名命名空间里 ——
抽成一个 header（参考 `plugins/calc/ExprEval.h`、`plugins/jsonfmt/JsonText.h`），
再用一个临时 CMake 工程 include 它做断言测试，测完把临时工程删掉即可。
