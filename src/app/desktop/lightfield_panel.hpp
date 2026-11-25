// 光场参数控件、状态刷新与用户操作回调。

#pragma once
#include "lightfield_controller.hpp"
#include <QWidget>

class QSpinBox;
class QDoubleSpinBox;
class QSlider;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QProgressBar;
class QLabel;
class QPushButton;

// 采集参数描述当前场景；重建控件只读取已完成数据的标定。同步数值时屏蔽编辑信号。
class LightFieldPanel final : public QWidget {
  public:
    // 建立采集与重建控件，连接工作台提供的视口和后台控制器。
    LightFieldPanel(LightFieldController& controller, VulkanViewport& viewport,
                    QWidget* parent = nullptr);
    // 将已有采集配置显示到面板，供场景预设恢复使用。
    void set_capture_settings(minirender::LightFieldCaptureSettings settings);
    // 读取各视点分辨率、相机阵列、采样数和计算预算。
    minirender::LightFieldCaptureSettings capture_settings() const;
    // 采集开始时从工作台取得不可变场景和积分器配置。
    std::function<std::shared_ptr<const minirender::SceneSnapshot>()> snapshot;
    std::function<void()>                                             on_demo;
    std::function<minirender::IntegratorSettings()>                   integrator;
    // 在资产任务占用期间更新界面操作状态。
    void set_asset_busy(bool busy);

  private:
    // 根据控制器的进度、数据版本与错误刷新控件。
    void sync();
    // 根据当前显示模式请求重聚焦图像或单个子视图。
    void request();
    // 执行控件操作，在状态区显示错误。
    void invoke(const std::function<void()>& work);
    LightFieldController& controller_;
    VulkanViewport&       viewport_;
    QWidget *             capture_fields_, *focus_fields_, *focus_shortcuts_;
    QSpinBox *width_, *height_, *spp_, *columns_, *rows_, *threads_, *memory_, *column_, *row_;
    QDoubleSpinBox *radius_, *reference_, *focus_, *aperture_, *exposure_;
    QLineEdit*      seed_;
    QSlider *       focus_slider_, *aperture_slider_;
    QComboBox*      mode_;
    QCheckBox*      show_;
    QPushButton *   start_, *pause_, *cancel_, *open_, *save_, *exr_, *png_, *demo_;
    QLabel *        status_, *info_, *notice_;
    QProgressBar*   progress_;
    std::uint64_t   revision_   = 0;
    bool            syncing_    = false;
    bool            asset_busy_ = false;
    QString         error_;
};
