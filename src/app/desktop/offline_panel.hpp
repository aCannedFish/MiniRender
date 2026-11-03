#pragma once

#include <minirender/render/render_session.hpp>

#include <QWidget>
#include <functional>

class VulkanViewport;
class QSpinBox;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QPushButton;
class QLabel;
class QProgressBar;

// Qt 任务控制器。固定启动时的场景快照，定时读取核心状态并发布独立预览。
// 渲染线程不访问控件，导出线程持有 session 的共享所有权，不捕获 QWidget。
class OfflinePanel final : public QWidget {
  public:
    OfflinePanel(VulkanViewport& viewport,
                 std::function<std::shared_ptr<const minirender::SceneSnapshot>()> snapshot,
                 QWidget* parent = nullptr);
    ~OfflinePanel() override;
    void start();
    void scene_changed();
    void set_settings(minirender::RenderSettings render, minirender::IntegratorSettings integrator,
                      minirender::SessionSettings session);
    void save_image(const QString& path, bool exr);
    bool exporting() const noexcept;
    bool stale() const noexcept;
    const minirender::RenderSession& session() const noexcept;

  private:
    void poll();
    void choose_export(bool exr);
    VulkanViewport& viewport_;
    std::function<std::shared_ptr<const minirender::SceneSnapshot>()> snapshot_;
    std::shared_ptr<minirender::RenderSession> session_;
    QWidget* settings_;
    QSpinBox *width_, *height_, *spp_, *depth_, *threads_, *rr_depth_;
    QLineEdit* seed_;
    QComboBox* mode_;
    QCheckBox *rr_, *show_;
    QDoubleSpinBox* exposure_;
    QPushButton *start_, *pause_, *cancel_, *save_exr_, *save_png_;
    QLabel *status_, *details_, *notice_;
    QProgressBar* progress_;
    minirender::SessionSettings session_settings_;
    std::uint64_t preview_samples_ = 0;
    bool preview_terminal_ = false;
    bool stale_ = false;
    bool exporting_ = false;
    QString export_message_;
};
