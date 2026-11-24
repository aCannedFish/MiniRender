// EXR 序列播放器。每次只解码一个帧，拖动滑块时合并尚未执行的请求。
#pragma once
#include "resource_catalog.hpp"
#include <QDialog>
#include <QPixmap>

class QLabel;
class QSlider;
class QTimer;

// 查看器持有索引和最后一次成功显示的图像，销毁窗口会断开后台结果回调。
class SequenceViewer final : public QDialog {
  public:
    SequenceViewer(catalog::Entry entry, QWidget* parent);

  private:
    void request_frame();
    void resizeEvent(QResizeEvent* event) override;
    void update_image();
    catalog::Entry       entry_;
    QList<catalog::File> frames_;
    QLabel*              image_;
    QLabel*              status_;
    QSlider*             slider_;
    QTimer*              timer_;
    QPixmap              current_;
    bool                 loading_ = false;
};
