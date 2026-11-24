// 预计算辐射传输的参数、任务与环境球谐更新。窗口关闭时取消后台烘焙。
#pragma once
#include <QWidget>
#include <minirender/render/prt.hpp>
class VulkanViewport;
class QComboBox;
class QSpinBox;
class QPushButton;
class QProgressBar;
class QLabel;

class PrtPanel final : public QWidget {
  public:
    PrtPanel(VulkanViewport& viewport, QWidget* parent);
    ~PrtPanel() override;

  private:
    void start();
    void refresh_lighting();
    VulkanViewport&                                  viewport_;
    QComboBox*                                       mode_;
    QSpinBox *                                       samples_, *bounces_;
    QPushButton *                                    bake_, *cancel_;
    QProgressBar*                                    progress_;
    QLabel*                                          status_;
    minirender::CancellationSource                   cancellation_;
    std::shared_ptr<std::atomic<int>>                percent_;
    std::shared_ptr<const minirender::Texture>       source_;
    minirender::ShLight                              projected_{};
    std::shared_ptr<const minirender::SceneSnapshot> last_scene_;
    bool                                             running_ = false, projecting_ = false;
};
