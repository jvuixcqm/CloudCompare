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
class ccMainAppInterface;

//! 嵌入 CloudCompare 侧栏的 TIFF/BMP 点云浏览面板
class TiffBmpPanel : public QWidget
{
    Q_OBJECT

public:
    static constexpr const char* kVersion     = "1.8";
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
        ccHObject* object            = nullptr;
        QString    error;
    };

    void tryParseResolutionFromFilename(const QString& path);
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
    bool currentModeNeedsBmp() const;
    int currentModeIndex() const;
    DisplayMode currentDisplayMode() const;
    bool prepareTiffInfo(const QString& tiffPath,
                         TiffBmpLoader::TiffInfo& outInfo,
                         int* outBitDepth = nullptr);
    void retranslateUi();                              //!< 按 m_langEn 更新所有控件文字
    QString ls(const char* zh, const char* en) const; //!< 返回当前语言字符串

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

    // ── 分辨率参数区 ──
    QCheckBox*      m_autoResChk     = nullptr;
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
