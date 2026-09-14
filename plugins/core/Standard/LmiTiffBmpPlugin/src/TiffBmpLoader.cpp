#include "TiffBmpLoader.h"

#include <ccPointCloud.h>
#include <ccMesh.h>

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QImageReader>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QTextStream>
#include <QtEndian>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <unordered_set>
#include <vector>

namespace TiffBmpLoader
{

// ── 1537 色彩映射表：Blue→Cyan→Green→Yellow→Red→Magenta→White ──────────────
// 与 GetColorFromHeight2(normalizedHeight) 完全对齐：
//   x = int(1536 * normalizedHeight)，0→Blue，1536→White
static ccColor::Rgb lutEntry(int i)
{
    i = std::max(0, std::min(i, 1536));
    quint8 r, g, b;
    if      (i <  256) { r=0;   g=(quint8)i;                          b=255; }
    else if (i <  512) { r=0;   g=255;                                b=(quint8)(511-i); }
    else if (i <  768) { r=(quint8)(i-512);  g=255;                   b=0; }
    else if (i < 1024) { r=255; g=(quint8)(1023-i);                   b=0; }
    else if (i < 1280) { r=255; g=0;                                  b=(quint8)(i-1024); }
    else               { r=255; g=(quint8)std::min(i-1280, 255);      b=255; } // Magenta→White，i=1536→White
    return ccColor::Rgb(r, g, b);
}

static const std::array<ccColor::Rgb, 1537>& colorLUT()
{
    static const auto lut = []() {
        std::array<ccColor::Rgb, 1537> a;
        for (int i = 0; i <= 1536; ++i)
            a[static_cast<size_t>(i)] = lutEntry(i);
        return a;
    }();
    return lut;
}

// ── 极简 TIFF 解析器（检测 32-bit float，读取非压缩数据）──────────────────

struct TiffIfdTag { quint16 type; quint32 count; quint32 value; };

static bool parseTiffIfd(const QString& path, bool& outLe,
                         QMap<quint16, TiffIfdTag>& outTags)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;

    quint8 hdr[8];
    if (f.read(reinterpret_cast<char*>(hdr), 8) != 8) return false;

    outLe = (hdr[0] == 'I' && hdr[1] == 'I');
    if (!outLe && !(hdr[0] == 'M' && hdr[1] == 'M')) return false;
    const bool le = outLe;

    auto u16 = [&](const quint8* p) -> quint16 {
        return le ? qFromLittleEndian<quint16>(p) : qFromBigEndian<quint16>(p);
    };
    auto u32 = [&](const quint8* p) -> quint32 {
        return le ? qFromLittleEndian<quint32>(p) : qFromBigEndian<quint32>(p);
    };

    if (u16(&hdr[2]) != 42) return false;
    const quint32 ifdOff = u32(&hdr[4]);
    if (!f.seek(ifdOff)) return false;

    quint8 nb[2];
    if (f.read(reinterpret_cast<char*>(nb), 2) != 2) return false;
    const int n = u16(nb);

    for (int i = 0; i < n; ++i) {
        quint8 e[12];
        if (f.read(reinterpret_cast<char*>(e), 12) != 12) break;
        TiffIfdTag t;
        const quint16 tag = u16(&e[0]);
        t.type  = u16(&e[2]);
        t.count = u32(&e[4]);
        t.value = u32(&e[8]);
        outTags[tag] = t;
    }
    return !outTags.isEmpty();
}

static quint32 tiffScalar(const QMap<quint16, TiffIfdTag>& tags,
                           quint16 tagId, quint32 def, bool le)
{
    auto it = tags.find(tagId);
    if (it == tags.end()) return def;
    if (it->type == 3)
        return le ? (it->value & 0xFFFF) : (it->value >> 16);
    return it->value;
}

static std::vector<quint32> readU32Array(const QString& path, bool le,
                                          const TiffIfdTag& tag)
{
    std::vector<quint32> arr;
    if (tag.count == 0) return arr;
    if (tag.count == 1) {
        quint32 v = (tag.type == 3)
            ? (le ? (tag.value & 0xFFFF) : (tag.value >> 16))
            : tag.value;
        arr.push_back(v);
        return arr;
    }
    // SHORT × 2 恰好 4 字节，内联存放于 value 字段，无需读文件
    if (tag.type == 3 && tag.count == 2) {
        if (le) {
            arr.push_back(tag.value & 0xFFFF);
            arr.push_back((tag.value >> 16) & 0xFFFF);
        } else {
            arr.push_back((tag.value >> 16) & 0xFFFF);
            arr.push_back(tag.value & 0xFFFF);
        }
        return arr;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(tag.value)) return arr;
    arr.reserve(tag.count);
    for (quint32 k = 0; k < tag.count; ++k) {
        if (tag.type == 3) {
            quint8 b[2]; f.read(reinterpret_cast<char*>(b), 2);
            arr.push_back(le ? qFromLittleEndian<quint16>(b) : qFromBigEndian<quint16>(b));
        } else {
            quint8 b[4]; f.read(reinterpret_cast<char*>(b), 4);
            arr.push_back(le ? qFromLittleEndian<quint32>(b) : qFromBigEndian<quint32>(b));
        }
    }
    return arr;
}

//! RATIONAL (type 5)：每个元素 8 字节（numerator + denominator，均为 LONG）
//! count=1 时 8 字节也无法内联（>4 字节），数据始终在 tag.value 指向的偏移处
static std::vector<double> readRationalArray(const QString& path, bool le,
                                              const TiffIfdTag& tag)
{
    std::vector<double> arr;
    if (tag.count == 0) return arr;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(tag.value)) return arr;
    arr.reserve(tag.count);
    for (quint32 k = 0; k < tag.count; ++k) {
        quint8 b[8];
        if (f.read(reinterpret_cast<char*>(b), 8) != 8) break;
        const quint32 num = le ? qFromLittleEndian<quint32>(b)
                                : qFromBigEndian<quint32>(b);
        const quint32 den = le ? qFromLittleEndian<quint32>(b + 4)
                                : qFromBigEndian<quint32>(b + 4);
        arr.push_back(den != 0 ? static_cast<double>(num) / static_cast<double>(den) : 0.0);
    }
    return arr;
}

//! DOUBLE (type 12)：每个元素 8 字节 IEEE 754 双精度
static std::vector<double> readDoubleArray(const QString& path, bool le,
                                            const TiffIfdTag& tag)
{
    std::vector<double> arr;
    if (tag.count == 0) return arr;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(tag.value)) return arr;
    arr.reserve(tag.count);
    for (quint32 k = 0; k < tag.count; ++k) {
        quint8 b[8];
        if (f.read(reinterpret_cast<char*>(b), 8) != 8) break;
        quint64 bits = le ? qFromLittleEndian<quint64>(b)
                           : qFromBigEndian<quint64>(b);
        double v;
        memcpy(&v, &bits, 8);
        arr.push_back(v);
    }
    return arr;
}

//! ASCII (type 2)：null 结尾字符串，count 包含 null 字节
//! count<=4 时数据内联于 value 字段
static QString readAsciiString(const QString& path, bool le,
                                const TiffIfdTag& tag)
{
    if (tag.count == 0) return {};
    QByteArray data;
    if (tag.count <= 4) {
        // 内联：tag.value 的 4 字节即为字符串数据（按文件字节序排列）
        // little-endian 文件：低字节在前；big-endian 文件：高字节在前
        quint8 b[4];
        if (le) {
            b[0] = static_cast<quint8>( tag.value        & 0xFF);
            b[1] = static_cast<quint8>((tag.value >> 8)  & 0xFF);
            b[2] = static_cast<quint8>((tag.value >> 16) & 0xFF);
            b[3] = static_cast<quint8>((tag.value >> 24) & 0xFF);
        } else {
            b[0] = static_cast<quint8>((tag.value >> 24) & 0xFF);
            b[1] = static_cast<quint8>((tag.value >> 16) & 0xFF);
            b[2] = static_cast<quint8>((tag.value >> 8)  & 0xFF);
            b[3] = static_cast<quint8>( tag.value        & 0xFF);
        }
        data = QByteArray(reinterpret_cast<const char*>(b),
                          static_cast<int>(std::min<quint32>(tag.count, 4)));
    } else {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly) || !f.seek(tag.value)) return {};
        data = f.read(tag.count);
    }
    // 去掉尾部 null
    while (!data.isEmpty() && data.back() == '\0')
        data.chop(1);
    return QString::fromUtf8(data);
}

//! 解析 LMI Gocator ImageDescription 中 key=value 行（大小写不敏感的键名）
//! 已知键：Unit, XResolution_mm, YResolution_mm, ZResolution_mm,
//!         XOffset_mm, YOffset_mm, ZOffset_mm, Storage
static void parseLmiGocatorDescription(const QString& text, TiffInfo& info)
{
    const QStringList lines = text.split(QRegularExpression("[\\r\\n]+"),
                                          Qt::SkipEmptyParts);
    bool anyResolution = false;
    bool anyOffset     = false;
    for (const QString& rawLine : lines) {
        const int eq = rawLine.indexOf('=');
        if (eq <= 0) continue;
        const QString key = rawLine.left(eq).trimmed();
        const QString val = rawLine.mid(eq + 1).trimmed();
        bool ok = false;
        const double dv = val.toDouble(&ok);

        if (key.compare("XResolution_mm", Qt::CaseInsensitive) == 0 && ok) {
            info.embeddedResX = dv; info.hasEmbeddedResX = true; anyResolution = true;
        } else if (key.compare("YResolution_mm", Qt::CaseInsensitive) == 0 && ok) {
            info.embeddedResY = dv; info.hasEmbeddedResY = true; anyResolution = true;
        } else if (key.compare("ZResolution_mm", Qt::CaseInsensitive) == 0 && ok) {
            info.embeddedResZ = dv; info.hasEmbeddedResZ = true; anyResolution = true;
        } else if (key.compare("XOffset_mm", Qt::CaseInsensitive) == 0 && ok) {
            info.embeddedOffX = dv; info.hasEmbeddedOffX = true; anyOffset = true;
        } else if (key.compare("YOffset_mm", Qt::CaseInsensitive) == 0 && ok) {
            info.embeddedOffY = dv; info.hasEmbeddedOffY = true; anyOffset = true;
        } else if (key.compare("ZOffset_mm", Qt::CaseInsensitive) == 0 && ok) {
            info.embeddedOffZ = dv; info.hasEmbeddedOffZ = true; anyOffset = true;
        } else if (key.compare("Storage", Qt::CaseInsensitive) == 0) {
            // Storage=UInt16RawPlus32768 表示 uint16 原始值需减 32768 还原为有符号
            if (val.compare("UInt16RawPlus32768", Qt::CaseInsensitive) == 0)
                info.embeddedUInt16Plus32768 = true;
        }
    }
    if (anyResolution || anyOffset)
        info.embeddedMetaSource = QStringLiteral("ImageDescription");
}

static QVector<quint32> toQVector(const std::vector<quint32>& values)
{
    QVector<quint32> out;
    out.reserve(static_cast<int>(values.size()));
    for (quint32 v : values)
        out.push_back(v);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// 内置 TIFF LZW 解码器（MSB-first，9-12 bit 变长，TIFF 早切码长）
//
// 用于绕过 Qt 内置 TIFF 解码器对 LZW+Predictor=2 支持不稳定的问题
// （观测到对 LMI Gocator 导出的 14113×7230 单行 strip + Predictor=2 直接崩溃）。
// 算法参考 TIFF 6.0 规范 + libtiff tif_lzw.c 的早切码长（"early change"）行为。
// ─────────────────────────────────────────────────────────────────────────────
namespace LzwTiff {
    static constexpr int kClearCode = 256;
    static constexpr int kEoiCode   = 257;
    static constexpr int kFirstFree = 258;
    static constexpr int kMaxCode   = 4096;        //!< 2^12，TIFF LZW 最大码

    //! 返回 false 表示解码失败（位流过短 / 数据损坏 / 异常码）
    //! @param expectedSize 期望的解压后字节数（用于预分配；解码遇 EOI 即停）
    static bool decodeStrip(const QByteArray& src,
                            QByteArray& dst,
                            int expectedSize)
    {
        dst.clear();
        if (expectedSize > 0)
            dst.reserve(expectedSize);

        const quint8* in  = reinterpret_cast<const quint8*>(src.constData());
        const int    inBytes = src.size();
        if (inBytes < 2) return false;

        // 字典：每条记录是一段已解码字符串
        // 用 (prefix, suffix) 链表表示：dict[code] = (prefix code, last byte)
        // 长度上限 = kMaxCode；前 258 项为内建（0..255 单字节、256/257 控制码）
        std::array<int,  kMaxCode> dictPrefix{};
        std::array<quint8, kMaxCode> dictSuffix{};
        int nextEntry = kFirstFree;
        int codeSize  = 9;     // 初始码长（首个 ClearCode 后）
        int prevCode  = -1;

        // MSB-first 位流读取器
        quint64 bitBuf = 0;
        int     bitCnt = 0;
        int     bytePos = 0;
        auto readCode = [&](int n) -> int {
            while (bitCnt < n) {
                if (bytePos >= inBytes) return -1;
                bitBuf = (bitBuf << 8) | in[bytePos++];
                bitCnt += 8;
            }
            const int code = static_cast<int>((bitBuf >> (bitCnt - n)) & ((1ULL << n) - 1));
            bitCnt -= n;
            return code;
        };

        // 写出字典条目对应字符串（先递归构建到栈，再反向 append）
        std::array<quint8, kMaxCode> stack{};
        auto emitCode = [&](int code, quint8* outFirst) -> bool {
            int sp = 0;
            int c = code;
            while (c >= kFirstFree) {
                if (sp >= kMaxCode || c < 0 || c >= kMaxCode) return false;
                stack[sp++] = dictSuffix[c];
                c = dictPrefix[c];
                if (c < 0) return false;
            }
            if (c < 0 || c > 255) return false;
            stack[sp++] = static_cast<quint8>(c);
            // 反向追加（字符串首字节是最后一个进栈的）
            for (int i = sp - 1; i >= 0; --i)
                dst.append(static_cast<char>(stack[i]));
            *outFirst = stack[sp - 1];
            return true;
        };

        // 解码主循环
        while (true) {
            const int code = readCode(codeSize);
            if (code < 0) break;
            if (code == kEoiCode) break;
            if (code == kClearCode) {
                nextEntry = kFirstFree;
                codeSize  = 9;
                prevCode  = -1;
                continue;
            }
            quint8 firstByte = 0;
            if (prevCode < 0) {
                // ClearCode 后首个码：必须是 0..255（单字节）
                if (code > 255) return false;
                dst.append(static_cast<char>(code));
                firstByte = static_cast<quint8>(code);
            } else if (code < nextEntry) {
                if (!emitCode(code, &firstByte)) return false;
            } else if (code == nextEntry) {
                // KwKwK 特殊情况：字典中尚未写入此条，串 = prev + prev[0]
                quint8 prevFirst = 0;
                if (!emitCode(prevCode, &prevFirst)) return false;
                dst.append(static_cast<char>(prevFirst));
                firstByte = prevFirst;
            } else {
                return false; // 非法码
            }
            // 添加新词条：prev + firstByte
            if (prevCode >= 0 && nextEntry < kMaxCode) {
                dictPrefix[nextEntry] = prevCode;
                dictSuffix[nextEntry] = firstByte;
                ++nextEntry;
                // TIFF "early change"：当 nextEntry 等于 (1<<codeSize) - 1 时切码长
                // （比普通 LZW 早一步切换；libtiff 的实现行为）
                if (nextEntry == (1 << codeSize) - 1 && codeSize < 12)
                    ++codeSize;
            }
            prevCode = code;
        }
        return !dst.isEmpty();
    }
} // namespace LzwTiff

// ─────────────────────────────────────────────────────────────────────────────
// 16-bit LZW (+ 可选 Predictor=2) strip 读取器
// 返回长度 W*H 的 quint16 数组（机器字节序）。失败返回空向量。
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<quint16> tryReadTiff16Lzw(const QString& path, const TiffInfo& info)
{
    if (!info.valid || info.compression != 5)         return {};
    if (info.samplesPerPixel != 1)                    return {};
    if (info.bitsPerSample.value(0, 0) != 16)         return {};
    if (info.planarConfig != 1)                       return {};
    if (info.stripOffsets.isEmpty()
        || info.stripOffsets.size() != info.stripByteCounts.size())
        return {};

    const int W = info.width;
    const int H = info.height;
    if (W <= 0 || H <= 0) return {};

    // 每像素 2 字节；strip 的未压缩字节数 = rowsPerStrip × W × 2
    const int rowsPerStrip = (info.rowsPerStrip > 0) ? info.rowsPerStrip : H;
    const int bpp          = 2;
    const int rowBytes     = W * bpp;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};

    std::vector<quint16> out(static_cast<size_t>(W) * static_cast<size_t>(H), 0);
    int rowCursor = 0;
    const bool le = info.littleEndian;
    const bool needPredictor = (info.predictor == 2);

    QByteArray decoded;
    for (int s = 0; s < info.stripOffsets.size() && rowCursor < H; ++s) {
        if (!f.seek(info.stripOffsets[s])) return {};
        const QByteArray raw = f.read(info.stripByteCounts[s]);
        if (raw.isEmpty()) return {};

        // 本 strip 实际行数：除最后一个 strip 外都 == rowsPerStrip
        const int rowsThisStrip = std::min(rowsPerStrip, H - rowCursor);
        const int expectedBytes = rowsThisStrip * rowBytes;

        if (!LzwTiff::decodeStrip(raw, decoded, expectedBytes))
            return {};
        if (decoded.size() < expectedBytes)
            return {};

        const quint8* dp = reinterpret_cast<const quint8*>(decoded.constData());
        for (int r = 0; r < rowsThisStrip; ++r) {
            // 解释字节为 16-bit 样本（TIFF 字节序）
            quint16* dst = out.data()
                + static_cast<size_t>(rowCursor + r) * static_cast<size_t>(W);
            const quint8* rowSrc = dp + r * rowBytes;
            if (le) {
                for (int x = 0; x < W; ++x)
                    dst[x] = static_cast<quint16>(rowSrc[2*x] | (rowSrc[2*x+1] << 8));
            } else {
                for (int x = 0; x < W; ++x)
                    dst[x] = static_cast<quint16>((rowSrc[2*x] << 8) | rowSrc[2*x+1]);
            }
            // Predictor=2：水平差分还原（u16 模运算）
            if (needPredictor) {
                for (int x = 1; x < W; ++x)
                    dst[x] = static_cast<quint16>(dst[x] + dst[x - 1]);
            }
        }
        rowCursor += rowsThisStrip;
    }

    if (rowCursor < H) return {}; // 数据不完整
    return out;
}

static bool buildTiffInfo(const QString& path, TiffInfo& outInfo)
{
    outInfo = TiffInfo{};

    bool le = true;
    QMap<quint16, TiffIfdTag> tags;
    if (!parseTiffIfd(path, le, tags))
        return false;

    outInfo.littleEndian    = le;
    outInfo.width           = static_cast<int>(tiffScalar(tags, 256, 0, le));
    outInfo.height          = static_cast<int>(tiffScalar(tags, 257, 0, le));
    outInfo.samplesPerPixel = static_cast<int>(tiffScalar(tags, 277, 1, le));
    outInfo.compression     = static_cast<int>(tiffScalar(tags, 259, 1, le));
    outInfo.planarConfig    = static_cast<int>(tiffScalar(tags, 284, 1, le));
    outInfo.predictor       = static_cast<int>(tiffScalar(tags, 317, 1, le));
    outInfo.rowsPerStrip    = static_cast<int>(tiffScalar(tags, 278, 0, le));

    if (tags.contains(258))
        outInfo.bitsPerSample = toQVector(readU32Array(path, le, tags[258]));
    if (tags.contains(339))
        outInfo.sampleFormats = toQVector(readU32Array(path, le, tags[339]));
    if (tags.contains(273))
        outInfo.stripOffsets = toQVector(readU32Array(path, le, tags[273]));
    if (tags.contains(279))
        outInfo.stripByteCounts = toQVector(readU32Array(path, le, tags[279]));

    if (outInfo.bitsPerSample.isEmpty())
        outInfo.bitsPerSample.push_back(static_cast<quint32>(tiffScalar(tags, 258, 16, le)));
    if (outInfo.sampleFormats.isEmpty())
        outInfo.sampleFormats.push_back(static_cast<quint32>(tiffScalar(tags, 339, 1, le)));

    const int firstBits = outInfo.bitsPerSample.value(0, 16);
    const int firstFmt  = outInfo.sampleFormats.value(0, 1);
    const int secondBits = outInfo.bitsPerSample.value(1, 0);
    const int secondFmt  = outInfo.sampleFormats.value(1, 1);

    const int thirdBits = outInfo.bitsPerSample.value(2, 0);
    const int thirdFmt  = outInfo.sampleFormats.value(2, 1);

    // 3 通道 32-bit float：每通道一个世界坐标分量 (X / Y / Z)，原始数据已为 mm
    outInfo.is96BitXYZ = (outInfo.samplesPerPixel == 3
        && firstBits  == 32 && firstFmt  == 3
        && secondBits == 32 && secondFmt == 3
        && thirdBits  == 32 && thirdFmt  == 3
        && outInfo.compression == 1);

    outInfo.is2Channel = (!outInfo.is96BitXYZ
        && outInfo.samplesPerPixel == 2
        && secondBits == 32
        && secondFmt == 3
        && outInfo.compression == 1);
    outInfo.is32Float = (!outInfo.is96BitXYZ
        && !outInfo.is2Channel
        && outInfo.samplesPerPixel == 1
        && firstBits == 32
        && firstFmt == 3
        && (outInfo.compression == 1 || outInfo.compression == 5));
    outInfo.is16BitSigned = (!outInfo.is96BitXYZ
        && !outInfo.is2Channel
        && !outInfo.is32Float
        && firstFmt == 2);

    outInfo.bitDepth = outInfo.is96BitXYZ ? 96
                     : outInfo.is2Channel ? 128
                     : outInfo.is32Float ? 32
                     : outInfo.is16BitSigned ? 17
                     : 16;
    outInfo.valid = (outInfo.width > 0 && outInfo.height > 0);

    // ── 嵌入式 3D 标定元数据解析（按优先级覆盖）────────────────────────────
    // 优先级 3（最低，仅 X/Y）：标准 TIFF XResolution(282)/YResolution(283) + ResolutionUnit(296)
    // ResolutionUnit: 1=无单位, 2=英寸, 3=厘米；存储的是 "每单位长度内的像素数"，物理间距 = 1/value
    {
        auto itXR = tags.find(282);
        auto itYR = tags.find(283);
        if (itXR != tags.end() && itYR != tags.end()
            && itXR->type == 5 && itYR->type == 5)
        {
            const auto xrArr = readRationalArray(path, le, itXR.value());
            const auto yrArr = readRationalArray(path, le, itYR.value());
            const int unit = static_cast<int>(tiffScalar(tags, 296, 2, le));
            // 单位换算到 mm：英寸=25.4，厘米=10，毫米=1，无单位/默认按厘米处理
            double unitMm = 10.0; // 默认 cm
            if (unit == 2)      unitMm = 25.4;
            else if (unit == 3) unitMm = 10.0;
            if (!xrArr.empty() && xrArr[0] > 0.0) {
                outInfo.embeddedResX = unitMm / xrArr[0];
                outInfo.hasEmbeddedResX = true;
            }
            if (!yrArr.empty() && yrArr[0] > 0.0) {
                outInfo.embeddedResY = unitMm / yrArr[0];
                outInfo.hasEmbeddedResY = true;
            }
            if (outInfo.hasEmbeddedResX || outInfo.hasEmbeddedResY)
                outInfo.embeddedMetaSource = QStringLiteral("XResolution");
        }
    }
    // 优先级 2：GeoTIFF ModelPixelScaleTag(33550) + ModelTiepointTag(33922)
    {
        auto itScale    = tags.find(33550);
        auto itTiePoint = tags.find(33922);
        if (itScale != tags.end() && itScale->type == 12 && itScale->count >= 3) {
            const auto v = readDoubleArray(path, le, itScale.value());
            if (v.size() >= 3) {
                if (v[0] > 0.0) { outInfo.embeddedResX = v[0]; outInfo.hasEmbeddedResX = true; }
                if (v[1] > 0.0) { outInfo.embeddedResY = v[1]; outInfo.hasEmbeddedResY = true; }
                if (v[2] > 0.0) { outInfo.embeddedResZ = v[2]; outInfo.hasEmbeddedResZ = true; }
                outInfo.embeddedMetaSource = QStringLiteral("ModelPixelScale");
            }
        }
        if (itTiePoint != tags.end() && itTiePoint->type == 12 && itTiePoint->count >= 6) {
            const auto v = readDoubleArray(path, le, itTiePoint.value());
            if (v.size() >= 6) {
                outInfo.embeddedOffX = v[3]; outInfo.hasEmbeddedOffX = true;
                outInfo.embeddedOffY = v[4]; outInfo.hasEmbeddedOffY = true;
                outInfo.embeddedOffZ = v[5]; outInfo.hasEmbeddedOffZ = true;
                if (outInfo.embeddedMetaSource.isEmpty())
                    outInfo.embeddedMetaSource = QStringLiteral("ModelTiepoint");
            }
        }
    }
    // 优先级 1（最高）：ImageDescription(270) LMI Gocator 文本
    {
        auto itDesc = tags.find(270);
        if (itDesc != tags.end() && itDesc->type == 2 && itDesc->count > 0) {
            const QString desc = readAsciiString(path, le, itDesc.value());
            if (desc.contains("LMI_Gocator", Qt::CaseInsensitive)
                || desc.contains("Resolution_mm", Qt::CaseInsensitive)
                || desc.contains("Offset_mm", Qt::CaseInsensitive))
            {
                parseLmiGocatorDescription(desc, outInfo);
            }
        }
    }

    return outInfo.valid;
}

// 主机字节序检测（编译期）
static constexpr bool hostIsLittleEndian()
{
#if defined(Q_BYTE_ORDER) && defined(Q_LITTLE_ENDIAN)
    return Q_BYTE_ORDER == Q_LITTLE_ENDIAN;
#else
    return true;   // x86 / x64 默认
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Predictor=3（浮点差分）反推 + 字节平面交错还原（行内）
// 输入 rowBytes 字节流是「按字节平面顺序：MSB 平面..LSB 平面」，且每个平面在行内经过
// 水平差分（mod 256）。本函数就地原地反差分后，将字节重排为「机器原生字节序的 float
// 序列」，供 memcpy 到 float 数组。
//
// stride = samplesPerPixel（chunky）或 1（planar）；samplesPerPixel*bps 通常 == bps
// 对 chunky 单通道：stride=1，全行字节连续差分。
// ─────────────────────────────────────────────────────────────────────────────
static void undoFpPredictor3Row(quint8* rowPtr, int W, int bps, int stride)
{
    const int rowBytes = W * bps;
    // 1) 水平字节反差分
    for (int i = stride; i < rowBytes; ++i)
        rowPtr[i] = static_cast<quint8>(rowPtr[i] + rowPtr[i - stride]);

    // 2) 字节平面 → 交错（输出为机器原生字节序）
    //    libtiff 风格：plane `b` (0=MSB) 的第 c 个字节 → sample[c] 的 (bps-1-b)（小端机）
    //                                                  → sample[c] 的 b           （大端机）
    std::vector<quint8> tmp(rowPtr, rowPtr + rowBytes);
    if (hostIsLittleEndian()) {
        for (int c = 0; c < W; ++c)
            for (int b = 0; b < bps; ++b)
                rowPtr[c * bps + (bps - 1 - b)] = tmp[b * W + c];
    } else {
        for (int c = 0; c < W; ++c)
            for (int b = 0; b < bps; ++b)
                rowPtr[c * bps + b] = tmp[b * W + c];
    }
}

static std::vector<float> tryReadTiff32Float(
    const QString& path, const TiffInfo& info)
{
    if (!info.valid || !info.is32Float)                       return {};
    if (info.compression != 1 && info.compression != 5)       return {};
    if (info.samplesPerPixel != 1)                            return {};
    if (info.planarConfig != 1)                               return {};
    if (info.bitsPerSample.value(0, 0) != 32)                 return {};

    const int W = info.width;
    const int H = info.height;
    if (W <= 0 || H <= 0
        || info.stripOffsets.isEmpty()
        || info.stripOffsets.size() != info.stripByteCounts.size())
    {
        return {};
    }

    const bool le         = info.littleEndian;
    const bool useLzw     = (info.compression == 5);
    const bool fpPred     = (info.predictor == 3);
    const int  bps        = 4;
    const int  rowBytes   = W * bps;
    const int  rowsPerStr = (info.rowsPerStrip > 0) ? info.rowsPerStrip : H;

    std::vector<float> data(static_cast<size_t>(W) * static_cast<size_t>(H), 0.f);

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};

    QByteArray decoded;
    int rowCursor = 0;
    for (int s = 0; s < info.stripOffsets.size() && rowCursor < H; ++s) {
        if (!f.seek(info.stripOffsets[s])) return {};
        const QByteArray raw = f.read(info.stripByteCounts[s]);
        if (raw.isEmpty()) return {};

        const int rowsThisStrip = std::min(rowsPerStr, H - rowCursor);
        const int expectedBytes = rowsThisStrip * rowBytes;

        const char* payload = nullptr;
        int         payloadSize = 0;
        if (useLzw) {
            if (!LzwTiff::decodeStrip(raw, decoded, expectedBytes)) return {};
            if (decoded.size() < expectedBytes) return {};
            payload     = decoded.constData();
            payloadSize = decoded.size();
        } else {
            payload     = raw.constData();
            payloadSize = raw.size();
        }
        if (payloadSize < expectedBytes) return {};

        // 对每一行处理
        // 注：Predictor=3 的字节平面布局是「行内」的：每行有 W*bps 字节按 (MSB 平面..LSB 平面) 排列。
        // 多行 strip 时每行独立反推。
        std::vector<quint8> rowBuf(rowBytes);
        for (int r = 0; r < rowsThisStrip; ++r) {
            memcpy(rowBuf.data(), payload + r * rowBytes, rowBytes);

            if (fpPred) {
                // 浮点差分 → 主机原生字节序 float
                undoFpPredictor3Row(rowBuf.data(), W, bps, /*stride=*/1);
                float* dst = data.data()
                    + static_cast<size_t>(rowCursor + r) * static_cast<size_t>(W);
                memcpy(dst, rowBuf.data(), rowBytes);
            } else {
                // Predictor=1（无差分）：按 TIFF 字节序解释
                float* dst = data.data()
                    + static_cast<size_t>(rowCursor + r) * static_cast<size_t>(W);
                for (int c = 0; c < W; ++c) {
                    quint32 bits;
                    memcpy(&bits, rowBuf.data() + c * 4, 4);
                    if (!le) bits = qbswap(bits);
                    memcpy(&dst[c], &bits, 4);
                }
            }
        }
        rowCursor += rowsThisStrip;
    }

    if (rowCursor < H) return {};
    return data;
}

// ── 双通道"128位"TIFF 解析（通道1=亮度 uint8/float32，通道2=高度 float32）──────
struct Tiff2ChData {
    std::vector<float>  height;     //!< 通道2：32-bit float 高度
    std::vector<quint8> brightness; //!< 通道1：8-bit 亮度（0-255）
    int  W = 0, H = 0;
    bool le = true;
};

static Tiff2ChData tryReadTiff2Channel(const QString& path, const TiffInfo& info)
{
    Tiff2ChData result;
    if (!info.valid || !info.is2Channel || info.compression != 1)
        return result;

    const bool le = info.littleEndian;
    result.le = le;

    if (info.samplesPerPixel != 2 || info.bitsPerSample.size() < 2)
        return result;

    const int W = info.width;
    const int H = info.height;
    if (W <= 0 || H <= 0
        || info.stripOffsets.isEmpty()
        || info.stripOffsets.size() != info.stripByteCounts.size())
    {
        return result;
    }

    const int bps0 = info.bitsPerSample.value(0, 0); // 通道1位深
    const int bps1 = info.bitsPerSample.value(1, 0); // 通道2位深

    // 通道2 必须是 32-bit
    if (bps1 != 32) return result;
    // 通道1 支持 8-bit 或 32-bit
    if (bps0 != 8 && bps0 != 32) return result;

    // SampleFormat：[sf0, sf1]，缺省 = 1 (unsigned int)
    const int sf1 = info.sampleFormats.value(1, 1);
    // 通道2 必须是 IEEE float (3)
    if (sf1 != 3) return result;

    // PlanarConfig：1=chunky（交错），2=planar（分离），缺省=1
    const int planarConfig = info.planarConfig;

    result.W = W; result.H = H;
    result.height    .resize(static_cast<size_t>(W * H), 0.f);
    result.brightness.resize(static_cast<size_t>(W * H), 0u);

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { result.W = result.H = 0; return result; }

    // 辅助：从内存读 32-bit float（含字节序处理）
    auto readF32 = [&](const char* src) -> float {
        quint32 bits;
        memcpy(&bits, src, 4);
        if (!le) bits = qbswap(bits);
        float v; memcpy(&v, &bits, 4);
        return v;
    };

    if (planarConfig == 1) {
        // Chunky：每像素 [ch1_bytes][ch2_float32] 交错存放
        const int ch1Bytes = bps0 / 8; // 1 or 4
        const int bpp      = ch1Bytes + 4;
        int row = 0;
        for (int s = 0; s < info.stripOffsets.size() && row < H; ++s) {
            f.seek(info.stripOffsets[s]);
            const QByteArray raw = f.read(info.stripByteCounts[s]);
            const int nRows = (raw.size() / bpp) / W;
            for (int r = 0; r < nRows && (row + r) < H; ++r) {
                for (int c = 0; c < W; ++c) {
                    const int off = (r * W + c) * bpp;
                    if (off + bpp > raw.size()) break;
                    const size_t pidx = static_cast<size_t>((row + r) * W + c);
                    // 通道1 → 亮度
                    if (bps0 == 8) {
                        result.brightness[pidx] =
                            static_cast<quint8>(
                                static_cast<unsigned char>(raw.constData()[off]));
                    } else {
                        const float fv = readF32(raw.constData() + off);
                        result.brightness[pidx] =
                            static_cast<quint8>(std::max(0.f, std::min(fv, 255.f)));
                    }
                    // 通道2 → 高度
                    result.height[pidx] = readF32(raw.constData() + off + ch1Bytes);
                }
            }
            row += nRows;
        }
    } else if (planarConfig == 2) {
        // Planar：前 N/2 个 strip = 通道1，后 N/2 = 通道2
        const int nTotal  = info.stripOffsets.size();
        const int nPerCh  = nTotal / 2;

        // 通道1（亮度）
        int row = 0;
        for (int s = 0; s < nPerCh && row < H; ++s) {
            f.seek(info.stripOffsets[s]);
            const QByteArray raw = f.read(info.stripByteCounts[s]);
            if (bps0 == 8) {
                const int nRows = raw.size() / W;
                for (int r = 0; r < nRows && (row + r) < H; ++r)
                    for (int c = 0; c < W; ++c)
                        result.brightness[static_cast<size_t>((row+r)*W+c)] =
                            static_cast<quint8>(
                                static_cast<unsigned char>(raw.constData()[r*W+c]));
                row += nRows;
            } else {
                const int nRows = (raw.size() / 4) / W;
                for (int r = 0; r < nRows && (row + r) < H; ++r)
                    for (int c = 0; c < W; ++c) {
                        const float fv = readF32(raw.constData() + (r*W+c)*4);
                        result.brightness[static_cast<size_t>((row+r)*W+c)] =
                            static_cast<quint8>(std::max(0.f, std::min(fv, 255.f)));
                    }
                row += nRows;
            }
        }

        // 通道2（高度 float32）
        row = 0;
        for (int s = nPerCh; s < nTotal && row < H; ++s) {
            f.seek(info.stripOffsets[s]);
            const QByteArray raw = f.read(info.stripByteCounts[s]);
            const int nRows = (raw.size() / 4) / W;
            for (int r = 0; r < nRows && (row + r) < H; ++r)
                for (int c = 0; c < W; ++c)
                    result.height[static_cast<size_t>((row+r)*W+c)] =
                        readF32(raw.constData() + (r*W+c)*4);
            row += nRows;
        }
    } else {
        // 不支持的 PlanarConfig
        result.W = result.H = 0;
        result.height.clear();
        result.brightness.clear();
    }

    return result;
}

// ── 三通道"96位"TIFF 解析（通道1=X，通道2=Y，通道3=Z，均为 32-bit float, 单位 mm）──
struct Tiff96BitXYZData {
    std::vector<float> X, Y, Z;
    int  W = 0, H = 0;
};

static Tiff96BitXYZData tryReadTiff96BitXYZ(const QString& path, const TiffInfo& info)
{
    Tiff96BitXYZData result;
    if (!info.valid || !info.is96BitXYZ || info.compression != 1)
        return result;

    const bool le = info.littleEndian;
    if (info.samplesPerPixel != 3 || info.bitsPerSample.size() < 3)
        return result;

    const int W = info.width;
    const int H = info.height;
    if (W <= 0 || H <= 0
        || info.stripOffsets.isEmpty()
        || info.stripOffsets.size() != info.stripByteCounts.size())
        return result;

    result.W = W; result.H = H;
    const size_t N = static_cast<size_t>(W) * static_cast<size_t>(H);
    result.X.assign(N, 0.f);
    result.Y.assign(N, 0.f);
    result.Z.assign(N, 0.f);

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { result.W = result.H = 0; return result; }

    auto readF32 = [&](const char* src) -> float {
        quint32 bits;
        memcpy(&bits, src, 4);
        if (!le) bits = qbswap(bits);
        float v; memcpy(&v, &bits, 4);
        return v;
    };

    const int planarConfig = info.planarConfig;
    if (planarConfig == 1) {
        // Chunky：每像素 [X_f32][Y_f32][Z_f32] 交错存放（12 字节/像素）
        constexpr int bpp = 12;
        int row = 0;
        for (int s = 0; s < info.stripOffsets.size() && row < H; ++s) {
            f.seek(info.stripOffsets[s]);
            const QByteArray raw = f.read(info.stripByteCounts[s]);
            const int nRows = (raw.size() / bpp) / W;
            for (int r = 0; r < nRows && (row + r) < H; ++r) {
                for (int c = 0; c < W; ++c) {
                    const int off = (r * W + c) * bpp;
                    if (off + bpp > raw.size()) break;
                    const size_t pidx = static_cast<size_t>(row + r) * W + c;
                    result.X[pidx] = readF32(raw.constData() + off);
                    result.Y[pidx] = readF32(raw.constData() + off + 4);
                    result.Z[pidx] = readF32(raw.constData() + off + 8);
                }
            }
            row += nRows;
        }
    } else if (planarConfig == 2) {
        // Planar：strip 数等分为 3 段，依次为 X / Y / Z
        const int nTotal = info.stripOffsets.size();
        const int nPerCh = nTotal / 3;
        std::vector<float>* chans[3] = { &result.X, &result.Y, &result.Z };
        for (int ch = 0; ch < 3; ++ch) {
            int row = 0;
            const int sBegin = ch * nPerCh;
            const int sEnd   = (ch == 2) ? nTotal : (ch + 1) * nPerCh;
            for (int s = sBegin; s < sEnd && row < H; ++s) {
                f.seek(info.stripOffsets[s]);
                const QByteArray raw = f.read(info.stripByteCounts[s]);
                const int nRows = (raw.size() / 4) / W;
                for (int r = 0; r < nRows && (row + r) < H; ++r) {
                    for (int c = 0; c < W; ++c) {
                        const size_t pidx = static_cast<size_t>(row + r) * W + c;
                        (*chans[ch])[pidx] = readF32(raw.constData() + (r * W + c) * 4);
                    }
                }
                row += nRows;
            }
        }
    } else {
        result.W = result.H = 0;
        result.X.clear(); result.Y.clear(); result.Z.clear();
    }

    return result;
}

// ── 连通分量孤岛过滤（4连通 BFS，可选 Z 跳变限制）──────────────────────────
// 找出所有有效像素的连通分量，移除像素数 < minSize 的分量。
// maxZGap > 0 时：相邻像素 Z 差（mm）超过阈值则视为不连通，Z 不连续的块会被分割成独立分量。
// 返回 W*H 的 mask：true=保留，false=移除（仅对有效像素有意义）
static std::vector<bool> computeIslandMask(
    int W, int H,
    const std::function<bool(int,int)>& isInvalid,
    const std::function<float(int,int)>& getZmm,
    float maxZGap,
    int minSize)
{
    const int N = W * H;
    static const int dx4[] = {1, -1, 0, 0};
    static const int dy4[] = {0, 0, 1, -1};

    std::vector<bool> mask(static_cast<size_t>(N), true);
    std::vector<bool> visited(static_cast<size_t>(N), false);

    // 无效像素标记为已访问，不参与 BFS
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (isInvalid(x, y))
                visited[static_cast<size_t>(y * W + x)] = true;

    std::vector<int> q, comp;
    q.reserve(std::min(N, 4 * 1024 * 1024));
    comp.reserve(4096);

    for (int sy = 0; sy < H; ++sy) {
        for (int sx = 0; sx < W; ++sx) {
            const int si = sy * W + sx;
            if (visited[static_cast<size_t>(si)]) continue;

            // BFS 收集连通分量
            q.clear(); comp.clear();
            q.push_back(si);
            visited[static_cast<size_t>(si)] = true;

            for (int head = 0; head < static_cast<int>(q.size()); ++head) {
                const int cur = q[head];
                comp.push_back(cur);
                const int cy = cur / W, cx = cur % W;
                const float zCur = (maxZGap > 0.f) ? getZmm(cx, cy) : 0.f;
                for (int d = 0; d < 4; ++d) {
                    const int nx = cx + dx4[d], ny = cy + dy4[d];
                    if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                    const int ni = ny * W + nx;
                    if (visited[static_cast<size_t>(ni)]) continue;
                    // Z 连通判断：跳变过大则不加入本分量，但不标记 visited
                    // 这样外层循环会在之后将其作为新分量的种子
                    if (maxZGap > 0.f &&
                        std::abs(getZmm(nx, ny) - zCur) > maxZGap)
                        continue;
                    visited[static_cast<size_t>(ni)] = true;
                    q.push_back(ni);
                }
            }

            // 分量过小 → 标记为移除
            if (static_cast<int>(comp.size()) < minSize)
                for (int idx : comp)
                    mask[static_cast<size_t>(idx)] = false;
        }
    }
    return mask;
}

// ─────────────────────────────────────────────────────────────────────────────
// ── SRF 格式（GoPxL / LMI3D 裸格式）解析 ─────────────────────────────────────
//   规则文档：SRF_FORMAT_RULES.md
//   - little-endian
//   - 60 字节公共头 + int16 高度 + （v2）uint32 has_intensity + 可选 uint8 亮度
//   - 无效高度固定 -32768
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr qint16 kSrfInvalidZ  = -32768;
constexpr int    kSrfHeaderSize = 60;
constexpr quint64 kSrfMaxPoints = 200000000ULL; // 与官方工具上限一致，防误读

// 读取 SRF 头部 + 像素数据（int16 height + 可选 uint8 intensity）
struct SrfRawData {
    bool valid = false;
    int  W = 0;
    int  L = 0;
    double scaleX = 1.0, scaleY = 1.0, scaleZ = 1.0;
    double offsetX = 0.0, offsetY = 0.0, offsetZ = 0.0;
    quint32 version = 0;
    std::vector<qint16> points;     // size = W*L, row-major (length, width)
    std::vector<quint8> intensity;  // 空表示无亮度
};

static bool readSrfRaw(const QString& path, SrfRawData& out, QString* outError)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (outError) *outError = QString("无法打开 SRF 文件: %1").arg(path);
        return false;
    }

    const qint64 fileSize = f.size();
    if (fileSize < kSrfHeaderSize) {
        if (outError) *outError =
            QString("SRF 文件过小（%1 字节，至少需要 %2 字节头部）")
                .arg(fileSize).arg(kSrfHeaderSize);
        return false;
    }

    quint8 hdr[kSrfHeaderSize];
    if (f.read(reinterpret_cast<char*>(hdr), kSrfHeaderSize) != kSrfHeaderSize) {
        if (outError) *outError = "读取 SRF 头部失败";
        return false;
    }

    auto u32 = [](const quint8* p) {
        return qFromLittleEndian<quint32>(p);
    };
    auto f64 = [](const quint8* p) {
        double v;
        std::memcpy(&v, p, 8);
        // x86 是 little-endian，且 IEEE754 与文件一致，直接 memcpy 即可
        return v;
    };

    out.version = u32(&hdr[0]);
    const quint32 width  = u32(&hdr[4]);
    const quint32 length = u32(&hdr[8]);
    out.scaleX  = f64(&hdr[12]);
    out.scaleY  = f64(&hdr[20]);
    out.scaleZ  = f64(&hdr[28]);
    out.offsetX = f64(&hdr[36]);
    out.offsetY = f64(&hdr[44]);
    out.offsetZ = f64(&hdr[52]);

    if (out.version != 1 && out.version != 2) {
        if (outError) *outError =
            QString("不支持的 SRF 版本: %1（当前仅支持 v1 / v2）").arg(out.version);
        return false;
    }
    if (width == 0 || length == 0) {
        if (outError) *outError =
            QString("SRF 尺寸无效: width=%1 length=%2").arg(width).arg(length);
        return false;
    }
    const quint64 N = static_cast<quint64>(width) * length;
    if (N > kSrfMaxPoints) {
        if (outError) *outError =
            QString("SRF 点数超过上限（%1 > %2）").arg(N).arg(kSrfMaxPoints);
        return false;
    }
    const qint64 needHeight = static_cast<qint64>(N) * 2;
    if (fileSize < kSrfHeaderSize + needHeight) {
        if (outError) *outError =
            QString("SRF 文件长度不足以容纳高度数组（需要 %1 字节，实际 %2）")
                .arg(kSrfHeaderSize + needHeight).arg(fileSize);
        return false;
    }

    out.W = static_cast<int>(width);
    out.L = static_cast<int>(length);

    out.points.resize(static_cast<size_t>(N));
    if (f.read(reinterpret_cast<char*>(out.points.data()), needHeight) != needHeight) {
        if (outError) *outError = "读取 SRF 高度数据失败";
        return false;
    }
    // little-endian 直接载入；本工程目标平台 x86，无需 byte swap

    // v2 还可能携带亮度
    if (out.version == 2 && f.bytesAvailable() >= 4) {
        quint8 hi[4];
        if (f.read(reinterpret_cast<char*>(hi), 4) == 4) {
            const quint32 hasIntensity = qFromLittleEndian<quint32>(hi);
            if (hasIntensity == 1) {
                const qint64 needIntensity = static_cast<qint64>(N);
                if (f.bytesAvailable() < needIntensity) {
                    if (outError) *outError =
                        QString("SRF v2 亮度数据不完整（需要 %1 字节，剩余 %2）")
                            .arg(needIntensity).arg(f.bytesAvailable());
                    return false;
                }
                out.intensity.resize(static_cast<size_t>(N));
                if (f.read(reinterpret_cast<char*>(out.intensity.data()), needIntensity)
                    != needIntensity) {
                    if (outError) *outError = "读取 SRF 亮度数据失败";
                    return false;
                }
            } else if (hasIntensity != 0) {
                if (outError) *outError =
                    QString("SRF v2 has_intensity 字段无效: %1").arg(hasIntensity);
                return false;
            }
        }
    }

    out.valid = true;
    return true;
}

} // anonymous namespace

bool parseSrfMeta(const QString& path, SrfMeta& out, QString* outError)
{
    out = SrfMeta{};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (outError) *outError = QString("无法打开 SRF 文件: %1").arg(path);
        return false;
    }
    quint8 hdr[kSrfHeaderSize];
    if (f.read(reinterpret_cast<char*>(hdr), kSrfHeaderSize) != kSrfHeaderSize) {
        if (outError) *outError = "读取 SRF 头部失败";
        return false;
    }

    out.version = qFromLittleEndian<quint32>(&hdr[0]);
    const quint32 width  = qFromLittleEndian<quint32>(&hdr[4]);
    const quint32 length = qFromLittleEndian<quint32>(&hdr[8]);
    std::memcpy(&out.scaleX,  &hdr[12], 8);
    std::memcpy(&out.scaleY,  &hdr[20], 8);
    std::memcpy(&out.scaleZ,  &hdr[28], 8);
    std::memcpy(&out.offsetX, &hdr[36], 8);
    std::memcpy(&out.offsetY, &hdr[44], 8);
    std::memcpy(&out.offsetZ, &hdr[52], 8);

    if (out.version != 1 && out.version != 2) {
        if (outError) *outError =
            QString("不支持的 SRF 版本: %1").arg(out.version);
        return false;
    }
    if (width == 0 || length == 0) {
        if (outError) *outError =
            QString("SRF 尺寸无效: width=%1 length=%2").arg(width).arg(length);
        return false;
    }
    out.width  = static_cast<int>(width);
    out.length = static_cast<int>(length);

    // v2: 检查 has_intensity（不读完整数组）
    if (out.version == 2) {
        const quint64 N = static_cast<quint64>(width) * length;
        const qint64 hiPos = static_cast<qint64>(kSrfHeaderSize) + static_cast<qint64>(N) * 2;
        if (f.size() >= hiPos + 4 && f.seek(hiPos)) {
            quint8 hi[4];
            if (f.read(reinterpret_cast<char*>(hi), 4) == 4) {
                out.hasIntensity = (qFromLittleEndian<quint32>(hi) == 1);
            }
        }
    }
    out.valid = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ── SUR（Digital Surf / MountainsMap）格式 ────────────────────────────────────
//   512 字节固定头 + Comment[Comment_size] + Private_zone[Private_size] + 点数据。
//   偏移量表参考 Digital Surf MountainsMap SDK 公开字段说明，并与开源实现
//   rsciio.digitalsurf（MIT 协议）交叉核对，little-endian。
//   压缩签名 "DSCOMPRESSED" 的点数据段格式（同样参考 rsciio.digitalsurf 的
//   _unpack_data 反推）：uint32 streamCount，随后 streamCount 组 (uint32 rawLen,
//   uint32 zipLen) 目录表，再拼接 streamCount 段 zlib 压缩块；每段独立解压后按
//   顺序拼接即为原始点数据字节流。用 Qt qUncompress()（底层 zlib::uncompress）解压，
//   前置其要求的 4 字节大端"原始长度"头即可，无需额外链接 zlib。
//   支持的 Object_Type：2(_SURFACE)、11(_INTENSITYSURFACE，未获得真实样本，按 13/16
//   规律推断实现)、13(_RGBSURFACE)、16(_RGBINTENSITYSURFACE)，以及 Number_of_Objects>1
//   的 _SURFACESERIE(5) 首帧（等价于 2）。11/13/16 各自的 R/G/B/亮度附加通道均是紧跟
//   主高度通道之后的完整独立子对象（自带头部+Comment/Private_zone+点数据），顺序与
//   换算公式见 surChannelCountForType() 注释，已用 PCB via.sur / Watch mechanism.sur /
//   Groove V flanks.sur（13）、Microwave.sur / Solar panel.sur（16）等真实 MountainsMap
//   样本核对。仅非多层/光谱数据（W_Size<=1）。
// ─────────────────────────────────────────────────────────────────────────────
namespace {

constexpr int     kSurHeaderSize = 512;
constexpr quint64 kSurMaxPoints  = 200000000ULL; // 与 SRF/PLY/PCD 上限一致，防误读

//! 512 字节固定头部关键字段（其余字段——Object_Name/Operator_Name/时间戳等——
//! 与本插件无关，不解析）
struct SurHeaderFields {
    QString signature;        // @0,   12 字节
    bool    compressed = false; // signature == "DSCOMPRESSED"
    quint16 numberOfObjects;  // @14,  uint16
    qint16  objectType;       // @18,  int16（2 = _SURFACE）
    qint16  pSize;            // @80,  int16，通道数（1=单通道高度图）
    qint16  specialPoints;    // @86,  int16，1=含无效点标记（raw==Zmin-2）
    quint32 wSize;            // @94,  uint32，>1 表示多层/光谱数据
    qint16  sizeOfPoints;     // @98,  int16，单点位深：16 或 32（bit）
    qint32  zmin;             // @100, int32
    qint32  numberOfPoints;   // @108, int32 = 宽度 W
    qint32  numberOfLines;    // @112, int32 = 高度 H
    qint32  totalNbOfPts;     // @116, int32
    float   xSpacing;         // @120, float32
    float   ySpacing;         // @124, float32
    float   zSpacing;         // @128, float32
    QString xStepUnit;        // @180, 16 字节
    QString yStepUnit;        // @196, 16 字节
    QString zStepUnit;        // @212, 16 字节
    float   zUnitRatio;       // @284, float32
    qint16  commentSize;      // @334, int16
    qint16  privateSize;      // @336, int16
    float   xOffset;          // @466, float32
    float   yOffset;          // @470, float32
    float   zOffset;          // @474, float32
};

//! Digital Surf 长度单位字符串 → mm 换算倍率；未知/空单位按 1.0（假定已是 mm）处理
double surUnitToMm(const QString& unit)
{
    const QString u = unit.trimmed().toLower();
    if (u == QLatin1String("m"))  return 1000.0;
    if (u == QLatin1String("cm")) return 10.0;
    if (u.isEmpty() || u == QLatin1String("mm")) return 1.0;
    if (u == QLatin1String("um") || u == QString::fromUtf8("\xc2\xb5m")) return 0.001;
    if (u == QLatin1String("nm")) return 0.000001;
    if (u == QLatin1String("pm")) return 0.000000001;
    return 1.0; // 未知单位：假定已是 mm，避免误报
}

//! 只读取 512 字节固定头部（调用方需保证 f 已定位到文件开头）
bool readSurHeader(QFile& f, SurHeaderFields& hdr, QString* outError)
{
    quint8 buf[kSurHeaderSize];
    if (f.read(reinterpret_cast<char*>(buf), kSurHeaderSize) != kSurHeaderSize) {
        if (outError) *outError = "读取 SUR 头部失败（文件过小）";
        return false;
    }

    auto i16 = [&](int off) { qint16  v; std::memcpy(&v, buf + off, 2); return v; };
    auto u16 = [&](int off) { quint16 v; std::memcpy(&v, buf + off, 2); return v; };
    auto i32 = [&](int off) { qint32  v; std::memcpy(&v, buf + off, 4); return v; };
    auto u32 = [&](int off) { quint32 v; std::memcpy(&v, buf + off, 4); return v; };
    auto f32 = [&](int off) { float   v; std::memcpy(&v, buf + off, 4); return v; };
    auto str = [&](int off, int len) {
        QByteArray b(reinterpret_cast<const char*>(buf + off), len);
        const int nul = b.indexOf('\0');
        if (nul >= 0) b.truncate(nul);
        return QString::fromLatin1(b).trimmed();
    };

    hdr.signature       = str(0, 12);
    hdr.compressed      = (hdr.signature == QLatin1String("DSCOMPRESSED"));
    hdr.numberOfObjects = u16(14);
    hdr.objectType      = i16(18);
    hdr.pSize           = i16(80);
    hdr.specialPoints   = i16(86);
    hdr.wSize           = u32(94);
    hdr.sizeOfPoints    = i16(98);
    hdr.zmin            = i32(100);
    hdr.numberOfPoints  = i32(108);
    hdr.numberOfLines   = i32(112);
    hdr.totalNbOfPts    = i32(116);
    hdr.xSpacing        = f32(120);
    hdr.ySpacing        = f32(124);
    hdr.zSpacing        = f32(128);
    hdr.xStepUnit       = str(180, 16);
    hdr.yStepUnit       = str(196, 16);
    hdr.zStepUnit       = str(212, 16);
    hdr.zUnitRatio      = f32(284);
    hdr.commentSize     = i16(334);
    hdr.privateSize     = i16(336);
    hdr.xOffset         = f32(466);
    hdr.yOffset         = f32(470);
    hdr.zOffset         = f32(474);
    return true;
}

//! 校验头部通用部分（与对象类型/通道数无关）；不满足时给出具体原因
bool validateSurHeaderScope(const SurHeaderFields& hdr, QString* outError)
{
    if (!hdr.compressed && hdr.signature != QLatin1String("DIGITAL SURF")) {
        if (outError) *outError = QString("不是有效的 .sur 文件（签名: \"%1\"）").arg(hdr.signature);
        return false;
    }
    if (hdr.numberOfObjects < 1) {
        if (outError) *outError = QString("SUR Number_of_Objects 无效: %1").arg(hdr.numberOfObjects);
        return false;
    }
    if (std::max<quint32>(hdr.wSize, 1) > 1) {
        if (outError) *outError = QString("暂不支持多层/光谱 .sur 数据（W_Size=%1）").arg(hdr.wSize);
        return false;
    }
    if (hdr.sizeOfPoints != 16 && hdr.sizeOfPoints != 32) {
        if (outError) *outError = QString("不支持的 .sur 点位深（Size_of_Points=%1，仅支持 16/32）").arg(hdr.sizeOfPoints);
        return false;
    }
    if (hdr.numberOfPoints <= 0 || hdr.numberOfLines <= 0) {
        if (outError) *outError = QString("SUR 尺寸无效: width=%1 height=%2").arg(hdr.numberOfPoints).arg(hdr.numberOfLines);
        return false;
    }
    const quint64 N = static_cast<quint64>(hdr.numberOfPoints) * static_cast<quint64>(hdr.numberOfLines);
    if (N > kSurMaxPoints) {
        if (outError) *outError = QString("SUR 点数超过上限（%1 > %2）").arg(N).arg(kSurMaxPoints);
        return false;
    }
    if (static_cast<quint64>(hdr.totalNbOfPts) != N) {
        if (outError) *outError = QString("SUR Total_Nb_of_Pts(%1) 与 width×height(%2) 不一致").arg(hdr.totalNbOfPts).arg(N);
        return false;
    }
    return true;
}

//! 读取 "DSCOMPRESSED" 点数据段（streamCount + 目录表 + 拼接 zlib 压缩块），
//! 解压后拼接为原始字节流。见文件顶部 SUR 格式说明中对该布局的推导依据。
//! @param expectedTotalBytes 解压后应得到的总字节数（= W*H*psizeBytes），用于校验
bool readSurCompressedPoints(QFile& f, quint64 expectedTotalBytes, QByteArray& out, QString* outError)
{
    constexpr quint32 kMaxStreams = 1024; // 防误读；官方写入器实际最多用 8 个流

    quint8 cntBuf[4];
    if (f.read(reinterpret_cast<char*>(cntBuf), 4) != 4) {
        if (outError) *outError = "读取 SUR 压缩流目录失败（streamCount）";
        return false;
    }
    const quint32 streamCount = qFromLittleEndian<quint32>(cntBuf);
    if (streamCount == 0 || streamCount > kMaxStreams) {
        if (outError) *outError = QString("SUR 压缩流数量异常（streamCount=%1）").arg(streamCount);
        return false;
    }

    std::vector<quint32> rawLens(streamCount), zipLens(streamCount);
    for (quint32 i = 0; i < streamCount; ++i) {
        quint8 dirBuf[8];
        if (f.read(reinterpret_cast<char*>(dirBuf), 8) != 8) {
            if (outError) *outError = "读取 SUR 压缩流目录失败（rawLen/zipLen）";
            return false;
        }
        rawLens[i] = qFromLittleEndian<quint32>(&dirBuf[0]);
        zipLens[i] = qFromLittleEndian<quint32>(&dirBuf[4]);
    }

    out.clear();
    for (quint32 i = 0; i < streamCount; ++i) {
        const quint32 zipLen = zipLens[i];
        const quint32 rawLen = rawLens[i];
        if (zipLen == 0 && rawLen == 0)
            continue; // 空流，跳过
        if (static_cast<quint64>(f.bytesAvailable()) < zipLen) {
            if (outError) *outError = QString("SUR 压缩数据不完整（流 %1 需要 %2 字节，剩余 %3）")
                .arg(i).arg(zipLen).arg(f.bytesAvailable());
            return false;
        }
        const QByteArray zipData = f.read(zipLen);
        if (static_cast<quint32>(zipData.size()) != zipLen) {
            if (outError) *outError = QString("读取 SUR 压缩流 %1 失败").arg(i);
            return false;
        }

        // qUncompress() 要求前置 4 字节大端"原始长度"头（Qt qCompress 格式），
        // 底层调用 zlib::uncompress()，与标准 zlib 压缩流（Digital Surf/Python
        // zlib.compress 均为此格式）完全兼容——手工拼出这个头即可复用 Qt 的解压实现
        QByteArray withHeader;
        withHeader.resize(4 + zipData.size());
        withHeader[0] = static_cast<char>((rawLen >> 24) & 0xFF);
        withHeader[1] = static_cast<char>((rawLen >> 16) & 0xFF);
        withHeader[2] = static_cast<char>((rawLen >> 8) & 0xFF);
        withHeader[3] = static_cast<char>(rawLen & 0xFF);
        std::memcpy(withHeader.data() + 4, zipData.constData(), static_cast<size_t>(zipData.size()));

        const QByteArray inflated = qUncompress(withHeader);
        if (static_cast<quint32>(inflated.size()) != rawLen) {
            if (outError) *outError = QString("SUR 压缩流 %1 解压失败或长度不符（期望 %2，实际 %3）")
                .arg(i).arg(rawLen).arg(inflated.size());
            return false;
        }
        out.append(inflated);
    }

    if (static_cast<quint64>(out.size()) != expectedTotalBytes) {
        if (outError) *outError = QString("SUR 解压后数据长度不符（期望 %1 字节，实际 %2 字节）")
            .arg(expectedTotalBytes).arg(out.size());
        return false;
    }
    return true;
}

//! 根据 Number_of_Objects / Object_Type 解析出本插件按哪种"有效类型"处理：
//!   - Number_of_Objects==1：Object_Type 本身即为有效类型
//!   - Number_of_Objects>1（多对象/序列文件）：仅取第 1 帧；已用真实样本
//!     （Engraved ring.sur / Sandpaper 4D.sur）核对 _SURFACESERIE(5) 的第 1 帧
//!     即完整、独立的普通高度网格数据，等价于 Object_Type==2。其余序列包装类型
//!     （_PROFILESERIE/_MULTILAYERPROFILE/_MULTILAYERSURFACE/_SERIESOFRGBIMAGES 等）
//!     未见真实样本，暂不支持
//! @return 有效类型（2/11/13/16），失败返回 -1 并写 outError
int resolveSurEffectiveObjectType(const SurHeaderFields& hdr, QString* outError)
{
    if (hdr.numberOfObjects <= 1)
        return hdr.objectType;
    if (hdr.objectType == 5) // _SURFACESERIE 第 1 帧等价于 _SURFACE
        return 2;
    if (outError) *outError = QString("暂不支持的 .sur 多对象序列类型（Object_Type=%1，Number_of_Objects=%2）")
        .arg(hdr.objectType).arg(hdr.numberOfObjects);
    return -1;
}

//! 每种支持的有效类型对应的子对象总数（1 个主高度通道 + N 个附加通道），
//! 均已用真实 MountainsMap 样本核对通道顺序——除 11（缺乏真实样本，按 13/16
//! 的规律"1 高度 + N 附加"类推实现，未实测，真实文件如与此不符会在读取附加
//! 通道时因尺寸不匹配报错，不会读出错误数据）：
//!   2  (_SURFACE)            : 1 主高度
//!   11 (_INTENSITYSURFACE)   : 主高度 + 灰度亮度（未实测）
//!   13 (_RGBSURFACE)         : 主高度 + R + G + B（PCB via.sur / Watch mechanism.sur /
//!                              Groove V flanks.sur 三个真实样本核对）
//!   16 (_RGBINTENSITYSURFACE): 主高度 + R + G + B + 亮度（Microwave.sur / Solar panel.sur
//!                              核对；本插件仅使用 RGB，末尾亮度通道当前忽略）
int surChannelCountForType(int effectiveType)
{
    switch (effectiveType) {
    case 2:  return 1;
    case 11: return 2;
    case 13: return 4;
    case 16: return 5;
    default: return 0;
    }
}

//! 综合校验：通用头部校验 + 解析有效类型 + 校验通道数（P_Size）是否匹配
bool validateSurHeaderAndResolveType(const SurHeaderFields& hdr, int& outEffectiveType,
                                      int& outChannelCount, QString* outError)
{
    if (!validateSurHeaderScope(hdr, outError)) return false;

    const int effType = resolveSurEffectiveObjectType(hdr, outError);
    if (effType < 0) return false;

    const int expectedChannels = surChannelCountForType(effType);
    if (expectedChannels == 0) {
        if (outError) *outError = QString("暂不支持的 .sur 对象类型（Object_Type=%1，仅支持 2/11/13/16 及 _SURFACESERIE 首帧）")
            .arg(hdr.objectType);
        return false;
    }
    const int actualChannels = std::max<qint16>(hdr.pSize, 1);
    if (actualChannels != expectedChannels) {
        if (outError) *outError = QString("SUR 通道数与对象类型不匹配（Object_Type=%1 期望通道数=%2，实际 P_Size=%3）")
            .arg(hdr.objectType).arg(expectedChannels).arg(hdr.pSize);
        return false;
    }
    outEffectiveType = effType;
    outChannelCount  = expectedChannels;
    return true;
}

//! 跳过当前子对象头部之后的 Comment + Private_zone，定位到该子对象点数据起始处
//! （调用方需保证 f 已读完该子对象的 512 字节头部）
bool seekPastSurCommentPrivate(QFile& f, const SurHeaderFields& hdr, QString* outError)
{
    const qint64 skip = std::max<qint16>(hdr.commentSize, 0) + std::max<qint16>(hdr.privateSize, 0);
    if (!f.seek(f.pos() + skip)) {
        if (outError) *outError = "定位 SUR 点数据失败（Comment/Private_zone 尺寸异常）";
        return false;
    }
    return true;
}

//! 读取"当前文件位置"起、已知头部（hdr）的一个子对象的原始整型点值
//! （16 位提升为 32 位，行优先）；不做物理换算。调用方需已定位到点数据起始处
bool readSurRawPointValues(QFile& f, const SurHeaderFields& hdr, std::vector<qint32>& out, QString* outError)
{
    const size_t N = static_cast<size_t>(hdr.numberOfPoints) * static_cast<size_t>(hdr.numberOfLines);
    const int psizeBytes = hdr.sizeOfPoints / 8; // 2 或 4
    const qint64 needBytes = static_cast<qint64>(N) * psizeBytes;

    out.resize(N);
    if (hdr.compressed) {
        QByteArray raw;
        if (!readSurCompressedPoints(f, static_cast<quint64>(needBytes), raw, outError))
            return false;
        if (psizeBytes == 2) {
            const qint16* p = reinterpret_cast<const qint16*>(raw.constData());
            for (size_t i = 0; i < N; ++i) out[i] = p[i];
        } else {
            std::memcpy(out.data(), raw.constData(), static_cast<size_t>(needBytes));
        }
    } else {
        if (f.bytesAvailable() < needBytes) {
            if (outError) *outError =
                QString("SUR 点数据不完整（需要 %1 字节，剩余 %2）").arg(needBytes).arg(f.bytesAvailable());
            return false;
        }
        if (psizeBytes == 2) {
            std::vector<qint16> tmp(N);
            if (f.read(reinterpret_cast<char*>(tmp.data()), needBytes) != needBytes) {
                if (outError) *outError = "读取 SUR 点数据失败";
                return false;
            }
            for (size_t i = 0; i < N; ++i) out[i] = tmp[i];
        } else {
            if (f.read(reinterpret_cast<char*>(out.data()), needBytes) != needBytes) {
                if (outError) *outError = "读取 SUR 点数据失败";
                return false;
            }
        }
    }
    return true;
}

//! 读取"当前文件位置"起的一个完整子对象：512 字节头 + Comment/Private_zone + 点数据
bool readSurFullObject(QFile& f, SurHeaderFields& hdr, std::vector<qint32>& rawPoints, QString* outError)
{
    if (!readSurHeader(f, hdr, outError)) return false;
    if (hdr.numberOfPoints <= 0 || hdr.numberOfLines <= 0) {
        if (outError) *outError = QString("SUR 子对象尺寸无效: width=%1 height=%2").arg(hdr.numberOfPoints).arg(hdr.numberOfLines);
        return false;
    }
    if (hdr.sizeOfPoints != 16 && hdr.sizeOfPoints != 32) {
        if (outError) *outError = QString("不支持的 SUR 子对象点位深（Size_of_Points=%1）").arg(hdr.sizeOfPoints);
        return false;
    }
    const quint64 N = static_cast<quint64>(hdr.numberOfPoints) * static_cast<quint64>(hdr.numberOfLines);
    if (N > kSurMaxPoints) {
        if (outError) *outError = QString("SUR 子对象点数超过上限（%1 > %2）").arg(N).arg(kSurMaxPoints);
        return false;
    }
    if (!seekPastSurCommentPrivate(f, hdr, outError)) return false;
    return readSurRawPointValues(f, hdr, rawPoints, outError);
}

struct SurRawData {
    bool   valid  = false;
    int    W = 0;
    int    H = 0;
    double scaleX = 1.0, scaleY = 1.0, scaleZ = 1.0;   // mm
    double offsetX = 0.0, offsetY = 0.0, offsetZ = 0.0; // mm
    qint32 zmin = 0;
    bool   hasSpecialPoints = false;
    std::vector<qint32> points;     // 主高度通道，统一提升为 int32，行优先
    std::vector<quint8> rgb;        // 交织 RGB（R,G,B,R,G,B,...）；Object_Type 有效为 13/16 时非空
    std::vector<quint8> intensity;  // 灰度亮度；Object_Type 有效为 11 时非空
};

bool readSurRaw(const QString& path, SurRawData& out, QString* outError)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (outError) *outError = QString("无法打开 SUR 文件: %1").arg(path);
        return false;
    }
    if (f.size() < kSurHeaderSize) {
        if (outError) *outError =
            QString("SUR 文件过小（%1 字节，至少需要 %2 字节头部）").arg(f.size()).arg(kSurHeaderSize);
        return false;
    }

    SurHeaderFields hdr;
    if (!readSurHeader(f, hdr, outError)) return false;

    int effType = 0, channelCount = 0;
    if (!validateSurHeaderAndResolveType(hdr, effType, channelCount, outError)) return false;

    if (!seekPastSurCommentPrivate(f, hdr, outError)) return false;

    std::vector<qint32> heightRaw;
    if (!readSurRawPointValues(f, hdr, heightRaw, outError)) return false;
    out.points = std::move(heightRaw);

    const int W = hdr.numberOfPoints;
    const int H = hdr.numberOfLines;
    const size_t N = static_cast<size_t>(W) * static_cast<size_t>(H);

    // 附加通道（R/G/B/亮度）：紧跟主高度通道之后依次排列，各自是完整独立的子对象
    // （自带头部/Comment/Private_zone/自己的 scale-offset），顺序见 surChannelCountForType 注释
    if (channelCount > 1) {
        std::vector<std::vector<quint8>> auxChannels(static_cast<size_t>(channelCount - 1));
        for (int c = 0; c < channelCount - 1; ++c) {
            SurHeaderFields auxHdr;
            std::vector<qint32> auxRaw;
            if (!readSurFullObject(f, auxHdr, auxRaw, outError)) return false;
            if (auxHdr.numberOfPoints != W || auxHdr.numberOfLines != H) {
                if (outError) *outError = QString("SUR 附加通道 %1 尺寸与主高度通道不一致（%2x%3 vs %4x%5）")
                    .arg(c).arg(auxHdr.numberOfPoints).arg(auxHdr.numberOfLines).arg(W).arg(H);
                return false;
            }
            const double auxRatio = (auxHdr.zUnitRatio != 0.f) ? static_cast<double>(auxHdr.zUnitRatio) : 1.0;
            std::vector<quint8>& outVec = auxChannels[static_cast<size_t>(c)];
            outVec.resize(N);
            for (size_t i = 0; i < N; ++i) {
                const double v = (static_cast<double>(auxRaw[i]) - auxHdr.zmin) * (auxHdr.zSpacing / auxRatio) + auxHdr.zOffset;
                const long iv = std::lround(v);
                outVec[i] = static_cast<quint8>(std::max(0L, std::min(255L, iv)));
            }
        }

        if (effType == 13 || effType == 16) {
            out.rgb.resize(N * 3);
            for (size_t i = 0; i < N; ++i) {
                out.rgb[i * 3 + 0] = auxChannels[0][i];
                out.rgb[i * 3 + 1] = auxChannels[1][i];
                out.rgb[i * 3 + 2] = auxChannels[2][i];
            }
            // effType==16 的第 4 个附加通道（额外亮度）当前不使用
        } else if (effType == 11) {
            out.intensity = std::move(auxChannels[0]);
        }
    }

    const double zRatio     = (hdr.zUnitRatio != 0.f) ? static_cast<double>(hdr.zUnitRatio) : 1.0;
    const double zMmPerUnit = surUnitToMm(hdr.zStepUnit);

    out.W = W;
    out.H = H;
    out.scaleX  = static_cast<double>(hdr.xSpacing) * surUnitToMm(hdr.xStepUnit);
    out.scaleY  = static_cast<double>(hdr.ySpacing) * surUnitToMm(hdr.yStepUnit);
    out.scaleZ  = (static_cast<double>(hdr.zSpacing) / zRatio) * zMmPerUnit;
    out.offsetX = static_cast<double>(hdr.xOffset) * surUnitToMm(hdr.xStepUnit);
    out.offsetY = static_cast<double>(hdr.yOffset) * surUnitToMm(hdr.yStepUnit);
    out.offsetZ = static_cast<double>(hdr.zOffset) * zMmPerUnit;
    out.zmin = hdr.zmin;
    out.hasSpecialPoints = (hdr.specialPoints == 1);
    out.valid = true;
    return true;
}

} // anonymous namespace

bool parseSurMeta(const QString& path, SurMeta& out, QString* outError)
{
    out = SurMeta{};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (outError) *outError = QString("无法打开 SUR 文件: %1").arg(path);
        return false;
    }
    if (f.size() < kSurHeaderSize) {
        if (outError) *outError =
            QString("SUR 文件过小（%1 字节，至少需要 %2 字节头部）").arg(f.size()).arg(kSurHeaderSize);
        return false;
    }
    SurHeaderFields hdr;
    if (!readSurHeader(f, hdr, outError)) return false;
    int effType = 0, channelCount = 0;
    if (!validateSurHeaderAndResolveType(hdr, effType, channelCount, outError)) return false;
    Q_UNUSED(effType);
    Q_UNUSED(channelCount);

    const double zRatio = (hdr.zUnitRatio != 0.f) ? static_cast<double>(hdr.zUnitRatio) : 1.0;
    out.width   = hdr.numberOfPoints;
    out.height  = hdr.numberOfLines;
    out.scaleX  = static_cast<double>(hdr.xSpacing) * surUnitToMm(hdr.xStepUnit);
    out.scaleY  = static_cast<double>(hdr.ySpacing) * surUnitToMm(hdr.yStepUnit);
    out.scaleZ  = (static_cast<double>(hdr.zSpacing) / zRatio) * surUnitToMm(hdr.zStepUnit);
    out.offsetX = static_cast<double>(hdr.xOffset) * surUnitToMm(hdr.xStepUnit);
    out.offsetY = static_cast<double>(hdr.yOffset) * surUnitToMm(hdr.yStepUnit);
    out.offsetZ = static_cast<double>(hdr.zOffset) * surUnitToMm(hdr.zStepUnit);
    out.valid = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ── PLY / PCD 点云直接导入 ────────────────────────────────────────────────────
//   优先尝试还原为规则网格（复用 SRF 的 floatData[W*H] 烘焙管线）；
//   网格重建失败（散点/非均匀间距/重复格点）→ 自动降级为普通无序点云，不报错。
// ─────────────────────────────────────────────────────────────────────────────
namespace {

constexpr quint64 kPlyPcdMaxPoints = 200000000ULL; // 与 SRF 上限一致，防误读/防御性截断

//! 单点：XYZ + 可选颜色 / 可选亮度（intensity）
struct PlyPcdPoint {
    float  x = 0.f, y = 0.f, z = 0.f;
    bool   hasColor     = false;
    quint8 r = 0, g = 0, b = 0;
    bool   hasIntensity = false;
    quint8 intensity    = 0;
};

struct PlyPcdPoints {
    std::vector<PlyPcdPoint> pts;
    bool anyColor     = false;
    bool anyIntensity = false;
};

// ── PLY 头部 / 二进制数据解析 ─────────────────────────────────────────────────

enum class PlyScalarType { Int8, UInt8, Int16, UInt16, Int32, UInt32, Float32, Float64, Unknown };

PlyScalarType plyTypeFromName(const QString& t)
{
    const QString n = t.toLower();
    if (n == QLatin1String("char")   || n == QLatin1String("int8"))    return PlyScalarType::Int8;
    if (n == QLatin1String("uchar")  || n == QLatin1String("uint8"))   return PlyScalarType::UInt8;
    if (n == QLatin1String("short")  || n == QLatin1String("int16"))   return PlyScalarType::Int16;
    if (n == QLatin1String("ushort") || n == QLatin1String("uint16"))  return PlyScalarType::UInt16;
    if (n == QLatin1String("int")    || n == QLatin1String("int32"))   return PlyScalarType::Int32;
    if (n == QLatin1String("uint")   || n == QLatin1String("uint32"))  return PlyScalarType::UInt32;
    if (n == QLatin1String("float")  || n == QLatin1String("float32")) return PlyScalarType::Float32;
    if (n == QLatin1String("double") || n == QLatin1String("float64")) return PlyScalarType::Float64;
    return PlyScalarType::Unknown;
}

int plyTypeSize(PlyScalarType t)
{
    switch (t) {
    case PlyScalarType::Int8:
    case PlyScalarType::UInt8:    return 1;
    case PlyScalarType::Int16:
    case PlyScalarType::UInt16:   return 2;
    case PlyScalarType::Int32:
    case PlyScalarType::UInt32:
    case PlyScalarType::Float32:  return 4;
    case PlyScalarType::Float64:  return 8;
    default:                      return 0;
    }
}

//! 从内存读取一个二进制标量并转换为 double（按给定字节序）
double plyReadScalar(const quint8* p, PlyScalarType t, bool bigEndian)
{
    switch (t) {
    case PlyScalarType::Int8:   return static_cast<double>(static_cast<qint8>(p[0]));
    case PlyScalarType::UInt8:  return static_cast<double>(p[0]);
    case PlyScalarType::Int16:  return bigEndian ? static_cast<double>(qFromBigEndian<qint16>(p))  : static_cast<double>(qFromLittleEndian<qint16>(p));
    case PlyScalarType::UInt16: return bigEndian ? static_cast<double>(qFromBigEndian<quint16>(p)) : static_cast<double>(qFromLittleEndian<quint16>(p));
    case PlyScalarType::Int32:  return bigEndian ? static_cast<double>(qFromBigEndian<qint32>(p))  : static_cast<double>(qFromLittleEndian<qint32>(p));
    case PlyScalarType::UInt32: return bigEndian ? static_cast<double>(qFromBigEndian<quint32>(p)) : static_cast<double>(qFromLittleEndian<quint32>(p));
    case PlyScalarType::Float32: {
        const quint32 bits = bigEndian ? qFromBigEndian<quint32>(p) : qFromLittleEndian<quint32>(p);
        float v; std::memcpy(&v, &bits, 4); return static_cast<double>(v);
    }
    case PlyScalarType::Float64: {
        const quint64 bits = bigEndian ? qFromBigEndian<quint64>(p) : qFromLittleEndian<quint64>(p);
        double v; std::memcpy(&v, &bits, 8); return v;
    }
    default: return 0.0;
    }
}

enum class PlyPropKind { Scalar, List };

struct PlyProperty {
    QString       name;
    PlyPropKind   kind      = PlyPropKind::Scalar;
    PlyScalarType type      = PlyScalarType::Unknown; // scalar 类型，或 list 的 item 类型
    PlyScalarType countType = PlyScalarType::Unknown; // 仅 list 有效
};

struct PlyElement {
    QString name;
    qint64  count = 0;
    std::vector<PlyProperty> properties;
};

enum class PlyFormat { Ascii, BinaryLE, BinaryBE };

//! 解析 PLY 文本头（直到 end_header）；不读取任何 element 数据
bool readPlyHeader(QFile& f, PlyFormat& outFormat, std::vector<PlyElement>& outElements, QString* outError)
{
    QByteArray line = f.readLine();
    if (QString::fromLatin1(line).trimmed() != QLatin1String("ply")) {
        if (outError) *outError = QStringLiteral("不是合法的 PLY 文件（缺少 'ply' 魔数）");
        return false;
    }

    bool formatSeen = false;
    PlyElement* curElem = nullptr;
    for (;;) {
        if (f.atEnd()) {
            if (outError) *outError = QStringLiteral("PLY 头部未正常结束（缺少 end_header）");
            return false;
        }
        line = f.readLine();
        const QString sline = QString::fromLatin1(line).trimmed();
        if (sline.isEmpty())
            continue;
        if (sline == QLatin1String("end_header"))
            break;
        if (sline.startsWith(QLatin1String("comment")) || sline.startsWith(QLatin1String("obj_info")))
            continue;

        const QStringList tok = sline.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (tok.isEmpty())
            continue;

        if (tok[0] == QLatin1String("format")) {
            if (tok.size() < 2) { if (outError) *outError = QStringLiteral("PLY format 行格式错误"); return false; }
            if (tok[1] == QLatin1String("ascii"))                    outFormat = PlyFormat::Ascii;
            else if (tok[1] == QLatin1String("binary_little_endian")) outFormat = PlyFormat::BinaryLE;
            else if (tok[1] == QLatin1String("binary_big_endian"))    outFormat = PlyFormat::BinaryBE;
            else { if (outError) *outError = QStringLiteral("不支持的 PLY format: %1").arg(tok[1]); return false; }
            formatSeen = true;
        } else if (tok[0] == QLatin1String("element")) {
            if (tok.size() < 3) { if (outError) *outError = QStringLiteral("PLY element 行格式错误"); return false; }
            PlyElement el;
            el.name = tok[1];
            bool ok = false;
            el.count = tok[2].toLongLong(&ok);
            if (!ok || el.count < 0) { if (outError) *outError = QStringLiteral("PLY element 数量无效: %1").arg(tok[2]); return false; }
            outElements.push_back(el);
            curElem = &outElements.back();
        } else if (tok[0] == QLatin1String("property")) {
            if (!curElem) { if (outError) *outError = QStringLiteral("PLY property 出现在 element 之前"); return false; }
            if (tok.size() >= 2 && tok[1] == QLatin1String("list")) {
                if (tok.size() < 5) { if (outError) *outError = QStringLiteral("PLY property list 行格式错误"); return false; }
                PlyProperty p;
                p.kind      = PlyPropKind::List;
                p.countType = plyTypeFromName(tok[2]);
                p.type      = plyTypeFromName(tok[3]);
                p.name      = tok[4];
                if (p.countType == PlyScalarType::Unknown || p.type == PlyScalarType::Unknown) {
                    if (outError) *outError = QStringLiteral("PLY property list 类型未知: %1").arg(sline);
                    return false;
                }
                curElem->properties.push_back(p);
            } else {
                if (tok.size() < 3) { if (outError) *outError = QStringLiteral("PLY property 行格式错误"); return false; }
                PlyProperty p;
                p.kind = PlyPropKind::Scalar;
                p.type = plyTypeFromName(tok[1]);
                p.name = tok[2];
                if (p.type == PlyScalarType::Unknown) {
                    if (outError) *outError = QStringLiteral("PLY property 类型未知: %1").arg(sline);
                    return false;
                }
                curElem->properties.push_back(p);
            }
        }
        // 其余未知关键字（如 obj_info 已处理之外的自定义行）宽松忽略
    }

    if (!formatSeen) {
        if (outError) *outError = QStringLiteral("PLY 缺少 format 声明");
        return false;
    }
    return true;
}

//! 按 props 顺序读取一条二进制记录；list 属性只读取计数并跳过其内容。
//! outValues 按 props 下标一一对应（list 属性对应位置保留为 0）。
bool plyReadOneBinary(QFile& f, const std::vector<PlyProperty>& props, bool bigEndian,
                      std::vector<double>& outValues, QString* outError)
{
    outValues.assign(props.size(), 0.0);
    for (size_t idx = 0; idx < props.size(); ++idx) {
        const PlyProperty& p = props[idx];
        if (p.kind == PlyPropKind::Scalar) {
            const int sz = plyTypeSize(p.type);
            quint8 buf[8];
            if (f.read(reinterpret_cast<char*>(buf), sz) != sz) {
                if (outError) *outError = QStringLiteral("PLY 二进制数据读取失败（属性 %1）").arg(p.name);
                return false;
            }
            outValues[idx] = plyReadScalar(buf, p.type, bigEndian);
        } else {
            const int csz = plyTypeSize(p.countType);
            quint8 cbuf[8];
            if (f.read(reinterpret_cast<char*>(cbuf), csz) != csz) {
                if (outError) *outError = QStringLiteral("PLY list 属性 %1 计数读取失败").arg(p.name);
                return false;
            }
            const qint64 n = static_cast<qint64>(plyReadScalar(cbuf, p.countType, bigEndian));
            if (n < 0 || n > 100000000LL) {
                if (outError) *outError = QStringLiteral("PLY list 属性 %1 长度异常: %2").arg(p.name).arg(n);
                return false;
            }
            if (n > 0) {
                const qint64 skipBytes = n * plyTypeSize(p.type);
                if (f.skip(skipBytes) != skipBytes) {
                    if (outError) *outError = QStringLiteral("PLY list 属性 %1 数据不完整").arg(p.name);
                    return false;
                }
            }
        }
    }
    return true;
}

//! 按空白快速切分一行文本中的数值字段，写入 out（调用方按需分配好长度，本函数原地覆盖复用，
//! 不做任何堆分配）。直接用 strtod 扫描原始字节，避免逐行构造 QRegularExpression + QStringList +
//! QString::toFloat/toDouble（locale 相关、逐次分配）——百万行级 ASCII 点云文件下，
//! 这一改动是加载耗时的主要来源（原实现在每一行都新建一个 QRegularExpression 对象）。
//! 返回实际解析到的字段数（遇到非数字 token 或行尾提前结束）。
int parseAsciiNumberFields(const QByteArray& line, double* out, int maxFields)
{
    const char* p = line.constData();
    const char* end = p + line.size();
    int n = 0;
    while (p < end && n < maxFields) {
        while (p < end && static_cast<unsigned char>(*p) <= ' ')
            ++p;
        if (p >= end)
            break;
        char* next = nullptr;
        const double v = std::strtod(p, &next);
        if (next == p)
            break; // 非数字 token
        out[n++] = v;
        p = next;
    }
    return n;
}

//! 读取整个 PLY 文件的点数据（vertex element），其余 element 一律跳过
bool readPlyPoints(const QString& path, PlyPcdPoints& out, QString* outError)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (outError) *outError = QStringLiteral("无法打开 PLY 文件: %1").arg(path);
        return false;
    }

    PlyFormat format = PlyFormat::Ascii;
    std::vector<PlyElement> elements;
    if (!readPlyHeader(f, format, elements, outError))
        return false;

    bool vertexFound = false;

    for (PlyElement& el : elements) {
        const bool isVertex = (el.name.compare(QLatin1String("vertex"), Qt::CaseInsensitive) == 0);

        if (!isVertex) {
            // 跳过整个 element，保证文件指针正确前进到下一 element
            if (format == PlyFormat::Ascii) {
                for (qint64 i = 0; i < el.count; ++i) {
                    if (f.atEnd()) break;
                    f.readLine();
                }
            } else {
                const bool bigEndian = (format == PlyFormat::BinaryBE);
                std::vector<double> dummy;
                for (qint64 i = 0; i < el.count; ++i) {
                    if (!plyReadOneBinary(f, el.properties, bigEndian, dummy, outError))
                        return false;
                }
            }
            continue;
        }

        vertexFound = true;
        if (el.count < 0 || static_cast<quint64>(el.count) > kPlyPcdMaxPoints) {
            if (outError) *outError = QStringLiteral("PLY 顶点数超过上限或无效: %1").arg(el.count);
            return false;
        }

        int idxX = -1, idxY = -1, idxZ = -1, idxR = -1, idxG = -1, idxB = -1, idxI = -1;
        bool vertexHasList = false;
        for (int i = 0; i < static_cast<int>(el.properties.size()); ++i) {
            const PlyProperty& p = el.properties[static_cast<size_t>(i)];
            if (p.kind == PlyPropKind::List) { vertexHasList = true; continue; }
            const QString n = p.name.toLower();
            if (n == QLatin1String("x")) idxX = i;
            else if (n == QLatin1String("y")) idxY = i;
            else if (n == QLatin1String("z")) idxZ = i;
            else if (n == QLatin1String("red") || n == QLatin1String("r") || n == QLatin1String("diffuse_red"))   idxR = i;
            else if (n == QLatin1String("green") || n == QLatin1String("g") || n == QLatin1String("diffuse_green")) idxG = i;
            else if (n == QLatin1String("blue") || n == QLatin1String("b") || n == QLatin1String("diffuse_blue"))  idxB = i;
            else if (n == QLatin1String("intensity") || n == QLatin1String("scalar_intensity")) idxI = i;
        }
        if (idxX < 0 || idxY < 0 || idxZ < 0) {
            if (outError) *outError = QStringLiteral("PLY vertex 缺少 x/y/z 属性");
            return false;
        }
        if (vertexHasList && format == PlyFormat::Ascii) {
            if (outError) *outError = QStringLiteral("不支持顶点内含 list 属性的 ASCII PLY");
            return false;
        }
        const bool hasRgb = (idxR >= 0 && idxG >= 0 && idxB >= 0);
        out.pts.reserve(out.pts.size() + static_cast<size_t>(el.count));
        out.anyColor     = out.anyColor || hasRgb;
        out.anyIntensity = out.anyIntensity || (idxI >= 0);

        if (format == PlyFormat::Ascii) {
            const int nProps = static_cast<int>(el.properties.size());
            std::vector<double> vals(static_cast<size_t>(nProps));
            for (qint64 i = 0; i < el.count; ++i) {
                if (f.atEnd()) {
                    if (outError) *outError = QStringLiteral("PLY 顶点数据不足（第 %1 行）").arg(i);
                    return false;
                }
                const QByteArray line = f.readLine();
                if (parseAsciiNumberFields(line, vals.data(), nProps) < nProps) {
                    if (outError) *outError = QStringLiteral("PLY 第 %1 行字段数不足").arg(i);
                    return false;
                }
                PlyPcdPoint pt;
                pt.x = static_cast<float>(vals[static_cast<size_t>(idxX)]);
                pt.y = static_cast<float>(vals[static_cast<size_t>(idxY)]);
                pt.z = static_cast<float>(vals[static_cast<size_t>(idxZ)]);
                if (hasRgb) {
                    pt.hasColor = true;
                    pt.r = static_cast<quint8>(vals[static_cast<size_t>(idxR)]);
                    pt.g = static_cast<quint8>(vals[static_cast<size_t>(idxG)]);
                    pt.b = static_cast<quint8>(vals[static_cast<size_t>(idxB)]);
                }
                if (idxI >= 0) {
                    pt.hasIntensity = true;
                    const double iv = vals[static_cast<size_t>(idxI)];
                    pt.intensity = static_cast<quint8>(std::max(0.0, std::min(255.0,
                        (iv >= 0.0 && iv <= 1.0) ? iv * 255.0 : iv)));
                }
                out.pts.push_back(pt);
            }
        } else {
            const bool bigEndian = (format == PlyFormat::BinaryBE);
            for (qint64 i = 0; i < el.count; ++i) {
                std::vector<double> values;
                if (!plyReadOneBinary(f, el.properties, bigEndian, values, outError))
                    return false;
                PlyPcdPoint pt;
                pt.x = static_cast<float>(values[static_cast<size_t>(idxX)]);
                pt.y = static_cast<float>(values[static_cast<size_t>(idxY)]);
                pt.z = static_cast<float>(values[static_cast<size_t>(idxZ)]);
                if (hasRgb) {
                    pt.hasColor = true;
                    pt.r = static_cast<quint8>(values[static_cast<size_t>(idxR)]);
                    pt.g = static_cast<quint8>(values[static_cast<size_t>(idxG)]);
                    pt.b = static_cast<quint8>(values[static_cast<size_t>(idxB)]);
                }
                if (idxI >= 0) {
                    pt.hasIntensity = true;
                    const double iv = values[static_cast<size_t>(idxI)];
                    pt.intensity = static_cast<quint8>(std::max(0.0, std::min(255.0,
                        (iv >= 0.0 && iv <= 1.0) ? iv * 255.0 : iv)));
                }
                out.pts.push_back(pt);
            }
        }
    }

    if (!vertexFound) {
        if (outError) *outError = QStringLiteral("PLY 文件缺少 vertex element");
        return false;
    }
    if (out.pts.empty()) {
        if (outError) *outError = QStringLiteral("PLY 文件不含有效顶点数据");
        return false;
    }
    return true;
}

// ── PCD 头部 / 数据解析 ───────────────────────────────────────────────────────

struct PcdField {
    QString name;
    int     size  = 4;
    char    type  = 'F'; // 'I' / 'U' / 'F'
    int     count = 1;
};

//! 按标准 PCL 约定，把打包的 rgb/rgba 32-bit 位模式拆成 RGB 三通道
void unpackPclRgb(quint32 packed, quint8& r, quint8& g, quint8& b)
{
    r = static_cast<quint8>((packed >> 16) & 0xFF);
    g = static_cast<quint8>((packed >> 8) & 0xFF);
    b = static_cast<quint8>(packed & 0xFF);
}

bool readPcdPoints(const QString& path, PlyPcdPoints& out, QString* outError)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (outError) *outError = QStringLiteral("无法打开 PCD 文件: %1").arg(path);
        return false;
    }

    std::vector<PcdField> fields;
    qint64  points = -1;
    QString dataMode;
    bool    headerDone = false;

    while (!f.atEnd()) {
        const QString sline = QString::fromLatin1(f.readLine()).trimmed();
        if (sline.isEmpty() || sline.startsWith(QLatin1Char('#')))
            continue;
        const QStringList tok = sline.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (tok.isEmpty())
            continue;
        const QString key = tok[0].toUpper();
        if (key == QLatin1String("FIELDS")) {
            fields.clear();
            for (int i = 1; i < tok.size(); ++i) {
                PcdField fld;
                fld.name = tok[i];
                fields.push_back(fld);
            }
        } else if (key == QLatin1String("SIZE")) {
            for (int i = 1; i < tok.size() && (i - 1) < static_cast<int>(fields.size()); ++i)
                fields[static_cast<size_t>(i - 1)].size = tok[i].toInt();
        } else if (key == QLatin1String("TYPE")) {
            for (int i = 1; i < tok.size() && (i - 1) < static_cast<int>(fields.size()); ++i)
                fields[static_cast<size_t>(i - 1)].type = tok[i].isEmpty() ? 'F' : tok[i].at(0).toUpper().toLatin1();
        } else if (key == QLatin1String("COUNT")) {
            for (int i = 1; i < tok.size() && (i - 1) < static_cast<int>(fields.size()); ++i)
                fields[static_cast<size_t>(i - 1)].count = tok[i].toInt();
        } else if (key == QLatin1String("POINTS")) {
            if (tok.size() >= 2) points = tok[1].toLongLong();
        } else if (key == QLatin1String("DATA")) {
            if (tok.size() >= 2) dataMode = tok[1].toLower();
            headerDone = true;
            break;
        }
        // VERSION / WIDTH / HEIGHT / VIEWPOINT 等忽略（网格重建统一走 XY 聚类，不依赖 PCD 自带的行列声明）
    }

    if (!headerDone) {
        if (outError) *outError = QStringLiteral("PCD 头部缺少 DATA 行");
        return false;
    }
    if (fields.empty()) {
        if (outError) *outError = QStringLiteral("PCD 头部缺少 FIELDS 声明");
        return false;
    }
    if (points < 0) {
        if (outError) *outError = QStringLiteral("PCD 头部缺少 POINTS 声明");
        return false;
    }
    if (static_cast<quint64>(points) > kPlyPcdMaxPoints) {
        if (outError) *outError = QStringLiteral("PCD 点数超过上限: %1").arg(points);
        return false;
    }
    if (dataMode != QLatin1String("ascii") && dataMode != QLatin1String("binary")) {
        if (outError) *outError = QStringLiteral("不支持的 PCD DATA 模式: %1（仅支持 ascii/binary）").arg(dataMode);
        return false;
    }

    int idxX = -1, idxY = -1, idxZ = -1, idxRgb = -1, idxIntensity = -1;
    for (int i = 0; i < static_cast<int>(fields.size()); ++i) {
        const QString n = fields[static_cast<size_t>(i)].name.toLower();
        if (n == QLatin1String("x")) idxX = i;
        else if (n == QLatin1String("y")) idxY = i;
        else if (n == QLatin1String("z")) idxZ = i;
        else if (n == QLatin1String("rgb") || n == QLatin1String("rgba")) idxRgb = i;
        else if (n == QLatin1String("intensity")) idxIntensity = i;
    }
    if (idxX < 0 || idxY < 0 || idxZ < 0) {
        if (outError) *outError = QStringLiteral("PCD FIELDS 缺少 x/y/z");
        return false;
    }

    out.pts.reserve(static_cast<size_t>(points));
    out.anyColor     = (idxRgb >= 0);
    out.anyIntensity = (idxIntensity >= 0);

    if (dataMode == QLatin1String("ascii")) {
        std::vector<int> colOffset(fields.size());
        int totalCols = 0;
        for (size_t k = 0; k < fields.size(); ++k) { colOffset[k] = totalCols; totalCols += fields[k].count; }

        std::vector<double> vals(static_cast<size_t>(totalCols));
        for (qint64 i = 0; i < points; ++i) {
            if (f.atEnd()) {
                if (outError) *outError = QStringLiteral("PCD 数据行不足（第 %1 行）").arg(i);
                return false;
            }
            const QByteArray line = f.readLine();
            if (parseAsciiNumberFields(line, vals.data(), totalCols) < totalCols) {
                if (outError) *outError = QStringLiteral("PCD 第 %1 行字段数不足").arg(i);
                return false;
            }
            PlyPcdPoint pt;
            pt.x = static_cast<float>(vals[static_cast<size_t>(colOffset[static_cast<size_t>(idxX)])]);
            pt.y = static_cast<float>(vals[static_cast<size_t>(colOffset[static_cast<size_t>(idxY)])]);
            pt.z = static_cast<float>(vals[static_cast<size_t>(colOffset[static_cast<size_t>(idxZ)])]);
            if (idxRgb >= 0) {
                // ASCII PCD 里 rgb/rgba 是把打包位模式重新解释成 float 后按十进制打印的（PCL 惯例）
                const float fv = static_cast<float>(vals[static_cast<size_t>(colOffset[static_cast<size_t>(idxRgb)])]);
                quint32 packed = 0;
                std::memcpy(&packed, &fv, 4);
                pt.hasColor = true;
                unpackPclRgb(packed, pt.r, pt.g, pt.b);
            }
            if (idxIntensity >= 0) {
                pt.hasIntensity = true;
                const double iv = vals[static_cast<size_t>(colOffset[static_cast<size_t>(idxIntensity)])];
                pt.intensity = static_cast<quint8>(std::max(0.0, std::min(255.0,
                    (iv >= 0.0 && iv <= 1.0) ? iv * 255.0 : iv)));
            }
            out.pts.push_back(pt);
        }
    } else {
        std::vector<int> byteOffset(fields.size());
        int recSize = 0;
        for (size_t k = 0; k < fields.size(); ++k) {
            byteOffset[k] = recSize;
            recSize += fields[k].size * fields[k].count;
        }
        if (recSize <= 0) {
            if (outError) *outError = QStringLiteral("PCD 字段大小无效");
            return false;
        }
        const qint64 needBytes = static_cast<qint64>(recSize) * points;
        if (f.bytesAvailable() < needBytes) {
            if (outError) *outError = QStringLiteral("PCD 二进制数据不完整（需要 %1 字节，剩余 %2）")
                .arg(needBytes).arg(f.bytesAvailable());
            return false;
        }

        std::vector<quint8> buf(static_cast<size_t>(recSize));
        auto readFieldAsDouble = [&](int fi) -> double {
            const PcdField& fld = fields[static_cast<size_t>(fi)];
            const quint8* p = buf.data() + byteOffset[static_cast<size_t>(fi)];
            if (fld.type == 'F') {
                if (fld.size == 4) { float v;  std::memcpy(&v, p, 4); return static_cast<double>(v); }
                if (fld.size == 8) { double v; std::memcpy(&v, p, 8); return v; }
            } else if (fld.type == 'U') {
                if (fld.size == 1) return static_cast<double>(p[0]);
                if (fld.size == 2) { quint16 v; std::memcpy(&v, p, 2); return static_cast<double>(v); }
                if (fld.size == 4) { quint32 v; std::memcpy(&v, p, 4); return static_cast<double>(v); }
            } else if (fld.type == 'I') {
                if (fld.size == 1) return static_cast<double>(static_cast<qint8>(p[0]));
                if (fld.size == 2) { qint16 v; std::memcpy(&v, p, 2); return static_cast<double>(v); }
                if (fld.size == 4) { qint32 v; std::memcpy(&v, p, 4); return static_cast<double>(v); }
            }
            return 0.0;
        };

        for (qint64 i = 0; i < points; ++i) {
            if (f.read(reinterpret_cast<char*>(buf.data()), recSize) != recSize) {
                if (outError) *outError = QStringLiteral("PCD 二进制数据读取失败（第 %1 点）").arg(i);
                return false;
            }
            PlyPcdPoint pt;
            pt.x = static_cast<float>(readFieldAsDouble(idxX));
            pt.y = static_cast<float>(readFieldAsDouble(idxY));
            pt.z = static_cast<float>(readFieldAsDouble(idxZ));
            if (idxRgb >= 0) {
                quint32 packed = 0;
                std::memcpy(&packed, buf.data() + byteOffset[static_cast<size_t>(idxRgb)], 4);
                pt.hasColor = true;
                unpackPclRgb(packed, pt.r, pt.g, pt.b);
            }
            if (idxIntensity >= 0) {
                pt.hasIntensity = true;
                const double iv = readFieldAsDouble(idxIntensity);
                pt.intensity = static_cast<quint8>(std::max(0.0, std::min(255.0,
                    (iv >= 0.0 && iv <= 1.0) ? iv * 255.0 : iv)));
            }
            out.pts.push_back(pt);
        }
    }

    if (out.pts.empty()) {
        if (outError) *outError = QStringLiteral("PCD 文件不含有效点数据");
        return false;
    }
    return true;
}

bool readPlyPcdPoints(const QString& path, PlyPcdPoints& out, QString* outError)
{
    if (isPlyPath(path)) return readPlyPoints(path, out, outError);
    if (isPcdPath(path)) return readPcdPoints(path, out, outError);
    if (outError) *outError = QStringLiteral("不支持的点云文件扩展名");
    return false;
}

// ── 均匀网格重建 ──────────────────────────────────────────────────────────────

struct UniformGridResult {
    int W = 0, H = 0;
    std::vector<float>  floatData;         // 行主序 W*H，mm，NaN=无效格
    std::vector<quint8> embeddedBrightness; // 可选；空=无亮度数据
    double resX = 1.0, resY = 1.0;
    double offX = 0.0, offY = 0.0;
};

//! 排序后按间隙分布自适应分簇，簇内取均值作为格中心。
//! 同一列/行的坐标在真实采集数据中并非逐点完全相等——测量噪声、float32 往返、
//! ASCII 文本格式化都会引入抖动，且抖动幅度无法预先假设（可能远大于 1e-6，
//! 也可能达到标称步长的一二十个百分点）；但也有大量真实数据（如编码器/像素索引
//! 直接决定的 X/Y，只有 Z 是噪声测量值）完全没有簇内抖动，且整行/整列可能因
//! 遮挡/超量程完全缺失，导致非零间隙里同时混有“单倍步长”和跨度不一的
//! “多倍步长”（因为跳过了若干缺失行/列）——旧版“找排序间隙序列里比值跳变最大的
//! 那一处”的双峰判定在这种场景下会失败：全局最大比值跳变可能恰好出现在
//! 多倍步长内部的某个偶然位置（尤其容易被序列末端唯一的最大间隙值带偏），
//! 而不是真正的“簇内噪声 / 簇间步长”分界，导致把绝大多数列/行错误合并。
//!
//! 新判定：先看非零间隙里的中位数 m——它必然对应数据中出现次数最多的那类间隙。
//! 如果以 m 为中心 ±10% 的窄带就已经覆盖了半数以上的非零间隙，说明这类间隙本身就是
//! 高度重复的离散值（真实网格步长的典型特征：同一步长在整份数据里逐行逐列反复出现），
//! 此时不存在簇内噪声，只需极小容差合并完全重复的坐标即可，不能再做任何比值双峰合并
//! （否则会像本例一样把大量真正不同列/行的单倍步长间隙也误判为“噪声”合并掉）。
//! 只有当窄带覆盖率低（说明这类间隙是连续分布的噪声抖动，而非离散重复的步长）时，
//! 才退回旧版的双峰比值跳变搜索去找“簇内噪声 / 簇间步长”的分界。
std::vector<double> clusterAxis(std::vector<double> vals)
{
    std::vector<double> centers;
    if (vals.empty())
        return centers;
    std::sort(vals.begin(), vals.end());
    if (vals.size() == 1) {
        centers.push_back(vals.front());
        return centers;
    }

    std::vector<double> gaps;
    gaps.reserve(vals.size() - 1);
    for (size_t i = 1; i < vals.size(); ++i)
        gaps.push_back(vals[i] - vals[i - 1]);

    constexpr double kTinyEps = 1e-9;
    std::vector<double> nz;
    nz.reserve(gaps.size());
    for (double g : gaps) {
        if (g > kTinyEps)
            nz.push_back(g);
    }
    std::sort(nz.begin(), nz.end());

    double threshold;
    if (nz.empty()) {
        threshold = kTinyEps; // 所有坐标完全相等
    } else {
        const double median = nz[nz.size() / 2];
        const double bandLo = median * 0.9;
        const double bandHi = median * 1.1;
        size_t inBand = 0;
        for (double g : nz) {
            if (g >= bandLo && g <= bandHi)
                ++inBand;
        }
        const double fracInBand = static_cast<double>(inBand) / static_cast<double>(nz.size());
        if (fracInBand >= 0.5) {
            // 非零间隙的主体是高度重复的离散值——就是真实步长本身，不是噪声，不能合并
            threshold = std::max(kTinyEps, median * 0.3);
        } else {
            // 非零间隙连续分布（噪声特征）——用双峰比值跳变搜索簇内噪声/簇间步长的分界
            size_t splitIdx = 0;
            double bestRatio = 1.0;
            for (size_t i = 1; i < nz.size(); ++i) {
                if (nz[i - 1] <= kTinyEps)
                    continue;
                const double ratio = nz[i] / nz[i - 1];
                if (ratio > bestRatio) {
                    bestRatio = ratio;
                    splitIdx = i;
                }
            }
            if (splitIdx == 0 || bestRatio < 3.0) {
                threshold = std::max(kTinyEps, median * 0.5);
            } else {
                const double smallSide = nz[splitIdx - 1];
                const double largeSide = nz[splitIdx];
                threshold = (smallSide > kTinyEps) ? std::sqrt(smallSide * largeSide) : largeSide * 0.5;
            }
        }
    }

    std::vector<double> cur;
    cur.push_back(vals.front());
    auto flush = [&]() {
        double sum = 0.0;
        for (double v : cur)
            sum += v;
        centers.push_back(sum / static_cast<double>(cur.size()));
        cur.clear();
    };
    for (size_t i = 1; i < vals.size(); ++i) {
        if (vals[i] - vals[i - 1] > threshold)
            flush();
        cur.push_back(vals[i]);
    }
    flush();
    return centers;
}

//! 从聚类中心序列估计真实网格步长：取最小的一批间隙的中位数。
//! 真实采集数据里整行/整列可能完全没有落点（传感器遮挡/超量程/反射率不足），
//! 相邻中心间隙因此可能是步长的整数倍而非彼此相等——不能像旧版 axisIsUniform 那样
//! 要求"所有间隙都接近同一个值"。单位步长必然是最常见、最小的那一类间隙
//! （缺行缺列只会让间隙变大，不会变小），取间隙分布最小 25% 的中位数即可稳健估出。
bool estimateAxisStep(const std::vector<double>& centers, double& outStep)
{
    if (centers.size() < 2)
        return false;
    std::vector<double> gaps;
    gaps.reserve(centers.size() - 1);
    for (size_t i = 1; i < centers.size(); ++i)
        gaps.push_back(centers[i] - centers[i - 1]);
    std::sort(gaps.begin(), gaps.end());
    const size_t quartileCount = std::max<size_t>(1, gaps.size() / 4);
    const double stepCandidate = gaps[quartileCount / 2];
    if (stepCandidate <= 0.0)
        return false;
    outStep = stepCandidate;
    return true;
}

//! 校验中心序列里所有相邻间隙是否都能对上 step 的整数倍（允许缺行缺列造成的大间隙，
//! 但间隙必须"对得上"整数倍关系）；真正的无规律散点数据不会呈现这种规律性，因此
//! 该检查依然能拒绝非网格数据——容差是相对单个 step 的固定值，不随倍数放大
//! （机械台架的重复定位误差是有界的，不会随跳过的格数累积）。
bool axisStepValid(const std::vector<double>& centers, double step, double tolRatio = 0.25)
{
    if (step <= 0.0)
        return false;
    for (size_t i = 1; i < centers.size(); ++i) {
        const double gap = centers[i] - centers[i - 1];
        const double n = std::round(gap / step);
        if (n < 1.0)
            return false;
        if (std::abs(gap - n * step) > step * tolRatio)
            return false;
    }
    return true;
}

//! 二分查找 value 在有序 centers 中最近的下标；超出容差（step * tolRatio）返回 -1
int nearestCenterIndex(const std::vector<double>& centers, double value, double tol)
{
    auto it = std::lower_bound(centers.begin(), centers.end(), value);
    int best = -1;
    double bestDist = std::numeric_limits<double>::max();
    if (it != centers.end()) {
        const double d = std::abs(*it - value);
        if (d < bestDist) { bestDist = d; best = static_cast<int>(it - centers.begin()); }
    }
    if (it != centers.begin()) {
        auto prev = std::prev(it);
        const double d = std::abs(*prev - value);
        if (d < bestDist) { bestDist = d; best = static_cast<int>(prev - centers.begin()); }
    }
    if (best < 0 || bestDist > tol)
        return -1;
    return best;
}

//! 把聚类中心换算成物理网格下标（第 0 个中心 = 下标 0）；下标之间可能有空洞
//! （对应完全缺失的行/列），空洞格子在网格数组里保持 NaN，这正是稀疏网格要的效果。
int physicalIndexOfCenter(const std::vector<double>& centers, int centerIdx, double step)
{
    return static_cast<int>(std::lround((centers[static_cast<size_t>(centerIdx)] - centers.front()) / step));
}

//! 尝试把散点还原为规则网格；失败（非网格/间距不均/重复格点）返回 false，调用方应降级为无序点云
bool tryBuildUniformGrid(const PlyPcdPoints& points, UniformGridResult& out)
{
    const size_t N = points.pts.size();
    if (N == 0)
        return false;

    std::vector<double> xs, ys;
    xs.reserve(N);
    ys.reserve(N);
    for (const PlyPcdPoint& p : points.pts) {
        xs.push_back(static_cast<double>(p.x));
        ys.push_back(static_cast<double>(p.y));
    }

    const std::vector<double> xCenters = clusterAxis(xs);
    const std::vector<double> yCenters = clusterAxis(ys);
    if (xCenters.size() < 2 || yCenters.size() < 2)
        return false; // 至少需要 2×2 才谈得上"网格"

    // 注意：真实采集数据里整行/整列可能完全没有落点（传感器遮挡/超量程），
    // xCenters/yCenters 的下标并不等于物理网格的行列号——不能像旧版那样直接拿
    // centers.size() 当 W/H。必须先估计真实步长，再把每个 center 换算成物理下标，
    // 用物理下标的极差决定 W/H，中间空缺的下标自然留空（NaN），这样才能正确处理稀疏网格。
    double stepX = 1.0, stepY = 1.0;
    if (!estimateAxisStep(xCenters, stepX) || !axisStepValid(xCenters, stepX))
        return false;
    if (!estimateAxisStep(yCenters, stepY) || !axisStepValid(yCenters, stepY))
        return false;

    std::vector<int> xPhysIdx(xCenters.size());
    for (size_t i = 0; i < xCenters.size(); ++i)
        xPhysIdx[i] = physicalIndexOfCenter(xCenters, static_cast<int>(i), stepX);
    std::vector<int> yPhysIdx(yCenters.size());
    for (size_t i = 0; i < yCenters.size(); ++i)
        yPhysIdx[i] = physicalIndexOfCenter(yCenters, static_cast<int>(i), stepY);

    const quint64 W64 = static_cast<quint64>(xPhysIdx.back()) + 1;
    const quint64 H64 = static_cast<quint64>(yPhysIdx.back()) + 1;
    if (W64 * H64 > kPlyPcdMaxPoints)
        return false;
    // 网格单元数相对点数过大的防御性上限：不再用于判定"是不是网格"（真实稀疏网格占用率
    // 可能低至一二十百分比，那个判断职责已经交给上面的 axisStepValid 整数倍校验），
    // 这里只是防止步长估计出错时网格失控膨胀，撑爆内存。
    if (static_cast<double>(W64) * static_cast<double>(H64) > static_cast<double>(N) * 200.0)
        return false;

    const int W = static_cast<int>(W64);
    const int H = static_cast<int>(H64);
    std::vector<float>  grid(static_cast<size_t>(W) * static_cast<size_t>(H), std::numeric_limits<float>::quiet_NaN());
    std::vector<quint8> occupied(static_cast<size_t>(W) * static_cast<size_t>(H), 0);
    const bool wantBrightness = points.anyIntensity || points.anyColor;
    std::vector<quint8> bright;
    if (wantBrightness)
        bright.assign(static_cast<size_t>(W) * static_cast<size_t>(H), 0);

    const double tolX = stepX * 0.3;
    const double tolY = stepY * 0.3;
    for (const PlyPcdPoint& p : points.pts) {
        const int cIdx = nearestCenterIndex(xCenters, static_cast<double>(p.x), tolX);
        const int rIdx = nearestCenterIndex(yCenters, static_cast<double>(p.y), tolY);
        if (cIdx < 0 || rIdx < 0)
            return false; // 有点偏离所有列/行中心——不是规则网格
        const int col = xPhysIdx[static_cast<size_t>(cIdx)];
        const int row = yPhysIdx[static_cast<size_t>(rIdx)];
        const size_t idx = static_cast<size_t>(row) * static_cast<size_t>(W) + static_cast<size_t>(col);
        if (occupied[idx])
            return false; // 重复格子（同一 XY 多个 Z，如悬垂/多层结构）——交给无序回退路径
        occupied[idx] = 1;
        grid[idx] = p.z;
        if (wantBrightness) {
            bright[idx] = p.hasIntensity
                ? p.intensity
                : (p.hasColor ? static_cast<quint8>((static_cast<int>(p.r) + p.g + p.b) / 3) : 0);
        }
    }

    out.W = W;
    out.H = H;
    out.floatData = std::move(grid);
    out.resX = stepX;
    out.resY = stepY;
    out.offX = xCenters.front();
    out.offY = yCenters.front();
    if (wantBrightness)
        out.embeddedBrightness = std::move(bright);
    return true;
}

} // anonymous namespace

//! 只读 PLY/PCD 文本头部拿到顶点/点数（不读顶点数据，供 UI 线程廉价预检）
bool peekPlyPcdPointCount(const QString& path, qint64& outCount, QString* outError)
{
    outCount = 0;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (outError) *outError = QStringLiteral("无法打开文件: %1").arg(path);
        return false;
    }

    if (isPlyPath(path)) {
        PlyFormat format = PlyFormat::Ascii;
        std::vector<PlyElement> elements;
        if (!readPlyHeader(f, format, elements, outError))
            return false;
        for (const PlyElement& el : elements) {
            if (el.name.compare(QLatin1String("vertex"), Qt::CaseInsensitive) == 0) {
                outCount = el.count;
                return true;
            }
        }
        if (outError) *outError = QStringLiteral("PLY 文件缺少 vertex element");
        return false;
    }

    if (isPcdPath(path)) {
        while (!f.atEnd()) {
            const QString sline = QString::fromLatin1(f.readLine()).trimmed();
            if (sline.isEmpty() || sline.startsWith(QLatin1Char('#')))
                continue;
            const QStringList tok = sline.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (tok.isEmpty())
                continue;
            if (tok[0].compare(QLatin1String("POINTS"), Qt::CaseInsensitive) == 0 && tok.size() >= 2) {
                outCount = tok[1].toLongLong();
                return true;
            }
            if (tok[0].compare(QLatin1String("DATA"), Qt::CaseInsensitive) == 0)
                break;
        }
        if (outError) *outError = QStringLiteral("PCD 头部缺少 POINTS");
        return false;
    }

    if (outError) *outError = QStringLiteral("不支持的点云文件扩展名");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────

ccHObject* load(
    const QString& tiffPath,
    const QString& bmpPath,
    double  resX,
    double  resY,
    double  resZ,
    DisplayMode mode,
    float   fusionAlpha,
    bool    buildMesh,
    float   colorRangeMin,
    float   colorRangeMax,
    double  zInvalidValue,
    bool    useSyntheticBmp,
    int*    outBitDepth,
    bool    removeIslands,
    int     minIslandPixels,
    int     minIslandDist,
    double  maxZGap,
    double  maxEdgeLength,
    bool    computeNormals,
    double  offX,
    double  offY,
    double  offZ,
    bool    reinterpretAsSignedZ,
    const TiffInfo* preparedInfo,
    QString* outError,
    int     yStride,
    unsigned cloudUID,
    unsigned meshUID,
    bool*   outFallbackToUnorganized)
{
    Q_UNUSED(minIslandDist);
    if (outFallbackToUnorganized) *outFallbackToUnorganized = false;

    TiffInfo info;
    int W = 0, H = 0;
    const bool wantsBmp = (mode == DisplayMode::Brightness || mode == DisplayMode::Fusion);
    std::vector<quint8> embeddedBrightness; // 来自 2 通道 TIFF / SRF / 规则网格 PLY/PCD 的亮度通道
    std::vector<quint8> embeddedRGB;        // 来自 SUR RGBSURFACE/RGBINTENSITYSURFACE 的真彩色，交织 RGB
    std::vector<float>  floatData;          // Z 高度，单位 mm（对 SRF/网格 PLY/PCD 已 baked scale+offset）
    bool bakedGridBranch = false;           // SRF 或网格重建成功的 PLY/PCD：数据已烘焙好，跳过 TIFF 专属读取

    if (isSrfPath(tiffPath))
    {
        // ── SRF 分支：自带 scale/offset 与可选亮度，忽略所有 panel 输入的分辨率/偏移
        SrfRawData raw;
        QString srfErr;
        if (!readSrfRaw(tiffPath, raw, &srfErr)) {
            if (outError) *outError = srfErr;
            return nullptr;
        }
        W = raw.W;
        H = raw.L;

        const size_t N = static_cast<size_t>(W) * H;
        floatData.resize(N);
        // 将 int16 高度转换为世界 Z (mm)；无效点写 NaN，后续 isInvalid 自动跳过
        for (size_t i = 0; i < N; ++i) {
            const qint16 z = raw.points[i];
            floatData[i] = (z == kSrfInvalidZ)
                ? std::numeric_limits<float>::quiet_NaN()
                : static_cast<float>(z * raw.scaleZ + raw.offsetZ);
        }
        if (!raw.intensity.empty())
            embeddedBrightness = std::move(raw.intensity);

        // 覆盖 panel 传入的分辨率/偏移：SRF 自带，禁止外部叠加
        resX = raw.scaleX;
        resY = raw.scaleY;
        resZ = 1.0;                       // 已 baked 到 floatData
        offX = raw.offsetX;
        offY = raw.offsetY;
        offZ = 0.0;                       // 已 baked

        // 标记走 32-bit float 路径（floatData 已填充）；info 留空
        info = TiffInfo{};
        info.valid     = true;
        info.width     = W;
        info.height    = H;
        info.is32Float = true;
        info.bitDepth  = 32;
        bakedGridBranch = true;
    }
    else if (isSurPath(tiffPath))
    {
        // ── SUR 分支：自带 scale/offset，忽略所有 panel 输入的分辨率/偏移
        SurRawData raw;
        QString surErr;
        if (!readSurRaw(tiffPath, raw, &surErr)) {
            if (outError) *outError = surErr;
            return nullptr;
        }
        W = raw.W;
        H = raw.H;

        const size_t N = static_cast<size_t>(W) * H;
        floatData.resize(N);
        // 将 int32 高度转换为世界 Z (mm)；无效点写 NaN，后续 isInvalid 自动跳过
        for (size_t i = 0; i < N; ++i) {
            const qint32 z = raw.points[i];
            const bool invalid = raw.hasSpecialPoints && (z == raw.zmin - 2);
            floatData[i] = invalid
                ? std::numeric_limits<float>::quiet_NaN()
                : static_cast<float>((z - raw.zmin) * raw.scaleZ + raw.offsetZ);
        }
        if (!raw.rgb.empty())
            embeddedRGB = std::move(raw.rgb);
        else if (!raw.intensity.empty())
            embeddedBrightness = std::move(raw.intensity);

        // 覆盖 panel 传入的分辨率/偏移：SUR 自带，禁止外部叠加
        resX = raw.scaleX;
        resY = raw.scaleY;
        resZ = 1.0;                       // 已 baked 到 floatData
        offX = raw.offsetX;
        offY = raw.offsetY;
        offZ = 0.0;                       // 已 baked

        info = TiffInfo{};
        info.valid     = true;
        info.width     = W;
        info.height    = H;
        info.is32Float = true;
        info.bitDepth  = 32;
        bakedGridBranch = true;
    }
    else if (isPlyPath(tiffPath) || isPcdPath(tiffPath))
    {
        // ── PLY / PCD 分支：优先尝试还原为规则网格（复用 SRF 的烘焙管线）；
        //    失败则自动降级为普通无序点云（不报错，早返回）
        PlyPcdPoints pcPoints;
        QString pcErr;
        if (!readPlyPcdPoints(tiffPath, pcPoints, &pcErr)) {
            if (outError) *outError = pcErr;
            return nullptr;
        }

        UniformGridResult grid;
        if (tryBuildUniformGrid(pcPoints, grid))
        {
            W = grid.W;
            H = grid.H;
            floatData          = std::move(grid.floatData);
            embeddedBrightness = std::move(grid.embeddedBrightness);
            resX = grid.resX;
            resY = grid.resY;
            resZ = 1.0;   // 已 baked 到 floatData
            offX = grid.offX;
            offY = grid.offY;
            offZ = 0.0;   // 已 baked

            info = TiffInfo{};
            info.valid     = true;
            info.width     = W;
            info.height    = H;
            info.is32Float = true;
            info.bitDepth  = 32;
            bakedGridBranch = true;
        }
        else
        {
            // ── 非规则网格：自动降级为普通无序点云（散点/间距不均/重复格点均走这里）──
            if (outFallbackToUnorganized) *outFallbackToUnorganized = true;

            const size_t N = pcPoints.pts.size();
            double zMinD = std::numeric_limits<double>::max();
            double zMaxD = std::numeric_limits<double>::lowest();
            for (const PlyPcdPoint& p : pcPoints.pts) {
                zMinD = std::min(zMinD, static_cast<double>(p.z));
                zMaxD = std::max(zMaxD, static_cast<double>(p.z));
            }
            const double zRangeD = (zMaxD > zMinD) ? (zMaxD - zMinD) : 1.0;
            // 高度彩图取色窗口：与规则网格/TIFF 主流水线一致，按 colorRangeMin/Max 收窄 Z 值域
            const float cMin  = static_cast<float>(zMinD) + colorRangeMin * static_cast<float>(zRangeD);
            const float cMax  = static_cast<float>(zMinD) + colorRangeMax * static_cast<float>(zRangeD);
            const float cSpan = (cMax > cMin) ? (cMax - cMin) : 1.f;
            const auto& lut = colorLUT();

            ccPointCloud* cloud = new ccPointCloudNoBB(QFileInfo(tiffPath).baseName(), cloudUID);
            if (!cloud->reserve(static_cast<unsigned>(N))) {
                delete cloud;
                if (outError) *outError = QString("内存分配失败（点数：%1）").arg(N);
                return nullptr;
            }
            cloud->reserveTheRGBTable();

            for (const PlyPcdPoint& p : pcPoints.pts) {
                // 散点无"列索引"概念：保留文件原始坐标，不做网格路径的 X 取反镜像
                cloud->addPoint(CCVector3(
                    static_cast<PointCoordinateType>(p.x),
                    static_cast<PointCoordinateType>(p.y),
                    static_cast<PointCoordinateType>(p.z)));

                // 高度与亮度分开计算：高度彩图始终由 Z 值重新映射得到；
                // 亮度优先用文件自带 intensity/RGB 灰度，否则按“之前的逻辑”用高度归一化合成亮度
                float norm = (static_cast<float>(p.z) - cMin) / cSpan;
                norm = std::max(0.f, std::min(1.f, norm));
                const int lutIdx = std::min(static_cast<int>(norm * 1536.f), 1536);
                const quint8 bv = p.hasIntensity
                    ? p.intensity
                    : (p.hasColor
                        ? static_cast<quint8>((static_cast<int>(p.r) + p.g + p.b) / 3)
                        : static_cast<quint8>(norm * 255.f));

                ccColor::Rgb color;
                switch (mode) {
                case DisplayMode::Brightness:
                    color = ccColor::Rgb(bv, bv, bv);
                    break;
                case DisplayMode::Fusion: {
                    const ccColor::Rgb& hc = lut[static_cast<size_t>(lutIdx)];
                    const float bf = (bv / 255.f) * 0.5f;
                    const float hw = 1.f - fusionAlpha;
                    auto blend = [](float h, float b) -> quint8 {
                        return static_cast<quint8>(std::max(0.f, std::min(h + b * 255.f, 255.f)));
                    };
                    color = ccColor::Rgb(blend(hc.r * hw, bf), blend(hc.g * hw, bf), blend(hc.b * hw, bf));
                    break;
                }
                case DisplayMode::HeightGray: {
                    const quint8 g = static_cast<quint8>(lutIdx * 255 / 1536);
                    color = ccColor::Rgb(g, g, g);
                    break;
                }
                case DisplayMode::Height:
                default:
                    color = lut[static_cast<size_t>(lutIdx)];
                    break;
                }
                cloud->addColor(color);
            }
            cloud->showColors(true);
            if (outBitDepth) *outBitDepth = 32;
            return cloud;
        }
    }
    else
    {
        if (preparedInfo && preparedInfo->valid)
            info = *preparedInfo;
        else
            buildTiffInfo(tiffPath, info);

        // ── 优先尝试 2 通道"128位"TIFF，否则回退到单通道 32-bit / 16-bit ──────
        W = info.width;
        H = info.height;
    }

    Tiff2ChData twoChData;
    Tiff96BitXYZData xyz96;
    if (!bakedGridBranch) {
        twoChData = (info.valid && info.is2Channel)
            ? tryReadTiff2Channel(tiffPath, info)
            : Tiff2ChData{};
    }
    const bool is2Channel = (!bakedGridBranch && twoChData.W > 0 && twoChData.H > 0);
    if (is2Channel) {
        W = twoChData.W;
        H = twoChData.H;
    }

    // ── 96-bit 3 通道 XYZ 解析（X/Y/Z 均为 mm 世界坐标）──────────────
    if (!bakedGridBranch) {
        xyz96 = (info.valid && info.is96BitXYZ)
            ? tryReadTiff96BitXYZ(tiffPath, info)
            : Tiff96BitXYZData{};
    }
    const bool is96BitXYZ = (!bakedGridBranch && xyz96.W > 0 && xyz96.H > 0);
    if (is96BitXYZ) {
        W = xyz96.W;
        H = xyz96.H;
    }

    std::vector<float> xRawData;       // 96-bit XYZ 的 X 通道
    std::vector<float> yRawData;       // 96-bit XYZ 的 Y 通道
    std::vector<bool>  zeroTripletMask;// 96-bit XYZ：X=Y=Z=0 视为无效（结构光相机典型空值）
    if (bakedGridBranch) {
        // floatData / embeddedBrightness 已在 SRF / 网格 PLY/PCD 分支填充
    } else if (is2Channel) {
        floatData          = std::move(twoChData.height);
        embeddedBrightness = std::move(twoChData.brightness);
    } else if (is96BitXYZ) {
        xRawData  = std::move(xyz96.X);
        yRawData  = std::move(xyz96.Y);
        floatData = std::move(xyz96.Z);
        const size_t N = static_cast<size_t>(W) * static_cast<size_t>(H);
        zeroTripletMask.assign(N, false);
        for (size_t i = 0; i < N; ++i) {
            if (xRawData[i] == 0.f && yRawData[i] == 0.f && floatData[i] == 0.f)
                zeroTripletMask[i] = true;
        }
    } else if (info.valid && info.is32Float) {
        floatData = tryReadTiff32Float(tiffPath, info);
    }
    const bool is32bit = !floatData.empty();
    bool is16BitSigned = info.valid && info.is16BitSigned;

    const double actualResZ = is32bit ? 1.0 : resZ;

    // ── 96-bit XYZ：由 TIFF 中 X/Y 通道反推 X/Y 起点偏移（中位数，剔除离群）──
    // 公式：world_x = col*resX + inferredOffX + user_offX
    //       inferredOffX = median(x_raw - col*resX)，仅对非零三元组像素计算
    double inferredOffX = 0.0;
    double inferredOffY = 0.0;
    if (is96BitXYZ) {
        std::vector<double> dxBuf, dyBuf;
        dxBuf.reserve(static_cast<size_t>(W) * H);
        dyBuf.reserve(static_cast<size_t>(W) * H);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const size_t i = static_cast<size_t>(y) * W + x;
                if (zeroTripletMask[i]) continue;
                const float xr = xRawData[i];
                const float yr = yRawData[i];
                if (!std::isfinite(xr) || !std::isfinite(yr)) continue;
                dxBuf.push_back(static_cast<double>(xr) - x * resX);
                dyBuf.push_back(static_cast<double>(yr) - y * resY);
            }
        }
        if (!dxBuf.empty()) {
            const size_t mid = dxBuf.size() / 2;
            std::nth_element(dxBuf.begin(), dxBuf.begin() + mid, dxBuf.end());
            inferredOffX = dxBuf[mid];
            std::nth_element(dyBuf.begin(), dyBuf.begin() + mid, dyBuf.end());
            inferredOffY = dyBuf[mid];
        }
    }

    // ── 读取 16-bit TIFF（非 32-bit 路径）──────────────────────────────
    QImage tiffImg;
    if (!is32bit) {
        QImage raw;

        // 内置 LZW + Predictor=2 解码路径：Qt 的 QImageReader 对 LZW+Predictor=2
        // 的 16-bit chunky TIFF 在大尺寸下会触发崩溃；直接走自己的 strip 解码器。
        if (info.compression == 5
            && info.samplesPerPixel == 1
            && info.bitsPerSample.value(0, 0) == 16
            && info.planarConfig == 1)
        {
            std::vector<quint16> u16 = tryReadTiff16Lzw(tiffPath, info);
            if (!u16.empty()
                && static_cast<int>(u16.size()) == info.width * info.height)
            {
                raw = QImage(info.width, info.height, QImage::Format_Grayscale16);
                if (!raw.isNull()) {
                    const int W2 = info.width;
                    const int H2 = info.height;
                    for (int y = 0; y < H2; ++y) {
                        memcpy(raw.scanLine(y),
                               u16.data() + static_cast<size_t>(y) * W2,
                               static_cast<size_t>(W2) * 2);
                    }
                }
            }
        }

        QImageReader tiffReader(tiffPath);
        tiffReader.setAutoTransform(false);
        if (raw.isNull())
            raw = tiffReader.read();
        if (raw.isNull()) {
            // 编译诊断信息：把已解析到的 TIFF 元数据一并打印，便于定位是哪种格式不支持
            auto compressionName = [](int c) -> QString {
                switch (c) {
                    case 1:     return "None";
                    case 2:     return "CCITT 1D";
                    case 3:     return "CCITT G3";
                    case 4:     return "CCITT G4";
                    case 5:     return "LZW";
                    case 6:     return "JPEG(old)";
                    case 7:     return "JPEG";
                    case 8:     return "Deflate(ZIP)";
                    case 32773: return "PackBits";
                    case 32946: return "Deflate(old)";
                    case 34712: return "JPEG2000";
                    case 50000: return "ZSTD";
                    case 50001: return "WebP";
                    default:    return QString("Unknown(%1)").arg(c);
                }
            };
            auto predictorName = [](int p) -> QString {
                switch (p) {
                    case 1: return "None";
                    case 2: return "Horizontal";
                    case 3: return "FloatingPoint";
                    default: return QString("Unknown(%1)").arg(p);
                }
            };
            auto formatName = [](int f) -> QString {
                switch (f) {
                    case 1: return "uint";
                    case 2: return "int";
                    case 3: return "float";
                    case 4: return "void";
                    default: return QString::number(f);
                }
            };
            QStringList bps, sfs;
            for (quint32 b : info.bitsPerSample)  bps << QString::number(b);
            for (quint32 s : info.sampleFormats)  sfs << formatName(static_cast<int>(s));

            if (outError) *outError =
                QString("无法读取 TIFF 文件: %1\n"
                        "─ 诊断信息（来自 IFD 解析）─\n"
                        "  尺寸:        %2 × %3\n"
                        "  压缩:        %4 (Tag259=%5)\n"
                        "  Predictor:   %6 (Tag317=%7)\n"
                        "  位深/通道:   %8 bit × %9 ch\n"
                        "  SampleFormat: %10\n"
                        "  PlanarConfig: %11 (1=chunky, 2=planar)\n"
                        "  StripCount:   %12\n"
                        "提示：Qt 内置 TIFF 解码器对 LZW+Predictor=2、JPEG 压缩、"
                        "JPEG2000、ZSTD 等格式支持有限；如其他软件能读取，请尝试"
                        "用 ImageMagick / tifffile 重新保存为「未压缩 TIFF」或「Deflate」。")
                .arg(tiffReader.errorString())
                .arg(info.width).arg(info.height)
                .arg(compressionName(info.compression)).arg(info.compression)
                .arg(predictorName(info.predictor)).arg(info.predictor)
                .arg(bps.isEmpty() ? "?" : bps.join('/'))
                .arg(info.samplesPerPixel)
                .arg(sfs.isEmpty() ? "?" : sfs.join('/'))
                .arg(info.planarConfig)
                .arg(info.stripOffsets.size());
            return nullptr;
        }
        tiffImg = raw.convertToFormat(QImage::Format_Grayscale16);
        if (tiffImg.isNull()) {
            if (outError) *outError =
                "TIFF 格式转换失败（不支持的通道布局或位深）";
            return nullptr;
        }
        W = tiffImg.width();
        H = tiffImg.height();
    }

    if (W <= 0 || H <= 0) {
        if (outError) *outError = "TIFF 尺寸无效，无法生成点云/网格";
        return nullptr;
    }

    if (outBitDepth)
        *outBitDepth = is96BitXYZ ? 96
                     : is2Channel ? 128
                     : is32bit ? 32
                     : (is16BitSigned ? 17 : 16);

    // ── 读取外部亮度图（可选）────────────────────────────────────────────
    QImage bmpImg;
    bool hasBmp = false;
    if (wantsBmp && !bmpPath.isEmpty()) {
        QImageReader bmpReader(bmpPath);
        bmpImg = bmpReader.read();
        if (bmpImg.isNull()) {
            hasBmp = false;
        } else {
            bmpImg = bmpImg.convertToFormat(QImage::Format_Grayscale8);
            if (bmpImg.width() != W || bmpImg.height() != H)
                bmpImg = bmpImg.scaled(W, H, Qt::IgnoreAspectRatio,
                                       Qt::FastTransformation);
            hasBmp = !bmpImg.isNull();
        }
    }

    // 若无外部亮度图但有内嵌亮度通道（2 通道 TIFF 或 SRF v2），用其替代外部 BMP
    if (!hasBmp && wantsBmp && !embeddedBrightness.empty()
        && static_cast<int>(embeddedBrightness.size()) == W * H) {
        bmpImg = QImage(W, H, QImage::Format_Grayscale8);
        for (int y = 0; y < H; ++y)
            memcpy(bmpImg.scanLine(y),
                   embeddedBrightness.data() + static_cast<size_t>(y) * W,
                   static_cast<size_t>(W));
        hasBmp = true;
    }

    // ── 基础有效性检查（仅 zInvalidValue，不含小岛）──────────────────────
    // 过滤规则：原始值 <= zInvalidValue 的点视为无效
    const float   invF = static_cast<float>(zInvalidValue);
    const quint16 invU = static_cast<quint16>(
        (zInvalidValue < 0) ? 0 : (zInvalidValue > 65535) ? 65535
                                                           : zInvalidValue);
    const qint16  invS = static_cast<qint16>(
        std::max(-32768.0, std::min(32767.0, zInvalidValue)));

    auto getZ32 = [&](int x, int y) -> float {
        return floatData[static_cast<size_t>(y * W + x)];
    };
    auto getZ16 = [&](int x, int y) -> quint16 {
        return reinterpret_cast<const quint16*>(tiffImg.constScanLine(y))[x];
    };
    auto isInvalidBase = [&](int x, int y) -> bool {
        // 96-bit XYZ：X=Y=Z=0 三元零点（结构光相机典型空值）直接判无效
        if (is96BitXYZ && zeroTripletMask[static_cast<size_t>(y) * W + x])
            return true;
        if (is32bit) { const float v = getZ32(x,y); return std::isnan(v) || v <= invF; }
        if (is16BitSigned) return static_cast<qint16>(getZ16(x, y)) <= invS;
        return getZ16(x, y) <= invU;
    };

    // Y 方向等步长降采样：仅保留行号为 yStride 整数倍的行（yStride<=1 表示全采）
    // 保留行间距恒为 yStride×resY，网格模式下相邻行可正常三角化
    auto keepRow = [&](int y) -> bool {
        return yStride <= 1 || (y % yStride == 0);
    };

    // ── 第一遍：找有效 Z 范围（全部行，确保色彩映射一致）────────────────────
    double zMinD = std::numeric_limits<double>::max();
    double zMaxD = std::numeric_limits<double>::lowest();
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (isInvalidBase(x, y)) continue;
            const double v = is32bit
                ? static_cast<double>(getZ32(x,y))
                : (reinterpretAsSignedZ && !is16BitSigned)
                    ? static_cast<double>(static_cast<int>(getZ16(x,y)) - 32768)
                    : is16BitSigned
                        ? static_cast<double>(static_cast<qint16>(getZ16(x,y)))
                        : static_cast<double>(getZ16(x,y));
            if (v < zMinD) zMinD = v;
            if (v > zMaxD) zMaxD = v;
        }
    if (zMinD > zMaxD) {
        if (outError) *outError =
            QString("所有像素均为无效值（无效值阈值 %1），无有效点可生成")
            .arg(zInvalidValue);
        return nullptr;
    }
    const float zRange = static_cast<float>(zMaxD - zMinD);

    // ── Z 方向连通辅助函数（返回物理 mm 值）──────────────────────────────
    auto getZmm = [&](int x, int y) -> float {
        if (is32bit)
            return getZ32(x, y); // 32-bit float 已为 mm
        float raw;
        if (reinterpretAsSignedZ && !is16BitSigned)
            raw = static_cast<float>(static_cast<int>(getZ16(x, y)) - 32768);
        else
            raw = is16BitSigned
                ? static_cast<float>(static_cast<qint16>(getZ16(x, y)))
                : static_cast<float>(getZ16(x, y));
        return raw * static_cast<float>(resZ);
    };

    // ── 小岛噪声过滤 ──────────────────────────────────────────────────────
    std::vector<bool> islandMask;
    if (removeIslands && minIslandPixels > 1) {
        // 孤岛过滤时将降采样跳过行视为无效，避免产生跨行的伪分量
        auto isInvForIsland = [&](int x, int y) -> bool {
            if (!keepRow(y)) return true;
            return isInvalidBase(x, y);
        };
        islandMask = computeIslandMask(W, H, isInvForIsland, getZmm,
                                       static_cast<float>(maxZGap), minIslandPixels);
    }

    // 最终有效性检查（叠加降采样行跳过 + 小岛 mask）
    auto isInvalid = [&](int x, int y) -> bool {
        if (!keepRow(y)) return true;
        if (isInvalidBase(x, y)) return true;
        return !islandMask.empty() && !islandMask[static_cast<size_t>(y * W + x)];
    };

    qint64 validCount = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (!isInvalid(x, y))
                ++validCount;

    if (validCount <= 0) {
        if (outError) {
            *outError = removeIslands && !islandMask.empty()
                ? QString("小岛过滤后无有效点可生成（最小小岛像素=%1，最大Z跳变=%2 mm）")
                      .arg(minIslandPixels)
                      .arg(maxZGap)
                : QString("所有像素均为无效值（无效值阈值 %1），无有效点可生成")
                      .arg(zInvalidValue);
        }
        return nullptr;
    }

    if (validCount > static_cast<qint64>(std::numeric_limits<unsigned>::max())) {
        if (outError) {
            *outError = QString("有效点数过大，超出点云容量上限：%1")
                            .arg(validCount);
        }
        return nullptr;
    }

    // ── HeightGray / BMP / 合成亮度 ───────────────────────────────────────
    // Height / HeightGray 不需要亮度图；Brightness / Fusion 才读取/合成亮度
    // useSyntheticBmp：不预生成 QImage，在第二遍逐像素按需计算
    const bool synthBmp = (wantsBmp && !hasBmp && useSyntheticBmp);
    if (synthBmp) hasBmp = true; // 标记已有亮度来源（来自合成）
    if (wantsBmp && !hasBmp)
        mode = DisplayMode::Height;

    // ── 颜色映射窗口 ──────────────────────────────────────────────────────
    const float cMin  = static_cast<float>(zMinD) + colorRangeMin * zRange;
    const float cMax  = static_cast<float>(zMinD) + colorRangeMax * zRange;
    const float cSpan = (cMax > cMin) ? (cMax - cMin) : 1.f;
    const auto& lut   = colorLUT();

    // ── 分配点云 ──────────────────────────────────────────────────────────
    ccPointCloud* cloud = new ccPointCloudNoBB(QFileInfo(tiffPath).baseName(), cloudUID);
    if (!cloud->reserve(static_cast<unsigned>(validCount))) {
        delete cloud;
        if (outError) *outError =
            QString("内存分配失败（有效点数：%1，图像尺寸：%2 × %3）")
            .arg(validCount).arg(W).arg(H);
        return nullptr;
    }
    cloud->reserveTheRGBTable();

    std::vector<int> indexGrid;
    if (buildMesh)
        indexGrid.assign(static_cast<size_t>(W * H), -1);

    // 颜色计算辅助（bv：0-255 亮度值，已由调用方计算好）
    auto computeColor = [&](quint8 bv, int lutIdx) -> ccColor::Rgb {
        switch (mode) {
        case DisplayMode::Height:
            return lut[static_cast<size_t>(lutIdx)];
        case DisplayMode::Brightness:
            return ccColor::Rgb(bv, bv, bv);
        case DisplayMode::Fusion: {
            const ccColor::Rgb& hc = lut[static_cast<size_t>(lutIdx)];
            const float bf = (bv / 255.f) * 0.5f;
            const float hw = 1.f - fusionAlpha;
            auto blend = [](float h, float b) -> quint8 {
                return static_cast<quint8>(std::max(0.f, std::min(h + b * 255.f, 255.f)));
            };
            return ccColor::Rgb(
                blend(hc.r * hw, bf),
                blend(hc.g * hw, bf),
                blend(hc.b * hw, bf));
        }
        case DisplayMode::HeightGray: {
            const quint8 g = static_cast<quint8>(lutIdx * 255 / 1536);
            return ccColor::Rgb(g, g, g);
        }
        }
        return ccColor::Rgb(128, 128, 128);
    };

    // ── 第二遍：逐像素填充 ────────────────────────────────────────────────
    int ptIdx = 0;
    for (int y = 0; y < H; ++y) {
        const quint8* bRow =
            (hasBmp && !synthBmp)
                ? reinterpret_cast<const quint8*>(bmpImg.constScanLine(y))
                : nullptr;

        if (is32bit) {
            for (int x = 0; x < W; ++x) {
                if (isInvalid(x, y)) continue;
                const float v = getZ32(x, y);

                if (buildMesh)
                    indexGrid[static_cast<size_t>(y * W + x)] = ptIdx;
                ++ptIdx;

                cloud->addPoint(CCVector3(
                    static_cast<PointCoordinateType>(-(x * resX + inferredOffX + offX)),
                    static_cast<PointCoordinateType>(y * resY + inferredOffY + offY),
                    static_cast<PointCoordinateType>(v * actualResZ + offZ)));

                // 真彩色（SUR RGBSURFACE/RGBINTENSITYSURFACE）优先于显示模式：
                // 有真实 RGB 时固定按真彩色渲染，不受高度色彩/融合等模式影响
                if (!embeddedRGB.empty()) {
                    const size_t rgbIdx = (static_cast<size_t>(y) * W + x) * 3;
                    cloud->addColor(ccColor::Rgb(embeddedRGB[rgbIdx],
                                                  embeddedRGB[rgbIdx + 1],
                                                  embeddedRGB[rgbIdx + 2]));
                } else {
                    float norm = (v - cMin) / cSpan;
                    norm = std::max(0.f, std::min(1.f, norm));
                    const quint8 bv = bRow ? bRow[x]
                        : (synthBmp ? static_cast<quint8>(norm * 255.f) : 128u);
                    cloud->addColor(computeColor(bv,
                        std::min(static_cast<int>(norm * 1536.f), 1536)));
                }
            }
        } else {
            const auto* tRow =
                reinterpret_cast<const quint16*>(tiffImg.constScanLine(y));
            for (int x = 0; x < W; ++x) {
                if (isInvalid(x, y)) continue;

                const float fv = (reinterpretAsSignedZ && !is16BitSigned)
                    ? static_cast<float>(static_cast<int>(tRow[x]) - 32768)
                    : (is16BitSigned
                        ? static_cast<float>(static_cast<qint16>(tRow[x]))
                        : static_cast<float>(tRow[x]));

                if (buildMesh)
                    indexGrid[static_cast<size_t>(y * W + x)] = ptIdx;
                ++ptIdx;

                cloud->addPoint(CCVector3(
                    static_cast<PointCoordinateType>(-(x * resX + offX)),
                    static_cast<PointCoordinateType>(y * resY + offY),
                    static_cast<PointCoordinateType>(fv * actualResZ + offZ)));

                float norm = (fv - cMin) / cSpan;
                norm = std::max(0.f, std::min(1.f, norm));
                const quint8 bv = bRow ? bRow[x]
                    : (synthBmp ? static_cast<quint8>(norm * 255.f) : 128u);
                cloud->addColor(computeColor(bv,
                    std::min(static_cast<int>(norm * 1536.f), 1536)));
            }
        }
    }

    cloud->showColors(true);

    if (!buildMesh) return cloud;

    // ── 网格模式：有序网格三角化（含最大边长过滤）──────────────────────
    // 最大边长检查：跳过跨越空洞/大高度跳变的悬空三角形
    const float maxEdgeSq = (maxEdgeLength > 0.0)
        ? static_cast<float>(maxEdgeLength * maxEdgeLength) : -1.f;

    auto edgeOk = [&](int ia, int ib) -> bool {
        if (maxEdgeSq < 0.f) return true;
        const CCVector3* pa = cloud->getPoint(static_cast<unsigned>(ia));
        const CCVector3* pb = cloud->getPoint(static_cast<unsigned>(ib));
        return (*pb - *pa).norm2() <= maxEdgeSq;
    };
    auto triOk = [&](int a, int b, int c) -> bool {
        return a >= 0 && b >= 0 && c >= 0
            && edgeOk(a, b) && edgeOk(b, c) && edgeOk(a, c);
    };

    // Y 方向三角化步长 = yStride（降采样时相邻保留行间距为 yStride 行）
    const int yStep = std::max(1, yStride);

    unsigned nTris = 0;
    for (int y = 0; y + yStep < H; y += yStep)
        for (int x = 0; x < W - 1; ++x) {
            const int i00 = indexGrid[static_cast<size_t>(y * W + x)];
            const int i10 = indexGrid[static_cast<size_t>(y * W + x + 1)];
            const int i01 = indexGrid[static_cast<size_t>((y + yStep) * W + x)];
            const int i11 = indexGrid[static_cast<size_t>((y + yStep) * W + x + 1)];
            if (triOk(i00, i10, i11)) ++nTris;
            if (triOk(i00, i11, i01)) ++nTris;
        }

    if (nTris == 0) return cloud;

    ccMesh* mesh = new ccMeshNoBB(cloud, meshUID);
    mesh->setName(cloud->getName());
    mesh->addChild(cloud);
    cloud->setEnabled(false);

    if (!mesh->reserve(nTris)) {
        mesh->detachChild(cloud); cloud->setEnabled(true);
        delete mesh; return cloud;
    }

    for (int y = 0; y + yStep < H; y += yStep)
        for (int x = 0; x < W - 1; ++x) {
            const int i00 = indexGrid[static_cast<size_t>(y * W + x)];
            const int i10 = indexGrid[static_cast<size_t>(y * W + x + 1)];
            const int i01 = indexGrid[static_cast<size_t>((y + yStep) * W + x)];
            const int i11 = indexGrid[static_cast<size_t>((y + yStep) * W + x + 1)];
            if (triOk(i00, i10, i11))
                mesh->addTriangle(
                    static_cast<unsigned>(i00),
                    static_cast<unsigned>(i10),
                    static_cast<unsigned>(i11));
            if (triOk(i00, i11, i01))
                mesh->addTriangle(
                    static_cast<unsigned>(i00),
                    static_cast<unsigned>(i11),
                    static_cast<unsigned>(i01));
        }

    mesh->showColors(true);
    if (computeNormals) {
        // 用 CC 内置方法计算 per-vertex 平滑法线（比手动差分更可靠）
        if (mesh->computeNormals(true))
            mesh->showNormals(true);
        else
            mesh->showNormals(false);
    } else {
        mesh->showNormals(false);
    }
    return mesh;
}

// ── 轻量位深检测（仅读 IFD，不读像素）────────────────────────────────────────
int detectBitDepth(const QString& path, TiffInfo* outInfo)
{
    TiffInfo info;
    const bool ok = buildTiffInfo(path, info);
    if (outInfo)
        *outInfo = info;
    return ok ? info.bitDepth : 16;
}

// ─────────────────────────────────────────────────────────────────────────────
// ── TIFF / SRF 导出（writer）─────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// ─── LZW 编码器（TIFF 风格，MSB-first 9..12 bit，含 libtiff early-change 规则）──
class LzwEncoder
{
    static constexpr int kClearCode = 256;
    static constexpr int kEoiCode   = 257;
    static constexpr int kFirstFree = 258;
    static constexpr int kMaxCode   = 4096;

public:
    QByteArray encode(const quint8* src, qint64 size)
    {
        out.clear();
        out.reserve(static_cast<int>(size + size / 8 + 64));
        bitsBuffer = 0; bitsInBuffer = 0;
        codeSize = 9; nextCode = kFirstFree;
        dict.clear();

        emit_code(kClearCode);
        if (size == 0) { emit_code(kEoiCode); flush_bits(); return out; }

        QByteArray w(1, static_cast<char>(src[0]));
        for (qint64 i = 1; i < size; ++i) {
            const char c = static_cast<char>(src[i]);
            QByteArray wc = w; wc.append(c);
            const int wcCode = lookup(wc);
            if (wcCode >= 0) {
                w = std::move(wc);
            } else {
                emit_code(lookup(w));
                if (nextCode < kMaxCode) {
                    dict.insert(wc, nextCode);
                    ++nextCode;
                    // LZW (TIFF "early change") 同步约定：
                    //   解码端每个 code 后才"补加"前一码的字典项（首码除外），
                    //   nextEntry 永远比编码端 nextCode 少 1；
                    //   解码器在 nextEntry == (1<<n)-1 时切码长，
                    //   所以编码器要在 nextCode == (1<<n) 时切——晚一拍。
                    if (nextCode == (1u << codeSize) && codeSize < 12)
                        ++codeSize;
                    if (nextCode >= kMaxCode) {
                        emit_code(kClearCode);
                        dict.clear();
                        nextCode = kFirstFree;
                        codeSize = 9;
                    }
                }
                w = QByteArray(1, c);
            }
        }
        emit_code(lookup(w));
        emit_code(kEoiCode);
        flush_bits();
        return out;
    }

private:
    int lookup(const QByteArray& s) const {
        if (s.size() == 1) return static_cast<quint8>(s[0]);
        auto it = dict.constFind(s);
        return (it != dict.constEnd()) ? it.value() : -1;
    }
    void emit_code(int code) {
        bitsBuffer = (bitsBuffer << codeSize) | static_cast<quint32>(code);
        bitsInBuffer += codeSize;
        while (bitsInBuffer >= 8) {
            const quint8 b = static_cast<quint8>(
                (bitsBuffer >> (bitsInBuffer - 8)) & 0xFF);
            out.append(static_cast<char>(b));
            bitsInBuffer -= 8;
            bitsBuffer &= (1u << bitsInBuffer) - 1;
        }
    }
    void flush_bits() {
        if (bitsInBuffer > 0) {
            const quint8 b = static_cast<quint8>(
                (bitsBuffer << (8 - bitsInBuffer)) & 0xFF);
            out.append(static_cast<char>(b));
            bitsBuffer = 0; bitsInBuffer = 0;
        }
    }
    QByteArray out;
    QHash<QByteArray, int> dict;
    quint32 bitsBuffer = 0;
    int     bitsInBuffer = 0;
    int     codeSize = 9;
    int     nextCode = kFirstFree;
};

// 16-bit Predictor=2 编码（行内水平差分，u16 模运算）
static void applyPredictor2Row_16bit(quint8* rowBytes, int W)
{
    quint16* p = reinterpret_cast<quint16*>(rowBytes);
    for (int x = W - 1; x >= 1; --x)
        p[x] = static_cast<quint16>(p[x] - p[x - 1]);
}

// 32-bit float Predictor=3 编码：行内字节平面化（MSB..LSB 按平面排列）+ 字节水平差分
// 输入 rowBytes 是机器原生字节序 float（x86 LE），输出 rowBytes 就地变为
// 解码端期望的格式（解码端 undoFpPredictor3Row 的逆过程）
static void applyPredictor3Row_float32(quint8* rowBytes, int W)
{
    const int bps = 4;
    const int rowSz = W * bps;
    std::vector<quint8> tmp(rowSz);
    // 1) 字节平面化：dst[b*W + c] = src[c*bps + (bps-1-b)]（小端机；MSB 平面在前）
    if constexpr (hostIsLittleEndian()) {
        for (int c = 0; c < W; ++c)
            for (int b = 0; b < bps; ++b)
                tmp[b * W + c] = rowBytes[c * bps + (bps - 1 - b)];
    } else {
        for (int c = 0; c < W; ++c)
            for (int b = 0; b < bps; ++b)
                tmp[b * W + c] = rowBytes[c * bps + b];
    }
    // 2) 行内逐字节水平差分（stride=1）
    for (int i = rowSz - 1; i >= 1; --i)
        tmp[i] = static_cast<quint8>(tmp[i] - tmp[i - 1]);
    memcpy(rowBytes, tmp.data(), rowSz);
}

// ── 完整 TIFF 写入：LZW + Predictor + GeoTIFF 标签 ───────────────────────────
//   bps:          16 或 32
//   sampleFormat: 1=uint, 2=int, 3=float
//   predictor:    1（无）、2（16-bit 水平差分）或 3（32-bit 浮点差分）
//   rawData:      未经 predictor 处理的原始像素（行优先，机器字节序）
//   dataBytes:    rawData 字节数 = W*H*(bps/8)
//   meta:         tag 用的标定元数据（mm；写入 33550/33922 与 ImageDescription）
//   storageStr:   ImageDescription 的 Storage 字段（"UInt16RawPlus32768" / "Int16Raw" / "Float32ActualHeight"）
//   hasNullValue: true 表示在 ImageDescription 加 NullValue= 行（Float32 用）
//   nullValue:    NullValue 字段值
static bool writeTiffLzwLE(const QString& path, int W, int H,
                            int bps, int sampleFormat, int predictor,
                            const void* rawData, qint64 dataBytes,
                            const TiffBmpLoader::ExportTiffMeta& meta,
                            const char* storageStr,
                            bool hasNullValue, double nullValue,
                            QString* outError)
{
    // ── 1) 应用 Predictor（按行）+ 串联所有行 ────────────────────────────
    const int bytesPerSample = bps / 8;
    const int rowBytes = W * bytesPerSample;
    QByteArray predicted;
    predicted.resize(static_cast<int>(dataBytes));
    memcpy(predicted.data(), rawData, dataBytes);

    if (predictor == 2 && bps == 16) {
        for (int y = 0; y < H; ++y) {
            quint8* rp = reinterpret_cast<quint8*>(predicted.data()) + y * rowBytes;
            applyPredictor2Row_16bit(rp, W);
        }
    } else if (predictor == 3 && bps == 32 && sampleFormat == 3) {
        for (int y = 0; y < H; ++y) {
            quint8* rp = reinterpret_cast<quint8*>(predicted.data()) + y * rowBytes;
            applyPredictor3Row_float32(rp, W);
        }
    }
    // predictor == 1：无差分，原样

    // ── 2) LZW 编码 ───────────────────────────────────────────────────────
    LzwEncoder lzw;
    const QByteArray stripData = lzw.encode(
        reinterpret_cast<const quint8*>(predicted.constData()), predicted.size());
    const qint64 stripBytes = stripData.size();

    // ── 3) 外置数据区准备（ImageDescription / Make / 标定值）─────────────
    // ImageDescription 文本块（lmi-gocator-geotiff-metadata 规约）
    QString desc;
    {
        QTextStream ts(&desc);
        ts.setRealNumberPrecision(12);
        ts.setRealNumberNotation(QTextStream::SmartNotation);
        ts << "LMI_Gocator_Metadata\n"
           << "Unit=mm\n"
           << "XResolution_mm=" << QString::number(meta.scaleX, 'g', 12) << "\n"
           << "YResolution_mm=" << QString::number(meta.scaleY, 'g', 12) << "\n"
           << "ZResolution_mm=" << QString::number(meta.scaleZ, 'g', 12) << "\n"
           << "XOffset_mm="     << QString::number(meta.offsetX, 'g', 12) << "\n"
           << "YOffset_mm="     << QString::number(meta.offsetY, 'g', 12) << "\n"
           << "ZOffset_mm="     << QString::number(meta.offsetZ, 'g', 12) << "\n";
        if (storageStr && storageStr[0])
            ts << "Storage=" << QString::fromLatin1(storageStr) << "\n";
        if (hasNullValue)
            ts << "NullValue=" << QString::number(nullValue, 'g', 12) << "\n";
    }
    QByteArray descBytes = desc.toUtf8();
    descBytes.append('\0');                 // ASCII tag 需以 NUL 结尾
    if (descBytes.size() & 1) descBytes.append('\0'); // 对齐到偶数字节

    QByteArray makeBytes("LMI TECHNOLOGIES INC.");
    makeBytes.append('\0');
    if (makeBytes.size() & 1) makeBytes.append('\0');

    // ModelPixelScaleTag (DOUBLE x 3 = 24 字节)
    QByteArray modelPixelScale(24, '\0');
    {
        double v[3] = { meta.scaleX, meta.scaleY, meta.scaleZ };
        memcpy(modelPixelScale.data(), v, 24);
    }
    // ModelTiepointTag (DOUBLE x 6 = 48 字节)：[0, 0, 0, offsetX, offsetY, offsetZ]
    QByteArray modelTiepoint(48, '\0');
    {
        double v[6] = { 0.0, 0.0, 0.0, meta.offsetX, meta.offsetY, meta.offsetZ };
        memcpy(modelTiepoint.data(), v, 48);
    }
    // XResolution / YResolution：RATIONAL (2 LONG = 8 字节)
    // = pixels per cm = 10 / scaleX(mm)。用 denom=1000000 提高精度
    auto buildRational = [](double pixelsPerCm) -> QByteArray {
        QByteArray a(8, '\0');
        const quint32 denom = 1000000u;
        double num = pixelsPerCm * static_cast<double>(denom);
        if (!std::isfinite(num) || num < 0) num = 0;
        if (num > 4.2e9) num = 4.2e9;
        const quint32 numer = static_cast<quint32>(std::lround(num));
        memcpy(a.data() + 0, &numer, 4);
        memcpy(a.data() + 4, &denom, 4);
        return a;
    };
    const double pxPerCmX = (meta.scaleX > 0) ? 10.0 / meta.scaleX : 0.0;
    const double pxPerCmY = (meta.scaleY > 0) ? 10.0 / meta.scaleY : 0.0;
    QByteArray xResBytes = buildRational(pxPerCmX);
    QByteArray yResBytes = buildRational(pxPerCmY);

    // ── 4) 计算外置数据偏移：头(8) + strip(stripBytes) 之后 ────────────────
    quint32 cursor = 8 + static_cast<quint32>(stripBytes);
    const quint32 descOff = cursor;            cursor += descBytes.size();
    const quint32 makeOff = cursor;            cursor += makeBytes.size();
    const quint32 mpsOff  = cursor;            cursor += modelPixelScale.size();
    const quint32 mtpOff  = cursor;            cursor += modelTiepoint.size();
    const quint32 xresOff = cursor;            cursor += xResBytes.size();
    const quint32 yresOff = cursor;            cursor += yResBytes.size();
    const quint32 ifdOff  = cursor;            // IFD 紧跟外置数据

    // ── 5) 打开文件，开始写 ──────────────────────────────────────────────
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (outError) *outError = QString("无法打开输出文件: %1").arg(path);
        return false;
    }

    auto wU16 = [&](quint16 v) {
        quint8 b[2]; qToLittleEndian(v, b); f.write(reinterpret_cast<char*>(b), 2);
    };
    auto wU32 = [&](quint32 v) {
        quint8 b[4]; qToLittleEndian(v, b); f.write(reinterpret_cast<char*>(b), 4);
    };

    // 头
    f.write("II", 2);
    wU16(42);
    wU32(ifdOff);

    // strip 数据
    f.write(stripData);
    // 外置数据
    f.write(descBytes);
    f.write(makeBytes);
    f.write(modelPixelScale);
    f.write(modelTiepoint);
    f.write(xResBytes);
    f.write(yResBytes);

    // IFD：19 个 tag（按 tag 升序排列）
    constexpr quint16 N_ENTRIES = 19;
    wU16(N_ENTRIES);

    auto entryShortInline = [&](quint16 tag, quint16 val) {
        wU16(tag); wU16(3 /*SHORT*/); wU32(1);
        wU16(val); wU16(0);
    };
    auto entryLongInline = [&](quint16 tag, quint32 val) {
        wU16(tag); wU16(4 /*LONG*/); wU32(1); wU32(val);
    };
    auto entryAsciiOffset = [&](quint16 tag, quint32 cnt, quint32 off) {
        wU16(tag); wU16(2 /*ASCII*/); wU32(cnt); wU32(off);
    };
    auto entryRationalOffset = [&](quint16 tag, quint32 off) {
        wU16(tag); wU16(5 /*RATIONAL*/); wU32(1); wU32(off);
    };
    auto entryDoubleOffset = [&](quint16 tag, quint32 cnt, quint32 off) {
        wU16(tag); wU16(12 /*DOUBLE*/); wU32(cnt); wU32(off);
    };

    entryLongInline    (256, static_cast<quint32>(W));          // ImageWidth
    entryLongInline    (257, static_cast<quint32>(H));          // ImageLength
    entryShortInline   (258, static_cast<quint16>(bps));        // BitsPerSample
    entryShortInline   (259, 5);                                // Compression = LZW
    entryShortInline   (262, 1);                                // PhotometricInterpretation
    entryAsciiOffset   (270,                                    // ImageDescription
                        static_cast<quint32>(desc.toUtf8().size() + 1), descOff);
    entryAsciiOffset   (271,                                    // Make
                        static_cast<quint32>(QByteArray("LMI TECHNOLOGIES INC.").size() + 1), makeOff);
    entryLongInline    (273, 8);                                // StripOffsets = 8
    entryShortInline   (277, 1);                                // SamplesPerPixel
    entryLongInline    (278, static_cast<quint32>(H));          // RowsPerStrip = H
    entryLongInline    (279, static_cast<quint32>(stripBytes)); // StripByteCounts
    entryRationalOffset(282, xresOff);                          // XResolution
    entryRationalOffset(283, yresOff);                          // YResolution
    entryShortInline   (284, 1);                                // PlanarConfiguration
    entryShortInline   (296, 3);                                // ResolutionUnit = cm
    entryShortInline   (317, static_cast<quint16>(predictor));  // Predictor
    entryShortInline   (339, static_cast<quint16>(sampleFormat));// SampleFormat
    entryDoubleOffset  (33550, 3, mpsOff);                      // ModelPixelScale
    entryDoubleOffset  (33922, 6, mtpOff);                      // ModelTiepoint

    wU32(0); // 下一 IFD = 0

    return f.error() == QFileDevice::NoError;
}

} // anonymous namespace

bool writeTiffUInt16(const QString& path, int W, int H, const qint16* rawInt16,
                     const ExportTiffMeta& meta, QString* outError)
{
    if (W <= 0 || H <= 0 || rawInt16 == nullptr) {
        if (outError) *outError = "writeTiffUInt16: 入参无效";
        return false;
    }
    const qint64 N = static_cast<qint64>(W) * H;
    // UInt16RawPlus32768：pixel = raw_int16 + 32768（无损保留 raw_int16；-32768→0=无效）
    std::vector<quint16> out(static_cast<size_t>(N));
    for (qint64 i = 0; i < N; ++i)
        out[i] = static_cast<quint16>(static_cast<int>(rawInt16[i]) + 32768);
    // 写入的 ZOffset_mm 需与 "pixel 直接使用" 的读取公式配套（Z = pixel*scaleZ + ZOffset_mm，
    // 不再 -32768 重新解释），而 meta.offsetZ 是按 rawInt16（已居中）语境传入的
    // （Z = rawInt16*scaleZ + meta.offsetZ），故此处补偿 -32768*scaleZ 再写入
    ExportTiffMeta uMeta = meta;
    uMeta.offsetZ = meta.offsetZ - 32768.0 * meta.scaleZ;
    return writeTiffLzwLE(path, W, H, 16, 1, 2,
                          out.data(), N * 2, uMeta,
                          "UInt16RawPlus32768", false, 0.0, outError);
}

bool writeTiffInt16(const QString& path, int W, int H, const qint16* rawInt16,
                    const ExportTiffMeta& meta, QString* outError)
{
    if (W <= 0 || H <= 0 || rawInt16 == nullptr) {
        if (outError) *outError = "writeTiffInt16: 入参无效";
        return false;
    }
    const qint64 N = static_cast<qint64>(W) * H;
    // Int16Raw：pixel = raw_int16 直接（-32768=无效）
    return writeTiffLzwLE(path, W, H, 16, 2, 2,
                          rawInt16, N * 2, meta,
                          "Int16Raw", false, 0.0, outError);
}

bool writeTiffFloat32(const QString& path, int W, int H, const qint16* rawInt16,
                      const ExportTiffMeta& meta,
                      float invalidFill, QString* outError)
{
    if (W <= 0 || H <= 0 || rawInt16 == nullptr) {
        if (outError) *outError = "writeTiffFloat32: 入参无效";
        return false;
    }
    const qint64 N = static_cast<qint64>(W) * H;
    // Float32ActualHeight：pixel = raw * scaleZ + offsetZ（mm，世界 Z）；-32768→invalidFill
    std::vector<float> out(static_cast<size_t>(N));
    const double sZ = meta.scaleZ;
    const double oZ = meta.offsetZ;
    for (qint64 i = 0; i < N; ++i) {
        const qint16 r = rawInt16[i];
        out[i] = (r == std::numeric_limits<qint16>::min())
            ? invalidFill
            : static_cast<float>(r * sZ + oZ);
    }
    // 写到 tag 的 ModelScaleZ/TiepointZ：Float32ActualHeight 像素已是世界 Z，
    // 故重写为 1.0 / 0.0（与 Tools3D.cs 一致），ImageDescription 内仍记录原始 mm 标定
    ExportTiffMeta floatMeta = meta;
    floatMeta.scaleZ = 1.0;
    floatMeta.offsetZ = 0.0;
    // Predictor=1 (无)，确保 ImageJ 等仅支持 horizontal predictor 的工具可读
    return writeTiffLzwLE(path, W, H, 32, 3, 1,
                          out.data(), N * 4, floatMeta,
                          "Float32ActualHeight",
                          true, static_cast<double>(invalidFill), outError);
}

bool writeTiffFloat32FromFloat(const QString& path, int W, int H, const float* zMm,
                                const ExportTiffMeta& meta,
                                float invalidFill, QString* outError)
{
    if (W <= 0 || H <= 0 || zMm == nullptr) {
        if (outError) *outError = "writeTiffFloat32FromFloat: 入参无效";
        return false;
    }
    const qint64 N = static_cast<qint64>(W) * H;
    std::vector<float> out(static_cast<size_t>(N));
    for (qint64 i = 0; i < N; ++i) {
        const float v = zMm[i];
        out[i] = std::isnan(v) ? invalidFill : v;
    }
    ExportTiffMeta floatMeta = meta;
    floatMeta.scaleZ = 1.0;
    floatMeta.offsetZ = 0.0;
    return writeTiffLzwLE(path, W, H, 32, 3, 1,
                          out.data(), N * 4, floatMeta,
                          "Float32ActualHeight",
                          true, static_cast<double>(invalidFill), outError);
}

bool writeBmpGray(const QString& path, int W, int H, const quint8* data,
                  double scaleXmm, double scaleYmm,
                  QString* outError)
{
    if (W <= 0 || H <= 0 || data == nullptr) {
        if (outError) *outError = "writeBmpGray: 入参无效";
        return false;
    }
    QImage img(W, H, QImage::Format_Grayscale8);
    if (img.isNull()) {
        if (outError) *outError = "无法分配 BMP 内存";
        return false;
    }
    for (int y = 0; y < H; ++y) {
        memcpy(img.scanLine(y), data + static_cast<size_t>(y) * W,
               static_cast<size_t>(W));
    }
    // 让 Qt 在写 BMP 头时填入 biX/YPelsPerMeter；备份在 writeBmpResolutionInPlace 中确保
    auto mmToPpm = [](double mm) -> int {
        if (!(mm > 0)) return 0;
        const double ppm = 1000.0 / mm;
        if (ppm > 2.1e9) return 2147483647;
        return static_cast<int>(std::lround(ppm));
    };
    const int ppmX = mmToPpm(scaleXmm);
    const int ppmY = mmToPpm(scaleYmm);
    if (ppmX > 0) img.setDotsPerMeterX(ppmX);
    if (ppmY > 0) img.setDotsPerMeterY(ppmY);

    if (!img.save(path, "BMP")) {
        if (outError) *outError = "QImage::save BMP 失败";
        return false;
    }
    // 兜底：Qt 不同版本对 BMP 头部 DPI 字段写入有差异，统一就地补写
    writeBmpResolutionInPlace(path, scaleXmm, scaleYmm);
    return true;
}

bool writeBmpResolutionInPlace(const QString& path,
                                double scaleXmm, double scaleYmm)
{
    if (!(scaleXmm > 0) || !(scaleYmm > 0)) return false;

    auto mmToPpm = [](double mm) -> qint32 {
        const double ppm = 1000.0 / mm;
        if (ppm > 2.1e9) return 2147483647;
        if (ppm < 0)     return 0;
        return static_cast<qint32>(std::lround(ppm));
    };
    const qint32 ppmX = mmToPpm(scaleXmm);
    const qint32 ppmY = mmToPpm(scaleYmm);

    QFile f(path);
    if (!f.open(QIODevice::ReadWrite)) return false;
    if (f.size() < 54) return false;   // BMP 文件最小头部 54 字节

    char sig[2];
    if (f.read(sig, 2) != 2) return false;
    if (sig[0] != 'B' || sig[1] != 'M') return false;

    // 检查 DIB 头部至少 40 字节（BITMAPINFOHEADER 及其变体）
    if (!f.seek(14)) return false;
    quint8 dibSizeBytes[4];
    if (f.read(reinterpret_cast<char*>(dibSizeBytes), 4) != 4) return false;
    const quint32 dibSize = qFromLittleEndian<quint32>(dibSizeBytes);
    if (dibSize < 40) return false;

    // biXPelsPerMeter @ 38, biYPelsPerMeter @ 42（DIB 内偏移 24 / 28；总偏移 14+24=38）
    auto writeI32 = [&](qint64 pos, qint32 v) {
        quint8 b[4];
        qToLittleEndian(static_cast<quint32>(v), b);
        f.seek(pos);
        f.write(reinterpret_cast<char*>(b), 4);
    };
    writeI32(38, ppmX);
    writeI32(42, ppmY);
    return true;
}

bool readSourceForExport(const QString& path, ExportSourceData& out, QString* outError)
{
    out = ExportSourceData{};
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        if (outError) *outError = QString("源文件不存在: %1").arg(path);
        return false;
    }

    if (isSrfPath(path))
    {
        SrfRawData raw;
        if (!readSrfRaw(path, raw, outError)) return false;
        out.W = raw.W;
        out.H = raw.L;
        out.scaleX  = raw.scaleX;
        out.scaleY  = raw.scaleY;
        out.scaleZ  = raw.scaleZ;
        out.offsetX = raw.offsetX;
        out.offsetY = raw.offsetY;
        out.offsetZ = raw.offsetZ;
        out.sourceIsFloat = false;
        out.rawInt16 = std::move(raw.points);  // 直接挪走，零拷贝
        if (!raw.intensity.empty())
            out.intensity = std::move(raw.intensity);
        out.valid = true;
        return true;
    }

    if (isSurPath(path))
    {
        SurRawData raw;
        if (!readSurRaw(path, raw, outError)) return false;
        out.W = raw.W;
        out.H = raw.H;
        out.scaleX  = raw.scaleX;
        out.scaleY  = raw.scaleY;
        out.scaleZ  = raw.scaleZ;
        out.offsetX = raw.offsetX;
        out.offsetY = raw.offsetY;
        out.offsetZ = raw.offsetZ;
        out.sourceIsFloat = true;
        out.zMm.resize(raw.points.size());
        for (size_t i = 0; i < raw.points.size(); ++i) {
            const qint32 z = raw.points[i];
            const bool invalid = raw.hasSpecialPoints && (z == raw.zmin - 2);
            out.zMm[i] = invalid
                ? std::numeric_limits<float>::quiet_NaN()
                : static_cast<float>((z - raw.zmin) * raw.scaleZ + raw.offsetZ);
        }
        if (!raw.intensity.empty())
            out.intensity = std::move(raw.intensity);
        // raw.rgb（真彩色，13/16 类型）当前导出管线不支持写出，仅高度参与导出
        out.valid = true;
        return true;
    }

    if (isPlyPath(path) || isPcdPath(path))
    {
        PlyPcdPoints pcPoints;
        QString pcErr;
        if (!readPlyPcdPoints(path, pcPoints, &pcErr)) {
            if (outError) *outError = pcErr;
            return false;
        }
        UniformGridResult grid;
        if (!tryBuildUniformGrid(pcPoints, grid)) {
            if (outError) *outError = QStringLiteral("点云不是规则网格（间距不均或存在重复格点），无法导出为 TIFF（TIFF 要求均匀间距）");
            return false;
        }
        out.W = grid.W;
        out.H = grid.H;
        out.scaleX  = grid.resX;
        out.scaleY  = grid.resY;
        out.scaleZ  = 1.0;
        out.offsetX = grid.offX;
        out.offsetY = grid.offY;
        out.offsetZ = 0.0;
        out.sourceIsFloat = true;
        out.zMm = std::move(grid.floatData);
        if (!grid.embeddedBrightness.empty())
            out.intensity = std::move(grid.embeddedBrightness);
        out.valid = true;
        return true;
    }

    // ── TIFF ───────────────────────────────────────────────────────────────
    TiffInfo info;
    if (!buildTiffInfo(path, info) || !info.valid) {
        if (outError) *outError = "TIFF 头部解析失败";
        return false;
    }
    out.W = info.width;
    out.H = info.height;
    out.scaleX  = info.hasEmbeddedResX ? info.embeddedResX : 1.0;
    out.scaleY  = info.hasEmbeddedResY ? info.embeddedResY : 1.0;
    out.scaleZ  = info.hasEmbeddedResZ ? info.embeddedResZ : 1.0;
    out.offsetX = info.hasEmbeddedOffX ? info.embeddedOffX : 0.0;
    out.offsetY = info.hasEmbeddedOffY ? info.embeddedOffY : 0.0;
    out.offsetZ = info.hasEmbeddedOffZ ? info.embeddedOffZ : 0.0;
    const size_t N = static_cast<size_t>(out.W) * out.H;

    if (info.is32Float) {
        // 32-bit float TIFF：保留 float 表示，避免转 int16 时精度损失
        std::vector<float> data = tryReadTiff32Float(path, info);
        if (data.empty() || data.size() != N) {
            if (outError) *outError = "32-bit float TIFF 解码失败";
            return false;
        }
        out.zMm = std::move(data);
        out.sourceIsFloat = true;
    }
    else if (info.is2Channel) {
        Tiff2ChData ch = tryReadTiff2Channel(path, info);
        if (ch.W == 0) {
            if (outError) *outError = "2 通道 TIFF 解码失败";
            return false;
        }
        // 2 通道 TIFF：高度通道是 float（已乘 scaleZ），亮度通道 uint8
        out.zMm        = std::move(ch.height);
        out.intensity  = std::move(ch.brightness);
        out.sourceIsFloat = true;
    }
    else if (info.is96BitXYZ) {
        if (outError) *outError = "96-bit XYZ TIFF 暂不支持导出";
        return false;
    }
    else {
        // 16-bit (unsigned or signed) → 抽出 raw int16
        QImage img;
        if (info.compression == 5 && info.samplesPerPixel == 1
            && info.bitsPerSample.value(0, 0) == 16 && info.planarConfig == 1)
        {
            std::vector<quint16> u16 = tryReadTiff16Lzw(path, info);
            if (!u16.empty() && static_cast<int>(u16.size()) == info.width * info.height) {
                img = QImage(info.width, info.height, QImage::Format_Grayscale16);
                for (int y = 0; y < info.height; ++y)
                    memcpy(img.scanLine(y),
                           u16.data() + static_cast<size_t>(y) * info.width,
                           static_cast<size_t>(info.width) * 2);
            }
        }
        if (img.isNull()) {
            QImageReader r(path);
            r.setAutoTransform(false);
            QImage raw = r.read();
            if (raw.isNull()) {
                if (outError) *outError = "QImageReader 无法读取 TIFF: " + r.errorString();
                return false;
            }
            img = raw.convertToFormat(QImage::Format_Grayscale16);
            if (img.isNull()) {
                if (outError) *outError = "TIFF → Grayscale16 转换失败";
                return false;
            }
        }
        out.rawInt16.resize(N);
        out.sourceIsFloat = false;
        const bool isSigned = info.is16BitSigned;
        const bool isPlus32768 = info.embeddedUInt16Plus32768;
        // rawInt16 在两个非有符号分支里都是 pixel-32768（居中表示），配合的 offsetZ
        // 需要调用方按 [[project_z_offset_uint16_fix]] 同样的语境额外补偿 +32768*scaleZ
        // （Storage 标记不足以区分新旧生成器，因此与有符号分支同等对待，见 sourceIsUnsignedUint16 注释）
        out.sourceIsUnsignedUint16 = !isSigned;
        for (int y = 0; y < info.height; ++y) {
            const quint16* row = reinterpret_cast<const quint16*>(img.constScanLine(y));
            for (int x = 0; x < info.width; ++x) {
                const quint16 u = row[x];
                qint16 r;
                if (isSigned) {
                    // 直接当 int16 解释
                    r = static_cast<qint16>(u);
                } else if (isPlus32768) {
                    // UInt16RawPlus32768：raw = pixel - 32768；pixel==0 视为无效
                    r = (u == 0)
                        ? std::numeric_limits<qint16>::min()
                        : static_cast<qint16>(static_cast<int>(u) - 32768);
                } else {
                    // 普通 uint16：直接减 32768 转 int16（pixel==0 也视为无效以兼容 LMI 旧文件）
                    if (u == 0) r = std::numeric_limits<qint16>::min();
                    else        r = static_cast<qint16>(
                        std::max(-32767, std::min(32767, static_cast<int>(u) - 32768)));
                }
                out.rawInt16[static_cast<size_t>(y) * info.width + x] = r;
            }
        }
    }
    out.valid = true;
    return true;
}

} // namespace TiffBmpLoader
