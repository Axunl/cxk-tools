#pragma once

// ---------------------------------------------------------------------------
// 抓帧线程与写盘线程之间的有界队列。
//
// 为什么需要队列而不是"抓到就写"：
//   抓屏必须发生在 GUI 线程（BitBlt/grabWindow 都要求），而 JPEG 编码是纯计算
//   （实测 2560×1600 q75 要 24ms）。如果编码也在 GUI 线程做，15fps 下每帧要占
//   28+24=52ms，界面会抖成幻灯片。所以策略是：GUI 线程只抓帧并塞进队列，
//   工作线程负责编码与落盘。
//
// 为什么有界：编码跟不上时不能无限缓存。容量 8 在 15fps 下是 533ms 的缓冲，
// 够吸收一次窗口重排级别的抖动，同时把内存占用压在 8×16MB（2560×1600）以内。
// 满了就**丢新帧**——丢旧帧会让画面时间倒退，而丢新帧只是画面卡一下。
// ---------------------------------------------------------------------------

#include "CursorPainter.h"

#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QPoint>
#include <QWaitCondition>
#include <QtGlobal>

#include <deque>
#include <utility>

namespace screenrec {

/// 一帧待编码的数据。frame 是设备像素、DPR 已归一为 1.0。
struct FrameJob
{
    QImage frame;
    CursorBitmap cursor;        ///< 光标位图；image 为空时由 drawCursor 走自绘兜底
    QPoint cursorPos;           ///< 热点在帧内的设备像素坐标
    bool cursorInside = false;
    bool drawCursor = false;
    /// 这节课要写几帧（1~4）。大于 1 表示上一拍落后了，用重复帧补齐，
    /// 这样 AVI 的恒定帧率与真实时长才对得上，且只编码一次。
    int copies = 1;
};

class FrameQueue
{
public:
    /// 默认容量。4K 屏幕下每帧 33MB，调用方应传更小的值（见 RecordService）。
    static constexpr int kDefaultCapacity = 8;

    explicit FrameQueue(int capacity = kDefaultCapacity)
        : m_capacity(qMax(1, capacity))
    {
    }

    FrameQueue(const FrameQueue &) = delete;
    FrameQueue &operator=(const FrameQueue &) = delete;

    /// 入队。队列满或已关闭时返回 false（调用方据此统计丢帧）。
    bool push(FrameJob job)
    {
        QMutexLocker locker(&m_mutex);
        if (m_closed)
            return false;
        if (int(m_items.size()) >= m_capacity)
            return false;
        m_items.push_back(std::move(job));
        m_condition.wakeOne();
        return true;
    }

    ///
    /// 出队。超时返回 false（调用方靠它周期性检查停止标志）。
    ///
    /// 阻塞等待时会释放锁，所以 push 不会被 pop 的长等待卡住。
    ///
    bool pop(FrameJob *out, int timeoutMs)
    {
        QMutexLocker locker(&m_mutex);
        if (m_items.empty() && !m_closed)
            m_condition.wait(&m_mutex, timeoutMs);
        if (m_items.empty())
            return false;
        *out = std::move(m_items.front());
        m_items.pop_front();
        return true;
    }

    /// 关闭：唤醒所有等待者，之后 push 一律失败。工作线程消费完剩余帧即可退出。
    void close()
    {
        QMutexLocker locker(&m_mutex);
        m_closed = true;
        m_condition.wakeAll();
    }

    bool isClosed() const
    {
        QMutexLocker locker(&m_mutex);
        return m_closed;
    }

    bool isEmpty() const
    {
        QMutexLocker locker(&m_mutex);
        return m_items.empty();
    }

    int size() const
    {
        QMutexLocker locker(&m_mutex);
        return int(m_items.size());
    }

    int capacity() const { return m_capacity; }

private:
    mutable QMutex m_mutex;
    QWaitCondition m_condition;
    std::deque<FrameJob> m_items;
    const int m_capacity;
    bool m_closed = false;
};

} // namespace screenrec
