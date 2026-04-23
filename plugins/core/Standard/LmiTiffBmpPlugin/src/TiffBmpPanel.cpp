// Copyright (c) 2026 LMI Technologies Inc. All rights reserved.
// Author: Jim Wang, LMI Technologies
#include "TiffBmpPanel.h"
#include "TiffBmpLoader.h"
#include "FlexibleDoubleSpinBox.h"

#include <ccMainAppInterface.h>
#include <ccObject.h>
#include <ccHObject.h>
#include <ccPointCloud.h>
#include <ccMesh.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QImageReader>
#include <QTimer>
#include <QSettings>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QApplication>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QShortcut>
#include <QKeySequence>
#include <QScrollArea>
#include <QFrame>
#include <QFutureWatcher>
#include <QSignalBlocker>
#include <QtConcurrent>
#include <ccGLWindowInterface.h>
#include <ccGLUtils.h>
#include <ccViewportParameters.h>
#include <ccBBox.h>

#include <algorithm>
#include <limits>
#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

// forward declaration — 实现在配置读写段
static bool restoreViewportFromSettings(QSettings& s, ccGLWindowInterface* glWin);

static QString pluginSettingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
         + "/LmiTiffBmpPlugin.ini";
}

static QSettings& panelSettings()
{
    static QSettings s(pluginSettingsPath(), QSettings::IniFormat);
    return s;
}

static constexpr int kPanelDefaultsVersion = 1;

// ─────────────────────────────────────────────────────────────────────────────

TiffBmpPanel::TiffBmpPanel(ccMainAppInterface* app, QWidget* parent, bool qcMode)
    : QWidget(parent)
    , m_app(app)
    , m_qcMode(qcMode)
{
    setFixedWidth(kPanelFixedWidth);
    setAcceptDrops(true);

    // ── 自动重载防抖定时器 ────────────────────────────────────────────────
    m_autoReloadTimer = new QTimer(this);
    m_autoReloadTimer->setSingleShot(true);
    m_autoReloadTimer->setInterval(400);
    connect(m_autoReloadTimer, &QTimer::timeout, this, &TiffBmpPanel::onLoad);

    m_settingsSaveTimer = new QTimer(this);
    m_settingsSaveTimer->setSingleShot(true);
    m_settingsSaveTimer->setInterval(800);
    connect(m_settingsSaveTimer, &QTimer::timeout, this, &TiffBmpPanel::flushSettings);

    m_loadWatcher = new QFutureWatcher<AsyncLoadResult>(this);
    connect(m_loadWatcher, &QFutureWatcher<AsyncLoadResult>::finished,
            this, &TiffBmpPanel::onAsyncLoadFinished);

    // ── 当前文件名显示 ────────────────────────────────────────────────────
    m_fileNameLabel = new QLabel("（未加载）");
    m_fileNameLabel->setAlignment(Qt::AlignCenter);
    m_fileNameLabel->setStyleSheet(
        "color:#333; font-weight:bold; background:#f0f0f0;"
        "border:1px solid #ccc; border-radius:3px; padding:2px 4px;");
    m_fileNameLabel->setWordWrap(true);
    m_fileNameLabel->setToolTip("当前加载的 TIFF 文件名");

    // ── 文件路径区 ────────────────────────────────────────────────────────
    m_fileGrp = new QGroupBox("文件路径");
    auto* fileBox = m_fileGrp;
    auto* fileLay = new QFormLayout(fileBox);
    fileLay->setSpacing(4);

    QPushButton* tiffBrowseBtn = nullptr;
    QPushButton* bmpBrowseBtn  = nullptr;
    QPushButton* bmpFolderBtn  = nullptr;

    {
        m_tiffEdit = new QLineEdit;
        m_tiffEdit->setPlaceholderText("选择或拖拽 TIFF 文件...");
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0,0,0,0);
        row->addWidget(m_tiffEdit);
        tiffBrowseBtn = new QPushButton("...");
        tiffBrowseBtn->setFixedWidth(28);
        row->addWidget(tiffBrowseBtn);
        fileLay->addRow("TIFF:", row);
    }
    {
        m_bmpEdit = new QLineEdit;
        m_bmpEdit->setPlaceholderText("自动匹配 / 手动选择亮度图...");
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0,0,0,0);
        row->addWidget(m_bmpEdit);
        bmpBrowseBtn = new QPushButton("...");
        bmpBrowseBtn->setFixedWidth(28);
        row->addWidget(bmpBrowseBtn);
        m_lblBmpRow = new QLabel("亮度图:");
        fileLay->addRow(m_lblBmpRow, row);
    }
    {
        m_bmpFolderEdit = new QLineEdit;
        m_bmpFolderEdit->setPlaceholderText("亮度图所在文件夹（可选）...");
        m_bmpFolderEdit->setToolTip(
            "若亮度图与 TIFF 不在同一文件夹，在此指定亮度图文件夹\n"
            "优先在此文件夹按同名、标准化名称、SN 前缀和常见亮度关键词进行智能匹配");
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0,0,0,0);
        row->addWidget(m_bmpFolderEdit);
        bmpFolderBtn = new QPushButton("...");
        bmpFolderBtn->setFixedWidth(28);
        row->addWidget(bmpFolderBtn);
        m_lblBmpFolderRow = new QLabel("亮度图文件夹:");
        fileLay->addRow(m_lblBmpFolderRow, row);
    }
    if (!m_qcMode) {
        m_keepObjectChk = new QCheckBox("保留为独立对象（勾选后需手动点击加载）");
        m_keepObjectChk->setToolTip(
            "勾选后每次加载结果作为独立对象保留，不覆盖上次导入的对象。\n"
            "同时禁用自动加载，需手动点击\"加载\"按钮触发。\n"
            "可用于将多张 TIFF 同时导入 CloudCompare 后进行对比或合并。");
        fileLay->addRow(m_keepObjectChk);
    }

    // ── 分辨率参数区 ──────────────────────────────────────────────────────
    m_resGrp = new QGroupBox("物理分辨率");
    auto* resBox = m_resGrp;
    auto* resLay = new QFormLayout(resBox);
    resLay->setSpacing(4);

    m_autoResChk = new QCheckBox("自动从文件名读取分辨率");
    m_autoResChk->setToolTip(
        "文件名格式: xxx_..._<X>_<Y>_<Z>.tif\n"
        "例: OUT5_16位TIFF_0.003_0.015_0.000100.tif");

    if (!m_qcMode) {
        m_useOffsetChk = new QCheckBox("启用偏移");
        m_useOffsetChk->setToolTip(
            "勾选后在分辨率列旁边显示偏移输入框\n"
            "最终坐标 = 像素位置 × 分辨率 + 偏移\n"
            "未勾选时偏移值视为 0");
    }

    {
        auto* chkRow = new QHBoxLayout;
        chkRow->setContentsMargins(0, 0, 0, 0);
        chkRow->setSpacing(12);
        chkRow->addWidget(m_autoResChk);
        if (m_useOffsetChk) chkRow->addWidget(m_useOffsetChk);
        chkRow->addStretch();
        resLay->addRow(chkRow);
    }

    auto makeResSpin = [](double val, double step,
                          const QString& tip) -> QDoubleSpinBox*
    {
        auto* s = new FlexibleDoubleSpinBox;
        s->setRange(1e-9, 1e9);
        s->setSingleStep(step);
        s->setValue(val);
        s->setSuffix(" mm");
        s->setToolTip(tip);
        return s;
    };
    auto makeOffSpin = [](const QString& tip) -> QDoubleSpinBox*
    {
        auto* s = new FlexibleDoubleSpinBox;
        s->setRange(-1e9, 1e9);
        s->setSingleStep(0.001);
        s->setValue(0.0);
        s->setSuffix(" mm");
        s->setToolTip(tip);
        return s;
    };

    m_resXSpin = makeResSpin(0.0069, 0.001, "X 方向像素间距，输出时 X 轴镜像");
    m_resYSpin = makeResSpin(0.015,  0.001, "Y 方向行间距");
    m_resZSpin = makeResSpin(0.0001, 0.001,
        "Z 高度分辨率: Z(mm) = raw × 此值\n（32-bit float TIFF 时自动忽略）");
    if (!m_qcMode) {
        m_offXSpin = makeOffSpin("X 偏移：最终 X = 像素列 × resX + offX");
        m_offYSpin = makeOffSpin("Y 偏移：最终 Y = 像素行 × resY + offY");
        m_offZSpin = makeOffSpin("Z 偏移：最终 Z = raw × resZ + offZ\n（32-bit float 时 raw 即浮点值）");
    }

    // ── 分辨率 (+ 偏移) 网格 ─────────────────────────────────────────────
    {
        auto* resOffWidget = new QWidget;
        auto* grid = new QGridLayout(resOffWidget);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(4);
        // 表头
        m_lblResHdr = new QLabel("分辨率");
        auto* hdrRes = m_lblResHdr;
        hdrRes->setAlignment(Qt::AlignCenter);
        QFont hdrFont = hdrRes->font();
        hdrFont.setPointSize(hdrFont.pointSize() - 1);
        hdrRes->setFont(hdrFont);
        hdrRes->setStyleSheet("color:#555;");
        grid->addWidget(hdrRes, 0, 1, Qt::AlignCenter);
        if (!m_qcMode) {
            m_offHdrLabel = new QLabel("偏移");
            m_offHdrLabel->setAlignment(Qt::AlignCenter);
            m_offHdrLabel->setFont(hdrFont);
            m_offHdrLabel->setStyleSheet("color:#555;");
            grid->addWidget(m_offHdrLabel, 0, 2, Qt::AlignCenter);
        }
        // X 行
        grid->addWidget(new QLabel("X:"), 1, 0, Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(m_resXSpin, 1, 1);
        if (!m_qcMode) grid->addWidget(m_offXSpin, 1, 2);
        // Y 行
        grid->addWidget(new QLabel("Y:"), 2, 0, Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(m_resYSpin, 2, 1);
        if (!m_qcMode) grid->addWidget(m_offYSpin, 2, 2);
        // Z 行
        grid->addWidget(new QLabel("Z:"), 3, 0, Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(m_resZSpin, 3, 1);
        if (!m_qcMode) grid->addWidget(m_offZSpin, 3, 2);
        grid->setColumnStretch(1, 1);
        if (!m_qcMode) grid->setColumnStretch(2, 1);
        resLay->addRow(resOffWidget);
    }
    // 初始隐藏偏移列（非 QC 模式）
    if (!m_qcMode) {
        m_offHdrLabel->setVisible(false);
        m_offXSpin->setVisible(false);
        m_offYSpin->setVisible(false);
        m_offZSpin->setVisible(false);
    }

    m_zInvalidSpin = new FlexibleDoubleSpinBox;
    m_zInvalidSpin->setRange(-1e9, 1e9);
    m_zInvalidSpin->setSingleStep(0.001);
    m_zInvalidSpin->setValue(0.0);
    m_zInvalidSpin->setToolTip(
        "原始像素值 ≤ 此值的点视为无效并跳过\n"
        "16-bit：整数（0~65535），默认 0（即跳过值为 0 的像素）\n"
        "32-bit float：NaN 像素自动跳过；浮点值 ≤ 此值的点同样跳过");
    m_lblZInvalidRow = new QLabel("Z 无效值:");
    resLay->addRow(m_lblZInvalidRow, m_zInvalidSpin);

    // 分辨率警告标签（初始隐藏）
    m_resWarningLabel = new QLabel();
    m_resWarningLabel->setWordWrap(true);
    m_resWarningLabel->setStyleSheet("color:red; font-size:11px;");
    m_resWarningLabel->setVisible(false);
    resLay->addRow(m_resWarningLabel);

    // ── 颜色渲染范围区 ────────────────────────────────────────────────────
    m_colorGrp = new QGroupBox("颜色渲染范围");
    auto* colorBox = m_colorGrp;
    auto* colorLay = new QFormLayout(colorBox);
    colorLay->setSpacing(4);

    auto makeColorSlider = [](int def) -> QSlider* {
        auto* s = new QSlider(Qt::Horizontal);
        s->setRange(0, 100);
        s->setValue(def);
        return s;
    };

    m_colorMinSlider = makeColorSlider(0);
    m_colorMinLabel  = new QLabel("0 %");
    m_colorMinLabel->setFixedWidth(40);
    auto* cMinRow = new QHBoxLayout;
    cMinRow->setContentsMargins(0,0,0,0);
    cMinRow->addWidget(m_colorMinSlider);
    cMinRow->addWidget(m_colorMinLabel);
    m_lblColorMinRow = new QLabel("下限:");
    colorLay->addRow(m_lblColorMinRow, cMinRow);

    m_colorMaxSlider = makeColorSlider(100);
    m_colorMaxLabel  = new QLabel("100 %");
    m_colorMaxLabel->setFixedWidth(40);
    auto* cMaxRow = new QHBoxLayout;
    cMaxRow->setContentsMargins(0,0,0,0);
    cMaxRow->addWidget(m_colorMaxSlider);
    cMaxRow->addWidget(m_colorMaxLabel);
    m_lblColorMaxRow = new QLabel("上限:");
    colorLay->addRow(m_lblColorMaxRow, cMaxRow);

    // ── 显示模式区 ────────────────────────────────────────────────────────
    m_modeGrp = new QGroupBox("显示模式");
    auto* modeBox = m_modeGrp;
    auto* modeLay = new QVBoxLayout(modeBox);
    modeLay->setSpacing(4);

    // 4 个模式切换按钮（与导航按钮等大：68×60）
    {
        struct ModeInfo { const char* label; const char* tip; };
        static const ModeInfo modes[4] = {
            { "高度\n色彩", "高度色彩 (Height)\nZ 值映射彩虹色，纯高度显示" },
            { "亮度\n灰度", "亮度灰度 (Brightness)\n使用 BMP/合成亮度图显示灰度" },
            { "融合\n模式", "融合模式 (Fusion)\n高度彩虹色与亮度灰度叠加，可调融合系数 α" },
            { "高度\n灰阶", "高度灰阶 (HeightGray)\nZ 值线性映射到 0~255 灰度" },
        };
        static const char* modeStyle =
            "QPushButton          { font-size:11pt; font-weight:bold;"
            "                       border:1px solid #bbb; border-radius:4px;"
            "                       background:#f5f5f5; color:#333; }"
            "QPushButton:checked  { background:#0078d4; color:white;"
            "                       border:2px solid #005a9e; }"
            "QPushButton:hover:!checked { background:#dde8f5; }"
            "QPushButton:checked:hover  { background:#006cbf; }";
        auto* modeBtnRow = new QHBoxLayout;
        modeBtnRow->setSpacing(6);
        modeBtnRow->setContentsMargins(0, 0, 0, 0);
        for (int i = 0; i < 4; ++i) {
            m_modeBtns[i] = new QPushButton(modes[i].label);
            m_modeBtns[i]->setFixedSize(68, 60);
            m_modeBtns[i]->setToolTip(modes[i].tip);
            m_modeBtns[i]->setCheckable(true);
            m_modeBtns[i]->setStyleSheet(modeStyle);
            modeBtnRow->addWidget(m_modeBtns[i]);
        }
        m_modeBtns[0]->setChecked(true);
        modeLay->addLayout(modeBtnRow);
    }

    m_fusionRow = new QWidget;
    auto* fusionLay = new QHBoxLayout(m_fusionRow);
    fusionLay->setContentsMargins(0, 2, 0, 0);
    m_lblAlphaText = new QLabel("融合系数 α:");
    fusionLay->addWidget(m_lblAlphaText);
    m_alphaSlider = new QSlider(Qt::Horizontal);
    m_alphaSlider->setRange(0, 100);
    m_alphaSlider->setValue(50);
    m_alphaSlider->setToolTip("0=纯高度色，100=纯亮度");
    fusionLay->addWidget(m_alphaSlider);
    m_alphaLabel = new QLabel("0.50");
    m_alphaLabel->setFixedWidth(36);
    fusionLay->addWidget(m_alphaLabel);
    modeLay->addWidget(m_fusionRow);
    m_fusionRow->setVisible(false);

    {
        auto* meshOptionsWidget = new QWidget(modeBox);
        auto* meshOptionsLay = new QVBoxLayout(meshOptionsWidget);
        meshOptionsLay->setContentsMargins(0, 0, 0, 0);
        meshOptionsLay->setSpacing(2);

        m_meshChk = new QCheckBox("生成三角网格（有序网格直接 mesh 化）");
        m_meshChk->setToolTip(
            "利用 TIFF 规则网格结构直接构建三角面片\n"
            "启用网格时会默认自动计算平滑法线");
        meshOptionsLay->addWidget(m_meshChk);

        m_maxEdgeSpin = new FlexibleDoubleSpinBox;
        m_maxEdgeSpin->setRange(0.0, 100.0);
        m_maxEdgeSpin->setSingleStep(0.01);
        m_maxEdgeSpin->setValue(0.1);
        m_maxEdgeSpin->setSuffix(" mm");
        m_maxEdgeSpin->setToolTip(
            "网格最大边长：超过此值的三角形被跳过，消除空洞边缘的悬空面片\n"
            "等效于 CC Delaunay 2.5D 的边长限制，但速度快得多\n"
            "0 = 不限制");

        auto* meshRow = new QHBoxLayout;
        meshRow->setContentsMargins(20, 0, 0, 0);
        meshRow->setSpacing(4);
        m_lblMaxEdgeText = new QLabel("最大边长:");
        meshRow->addWidget(m_lblMaxEdgeText);
        meshRow->addWidget(m_maxEdgeSpin);
        meshRow->addStretch();
        meshOptionsLay->addLayout(meshRow);

        modeLay->addWidget(meshOptionsWidget);
    }

    m_rotateZ90Chk = new QCheckBox("绕Z+旋转90°");
    m_rotateZ90Chk->setToolTip(
        "将点云/网格绕 Z 轴正方向旋转 90°\n"
        "用于纠正扫描方向与坐标系不一致的情况");

    m_rotateZ180Chk = new QCheckBox("绕Z+旋转180°");
    m_rotateZ180Chk->setToolTip(
        "将点云/网格绕 Z 轴正方向旋转 180°\n"
        "用于将扫描结果翻转方向");

    m_yDsSampleCb = new NoWheelComboBox;
    m_yDsSampleCb->addItem("全采");
    m_yDsSampleCb->addItem("1/2");
    m_yDsSampleCb->addItem("1/3");
    m_yDsSampleCb->addItem("1/5");
    m_yDsSampleCb->addItem("1/10");
    m_yDsSampleCb->addItem("1/20");
    m_yDsSampleCb->setToolTip(
        "Y 方向等步长降采样（大数据量快速预览）\n"
        "等步长保证相邻保留行间距均匀，网格模式下可正常连接\n"
        "全采：保留全部行（stride=1）\n"
        "1/2：每 2 行取 1 行（stride=2）\n"
        "1/3：每 3 行取 1 行（stride=3）\n"
        "1/5：每 5 行取 1 行（stride=5）\n"
        "1/10：每 10 行取 1 行（stride=10）\n"
        "1/20：每 20 行取 1 行（stride=20）");

    {
        auto* rotDsRow = new QHBoxLayout;
        rotDsRow->setContentsMargins(0, 0, 0, 0);
        rotDsRow->setSpacing(4);
        rotDsRow->addWidget(m_rotateZ90Chk);
        rotDsRow->addWidget(m_rotateZ180Chk);
        rotDsRow->addStretch();
        m_lblYDsText = new QLabel("Y降采:");
        rotDsRow->addWidget(m_lblYDsText);
        rotDsRow->addWidget(m_yDsSampleCb);
        modeLay->addLayout(rotDsRow);
    }

    // ── 噪声过滤区 ────────────────────────────────────────────────────────
    m_noiseGrp = new QGroupBox("噪声过滤");
    auto* noiseBox = m_noiseGrp;
    auto* noiseLay = new QVBoxLayout(noiseBox);
    noiseLay->setSpacing(4);

    m_removeIslandsChk = new QCheckBox("移除孤岛噪声");
    m_removeIslandsChk->setToolTip(
        "4连通 BFS 找出所有有效像素连通分量\n"
        "像素数低于阈值的分量视为孤岛并移除");
    m_minIslandSpin = new NoWheelSpinBox;
    m_minIslandSpin->setRange(2, 1000000);
    m_minIslandSpin->setValue(200);
    m_minIslandSpin->setSuffix(" px");
    m_minIslandSpin->setToolTip(
        "最小保留点数\n"
        "连通分量像素数 < 此值时视为孤岛并移除\n"
        "建议值：100~2000（取决于分辨率和孤岛大小）");
    m_minIslandSpin->setEnabled(false);

    // minIslandDistSpin 保留但隐藏（TiffBmpLoader 接口仍需要此参数）
    m_minIslandDistSpin = new NoWheelSpinBox;
    m_minIslandDistSpin->setVisible(false);
    m_minIslandDistSpin->setValue(0);

    m_maxZGapSpin = new FlexibleDoubleSpinBox;
    m_maxZGapSpin->setRange(0.0, 9999.0);
    m_maxZGapSpin->setValue(0.0);
    m_maxZGapSpin->setSuffix(" mm");
    m_maxZGapSpin->setToolTip(
        "Z 方向连通阈值（mm）\n"
        "相邻像素 Z 差超过此值时视为不连通，将被分成独立分量\n"
        "0 = 不限制（纯 XY 平面连通）\n"
        "建议从较大值开始调试（如 5.0 mm）");
    m_maxZGapSpin->setEnabled(false);

    auto* noiseRow1 = new QHBoxLayout;
    noiseRow1->setContentsMargins(0, 0, 0, 0);
    noiseRow1->setSpacing(4);
    noiseRow1->addWidget(m_removeIslandsChk);
    m_lblNoiseMinPx = new QLabel("最小点数:");
    noiseRow1->addWidget(m_lblNoiseMinPx);
    noiseRow1->addWidget(m_minIslandSpin);
    noiseRow1->addStretch();
    noiseLay->addLayout(noiseRow1);

    auto* noiseRow2 = new QHBoxLayout;
    noiseRow2->setContentsMargins(0, 0, 0, 0);
    noiseRow2->setSpacing(4);
    m_lblNoiseZGap = new QLabel("最大Z跳变:");
    noiseRow2->addWidget(m_lblNoiseZGap);
    noiseRow2->addWidget(m_maxZGapSpin);
    noiseRow2->addStretch();
    noiseLay->addLayout(noiseRow2);

    // ── 图像导航区 ────────────────────────────────────────────────────────
    m_navGrp = new QGroupBox("图像导航（文件夹）");
    auto* navBox = m_navGrp;
    navBox->setToolTip(
        "快捷键（CC 主窗口在前台时生效）：\n"
        "  ◀ / ▶   Left / Right  — 上一张 / 下一张\n"
        "|◀ / ▶|  Home  / End   — 第一张 / 最后一张\n"
        "  ↺        F5           — 刷新目录文件列表\n"
        "  ⟳        Space        — 重新加载当前帧");
    auto* navBoxLay = new QVBoxLayout(navBox);
    navBoxLay->setSpacing(4);

    // 排序方式
    auto* sortRow = new QHBoxLayout;
    sortRow->setContentsMargins(0, 0, 0, 0);
    m_lblSortText = new QLabel("排序方式:");
    sortRow->addWidget(m_lblSortText);
    m_sortOrderCb = new NoWheelComboBox;
    m_sortOrderCb->addItem("按文件名");
    m_sortOrderCb->addItem("按修改时间（旧→新）");
    m_sortOrderCb->addItem("按修改时间（新→旧）");
    m_sortOrderCb->setToolTip("设置文件夹内 TIFF 文件的排列顺序");
    sortRow->addWidget(m_sortOrderCb, 1);
    navBoxLay->addLayout(sortRow);

    // |◀  ◀  nav  ▶  ▶|
    auto* navBtnRow = new QHBoxLayout;
    navBtnRow->setSpacing(6);
    m_firstBtn = new QPushButton("|◀");
    m_firstBtn->setFixedSize(68, 60);
    m_firstBtn->setToolTip("第一张（刷新目录后跳转）");
    m_prevBtn = new QPushButton("◀");
    m_prevBtn->setFixedSize(68, 60);
    m_prevBtn->setToolTip("上一张（刷新目录后跳转）");
    m_navLabel = new QLabel("—");
    m_navLabel->setAlignment(Qt::AlignCenter);
    m_nextBtn = new QPushButton("▶");
    m_nextBtn->setFixedSize(68, 60);
    m_nextBtn->setToolTip("下一张（刷新目录后跳转）");
    m_lastBtn = new QPushButton("▶|");
    m_lastBtn->setFixedSize(68, 60);
    m_lastBtn->setToolTip("最后一张（刷新目录后跳转，常用于查看最新文件）");
    {
        QFont f = m_firstBtn->font();
        f.setPointSize(14);
        f.setBold(true);
        for (auto* btn : {m_firstBtn, m_prevBtn, m_nextBtn, m_lastBtn})
            btn->setFont(f);
    }
    navBtnRow->addWidget(m_firstBtn);
    navBtnRow->addWidget(m_prevBtn);
    navBtnRow->addWidget(m_navLabel, 1);
    navBtnRow->addWidget(m_nextBtn);
    navBtnRow->addWidget(m_lastBtn);
    navBoxLay->addLayout(navBtnRow);

    // ── 加载按钮 + 视角复位 + 状态 ──────────────────────────────────────
    m_loadButton = new QPushButton("加  载");
    {
        QFont f = m_loadButton->font();
        f.setBold(true);
        m_loadButton->setFont(f);
        m_loadButton->setFixedHeight(30);
    }

    auto* resetViewBtn = new QPushButton("复位视角");
    resetViewBtn->setFixedHeight(30);
    resetViewBtn->setToolTip("Zoom to fit — 将点云/网格居中显示，找回丢失的视角");

    auto* clearAllBtn = new QPushButton("清空场景");
    clearAllBtn->setFixedHeight(30);
    clearAllBtn->setToolTip(
        "移除 CloudCompare 场景中的全部点云和网格对象\n"
        "常用于「保留为独立对象」模式累积多帧后一键清理\n"
        "⚠ 清空后无法撤销");
    clearAllBtn->setStyleSheet(
        "QPushButton       { color:#cc3300; }"
        "QPushButton:hover { background:#fff0ee; color:#cc3300; }");

    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(6);
    btnRow->addWidget(m_loadButton, 3);
    btnRow->addWidget(clearAllBtn, 1);
    btnRow->addWidget(resetViewBtn, 1);

    m_statusLabel = new QLabel("就绪 — 支持拖拽 TIFF 文件");
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet("color:gray; font-style:italic;");

    // ── 主布局（内容放入滚动区，窗口过小时可滚动，不遮挡控件）────────────
    auto* contentWidget = new QWidget;
    auto* mainLay = new QVBoxLayout(contentWidget);
    mainLay->setContentsMargins(6, 6, 6, 6);
    mainLay->setSpacing(6);
    mainLay->addWidget(m_fileNameLabel);
    mainLay->addWidget(fileBox);
    mainLay->addWidget(resBox);
    mainLay->addWidget(colorBox);
    mainLay->addWidget(modeBox);
    mainLay->addWidget(noiseBox);
    mainLay->addWidget(navBox);

    // ── QC 模式：判定输出路径分组 ─────────────────────────────────────────
    if (m_qcMode) {
        m_qcGrp = new QGroupBox("判定输出路径");
        auto* qcBox    = m_qcGrp;
        auto* qcLay    = new QFormLayout(qcBox);
        qcLay->setSpacing(4);

        auto makePathRow = [&](QLineEdit*& edit, const QString& tip) {
            edit = new QLineEdit;
            edit->setPlaceholderText("（点击 ... 选择文件夹）");
            edit->setToolTip(tip);
            auto* btn = new QPushButton("...");
            btn->setFixedWidth(28);
            btn->setToolTip(tip);
            auto* row = new QHBoxLayout;
            row->setSpacing(4);
            row->addWidget(edit);
            row->addWidget(btn);
            connect(btn, &QPushButton::clicked, this, [this, &edit]() {
                const QString d = QFileDialog::getExistingDirectory(
                    this, "选择文件夹", edit->text().isEmpty()
                        ? QDir::homePath() : edit->text());
                if (!d.isEmpty()) {
                    edit->setText(d);
                    scheduleSettingsSave();
                }
            });
            connect(edit, &QLineEdit::textChanged,
                    this, &TiffBmpPanel::scheduleSettingsSave);
            return row;
        };

        m_lblOkFolderRow = new QLabel("OK 文件夹:");
        m_lblNgFolderRow = new QLabel("NG 文件夹:");
        qcLay->addRow(m_lblOkFolderRow, makePathRow(m_okFolderEdit,
            "判定为 OK 时，自动将 TIFF 和亮度图复制到此文件夹"));
        qcLay->addRow(m_lblNgFolderRow, makePathRow(m_ngFolderEdit,
            "判定为 NG 时，自动将 TIFF 和亮度图复制到此文件夹"));
        mainLay->addWidget(qcBox);
    }

    mainLay->addLayout(btnRow);
    mainLay->addWidget(m_statusLabel);

    // ── 语言切换行 ──────────────────────────────────────────────────────
    {
        auto* langRow = new QHBoxLayout;
        langRow->setContentsMargins(0, 0, 0, 0);
        m_lblLangText = new QLabel("界面语言:");
        m_langBtnZh = new QPushButton("中文");
        m_langBtnEn = new QPushButton("English");
        m_langBtnZh->setCheckable(true);
        m_langBtnEn->setCheckable(true);
        m_langBtnZh->setFixedHeight(22);
        m_langBtnEn->setFixedHeight(22);
        langRow->addStretch();
        langRow->addWidget(m_lblLangText);
        langRow->addSpacing(4);
        langRow->addWidget(m_langBtnZh);
        langRow->addWidget(m_langBtnEn);
        mainLay->addLayout(langRow);

        connect(m_langBtnZh, &QPushButton::clicked, this, [this] {
            m_langEn = false;
            saveSettings();
            retranslateUi();
        });
        connect(m_langBtnEn, &QPushButton::clicked, this, [this] {
            m_langEn = true;
            saveSettings();
            retranslateUi();
        });
    }

    mainLay->addStretch();

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidget(contentWidget);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* outerLay = new QVBoxLayout(this);
    outerLay->setContentsMargins(0, 0, 0, 0);
    outerLay->setSpacing(0);
    outerLay->addWidget(scrollArea);

    // ── 信号连接 ──────────────────────────────────────────────────────────
    connect(tiffBrowseBtn, &QPushButton::clicked, this, &TiffBmpPanel::onBrowseTiff);
    connect(bmpBrowseBtn,  &QPushButton::clicked, this, &TiffBmpPanel::onBrowseBmp);
    connect(bmpFolderBtn,  &QPushButton::clicked, this, &TiffBmpPanel::onBrowseBmpFolder);
    connect(m_tiffEdit, &QLineEdit::textChanged,
            this, &TiffBmpPanel::onTiffPathChanged);
    connect(m_bmpEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        if (currentModeNeedsBmp())
            triggerAutoReload();
        else
            scheduleSettingsSave();
    });
    connect(m_bmpFolderEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        m_dirImageFilesCache.clear();
        m_bmpMatchCache.clear();
        const QString tiff = m_tiffEdit->text().trimmed();
        if (currentModeNeedsBmp() && !tiff.isEmpty() && QFileInfo::exists(tiff)) {
            m_bmpEdit->setText(autoFindBmp(tiff));
            triggerAutoReload();
        } else {
            scheduleSettingsSave();
        }
    });
    connect(m_resXSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double) { triggerAutoReload(); });
    connect(m_resYSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double) { triggerAutoReload(); });
    connect(m_resZSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double) { triggerAutoReload(); });
    if (m_useOffsetChk) {
        connect(m_useOffsetChk, &QCheckBox::toggled, this, [this](bool on) {
            m_offHdrLabel->setVisible(on);
            m_offXSpin->setVisible(on);
            m_offYSpin->setVisible(on);
            m_offZSpin->setVisible(on);
            triggerAutoReload();
            scheduleSettingsSave();
        });
        connect(m_offXSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double) { triggerAutoReload(); });
        connect(m_offYSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double) { triggerAutoReload(); });
        connect(m_offZSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double) { triggerAutoReload(); });
    }
    connect(m_zInvalidSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double) { triggerAutoReload(); });
    for (int i = 0; i < 4; ++i) {
        connect(m_modeBtns[i], &QPushButton::clicked, this, [this, i]() {
            onDisplayModeChanged(i);
        });
    }
    connect(m_alphaSlider, &QSlider::valueChanged, this, [this](int v) {
        m_alphaLabel->setText(QString::number(v / 100.0, 'f', 2));
        triggerAutoReload();
    });
    connect(m_colorMinSlider, &QSlider::valueChanged,
            this, &TiffBmpPanel::onColorMinChanged);
    connect(m_colorMaxSlider, &QSlider::valueChanged,
            this, &TiffBmpPanel::onColorMaxChanged);
    connect(m_meshChk, &QCheckBox::toggled,
            this, [this](bool) { triggerAutoReload(); });
    connect(m_rotateZ90Chk,  &QCheckBox::toggled, this, [this](bool) { triggerAutoReload(); });
    connect(m_rotateZ180Chk, &QCheckBox::toggled, this, [this](bool) { triggerAutoReload(); });
    connect(m_yDsSampleCb, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { triggerAutoReload(); });
    if (m_keepObjectChk)
        connect(m_keepObjectChk, &QCheckBox::toggled,
                this, [this](bool) { scheduleSettingsSave(); });
    connect(m_maxEdgeSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double) { triggerAutoReload(); });
    connect(m_removeIslandsChk, &QCheckBox::toggled, this, [this](bool checked) {
        m_minIslandSpin->setEnabled(checked);
        m_maxZGapSpin->setEnabled(checked);
        triggerAutoReload();
    });
    connect(m_minIslandSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int) {
                if (m_removeIslandsChk->isChecked())
                    triggerAutoReload();
            });
    connect(m_maxZGapSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double) {
                if (m_removeIslandsChk->isChecked())
                    triggerAutoReload();
            });
    // m_minIslandDistSpin is hidden and unused; no signal connection needed
    connect(m_sortOrderCb, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &TiffBmpPanel::onSortOrderChanged);
    connect(m_autoResChk, &QCheckBox::toggled, this, [this](bool checked) {
        if (!checked) {
            m_resWarningLabel->setVisible(false);
        } else {
            const QString t = m_tiffEdit->text().trimmed();
            if (!t.isEmpty() && QFileInfo::exists(t)) {
                tryParseResolutionFromFilename(t);
                triggerAutoReload();
            }
        }
    });
    connect(m_loadButton,  &QPushButton::clicked, this, &TiffBmpPanel::onLoad);
    connect(resetViewBtn,  &QPushButton::clicked, this, [this]() {
        ccGLWindowInterface* glWin = m_app->getActiveGLWindow();
        if (!glWin) return;
        if (m_lastUID != 0) {
            ccHObject* obj = m_app->dbRootObject()->find(m_lastUID);
            if (obj) {
                ccBBox bb = obj->getOwnBB();
                glWin->updateConstellationCenterAndZoom(&bb);
                glWin->redraw();
                return;
            }
        }
        // fallback: zoom all
        glWin->setView(CC_FRONT_VIEW);
    });
    connect(clearAllBtn, &QPushButton::clicked, this, [this]() {
        if (!m_app) return;
        ccHObject* root = m_app->dbRootObject();
        if (!root || root->getChildrenNumber() == 0) {
            m_statusLabel->setText(ls("场景已空，无需清理", "Scene already empty"));
            m_statusLabel->setStyleSheet("color:#555; font-style:normal;");
            return;
        }
        const int count = static_cast<int>(root->getChildrenNumber());
        std::vector<ccHObject*> toRemove;
        toRemove.reserve(count);
        for (unsigned i = 0; i < root->getChildrenNumber(); ++i)
            toRemove.push_back(root->getChild(i));
        for (ccHObject* obj : toRemove)
            m_app->removeFromDB(obj, true);
        m_lastUID = 0;
        m_app->refreshAll();
        m_app->updateUI();
        m_statusLabel->setText(
            QString(ls("已清空场景（移除 %1 个对象）", "Scene cleared (%1 objects removed)"))
                .arg(count));
        m_statusLabel->setStyleSheet("color:#0055cc; font-style:normal;");
    });
    connect(m_firstBtn, &QPushButton::clicked, this, &TiffBmpPanel::onFirst);
    connect(m_prevBtn,  &QPushButton::clicked, this, &TiffBmpPanel::onPrev);
    connect(m_nextBtn,  &QPushButton::clicked, this, &TiffBmpPanel::onNext);
    connect(m_lastBtn,  &QPushButton::clicked, this, &TiffBmpPanel::onLast);

    updateNavUI();

    // ── 快捷键（Qt::WindowShortcut：CC 主窗口在前台时生效）────────────────
    // 导航：左右箭头 = 上/下一张；Home/End = 第一张/最后一张
    auto* scLeft  = new QShortcut(Qt::Key_Left,  this, nullptr, nullptr, Qt::WindowShortcut);
    auto* scRight = new QShortcut(Qt::Key_Right, this, nullptr, nullptr, Qt::WindowShortcut);
    auto* scHome  = new QShortcut(Qt::Key_Home,  this, nullptr, nullptr, Qt::WindowShortcut);
    auto* scEnd   = new QShortcut(Qt::Key_End,   this, nullptr, nullptr, Qt::WindowShortcut);
    // F5 = 仅刷新目录（更新文件数量/排序，不跳转）
    auto* scF5    = new QShortcut(Qt::Key_F5,    this, nullptr, nullptr, Qt::WindowShortcut);
    // Space = 重新加载当前帧
    auto* scSpace = new QShortcut(Qt::Key_Space, this, nullptr, nullptr, Qt::WindowShortcut);

    connect(scLeft,  &QShortcut::activated, this, &TiffBmpPanel::onPrev);
    connect(scRight, &QShortcut::activated, this, &TiffBmpPanel::onNext);
    connect(scHome,  &QShortcut::activated, this, &TiffBmpPanel::onFirst);
    connect(scEnd,   &QShortcut::activated, this, &TiffBmpPanel::onLast);
    connect(scF5,    &QShortcut::activated, this, &TiffBmpPanel::onRefreshDir);
    connect(scSpace, &QShortcut::activated, this, &TiffBmpPanel::onLoad);

    // ── 加载上次保存的配置 ────────────────────────────────────────────────
    loadSettings();
}

TiffBmpPanel::~TiffBmpPanel()
{
    if (m_loadWatcher) {
        disconnect(m_loadWatcher, nullptr, this, nullptr);
        if (m_loadWatcher->isRunning()) {
            m_loadWatcher->waitForFinished();
            const AsyncLoadResult result = m_loadWatcher->future().result();
            delete result.object;
        }
    }

    if (m_settingsSaveTimer && m_settingsSaveTimer->isActive())
        m_settingsSaveTimer->stop();
    flushSettings();
}

// ─────────────────────────────────────────────────────────────────────────────
// 拖拽支持
// ─────────────────────────────────────────────────────────────────────────────

void TiffBmpPanel::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls()) {
        for (const QUrl& u : e->mimeData()->urls()) {
            const QString p = u.toLocalFile().toLower();
            if (p.endsWith(".tif") || p.endsWith(".tiff")) {
                e->acceptProposedAction();
                return;
            }
        }
    }
}

void TiffBmpPanel::dropEvent(QDropEvent* e)
{
    if (!e->mimeData()->hasUrls()) return;
    for (const QUrl& u : e->mimeData()->urls()) {
        const QString p = u.toLocalFile();
        if (p.toLower().endsWith(".tif") || p.toLower().endsWith(".tiff")) {
            m_tiffEdit->setText(p);
            e->acceptProposedAction();
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 槽函数
// ─────────────────────────────────────────────────────────────────────────────

void TiffBmpPanel::onBrowseTiff()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "选择 TIFF 文件",
        QFileInfo(m_tiffEdit->text()).absolutePath(),
        "TIFF Files (*.tif *.tiff)");
    if (!path.isEmpty())
        m_tiffEdit->setText(path);
}

void TiffBmpPanel::onBrowseBmp()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "选择亮度图（可选）",
        QFileInfo(m_bmpEdit->text()).absolutePath(),
        "Image Files (*.bmp *.BMP *.png *.jpg *.jpeg *.tif *.tiff *.TIF *.TIFF)");
    if (!path.isEmpty()) {
        m_bmpEdit->setText(path);
        if (currentModeNeedsBmp())
            triggerAutoReload();
        else
            scheduleSettingsSave();
    }
}

void TiffBmpPanel::onBrowseBmpFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, "选择亮度图所在文件夹",
        m_bmpFolderEdit->text().isEmpty()
            ? QFileInfo(m_tiffEdit->text()).absolutePath()
            : m_bmpFolderEdit->text());
    if (!dir.isEmpty())
        m_bmpFolderEdit->setText(dir);
}

void TiffBmpPanel::onTiffPathChanged(const QString& path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
    {
        scheduleSettingsSave();
        return;
    }

    // 若 TIFF 所在目录发生变化，自动清空亮度图文件夹（避免跨项目带入旧路径）
    // m_lastScannedFolder 记录上次扫描的目录，空表示首次加载，不触发清空
    {
        const QString newDir = QFileInfo(path).absolutePath();
        if (!m_lastScannedFolder.isEmpty()
            && !m_bmpFolderEdit->text().isEmpty()
            && QDir(newDir).absolutePath() != QDir(m_lastScannedFolder).absolutePath())
        {
            QSignalBlocker blocker(m_bmpFolderEdit);
            m_bmpFolderEdit->clear();
            m_dirImageFilesCache.clear();
            m_bmpMatchCache.clear();
        }
    }

    scanFolder(path, true);
    if (m_autoResChk->isChecked())
        tryParseResolutionFromFilename(path);

    if (currentModeNeedsBmp()) {
        const QString matchedBmp = autoFindBmp(path);
        if (m_bmpEdit->text() != matchedBmp) {
            QSignalBlocker blocker(m_bmpEdit);
            m_bmpEdit->setText(matchedBmp);
        }
    }

    triggerAutoReload();   // 选中有效文件后自动加载一次
}

void TiffBmpPanel::onDisplayModeChanged(int index)
{
    // 互斥选中状态
    for (int i = 0; i < 4; ++i)
        m_modeBtns[i]->setChecked(i == index);

    m_fusionRow->setVisible(index == 2);              // 仅 Fusion 显示 α 滑块
    m_bmpEdit->setEnabled(currentModeNeedsBmp());     // Height/HeightGray 不需要 BMP

    const QString t = m_tiffEdit->text().trimmed();
    if (currentModeNeedsBmp() && !t.isEmpty() && QFileInfo::exists(t) && m_bmpEdit->text().trimmed().isEmpty()) {
        const QString matchedBmp = autoFindBmp(t);
        if (!matchedBmp.isEmpty()) {
            QSignalBlocker blocker(m_bmpEdit);
            m_bmpEdit->setText(matchedBmp);
        }
    }

    triggerAutoReload();
}

void TiffBmpPanel::onColorMinChanged(int val)
{
    m_colorMinLabel->setText(QString::number(val) + " %");
    if (val > m_colorMaxSlider->value()) {
        m_colorMaxSlider->blockSignals(true);
        m_colorMaxSlider->setValue(val);
        m_colorMaxSlider->blockSignals(false);
        m_colorMaxLabel->setText(QString::number(val) + " %");
    }
    triggerAutoReload();
}

void TiffBmpPanel::onColorMaxChanged(int val)
{
    m_colorMaxLabel->setText(QString::number(val) + " %");
    if (val < m_colorMinSlider->value()) {
        m_colorMinSlider->blockSignals(true);
        m_colorMinSlider->setValue(val);
        m_colorMinSlider->blockSignals(false);
        m_colorMinLabel->setText(QString::number(val) + " %");
    }
    triggerAutoReload();
}

void TiffBmpPanel::onSortOrderChanged(int)
{
    scheduleSettingsSave();

    const QString tiff = m_tiffEdit->text().trimmed();
    if (!tiff.isEmpty() && QFileInfo::exists(tiff))
        scanFolder(tiff, true);
}

void TiffBmpPanel::onRefreshDir()
{
    const QString tiff = m_tiffEdit->text().trimmed();
    if (tiff.isEmpty() || !QFileInfo::exists(tiff)) return;
    const int oldCount = m_folderFiles.size();
    scanFolder(tiff, true);
    const int newCount = m_folderFiles.size();
    m_statusLabel->setText(
        newCount == oldCount
            ? QString("目录已刷新（共 %1 个文件，无变化）").arg(newCount)
            : QString("目录已刷新：%1 → %2 个文件").arg(oldCount).arg(newCount));
    m_statusLabel->setStyleSheet("color:#0055cc;");
}

void TiffBmpPanel::onLoad()
{
    const QString tiff = m_tiffEdit->text().trimmed();
    if (tiff.isEmpty() || !QFileInfo::exists(tiff)) {
        m_statusLabel->setText(ls("❌ 请先选择有效的 TIFF 文件",
                                  "❌ Please select a valid TIFF file"));
        m_statusLabel->setStyleSheet("color:red;");
        return;
    }
    loadFile(tiff);
}

// 公共辅助：刷新目录后跳转到指定索引并加载
void TiffBmpPanel::onFirst()
{
    const QString cur = m_tiffEdit->text().trimmed();
    if (cur.isEmpty() || !QFileInfo::exists(cur)) return;
    scanFolder(cur, true);          // 刷新目录 + 重新排序
    if (m_folderFiles.isEmpty()) return;
    activateFileAtIndex(0);
}

void TiffBmpPanel::onPrev()
{
    const QString cur = m_tiffEdit->text().trimmed();
    if (cur.isEmpty() || !QFileInfo::exists(cur)) return;
    const int savedIdx = m_currentIdx;   // 记录刷新前的位置
    scanFolder(cur, true);               // 强制刷新，捕获目录变化
    // 用刷新前位置-1：避免新文件插在前面时当前文件位置后移、导致跳到非预期帧
    const int target = savedIdx - 1;
    if (target >= 0 && target < m_folderFiles.size()) {
        activateFileAtIndex(target);
    } else {
        m_statusLabel->setText(
            QString(ls("已是第一张（共 %1 个文件）", "Already at first (%1 files)"))
                .arg(m_folderFiles.size()));
        m_statusLabel->setStyleSheet("color:#0055cc;");
    }
}

void TiffBmpPanel::onNext()
{
    const QString cur = m_tiffEdit->text().trimmed();
    if (cur.isEmpty() || !QFileInfo::exists(cur)) return;
    const int savedIdx = m_currentIdx;   // 记录刷新前的位置
    scanFolder(cur, true);               // 强制刷新，捕获新写入的文件
    // 用刷新前位置+1：即使新文件排序在当前文件之前，也只向后走一步（到第N+1张，而非更远）
    const int target = savedIdx + 1;
    if (target < m_folderFiles.size()) {
        activateFileAtIndex(target);
    } else {
        m_statusLabel->setText(
            QString(ls("已是最后一张（共 %1 个文件）", "Already at last (%1 files)"))
                .arg(m_folderFiles.size()));
        m_statusLabel->setStyleSheet("color:#0055cc;");
    }
}

void TiffBmpPanel::onLast()
{
    const QString cur = m_tiffEdit->text().trimmed();
    if (cur.isEmpty() || !QFileInfo::exists(cur)) return;
    scanFolder(cur, true);          // 刷新目录，捕获新写入的文件
    if (m_folderFiles.isEmpty()) return;
    activateFileAtIndex(m_folderFiles.size() - 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// 私有辅助
// ─────────────────────────────────────────────────────────────────────────────

void TiffBmpPanel::triggerAutoReload()
{
    scheduleSettingsSave();

    // 保留模式下禁止自动加载，需手动点击加载按钮
    if (m_keepObjectChk && m_keepObjectChk->isChecked())
        return;

    const QString t = m_tiffEdit->text().trimmed();
    if (!t.isEmpty() && QFileInfo::exists(t))
        m_autoReloadTimer->start();
}

void TiffBmpPanel::activateFileAtIndex(int index)
{
    if (index < 0 || index >= m_folderFiles.size())
        return;

    m_currentIdx = index;
    const QString tiff = m_folderFiles[m_currentIdx];
    m_tiffEdit->blockSignals(true);
    m_tiffEdit->setText(tiff);
    m_tiffEdit->blockSignals(false);
    if (m_autoResChk->isChecked())
        tryParseResolutionFromFilename(tiff);

    if (currentModeNeedsBmp()) {
        QSignalBlocker blocker(m_bmpEdit);
        m_bmpEdit->setText(autoFindBmp(tiff));
    }

    updateNavUI();

    // 保留模式下只切换路径，不自动加载（需手动点击加载按钮）
    if (m_keepObjectChk && m_keepObjectChk->isChecked()) {
        m_statusLabel->setText(
            QString(ls("已切换: %1（保留模式，请手动点击加载）",
                       "Switched: %1 (keep mode — click Load manually)"))
                .arg(QFileInfo(tiff).fileName()));
        m_statusLabel->setStyleSheet("color:#555;");
        scheduleSettingsSave();
        return;
    }

    loadFile(tiff);
}

void TiffBmpPanel::tryParseResolutionFromFilename(const QString& path)
{
    const QString base = QFileInfo(path).completeBaseName();
    const QStringList parts = base.split('_');

    auto warn = [&](const QString& msg) {
        m_resWarningLabel->setText(msg);
        m_resWarningLabel->setVisible(true);
    };

    if (parts.size() < 3) {
        warn(ls("⚠ 文件名不含分辨率信息（需末尾有 3 段下划线分隔的数值）",
                "⚠ No resolution in filename (need 3 underscore-separated values at end)"));
        return;
    }
    bool ok1, ok2, ok3;
    const double rx = parts[parts.size()-3].toDouble(&ok1);
    const double ry = parts[parts.size()-2].toDouble(&ok2);
    const double rz = parts[parts.size()-1].toDouble(&ok3);

    if (!ok1 || !ok2 || !ok3) {
        warn(ls("⚠ 文件名末尾 3 段无法解析为数值，请手动设置分辨率",
                "⚠ Last 3 segments cannot be parsed as numbers, set manually"));
        return;
    }
    if (rx > 1.0 || ry > 1.0 || rz > 1.0) {
        m_resWarningLabel->setVisible(false);
        m_statusLabel->setText(
            QString(ls("⚠ 自动分辨率异常（X=%1 Y=%2 Z=%3 mm，含大于 1 的值），已忽略，请手动设置",
                       "⚠ Auto-res abnormal (X=%1 Y=%2 Z=%3 mm, value >1), ignored — set manually"))
                .arg(rx, 0, 'g', 4).arg(ry, 0, 'g', 4).arg(rz, 0, 'g', 4));
        m_statusLabel->setStyleSheet("color:#cc6600;");
        return;
    }
    if (rx <= 0 || ry <= 0 || rz <= 0) {
        warn(QString(ls("⚠ 解析到的分辨率含零或负值（X=%1 Y=%2 Z=%3），请手动设置",
                        "⚠ Parsed resolution has zero/negative value (X=%1 Y=%2 Z=%3), set manually"))
             .arg(rx, 0, 'g', 4).arg(ry, 0, 'g', 4).arg(rz, 0, 'g', 4));
        return;
    }

    m_resXSpin->setValue(rx);
    m_resYSpin->setValue(ry);
    m_resZSpin->setValue(rz);
    m_resWarningLabel->setVisible(false);
    m_statusLabel->setText(
        QString(ls("自动分辨率: X=%1 Y=%2 Z=%3", "Auto-res: X=%1 Y=%2 Z=%3"))
            .arg(rx).arg(ry).arg(rz));
    m_statusLabel->setStyleSheet("color:#0055cc;");
}

// 自然排序比较：数字段按数值大小，非数字段大小写不敏感
// 解决 OUT11 排在 OUT2 前面的问题
static bool naturalLessThan(const QString& a, const QString& b)
{
    int ia = 0, ib = 0;
    const int la = a.size(), lb = b.size();
    while (ia < la && ib < lb) {
        if (a[ia].isDigit() && b[ib].isDigit()) {
            int sa = ia, sb = ib;
            while (ia < la && a[ia] == QLatin1Char('0')) ++ia;
            while (ib < lb && b[ib] == QLatin1Char('0')) ++ib;
            int ea = ia, eb = ib;
            while (ea < la && a[ea].isDigit()) ++ea;
            while (eb < lb && b[eb].isDigit()) ++eb;
            const int lenA = ea - ia, lenB = eb - ib;
            if (lenA != lenB) return lenA < lenB;
            for (int k = 0; k < lenA; ++k)
                if (a[ia + k] != b[ib + k]) return a[ia + k] < b[ib + k];
            ia = ea; ib = eb;
            (void)sa; (void)sb;
        } else {
            const QChar ca = a[ia].toLower(), cb = b[ib].toLower();
            if (ca != cb) return ca < cb;
            ++ia; ++ib;
        }
    }
    return (la - ia) < (lb - ib);
}

static QString stripResolutionSuffix(const QString& baseName)
{
    const QStringList parts = baseName.split('_', Qt::KeepEmptyParts);
    if (parts.size() < 4)
        return baseName;

    bool okX = false;
    bool okY = false;
    bool okZ = false;
    parts[parts.size() - 3].toDouble(&okX);
    parts[parts.size() - 2].toDouble(&okY);
    parts[parts.size() - 1].toDouble(&okZ);
    if (!okX || !okY || !okZ)
        return baseName;

    QStringList trimmed = parts;
    trimmed.removeLast();
    trimmed.removeLast();
    trimmed.removeLast();
    return trimmed.join('_');
}

static QStringList splitMatchTokens(const QString& text)
{
    return text.toLower().split(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+")),
                                Qt::SkipEmptyParts);
}

static QString cleanupMatchToken(QString token)
{
    static const QStringList noiseWords = {
        QStringLiteral("tif"), QStringLiteral("tiff"),
        QStringLiteral("bmp"), QStringLiteral("png"),
        QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("gray"), QStringLiteral("grey"),
        QStringLiteral("grayscale"), QStringLiteral("greyscale"),
        QStringLiteral("brightness"), QStringLiteral("bright"),
        QStringLiteral("luma"), QStringLiteral("luminance"),
        QStringLiteral("intensity"), QStringLiteral("image"),
        QStringLiteral("img"), QStringLiteral("photo"),
        QStringLiteral("pic"), QStringLiteral("rgb"),
        QStringLiteral("rgba"), QStringLiteral("color"),
        QStringLiteral("colour"), QStringLiteral("height"),
        QStringLiteral("mono"), QStringLiteral("channel"),
        QStringLiteral("高程"), QStringLiteral("高度"),
        QStringLiteral("亮度"), QStringLiteral("灰度"),
        QStringLiteral("灰阶"), QStringLiteral("图像"),
        QStringLiteral("影像")
    };

    token = token.toLower();
    for (const QString& word : noiseWords)
        token.remove(word);
    token.remove(QRegularExpression(QStringLiteral("\\d+bit")));
    token.remove(QRegularExpression(QStringLiteral("\\d+位")));
    return token.trimmed();
}

static QStringList coreMatchTokens(const QString& baseName)
{
    QStringList out;
    const QString stripped = stripResolutionSuffix(baseName);
    for (const QString& token : splitMatchTokens(stripped)) {
        const QString cleaned = cleanupMatchToken(token);
        if (!cleaned.isEmpty())
            out << cleaned;
    }
    out.removeDuplicates();
    return out;
}

static QString normalizedMatchKey(const QString& baseName)
{
    const QString key = coreMatchTokens(baseName).join(QString());
    if (!key.isEmpty())
        return key;

    QString fallback = stripResolutionSuffix(baseName).toLower();
    fallback.remove(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+")));
    fallback = cleanupMatchToken(fallback);
    return fallback;
}

static int sharedTokenCount(const QStringList& lhs, const QStringList& rhs)
{
    const QSet<QString> right(rhs.begin(), rhs.end());
    int shared = 0;
    for (const QString& token : lhs) {
        if (right.contains(token))
            ++shared;
    }
    return shared;
}

static bool containsAllTokens(const QStringList& haystack, const QStringList& needle)
{
    if (needle.isEmpty())
        return false;

    const QSet<QString> hay(haystack.begin(), haystack.end());
    for (const QString& token : needle) {
        if (!hay.contains(token))
            return false;
    }
    return true;
}

// 判断一个 token 是否像产品 SN：纯字母数字、长度 > 4、同时含字母和数字
static bool isSNToken(const QString& token)
{
    if (token.length() <= 4) return false;
    bool hasLetter = false, hasDigit = false;
    for (const QChar& c : token) {
        if (!c.isLetterOrNumber()) return false;
        if (c.isLetter()) hasLetter = true;
        else               hasDigit = true;
    }
    return hasLetter && hasDigit;
}

// 从文件基名中提取所有 SN-like token（位置不限，可在文件名任意位置）
static QStringList extractSNTokens(const QString& baseName)
{
    QStringList result;
    for (const QString& part :
         baseName.split(QRegularExpression(QStringLiteral("[_\\-\\s.]+")),
                        Qt::SkipEmptyParts))
    {
        if (isSNToken(part))
            result << part.toLower();
    }
    result.removeDuplicates();
    return result;
}

// 两组 SN token 是否有交集（共享至少一个相同 SN）
static bool sharesSNToken(const QStringList& a, const QStringList& b)
{
    const QSet<QString> setA(a.begin(), a.end());
    for (const QString& t : b)
        if (setA.contains(t)) return true;
    return false;
}

static bool hasBrightnessHint(const QString& lowerName)
{
    static const QStringList hints = {
        QStringLiteral("bmp"), QStringLiteral("gray"), QStringLiteral("grey"),
        QStringLiteral("grayscale"), QStringLiteral("greyscale"),
        QStringLiteral("brightness"), QStringLiteral("bright"),
        QStringLiteral("luma"), QStringLiteral("luminance"),
        QStringLiteral("intensity"), QStringLiteral("mono"),
        QStringLiteral("亮度"), QStringLiteral("灰度"), QStringLiteral("灰阶")
    };
    for (const QString& hint : hints) {
        if (lowerName.contains(hint))
            return true;
    }
    return false;
}

static bool fileMatchesExtensions(const QString& fileName, const QStringList& extensions)
{
    const QString lowerName = fileName.toLower();
    for (const QString& ext : extensions) {
        if (lowerName.endsWith(ext.toLower()))
            return true;
    }
    return false;
}

void TiffBmpPanel::scanFolder(const QString& tiffPath, bool forceRefresh)
{
    const QFileInfo fi(tiffPath);
    if (!fi.exists()) return;

    const QString folderPath = fi.absolutePath();
    const int sortOrder = m_sortOrderCb->currentIndex();

    if (!forceRefresh
        && folderPath == m_lastScannedFolder
        && sortOrder == m_lastScanSortOrder
        && !m_folderFiles.isEmpty())
    {
        m_currentIdx = m_folderFiles.indexOf(fi.absoluteFilePath());
        updateNavUI();
        return;
    }

    if (forceRefresh) {
        m_dirImageFilesCache.clear();
        m_bmpMatchCache.clear();
    }

    const QDir dir = fi.absoluteDir();

    QDir::SortFlags sf;
    switch (sortOrder) {
        case 1: sf = QDir::Time | QDir::Reversed; break; // 旧→新
        case 2: sf = QDir::Time;                  break; // 新→旧
        default: sf = QDir::NoSort;               break; // 自然排序由下方 std::sort 处理
    }

    QStringList names = dir.entryList({"*.tif", "*.tiff"}, QDir::Files, sf);

    // 按文件名自然排序（数字部分按数值，OUT2 < OUT11）
    if (m_sortOrderCb->currentIndex() == 0)
        std::sort(names.begin(), names.end(), naturalLessThan);

    m_folderFiles.clear();
    for (const QString& n : names)
        m_folderFiles << dir.absoluteFilePath(n);

    m_currentIdx = m_folderFiles.indexOf(fi.absoluteFilePath());
    m_lastScannedFolder = folderPath;
    m_lastScanSortOrder = sortOrder;
    updateNavUI();
}

void TiffBmpPanel::updateNavUI()
{
    const bool hasFiles = !m_folderFiles.isEmpty();
    const int  last     = m_folderFiles.size() - 1;
    m_firstBtn->setEnabled(hasFiles && m_currentIdx > 0);
    m_prevBtn ->setEnabled(hasFiles);   // 不禁用：即使在第一张也刷新目录后给出提示
    m_nextBtn ->setEnabled(hasFiles);   // 不禁用：即使在最后一张也刷新目录后给出提示
    m_lastBtn ->setEnabled(hasFiles && m_currentIdx < last);

    if (hasFiles && m_currentIdx >= 0)
        m_navLabel->setText(
            QString("%1 / %2").arg(m_currentIdx + 1).arg(m_folderFiles.size()));
    else
        m_navLabel->setText("—");
}

QString TiffBmpPanel::autoFindBmp(const QString& tiffPath) const
{
    struct MatchCandidate
    {
        QString path;
        QString fileName;
        qint64  score = std::numeric_limits<qint64>::min();
        qint64  timeDelta = std::numeric_limits<qint64>::max();
    };

    const QFileInfo fi(tiffPath);
    const QString tiffDir = fi.absolutePath();
    const QString base    = fi.completeBaseName();
    const QString strippedBase = stripResolutionSuffix(base);
    const QString baseLower = base.toLower();
    const QString strippedBaseLower = strippedBase.toLower();
    QDir tiffDirObj(tiffDir);
    QString parentDir = tiffDir;
    if (tiffDirObj.cdUp())
        parentDir = tiffDirObj.absolutePath();
    const QString baseKey = normalizedMatchKey(base);
    const QString strippedBaseKey = normalizedMatchKey(strippedBase);
    const QStringList baseTokens = coreMatchTokens(base);
    const QStringList tiffSNTokens = extractSNTokens(strippedBase);
    const QString bmpFolder = m_bmpFolderEdit->text().trimmed();
    const qint64 tiffMTime = fi.lastModified().toMSecsSinceEpoch();

    auto normalizePathKey = [](const QString& path) -> QString {
        if (path.isEmpty())
            return {};
        return QDir::cleanPath(path).toLower();
    };

    const QString tiffPathKey = normalizePathKey(fi.absoluteFilePath());
    const QString tiffDirKey = normalizePathKey(QDir(tiffDir).canonicalPath().isEmpty()
        ? QDir(tiffDir).absolutePath()
        : QDir(tiffDir).canonicalPath());
    const QString bmpFolderAbs = bmpFolder.isEmpty() ? QString() : QDir(bmpFolder).absolutePath();
    const QString bmpFolderKey = normalizePathKey(QDir(bmpFolderAbs).canonicalPath().isEmpty()
        ? bmpFolderAbs
        : QDir(bmpFolderAbs).canonicalPath());
    const QString cacheKey = tiffPathKey + "|" + bmpFolderKey;

    auto it = m_bmpMatchCache.constFind(cacheKey);
    if (it != m_bmpMatchCache.constEnd())
        return it.value();

    auto cacheAndReturn = [&](const QString& matchedPath) -> QString {
        m_bmpMatchCache.insert(cacheKey, matchedPath);
        return matchedPath;
    };

    // ── 获取 TIFF 像素尺寸（只读文件头，结果缓存）────────────────────────
    // autoFindBmp 可能在 prepareTiffInfo 之前被调用，此处按需填充缓存。
    {
        const QString absKey = fi.absoluteFilePath();
        if (!m_tiffInfoCache.contains(absKey)) {
            TiffBmpLoader::TiffInfo tinfo;
            TiffBmpLoader::detectBitDepth(absKey, &tinfo);
            if (tinfo.valid)
                m_tiffInfoCache.insert(absKey, tinfo);
        }
    }
    const auto tiffIt = m_tiffInfoCache.constFind(fi.absoluteFilePath());
    const int tiffW = (tiffIt != m_tiffInfoCache.constEnd()) ? tiffIt->width  : 0;
    const int tiffH = (tiffIt != m_tiffInfoCache.constEnd()) ? tiffIt->height : 0;

    // 候选文件尺寸与 TIFF 是否一致（只读图像文件头，不解码像素）
    // tiffW/tiffH 未知时返回 true（放行），避免因解析失败而过度过滤。
    auto dimMatch = [&](const QString& path) -> bool {
        if (tiffW <= 0 || tiffH <= 0) return true;
        QImageReader reader(path);
        const QSize sz = reader.size();
        return sz.isValid() && sz.width() == tiffW && sz.height() == tiffH;
    };

    // baseExts: normal search — no tif/tiff to avoid re-loading the source TIFF as brightness
    static const QStringList baseExts = {
        ".bmp", ".BMP", ".png", ".PNG", ".jpg", ".JPG", ".jpeg", ".JPEG"
    };
    // folderExts: tif/tiff only allowed when a separate brightness folder is specified
    static const QStringList folderExts = {
        ".bmp", ".BMP", ".png", ".PNG", ".jpg", ".JPG", ".jpeg", ".JPEG",
        ".tif", ".TIF", ".tiff", ".TIFF"
    };

    // Enable tif/tiff only when bmpFolder is set AND is a different directory from tiffDir
    const bool useFolderExts = !bmpFolderKey.isEmpty() && bmpFolderKey != tiffDirKey;

    // ── 快速路径：按名称直接探测文件是否存在，跳过全目录扫描 ──────────────
    // 覆盖绝大多数情况：TIFF 与亮度图同名（仅扩展名不同）
    {
        // 检测给定目录下是否存在 baseName + ext（不扫描目录），并验证像素尺寸
        auto tryExact = [&](const QString& dir, const QString& bn,
                            const QStringList& exts) -> QString {
            if (dir.isEmpty() || bn.isEmpty()) return {};
            for (const QString& ext : exts) {
                const QString p = dir + "/" + bn + ext;
                if (QFileInfo::exists(p)
                    && normalizePathKey(p) != tiffPathKey
                    && dimMatch(p))
                    return QDir::cleanPath(p);
            }
            return {};
        };

        // 尝试顺序：独立亮度图文件夹 → tiffDir/bmp → tiffDir（均只试 baseExts）
        // 若 bmpFolder 指向不同目录且 useFolderExts，才额外尝试 tif/tiff
        const QStringList& fastFolderExts = useFolderExts ? folderExts : baseExts;

        QString found;
        if (!bmpFolder.isEmpty()) {
            found = tryExact(bmpFolder, base, fastFolderExts);
            if (found.isEmpty()) found = tryExact(bmpFolder, strippedBase, fastFolderExts);
        }
        if (found.isEmpty()) found = tryExact(tiffDir + "/bmp",  base,        baseExts);
        if (found.isEmpty()) found = tryExact(tiffDir + "/bmp",  strippedBase,baseExts);
        if (found.isEmpty()) found = tryExact(tiffDir,           base,        baseExts);
        if (found.isEmpty()) found = tryExact(tiffDir,           strippedBase,baseExts);
        if (!found.isEmpty())
            return cacheAndReturn(found);
    }

    MatchCandidate bestMatch;

    auto betterThanCurrent = [&](const MatchCandidate& candidate) -> bool {
        if (candidate.score != bestMatch.score)
            return candidate.score > bestMatch.score;
        if (candidate.timeDelta != bestMatch.timeDelta)
            return candidate.timeDelta < bestMatch.timeDelta;
        return bestMatch.fileName.isEmpty() || naturalLessThan(candidate.fileName, bestMatch.fileName);
    };

    auto evaluateDir = [&](const QString& searchPath, const QStringList& exts, qint64 dirBonus) {
        const QDir qdir(searchPath);
        if (!qdir.exists())
            return;

        const QStringList files = listImageFiles(qdir.absolutePath());
        for (const QString& f : files) {
            if (!fileMatchesExtensions(f, exts))
                continue;

            const QString absPath = qdir.absoluteFilePath(f);
            if (normalizePathKey(absPath) == tiffPathKey)
                continue;

            const QFileInfo candFi(absPath);
            if (!candFi.exists())
                continue;

            // 像素尺寸与 TIFF 不符则直接跳过（只读文件头，成本极低）
            if (!dimMatch(absPath))
                continue;

            const QString candidateBase = candFi.completeBaseName();
            const QString candidateStrippedBase = stripResolutionSuffix(candidateBase);
            const QString candidateBaseLower = candidateBase.toLower();
            const QString candidateStrippedLower = candidateStrippedBase.toLower();
            const QString candidateKey = normalizedMatchKey(candidateBase);
            const QString candidateStrippedKey = normalizedMatchKey(candidateStrippedBase);
            const QStringList candidateTokens = coreMatchTokens(candidateBase);
            const QStringList candSNTokens = extractSNTokens(candidateStrippedBase);
            const int shared = sharedTokenCount(baseTokens, candidateTokens);

            const bool exactBaseMatch = (candidateBaseLower == baseLower);
            const bool exactStrippedMatch = (!strippedBaseLower.isEmpty()
                                             && candidateStrippedLower == strippedBaseLower);
            const bool keyMatch = (!baseKey.isEmpty()
                                   && (!candidateKey.isEmpty() && candidateKey == baseKey));
            const bool strippedKeyMatch = (!strippedBaseKey.isEmpty()
                                           && ((!candidateKey.isEmpty() && candidateKey == strippedBaseKey)
                                               || (!candidateStrippedKey.isEmpty()
                                                   && candidateStrippedKey == strippedBaseKey)));
            const bool prefixMatch = (!strippedBaseLower.isEmpty()
                                      && (candidateBaseLower.startsWith(strippedBaseLower)
                                          || candidateStrippedLower.startsWith(strippedBaseLower)
                                          || strippedBaseLower.startsWith(candidateStrippedLower)));
            const bool containsMatch = (!strippedBaseLower.isEmpty()
                                        && !candidateStrippedLower.isEmpty()
                                        && (candidateBaseLower.contains(strippedBaseLower)
                                            || candidateStrippedLower.contains(strippedBaseLower)
                                            || strippedBaseLower.contains(candidateStrippedLower)));
            const bool snMatch = !tiffSNTokens.isEmpty() && !candSNTokens.isEmpty()
                                  && sharesSNToken(tiffSNTokens, candSNTokens);
            const bool tokenCover = containsAllTokens(candidateTokens, baseTokens);
            const bool tokenSubset = containsAllTokens(baseTokens, candidateTokens);
            const bool plausible = exactBaseMatch || exactStrippedMatch || keyMatch
                                   || strippedKeyMatch || prefixMatch || containsMatch
                                   || snMatch || shared > 0 || tokenCover || tokenSubset;
            if (!plausible)
                continue;

            qint64 score = dirBonus;
            if (exactBaseMatch)
                score += 120000;
            if (exactStrippedMatch)
                score += 90000;
            if (keyMatch)
                score += 65000;
            if (strippedKeyMatch)
                score += 60000;
            if (prefixMatch)
                score += 22000;
            if (containsMatch)
                score += 12000;
            if (snMatch)
                score += 18000;
            score += static_cast<qint64>(shared) * 6000;
            if (tokenCover)
                score += 12000;
            if (tokenSubset)
                score += 6000;
            if (hasBrightnessHint(candidateBaseLower))
                score += 2500;

            const QString lowerFileName = f.toLower();
            if (lowerFileName.endsWith(".bmp") || lowerFileName.endsWith(".png"))
                score += 900;
            else if (lowerFileName.endsWith(".jpg") || lowerFileName.endsWith(".jpeg"))
                score += 600;
            else if (lowerFileName.endsWith(".tif") || lowerFileName.endsWith(".tiff"))
                score += 300;

            const qint64 candidateMTime = candFi.lastModified().toMSecsSinceEpoch();
            const qint64 timeDelta = (candidateMTime >= tiffMTime)
                ? (candidateMTime - tiffMTime)
                : (tiffMTime - candidateMTime);

            MatchCandidate candidate;
            candidate.path = absPath;
            candidate.fileName = f;
            candidate.score = score;
            candidate.timeDelta = timeDelta;
            if (betterThanCurrent(candidate))
                bestMatch = candidate;
        }
    };

    QList<QPair<QString, qint64>> searchDirs;
    QSet<QString> seenDirs;
    auto addSearchDir = [&](const QString& path, qint64 dirBonus) {
        const QDir dir(path);
        if (!dir.exists())
            return;
        QString key = dir.canonicalPath();
        if (key.isEmpty())
            key = dir.absolutePath();
        key = QDir::cleanPath(key).toLower();
        if (seenDirs.contains(key))
            return;
        seenDirs.insert(key);
        searchDirs.append(qMakePair(dir.absolutePath(), dirBonus));
    };

    if (!bmpFolder.isEmpty())
        addSearchDir(bmpFolder, 5000);
    addSearchDir(tiffDir, 3200);
    addSearchDir(parentDir, 1800);
    addSearchDir(tiffDir + "/bmp", 3600);
    addSearchDir(tiffDir + "/gray", 3000);
    addSearchDir(tiffDir + "/grayscale", 3000);
    addSearchDir(tiffDir + "/brightness", 3000);
    addSearchDir(tiffDir + "/img", 2800);
    addSearchDir(tiffDir + "/images", 2800);
    addSearchDir(parentDir + "/bmp", 3400);
    addSearchDir(parentDir + "/gray", 2900);
    addSearchDir(parentDir + "/grayscale", 2900);
    addSearchDir(parentDir + "/brightness", 2900);
    addSearchDir(parentDir + "/img", 2600);
    addSearchDir(parentDir + "/images", 2600);

    for (const auto& dirEntry : searchDirs) {
        const QString dirKey = normalizePathKey(QDir(dirEntry.first).canonicalPath().isEmpty()
            ? QDir(dirEntry.first).absolutePath()
            : QDir(dirEntry.first).canonicalPath());
        const QStringList& exts = (!bmpFolderKey.isEmpty() && dirKey == bmpFolderKey)
            ? (useFolderExts ? folderExts : baseExts)
            : baseExts;
        evaluateDir(dirEntry.first, exts, dirEntry.second);
    }

    return cacheAndReturn(bestMatch.path);
}

QStringList TiffBmpPanel::listImageFiles(const QString& dirPath) const
{
    const QString absDirPath = QDir(dirPath).absolutePath();
    auto it = m_dirImageFilesCache.constFind(absDirPath);
    if (it != m_dirImageFilesCache.constEnd())
        return it.value();

    QDir dir(absDirPath);
    // Windows FS 不区分大小写，小写通配符已可匹配 .BMP/.PNG 等
    const QStringList files = dir.exists()
        ? dir.entryList({"*.bmp", "*.png", "*.jpg", "*.jpeg", "*.tif", "*.tiff"},
                        QDir::Files,
                        QDir::Name)
        : QStringList{};

    m_dirImageFilesCache.insert(absDirPath, files);
    return files;
}

void TiffBmpPanel::loadFile(const QString& tiffPath)
{
    if (tiffPath.isEmpty() || !QFileInfo::exists(tiffPath)) {
        m_statusLabel->setText("❌ 请先选择有效的 TIFF 文件");
        m_statusLabel->setStyleSheet("color:red;");
        return;
    }

    // ── 位深检测：在后台加载前先轻量预解析并缓存 IFD 元数据 ───────────────
    {
        TiffBmpLoader::TiffInfo info;
        int detectedDepth = 0;
        prepareTiffInfo(tiffPath, info, &detectedDepth);

        auto defaultInvalidForDepth = [](int depth) -> double {
            if (depth >= 32) return -100.0;    // 32-bit float / 128-bit
            if (depth == 17) return -32768.0;  // 16-bit signed
            return 0.0;                        // 16-bit unsigned
        };

        if (detectedDepth > 0 && m_lastBitDepth != detectedDepth) {
            QSignalBlocker blocker(m_zInvalidSpin);
            m_zInvalidSpin->setValue(defaultInvalidForDepth(detectedDepth));
        }
        if (detectedDepth > 0)
            m_lastBitDepth = detectedDepth;
    }

    ++m_requestedGeneration;
    const quint64 generation = m_requestedGeneration;

    if (m_loadWatcher && m_loadWatcher->isRunning()) {
        m_reloadPending = true;
        m_statusLabel->setText(
            QString("正在加载，已排队重新加载：%1").arg(QFileInfo(tiffPath).fileName()));
        m_statusLabel->setStyleSheet("color:#0055cc;");
        scheduleSettingsSave();
        return;
    }

    startAsyncLoad(tiffPath, generation);
}

bool TiffBmpPanel::currentModeNeedsBmp() const
{
    const DisplayMode mode = currentDisplayMode();
    return mode == DisplayMode::Brightness || mode == DisplayMode::Fusion;
}

int TiffBmpPanel::currentModeIndex() const
{
    for (int i = 0; i < 4; ++i) {
        if (m_modeBtns[i] && m_modeBtns[i]->isChecked())
            return i;
    }
    return 0;
}

DisplayMode TiffBmpPanel::currentDisplayMode() const
{
    switch (currentModeIndex()) {
        case 1:  return DisplayMode::Brightness;
        case 2:  return DisplayMode::Fusion;
        case 3:  return DisplayMode::HeightGray;
        default: return DisplayMode::Height;
    }
}

bool TiffBmpPanel::prepareTiffInfo(const QString& tiffPath,
                                   TiffBmpLoader::TiffInfo& outInfo,
                                   int* outBitDepth)
{
    outInfo = TiffBmpLoader::TiffInfo{};
    if (outBitDepth)
        *outBitDepth = 0;

    if (tiffPath.isEmpty() || !QFileInfo::exists(tiffPath))
        return false;

    auto it = m_tiffInfoCache.constFind(tiffPath);
    if (it != m_tiffInfoCache.constEnd()) {
        outInfo = it.value();
        if (outBitDepth)
            *outBitDepth = outInfo.bitDepth;
        return true;
    }

    TiffBmpLoader::TiffInfo info;
    const int detectedDepth = TiffBmpLoader::detectBitDepth(tiffPath, &info);
    info.bitDepth = detectedDepth;

    if (outBitDepth)
        *outBitDepth = detectedDepth;

    if (info.valid)
        m_tiffInfoCache.insert(tiffPath, info);

    outInfo = info;
    return true;
}

void TiffBmpPanel::scheduleSettingsSave()
{
    if (m_settingsSaveTimer)
        m_settingsSaveTimer->start();
}

void TiffBmpPanel::flushSettings()
{
    if (m_settingsSaveTimer && m_settingsSaveTimer->isActive())
        m_settingsSaveTimer->stop();
    saveSettings();
}

void TiffBmpPanel::startAsyncLoad(const QString& tiffPath, quint64 generation)
{
    if (!m_loadWatcher || tiffPath.isEmpty() || !QFileInfo::exists(tiffPath))
        return;

    TiffBmpLoader::TiffInfo preparedInfo;
    int preparedBitDepth = 0;
    prepareTiffInfo(tiffPath, preparedInfo, &preparedBitDepth);

    // 分辨率值（过小时使用默认值，并可能已在 tryParseResolutionFromFilename 中警告）
    double resX = m_resXSpin->value();
    double resY = m_resYSpin->value();
    double resZ = m_resZSpin->value();
    if (resX < 1e-10) resX = 0.01;
    if (resY < 1e-10) resY = 0.1;
    if (resZ < 1e-10) resZ = 0.0001;
    const bool   useOffset = m_useOffsetChk && m_useOffsetChk->isChecked();
    const double offX = useOffset ? m_offXSpin->value() : 0.0;
    const double offY = useOffset ? m_offYSpin->value() : 0.0;
    const double offZ = useOffset ? m_offZSpin->value() : 0.0;

    const DisplayMode mode = currentDisplayMode();
    const bool needsBmp = (mode == DisplayMode::Brightness || mode == DisplayMode::Fusion);
    const float  alpha     = m_alphaSlider->value() / 100.f;
    const float  colorMin  = m_colorMinSlider->value() / 100.f;
    const float  colorMax  = m_colorMaxSlider->value() / 100.f;
    const bool   buildMesh = m_meshChk->isChecked();
    const bool   computeNormals = buildMesh;
    const bool   rotateZ90  = m_rotateZ90Chk->isChecked();
    const bool   rotateZ180 = m_rotateZ180Chk->isChecked();
    // 等步长降采样 stride：保留行号为 stride 整数倍的行，间距均匀，网格可正常连接
    int yStride = 1;
    switch (m_yDsSampleCb->currentIndex()) {
    case 1: yStride = 2;  break;
    case 2: yStride = 3;  break;
    case 3: yStride = 5;  break;
    case 4: yStride = 10; break;
    case 5: yStride = 20; break;
    default: break; // index 0: 全采 stride=1
    }
    const double zInvalid   = m_zInvalidSpin->value();
    const QString bmpPath   = needsBmp ? m_bmpEdit->text().trimmed() : QString();
    const bool   useSynth   = needsBmp && bmpPath.isEmpty();
    const QString autoMatchedBmp = needsBmp ? autoFindBmp(tiffPath) : QString();
    auto normalizePath = [](const QString& path) -> QString {
        if (path.isEmpty())
            return {};
        return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toLower();
    };
    const bool autoMatchedBmpUsed = needsBmp
        && !bmpPath.isEmpty()
        && !autoMatchedBmp.isEmpty()
        && normalizePath(bmpPath) == normalizePath(autoMatchedBmp);
    const bool embeddedBrightnessUsed = needsBmp
        && bmpPath.isEmpty()
        && preparedInfo.valid
        && preparedInfo.is2Channel;
    const bool   rmIslands     = m_removeIslandsChk->isChecked();
    const int    minIsland     = m_minIslandSpin->value();
    const int    minIslandDist = m_minIslandDistSpin->value();
    const double maxZGap       = m_maxZGapSpin->value();
    const double maxEdge       = m_maxEdgeSpin->value();

    // ── 在 UI 线程预分配 UID，避免后台线程调用全局非线程安全的 GetNextUniqueID() ──
    // ccPointCloud / ccMesh 构造函数有 uniqueID 参数：传入预分配的 ID 后，
    // 构造函数跳过内部的 GetNextUniqueID() 调用，消除与 UI 线程的数据竞争。
    const unsigned cloudUID = ccObject::GetNextUniqueID();
    const unsigned meshUID  = buildMesh
                              ? ccObject::GetNextUniqueID()
                              : static_cast<unsigned>(ccUniqueIDGenerator::InvalidUniqueID);

    m_runningGeneration = generation;
    m_reloadPending = false;
    m_statusLabel->setText(QString("正在后台加载：%1").arg(QFileInfo(tiffPath).fileName()));
    m_statusLabel->setStyleSheet("color:#0055cc;");
    if (m_loadButton) {
        m_loadButton->setEnabled(false);
        m_loadButton->setText("加载中...");
    }

    m_loadWatcher->setFuture(QtConcurrent::run([=]() -> AsyncLoadResult {
        AsyncLoadResult result;
        result.generation        = generation;
        result.tiffPath          = tiffPath;
        result.bmpPath           = bmpPath;
        result.displayMode       = mode;
        result.bitDepth          = preparedBitDepth;
        result.buildMeshUsed     = buildMesh;
        result.autoMatchedBmpUsed = autoMatchedBmpUsed;
        result.embeddedBrightnessUsed = embeddedBrightnessUsed;
        result.syntheticBmpUsed  = useSynth && !embeddedBrightnessUsed;
        result.removeIslandsUsed = rmIslands;
        result.computeNormalsUsed = computeNormals; // 标记"已请求法线"，实际计算在 applyLoadResult
        result.rotateZ90  = rotateZ90;   // 旋转也在 UI 线程的 applyLoadResult 中执行
        result.rotateZ180 = rotateZ180;

        TiffBmpLoader::TiffInfo infoCopy = preparedInfo;
        int bitDepth = preparedBitDepth;
        result.object = TiffBmpLoader::load(
            tiffPath, bmpPath,
            resX, resY, resZ,
            mode, alpha, buildMesh,
            colorMin, colorMax,
            zInvalid, useSynth,
            &bitDepth,
            rmIslands, minIsland, minIslandDist,
            maxZGap, maxEdge,
            false,       // computeNormals: 不在后台线程计算（ccNormalVectors 单例初始化非线程安全）
            offX, offY, offZ,
            useOffset,   // reinterpretAsSignedZ: uint16 视为有符号（仅启用偏移时生效）
            infoCopy.valid ? &infoCopy : nullptr,
            &result.error,
            yStride,
            cloudUID,    // 预分配 UID，绕过后台线程中的 GetNextUniqueID() 调用
            meshUID);
        result.bitDepth = bitDepth;
        // 注意：旋转和法线计算已移至 UI 线程 applyLoadResult() 中执行
        return result;
    }));

    scheduleSettingsSave();
}

void TiffBmpPanel::onAsyncLoadFinished()
{
    if (!m_loadWatcher)
        return;

    const AsyncLoadResult result = m_loadWatcher->future().result();
    m_runningGeneration = 0;

    applyLoadResult(result);

    const bool needsRestart = m_reloadPending || m_requestedGeneration > result.generation;
    const QString currentTiff = m_tiffEdit->text().trimmed();
    if (needsRestart && !currentTiff.isEmpty() && QFileInfo::exists(currentTiff)) {
        m_reloadPending = false;
        startAsyncLoad(currentTiff, m_requestedGeneration);
        return;
    }

    m_reloadPending = false;
    if (m_loadButton) {
        m_loadButton->setEnabled(true);
        m_loadButton->setText("加  载");
    }
}

void TiffBmpPanel::applyLoadResult(const AsyncLoadResult& result)
{
    // 追踪最近加载的路径（QC 模式判定逻辑读取）
    m_lastLoadedTiff = result.tiffPath;
    m_lastLoadedBmp  = result.bmpPath;

    if (result.generation < m_requestedGeneration) {
        delete result.object;
        return;
    }

    m_handledGeneration = std::max(m_handledGeneration, result.generation);
    if (result.bitDepth > 0)
        m_lastBitDepth = result.bitDepth;

    if (!result.object) {
        m_statusLabel->setText("❌ " + (result.error.isEmpty()
            ? ls("加载失败（未知错误）", "Load failed (unknown error)") : result.error));
        m_statusLabel->setStyleSheet("color:red;");
        return;
    }

    ccHObject* object = result.object;

    // ── 旋转（UI 线程，线程安全）────────────────────────────────────────────
    // 原来在后台 lambda 中执行；移至此处确保线程安全，且在法线计算之前
    {
        auto applyRotZ = [&](float angle_rad) {
            ccGLMatrix rot;
            rot.initFromParameters(angle_rad, CCVector3(0.f, 0.f, 1.f), CCVector3(0.f, 0.f, 0.f));
            if (auto* mesh = dynamic_cast<ccMesh*>(object)) {
                auto* verts = dynamic_cast<ccPointCloud*>(mesh->getAssociatedCloud());
                if (verts) { verts->applyRigidTransformation(rot); mesh->refreshBB(); }
            } else if (auto* cloud = dynamic_cast<ccPointCloud*>(object)) {
                cloud->applyRigidTransformation(rot);
            }
        };
        if (result.rotateZ90)
            applyRotZ(static_cast<float>(M_PI / 2.0));
        if (result.rotateZ180)
            applyRotZ(static_cast<float>(M_PI));
    }

    // ── 法线计算（UI 线程，线程安全）────────────────────────────────────────
    // ccNormalVectors 单例首次初始化非线程安全，必须在 UI 线程执行
    if (result.computeNormalsUsed) {
        if (auto* mesh = dynamic_cast<ccMesh*>(object)) {
            if (mesh->computeNormals(true))
                mesh->showNormals(true);
            else
                mesh->showNormals(false);
        }
    }

    // 用 TIFF 文件名（不含扩展名）作为对象名，在 CC 数据库树中清晰可辨
    object->setName(QFileInfo(result.tiffPath).completeBaseName());

    ccGLWindowInterface* glWin = m_app->getActiveGLWindow();
    const bool keepMode = m_keepObjectChk && m_keepObjectChk->isChecked();
    const bool isReload = (m_lastUID != 0);
    ccViewportParameters savedVP;
    ccBBox oldBB;
    bool hasOldBB = false;

    if (isReload && glWin) {
        savedVP = glWin->getViewportParameters();
        // 移除旧对象前记录包围盒，用于中心偏移判断
        ccHObject* oldObj = m_app->dbRootObject()->find(m_lastUID);
        if (oldObj) {
            oldBB    = oldObj->getOwnBB();
            hasOldBB = oldBB.isValid();
        }
    }

    // ── 提前决定视角（在 refreshAll 之前设置好，第一帧就显示在正确位置）──
    // 首次加载必须重置；重载时判断中心偏移是否超过旧幅面长边的 1/3
    bool shouldReset = !isReload;
    if (isReload && hasOldBB) {
        const ccBBox    newBB     = object->getOwnBB();
        const CCVector3 oldCenter = oldBB.getCenter();
        const CCVector3 newCenter = newBB.getCenter();
        const CCVector3 oldDiag   = oldBB.getDiagVec();
        const float threshold = std::max(oldDiag.x, oldDiag.y) * (1.f / 3.f);
        const float dx = newCenter.x - oldCenter.x;
        const float dy = newCenter.y - oldCenter.y;
        shouldReset = (dx*dx + dy*dy > threshold * threshold);
    }

    // 加入新对象（不 zoom、不 redraw）
    m_app->addToDB(object,
        /*updateZoom=*/false,
        /*autoExpandDBTree=*/true,
        /*checkDimensions=*/false,
        /*autoRedraw=*/false);

    // ── 视角在此设置（在 removeFromDB 之前，避免内部重绘闪现旧视角）────
    if (glWin) {
        if (shouldReset) {
            // 首次加载：优先尝试恢复 INI 存档的视角
            bool restoredFromIni = false;
            if (!isReload && m_firstLoad) {
                const QString configPath =
                    QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/LmiTiffBmpPlugin.ini";
                QSettings s(configPath, QSettings::IniFormat);
                restoredFromIni = restoreViewportFromSettings(s, glWin);
            }
            if (!restoredFromIni) {
                glWin->setView(CC_TOP_VIEW);
                ccBBox bb = object->getOwnBB();
                glWin->updateConstellationCenterAndZoom(&bb);
            }
        } else {
            glWin->setViewportParameters(savedVP);
        }
    }
    if (!isReload)
        m_firstLoad = false;

    // 删除旧对象（视角已就绪，内部重绘不会再闪）
    // 保留模式下：旧对象留在 CC 数据库中，由用户自行管理
    if (m_lastUID != 0) {
        if (!keepMode) {
            ccHObject* old = m_app->dbRootObject()->find(m_lastUID);
            if (old)
                m_app->removeFromDB(old, true);
        }
        m_lastUID = 0;
    }

    m_lastUID = object->getUniqueID();
    m_app->setSelectedInDB(object, true);
    m_app->refreshAll();
    m_app->updateUI();

    m_fileNameLabel->setText(QFileInfo(result.tiffPath).fileName());

    const auto displayModeLabel = [this](DisplayMode mode) -> QString {
        switch (mode) {
        case DisplayMode::Brightness: return ls("亮度", "Brightness");
        case DisplayMode::Fusion:     return ls("融合", "Fusion");
        case DisplayMode::HeightGray: return ls("高度灰阶", "HeightGray");
        case DisplayMode::Height:
        default:                      return ls("高度", "Height");
        }
    };

    const QString bitInfo   = (result.bitDepth == 128) ? ls(" [128bit双通道]", " [128-bit 2-ch]")
                            : (result.bitDepth == 32)  ? QStringLiteral(" [32bit]")
                            : (result.bitDepth == 17)  ? ls(" [16bit有符号]", " [16-bit signed]")
                            :                            QStringLiteral(" [16bit]");
    QString brightnessInfo;
    if (result.displayMode == DisplayMode::Brightness || result.displayMode == DisplayMode::Fusion) {
        if (result.embeddedBrightnessUsed) {
            brightnessInfo = ls("（内嵌亮度通道）", "(embedded brightness)");
        } else if (result.syntheticBmpUsed) {
            brightnessInfo = ls("（合成亮度）", "(synthetic brightness)");
        } else if (!result.bmpPath.isEmpty()) {
            const QString bmpName = QFileInfo(result.bmpPath).fileName();
            brightnessInfo = result.autoMatchedBmpUsed
                ? QString(ls("（自动匹配亮度图：%1）", "(auto-matched BMP: %1)")).arg(bmpName)
                : QString(ls("（亮度图：%1）", "(BMP: %1)")).arg(bmpName);
        }
    }
    const QString islandInfo  = result.removeIslandsUsed
        ? ls("（已过滤小岛）", "(noise filtered)") : QString();
    const QString normalsInfo = (dynamic_cast<ccMesh*>(object) && result.computeNormalsUsed)
        ? ls("（平滑法线）", "(smooth normals)") : QString();
    const QString modeInfo = QString(ls("（%1模式）", "(%1 mode)"))
        .arg(displayModeLabel(result.displayMode));

    auto* mesh  = dynamic_cast<ccMesh*>(object);
    auto* cloud = dynamic_cast<ccPointCloud*>(object);
    if (mesh) {
        m_statusLabel->setText(
            QString(ls("✓ 网格 %L1 三角形%2%3%4%5%6", "✓ Mesh %L1 tris%2%3%4%5%6"))
                .arg(mesh->size()).arg(bitInfo).arg(modeInfo).arg(brightnessInfo).arg(islandInfo)
                .arg(normalsInfo));
    } else if (cloud) {
        m_statusLabel->setText(
            QString(ls("✓ 点云 %L1 个点%2%3%4%5", "✓ Cloud %L1 pts%2%3%4%5"))
                .arg(cloud->size()).arg(bitInfo).arg(modeInfo).arg(brightnessInfo).arg(islandInfo));
    } else {
        m_statusLabel->setText(ls("✓ 已加载", "✓ Loaded") + bitInfo + modeInfo
                               + brightnessInfo + islandInfo);
    }
    m_statusLabel->setStyleSheet("color:green;");

    scheduleSettingsSave();
}

// ─────────────────────────────────────────────────────────────────────────────
// 语言切换
// ─────────────────────────────────────────────────────────────────────────────

QString TiffBmpPanel::ls(const char* zh, const char* en) const
{
    return m_langEn ? QString::fromUtf8(en) : QString::fromUtf8(zh);
}

void TiffBmpPanel::retranslateUi()
{
    if (!m_langBtnZh) return; // 构造尚未完成

    m_langBtnZh->setChecked(!m_langEn);
    m_langBtnEn->setChecked( m_langEn);
    if (m_lblLangText) m_lblLangText->setText(ls("界面语言:", "Language:"));

    // 文件路径区
    if (m_fileGrp)         m_fileGrp->setTitle(ls("文件路径", "File Path"));
    if (m_lblBmpRow)       m_lblBmpRow->setText(ls("亮度图:", "Brightness:"));
    if (m_lblBmpFolderRow) m_lblBmpFolderRow->setText(ls("亮度图文件夹:", "Brightness Folder:"));
    m_tiffEdit->setPlaceholderText(ls("选择或拖拽 TIFF 文件...", "Select or drag TIFF file..."));
    m_bmpEdit->setPlaceholderText(ls("自动匹配 / 手动选择亮度图...", "Auto-match / select brightness..."));
    m_bmpFolderEdit->setPlaceholderText(ls("亮度图所在文件夹（可选）...", "Brightness folder (optional)..."));
    if (m_keepObjectChk)
        m_keepObjectChk->setText(ls("保留为独立对象（勾选后需手动点击加载）",
                                    "Keep as separate object (manual load)"));

    // 分辨率区
    if (m_resGrp)       m_resGrp->setTitle(ls("物理分辨率 (mm/pixel)", "Resolution (mm/pixel)"));
    m_autoResChk->setText(ls("自动从文件名读取分辨率", "Auto-detect from filename"));
    if (m_useOffsetChk) m_useOffsetChk->setText(ls("启用偏移", "Enable Offset"));
    if (m_lblResHdr)    m_lblResHdr->setText(ls("分辨率", "Resolution"));
    if (m_offHdrLabel)  m_offHdrLabel->setText(ls("偏移", "Offset"));
    if (m_lblZInvalidRow) m_lblZInvalidRow->setText(ls("Z 无效值:", "Z Invalid:"));

    // 颜色渲染范围区
    if (m_colorGrp)       m_colorGrp->setTitle(ls("颜色渲染范围", "Color Range"));
    if (m_lblColorMinRow) m_lblColorMinRow->setText(ls("下限:", "Min:"));
    if (m_lblColorMaxRow) m_lblColorMaxRow->setText(ls("上限:", "Max:"));

    // 显示模式区
    if (m_modeGrp) m_modeGrp->setTitle(ls("显示模式", "Display Mode"));
    m_modeBtns[0]->setText(ls("高度\n色彩",  "Height\nColor"));
    m_modeBtns[1]->setText(ls("亮度\n灰度",  "Bright-\nness"));
    m_modeBtns[2]->setText(ls("融合\n模式",  "Fu-\nsion"));
    m_modeBtns[3]->setText(ls("高度\n灰阶",  "Height\nGray"));
    if (m_lblAlphaText)   m_lblAlphaText->setText(ls("融合系数 α:", "Fusion Alpha:"));
    m_meshChk->setText(ls("生成三角网格（有序网格直接 mesh 化）", "Generate mesh (ordered grid)"));
    if (m_lblMaxEdgeText) m_lblMaxEdgeText->setText(ls("最大边长:", "Max Edge:"));
    m_rotateZ90Chk->setText(ls("绕Z+旋转90°",  "Rotate Z+ 90°"));
    m_rotateZ180Chk->setText(ls("绕Z+旋转180°", "Rotate Z+ 180°"));
    if (m_lblYDsText) m_lblYDsText->setText(ls("Y降采:", "Y Sub:"));
    {
        const int cur = m_yDsSampleCb->currentIndex();
        QSignalBlocker blk(m_yDsSampleCb);
        m_yDsSampleCb->clear();
        m_yDsSampleCb->addItem(ls("全采", "Full"));
        m_yDsSampleCb->addItem("1/2");
        m_yDsSampleCb->addItem("1/3");
        m_yDsSampleCb->addItem("1/5");
        m_yDsSampleCb->addItem("1/10");
        m_yDsSampleCb->addItem("1/20");
        m_yDsSampleCb->setCurrentIndex(cur);
    }

    // 噪声过滤区
    if (m_noiseGrp)       m_noiseGrp->setTitle(ls("噪声过滤", "Noise Filter"));
    m_removeIslandsChk->setText(ls("移除孤岛噪声", "Remove Island Noise"));
    if (m_lblNoiseMinPx) m_lblNoiseMinPx->setText(ls("最小点数:", "Min Pixels:"));
    if (m_lblNoiseZGap)  m_lblNoiseZGap->setText(ls("最大Z跳变:", "Max Z Gap:"));

    // 图像导航区
    if (m_navGrp)      m_navGrp->setTitle(ls("图像导航（文件夹）", "Image Navigation (Folder)"));
    if (m_lblSortText) m_lblSortText->setText(ls("排序方式:", "Sort:"));
    {
        const int cur = m_sortOrderCb->currentIndex();
        QSignalBlocker blk(m_sortOrderCb);
        m_sortOrderCb->clear();
        m_sortOrderCb->addItem(ls("按文件名", "By Name"));
        m_sortOrderCb->addItem(ls("按修改时间（旧→新）", "By Date (Old\xe2\x86\x92New)"));
        m_sortOrderCb->addItem(ls("按修改时间（新→旧）", "By Date (New\xe2\x86\x92Old)"));
        m_sortOrderCb->setCurrentIndex(cur);
    }

    // 加载按钮
    m_loadButton->setText(ls("加  载", "  Load  "));

    // QC 模式
    if (m_qcGrp)          m_qcGrp->setTitle(ls("判定输出路径", "QC Output Paths"));
    if (m_lblOkFolderRow) m_lblOkFolderRow->setText(ls("OK 文件夹:", "OK Folder:"));
    if (m_lblNgFolderRow) m_lblNgFolderRow->setText(ls("NG 文件夹:", "NG Folder:"));

    // 状态标签：仅在显示默认就绪文字时同步翻译
    const QString readyZh = QString::fromUtf8("就绪 — 支持拖拽 TIFF 文件");
    const QString readyEn = QString::fromUtf8("Ready — drag & drop TIFF files");
    if (m_statusLabel->text() == readyZh || m_statusLabel->text() == readyEn)
        m_statusLabel->setText(ls("就绪 — 支持拖拽 TIFF 文件", "Ready — drag & drop TIFF files"));

    // 文件名标签：仅在显示默认"未加载"文字时同步翻译
    const QString notLoadedZh = QString::fromUtf8("（未加载）");
    const QString notLoadedEn = QString::fromUtf8("(not loaded)");
    if (m_fileNameLabel->text() == notLoadedZh || m_fileNameLabel->text() == notLoadedEn)
        m_fileNameLabel->setText(ls("（未加载）", "(not loaded)"));
}

// ─────────────────────────────────────────────────────────────────────────────
// 配置文件读写
// ─────────────────────────────────────────────────────────────────────────────

void TiffBmpPanel::loadSettings()
{
    QSettings s(pluginSettingsPath(), QSettings::IniFormat);

    const int savedVer = s.value("panelDefaultsVersion", 0).toInt();
    if (savedVer < kPanelDefaultsVersion) {
        s.setValue("mesh", true);
        s.setValue("panelDefaultsVersion", kPanelDefaultsVersion);
        s.sync();
    }
    s.remove("computeNormals");

    // 逐控件读取，完成后手动更新依赖状态
    m_resXSpin->setValue(s.value("resX", 0.0069).toDouble());
    m_resYSpin->setValue(s.value("resY", 0.015).toDouble());
    m_resZSpin->setValue(s.value("resZ", 0.0001).toDouble());
    if (!m_qcMode) {
        // 先静默设值，再恢复复选框（toggled 信号负责显隐）
        QSignalBlocker bx(m_offXSpin), by(m_offYSpin), bz(m_offZSpin);
        m_offXSpin->setValue(s.value("offX", 0.0).toDouble());
        m_offYSpin->setValue(s.value("offY", 0.0).toDouble());
        m_offZSpin->setValue(s.value("offZ", 0.0).toDouble());
        m_useOffsetChk->setChecked(s.value("useOffset", false).toBool());
    }
    m_zInvalidSpin->setValue(s.value("zInvalid", 0.0).toDouble());
    m_autoResChk->setChecked(s.value("autoRes", false).toBool());

    m_colorMinSlider->setValue(s.value("colorMin", 0).toInt());
    m_colorMaxSlider->setValue(s.value("colorMax", 100).toInt());
    m_colorMinLabel->setText(QString::number(m_colorMinSlider->value()) + " %");
    m_colorMaxLabel->setText(QString::number(m_colorMaxSlider->value()) + " %");

    // 恢复显示模式（同时更新按钮选中态和融合行可见性）
    onDisplayModeChanged(qBound(0, s.value("displayMode", 0).toInt(), 3));

    m_alphaSlider->setValue(s.value("alpha", 50).toInt());
    m_alphaLabel->setText(QString::number(m_alphaSlider->value() / 100.0, 'f', 2));

    m_meshChk->setChecked(s.value("mesh", true).toBool());
    if (m_keepObjectChk)
        m_keepObjectChk->setChecked(s.value("keepObject", false).toBool());
    if (m_qcMode && m_okFolderEdit) {
        m_okFolderEdit->setText(s.value("QC/okFolder", "").toString());
        m_ngFolderEdit->setText(s.value("QC/ngFolder", "").toString());
    }
    m_maxEdgeSpin->setValue(s.value("maxEdge", 0.1).toDouble());
    m_rotateZ90Chk->setChecked(s.value("rotateZ90",  false).toBool());
    m_rotateZ180Chk->setChecked(s.value("rotateZ180", false).toBool());
    m_yDsSampleCb->setCurrentIndex(qBound(0, s.value("yDownsample", 0).toInt(), 5));
    m_removeIslandsChk->setChecked(s.value("removeIslands", false).toBool());
    m_minIslandSpin->setValue(s.value("minIslandPixels", 100).toInt());
    m_minIslandDistSpin->setValue(s.value("minIslandDist", 0).toInt());
    m_maxZGapSpin->setValue(s.value("maxZGap", 0.0).toDouble());
    m_minIslandSpin->setEnabled(m_removeIslandsChk->isChecked());
    m_maxZGapSpin->setEnabled(m_removeIslandsChk->isChecked());

    m_sortOrderCb->setCurrentIndex(s.value("sortOrder", 0).toInt());
    m_langEn = (s.value("UI/lang", "zh").toString() == "en");

    // 路径最后设置（会触发 onTiffPathChanged → scanFolder 等）
    const QString bmpFolder = s.value("bmpFolder", "").toString();
    if (!bmpFolder.isEmpty())
        m_bmpFolderEdit->setText(bmpFolder);

    const QString bmpPath = s.value("bmpPath", "").toString();
    if (!bmpPath.isEmpty())
        m_bmpEdit->setText(bmpPath);

    const QString tiffPath = s.value("tiffPath", "").toString();
    if (!tiffPath.isEmpty() && QFileInfo::exists(tiffPath))
        m_tiffEdit->setText(tiffPath);

    retranslateUi();
}

// ─────────────────────────────────────────────────────────────────────────────
// 视角保存 / 恢复
// ─────────────────────────────────────────────────────────────────────────────

void TiffBmpPanel::saveViewportNow()
{
    ccGLWindowInterface* glWin = m_app ? m_app->getActiveGLWindow() : nullptr;
    if (!glWin) return;

    QSettings s(pluginSettingsPath(), QSettings::IniFormat);

    const ccViewportParameters& vp = glWin->getViewportParameters();

    // 旋转矩阵（16 个 double，以逗号分隔存储）
    const double* m = vp.viewMat.data();
    QStringList matList;
    for (int i = 0; i < 16; ++i)
        matList << QString::number(m[i], 'g', 17);
    s.setValue("vp/viewMat", matList.join(','));

    // 相机/枢轴参数
    const CCVector3d& piv = vp.getPivotPoint();
    s.setValue("vp/pivotX",  piv.x);
    s.setValue("vp/pivotY",  piv.y);
    s.setValue("vp/pivotZ",  piv.z);
    const CCVector3d& cam = vp.getCameraCenter();
    s.setValue("vp/camX",    cam.x);
    s.setValue("vp/camY",    cam.y);
    s.setValue("vp/camZ",    cam.z);
    s.setValue("vp/focal",   vp.getFocalDistance());

    // 投影 / 其它
    s.setValue("vp/perspective",     vp.perspectiveView);
    s.setValue("vp/objectCentered",  vp.objectCenteredView);
    s.setValue("vp/pointSize",       (double)vp.defaultPointSize);
    s.setValue("vp/fov",             (double)vp.fov_deg);
    s.setValue("vp/zNearCoef",       vp.zNearCoef);
    s.setValue("vp/saved",           true);
    s.sync();
}

static bool restoreViewportFromSettings(QSettings& s, ccGLWindowInterface* glWin)
{
    if (!s.value("vp/saved", false).toBool()) return false;

    const QStringList matList = s.value("vp/viewMat").toString().split(',');
    if (matList.size() != 16) return false;

    double mat16[16];
    for (int i = 0; i < 16; ++i)
        mat16[i] = matList[i].toDouble();

    ccViewportParameters vp = glWin->getViewportParameters(); // 先拷贝当前值
    vp.viewMat = ccGLMatrixd(mat16);

    CCVector3d piv(s.value("vp/pivotX").toDouble(),
                   s.value("vp/pivotY").toDouble(),
                   s.value("vp/pivotZ").toDouble());
    CCVector3d cam(s.value("vp/camX").toDouble(),
                   s.value("vp/camY").toDouble(),
                   s.value("vp/camZ").toDouble());
    vp.setPivotPoint(piv, false);
    vp.setCameraCenter(cam, false);
    vp.setFocalDistance(s.value("vp/focal").toDouble());

    vp.perspectiveView    = s.value("vp/perspective",    false).toBool();
    vp.objectCenteredView = s.value("vp/objectCentered", true).toBool();
    vp.defaultPointSize   = (float)s.value("vp/pointSize", 2.0).toDouble();
    vp.fov_deg            = (float)s.value("vp/fov",      60.0).toDouble();
    vp.zNearCoef          = s.value("vp/zNearCoef", 0.005).toDouble();

    glWin->setViewportParameters(vp);
    glWin->setPerspectiveState(vp.perspectiveView, vp.objectCenteredView);
    glWin->redraw();
    return true;
}

void TiffBmpPanel::saveSettings()
{
    QSettings s(pluginSettingsPath(), QSettings::IniFormat);

    s.setValue("tiffPath",        m_tiffEdit->text());
    s.setValue("bmpPath",         m_bmpEdit->text());
    s.setValue("bmpFolder",       m_bmpFolderEdit->text());
    s.setValue("autoRes",         m_autoResChk->isChecked());
    s.setValue("resX",            m_resXSpin->value());
    s.setValue("resY",            m_resYSpin->value());
    s.setValue("resZ",            m_resZSpin->value());
    if (!m_qcMode) {
        s.setValue("useOffset",   m_useOffsetChk->isChecked());
        s.setValue("offX",        m_offXSpin->value());
        s.setValue("offY",        m_offYSpin->value());
        s.setValue("offZ",        m_offZSpin->value());
    }
    s.setValue("zInvalid",        m_zInvalidSpin->value());
    s.setValue("colorMin",        m_colorMinSlider->value());
    s.setValue("colorMax",        m_colorMaxSlider->value());
    { int mi = 0; for (int i = 0; i < 4; ++i) if (m_modeBtns[i]->isChecked()) { mi = i; break; }
      s.setValue("displayMode", mi); }
    s.setValue("alpha",           m_alphaSlider->value());
    s.setValue("mesh",            m_meshChk->isChecked());
    if (m_keepObjectChk)
        s.setValue("keepObject",  m_keepObjectChk->isChecked());
    if (m_qcMode && m_okFolderEdit) {
        s.setValue("QC/okFolder", m_okFolderEdit->text());
        s.setValue("QC/ngFolder", m_ngFolderEdit->text());
    }
    s.setValue("maxEdge",         m_maxEdgeSpin->value());
    s.setValue("rotateZ90",       m_rotateZ90Chk->isChecked());
    s.setValue("rotateZ180",      m_rotateZ180Chk->isChecked());
    s.setValue("yDownsample",     m_yDsSampleCb->currentIndex());
    s.setValue("removeIslands",   m_removeIslandsChk->isChecked());
    s.setValue("minIslandPixels", m_minIslandSpin->value());
    s.setValue("minIslandDist",   m_minIslandDistSpin->value());
    s.setValue("maxZGap",         m_maxZGapSpin->value());
    s.setValue("sortOrder",       m_sortOrderCb->currentIndex());
    s.setValue("UI/lang",         m_langEn ? "en" : "zh");
    s.setValue("panelDefaultsVersion", kPanelDefaultsVersion);
    s.remove("computeNormals");
    s.sync();
}

// ── QC 模式公共方法 ────────────────────────────────────────────────────────

QString TiffBmpPanel::okFolderPath() const
{
    return m_okFolderEdit ? m_okFolderEdit->text().trimmed() : QString();
}

QString TiffBmpPanel::ngFolderPath() const
{
    return m_ngFolderEdit ? m_ngFolderEdit->text().trimmed() : QString();
}

void TiffBmpPanel::goNext()
{
    onNext();
}
