#pragma once

#include <QDialog>

class QRadioButton;
class QDoubleSpinBox;
class QSpinBox;
class QStackedWidget;
class QLabel;

//! 显示模式选择对话框
//! 让用户在加载 TIFF/BMP 之前选择三种显示模式之一，并填写对应参数
class DisplayModeDialog : public QDialog
{
    Q_OBJECT

public:
    enum Mode { Heightmap, Texture, Volume };

    explicit DisplayModeDialog(QWidget* parent = nullptr);

    Mode    selectedMode()   const;
    double  zScale()         const;  // Heightmap: Z 缩放
    double  meshWidth()      const;  // Texture: 平面宽度
    double  meshHeight()     const;  // Texture: 平面高度
    double  voxelSize()      const;  // Volume: 体素尺寸
    int     grayThreshold()  const;  // Volume: 灰度阈值

private slots:
    void onModeChanged();

private:
    QRadioButton* m_rbHeightmap = nullptr;
    QRadioButton* m_rbTexture   = nullptr;
    QRadioButton* m_rbVolume    = nullptr;
    QStackedWidget* m_stack     = nullptr;

    // Heightmap 参数
    QDoubleSpinBox* m_zScale      = nullptr;

    // Texture 参数
    QDoubleSpinBox* m_meshWidth   = nullptr;
    QDoubleSpinBox* m_meshHeight  = nullptr;

    // Volume 参数
    QDoubleSpinBox* m_voxelSize   = nullptr;
    QSpinBox*       m_grayThresh  = nullptr;
};
