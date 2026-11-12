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
    LightFieldPanel(LightFieldController& controller, VulkanViewport& viewport,
                    QWidget* parent = nullptr);
    void set_capture_settings(minirender::LightFieldCaptureSettings settings);
    minirender::LightFieldCaptureSettings capture_settings() const;
    std::function<std::shared_ptr<const minirender::SceneSnapshot>()> snapshot;
    std::function<void()> on_demo;
    std::function<minirender::IntegratorSettings()> integrator;
    void set_asset_busy(bool busy);

  private:
    void sync();
    void request();
    void invoke(const std::function<void()>& work);
    LightFieldController& controller_;
    VulkanViewport& viewport_;
    QWidget *capture_fields_, *focus_fields_, *focus_shortcuts_;
    QSpinBox *width_, *height_, *spp_, *columns_, *rows_, *threads_, *memory_, *column_, *row_;
    QDoubleSpinBox *radius_, *reference_, *focus_, *aperture_, *exposure_;
    QLineEdit* seed_;
    QSlider *focus_slider_, *aperture_slider_;
    QComboBox* mode_;
    QCheckBox* show_;
    QPushButton *start_, *pause_, *cancel_, *open_, *save_, *exr_, *png_, *demo_;
    QLabel *status_, *info_, *notice_;
    QProgressBar* progress_;
    std::uint64_t revision_ = 0;
    bool syncing_ = false, asset_busy_ = false;
    QString error_;
};
