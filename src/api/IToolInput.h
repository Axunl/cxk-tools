#pragma once

// ---------------------------------------------------------------------------
// 可选的「带内容进入」接口。
//
// 背景：宿主首页搜索会识别用户意图（一段 JSON / 一张粘贴进来的图片 / 一段待翻译
// 英文），并把**那段内容本身**带进对应工具。ITool::createPanel() 没有参数，
// 无法表达这件事，所以额外定义一个独立接口。
//
// 为什么是一个新接口而不是给 ITool 加虚函数：
//   * 加虚函数会改 ITool 的虚表布局，必须升 XTOOLS_API_VERSION，所有旧插件当场失效；
//   * 这个能力本来就是可选的 —— 不实现它的插件照常工作，宿主只会「打开工具但不带内容」；
//   * 用独立 IID 做 qobject_cast，旧插件返回 nullptr，宿主记一条日志继续，不存在兼容风险。
//
// 调用顺序约定（宿主负责保证）：createPanel() 之后立刻调用 setToolInput()，
// 此时面板已存在但尚未显示。实现方应自己处理「滚动到合适位置 / 聚焦输入框」。
// ---------------------------------------------------------------------------

#include <QImage>
#include <QMetaType>
// Q_DECLARE_INTERFACE 定义在 qobject.h 里；本头文件只用到几个值类型，
// 不包含 QObject 的话下面那行宏会被当成普通函数声明（MSVC: C2059）。
#include <QObject>
#include <QString>

namespace xtools {

/// 交给工具的内容。只有一个「当前有效」的字段，由 kind 决定。
struct ToolInput
{
    enum class Kind {
        None,     ///< 没有内容（宿主用于表达「普通打开」）
        Text,     ///< text 有效：JSON 文本 / 待翻译文本
        Image,    ///< image 有效：要从图片里提取文字的图片
    };

    Kind    kind = Kind::None;
    QString text;    ///< Kind::Text 时有意义
    QImage  image;   ///< Kind::Image 时有意义（QImage 隐式共享，跨 DLL 传递无额外拷贝）
};

///
/// 工具若希望能接收首页带进来的内容，就额外实现这个接口。
///
/// 注意：接口方法会在宿主 UI 线程同步调用，实现里不要开事件循环、不要长时间阻塞
/// （OCR 这类同步任务目前可以接受，但必须保证不会在函数内销毁面板自身）。
///
class IToolInput
{
public:
    virtual ~IToolInput() = default;

    /// 接收宿主带进来的内容。kind == None 时应当什么也不做。
    virtual void setToolInput(const ToolInput &input) = 0;
};

} // namespace xtools

/// 与 ITool 的 IID 相互独立：新增这个接口不影响 XTOOLS_API_VERSION。
#define XTOOLS_ITOOLINPUT_IID "cn.cxk.xtools.IToolInput/1.0"
Q_DECLARE_INTERFACE(xtools::IToolInput, XTOOLS_ITOOLINPUT_IID)

/// 让 ToolInput 能进信号槽（HomePage -> MainWindow 用直接连接传值，
/// 声明成元类型是为了将来改成队列连接时不会静默丢内容）。
Q_DECLARE_METATYPE(xtools::ToolInput)
