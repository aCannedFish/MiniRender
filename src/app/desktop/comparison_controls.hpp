// 相机面板内的 A/B 比较控件与教学点光源编辑器。
#pragma once
class QFormLayout;
class QComboBox;
class VulkanViewport;
// 两侧共用视口和相机。mode 是主显示选择器，第二个选择器同步提供可用显示模式。
void add_comparison_controls(QFormLayout& layout, QComboBox& mode, VulkanViewport& viewport);
