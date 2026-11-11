#include "RecorderWorker.h"

#include "CursorPainter.h"

#include <QBuffer>
#include <QMutexLocker>

namespace screenrec {

void RecorderWorker::run()
{
    for (;;) {
        FrameJob job;
        const bool got = m_queue->pop(&job, 100);
        if (got) {
            encodeAndWrite(job);
            continue;
        }
        // 没取到数据：只有"队列已关闭且已排空"才是收工信号。
        // 不能只看停止标志就退出 —— 那会把最后几帧直接扔掉。
        if (m_queue->isClosed() && m_queue->isEmpty())
            break;
    }

    // 收尾必须在这里做（而不是由 RecordService 在 GUI 线程调）：finish() 会写
    // idx1 索引并回填长度，都是文件 IO；放在写盘线程里才不会卡住界面。
    if (!m_writer->finish()) {
        QMutexLocker locker(&m_mutex);
        m_stats.failed = true;
        m_stats.error = m_writer->errorString();
    }
}

void RecorderWorker::encodeAndWrite(FrameJob &job)
{
    {
        QMutexLocker locker(&m_mutex);
        if (m_stats.failed)
            return;   // 已经失败（磁盘满等），继续消费队列但不再写
    }

    // 光标在编码前画上去：画在原图的副本上，不动队列里其他帧。
    // 光标位置是抓帧那一刻采样的，比"写盘时再取一次"准确。
    if (job.drawCursor && job.cursorInside)
        drawCursor(&job.frame, job.cursor, job.cursorPos);

    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    if (!job.frame.save(&buffer, "JPG", m_quality)) {
        buffer.close();
        markFailed(QStringLiteral("JPEG 编码失败"));
        return;
    }
    buffer.close();

    if (jpeg.isEmpty()) {
        markFailed(QStringLiteral("JPEG 编码结果为空"));
        return;
    }

    const int copies = qBound(1, job.copies, 4);
    for (int i = 0; i < copies; ++i) {
        // 补帧复用同一份 JPEG 字节，所以第 2~4 帧除了写盘几乎不花时间。
        if (!m_writer->addFrame(jpeg)) {
            markFailed(m_writer->errorString().isEmpty()
                           ? QStringLiteral("写入视频帧失败")
                           : m_writer->errorString());
            return;
        }
    }

    QMutexLocker locker(&m_mutex);
    m_stats.frames += copies;
    m_stats.dups += copies - 1;
    m_stats.bytes += qint64(jpeg.size()) * copies;
}

void RecorderWorker::markFailed(const QString &message)
{
    QMutexLocker locker(&m_mutex);
    if (m_stats.failed)
        return;   // 只保留第一个错误：后续的多半是同一个原因的连锁反应
    m_stats.failed = true;
    m_stats.error = message;
}

WorkerStats RecorderWorker::stats() const
{
    QMutexLocker locker(&m_mutex);
    return m_stats;
}

} // namespace screenrec
