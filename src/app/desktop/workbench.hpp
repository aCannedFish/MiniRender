#pragma once

#include "vulkan_viewport.hpp"

#include <QElapsedTimer>
#include <QMainWindow>
#include <minirender/scene/editor.hpp>

class QTreeWidget;
class QLineEdit;
class QDoubleSpinBox;
class QComboBox;
class VectorField;
class QProgressBar;
class QLabel;
class OfflinePanel;
class EnvironmentPanel;
class IblController;
class GeometryController;
class GeometryPanel;
struct GeometryTaskResult;

// 桌面编辑控制器：控件编辑委托给 SceneEditor，渲染器只收到不可变快照。
// file/undo/selection 都通过这里协调，不在 Vulkan 命令录制代码中读取控件。
class Workbench final : public QMainWindow {
  public:
    explicit Workbench(QVulkanInstance* instance, bool diagnostic = false);
    VulkanViewport* viewport() const noexcept;
    const minirender::SceneDocument& document() const noexcept;
    void load_scene_file(const QString& path);
    // 用户操作采用后台文件解析；同步入口供自动回归与确定的场景设置使用。
    void open_scene_file(const QString& path);
    void import_object_file(const QString& path);
    void save_scene_file(const QString& path);
    // 工具栏及几何操作使用后台入口；同步保存保留给确定执行顺序的回归检查。
    void save_scene_async(const QString& path);
    void apply_geometry();
    void import_height_file(const QString& path);
    void export_geometry_file(const QString& path);
    GeometryController* geometry_controller() const noexcept;
    GeometryPanel* geometry_panel() const noexcept;
    void select_object(const std::string& id);
    void import_texture_file(const QString& path, minirender::ColorEncoding encoding);
    bool texture_loading() const noexcept;
    bool asset_loading() const noexcept;
    OfflinePanel* offline_panel() const noexcept;
    IblController* ibl_controller() const noexcept;
    void import_environment_file(const QString& path);

  private:
    void build_object_panel();
    void build_light_panel();
    void build_camera_panel();
    void refresh();
    void refresh_properties();
    void refresh_camera();
    void perform_edit(const std::function<void(minirender::SceneDocument&)>& edit,
                      const minirender::MeshValidationCache* validated = nullptr);
    void edit_object_property(const std::function<void(minirender::SceneObject&)>& edit);
    void edit_material_property(const std::function<void(minirender::Material&)>& edit);
    void duplicate_object();
    void delete_object();
    void import_object();
    void import_texture();
    void report_error(const std::exception& error);
    void begin_asset_task(const QString& message);
    void finish_asset_task();
    bool finish_geometry_task(const GeometryTaskResult& result);
    void install_scene(minirender::SceneDocument document, const QString& path,
                       const minirender::MeshValidationCache* validated = nullptr);
    minirender::SceneObject* selected(minirender::SceneDocument& scene) const;

    VulkanViewport* viewport_ = nullptr;
    minirender::SceneEditor editor_{minirender::SceneDocument{}};
    QTreeWidget* tree_ = nullptr;
    QTreeWidget* assets_ = nullptr;
    QWidget* object_panel_ = nullptr;
    QWidget* light_panel_ = nullptr;
    QWidget* camera_panel_ = nullptr;
    OfflinePanel* offline_ = nullptr;
    EnvironmentPanel* environment_ = nullptr;
    IblController* ibl_ = nullptr;
    GeometryController* geometry_tasks_ = nullptr;
    GeometryPanel* geometry_ = nullptr;
    QLineEdit* object_name_ = nullptr;
    VectorField* translation_ = nullptr;
    VectorField* rotation_ = nullptr;
    VectorField* scale_ = nullptr;
    VectorField* base_color_ = nullptr;
    VectorField* emission_ = nullptr;
    QComboBox* material_kind_ = nullptr;
    QComboBox* texture_selector_ = nullptr;
    QComboBox* texture_filter_ = nullptr;
    QAction* import_texture_action_ = nullptr;
    QAction* import_object_action_ = nullptr;
    QAction* open_action_ = nullptr;
    QAction* save_action_ = nullptr;
    QAction* save_as_action_ = nullptr;
    QProgressBar* task_progress_ = nullptr;
    QLabel* task_label_ = nullptr;
    QString task_message_;
    QString geometry_notice_extra_;
    QElapsedTimer geometry_notice_;
    bool asset_loading_ = false;
    bool texture_loading_ = false;
    std::uint64_t document_generation_ = 0;
    std::uint64_t document_revision_ = 0;
    QDoubleSpinBox* roughness_ = nullptr;
    QDoubleSpinBox* ior_ = nullptr;
    QComboBox* light_selector_ = nullptr;
    VectorField* light_direction_ = nullptr;
    VectorField* light_color_ = nullptr;
    QDoubleSpinBox* light_intensity_ = nullptr;
    VectorField* camera_position_ = nullptr;
    VectorField* camera_target_ = nullptr;
    QDoubleSpinBox* fov_ = nullptr;
    QDoubleSpinBox* aperture_ = nullptr;
    QDoubleSpinBox* focus_ = nullptr;
    QDoubleSpinBox* exposure_ = nullptr;
    QAction* undo_ = nullptr;
    QAction* redo_ = nullptr;
    std::string selected_id_;
    QString scene_path_;
    bool refreshing_ = false;
    bool camera_refresh_pending_ = false;
    bool diagnostic_ = false;
};
