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
    explicit SdfPanel(VulkanViewport& viewport, QWidget* parent = nullptr);
    const minirender::SdfDocument& document() const noexcept;
    void load_file(const QString& path);
    void save_file(const QString& path);

  private:
    void build_document_controls(QFormLayout* layout);
    void build_node_controls(QFormLayout* layout);
    void build_render_controls(QFormLayout* layout);
    QPushButton* button(const QString& text, const char* name, const std::function<void()>& work);
    void invoke(const std::function<void()>& work);
    void edit(const std::function<void(minirender::SdfDocument&)>& work, bool rebuild_list = false);
    void commit(minirender::SdfDocument next, bool reset_camera, bool rebuild_list);
    void refresh(bool rebuild_list);
    void publish(bool reset_camera = false);
    void append_primitive(minirender::SdfKind kind);
    void combine(minirender::SdfKind kind);
    void remove_node();
    void restore_history(bool redo);
    std::uint32_t selected_index() const;
    std::string new_id(const minirender::SdfDocument& document) const;
    VulkanViewport& viewport_;
    minirender::SdfDocument document_;
    std::vector<minirender::SdfDocument> undo_, redo_;
    bool refreshing_ = false;
    QComboBox *nodes_ = nullptr, *root_ = nullptr, *left_ = nullptr, *right_ = nullptr;
    QComboBox* view_ = nullptr;
    QLineEdit* name_ = nullptr;
    QCheckBox *visible_ = nullptr, *shadows_ = nullptr;
    QLabel *notice_ = nullptr, *kind_ = nullptr;
    QPushButton *undo_button_ = nullptr, *redo_button_ = nullptr, *remove_ = nullptr;
    QWidget *primitive_ = nullptr, *operation_ = nullptr;
    SdfVectorField *parameters_ = nullptr, *position_ = nullptr, *rotation_ = nullptr;
    SdfVectorField *color_ = nullptr, *light_ = nullptr, *light_color_ = nullptr,
                   *ambient_ = nullptr;
    QDoubleSpinBox *scale_ = nullptr, *intensity_ = nullptr;
    QDoubleSpinBox *epsilon_ = nullptr, *relative_ = nullptr, *normal_ = nullptr;
    QDoubleSpinBox *distance_ = nullptr, *safety_ = nullptr;
    QSpinBox *steps_ = nullptr, *shadow_steps_ = nullptr;
};
