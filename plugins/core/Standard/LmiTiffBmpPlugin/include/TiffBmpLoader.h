#pragma once

#include <QString>
#include <QVector>
#include <ccHObject.h>
#include <ccPointCloud.h>
#include <ccMesh.h>

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
        int  bitDepth        = 16;   //!< 16 / 17(16-bit signed) / 32 / 128(2-channel)
        bool is32Float       = false;
        bool is16BitSigned   = false;
        bool is2Channel      = false;
        int  samplesPerPixel = 1;
        int  compression     = 1;
        int  planarConfig    = 1;

        QVector<quint32> bitsPerSample;
        QVector<quint32> sampleFormats;
        QVector<quint32> stripOffsets;
        QVector<quint32> stripByteCounts;
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
        unsigned meshUID        = ccUniqueIDGenerator::InvalidUniqueID
    );

    //! 轻量位深检测（仅读 IFD，不读像素）
    //! @return 128=双通道, 32=32-bit float, 17=16-bit有符号, 16=16-bit无符号
    int detectBitDepth(const QString& path, TiffInfo* outInfo = nullptr);
}
