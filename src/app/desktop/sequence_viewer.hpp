// 序列与离线结果共用的降噪比较窗口。后台只处理一个请求，拖动跳帧会取消旧请求。
#pragma once
#include "resource_catalog.hpp"
#include <QDialog>
#include <QImage>
#include <minirender/render/denoiser.hpp>
class QLabel;
class QSlider;
class QComboBox;
class QPushButton;
class QProgressBar;
class QTimer;
class ComparisonImage;
struct ViewedFrame;

// 查看器缓存当前输入和最近一次连续的时间累积结果。关闭窗口会取消滤波。
class SequenceViewer final : public QDialog {
  public:
    SequenceViewer(catalog::Entry entry, QWidget* parent);
    SequenceViewer(std::function<std::shared_ptr<const minirender::Image>()> image,
                   std::shared_ptr<const minirender::SceneSnapshot> scene, QWidget* parent);
    ~SequenceViewer() override;

  private:
    // 构造序列与单幅离线图共用的控件。
    void build();
    // 根据输入是否有引导和参考图启用对应的滤波方式。
    void prepare_controls();
    // 固定帧号和参数，在后台校验、读取和滤波，只发布最新请求。
    void request_frame();
    // 将当前结果导出为线性 EXR 或与显示一致的 PNG。
    void save();
    catalog::Entry        entry_{};
    QList<catalog::Frame> frames_;
    // 离线输入的读取回调固定任务 ID，可在后台安全获取完整分辨率图像。
    std::function<std::shared_ptr<const minirender::Image>()> offline_image_;
    std::shared_ptr<const minirender::SceneSnapshot>          offline_scene_;
    // 最后一次成功发布的帧，可复用原图、引导及相邻帧的时间历史。
    std::shared_ptr<const ViewedFrame> current_;
    QComboBox*                         filter_;
    QLabel*                            status_;
    QSlider*                           slider_;
    QTimer*                            play_timer_;
    QPushButton *                      play_, *save_;
    QProgressBar*                      progress_;
    minirender::DenoiseSettings        settings_;
    ComparisonImage*                   image_;
    minirender::CancellationSource     cancel_;
    std::shared_ptr<std::atomic<int>>  percent_;
    // 滑块或滤波设置改变时递增，用于丢弃迟到结果。
    int  revision_ = 0;
    bool loading_  = false;
};
