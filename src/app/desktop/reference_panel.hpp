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

// 工作台拥有面板和渲染会话，面板借用视口与右侧模式选择器。
class ReferencePanel final : public QWidget {
  public:
    ReferencePanel(VulkanViewport& viewport, QComboBox& right, QWidget* parent);
    ~ReferencePanel() override;

  private:
    // 快照固定几何和当前相机，按视口宽高比确定离线分辨率。
    void start();
    // 同步采样进度；相机或场景已改变时清除过期参考。
    void poll();
    // 接收完成的线性参考纹理，并负责识别相机或视口比例变动。
    VulkanViewport& viewport_;
    // 任务完成后切换到参考图，失效时恢复普通显示。
    QComboBox& right_;
    // 独立的 CPU 路径追踪任务，只有完整的结果进入比较视图。
    minirender::RenderSession session_;
    // 运行中的源快照，与相机和宽高比共同确定可复用的参考。
    std::shared_ptr<const minirender::SceneSnapshot> source_;
    minirender::Camera                               camera_;
    float                                            aspect_ = 1;
    // 参考图最长边和每像素样本数。以下控件由 Qt 父子关系管理。
    QSpinBox *    size_, *samples_;
    QPushButton * run_, *cancel_;
    QLabel*       status_;
    QProgressBar* progress_;
    // 分别记录尚在计算的任务和已发布到视口的参考。
    bool running_ = false, published_ = false;
};
