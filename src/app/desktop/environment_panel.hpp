#pragma once

#include "ibl_controller.hpp"
#include <QWidget>
#include <functional>

class QComboBox;
class QDoubleSpinBox;
class QPushButton;

// 环境场景参数通过事务回调写入文档；背景显示与任务控制只属于当前视口。
class EnvironmentPanel final : public QWidget {
  public:
    EnvironmentPanel(VulkanViewport& viewport, IblController& controller,
                     QWidget* parent = nullptr);
    void set_document(const minirender::SceneDocument& document);
    void set_import_enabled(bool enabled);
    std::function<void(glm::vec3, minirender::EnvironmentMap)> on_edited;
    std::function<void()> on_import;

  private:
    void edit();
    QComboBox* texture_;
    std::array<QDoubleSpinBox*, 3> constant_;
    QDoubleSpinBox *strength_, *rotation_;
    QPushButton* import_;
};
