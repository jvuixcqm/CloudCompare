#pragma once

#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QComboBox>
#include <QWheelEvent>
#include <QLocale>

//! 高精度浮点输入框：允许输入较多小数位，但显示时不强制补零。
//! 滚轮事件被忽略，防止意外改值。
class FlexibleDoubleSpinBox : public QDoubleSpinBox
{
public:
    explicit FlexibleDoubleSpinBox(QWidget* parent = nullptr)
        : QDoubleSpinBox(parent)
    {
        setDecimals(100);
        setStepType(QAbstractSpinBox::AdaptiveDecimalStepType);
    }

protected:
    QString textFromValue(double value) const override
    {
        QLocale loc = locale();
        loc.setNumberOptions(loc.numberOptions() | QLocale::OmitGroupSeparator);
        return loc.toString(value, 'g', 15);
    }

    void wheelEvent(QWheelEvent* e) override { e->ignore(); }
};

//! 整数输入框，滚轮事件被忽略，防止意外改值。
class NoWheelSpinBox : public QSpinBox
{
public:
    explicit NoWheelSpinBox(QWidget* parent = nullptr) : QSpinBox(parent) {}
protected:
    void wheelEvent(QWheelEvent* e) override { e->ignore(); }
};

//! 下拉选择框，滚轮事件被忽略，防止意外改值。
class NoWheelComboBox : public QComboBox
{
public:
    explicit NoWheelComboBox(QWidget* parent = nullptr) : QComboBox(parent) {}
protected:
    void wheelEvent(QWheelEvent* e) override { e->ignore(); }
};