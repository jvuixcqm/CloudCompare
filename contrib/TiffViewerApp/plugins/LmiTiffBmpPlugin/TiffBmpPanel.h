// Copyright (c) 2026 LMI Technologies Inc. All rights reserved.
// Author: Jim Wang, LMI Technologies
#pragma once

#include <QHash>
#include <QWidget>
#include <QStringList>

template <typename T>
class QFutureWatcher;

#include "TiffBmpLoader.h"

class QLineEdit;
class QDoubleSpinBox;
class QGroupBox;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QSlider;
class QLabel;
class QPushButton;
class QTimer;
class QScrollArea;
class ccMainAppInterface;

//! 嵌入 CloudCompare 侧栏的 TIFF/BMP 点云浏览面板
class TiffBmpPanel : public QWidget
{
    Q_OBJECT

public:
    static constexpr const char* kVersion     = "2.3";
    static constexpr int kPanelFixedWidth = 392;
    static constexpr int kDockFixedWidth  = 408;

    explicit TiffBmpPanel(ccMainAppInterface* app, QWidget* parent = nullptr,
                          bool qcMode = false);
    ~TiffBmpPanel() override;

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private slots:
    void onBrowseTiff();
    void onExportCurrent();   //!< 导出当前载入的 3D 数据为 TIFF
    void onExportAll();       //!< 循环导出当前文件夹的全部 TIFF / SRF
    void onBrowseBmp();
    void onBrowseBmpFolder();
    void onTiffPathChanged(const QString& path);
    void onDisplayModeChanged(int index);
    void onColorMinChanged(int val);
    void onColorMaxChanged(int val);
    void onSortOrderChanged(int index);
    void onLoad();
    void onFirst();
    void onPrev();
    void onNext();
    void onLast();
    void onRefreshDir(); //!< F5: 仅刷新目录，不跳转
    void onAsyncLoadFinished();

private:
    struct AsyncLoadResult
    {
        quint64    generation        = 0;
        QString    tiffPath;
        QString    bmpPath;
        DisplayMode displayMode      = DisplayMode::Height;
        int        bitDepth          = 0;
        bool       buildMeshUsed     = false;
        bool       autoMatchedBmpUsed = false;
        bool       embeddedBrightnessUsed = false;
        bool       syntheticBmpUsed  = false;
        bool       removeIslandsUsed = false;
        bool       computeNormalsUsed = false; //!< 法线被请求（在 applyLoadResult 中执行）
        bool       rotateZ90         = false;  //!< 在 applyLoadResult 中绕 Z 旋转 90°
        bool       rotateZ180        = false;  //!< 在 applyLoadResult 中绕 Z 旋转 180°
        int        imageWidth        = 0;      //!< 加载图的像素宽（用于尺寸突变判断）
        int        imageHeight       = 0;      //!< 加载图的像素高
        bool       fallbackToUnorganizedCloud = false; //!< PLY/PCD 非规则网格，已降级为普通无序点云
        ccHObject* object            = nullptr;
        QString    error;
    };

    void tryParseResolutionFromFilename(const QString& path);
    //! 优先用 TIFF 嵌入元数据（ImageDescription / ModelPixelScale / XResolution）
    //! 设置分辨率和偏移；返回 true 表示已应用任意嵌入字段（调用方可跳过文件名解析）
    bool tryApplyEmbeddedMetadata(const QString& path);
    void scanFolder(const QString& tiffPath, bool forceRefresh = false);
    void updateNavUI();
    QString autoFindBmp(const QString& tiffPath) const;
    QStringList listImageFiles(const QString& dirPath) const;
    void activateFileAtIndex(int index);
    void loadFile(const QString& tiffPath);
    void triggerAutoReload();   //!< 如果 TIFF 有效则启动防抖定时器
    void loadSettings();        //!< 从配置文件读取所有控件值
    void saveSettings();        //!< 将所有控件值写入配置文件
    void scheduleSettingsSave();
    void flushSettings();
    void startAsyncLoad(const QString& tiffPath, quint64 generation);
    void applyLoadResult(const AsyncLoadResult& result);
    //! 应用/刷新"Z 显示放大"：仅影响渲染（ccGLTransformation，Z 轴对角线缩放），
    //! 不改变对象的真实坐标数据——测距/点坐标读数仍反映原始 mm 值。
    //! 倍数为 1 时调用 resetGLTransformation() 还原。
    void applyZDisplayScale(ccHObject* obj);
    bool currentModeNeedsBmp() const;
    int currentModeIndex() const;
    DisplayMode currentDisplayMode() const;
    bool prepareTiffInfo(const QString& tiffPath,
                         TiffBmpLoader::TiffInfo& outInfo,
                         int* outBitDepth = nullptr);
    void retranslateUi();                              //!< 按 m_langEn 更新所有控件文字
    QString ls(const char* zh, const char* en) const; //!< 返回当前语言字符串

    //! 根据当前选中的文件后缀切换 SRF/PLY/PCD（网格类）与 TIFF 模式 UI：
    //!  - SRF/PLY/PCD：禁用分辨率组（仅显示）、亮度图/亮度图文件夹及其浏览按钮；
    //!         清空亮度图路径；SRF 额外把嵌入的 scale_x/y/z 写入对应分辨率框
    //!         （QSignalBlocker 避免触发重载）——PLY/PCD 无法在不整档解析时预知分辨率，跳过该步
    //!  - 其余（TIFF）：恢复上述全部控件
    //! @return true=该路径是 SRF/PLY/PCD（面板控件已按网格类格式禁用）
    bool applyGridFormatModeFromPath(const QString& path);

    //! 从源文件名提取干净的"基名"——若已是
    //! "xxx_(16位无符号|16位有符号|32位)TIFF_X_Y_Z.tif" 形式，只取 xxx
    QString extractExportStem(const QString& srcPath) const;

    //! 把单个源文件（TIFF / SRF）按当前 UI 设置导出为目标格式
    //! @param meta XYZ 分辨率/偏移（mm，由调用方决定使用面板值还是源文件解析值）
    //! @return true=成功
    bool exportSourceTo(const QString& srcPath,
                        const QString& outDir,
                        int format,           //!< 0=16U, 1=16S, 2=32F
                        float invalidFill32,  //!< 仅 format==2 时使用
                        const TiffBmpLoader::ExportTiffMeta& meta,
                        QString* outError = nullptr);

    //! 解析源文件 mm 单位的 XYZ 分辨率/偏移（用于批量导出每个文件单独的元数据）：
    //!   1) SRF 头部；2) TIFF 嵌入元数据；3) 文件名末尾 _X_Y_Z 模式；4) 面板当前值
    TiffBmpLoader::ExportTiffMeta resolveExportMetaForFile(const QString& srcPath) const;

    //! 取面板当前显示的 mm 值（适合 onExportCurrent 用——反映"输入时的 mm 数值"）
    TiffBmpLoader::ExportTiffMeta currentPanelMeta() const;

    //! 确保导出输出目录存在；自动追加"转换TIFF输出目录"子目录
    //! @return 实际写入目录的绝对路径（创建失败返回空）
    QString ensureExportDir() const;

public:
    void saveViewportNow();     //!< 立即将当前视角写入配置文件（关窗时调用）

    // ── QC 模式专用 ──────────────────────────────────────────────────────────
    QString currentTiffPath() const { return m_lastLoadedTiff; }
    QString currentBmpPath()  const { return m_lastLoadedBmp; }
    QString okFolderPath()    const;
    QString ngFolderPath()    const;
    void    goNext();           //!< 供外部调用，等价于 onNext()

private:
    ccMainAppInterface* m_app      = nullptr;
    unsigned            m_lastUID      = 0;
    bool                m_firstLoad    = true;  //!< 是否本次会话的首次加载
    int                 m_lastBitDepth = 0;     //!< 上次检测到的位深（用于自动更新 Z 无效值）
    int                 m_prevImageW   = 0;     //!< 上次成功加载图像的像素宽（用于尺寸突变自动复位视角）
    int                 m_prevImageH   = 0;
    bool                m_qcMode       = false; //!< QC 模式：隐藏保留对象/偏移，显示判定输出路径

    // ── QC 模式：上次成功加载的路径（供外部判定逻辑读取）──
    QString             m_lastLoadedTiff;
    QString             m_lastLoadedBmp;

    // ── 当前文件名显示 ──
    QLabel*         m_fileNameLabel  = nullptr;

    // ── 文件路径区 ──
    QLineEdit*      m_tiffEdit       = nullptr;
    QLineEdit*      m_bmpEdit        = nullptr;
    QLineEdit*      m_bmpFolderEdit  = nullptr;
    QPushButton*    m_bmpBrowseBtn   = nullptr; //!< 亮度图 "..." 按钮（SRF 时禁用）
    QPushButton*    m_bmpFolderBtn   = nullptr; //!< 亮度图文件夹 "..." 按钮（SRF 时禁用）

    // ── 分辨率参数区 ──
    QCheckBox*      m_autoResChk     = nullptr; //!< 自动从文件名读取分辨率
    QCheckBox*      m_autoTiffTagChk = nullptr; //!< 自动从 TIFF 标签读取分辨率
    QDoubleSpinBox* m_resXSpin       = nullptr;
    QDoubleSpinBox* m_resYSpin       = nullptr;
    QDoubleSpinBox* m_resZSpin       = nullptr;
    QCheckBox*      m_useOffsetChk   = nullptr; //!< 启用 XYZ 偏移
    QLabel*         m_offHdrLabel    = nullptr; //!< 偏移列表头（随复选框显隐）
    QDoubleSpinBox* m_offXSpin       = nullptr;
    QDoubleSpinBox* m_offYSpin       = nullptr;
    QDoubleSpinBox* m_offZSpin       = nullptr;
    QDoubleSpinBox* m_zInvalidSpin   = nullptr;
    QLabel*         m_resWarningLabel = nullptr; //!< 红色警告（自动分辨率失败时显示）
    //! 16位无符号旧版兼容开关（-32768 重新解释）：无可见控件，焦点在面板内时按 F1 切换；
    //! 仅在 m_useOffsetChk 勾选时生效；不写入设置，每次启动默认 false
    bool            m_legacyUint16CompatEnabled = false;

    // ── 颜色渲染范围区 ──
    QSlider*        m_colorMinSlider = nullptr;
    QSlider*        m_colorMaxSlider = nullptr;
    QLabel*         m_colorMinLabel  = nullptr;
    QLabel*         m_colorMaxLabel  = nullptr;

    // ── 显示模式区 ──
    QPushButton*    m_modeBtns[4]    = {};    //!< 0=Height 1=Brightness 2=Fusion 3=HeightGray
    QWidget*        m_fusionRow      = nullptr;
    QSlider*        m_alphaSlider    = nullptr;
    QLabel*         m_alphaLabel     = nullptr;
    QCheckBox*      m_meshChk        = nullptr;
    QDoubleSpinBox* m_maxEdgeSpin    = nullptr; //!< 网格最大边长 (mm)
    QCheckBox*      m_keepObjectChk  = nullptr; //!< 保留为独立对象（不覆盖上次导入）
    QCheckBox*      m_rotateZ90Chk   = nullptr; //!< 加载后绕 Z+ 旋转 90°
    QCheckBox*      m_rotateZ180Chk  = nullptr; //!< 加载后绕 Z+ 旋转 180°
    QComboBox*      m_yDsSampleCb    = nullptr; //!< Y 方向降采样比例（1=全采, 4/5, 3/5, 2/5, 1/5, 1/10）
    QDoubleSpinBox* m_zScaleSpin     = nullptr; //!< Z 显示放大倍数（纯渲染效果，1=不放大）
    QPushButton*    m_zScaleResetBtn = nullptr; //!< 一键恢复 Z 显示放大为 1×

    // ── 噪声过滤区 ──
    QCheckBox*      m_removeIslandsChk  = nullptr;
    QSpinBox*       m_minIslandSpin     = nullptr;
    QSpinBox*       m_minIslandDistSpin = nullptr; //!< 离岛距离（像素）
    QDoubleSpinBox* m_maxZGapSpin       = nullptr; //!< Z 方向连通阈值 (mm)

    // ── 图像导航区 ──
    QComboBox*      m_sortOrderCb    = nullptr;
    QPushButton*    m_firstBtn       = nullptr;
    QPushButton*    m_prevBtn        = nullptr;
    QPushButton*    m_nextBtn        = nullptr;
    QPushButton*    m_lastBtn        = nullptr;
    QLabel*         m_navLabel       = nullptr;

    // ── 状态栏 ──
    QLabel*         m_statusLabel    = nullptr;
    QPushButton*    m_loadButton     = nullptr;

    // ── QC 模式：判定输出路径控件（仅 qcMode 时创建）──
    QLineEdit*      m_okFolderEdit   = nullptr;
    QLineEdit*      m_ngFolderEdit   = nullptr;

    // ── 语言切换 ──
    bool            m_langEn         = false;
    QPushButton*    m_langBtnZh      = nullptr;
    QPushButton*    m_langBtnEn      = nullptr;

    // ── 整体滚动容器（用于展开导出面板时自动滚到底）─────────────────────
    QScrollArea*    m_scrollArea      = nullptr;

    // ── 导出 TIFF 区（默认折叠，点击按钮展开/收起）─────────────────────────
    QPushButton*    m_exportEnableBtn = nullptr; //!< 横向充满宽度的折叠按钮（checkable）
    QGroupBox*      m_exportGrp       = nullptr;
    QLineEdit*      m_exportDirEdit   = nullptr;
    QPushButton*    m_exportDirBtn    = nullptr;  //!< "..."  浏览目录
    QPushButton*    m_exportOpenBtn   = nullptr;  //!< 📂 在资源管理器中打开输出目录
    QComboBox*      m_exportFmtCb     = nullptr; //!< 0=16U, 1=16S, 2=32F（NoWheelComboBox）
    QDoubleSpinBox* m_exportInvalidSpin = nullptr; //!< 32-bit 无效填充值
    QLabel*         m_exportInvalidLbl  = nullptr;
    QPushButton*    m_exportOneBtn    = nullptr;
    QPushButton*    m_exportAllBtn    = nullptr;
    QLabel*         m_lblExportDirRow = nullptr;
    QLabel*         m_lblExportFmtRow = nullptr;
    QLabel*         m_lblExportEnable = nullptr; //!< "导出 TIFF" 提示标签

    // ── 各区 GroupBox（供 retranslateUi 更新标题）──
    QGroupBox*      m_fileGrp        = nullptr;
    QGroupBox*      m_resGrp         = nullptr;
    QGroupBox*      m_colorGrp       = nullptr;
    QGroupBox*      m_modeGrp        = nullptr;
    QGroupBox*      m_noiseGrp       = nullptr;
    QGroupBox*      m_navGrp         = nullptr;
    QGroupBox*      m_qcGrp          = nullptr;

    // ── 翻译用标签（供 retranslateUi 更新文字）──
    QLabel*         m_lblBmpRow      = nullptr;
    QLabel*         m_lblBmpFolderRow= nullptr;
    QLabel*         m_lblResHdr      = nullptr;
    QLabel*         m_lblZInvalidRow = nullptr;
    QLabel*         m_lblColorMinRow = nullptr;
    QLabel*         m_lblColorMaxRow = nullptr;
    QLabel*         m_lblAlphaText   = nullptr;
    QLabel*         m_lblMaxEdgeText = nullptr;
    QLabel*         m_lblNoiseMinPx  = nullptr;
    QLabel*         m_lblNoiseZGap   = nullptr;
    QLabel*         m_lblSortText    = nullptr;
    QLabel*         m_lblYDsText     = nullptr;
    QLabel*         m_lblZScaleText  = nullptr;
    QLabel*         m_lblLangText    = nullptr;
    QLabel*         m_lblOkFolderRow = nullptr;
    QLabel*         m_lblNgFolderRow = nullptr;

    // ── 自动重载防抖 ──
    QTimer*         m_autoReloadTimer = nullptr;
    QTimer*         m_settingsSaveTimer = nullptr;
    QFutureWatcher<AsyncLoadResult>* m_loadWatcher = nullptr;
    quint64         m_requestedGeneration = 0;
    quint64         m_runningGeneration   = 0;
    quint64         m_handledGeneration   = 0;
    bool            m_reloadPending       = false;

    // ── 文件夹导航状态 ──
    QStringList     m_folderFiles;
    int             m_currentIdx     = -1;
    QString         m_lastScannedFolder;
    int             m_lastScanSortOrder = -1;

    // ── 目录扫描 / BMP 自动匹配缓存 ──
    mutable QHash<QString, QStringList> m_dirImageFilesCache;
    mutable QHash<QString, QString>     m_bmpMatchCache;
    mutable QHash<QString, TiffBmpLoader::TiffInfo> m_tiffInfoCache;
};
