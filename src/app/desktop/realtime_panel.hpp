#pragma once

#include <QWidget>
class VulkanViewport;

// 实时效果参数只更新视口，不修改离线任务的场景快照。
class RealtimePanel final : public QWidget {
  public:
    explicit RealtimePanel(VulkanViewport& viewport, QWidget* parent = nullptr);
};
