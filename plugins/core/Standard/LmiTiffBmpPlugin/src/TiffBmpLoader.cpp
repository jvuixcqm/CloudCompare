#include "TiffBmpLoader.h"

#include <ccPointCloud.h>
#include <ccMesh.h>

#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QFileInfo>
#include <QMap>
#include <QtEndian>

#include <array>
#include <algorithm>
#include <cmath>
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

static QVector<quint32> toQVector(const std::vector<quint32>& values)
{
    QVector<quint32> out;
    out.reserve(static_cast<int>(values.size()));
    for (quint32 v : values)
        out.push_back(v);
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

    outInfo.is2Channel = (outInfo.samplesPerPixel == 2
        && secondBits == 32
        && secondFmt == 3
        && outInfo.compression == 1);
    outInfo.is32Float = (!outInfo.is2Channel
        && firstBits == 32
        && firstFmt == 3
        && outInfo.compression == 1);
    outInfo.is16BitSigned = (!outInfo.is2Channel
        && !outInfo.is32Float
        && firstFmt == 2);

    outInfo.bitDepth = outInfo.is2Channel ? 128
                     : outInfo.is32Float ? 32
                     : outInfo.is16BitSigned ? 17
                     : 16;
    outInfo.valid = (outInfo.width > 0 && outInfo.height > 0);
    return outInfo.valid;
}

static std::vector<float> tryReadTiff32Float(
    const QString& path, const TiffInfo& info)
{
    if (!info.valid || !info.is32Float || info.compression != 1)
        return {};

    const int W = info.width;
    const int H = info.height;
    if (W <= 0 || H <= 0
        || info.stripOffsets.isEmpty()
        || info.stripOffsets.size() != info.stripByteCounts.size())
    {
        return {};
    }

    const bool le = info.littleEndian;
    std::vector<float> data(static_cast<size_t>(W * H), 0.f);
    int row = 0;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};

    for (int s = 0; s < info.stripOffsets.size() && row < H; ++s) {
        f.seek(info.stripOffsets[s]);
        const QByteArray raw = f.read(info.stripByteCounts[s]);
        const int nFloats = raw.size() / 4;
        const int nRows   = nFloats / W;
        for (int r = 0; r < nRows && (row + r) < H; ++r) {
            for (int c = 0; c < W; ++c) {
                const int idx = r * W + c;
                if (idx >= nFloats) break;
                quint32 bits;
                memcpy(&bits, raw.constData() + idx * 4, 4);
                if (!le) bits = qbswap(bits);
                float v;
                memcpy(&v, &bits, 4);
                data[static_cast<size_t>((row + r) * W + c)] = v;
            }
        }
        row += nRows;
    }
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
    unsigned meshUID)
{
    Q_UNUSED(minIslandDist);

    TiffInfo info;
    if (preparedInfo && preparedInfo->valid)
        info = *preparedInfo;
    else
        buildTiffInfo(tiffPath, info);

    // ── 优先尝试 2 通道"128位"TIFF，否则回退到单通道 32-bit / 16-bit ──────
    int W = info.width;
    int H = info.height;
    const bool wantsBmp = (mode == DisplayMode::Brightness || mode == DisplayMode::Fusion);
    std::vector<quint8> embeddedBrightness; // 来自 2 通道 TIFF 的亮度通道

    Tiff2ChData twoChData = (info.valid && info.is2Channel)
        ? tryReadTiff2Channel(tiffPath, info)
        : Tiff2ChData{};
    const bool is2Channel = (twoChData.W > 0 && twoChData.H > 0);
    if (is2Channel) {
        W = twoChData.W;
        H = twoChData.H;
    }

    std::vector<float> floatData;
    if (is2Channel) {
        floatData          = std::move(twoChData.height);
        embeddedBrightness = std::move(twoChData.brightness);
    } else if (info.valid && info.is32Float) {
        floatData = tryReadTiff32Float(tiffPath, info);
    }
    const bool is32bit = !floatData.empty();
    bool is16BitSigned = info.valid && info.is16BitSigned;

    const double actualResZ = is32bit ? 1.0 : resZ;

    // ── 读取 16-bit TIFF（非 32-bit 路径）──────────────────────────────
    QImage tiffImg;
    if (!is32bit) {
        QImageReader tiffReader(tiffPath);
        tiffReader.setAutoTransform(false);
        QImage raw = tiffReader.read();
        if (raw.isNull()) {
            if (outError) *outError =
                QString("无法读取 TIFF 文件（可能是不支持的压缩方式或格式）: %1")
                .arg(tiffReader.errorString());
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
        *outBitDepth = is2Channel ? 128 : (is32bit ? 32 : (is16BitSigned ? 17 : 16));

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

    // 若无外部亮度图且是 2 通道 TIFF，使用内嵌亮度通道替代外部 BMP
    if (!hasBmp && wantsBmp && is2Channel && !embeddedBrightness.empty()) {
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
                    static_cast<PointCoordinateType>(-(x * resX + offX)),
                    static_cast<PointCoordinateType>(y * resY + offY),
                    static_cast<PointCoordinateType>(v * actualResZ + offZ)));

                float norm = (v - cMin) / cSpan;
                norm = std::max(0.f, std::min(1.f, norm));
                const quint8 bv = bRow ? bRow[x]
                    : (synthBmp ? static_cast<quint8>(norm * 255.f) : 128u);
                cloud->addColor(computeColor(bv,
                    std::min(static_cast<int>(norm * 1536.f), 1536)));
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

} // namespace TiffBmpLoader
