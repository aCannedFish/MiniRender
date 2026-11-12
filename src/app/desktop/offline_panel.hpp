#pragma once

#include <minirender/render/render_session.hpp>

#include <QWidget>
#include <functional>
#include <optional>

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
    void prepare_import(minirender::RenderSettings render,
                        minirender::IntegratorSettings integrator);
    void save_image(const QString& path, bool exr);
    bool exporting() const noexcept;
    bool stale() const noexcept;
    minirender::IntegratorSettings integrator_settings() const;
    void set_integrator_kind(minirender::IntegratorKind kind);
    const minirender::RenderSession& session() const noexcept;

  private:
    void poll();
    void update_backend_controls();
    void choose_export(bool exr);
    // 覆盖导入先异步取消旧任务，终止后再应用新建议，避免阻塞界面。
    struct ImportSettings {
        minirender::RenderSettings render;
        minirender::IntegratorSettings integrator;
    };
    std::optional<ImportSettings> imported_settings_;
    VulkanViewport& viewport_;
    std::function<std::shared_ptr<const minirender::SceneSnapshot>()> snapshot_;
    std::shared_ptr<minirender::RenderSession> session_;
    QWidget* settings_;
    QSpinBox *width_, *height_, *spp_, *depth_, *threads_, *rr_depth_, *photons_, *memory_;
    QLineEdit* seed_;
    QComboBox *mode_, *backend_;
    QCheckBox *rr_, *show_;
    QDoubleSpinBox *exposure_, *radius_, *alpha_;
    QWidget* sppm_settings_;
    QPushButton *start_, *pause_, *cancel_, *save_exr_, *save_png_;
    QLabel *status_, *details_, *notice_, *samples_label_, *sppm_details_, *phase_status_;
    QProgressBar *progress_, *phase_progress_;
    minirender::SessionSettings session_settings_;
    // 控件只更新用户改动的字段，导入时保留 double 和字节预算的完整精度。
    minirender::SppmOptions sppm_options_;
    std::uint32_t job_iterations_ = 0;
    std::uint64_t preview_samples_ = 0;
    bool preview_terminal_ = false;
    bool stale_ = false;
    bool exporting_ = false;
    QString export_message_;
};
