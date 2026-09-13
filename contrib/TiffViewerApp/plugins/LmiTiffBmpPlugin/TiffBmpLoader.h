#pragma once

#include <QString>
#include <QVector>
#include <vector>
#include <ccHObject.h>
#include <ccPointCloud.h>
#include <ccMesh.h>

// Suppress bounding-box display even when selected in CloudCompare's DB tree.
class ccPointCloudNoBB : public ccPointCloud {
public:
    using ccPointCloud::ccPointCloud;
    void drawBB(CC_DRAW_CONTEXT&, const ccColor::Rgb&) override {}
};
class ccMeshNoBB : public ccMesh {
public:
    explicit ccMeshNoBB(ccGenericPointCloud* vertices, unsigned uniqueID = 0)
        : ccMesh(vertices, uniqueID) {}
    void drawBB(CC_DRAW_CONTEXT&, const ccColor::Rgb&) override {}
};

//! 显示模式
enum class DisplayMode {
    Height,      // 16/32-bit Z值 -> 蓝-青-绿-黄-红-紫-白 彩虹高度色
    Brightness,  // 亮度图像素 -> 灰度
    Fusion,      // 高度色 * (1-alpha) + 亮度 * 0.5
    HeightGray   // Z 高度线性映射到 0-255 灰阶（受颜色范围滑块控制）
};

namespace TiffBmpLoader
{
    //! 轻量 TIFF 元数据缓存，可在 UI 线程预解析后复用于后台加载
    struct TiffInfo
    {
        bool valid           = false;
        bool littleEndian    = true;
        int  width           = 0;
        int  height          = 0;
        int  bitDepth        = 16;   //!< 16 / 17(16-bit signed) / 32 / 96(3-ch XYZ float) / 128(2-channel)
        bool is32Float       = false;
        bool is16BitSigned   = false;
        bool is2Channel      = false;
        bool is96BitXYZ      = false;  //!< 3 通道 32-bit float（X/Y/Z 三个通道，已是世界坐标 mm）
        int  samplesPerPixel = 1;
        int  compression     = 1;
        int  planarConfig    = 1;
        int  predictor       = 1;   //!< Tag 317: 1=无, 2=水平差分, 3=浮点差分
        int  rowsPerStrip    = 0;   //!< Tag 278: 每个 strip 包含的行数（0=未设定，按 H 处理）

        QVector<quint32> bitsPerSample;
        QVector<quint32> sampleFormats;
        QVector<quint32> stripOffsets;
        QVector<quint32> stripByteCounts;

        //! ── 嵌入式 3D 标定元数据（来自 ImageDescription / ModelPixelScale / XResolution）──
        //! 优先级：ImageDescription(LMI_Gocator) > ModelPixelScale/Tiepoint > XResolution/YResolution
        bool   hasEmbeddedResX  = false;   //!< 检测到 X 分辨率（mm/pixel）
        bool   hasEmbeddedResY  = false;
        bool   hasEmbeddedResZ  = false;   //!< 仅 ImageDescription 与 ModelPixelScale 提供
        bool   hasEmbeddedOffX  = false;   //!< 检测到 X 偏移（mm）
        bool   hasEmbeddedOffY  = false;
        bool   hasEmbeddedOffZ  = false;
        bool   embeddedUInt16Plus32768 = false; //!< Storage=UInt16RawPlus32768（需 reinterpret 为有符号）
        double embeddedResX     = 0.0;     //!< mm/pixel
        double embeddedResY     = 0.0;
        double embeddedResZ     = 0.0;
        double embeddedOffX     = 0.0;     //!< mm
        double embeddedOffY     = 0.0;
        double embeddedOffZ     = 0.0;
        QString embeddedMetaSource;        //!< 来源（"ImageDescription" / "ModelPixelScale" / "XResolution"）
    };

    //! 加载 TIFF 高程图 + 可选亮度图，生成彩色点云或三角网格
    //!
    //! @param tiffPath       TIFF 路径（16-bit 整数或 32-bit float）
    //! @param bmpPath        8-bit 灰度亮度图路径（可为空）
    //! @param resX           X 方向物理分辨率 (mm/pixel)，X 轴镜像输出
    //! @param resY           Y 方向物理分辨率 (mm/pixel)
    //! @param resZ           Z 高度分辨率 (mm/count)，32-bit float 时自动忽略
    //! @param mode           显示模式
    //! @param fusionAlpha    融合系数 [0,1]
    //! @param buildMesh      true=利用有序网格构建三角面片
    //! @param colorRangeMin  颜色映射范围下限 [0,1]
    //! @param colorRangeMax  颜色映射范围上限 [0,1]
    //! @param zInvalidValue  无效像素原始值（该值的点被跳过）；NaN 自动跳过
    //! @param useSyntheticBmp 无 BMP 时自动用 TIFF 归一化值生成合成亮度
    //! @param outBitDepth    输出：检测到的位深 16、17（16位有符号）、32 或 128（双通道），nullptr 则忽略
    //! @param removeIslands   true=移除小岛噪声（孤立小连通分量）
    //! @param minIslandPixels 小岛阈值：像素数低于此值的连通分量视为候选小岛
    //! @param minIslandDist   离岛距离（像素）：候选小岛距其他有效像素超过此距离才真正移除；0=不检查距离
    //! @param maxZGap         Z 方向连通阈值 (mm)：相邻像素 Z 差超过此值视为不连通；0=不限制
    //! @param maxEdgeLength   网格最大边长 (mm)，超过此值的三角形被跳过；0=不限制
    //! @param computeNormals  网格模式下是否计算平滑法线（界面层默认随生成网格自动开启）
    //! @param offX            X 方向平移偏移 (mm)：最终 X = 列 × resX + offX
    //! @param offY            Y 方向平移偏移 (mm)：最终 Y = 行 × resY + offY
    //! @param offZ            Z 方向平移偏移 (mm)：最终 Z = raw × resZ + offZ
    //! @param reinterpretAsSignedZ  true=将 uint16 Z 值重新解释为有符号（减去 32768），
    //!                              再乘以 resZ + offZ；仅对 16-bit 无符号 TIFF 生效
    //! @param preparedInfo    预解析的 TIFF 元数据；传入可避免重复 IFD/位深解析
    //! @param outError        输出：失败原因描述，nullptr 则忽略
    //! @param yStride         Y 方向等步长降采样：仅保留行号为 yStride 整数倍的行
    //!                        （y=0,yStride,2×yStride,...），保证输出行间距均匀（stride×resY），
    //!                        网格模式下相邻保留行可正常连接；1=全采，2=隔行取1...
    //! @param cloudUID        预分配的 ccPointCloud 唯一 ID（在 UI 线程预分配后传入，
    //!                        可避免后台线程调用全局非线程安全 GetNextUniqueID()）；
    //!                        默认 InvalidUniqueID = 自动分配
    //! @param meshUID         预分配的 ccMesh 唯一 ID（同上）；默认 InvalidUniqueID
    //! @param outFallbackToUnorganized 输出：PLY/PCD 输入且无法还原为规则网格时置 true，
    //!                        表示已降级为普通无序点云（不建网格/不做小岛过滤）；nullptr 则忽略
    //! @return ccPointCloud* 或 ccMesh*（调用方负责释放），失败返回 nullptr
    ccHObject* load(
        const QString& tiffPath,
        const QString& bmpPath,
        double  resX,
        double  resY,
        double  resZ,
        DisplayMode mode,
        float   fusionAlpha     = 0.5f,
        bool    buildMesh       = false,
        float   colorRangeMin   = 0.0f,
        float   colorRangeMax   = 1.0f,
        double  zInvalidValue   = 0.0,
        bool    useSyntheticBmp = true,
        int*    outBitDepth     = nullptr,
        bool    removeIslands   = false,
        int     minIslandPixels = 100,
        int     minIslandDist   = 10,
        double  maxZGap         = 0.0,
        double  maxEdgeLength   = 0.0,
        bool    computeNormals  = true,
        double  offX            = 0.0,
        double  offY            = 0.0,
        double  offZ            = 0.0,
        bool    reinterpretAsSignedZ = false,
        const TiffInfo* preparedInfo = nullptr,
        QString* outError       = nullptr,
        int     yStride         = 1,
        unsigned cloudUID       = ccUniqueIDGenerator::InvalidUniqueID,
        unsigned meshUID        = ccUniqueIDGenerator::InvalidUniqueID,
        bool*   outFallbackToUnorganized = nullptr
    );

    //! 轻量位深检测（仅读 IFD，不读像素）
    //! @return 128=双通道, 32=32-bit float, 17=16-bit有符号, 16=16-bit无符号
    int detectBitDepth(const QString& path, TiffInfo* outInfo = nullptr);

    // ────────────────────────────────────────────────────────────────────
    //! SRF 格式（GoPxL / LMI3D 裸格式）：2.5D 均匀网格高度图 + 可选亮度
    //! 文件结构（little-endian）：
    //!   uint32 version (1 或 2) | uint32 width | uint32 length
    //!   float64 scale_x/y/z      | float64 offset_x/y/z
    //!   int16  points[length][width]   行优先；无效高度固定为 -32768
    //!   仅 v2：uint32 has_intensity | uint8 intensity[length][width]（若 has_intensity==1）
    //! 世界坐标：world_x = col * scale_x + offset_x（Y/Z 同理）
    struct SrfMeta {
        bool   valid        = false;
        quint32 version     = 0;   //!< 1 或 2
        int    width        = 0;
        int    length       = 0;
        double scaleX       = 1.0;
        double scaleY       = 1.0;
        double scaleZ       = 1.0;
        double offsetX      = 0.0;
        double offsetY      = 0.0;
        double offsetZ      = 0.0;
        bool   hasIntensity = false; //!< 仅 v2 有效；v1 永远为 false
    };

    //! 通过后缀判断是否为 SRF 文件（大小写不敏感）
    inline bool isSrfPath(const QString& path) {
        return path.endsWith(QStringLiteral(".srf"), Qt::CaseInsensitive);
    }

    //! 读取 SRF 头部（版本、尺寸、scale/offset、是否有亮度）；不读取像素数据
    //! @return true=成功
    bool parseSrfMeta(const QString& path, SrfMeta& out, QString* outError = nullptr);

    // ────────────────────────────────────────────────────────────────────
    //! SUR 格式（Digital Surf / MountainsMap）：2.5D 均匀网格高度图
    //! 按公开字节布局直接解析（未调用第三方 surfapi.dll，字段表参考开源实现
    //! rsciio.digitalsurf，MIT 协议，仅借用其记录的格式表，未使用其代码）。
    //! 支持范围：
    //!   - 支持未压缩签名 "DIGITAL SURF" 与压缩签名 "DSCOMPRESSED"（zlib，流目录
    //!     格式参考 rsciio.digitalsurf 的 _unpack_data，用 Qt qUncompress 解压）
    //!   - 支持 Number_of_Objects>1 的多对象/序列文件（仅取第 1 帧，其余帧忽略）
    //!   - 支持 Object_Type==2（_SURFACE）、11（_INTENSITYSURFACE，未获得真实样本，
    //!     按 13/16 规律推断实现）、13（_RGBSURFACE，真彩色）、16（_RGBINTENSITYSURFACE，
    //!     真彩色，忽略其自带的额外亮度通道），以及多对象文件的 _SURFACESERIE(5) 首帧
    //!   - 仅非多层/光谱数据（W_Size<=1）
    //! 世界坐标（mm，已按 *_Step_Unit 换算）：
    //!   x = col * scaleX + offsetX；y = row * scaleY + offsetY
    //!   z = (raw - Zmin) * scaleZ + offsetZ；Special_Points==1 时 raw==Zmin-2 为无效点
    struct SurMeta {
        bool   valid   = false;
        int    width   = 0;   // Number_of_Points
        int    height  = 0;   // Number_of_Lines
        double scaleX  = 1.0; // mm/pixel
        double scaleY  = 1.0;
        double scaleZ  = 1.0; // mm/count（已含 Z_Spacing/Z_Unit_Ratio 与单位换算）
        double offsetX = 0.0; // mm
        double offsetY = 0.0;
        double offsetZ = 0.0;
    };

    //! 通过后缀判断是否为 SUR 文件（大小写不敏感）
    inline bool isSurPath(const QString& path) {
        return path.endsWith(QStringLiteral(".sur"), Qt::CaseInsensitive);
    }

    //! 读取 SUR 头部（尺寸、scale/offset）；不读取像素数据
    //! @return true=成功（单对象、规则表面类型，压缩/未压缩均可；否则 outError 给出具体原因）
    bool parseSurMeta(const QString& path, SurMeta& out, QString* outError = nullptr);

    // ────────────────────────────────────────────────────────────────────
    //! PLY / PCD 点云直接导入：优先尝试还原为规则网格（走与 SRF 相同的
    //! floatData[W*H] 烘焙管线），失败时自动降级为普通无序点云（不报错）。
    //!   - PLY：支持 format ascii / binary_little_endian / binary_big_endian 1.0；
    //!          识别 x/y/z（必需）、red/green/blue（可选颜色）、intensity（可选亮度）
    //!   - PCD：支持 DATA ascii / binary（不支持 binary_compressed）；
    //!          rgb/rgba 字段按标准 PCL 位模式（uint32 bit-cast）解析，而非当 float 数值

    //! 通过后缀判断是否为 PLY 文件（大小写不敏感）
    inline bool isPlyPath(const QString& path) {
        return path.endsWith(QStringLiteral(".ply"), Qt::CaseInsensitive);
    }

    //! 通过后缀判断是否为 PCD 文件（大小写不敏感）
    inline bool isPcdPath(const QString& path) {
        return path.endsWith(QStringLiteral(".pcd"), Qt::CaseInsensitive);
    }

    //! 只读 PLY/PCD 文本头部拿到顶点/点数（不读顶点数据，供 UI 线程廉价预检）
    //! @return true=成功识别为合法 PLY/PCD 头部
    bool peekPlyPcdPointCount(const QString& path, qint64& outCount, QString* outError = nullptr);

    // ────────────────────────────────────────────────────────────────────
    //! TIFF / SRF → TIFF 格式转换支持（导出/批量转换功能）
    //!
    //! 统一中间表示：world Z (mm, float)，无效点为 NaN
    //!   - 32-bit float TIFF：NaN 原样保留
    //!   - 16-bit signed (Storage=Int16RawPlus32768) 的 -32768 → NaN
    //!   - SRF：-32768 → NaN（按 SRF 规范）
    //!   - 16-bit unsigned 不识别"无效值"（无统一约定，保持原值）
    //! 输出 Z 数据时由 writer 把 NaN 映射为目标格式约定值（int16: -32768，float32: 用户指定）
    struct ExportSourceData
    {
        bool   valid = false;
        int    W = 0;
        int    H = 0;
        double scaleX = 1.0,  scaleY = 1.0,  scaleZ = 1.0;
        double offsetX = 0.0, offsetY = 0.0, offsetZ = 0.0;
        //! 源是否本身是 float（32-bit float TIFF）；否则使用 rawInt16
        bool   sourceIsFloat = false;
        //! 行优先 int16 高度（SRF 与 16-bit TIFF 走这里；-32768=无效）
        std::vector<qint16> rawInt16;
        //! 行优先 mm 浮点（仅 32-bit float TIFF 源；NaN=无效）
        std::vector<float>  zMm;
        //! 可选 uint8 亮度（与高度图同长度则有效）
        std::vector<quint8> intensity;
    };

    //! 从源文件（TIFF / SRF）读取统一中间表示
    bool readSourceForExport(const QString& path, ExportSourceData& out,
                              QString* outError = nullptr);

    //! 写 TIFF 时一并嵌入的标定元数据（mm 单位）
    //! 写入 GeoTIFF 标签：
    //!   33550 ModelPixelScaleTag    = [scaleX, scaleY, scaleZ]
    //!   33922 ModelTiepointTag      = [0, 0, 0, offsetX, offsetY, offsetZ]
    //! 以及 282/283 X/YResolution（pixels per cm）+ 296 ResolutionUnit=3(cm)
    //! + 271 Make + 270 ImageDescription（人可读 LMI_Gocator_Metadata 块）
    struct ExportTiffMeta {
        double scaleX  = 1.0;
        double scaleY  = 1.0;
        double scaleZ  = 1.0;
        double offsetX = 0.0;
        double offsetY = 0.0;
        double offsetZ = 0.0;
    };

    //! 写 16-bit unsigned TIFF（SampleFormat=1，LZW + Predictor=2）：
    //! 像素 = raw_int16 + 32768（UInt16RawPlus32768；与 LMI/Gocator 约定一致，无损保留 raw_int16）
    //! tag: ModelScaleZ = meta.scaleZ, TiepointZ = meta.offsetZ
    //! @param rawInt16  长度=W*H 的 int16 数组（-32768=无效；行优先）
    bool writeTiffUInt16(const QString& path, int W, int H,
                         const qint16* rawInt16,
                         const ExportTiffMeta& meta, QString* outError = nullptr);

    //! 写 16-bit signed TIFF（SampleFormat=2，LZW + Predictor=2）：
    //! 像素 = raw_int16 原值（Int16Raw；-32768 视为无效）
    bool writeTiffInt16(const QString& path, int W, int H,
                        const qint16* rawInt16,
                        const ExportTiffMeta& meta, QString* outError = nullptr);

    //! 写 32-bit float TIFF（SampleFormat=3，LZW，无 Predictor 以兼容 ImageJ）：
    //! 像素 = raw_int16 * scaleZ + offsetZ（mm）；raw==-32768 → invalidFill
    //! tag: ModelScaleZ = 1.0, TiepointZ = 0.0（Float32ActualHeight：像素已是世界 Z mm）
    bool writeTiffFloat32(const QString& path, int W, int H,
                          const qint16* rawInt16,
                          const ExportTiffMeta& meta,
                          float invalidFill, QString* outError = nullptr);

    //! 写 32-bit float TIFF —— 直接以 mm 浮点输入（用于源就是 float 的场景）
    //! 像素 = zMm[i]（NaN→invalidFill）；tag 同上
    bool writeTiffFloat32FromFloat(const QString& path, int W, int H,
                                    const float* zMm,
                                    const ExportTiffMeta& meta,
                                    float invalidFill, QString* outError = nullptr);

    //! 写灰度 BMP（uint8），并把 X/Y 像素分辨率写进 BITMAPINFOHEADER 的
    //! biXPelsPerMeter / biYPelsPerMeter（resolution = round(1000 / mmPerPixel)）
    //! 这样 Windows 资源管理器缩略图按真实比例显示
    //! @param scaleXmm,scaleYmm 单位 mm/pixel；<=0 时跳过写入分辨率字段
    bool writeBmpGray(const QString& path, int W, int H, const quint8* data,
                      double scaleXmm, double scaleYmm,
                      QString* outError = nullptr);

    //! 对已存在的 BMP 文件就地补写 X/Y 像素分辨率（pixels per meter）。
    //! 用于"复制外部 BMP 到导出目录"后，让其 DPI 与导出的 TIFF 一致。
    //! @return true=成功（找到合法 BMP 头并写入），false=非 BMP 或参数无效
    bool writeBmpResolutionInPlace(const QString& path,
                                    double scaleXmm, double scaleYmm);
}
