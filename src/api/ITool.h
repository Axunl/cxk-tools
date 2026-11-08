#pragma once

// ---------------------------------------------------------------------------
// 插件接口。这是宿主与插件之间唯一的 ABI 边界。
//
// 设计取舍：
//   * 纯头文件 + 纯虚接口，不链接任何共享库 —— 插件只需要一个 include 路径；
//   * 只暴露 Qt 值类型与 QWidget 指针，避免跨 DLL 传 STL / 自定义结构；
//   * 用 apiVersion() 做版本挡板：接口变了就升版本，宿主拒绝加载旧插件，
//     而不是冒着虚表错位直接崩溃。
// ---------------------------------------------------------------------------

#include <QIcon>
#include <QString>
#include <QStringList>
#include <QWidget>

namespace xtools {

/// 工具的静态元信息。宿主用它做搜索匹配、侧边栏展示与页头标题。
struct ToolInfo
{
    QString     id;            ///< 唯一标识，小写英文，如 "jsonfmt"。空则由宿主取文件名
    QString     name;          ///< 展示名，如 "JSON 格式化"
    QStringList keywords;      ///< 搜索关键词，如 {"json", "格式化", "format"}
    QString     description;   ///< 一句话说明，显示在页头与悬浮提示
    QString     iconPath;      ///< 可选，相对插件目录的图标文件；留空则由宿主生成字母徽章
};

///
/// 工具接口。所有本地工具（内置或 plugins/ 下的插件动态库）都实现它。
///
/// 实现约定（宿主依赖这些前提，违反会崩溃或内存泄漏）：
///  - createPanel() 可能被反复调用，每次都必须返回一个**全新**的 QWidget；
///  - 返回的面板不要设 parent，宿主会 setParent 并塞进 QStackedWidget；
///  - 面板创建后的生命周期由宿主接管，插件不要自己 delete；
///  - 任何异常情况都不要让宿主崩溃，返回 nullptr 即可（宿主会显示占位页）。
///
class ITool
{
public:
    virtual ~ITool() = default;

    /// 接口版本，宿主当前为 1。不匹配的插件会被拒绝加载并记录原因。
    virtual int apiVersion() const { return 1; }

    virtual ToolInfo info() const = 0;

    /// 创建工具主面板。parent 可能为 nullptr，实现里不要依赖它非空。
    virtual QWidget *createPanel(QWidget *parent = nullptr) = 0;
};

} // namespace xtools

/// 插件元数据标识。插件里 Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID) 必须完全一致。
#define XTOOLS_ITOOL_IID "cn.cxk.xtools.ITool/1.0"
Q_DECLARE_INTERFACE(xtools::ITool, XTOOLS_ITOOL_IID)

/// 宿主支持的接口版本。改 ITool.h 的虚函数或 ToolInfo 字段时必须同步 +1。
#define XTOOLS_API_VERSION 1
