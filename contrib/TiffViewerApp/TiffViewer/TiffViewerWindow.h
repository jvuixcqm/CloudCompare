#pragma once
// TiffViewer — 主窗口：同时实现 ccMainAppInterface
// Author: Jim

#include <QMainWindow>
#include <QCloseEvent>
#include <ccMainAppInterface.h>
#include <ccHObject.h>

class ccGLWindowInterface;
class TiffBmpPanel;
class QDockWidget;
class QToolButton;

//! 独立查看器主窗口，实现 ccMainAppInterface 供 TiffBmpPanel 使用
class TiffViewerWindow : public QMainWindow, public ccMainAppInterface
{
    Q_OBJECT

public:
    explicit TiffViewerWindow(QWidget* parent = nullptr);
    ~TiffViewerWindow() override;

    // ── ccMainAppInterface 纯虚方法实现 ─────────────────────────────────────

    QMainWindow* getMainWindow() override { return this; }

    ccGLWindowInterface* getActiveGLWindow() override { return m_glWindow; }

    void addToDB(ccHObject* obj,
                 bool       updateZoom       = false,
                 bool       autoExpandDBTree = true,
                 bool       checkDimensions  = false,
                 bool       autoRedraw       = true) override;

    void removeFromDB(ccHObject* obj, bool autoDelete = true) override;

    void setSelectedInDB(ccHObject* obj, bool selected) override {}

    const ccHObject::Container& getSelectedEntities() const override;

    void dispToConsole(QString message,
                       ConsoleMessageLevel level = STD_CONSOLE_MESSAGE) override;

    ccHObject* dbRootObject() override;

    void redrawAll(bool only2D = false) override;
    void refreshAll(bool only2D = false) override;

    void enableAll() override;
    void disableAll() override;
    void disableAllBut(ccGLWindowInterface* win) override;

    void updateUI() override {}
    void freezeUI(bool state) override {}

    ccUniqueIDGenerator::Shared getUniqueIDGenerator() override;

    ccHObject* loadFile(QString filename, bool silent) override { return nullptr; }

    void setView(CC_VIEW_ORIENTATION view) override;
    void toggleActiveWindowCenteredPerspective() override {}
    void toggleActiveWindowCustomLight() override {}
    void toggleActiveWindowSunLight() override {}
    void toggleActiveWindowViewerBasedPerspective() override {}
    void zoomOnSelectedEntities() override {}
    void setGlobalZoom() override;
    void increasePointSize() override;
    void decreasePointSize() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void showDisplaySettings();
    void onAspectChanged();
    void setPivotAlways();
    void setPivotOnMove();
    void setPivotOff();
    void toggleAutoPickPivot(bool checked);
    void toggleLockRotAxis(bool checked);
    void setOrthoView();
    void setCenteredPerspView();
    void setViewerPerspView();

private:
    void buildToolBar();
    void buildMenuBar();
    void saveToolbarSettings();
    void restoreToolbarSettings();

    ccGLWindowInterface* m_glWindow          = nullptr;
    QWidget*             m_glWidget          = nullptr;
    TiffBmpPanel*        m_panel             = nullptr;
    QDockWidget*         m_dock              = nullptr;

    QAction*             m_autoPickPivotAct  = nullptr;
    QAction*             m_lockAxisAct       = nullptr;
    QAction*             m_pivotAlwaysAct    = nullptr;
    QAction*             m_pivotOnMoveAct    = nullptr;
    QAction*             m_pivotOffAct       = nullptr;
    QAction*             m_orthoAct          = nullptr;
    QAction*             m_centPerspAct      = nullptr;
    QAction*             m_viewerPerspAct    = nullptr;

    QToolButton*         m_projBtn           = nullptr;
    QToolButton*         m_pivotBtn          = nullptr;
};
