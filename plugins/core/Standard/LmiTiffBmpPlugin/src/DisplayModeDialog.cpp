#include "DisplayModeDialog.h"

#include <QRadioButton>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QStackedWidget>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>

DisplayModeDialog::DisplayModeDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("选择 3D 显示模式");
    setMinimumWidth(360);

    // --- 模式选择区 ---
    m_rbHeightmap = new QRadioButton("高程点云（灰度 → Z 高度）");
    m_rbTexture   = new QRadioButton("纹理平面（图像贴到平面网格）");
    m_rbVolume    = new QRadioButton("体积点云（多层 TIFF）");
    m_rbHeightmap->setChecked(true);

    QGroupBox* modeBox = new QGroupBox("显示模式");
    QVBoxLayout* modeLay = new QVBoxLayout(modeBox);
    modeLay->addWidget(m_rbHeightmap);
    modeLay->addWidget(m_rbTexture);
    modeLay->addWidget(m_rbVolume);

    // --- 参数面板（Stacked）---
    m_stack = new QStackedWidget;

    // Page 0: Heightmap
    {
        QWidget* pg = new QWidget;
        QFormLayout* fl = new QFormLayout(pg);
        m_zScale = new QDoubleSpinBox;
        m_zScale->setRange(0.001, 10000.0);
        m_zScale->setDecimals(3);
        m_zScale->setValue(1.0);
        m_zScale->setSuffix(" 米");
        fl->addRow("像素值 255 对应的 Z 高度：", m_zScale);
        m_stack->addWidget(pg);
    }

    // Page 1: Texture
    {
        QWidget* pg = new QWidget;
        QFormLayout* fl = new QFormLayout(pg);
        m_meshWidth = new QDoubleSpinBox;
        m_meshWidth->setRange(0.001, 100000.0);
        m_meshWidth->setDecimals(3);
        m_meshWidth->setValue(1.0);
        m_meshWidth->setSuffix(" 米");

        m_meshHeight = new QDoubleSpinBox;
        m_meshHeight->setRange(0.001, 100000.0);
        m_meshHeight->setDecimals(3);
        m_meshHeight->setValue(1.0);
        m_meshHeight->setSuffix(" 米");

        fl->addRow("平面宽度：", m_meshWidth);
        fl->addRow("平面高度：", m_meshHeight);
        m_stack->addWidget(pg);
    }

    // Page 2: Volume
    {
        QWidget* pg = new QWidget;
        QFormLayout* fl = new QFormLayout(pg);
        m_voxelSize = new QDoubleSpinBox;
        m_voxelSize->setRange(0.0001, 100.0);
        m_voxelSize->setDecimals(4);
        m_voxelSize->setValue(0.001);
        m_voxelSize->setSuffix(" 米");

        m_grayThresh = new QSpinBox;
        m_grayThresh->setRange(0, 255);
        m_grayThresh->setValue(10);

        fl->addRow("体素物理尺寸：", m_voxelSize);
        fl->addRow("灰度过滤阈值（低于此值忽略）：", m_grayThresh);
        m_stack->addWidget(pg);
    }

    QGroupBox* paramBox = new QGroupBox("参数");
    QVBoxLayout* paramLay = new QVBoxLayout(paramBox);
    paramLay->addWidget(m_stack);

    // --- 确定 / 取消 ---
    QDialogButtonBox* btns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    QVBoxLayout* mainLay = new QVBoxLayout(this);
    mainLay->addWidget(modeBox);
    mainLay->addWidget(paramBox);
    mainLay->addWidget(btns);

    connect(m_rbHeightmap, &QRadioButton::toggled, this, &DisplayModeDialog::onModeChanged);
    connect(m_rbTexture,   &QRadioButton::toggled, this, &DisplayModeDialog::onModeChanged);
    connect(m_rbVolume,    &QRadioButton::toggled, this, &DisplayModeDialog::onModeChanged);
    connect(btns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void DisplayModeDialog::onModeChanged()
{
    if (m_rbHeightmap->isChecked())
        m_stack->setCurrentIndex(0);
    else if (m_rbTexture->isChecked())
        m_stack->setCurrentIndex(1);
    else
        m_stack->setCurrentIndex(2);
}

DisplayModeDialog::Mode DisplayModeDialog::selectedMode() const
{
    if (m_rbTexture->isChecked())  return Texture;
    if (m_rbVolume->isChecked())   return Volume;
    return Heightmap;
}

double DisplayModeDialog::zScale()        const { return m_zScale->value(); }
double DisplayModeDialog::meshWidth()     const { return m_meshWidth->value(); }
double DisplayModeDialog::meshHeight()    const { return m_meshHeight->value(); }
double DisplayModeDialog::voxelSize()     const { return m_voxelSize->value(); }
int    DisplayModeDialog::grayThreshold() const { return m_grayThresh->value(); }
