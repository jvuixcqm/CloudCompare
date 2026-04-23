#include "../include/LmiTiffBmpPlugin.h"
#include "../include/TiffBmpPanel.h"
#include <QString>

#include <ccMainAppInterface.h>
#include <ccHObject.h>
#include <ccGLWindowInterface.h>

#include <QAction>
#include <QDockWidget>
#include <QMainWindow>

LmiTiffBmpPlugin::LmiTiffBmpPlugin(QObject* parent)
    : QObject(parent)
    , ccStdPluginInterface(":/CC/plugin/LmiTiffBmpPlugin/info.json")
{}

void LmiTiffBmpPlugin::onNewSelection(const ccHObject::Container&) {}

QList<QAction*> LmiTiffBmpPlugin::getActions()
{
    if (!m_action) {
        m_action = new QAction(QString("TIFF/BMP 点云浏览器 v%1").arg(TiffBmpPanel::kVersion), this);
        m_action->setToolTip(
            "加载 16-bit TIFF 高程图 + BMP 亮度图，生成三维点云\n"
            "支持高度色彩 / 亮度灰度 / 融合 / 高度灰阶四种显示模式，可逐帧浏览文件夹");
        connect(m_action, &QAction::triggered,
                this, &LmiTiffBmpPlugin::showPanel);
    }
    return { m_action };
}

void LmiTiffBmpPlugin::showPanel()
{
    if (!m_dock) {
        m_dock  = new QDockWidget(QString("TIFF/BMP 点云浏览器 v%1").arg(TiffBmpPanel::kVersion),
                                   m_app->getMainWindow());
        m_panel = new TiffBmpPanel(m_app, m_dock);
        m_dock->setWidget(m_panel);
        m_dock->setMinimumWidth(TiffBmpPanel::kDockFixedWidth);
        m_dock->setMaximumWidth(TiffBmpPanel::kDockFixedWidth);
        m_dock->setAllowedAreas(
            Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        m_app->getMainWindow()->addDockWidget(
            Qt::RightDockWidgetArea, m_dock);

        // 首次打开面板时设置 GL 视口默认行为：
        //   关闭"自动选择旋转中心"（由用户双击手动设置）
        //   关闭旋转轴图标显示
        if (auto* glw = m_app->getActiveGLWindow()) {
            glw->setAutoPickPivotAtCenter(false);
            glw->setPivotVisibility(ccGLWindowInterface::PIVOT_HIDE);
            glw->redraw();
        }
    }
    m_dock->show();
    m_dock->raise();
}
