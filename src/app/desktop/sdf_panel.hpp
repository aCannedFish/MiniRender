// 距离场参数控件、状态刷新与用户操作回调。

#pragma once

#include <QWidget>
#include <minirender/geometry/sdf.hpp>

#include <functional>

class VulkanViewport;
class QComboBox;
class QLineEdit;
class QLabel;
class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;
class QPushButton;
class QFormLayout;
class SdfVectorField;

// SDF 文档有独立历史与文件入口，参数修改先验证副本，再发布不可变渲染快照。
// 主场景编辑器保留自己的资产、历史和相机，页面切换不提交主场景事务。
class SdfPanel final : public QWidget {
  public:
    // 建立距离场工作页，并绑定发布快照的视口。
    explicit SdfPanel(VulkanViewport& viewport, QWidget* parent = nullptr);
    // 借用工作页当前正在编辑的距离场文档。
    const minirender::SdfDocument& document() const noexcept;
    // 读取距离场文件，将结果作为一次文档提交并刷新视口。
    void load_file(const QString& path);
    // 将当前距离场、相机和光照写入选定文件。
    void save_file(const QString& path);

  private:
    // 建立文件、历史及根节点选择控件。
    void build_document_controls(QFormLayout* layout);
    // 建立图元变换、材质和 CSG 子节点控件。
    void build_node_controls(QFormLayout* layout);
    // 建立光照、诊断视图和射线步进参数控件。
    void build_render_controls(QFormLayout* layout);
    // 建立带有稳定对象名的按钮，点击时通过 invoke 报告操作错误。
    QPushButton* button(const QString& text, const char* name, const std::function<void()>& work);
    // 执行按钮操作，在面板中显示异常信息。
    void invoke(const std::function<void()>& work);
    // 在文档副本上应用修改，失败时按原文档恢复控件。
    void edit(const std::function<void(minirender::SdfDocument&)>& work, bool rebuild_list = false);
    // 校验候选文档，写入撤销历史，发布快照并刷新控件。
    void commit(minirender::SdfDocument next, bool reset_camera, bool rebuild_list);
    // 从文档同步选择和参数，rebuild_list 控制是否重建节点列表。
    void refresh(bool rebuild_list);
    // 向视口发布只读文档副本，可同时采用文档相机。
    void publish(bool reset_camera = false);
    // 添加带默认参数的图元，并选中新节点。
    void append_primitive(minirender::SdfKind kind);
    // 以原根和选中节点为左右子节点，将新运算节点设为根。
    void combine(minirender::SdfKind kind);
    // 删除未被引用的非根节点，并修正受影响的节点索引。
    void remove_node();
    // 在撤销、重做栈之间移动文档快照，并重新发布当前文档。
    void restore_history(bool redo);
    // 取得节点下拉框中选中项的文档索引。
    std::uint32_t selected_index() const;
    // 选择文档中第一个尚未使用的 sdf-N 标识。
    std::string new_id(const minirender::SdfDocument& document) const;
    // 视口由工作台持有。文档及其两份历史栈归本面板管理。
    VulkanViewport&                      viewport_;
    minirender::SdfDocument              document_;
    std::vector<minirender::SdfDocument> undo_, redo_;
    bool                                 refreshing_ = false;
    // Qt 对象树持有下列控件，refreshing_ 阻止刷新时产生新的编辑记录。
    QComboBox *     nodes_ = nullptr, *root_ = nullptr, *left_ = nullptr, *right_ = nullptr;
    QComboBox*      view_    = nullptr;
    QLineEdit*      name_    = nullptr;
    QCheckBox *     visible_ = nullptr, *shadows_ = nullptr;
    QLabel *        notice_ = nullptr, *kind_ = nullptr;
    QPushButton *   undo_button_ = nullptr, *redo_button_ = nullptr, *remove_ = nullptr;
    QWidget *       primitive_ = nullptr, *operation_ = nullptr;
    SdfVectorField *parameters_ = nullptr, *position_ = nullptr, *rotation_ = nullptr;
    SdfVectorField *color_ = nullptr, *light_ = nullptr, *light_color_ = nullptr,
                   *ambient_ = nullptr;
    QDoubleSpinBox *scale_ = nullptr, *intensity_ = nullptr;
    QDoubleSpinBox *epsilon_ = nullptr, *relative_ = nullptr, *normal_ = nullptr;
    QDoubleSpinBox *distance_ = nullptr, *safety_ = nullptr;
    QSpinBox *      steps_ = nullptr, *shadow_steps_ = nullptr;
};
