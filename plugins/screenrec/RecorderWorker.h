#pragma once

// ---------------------------------------------------------------------------
// 写盘线程的工作体：从 FrameQueue 取帧 → 画光标 → JPEG 编码 → 写 AVI。
//
// 刻意**不是 QObject、不带 Q_OBJECT**：
//   * 它整个生命周期都跑在自己的线程里，不需要信号槽；
//   * 状态由 RecordService 通过加锁快照读取（stats()），比跨线程信号更直白；
//   * 省一次 moc，也方便临时工程直接 include。
//
// 生命周期约定（由 RecordService 保证）：
//   * run() 只在线程里被调用一次；
//   * writer 与 queue 的生命周期都要长于 run()；
//   * 队列 close() 之后 run() 会把剩余帧消费完，再调 writer->finish()。
// ---------------------------------------------------------------------------

#include "AviMjpegWriter.h"
#include "FrameQueue.h"

#include <QMutex>
#include <QString>
#include <QtGlobal>

namespace screenrec {

/// 写盘线程的统计快照。跨线程读，全部字段在锁内更新。
struct WorkerStats
{
    qint64 frames = 0;    ///< 已写入的帧数（含补帧）
    qint64 dups = 0;      ///< 其中有多少是补的重复帧
    qint64 bytes = 0;     ///< 写入的 JPEG 原始字节（不含块头，仅用于展示）
    bool failed = false;
    QString error;
};

class RecorderWorker
{
public:
    RecorderWorker(AviMjpegWriter *writer, FrameQueue *queue, int jpegQuality)
        : m_writer(writer), m_queue(queue), m_quality(jpegQuality)
    {
    }

    RecorderWorker(const RecorderWorker &) = delete;
    RecorderWorker &operator=(const RecorderWorker &) = delete;

    /// 线程体。返回前一定会调 writer->finish()，保证文件可播。
    void run();

    /// 加锁快照，可从任意线程调用。
    WorkerStats stats() const;

private:
    /// 编码一帧并按 copies 写入。失败时把错误记进 m_stats 并停止后续写入。
    void encodeAndWrite(FrameJob &job);

    void markFailed(const QString &message);

    AviMjpegWriter *m_writer = nullptr;
    FrameQueue *m_queue = nullptr;
    int m_quality = 75;

    mutable QMutex m_mutex;
    WorkerStats m_stats;
};

} // namespace screenrec
