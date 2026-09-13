// TiffViewer — 主窗口实现
// Author: Jim

#include "TiffViewerWindow.h"
#include "TiffBmpPanel.h"

#include <ccGLWindowInterface.h>
#include <ccObject.h>
#include <ccDisplaySettingsDlg.h>
#include <ccGuiParameters.h>

#include <CCGeom.h>

#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>
#include <QAction>
#include <QActionGroup>
#include <QSettings>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdio>

TiffViewerWindow::~TiffViewerWindow() = default;

void TiffViewerWindow::closeEvent(QCloseEvent* event)
{
    if (m_panel)
        m_panel->saveViewportNow();
    saveToolbarSettings();
    QMainWindow::closeEvent(event);
}

TiffViewerWindow::TiffViewerWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QString("TiffViewer v%1  —  TIFF/BMP 点云浏览器").arg(TiffBmpPanel::kVersion));
    resize(1280, 800);

    // ── TiffViewer 专属显示默认值 ─────────────────────────────────────────
    // 用版本戳保证只在首次（或版本升级后）强制写入一次；之后用户通过
    // "显示参数设置" 对话框的修改可正常持久化，不会被覆盖。
    {
        constexpr int kDisplayDefaultsVersion = 2;
        QSettings stamp;
        const int savedVer = stamp.value("TiffViewer/displayDefaultsVer", 0).toInt();
        if (savedVer < kDisplayDefaultsVersion) {
            ccGui::ParamStruct p = ccGui::Parameters();
            p.decimateMeshOnMove  = false;
            p.decimateCloudOnMove = false;
            p.backgroundCol       = ccColor::Rgbub(0, 0, 0);
            p.displayCross        = false;
            p.singleClickPicking  = false;
            ccGui::Set(p);
            p.toPersistentSettings();
            stamp.setValue("TiffViewer/displayDefaultsVer", kDisplayDefaultsVersion);
        }
    }

    // ── 创建 OpenGL 窗口 ───────────────────────────────────────────────────
    {
        bool stereoMode = ccGLWindowInterface::TestStereoSupport();
        ccGLWindowInterface::Create(m_glWindow, m_glWidget, stereoMode);
        Q_ASSERT(m_glWindow && m_glWidget);
        // 去掉左上角点大小/线宽热区控件（TiffViewer 不需要鼠标悬停式调节）
        auto flags = m_glWindow->getInteractionMode();
        flags &= ~ccGLWindowInterface::INTERACT_CLICKABLE_ITEMS;
        m_glWindow->setInteractionMode(flags);
    }

    // 将 GL widget 放入中央区域
    auto* central = new QWidget(this);
    auto* lay     = new QVBoxLayout(central);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(m_glWidget);
    setCentralWidget(central);

    // ── 侧边栏：TiffBmpPanel（锁定，不可关闭/浮动/移动）───────────────────
    m_dock  = new QDockWidget("TIFF/BMP 点云浏览器", this);
    m_panel = new TiffBmpPanel(this, m_dock);
    m_dock->setWidget(m_panel);
    m_dock->setMinimumWidth(TiffBmpPanel::kDockFixedWidth);
    m_dock->setMaximumWidth(TiffBmpPanel::kDockFixedWidth);
    m_dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_dock->setFeatures(QDockWidget::NoDockWidgetFeatures);   // 禁止关闭/浮动
    // 用空 widget 替换标题栏，彻底隐藏拖拽区域
    auto* emptyTitleBar = new QWidget(m_dock);
    emptyTitleBar->setMaximumHeight(0);
    m_dock->setTitleBarWidget(emptyTitleBar);
    addDockWidget(Qt::LeftDockWidgetArea, m_dock);

    // ── 工具栏（竖向，位于左侧控制面板左边）────────────────────────────────
    buildToolBar();
    restoreToolbarSettings();

    // 顶部文字菜单不显示
    menuBar()->hide();

    statusBar()->showMessage(QString("就绪  ·  TiffViewer v%1  ·  by Jim").arg(TiffBmpPanel::kVersion));
}

void TiffViewerWindow::buildMenuBar()
{
    // ── 视图菜单 ──────────────────────────────────────────────────────────
    QMenu* viewMenu = menuBar()->addMenu("视图(&V)");

    auto addViewAction = [&](const QString& text, CC_VIEW_ORIENTATION ori,
                             const QString& iconPath, const QString& shortcut = {})
    {
        QAction* a = viewMenu->addAction(QIcon(iconPath), text,
                                         this, [this, ori]{ setView(ori); });
        if (!shortcut.isEmpty()) a->setShortcut(QKeySequence(shortcut));
        return a;
    };

    addViewAction("俯视  Top",      CC_TOP_VIEW,    ":/tv/icons/ccViewZpos.png", "7");
    addViewAction("仰视  Bottom",   CC_BOTTOM_VIEW, ":/tv/icons/ccViewZneg.png");
    addViewAction("前视  Front",    CC_FRONT_VIEW,  ":/tv/icons/ccViewYneg.png", "1");
    addViewAction("后视  Back",     CC_BACK_VIEW,   ":/tv/icons/ccViewYpos.png");
    addViewAction("左视  Left",     CC_LEFT_VIEW,   ":/tv/icons/ccViewXneg.png", "3");
    addViewAction("右视  Right",    CC_RIGHT_VIEW,  ":/tv/icons/ccViewXpos.png");
    viewMenu->addSeparator();
    addViewAction("等轴 1  ISO-1",  CC_ISO_VIEW_1,  ":/tv/icons/ccViewIso1.png", "0");
    addViewAction("等轴 2  ISO-2",  CC_ISO_VIEW_2,  ":/tv/icons/ccViewIso2.png");
    viewMenu->addSeparator();
    {
        auto* a = viewMenu->addAction(QIcon(":/tv/icons/ccGlobalZoom.png"),
                                      "适应全部  Fit All",
                                      this, &TiffViewerWindow::setGlobalZoom);
        a->setShortcut(QKeySequence("F"));
    }

    // ── 显示菜单 ──────────────────────────────────────────────────────────
    QMenu* dispMenu = menuBar()->addMenu("显示(&D)");

    {
        auto* a = dispMenu->addAction(QIcon(":/tv/icons/ccOrthoMode32.png"),
                                      "显示参数设置...",
                                      this, &TiffViewerWindow::showDisplaySettings);
        a->setShortcut(QKeySequence("P"));
    }
    dispMenu->addSeparator();

    QAction* orthoAct = dispMenu->addAction(QIcon(":/tv/icons/ccOrthoMode32.png"), "正交投影");
    orthoAct->setCheckable(true);
    orthoAct->setChecked(true);
    QAction* perspAct = dispMenu->addAction(QIcon(":/tv/icons/ccCenteredPerspective32.png"),
                                            "透视投影（以物体为中心）");
    perspAct->setCheckable(true);

    auto* projGroup = new QActionGroup(this);
    projGroup->addAction(orthoAct);
    projGroup->addAction(perspAct);
    connect(orthoAct,  &QAction::triggered, this, [this]{
        if (m_glWindow) { m_glWindow->setPerspectiveState(false, true); m_glWindow->redraw(); }
    });
    connect(perspAct, &QAction::triggered, this, [this]{
        if (m_glWindow) { m_glWindow->setPerspectiveState(true, true); m_glWindow->redraw(); }
    });

    dispMenu->addSeparator();
    dispMenu->addAction("点大小 +", this, &TiffViewerWindow::increasePointSize,
                        QKeySequence("+"));
    dispMenu->addAction("点大小 -", this, &TiffViewerWindow::decreasePointSize,
                        QKeySequence("-"));
}

void TiffViewerWindow::buildToolBar()
{
    QToolBar* tb = new QToolBar("视角", this);
    tb->setObjectName("ViewToolBar");
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);
    tb->setIconSize(QSize(28, 28));
    addToolBar(Qt::LeftToolBarArea, tb);

    // ── 适应全部 ────────────────────────────────────────────────────────────
    auto* fitAll = new QAction(QIcon(":/tv/icons/ccGlobalZoom.png"), "适应全部", this);
    fitAll->setToolTip("适应全部  Fit All  [F]");
    connect(fitAll, &QAction::triggered, this, &TiffViewerWindow::setGlobalZoom);
    tb->addAction(fitAll);

    tb->addSeparator();

    // ── 投影模式弹出按钮 ─────────────────────────────────────────────────────
    m_orthoAct = new QAction(QIcon(":/tv/icons/ccOrthoMode32.png"),
                             "正交投影", this);
    m_orthoAct->setToolTip("设为正交投影");
    m_orthoAct->setCheckable(true);
    m_orthoAct->setChecked(true);
    connect(m_orthoAct, &QAction::triggered, this, &TiffViewerWindow::setOrthoView);

    m_centPerspAct = new QAction(QIcon(":/tv/icons/ccCenteredPerspective32.png"),
                                 "物体中心透视", this);
    m_centPerspAct->setToolTip("设为物体中心透视投影");
    m_centPerspAct->setCheckable(true);
    connect(m_centPerspAct, &QAction::triggered, this, &TiffViewerWindow::setCenteredPerspView);

    m_viewerPerspAct = new QAction(QIcon(":/tv/icons/ccViewerBasedPerspective32.png"),
                                   "观察者透视", this);
    m_viewerPerspAct->setToolTip("设为观察者基准透视投影");
    m_viewerPerspAct->setCheckable(true);
    connect(m_viewerPerspAct, &QAction::triggered, this, &TiffViewerWindow::setViewerPerspView);

    auto* projGroup = new QActionGroup(this);
    projGroup->addAction(m_orthoAct);
    projGroup->addAction(m_centPerspAct);
    projGroup->addAction(m_viewerPerspAct);

    auto* projMenu = new QMenu(this);
    projMenu->addAction(m_orthoAct);
    projMenu->addAction(m_centPerspAct);
    projMenu->addAction(m_viewerPerspAct);

    m_projBtn = new QToolButton(this);
    m_projBtn->setMenu(projMenu);
    m_projBtn->setPopupMode(QToolButton::InstantPopup);
    m_projBtn->setDefaultAction(m_orthoAct);
    m_projBtn->setIcon(QIcon(":/tv/icons/ccOrthoMode32.png"));
    m_projBtn->setToolTip("设置投影模式");
    m_projBtn->setAutoRaise(true);
    m_projBtn->setIconSize(QSize(28, 28));
    connect(m_orthoAct,       &QAction::triggered, this, [this]{ m_projBtn->setIcon(m_orthoAct->icon()); });
    connect(m_centPerspAct,   &QAction::triggered, this, [this]{ m_projBtn->setIcon(m_centPerspAct->icon()); });
    connect(m_viewerPerspAct, &QAction::triggered, this, [this]{ m_projBtn->setIcon(m_viewerPerspAct->icon()); });
    tb->addWidget(m_projBtn);

    // ── 旋转轴图标可见性弹出按钮 ────────────────────────────────────────────
    m_pivotAlwaysAct = new QAction(QIcon(":/tv/icons/ccPivotOn.png"),
                                   "旋转轴始终显示", this);
    m_pivotAlwaysAct->setToolTip("旋转轴始终显示");
    m_pivotAlwaysAct->setCheckable(true);
    connect(m_pivotAlwaysAct, &QAction::triggered, this, &TiffViewerWindow::setPivotAlways);

    m_pivotOnMoveAct = new QAction(QIcon(":/tv/icons/ccPivotAuto.png"),
                                   "旋转时显示旋转轴", this);
    m_pivotOnMoveAct->setToolTip("旋转时显示旋转轴");
    m_pivotOnMoveAct->setCheckable(true);
    connect(m_pivotOnMoveAct, &QAction::triggered, this, &TiffViewerWindow::setPivotOnMove);

    m_pivotOffAct = new QAction(QIcon(":/tv/icons/ccPivotOff.png"),
                                "隐藏旋转轴", this);
    m_pivotOffAct->setToolTip("隐藏旋转轴图标");
    m_pivotOffAct->setCheckable(true);
    m_pivotOffAct->setChecked(true);
    connect(m_pivotOffAct, &QAction::triggered, this, &TiffViewerWindow::setPivotOff);

    auto* pivotGroup = new QActionGroup(this);
    pivotGroup->addAction(m_pivotAlwaysAct);
    pivotGroup->addAction(m_pivotOnMoveAct);
    pivotGroup->addAction(m_pivotOffAct);

    auto* pivotMenu = new QMenu(this);
    pivotMenu->addAction(m_pivotAlwaysAct);
    pivotMenu->addAction(m_pivotOnMoveAct);
    pivotMenu->addAction(m_pivotOffAct);

    m_pivotBtn = new QToolButton(this);
    m_pivotBtn->setMenu(pivotMenu);
    m_pivotBtn->setPopupMode(QToolButton::InstantPopup);
    m_pivotBtn->setIcon(QIcon(":/tv/icons/ccPivotOff.png"));
    m_pivotBtn->setToolTip("设置旋转轴可见性");
    m_pivotBtn->setAutoRaise(true);
    m_pivotBtn->setIconSize(QSize(28, 28));
    connect(m_pivotAlwaysAct, &QAction::triggered, this, [this]{ m_pivotBtn->setIcon(m_pivotAlwaysAct->icon()); });
    connect(m_pivotOnMoveAct, &QAction::triggered, this, [this]{ m_pivotBtn->setIcon(m_pivotOnMoveAct->icon()); });
    connect(m_pivotOffAct,    &QAction::triggered, this, [this]{ m_pivotBtn->setIcon(m_pivotOffAct->icon()); });
    tb->addWidget(m_pivotBtn);

    // ── 自动选择旋转中心 ─────────────────────────────────────────────────────
    m_autoPickPivotAct = new QAction(QIcon(":/tv/icons/ccPickCenterAuto.png"),
                                     "自动选择旋转中心", this);
    m_autoPickPivotAct->setToolTip("自动选择旋转中心（点击物体时以最近点为轴心）");
    m_autoPickPivotAct->setCheckable(true);
    connect(m_autoPickPivotAct, &QAction::toggled,
            this, &TiffViewerWindow::toggleAutoPickPivot);
    tb->addAction(m_autoPickPivotAct);

    // ── 锁定绕竖轴旋转 ──────────────────────────────────────────────────────
    m_lockAxisAct = new QAction(QIcon(":/tv/icons/lockedAxis.png"),
                                "锁定竖轴旋转", this);
    m_lockAxisAct->setToolTip("锁定绕竖轴（Z 轴）旋转");
    m_lockAxisAct->setCheckable(true);
    connect(m_lockAxisAct, &QAction::toggled,
            this, &TiffViewerWindow::toggleLockRotAxis);
    tb->addAction(m_lockAxisAct);

    tb->addSeparator();

    // ── 视角按钮 ─────────────────────────────────────────────────────────────
    struct ViewBtn { const char* label; CC_VIEW_ORIENTATION ori; const char* tip; const char* icon; };
    static const ViewBtn btns[] = {
        { "俯视",  CC_TOP_VIEW,    "俯视  Top",    ":/tv/icons/ccViewZpos.png" },
        { "仰视",  CC_BOTTOM_VIEW, "仰视  Bottom", ":/tv/icons/ccViewZneg.png" },
        { "前视",  CC_FRONT_VIEW,  "前视  Front",  ":/tv/icons/ccViewYneg.png" },
        { "后视",  CC_BACK_VIEW,   "后视  Back",   ":/tv/icons/ccViewYpos.png" },
        { "左视",  CC_LEFT_VIEW,   "左视  Left",   ":/tv/icons/ccViewXneg.png" },
        { "右视",  CC_RIGHT_VIEW,  "右视  Right",  ":/tv/icons/ccViewXpos.png" },
        { "ISO-1", CC_ISO_VIEW_1,  "等轴视角1",    ":/tv/icons/ccViewIso1.png" },
        { "ISO-2", CC_ISO_VIEW_2,  "等轴视角2",    ":/tv/icons/ccViewIso2.png" },
    };
    for (auto& b : btns) {
        auto* a = new QAction(QIcon(QString::fromUtf8(b.icon)),
                              QString::fromUtf8(b.label), this);
        a->setToolTip(QString::fromUtf8(b.tip));
        CC_VIEW_ORIENTATION ori = b.ori;
        connect(a, &QAction::triggered, this, [this, ori]{ setView(ori); });
        tb->addAction(a);
    }

    tb->addSeparator();

    // ── 点大小 ───────────────────────────────────────────────────────────────
    auto* ptPlus = new QAction(QIcon(":/tv/icons/ccPointSize.png"), "点大小+", this);
    ptPlus->setToolTip("点大小 +  [+]");
    connect(ptPlus, &QAction::triggered, this, &TiffViewerWindow::increasePointSize);
    tb->addAction(ptPlus);

    auto* ptMinus = new QAction(QIcon(":/tv/icons/ccPointSize.png"), "点大小−", this);
    ptMinus->setToolTip("点大小 -  [-]");
    connect(ptMinus, &QAction::triggered, this, &TiffViewerWindow::decreasePointSize);
    tb->addAction(ptMinus);

    tb->addSeparator();

    // ── 显示参数设置 ─────────────────────────────────────────────────────────
    auto* dispSettings = new QAction(QIcon(":/tv/icons/ccOrthoMode32.png"), "显示设置", this);
    dispSettings->setToolTip("显示参数设置  [P]");
    connect(dispSettings, &QAction::triggered, this, &TiffViewerWindow::showDisplaySettings);
    tb->addAction(dispSettings);
}

void TiffViewerWindow::setOrthoView()
{
    if (m_glWindow) { m_glWindow->setPerspectiveState(false, true); m_glWindow->redraw(); }
}

void TiffViewerWindow::setCenteredPerspView()
{
    if (m_glWindow) { m_glWindow->setPerspectiveState(true, true); m_glWindow->redraw(); }
}

void TiffViewerWindow::setViewerPerspView()
{
    if (m_glWindow) { m_glWindow->setPerspectiveState(true, false); m_glWindow->redraw(); }
}

void TiffViewerWindow::setPivotAlways()
{
    if (m_glWindow) { m_glWindow->setPivotVisibility(ccGLWindowInterface::PIVOT_ALWAYS_SHOW); m_glWindow->redraw(); }
}

void TiffViewerWindow::setPivotOnMove()
{
    if (m_glWindow) { m_glWindow->setPivotVisibility(ccGLWindowInterface::PIVOT_SHOW_ON_MOVE); m_glWindow->redraw(); }
}

void TiffViewerWindow::setPivotOff()
{
    if (m_glWindow) { m_glWindow->setPivotVisibility(ccGLWindowInterface::PIVOT_HIDE); m_glWindow->redraw(); }
}

void TiffViewerWindow::toggleAutoPickPivot(bool checked)
{
    if (m_glWindow) m_glWindow->setAutoPickPivotAtCenter(checked);
}

void TiffViewerWindow::toggleLockRotAxis(bool checked)
{
    if (m_glWindow) m_glWindow->lockRotationAxis(checked, CCVector3d(0, 0, 1));
}

// ── 工具栏状态持久化 ───────────────────────────────────────────────────────
void TiffViewerWindow::saveToolbarSettings()
{
    QSettings s;
    s.setValue("TiffViewer/toolbar/autoPickPivot", m_autoPickPivotAct->isChecked());
    s.setValue("TiffViewer/toolbar/lockAxis",      m_lockAxisAct->isChecked());
    const int pivotMode = m_pivotAlwaysAct->isChecked() ? 0
                        : m_pivotOnMoveAct->isChecked()  ? 1 : 2;
    s.setValue("TiffViewer/toolbar/pivotMode", pivotMode);
    const int projMode = m_centPerspAct->isChecked() ? 1
                       : m_viewerPerspAct->isChecked() ? 2 : 0;
    s.setValue("TiffViewer/toolbar/projMode", projMode);
}

void TiffViewerWindow::restoreToolbarSettings()
{
    QSettings s;
    const bool autoPickPivot = s.value("TiffViewer/toolbar/autoPickPivot", false).toBool();
    const bool lockAxis      = s.value("TiffViewer/toolbar/lockAxis",      false).toBool();
    const int  pivotMode     = s.value("TiffViewer/toolbar/pivotMode",     2).toInt();
    const int  projMode      = s.value("TiffViewer/toolbar/projMode",      0).toInt();

    m_autoPickPivotAct->setChecked(autoPickPivot);
    if (m_glWindow) m_glWindow->setAutoPickPivotAtCenter(autoPickPivot);

    m_lockAxisAct->setChecked(lockAxis);
    if (m_glWindow) m_glWindow->lockRotationAxis(lockAxis, CCVector3d(0, 0, 1));

    if (pivotMode == 0) {
        m_pivotAlwaysAct->setChecked(true);
        m_pivotBtn->setIcon(m_pivotAlwaysAct->icon());
        if (m_glWindow) m_glWindow->setPivotVisibility(ccGLWindowInterface::PIVOT_ALWAYS_SHOW);
    } else if (pivotMode == 1) {
        m_pivotOnMoveAct->setChecked(true);
        m_pivotBtn->setIcon(m_pivotOnMoveAct->icon());
        if (m_glWindow) m_glWindow->setPivotVisibility(ccGLWindowInterface::PIVOT_SHOW_ON_MOVE);
    } else {
        m_pivotOffAct->setChecked(true);
        m_pivotBtn->setIcon(m_pivotOffAct->icon());
        if (m_glWindow) m_glWindow->setPivotVisibility(ccGLWindowInterface::PIVOT_HIDE);
    }

    if (projMode == 1) {
        m_centPerspAct->setChecked(true);
        m_projBtn->setIcon(m_centPerspAct->icon());
        if (m_glWindow) m_glWindow->setPerspectiveState(true, true);
    } else if (projMode == 2) {
        m_viewerPerspAct->setChecked(true);
        m_projBtn->setIcon(m_viewerPerspAct->icon());
        if (m_glWindow) m_glWindow->setPerspectiveState(true, false);
    } else {
        m_orthoAct->setChecked(true);
        m_projBtn->setIcon(m_orthoAct->icon());
        if (m_glWindow) m_glWindow->setPerspectiveState(false, true);
    }

    if (m_glWindow) m_glWindow->redraw();
}

// ── 显示参数设置对话框 ─────────────────────────────────────────────────────
void TiffViewerWindow::showDisplaySettings()
{
    ccDisplaySettingsDlg dlg(this);
    connect(&dlg, &ccDisplaySettingsDlg::aspectHasChanged,
            this, &TiffViewerWindow::onAspectChanged);
    dlg.exec();
}

void TiffViewerWindow::onAspectChanged()
{
    ccGui::Parameters().toPersistentSettings();
    if (m_glWindow)
        m_glWindow->redraw();
}

// ── addToDB ────────────────────────────────────────────────────────────────
void TiffViewerWindow::addToDB(ccHObject* obj,
                               bool       updateZoom,
                               bool       /*autoExpandDBTree*/,
                               bool       /*checkDimensions*/,
                               bool       autoRedraw)
{
    if (!obj || !m_glWindow)
        return;

    obj->setDisplay_recursive(m_glWindow);

    ccHObject* currentRoot = m_glWindow->getSceneDB();
    if (currentRoot)
    {
        if (currentRoot->isA(CC_TYPES::HIERARCHY_OBJECT))
        {
            currentRoot->addChild(obj);
        }
        else
        {
            auto* root = new ccHObject("root");
            root->addChild(currentRoot);
            root->addChild(obj);
            m_glWindow->setSceneDB(root);
        }
    }
    else
    {
        auto* root = new ccHObject("root");
        root->addChild(obj);
        m_glWindow->setSceneDB(root);
    }

    if (updateZoom)
        m_glWindow->zoomGlobal();

    if (autoRedraw)
        m_glWindow->redraw();
}

// ── removeFromDB ───────────────────────────────────────────────────────────
void TiffViewerWindow::removeFromDB(ccHObject* obj, bool autoDelete)
{
    if (!obj || !m_glWindow)
        return;

    ccHObject* currentRoot = m_glWindow->getSceneDB();
    if (currentRoot)
    {
        if (currentRoot == obj)
        {
            m_glWindow->setSceneDB(nullptr);
            if (autoDelete)
                delete obj;
        }
        else
        {
            if (!autoDelete)
                currentRoot->detachChild(obj);
            else
                currentRoot->removeChild(obj);
        }
    }

    m_glWindow->redraw();
}

// ── dbRootObject ──────────────────────────────────────────────────────────
ccHObject* TiffViewerWindow::dbRootObject()
{
    return m_glWindow ? m_glWindow->getSceneDB() : nullptr;
}

// ── getSelectedEntities ───────────────────────────────────────────────────
const ccHObject::Container& TiffViewerWindow::getSelectedEntities() const
{
    static ccHObject::Container empty;
    return empty;
}

// ── dispToConsole ─────────────────────────────────────────────────────────
void TiffViewerWindow::dispToConsole(QString message, ConsoleMessageLevel level)
{
    const char* prefix = (level == ERR_CONSOLE_MESSAGE) ? "[ERROR] "
                       : (level == WRN_CONSOLE_MESSAGE) ? "[WARN]  "
                       : "";
    printf("%s%s\n", prefix, qUtf8Printable(message));

    if (level == ERR_CONSOLE_MESSAGE)
        statusBar()->showMessage("错误: " + message);
    else if (level == WRN_CONSOLE_MESSAGE)
        statusBar()->showMessage("警告: " + message);
}

// ── 显示控制 ───────────────────────────────────────────────────────────────
void TiffViewerWindow::redrawAll(bool only2D)
{
    if (m_glWindow) m_glWindow->redraw(only2D);
}

void TiffViewerWindow::refreshAll(bool only2D)
{
    if (m_glWindow) m_glWindow->refresh(only2D);
}

void TiffViewerWindow::enableAll()
{
    if (m_glWindow) m_glWindow->asWidget()->setEnabled(true);
}

void TiffViewerWindow::disableAll()
{
    if (m_glWindow) m_glWindow->asWidget()->setEnabled(false);
}

void TiffViewerWindow::disableAllBut(ccGLWindowInterface* win)
{
    if (m_glWindow && win != m_glWindow)
        m_glWindow->asWidget()->setEnabled(false);
}

// ── 视图控制 ───────────────────────────────────────────────────────────────
void TiffViewerWindow::setView(CC_VIEW_ORIENTATION view)
{
    if (m_glWindow) m_glWindow->setView(view, true);
}

void TiffViewerWindow::setGlobalZoom()
{
    if (m_glWindow) m_glWindow->zoomGlobal();
}

void TiffViewerWindow::increasePointSize()
{
    if (!m_glWindow) return;
    m_glWindow->setPointSize(m_glWindow->getViewportParameters().defaultPointSize + 1);
    m_glWindow->redraw();
}

void TiffViewerWindow::decreasePointSize()
{
    if (!m_glWindow) return;
    const float nextPointSize =
        m_glWindow->getViewportParameters().defaultPointSize - 1.0f;
    m_glWindow->setPointSize(nextPointSize < 1.0f ? 1.0f : nextPointSize);
    m_glWindow->redraw();
}

// ── getUniqueIDGenerator ──────────────────────────────────────────────────
ccUniqueIDGenerator::Shared TiffViewerWindow::getUniqueIDGenerator()
{
    return ccObject::GetUniqueIDGenerator();
}
