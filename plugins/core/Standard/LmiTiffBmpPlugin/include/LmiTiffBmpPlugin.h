// Author: Jim
#pragma once

#include <ccStdPluginInterface.h>

class QAction;
class QDockWidget;
class TiffBmpPanel;

//! CloudCompare 插件：16-bit TIFF 高程图 + BMP 亮度图 → 三维点云浏览器
class LmiTiffBmpPlugin : public QObject, public ccStdPluginInterface
{
    Q_OBJECT
    Q_INTERFACES( ccPluginInterface ccStdPluginInterface )
    Q_PLUGIN_METADATA( IID "cccorp.cloudcompare.plugin.LmiTiffBmpPlugin"
                       FILE "../info.json" )

public:
    explicit LmiTiffBmpPlugin(QObject* parent = nullptr);
    ~LmiTiffBmpPlugin() override = default;

    void onNewSelection(const ccHObject::Container& selectedEntities) override;
    QList<QAction*> getActions() override;

private slots:
    void showPanel();

private:
    QAction*      m_action = nullptr;
    QDockWidget*  m_dock   = nullptr;
    TiffBmpPanel* m_panel  = nullptr;
};
