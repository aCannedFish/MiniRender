// 固定当前视口相机与几何，为 A/B 右半幅生成独立的路径追踪参考。
#pragma once
#include <QWidget>
#include <minirender/render/render_session.hpp>
class VulkanViewport;
class QComboBox;
class QSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;

class ReferencePanel final : public QWidget {
  public:
    ReferencePanel(VulkanViewport& viewport, QComboBox& right, QWidget* parent);
    ~ReferencePanel() override;

  private:
    void start();
    void poll();
    VulkanViewport&                                  viewport_;
    QComboBox&                                       right_;
    minirender::RenderSession                        session_;
    std::shared_ptr<const minirender::SceneSnapshot> source_;
    minirender::Camera                               camera_;
    float                                            aspect_ = 1;
    QSpinBox *                                       size_, *samples_;
    QPushButton *                                    run_, *cancel_;
    QLabel*                                          status_;
    QProgressBar*                                    progress_;
    bool                                             running_ = false, published_ = false;
};
