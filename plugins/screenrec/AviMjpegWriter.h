#pragma once

// ---------------------------------------------------------------------------
// MJPEG-in-AVI 封装器（纯 header，只依赖 QtCore，可被临时工程直接 include 做断言）。
//
// 为什么自己写而不调 ffmpeg / 不引 Qt Multimedia：
//   * 目标机器上没有 ffmpeg，也没装 Qt Multimedia 模块，两者都会变成"依赖"；
//   * 单流 MJPEG 的 AVI 结构非常小（一个固定 224 字节的头 + 每帧一个带头的块 +
//     末尾一张索引表），手写比引依赖更可控，也与仓库里 qrcode 自研编解码的风格一致。
//
// 已知取舍（写进 README 的边界）：
//   * **恒定帧率**：AVI(MJPEG) 没有逐帧时间戳，时长 = 帧数 / fps。抖动只能靠
//     "补写上一帧"来吸收，由调用方（RecorderWorker）负责，本类只按顺序收帧；
//   * 只能装 JPEG 帧，且每帧都是关键帧（AVIIF_KEYFRAME），没有 B/P 帧与音频流。
//
// 文件布局（偏移为绝对文件偏移，全部小端）：
//
//   0    'RIFF'   size@4  = 224 + S + 16n        ← finish() 最后回填
//   8    'AVI '
//   12   'LIST'   size@16 = 192
//   20   'hdrl'
//   24   'avih'   size@28 = 56
//   32   MainAVIHeader[56]
//   88   'LIST'   size@92 = 116
//   96   'strl'
//   100  'strh'   size@104 = 56
//   108  AVIStreamHeader[56]
//   164  'strf'   size@168 = 40
//   172  BITMAPINFOHEADER[40]
//   212  'LIST'   size@216 = 4 + S               ← 每秒 checkpoint 回填
//   220  'movi'                                   ← idx1 偏移基准点
//   224  第一帧 '00dc'
//   224+S 'idx1'  size = 16n
//
// 其中 S = Σ(8 + len_i + (len_i & 1))，即各帧"块头 + 数据 + 奇数补位"之和。
// 恒等式（测试里锁死）：fileSize == 232 + S + 16n，riffSize == fileSize - 8。
// ---------------------------------------------------------------------------

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace screenrec {

namespace avi {

/// 头部固定长度 = 第一帧数据开始的位置。与画面尺寸、帧率、帧数都无关。
inline constexpr int kHeaderSize = 224;

// ---- 关键字段的绝对文件偏移。集中写死在这里，避免正文里撒满魔法数字。
inline constexpr int kOffRiffSize         = 4;    // RIFF 总长度 - 8
inline constexpr int kOffMainMaxBytes     = 36;   // avih.dwMaxBytesPerSec
inline constexpr int kOffMainFlags        = 44;   // avih.dwFlags
inline constexpr int kOffMainTotalFrames  = 48;   // avih.dwTotalFrames
inline constexpr int kOffMainBufferSize   = 60;   // avih.dwSuggestedBufferSize
inline constexpr int kOffMainWidth        = 64;   // avih.dwWidth（设备像素）
inline constexpr int kOffMainHeight       = 68;   // avih.dwHeight（设备像素）
inline constexpr int kOffStreamLength     = 140;  // strh.dwLength（帧数）
inline constexpr int kOffStreamBufferSize = 144;  // strh.dwSuggestedBufferSize
inline constexpr int kOffMoviListSize     = 216;  // LIST movi 的 size 字段
inline constexpr int kOffMoviFourcc       = 220;  // 'movi' 四个字母

/// avih.dwFlags 位：文件末尾带了 idx1 索引表。
inline constexpr quint32 kFlagHasIndex = 0x10;
/// idx1 条目标志位：这一帧是关键帧。MJPEG 每帧都是。
inline constexpr quint32 kFlagKeyFrame = 0x10;

/// 小端写 16 位。AVI/RIFX 规范全部小端，与主机字节序无关，不能直接 memcpy 结构体。
inline void putU16(QByteArray &buf, int offset, quint16 value)
{
    if (offset < 0 || offset + 2 > buf.size())
        return;
    buf[offset + 0] = char(value & 0xFF);
    buf[offset + 1] = char((value >> 8) & 0xFF);
}

/// 小端写 32 位。越界时静默返回：所有调用点的偏移都由常量给出，越界只可能是代码写错，
/// 而封装器不该为此抛异常打断正在进行的录制。
inline void putU32(QByteArray &buf, int offset, quint32 value)
{
    if (offset < 0 || offset + 4 > buf.size())
        return;
    buf[offset + 0] = char(value & 0xFF);
    buf[offset + 1] = char((value >> 8) & 0xFF);
    buf[offset + 2] = char((value >> 16) & 0xFF);
    buf[offset + 3] = char((value >> 24) & 0xFF);
}

///
/// 构造 224 字节的固定头部（帧数、长度等"事后才知道"的字段先写 0，finish 时回填）。
///
/// @param width  视频宽度，**设备像素**（不是逻辑像素：写逻辑值会让 4K 屏录出来的
///               文件元数据是 1080p，而画面是 4K）
/// @param height 同上
/// @param fps    目标帧率，必须是正整数（写入 dwRate，dwScale 固定 1）
///
inline QByteArray buildHeader(int width, int height, int fps)
{
    QByteArray h(kHeaderSize, '\0');

    // ---- RIFF 容器
    h.replace(0, 4, "RIFF");
    putU32(h, kOffRiffSize, 0);   // 占位
    h.replace(8, 4, "AVI ");

    // ---- LIST hdrl（size 恒 192：4 + avih 块 64 + strl LIST 124）
    h.replace(12, 4, "LIST");
    putU32(h, 16, 192);
    h.replace(20, 4, "hdrl");

    // ---- avih：MainAVIHeader
    h.replace(24, 4, "avih");
    putU32(h, 28, 56);
    // dwMicroSecPerFrame：只能是 1e6/fps 的四舍五入，24fps 会被写成 41667。
    // 播放器算时长用的是这个值 × 帧数，误差小于 1 微秒/帧，累积 1 小时也不到 4ms。
    putU32(h, 32, quint32(qRound(1000000.0 / qMax(1, fps))));
    putU32(h, kOffMainMaxBytes, 0);      // 数据率，finish 回填
    putU32(h, 40, 0);                    // dwPaddingGranularity
    putU32(h, kOffMainFlags, 0);         // 先不带索引，finish 成功后再置位
    putU32(h, kOffMainTotalFrames, 0);   // 回填
    putU32(h, 52, 0);                    // dwInitialFrames
    putU32(h, 56, 1);                    // dwStreams：只有一条视频流
    putU32(h, kOffMainBufferSize, 0);    // 回填
    putU32(h, kOffMainWidth, quint32(qMax(1, width)));
    putU32(h, kOffMainHeight, quint32(qMax(1, height)));
    // dwReserved[4] @72..88 保持 0

    // ---- LIST strl（size 恒 116：4 + strh 块 64 + strf 块 48）
    h.replace(88, 4, "LIST");
    putU32(h, 92, 116);
    h.replace(96, 4, "strl");

    // ---- strh：AVIStreamHeader
    h.replace(100, 4, "strh");
    putU32(h, 104, 56);
    h.replace(108, 4, "vids");
    h.replace(112, 4, "MJPG");
    putU32(h, 116, 0);            // dwFlags
    putU16(h, 120, 0);            // wPriority
    putU16(h, 122, 0);            // wLanguage
    putU32(h, 124, 0);            // dwInitialFrames
    putU32(h, 128, 1);            // dwScale
    putU32(h, 132, quint32(qMax(1, fps)));   // dwRate，rate/scale == fps
    putU32(h, 136, 0);            // dwStart
    putU32(h, kOffStreamLength, 0);          // dwLength：帧数，回填
    putU32(h, kOffStreamBufferSize, 0);      // 回填
    putU32(h, 148, 0xFFFFFFFF);   // dwQuality = -1，表示用编码器默认
    // dwSampleSize 必须为 0：MJPEG 每帧长度不同，写了会被当成固定帧长去算时长。
    putU32(h, 152, 0);
    putU16(h, 156, 0);                                            // rcFrame.left
    putU16(h, 158, 0);                                            // rcFrame.top
    putU16(h, 160, quint16(qMin(qMax(1, width), 0x7FFF)));         // rcFrame.right
    putU16(h, 162, quint16(qMin(qMax(1, height), 0x7FFF)));        // rcFrame.bottom

    // ---- strf：BITMAPINFOHEADER
    h.replace(164, 4, "strf");
    putU32(h, 168, 40);
    putU32(h, 172, 40);                                   // biSize
    putU32(h, 176, quint32(qMax(1, width)));              // biWidth
    // biHeight 写**正数**：MJPEG 的实际方向由 JPEG 码流自己决定，解码器不读这个
    // 符号；写负数会被 VirtualDub 这类按 vfw 语义解析的老工具上下翻转。
    putU32(h, 180, quint32(qMax(1, height)));             // biHeight
    putU16(h, 184, 1);                                    // biPlanes
    putU16(h, 186, 24);                                   // biBitCount
    h.replace(188, 4, "MJPG");                            // biCompression
    // biSizeImage 写 0：它表示"未压缩图像长度"，对逐帧变化的 JPEG 没有意义。
    // 写 w*h*3 也能播（ffmpeg 忽略），写某一帧的长度则是错的。
    putU32(h, 192, 0);
    putU32(h, 196, 0);   // biXPelsPerMeter
    putU32(h, 200, 0);   // biYPelsPerMeter
    putU32(h, 204, 0);   // biClrUsed
    putU32(h, 208, 0);   // biClrImportant

    // ---- LIST movi
    h.replace(212, 4, "LIST");
    putU32(h, kOffMoviListSize, 4);   // 回填 4 + S
    h.replace(kOffMoviFourcc, 4, "movi");

    return h;
}

} // namespace avi

///
/// 流式写的 MJPEG AVI 写入器。
///
/// 生命周期：open() → addFrame() × n → finish()。析构函数会自动 finish()，
/// 所以"面板被销毁""进程正常退出"这两条路径不会留下半截文件。
///
/// 崩溃/强杀的情况靠 checkpoint()：每约一秒把帧数、流长度、movi size 回填一次。
/// 这样即使进程被 taskkill，文件仍有正确的时长与帧数，只是没有 idx1 索引
/// （dwFlags 里不带 AVIF_HASINDEX，播放器会退化为顺序扫描，画面完全正常）。
///
class AviMjpegWriter
{
public:
    AviMjpegWriter() = default;

    AviMjpegWriter(const AviMjpegWriter &) = delete;
    AviMjpegWriter &operator=(const AviMjpegWriter &) = delete;

    /// 析构即收尾：正在录的视频必须可播，不能因为"没人调 finish"就丢掉。
    ~AviMjpegWriter() { finish(); }

    ///
    /// 打开文件并写入占位头。
    ///
    /// @param path   目标文件，父目录必须已存在（调用方负责 mkpath）
    /// @param width  设备像素宽
    /// @param height 设备像素高
    /// @param fps    目标帧率（正整数）
    /// @param error  失败时写入中文原因，可为 nullptr
    ///
    bool open(const QString &path, int width, int height, int fps, QString *error)
    {
        if (m_open)
            finish();

        if (width <= 0 || height <= 0) {
            m_error = QStringLiteral("视频尺寸无效（%1×%2）").arg(width).arg(height);
            if (error)
                *error = m_error;
            return false;
        }

        m_file.setFileName(path);
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            m_error = QStringLiteral("无法写入文件：%1（%2）").arg(path, m_file.errorString());
            if (error)
                *error = m_error;
            return false;
        }

        m_fps = qMax(1, fps);
        m_header = avi::buildHeader(width, height, m_fps);

        if (m_file.write(m_header) != m_header.size()) {
            m_error = QStringLiteral("写入文件头失败：%1").arg(m_file.errorString());
            m_file.close();
            if (error)
                *error = m_error;
            return false;
        }

        clearState();
        m_open = true;
        m_endPos = avi::kHeaderSize;
        m_path = path;

        // 约半秒回填一次头部。用帧数而不是挂时钟，避免在工作线程里再引入一个 QTimer。
        // 越频繁越能少丢——每次回填只是写 224 字节，成本可忽略（见 checkpoint()）。
        m_checkpointEvery = qMax(8, m_fps / 2);
        return true;
    }

    ///
    /// 追加一帧 JPEG。调用方保证传入的是完整可解码的 JPEG 字节。
    ///
    /// 写失败（磁盘满、路径被删）后本对象进入失败态：后续调用直接返回 false，
    /// 但 finish() 仍会把已写的部分收成一个可顺序播放的文件。
    ///
    bool addFrame(const QByteArray &jpeg)
    {
        if (!m_open || m_failed || jpeg.isEmpty())
            return false;

        QByteArray chunk(8, '\0');
        chunk.replace(0, 4, "00dc");
        avi::putU32(chunk, 4, quint32(jpeg.size()));

        bool ok = (m_file.write(chunk) == chunk.size())
                  && (m_file.write(jpeg) == jpeg.size());

        // RIFF 要求每个块的长度是偶数，奇数长度必须补 1 字节。补位字节**计入**
        // movi 与 RIFF 的长度，但**不计入**块头里的 size，也不计入 idx1 的
        // chunkLength —— 这两处搞混就会出现"能播但拖到最后一帧卡住"。
        const int pad = jpeg.size() & 1;
        if (ok && pad == 1)
            ok = (m_file.write("\0", 1) == 1);

        if (!ok) {
            m_failed = true;
            m_error = QStringLiteral("写入视频帧失败：%1").arg(m_file.errorString());
            return false;
        }

        IndexEntry entry;
        entry.offset = quint32(m_moviRelative);
        entry.length = quint32(jpeg.size());
        m_index.append(entry);

        const int consumed = 8 + jpeg.size() + pad;
        m_moviRelative += consumed;
        m_dataBytes += consumed;
        m_endPos += consumed;
        m_maxFrameBytes = qMax(m_maxFrameBytes, jpeg.size());
        m_framesSinceCheckpoint += 1;

        if (m_framesSinceCheckpoint >= m_checkpointEvery)
            checkpoint();

        return true;
    }

    ///
    /// 收尾：回填长度、写 idx1 索引、关闭文件。幂等，可重复调用。
    ///
    /// 回填顺序有讲究：riffSize **最后**写。它相当于"文件到此为止"的截止点，
    /// 先放大会让正在读这个文件的播放器读到还没写完的区域。
    ///
    bool finish()
    {
        if (!m_open)
            return !m_failed;
        if (m_finished)
            return !m_failed;
        m_finished = true;

        const quint32 frames = quint32(m_index.size());

        patchCounters();
        avi::putU32(m_header, avi::kOffMoviListSize, quint32(4 + m_dataBytes));

        // 只有真的写出了索引才置 AVIF_HASINDEX。写盘失败时诚实地说"没索引"，
        // 播放器会退化为顺序扫描，而不是按一张不可靠的表去 seek。
        const bool withIndex = !m_failed && frames > 0;
        avi::putU32(m_header, avi::kOffMainFlags, withIndex ? avi::kFlagHasIndex : 0);

        if (m_file.seek(0) && m_file.write(m_header) != m_header.size()) {
            m_failed = true;
            m_error = QStringLiteral("回填文件头失败：%1").arg(m_file.errorString());
        }

        qint64 endPos = m_endPos;
        if (withIndex) {
            QByteArray idx(8, '\0');
            idx.replace(0, 4, "idx1");
            avi::putU32(idx, 4, quint32(16 * frames));

            QByteArray entries;
            entries.reserve(static_cast<int>(16 * frames));
            for (const IndexEntry &e : m_index) {
                QByteArray item(16, '\0');
                item.replace(0, 4, "00dc");
                avi::putU32(item, 4, avi::kFlagKeyFrame);
                avi::putU32(item, 8, e.offset);
                avi::putU32(item, 12, e.length);
                entries += item;
            }

            const bool wrote = m_file.seek(endPos)
                               && m_file.write(idx) == idx.size()
                               && m_file.write(entries) == entries.size();
            if (!wrote) {
                m_failed = true;
                m_error = QStringLiteral("写入索引失败：%1").arg(m_file.errorString());
            } else {
                endPos += 8 + entries.size();
            }
        }

        avi::putU32(m_header, avi::kOffRiffSize, quint32(endPos - 8));
        if (m_file.seek(avi::kOffRiffSize)
            && m_file.write(m_header.mid(avi::kOffRiffSize, 4)) != 4) {
            m_failed = true;
            m_error = QStringLiteral("回填文件长度失败：%1").arg(m_file.errorString());
        }

        m_file.flush();
        m_file.close();
        m_open = false;
        return !m_failed;
    }

    /// 放弃这次录制：先按 finish 收尾，再把文件删掉。只用于"一帧都没录成"的清理。
    void abort()
    {
        const QString path = m_path;
        finish();
        if (!path.isEmpty())
            QFile::remove(path);
        clearState();
        m_path.clear();
    }

    bool isOpen() const { return m_open; }
    qint64 frameCount() const { return m_index.size(); }
    qint64 dataBytes() const { return m_dataBytes; }
    int maxFrameBytes() const { return m_maxFrameBytes; }
    QString errorString() const { return m_error; }
    QString filePath() const { return m_path; }

private:
    /// idx1 条目。四字符 id 与标志位是常量，不必逐项存。
    struct IndexEntry
    {
        quint32 offset = 0;   ///< 相对 'movi' 四字符的偏移，第一帧恒为 4
        quint32 length = 0;   ///< 帧数据长度，不含 8 字节块头、不含补位
    };

    void clearState()
    {
        m_index.clear();
        m_dataBytes = 0;
        m_moviRelative = 4;   // 'movi' 之后紧跟第一帧的块头
        m_maxFrameBytes = 0;
        m_endPos = avi::kHeaderSize;
        m_framesSinceCheckpoint = 0;
        m_failed = false;
        m_finished = false;
    }

    /// 把"事后才知道"的计数写进内存里的头部副本（不落盘）。
    void patchCounters()
    {
        const quint32 frames = quint32(m_index.size());
        avi::putU32(m_header, avi::kOffMainTotalFrames, frames);
        avi::putU32(m_header, avi::kOffStreamLength, frames);
        avi::putU32(m_header, avi::kOffMainBufferSize, quint32(m_maxFrameBytes));
        avi::putU32(m_header, avi::kOffStreamBufferSize, quint32(m_maxFrameBytes));

        quint32 rate = 0;
        if (frames > 0 && m_fps > 0) {
            rate = quint32(qRound(double(m_dataBytes) * m_fps / double(frames)));
        }
        avi::putU32(m_header, avi::kOffMainMaxBytes, rate);
    }

    /// 中途回填：更新帧数/长度/movi size/riffSize，不写索引、不置 AVIF_HASINDEX。
    /// 进程被强杀时，保住的就是这一份信息。
    ///
    /// riffSize 也必须回填：open() 时它写的是 0，如果强杀前一直没更新，严格按规范
    /// 解析的工具会认为文件是空的。写成"当前末尾 - 8"对正在进行录制的文件是精确的。
    ///
    /// 刻意不 flush：write 已经进 OS page cache，进程被 taskkill 不会丢，只有断电
    /// 才会丢；而每半秒一次 fsync 在慢盘上能吃掉几十毫秒，会拖慢写盘线程、进而让
    /// 帧队列堆积。finish() 时才真正 flush。
    void checkpoint()
    {
        if (!m_open || m_failed)
            return;

        patchCounters();
        avi::putU32(m_header, avi::kOffMoviListSize, quint32(4 + m_dataBytes));
        avi::putU32(m_header, avi::kOffRiffSize, quint32(m_endPos - 8));

        const qint64 back = m_file.pos();
        if (m_file.seek(0))
            m_file.write(m_header);
        m_file.seek(back);

        m_framesSinceCheckpoint = 0;
    }

    QFile m_file;
    QByteArray m_header;
    QVector<IndexEntry> m_index;
    QString m_path;
    QString m_error;

    qint64 m_endPos = avi::kHeaderSize;   ///< 当前写入末尾（收尾时从这里追加 idx1）
    qint64 m_dataBytes = 0;               ///< S：所有块头+数据+补位之和
    qint64 m_moviRelative = 4;            ///< 下一帧块头相对 'movi' 的偏移
    int m_maxFrameBytes = 0;
    int m_fps = 15;
    int m_checkpointEvery = 15;
    int m_framesSinceCheckpoint = 0;
    bool m_open = false;
    bool m_finished = false;
    bool m_failed = false;
};

} // namespace screenrec
