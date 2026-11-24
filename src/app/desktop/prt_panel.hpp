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

// 持有烘焙任务的取消源和投影缓存，工作线程只读取固定的场景快照。
class PrtPanel final : public QWidget {
  public:
    PrtPanel(VulkanViewport& viewport, QWidget* parent);
    ~PrtPanel() override;

  private:
    // 根据传输模式和采样设置读取磁盘缓存，缺失时启动烘焙。
    void start();
    // 环境贴图变化时在后台投影；强度或旋转变化时直接更新九项系数。
    void refresh_lighting();
    VulkanViewport& viewport_;
    // 传输类型与采样控件由面板持有。
    QComboBox*    mode_;
    QSpinBox *    samples_, *bounces_;
    QPushButton * bake_, *cancel_;
    QProgressBar* progress_;
    QLabel*       status_;
    // 每次烘焙使用独立取消源，关闭窗口也会通知后台任务。
    minirender::CancellationSource cancellation_;
    // 工作线程发布百分比，Qt 定时器在 GUI 线程更新进度条。
    std::shared_ptr<std::atomic<int>> percent_;
    // projected_ 对应的环境纹理；改变旋转可以复用该投影。
    std::shared_ptr<const minirender::Texture> source_;
    minirender::ShLight                        projected_{};
    // 最近一次已应用环境照明的场景快照。
    std::shared_ptr<const minirender::SceneSnapshot> last_scene_;
    // 分别记录传输烘焙和环境投影是否在后台执行。
    bool running_ = false, projecting_ = false;
};
