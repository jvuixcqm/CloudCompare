// TiffViewer — 独立 TIFF/BMP 点云浏览器
// Author: Jim

#include "TiffViewerWindow.h"
#include "TiffBmpLoader.h"

#include <QApplication>
#include <QMessageBox>
#include <algorithm>
#include <QSurfaceFormat>
#include <QOpenGLContext>
#include <QOpenGLVersionFunctionsFactory>
#include <QOpenGLFunctions_2_1>
#include <QDir>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QFile>
#include <QTextStream>

// CloudCompare 初始化例程（QCC_DB_LIB）
#include <ccColorScalesManager.h>
#include <ccNormalVectors.h>
#include <ccPointCloud.h>
#include <ccMesh.h>

// ── 应用图标：透明背景，6 段彩虹色 3D 等轴测高度图曲面 ──────────────────────
// 颜色与 TiffBmpLoader::lutEntry() 完全一致：
//   Blue -> Cyan -> Green -> Yellow -> Red -> Magenta -> White
static QIcon createAppIcon()
{
    // 6 段 LUT（i = 0..1536）
    auto lutColor = [](double t) -> QColor {
        int i = qBound(0, int(t * 1536.0), 1536);
        int r, g, b;
        if      (i <  256) { r=0;   g=i;                      b=255;   }
        else if (i <  512) { r=0;   g=255;                     b=511-i; }
        else if (i <  768) { r=i-512;   g=255;                 b=0;     }
        else if (i < 1024) { r=255; g=1023-i;                  b=0;     }
        else if (i < 1280) { r=255; g=0;                       b=i-1024;}
        else               { r=255; g=std::min(i-1280, 255);   b=255;   }
        return QColor(r, g, b, 255);
    };

    auto darken = [](QColor c, double f) -> QColor {
        return QColor(qBound(0,int(c.red()*f),255),
                      qBound(0,int(c.green()*f),255),
                      qBound(0,int(c.blue()*f),255), 255);
    };

    QIcon icon;
    for (int sz : {16, 32, 48, 64, 256})
    {
        QPixmap pm(sz, sz);
        pm.fill(Qt::transparent);          // 透明背景
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);

        // 分辨率越高格子越多
        const int cols = (sz >= 128) ? 8 : (sz >= 48) ? 6 : 5;
        const int rows = cols;
        const double margin = sz * 0.06;
        const double hw  = (sz - 2.0*margin) / (cols + rows + 1.0);
        const double hh  = hw * 0.48;
        const double scx = sz * 0.50;
        const double zsc = hw * 1.8;
        const double scy = sz * 0.50 + cols * zsc * 0.25;

        // 从后往前绘制（画家算法）
        for (int row = rows-1; row >= 0; --row)
        {
            for (int col = 0; col < cols; ++col)
            {
                const double t  = (col + (rows-1-row)) / double(cols + rows - 2);
                const double ht = t * t * t;   // 三次方：底部平缓，顶部层次丰富

                const double u  = col - (cols-1)*0.5;
                const double v  = row - (rows-1)*0.5;
                const double cx = scx + (u - v) * hw;
                const double cy = scy + (u + v) * hh - ht * zsc * cols;

                QColor tc = lutColor(t);

                // 侧面（32px 以上 + 有足够高度时绘制，增加立体感）
                if (sz >= 32 && ht > 0.01)
                {
                    const double sh = ht * zsc * cols * 0.32;
                    QPointF lp[4] = {{cx-hw,cy},{cx,cy+hh},{cx,cy+hh+sh},{cx-hw,cy+sh}};
                    QPointF rp[4] = {{cx+hw,cy},{cx,cy+hh},{cx,cy+hh+sh},{cx+hw,cy+sh}};
                    p.setPen(Qt::NoPen);
                    p.setBrush(darken(tc, 0.55));   // 左侧面更暗
                    p.drawPolygon(lp, 4);
                    p.setBrush(darken(tc, 0.72));   // 右侧面次暗
                    p.drawPolygon(rp, 4);
                }

                // 顶面
                QPointF top[4] = {{cx,cy-hh},{cx+hw,cy},{cx,cy+hh},{cx-hw,cy}};
                p.setBrush(tc);
                if (sz >= 24)
                    p.setPen(QPen(QColor(0,0,0, sz>=64?50:35), sz*0.004f));
                else
                    p.setPen(Qt::NoPen);
                p.drawPolygon(top, 4);
            }
        }
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

static void initOpenGLFormat()
{
    // 与 ccApplicationBase::InitOpenGL() 的 Windows 路径保持一致
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    fmt.setStencilBufferSize(0);
    fmt.setStereo(true);   // 请求立体支持（不支持时自动回退）
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    QSurfaceFormat::setDefaultFormat(fmt);
}

//! 命令行自检模式：绕过 GUI，直接调用 TiffBmpLoader::load() 并把结果写入日志文件。
//! 用法：TiffViewer.exe --selftest <输入文件> <日志输出路径>
//! 仅供开发期验证 PLY/PCD 解析与网格重建逻辑使用，不影响正常 GUI 启动流程。
static int runSelfTest(const QString& inputPath, const QString& logPath, const QString& modeStr = QStringLiteral("height"))
{
    QFile logFile(logPath);
    if (!logFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        return 2;
    QTextStream log(&logFile);

    DisplayMode mode = DisplayMode::Height;
    if (modeStr.compare(QLatin1String("brightness"), Qt::CaseInsensitive) == 0) mode = DisplayMode::Brightness;
    else if (modeStr.compare(QLatin1String("fusion"), Qt::CaseInsensitive) == 0) mode = DisplayMode::Fusion;
    else if (modeStr.compare(QLatin1String("heightgray"), Qt::CaseInsensitive) == 0) mode = DisplayMode::HeightGray;

    log << "input=" << inputPath << "\n";
    log << "mode=" << modeStr << "\n";

    QString err;
    int bitDepth = 0;
    bool fallback = false;
    ccHObject* obj = TiffBmpLoader::load(
        inputPath, QString(), 1.0, 1.0, 1.0, mode,
        0.5f, /*buildMesh=*/true, 0.0f, 1.0f, 0.0, /*useSyntheticBmp=*/true,
        &bitDepth, /*removeIslands=*/false, 100, 10, 0.0, 0.0, /*computeNormals=*/true,
        0.0, 0.0, 0.0, false, nullptr, &err, 1,
        ccUniqueIDGenerator::InvalidUniqueID, ccUniqueIDGenerator::InvalidUniqueID,
        &fallback);

    log << "bitDepth=" << bitDepth << "\n";
    log << "fallbackToUnorganized=" << (fallback ? "true" : "false") << "\n";

    if (!obj) {
        log << "result=FAIL\n";
        log << "error=" << err << "\n";
        logFile.close();
        return 1;
    }

    ccMesh* mesh = dynamic_cast<ccMesh*>(obj);
    ccPointCloud* cloud = mesh
        ? dynamic_cast<ccPointCloud*>(mesh->getAssociatedCloud())
        : dynamic_cast<ccPointCloud*>(obj);

    log << "result=OK\n";
    log << "objType=" << (mesh ? "Mesh" : "Cloud") << "\n";
    if (mesh)
        log << "triangleCount=" << mesh->size() << "\n";
    if (cloud) {
        log << "pointCount=" << cloud->size() << "\n";
        log << "hasColors=" << (cloud->hasColors() ? "true" : "false") << "\n";
        const unsigned n = std::min<unsigned>(cloud->size(), 5);
        for (unsigned i = 0; i < n; ++i) {
            const CCVector3* p = cloud->getPoint(i);
            log << "pt[" << i << "]=(" << p->x << "," << p->y << "," << p->z << ")";
            if (cloud->hasColors()) {
                const ccColor::Rgb& c = cloud->getPointColor(i);
                log << " rgb=(" << int(c.r) << "," << int(c.g) << "," << int(c.b) << ")";
            }
            log << "\n";
        }
    } else {
        log << "pointCount=UNKNOWN(cast failed)\n";
    }

    delete obj;
    logFile.close();
    return 0;
}

int main(int argc, char* argv[])
{
    // 必须在 QApplication 之前设置 OpenGL 格式
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    initOpenGLFormat();

    QApplication app(argc, argv);
    app.setApplicationName("TiffViewer");
    app.setApplicationVersion("1.0");
    app.setWindowIcon(createAppIcon());  // 任务栏 + 标题栏图标

    // 设置工作目录为可执行文件所在目录（方便相对路径的资源查找）
    QDir::setCurrent(QCoreApplication::applicationDirPath());

    // 命令行自检模式：TiffViewer.exe --selftest <文件> <日志路径>（不显示窗口，不需要 OpenGL）
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--selftest"))
    {
        ccNormalVectors::GetUniqueInstance();
        ccColorScalesManager::GetUniqueInstance();
        const QString modeStr = argc >= 5 ? QString::fromLocal8Bit(argv[4]) : QStringLiteral("height");
        const int rc = runSelfTest(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]), modeStr);
        ccPointCloud::ReleaseShaders();
        return rc;
    }

    // 验证 OpenGL 支持
    {
        QOpenGLContext ctx;
        if (!ctx.create())
        {
            QMessageBox::critical(nullptr, "错误",
                "此程序需要 OpenGL 支持才能运行。");
            return EXIT_FAILURE;
        }
        auto* glFunc = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_2_1>(&ctx);
        if (!glFunc)
        {
            QMessageBox::critical(nullptr, "错误",
                "此程序至少需要 OpenGL 2.1 才能运行。");
            return EXIT_FAILURE;
        }
    }

    // 初始化 CloudCompare 全局数据（预计算法线、颜色表）
    ccNormalVectors::GetUniqueInstance();
    ccColorScalesManager::GetUniqueInstance();

    TiffViewerWindow w;
    w.show();

    int result = app.exec();

    // 释放全局资源
    ccPointCloud::ReleaseShaders();

    return result;
}
