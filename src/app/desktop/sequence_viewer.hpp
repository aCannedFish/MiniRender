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
    void build();
    void prepare_controls();
    void request_frame();
    void save();
    catalog::Entry                                            entry_{};
    QList<catalog::Frame>                                     frames_;
    std::function<std::shared_ptr<const minirender::Image>()> offline_image_;
    std::shared_ptr<const minirender::SceneSnapshot>          offline_scene_;
    std::shared_ptr<const ViewedFrame>                        current_;
    QComboBox*                                                filter_;
    QLabel*                                                   status_;
    QSlider*                                                  slider_;
    QTimer*                                                   play_timer_;
    QPushButton *                                             play_, *save_;
    QProgressBar*                                             progress_;
    minirender::DenoiseSettings                               settings_;
    ComparisonImage*                                          image_;
    minirender::CancellationSource                            cancel_;
    std::shared_ptr<std::atomic<int>>                         percent_;
    int                                                       revision_ = 0;
    bool                                                      loading_  = false;
};
