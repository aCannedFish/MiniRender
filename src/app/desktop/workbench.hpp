// Qt 工作台的视口、面板、编辑事务及后台资产任务协调。

#pragma once

#include "workbench_panels.hpp"

#include "vulkan_viewport.hpp"

#include <QElapsedTimer>
#include <QMainWindow>
#include <minirender/scene/editor.hpp>

struct BuiltinScene;

namespace catalog {
struct Entry;
enum class ImportAction;
} // namespace catalog
class QTreeWidget;
class QLineEdit;
class QDoubleSpinBox;
class QComboBox;
class QToolButton;
class QCheckBox;
class VectorField;
class QProgressBar;
class QLabel;
class OfflinePanel;
class EnvironmentPanel;
class IblController;
class GeometryController;
class GeometryPanel;
class PointCloudPanel;
class SdfPanel;
class ClothMaterialPanel;
class ClothColliderPanel;
class ClothController;
class ClothPanel;
class VolumePanel;
class LightFieldPanel;
class LightFieldController;
class SubsurfaceMaterialPanel;
struct ClothRuntimeFrame;

namespace minirender {
struct ClothDefinition;
}
struct GeometryTaskResult;

// 桌面编辑控制器：控件编辑委托给 SceneEditor，渲染器只收到不可变快照。
// file/undo/selection 都通过这里协调，不在 Vulkan 命令录制代码中读取控件。
class Workbench final : public QMainWindow {
  public:
    // 返回页面路由器，供工具和回归选择物体或全局设置页。
    WorkbenchPanels* panels() const noexcept {
        return panels_;
    }

    // 创建 Qt 工作台并绑定 Vulkan 视口；diagnostic 选择基础 GPU 诊断布局。
    explicit Workbench(QVulkanInstance* instance, bool diagnostic = false);
    ~Workbench() override;
    // 返回工作台持有的视口，生命周期由主窗口管理。
    VulkanViewport* viewport() const noexcept;
    // 返回当前只读场景文档，修改应通过编辑事务完成。
    const minirender::SceneDocument& document() const noexcept;
    // 同步读取并安装场景，供启动和确定执行顺序的回归使用。
    void load_scene_file(const QString& path);
    // 用户操作采用后台文件解析；同步入口供自动回归与确定的场景设置使用。
    // 在后台加载及校验场景，完成后在 GUI 线程安装并调用 after_load。
    void open_scene_file(const QString& path, std::function<void()> after_load = {});
    // 按模板标识覆盖导入场景，同时应用预览模式和离线推荐参数。
    void import_builtin_scene(const QString& id);
    // 根据资源类型覆盖场景、追加物体或打开帧序列。
    void import_resource(const catalog::Entry& entry, catalog::ImportAction action);
    // 后台解析 OBJ 并准备渲染数据，完成后创建场景资产和物体。
    void import_object_file(const QString& path);
    // 同步保存文档与生成资产，并记录当前保存路径。
    void save_scene_file(const QString& path);
    // 工具栏及几何操作使用后台入口；同步保存保留给确定执行顺序的回归检查。
    // 固定当前文档快照，在后台写出场景和生成资产。
    void save_scene_async(const QString& path);
    // 从选中物体和几何面板固定输入，后台处理后通过事务替换目标资产。
    void apply_geometry();
    // 后台加载线性高度图，完成后放入纹理资产并交给几何面板。
    void import_height_file(const QString& path);
    // 导出选中物体的局部网格，进度和取消由几何任务控制器管理。
    void export_geometry_file(const QString& path);
    // 返回后台几何与文件任务的控制器。
    GeometryController* geometry_controller() const noexcept;
    // 返回选中物体的几何参数面板。
    GeometryPanel* geometry_panel() const noexcept;
    // 返回点云输入、法线和重建参数面板。
    PointCloudPanel* point_cloud_panel() const noexcept;
    // 返回独立距离场文档的编辑面板。
    SdfPanel* sdf_panel() const noexcept;
    // 返回后台布料模拟及完整帧发布控制器。
    ClothController* cloth_controller() const noexcept;
    // 创建指定布料示例与碰撞环境，作为可编辑场景安装。
    void create_cloth_demo(int kind);
    // 把当前布料姿态复制为普通网格，保留原模拟对象。
    void freeze_cloth();
    // 后台加载 PLY 点云，准备独立预览及后续重建输入。
    void import_point_file(const QString& path);
    // 从当前物体网格采样点云，并固定采样时的目标与参数。
    void sample_point_cloud();
    // 后台计算点云法线及定向，并保存可复用的法线缓存。
    void prepare_point_cloud_normals();
    // 按当前法线和半径执行重建，将结果作为普通网格提交到场景。
    void reconstruct_point_cloud();
    // 后台写出当前工具点云，场景网格与点云文件分别保存。
    void export_point_file(const QString& path);
    // 按标识选中物体，结束未完成操纵手势并刷新检查器。
    void select_object(const std::string& id);
    // 按颜色编码后台读取图像并生成 mipmap，目标材质由启动时的选择固定。
    void import_texture_file(const QString& path, minirender::ColorEncoding encoding,
                             bool normal_map = false);
    // 查询当前任务是否为底色纹理导入。
    bool texture_loading() const noexcept;
    // 查询是否有互斥资产任务正在执行。
    bool asset_loading() const noexcept;
    // 返回 CPU 离线任务的参数与控制面板。
    OfflinePanel* offline_panel() const noexcept;
    // 返回光场采集、文件和重聚焦任务控制器。
    LightFieldController* lightfield_controller() const noexcept;
    // 返回光场参数、进度及显示控件。
    LightFieldPanel* lightfield_panel() const noexcept;
    // 根据当前显示来源导出完整线性图像，离线结果使用其原始分辨率。
    void export_display_hdr(const QString& path);
    // 返回环境光预处理与缓存控制器。
    IblController* ibl_controller() const noexcept;
    // 后台读取环境纹理并更新场景引用，预处理由 IBL 控制器继续执行。
    void import_environment_file(const QString& path);

  private:
    // 安装模板后同步默认预览、选中物体及离线参数。
    void apply_builtin_settings(const BuiltinScene& preset);
    // 创建物体、材质和专用材质编辑控件，并绑定编辑事务。
    void build_object_panel();
    // 创建方向光选择与参数控件，编辑时更新所选光源。
    void build_light_panel();
    // 创建相机与显示控件，区分场景相机参数和视口显示设置。
    void build_camera_panel();
    // 从编辑器快照同步物体、资产、视口与检查器状态。
    void refresh();
    // 把选中物体写入控件；刷新期间抑制控件产生新的编辑事务。
    void refresh_properties();
    // 把当前场景相机写入面板，避免导航更新反向触发重复编辑。
    void refresh_camera();
    // 固定操纵前的对象与变换，并为一次可撤销手势建立事务。
    bool begin_transform();
    // 发布拖拽中的临时变换，保持当前手势的目标与历史一致。
    bool preview_transform(const minirender::Transform& transform);
    // 提交或回滚整次操纵手势，清理预览并同步布料上下文。
    void finish_transform(bool commit);
    // 执行文档编辑事务，更新版本号并同步视口；可复用已校验网格缓存。
    // 返回成功后，调用方可选中事务中新建的物体。
    bool perform_edit(const std::function<void(minirender::SceneDocument&)>& edit,
                      const minirender::MeshValidationCache*                 validated = nullptr);
    // 对选中对象执行属性编辑，复用统一的事务与刷新路径。
    void edit_object_property(const std::function<void(minirender::SceneObject&)>& edit);
    // 编辑选中对象引用的材质，同材质实例共享修改。
    void edit_material_property(const std::function<void(minirender::Material&)>& edit);
    // 复制选中对象，生成新标识并通过事务加入场景。
    void duplicate_object();
    // 删除选中对象并使该目标的后台结果失效。
    void delete_object();
    // 打开模型文件选择器并转入后台导入入口。
    void import_object();
    // 选择图像文件，根据扩展名确定颜色编码后转入后台导入。
    void import_texture();
    // 将异常转为用户可见的工作台错误信息。
    void report_error(const std::exception& error);
    // 进入资产互斥状态，更新任务提示并禁止冲突的操作入口。
    void begin_asset_task(const QString& message);
    // 结束资产互斥状态，恢复各面板和文件操作入口。
    void finish_asset_task();
    // 结束任务并启动结果提示计时，返回是否可以应用成功结果。
    bool finish_geometry_task(const GeometryTaskResult& result);
    // 绑定点云面板的导入、采样、法线、重建和导出回调。
    void connect_point_panel();
    // 同步点云重建目标与场景选择，防止结果覆盖已变化的网格。
    void update_point_selection();
    // 将当前文档、模拟目标和碰撞体同步到布料控制器。
    void sync_cloth_context();
    // 返回普通场景视图以显示布料运动，协调其他独立预览来源。
    void show_cloth_preview();
    // 验证帧对应的场景版本与目标，将完整模拟姿态发布给场景。
    bool accept_cloth_frame(const ClothRuntimeFrame& frame);
    // 通过事务应用布料定义，并按 reset 决定是否恢复静止状态。
    void apply_cloth_definition(std::shared_ptr<const minirender::ClothDefinition> definition,
                                bool                                               reset);
    // 固定当前可见场景与布料姿态，作为离线任务的不可变输入。
    std::shared_ptr<const minirender::SceneSnapshot> offline_snapshot();
    // 替换文档、重置旧目标和任务关联，统一更新文件路径与视口。
    void install_scene(minirender::SceneDocument document, const QString& path,
                       const minirender::MeshValidationCache* validated = nullptr);
    // 在给定文档中查找当前选中对象，目标不存在时返回空指针。
    minirender::SceneObject* selected(minirender::SceneDocument& scene) const;

    // 工作台页面路由器；在主窗口布局销毁前解除导航回调。
    WorkbenchPanels* panels_ = nullptr;
    // 显示和输入视口，由 Qt 窗口容器管理。
    VulkanViewport* viewport_ = nullptr;
    // 当前文档与撤销重做历史的唯一编辑入口。
    minirender::SceneEditor editor_{minirender::SceneDocument{}};
    // 场景物体列表，保存选中和可见状态。
    QTreeWidget* tree_ = nullptr;
    // 网格及纹理资产列表。
    QTreeWidget* assets_ = nullptr;
    // 物体属性页的根控件。
    QWidget* object_panel_ = nullptr;
    // 全局方向光属性页。
    QWidget* light_panel_ = nullptr;
    // 全局相机和显示设置页。
    QWidget* camera_panel_ = nullptr;
    // CPU 离线渲染的控件与任务入口。
    OfflinePanel* offline_ = nullptr;
    // 环境图选择、强度与旋转控件。
    EnvironmentPanel* environment_ = nullptr;
    // 环境预处理任务和结果缓存。
    IblController* ibl_ = nullptr;
    // 共享的几何与资产后台任务控制器。
    GeometryController* geometry_tasks_ = nullptr;
    // 当前物体的细分、位移、简化与重网格设置。
    GeometryPanel* geometry_ = nullptr;
    // 独立点云工具的输入、预览与重建参数。
    PointCloudPanel* points_ = nullptr;
    // 距离场文档及其独立预览配置。
    SdfPanel* sdf_ = nullptr;
    // 织物经纬模型的专用材质控件。
    ClothMaterialPanel* cloth_material_ = nullptr;
    // 次表面材质参数草稿与提交控件。
    SubsurfaceMaterialPanel* subsurface_material_ = nullptr;
    QLabel*                  material_notice_     = nullptr;
    ClothColliderPanel*      cloth_collider_      = nullptr;
    ClothController*         cloth_controller_    = nullptr;
    ClothPanel*              cloth_panel_         = nullptr;
    VolumePanel*             volume_              = nullptr;
    LightFieldPanel*         lightfield_panel_    = nullptr;
    LightFieldController*    lightfield_          = nullptr;
    // 通知离线和光场面板，已有任务使用的场景快照已过期。
    void mark_snapshot_changed();
    std::string cloth_target_;
    // 当前物体名称输入框。
    QLineEdit* object_name_ = nullptr;
    // 选中物体的三轴平移，单位为米。
    VectorField* translation_ = nullptr;
    // 选中物体的欧拉角，单位为度。
    VectorField* rotation_ = nullptr;
    // 选中物体的三轴缩放。
    VectorField* scale_ = nullptr;
    // 材质线性 RGB 底色。
    VectorField* base_color_ = nullptr;
    // 材质线性 RGB 自发光强度。
    VectorField* emission_ = nullptr;
    // 基础与专用材质类型选择器。
    QComboBox* material_kind_ = nullptr;
    // 材质底色纹理资产选择器。
    QComboBox*      normal_selector_  = nullptr;
    QDoubleSpinBox* normal_strength_  = nullptr;
    QCheckBox*      normal_flip_      = nullptr;
    QComboBox*      texture_selector_ = nullptr;
    // 底色纹理的采样过滤方式。
    QComboBox* texture_filter_ = nullptr;
    // 材质双面显示开关。
    QCheckBox*   energy_compensation_   = nullptr;
    QCheckBox*   double_sided_          = nullptr;
    QAction*     import_texture_action_ = nullptr;
    QAction*     import_object_action_  = nullptr;
    QAction*     open_action_           = nullptr;
    QToolButton* builtin_button_        = nullptr;
    QAction*     save_action_           = nullptr;
    QAction*     save_as_action_        = nullptr;
    // 状态栏中的任务进度控件。
    QProgressBar* task_progress_ = nullptr;
    // 状态栏中的任务阶段标签。
    QLabel* task_label_ = nullptr;
    // 当前资产任务的用户可见阶段文字。
    QString task_message_;
    // 几何任务结果的补充统计说明。
    QString geometry_notice_extra_;
    // 几何任务结束提示的持续时间计时器。
    QElapsedTimer geometry_notice_;
    // 资产任务互斥锁的 GUI 状态。
    bool asset_loading_ = false;
    // 标识正在执行的底色纹理读取。
    bool texture_loading_ = false;
    // 当前操纵手势的暂存变换。
    std::optional<minirender::Transform> transform_preview_;
    // 场景替换代号，用于拒绝旧场景后台结果。
    std::uint64_t document_generation_ = 0;
    // 同一场景内的修改版本，用于标记任务快照过期。
    std::uint64_t document_revision_ = 0;
    // 材质表面粗糙度输入。
    QDoubleSpinBox* roughness_ = nullptr;
    // 介质界面的折射率输入。
    QDoubleSpinBox* ior_ = nullptr;
    // 当前编辑的方向光。
    QComboBox* light_selector_ = nullptr;
    // 方向光的世界空间方向。
    VectorField* light_direction_ = nullptr;
    // 方向光线性 RGB 颜色。
    VectorField* light_color_ = nullptr;
    // 方向光强度倍率。
    QDoubleSpinBox* light_intensity_ = nullptr;
    // 相机世界位置。
    VectorField* camera_position_ = nullptr;
    // 相机观察目标点。
    VectorField* camera_target_ = nullptr;
    // 相机垂直视场角，单位为度。
    QDoubleSpinBox* fov_ = nullptr;
    // 薄透镜孔径半径，单位为米。
    QDoubleSpinBox* aperture_ = nullptr;
    // 薄透镜参考焦面距离，单位为米。
    QDoubleSpinBox* focus_ = nullptr;
    // 光照显示模式使用的曝光 EV。
    QDoubleSpinBox* exposure_ = nullptr;
    // 撤销操作入口，启用状态随编辑历史更新。
    QAction* undo_ = nullptr;
    // 重做操作入口，启用状态随编辑历史更新。
    QAction* redo_ = nullptr;
    // 当前场景选择的稳定对象标识。
    std::string selected_id_;
    // 当前保存位置；模板副本首次保存时选择用户路径。
    QString scene_path_;
    // 控件刷新保护，防止程序写值触发递归编辑。
    bool refreshing_ = false;
    // 相机面板已有合并后的延迟刷新请求。
    bool camera_refresh_pending_ = false;
    // 当前窗口是否使用基础 Vulkan 诊断场景。
    bool diagnostic_ = false;
};
