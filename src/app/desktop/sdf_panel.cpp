#include "sdf_panel.hpp"
#include "vulkan_viewport.hpp"

#include <minirender/assets/sdf_io.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace {

QDoubleSpinBox* scalar(QWidget* parent, const QString& name, double low, double high,
                       double step = .1, int decimals = 4) {
    auto* spin = new QDoubleSpinBox(parent);
    spin->setObjectName(name);
    spin->setRange(low, high);
    spin->setDecimals(decimals);
    spin->setSingleStep(step);
    spin->setKeyboardTracking(false);
    spin->setLocale(QLocale::c());
    spin->setMinimumWidth(82);
    spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    return spin;
}
QFormLayout* form(QWidget* widget) {
    auto* result = new QFormLayout(widget);
    result->setContentsMargins(0, 0, 0, 0);
    result->setVerticalSpacing(12);
    result->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    result->setRowWrapPolicy(QFormLayout::WrapLongRows);
    return result;
}
QWidget* buttons(QWidget* parent, std::initializer_list<QPushButton*> values) {
    auto* widget = new QWidget(parent);
    auto* row = new QHBoxLayout(widget);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    for (auto* button : values) {
        row->addWidget(button);
    }
    return widget;
}

} // namespace

// SDF 参数采用相同的三列数值布局。球只显示 X，圆环显示 R/r，盒显示三个半长。
class SdfVectorField final : public QWidget {
  public:
    SdfVectorField(QWidget* parent, const QString& name, double low, double high, double step = .1)
        : QWidget(parent) {
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        for (std::size_t i = 0; i < values_.size(); ++i) {
            const auto axis = QString(QChar('x' + static_cast<char>(i)));
            values_[i] = scalar(this, name + '.' + axis, low, high, step);
            values_[i]->setPrefix(axis + ' ');
            row->addWidget(values_[i], 1);
            connect(values_[i], &QDoubleSpinBox::valueChanged, this, [this, i] {
                if (changed) {
                    changed(static_cast<int>(i));
                }
            });
        }
    }
    glm::vec3 value() const {
        return {float(values_[0]->value()), float(values_[1]->value()), float(values_[2]->value())};
    }
    void set(glm::vec3 value) {
        for (int i = 0; i < 3; ++i) {
            const QSignalBlocker blocker(values_[std::size_t(i)]);
            values_[std::size_t(i)]->setValue(value[i]);
        }
    }
    void components(int count) {
        for (int i = 0; i < 3; ++i) {
            values_[std::size_t(i)]->setVisible(i < count);
        }
    }
    std::function<void(int)> changed;

  private:
    std::array<QDoubleSpinBox*, 3> values_{};
};

SdfPanel::SdfPanel(VulkanViewport& viewport, QWidget* parent)
    : QWidget(parent), viewport_(viewport), document_(minirender::make_sdf_example()) {
    setObjectName("sdf_panel");
    auto* layout = form(this);
    layout->setContentsMargins(10, 12, 10, 12);
    build_document_controls(layout);
    build_node_controls(layout);
    build_render_controls(layout);
    notice_ = new QLabel(this);
    notice_->setObjectName("sdf_notice");
    notice_->setWordWrap(true);
    notice_->setTextFormat(Qt::PlainText);
    layout->addRow(notice_);
    publish();
    refresh(true);
}

QPushButton* SdfPanel::button(const QString& text, const char* name,
                              const std::function<void()>& work) {
    auto* value = new QPushButton(text, this);
    value->setObjectName(name);
    connect(value, &QPushButton::clicked, this, work);
    return value;
}

void SdfPanel::invoke(const std::function<void()>& work) {
    try {
        work();
    } catch (const std::exception& error) {
        notice_->setText(QString::fromUtf8(error.what()));
    }
}

void SdfPanel::build_document_controls(QFormLayout* layout) {
    auto* example = button("示例", "sdf_example", [this] {
        commit(minirender::make_sdf_example(), true, true);
        viewport_.set_sdf_visible(true);
    });
    auto* open = button("打开", "sdf_open", [this] {
        const auto path =
            QFileDialog::getOpenFileName(this, "打开距离场", {}, "SDF (*.sdf.json *.json)");
        if (!path.isEmpty()) {
            invoke([&] { load_file(path); });
        }
    });
    auto* save = button("保存", "sdf_save", [this] {
        const auto path =
            QFileDialog::getSaveFileName(this, "保存距离场", "scene.sdf.json", "SDF (*.sdf.json)");
        if (!path.isEmpty()) {
            invoke([&] { save_file(path); });
        }
    });
    layout->addRow(buttons(this, {example, open, save}));
    visible_ = new QCheckBox("显示距离场", this);
    visible_->setObjectName("sdf_visible");
    layout->addRow(visible_);
    connect(visible_, &QCheckBox::toggled, this,
            [this](bool value) { viewport_.set_sdf_visible(value); });
    viewport_.on_sdf_visible_changed = [this](bool visible) {
        const QSignalBlocker blocker(visible_);
        visible_->setChecked(visible);
    };
    view_ = new QComboBox(this);
    view_->setObjectName("sdf_view");
    view_->addItems({"表面光照", "法线", "深度", "步数热图"});
    connect(view_, &QComboBox::currentIndexChanged, this,
            [this](int value) { viewport_.set_sdf_view(static_cast<minirender::SdfView>(value)); });
    layout->addRow("视图", view_);
    shadows_ = new QCheckBox("方向光阴影", this);
    shadows_->setObjectName("sdf_shadows");
    shadows_->setChecked(true);
    connect(shadows_, &QCheckBox::toggled, this,
            [this](bool value) { viewport_.set_sdf_shadows(value); });
    layout->addRow(shadows_);
    viewport_.on_sdf_display_changed = [this] {
        const QSignalBlocker view_blocker(view_);
        const QSignalBlocker shadow_blocker(shadows_);
        view_->setCurrentIndex(static_cast<int>(viewport_.sdf_view()));
        shadows_->setChecked(viewport_.sdf_shadows());
    };
    auto* reset_camera = button("重置视角", "sdf_reset_camera", [this] {
        if (viewport_.sdf_visible()) {
            viewport_.set_camera(document_.camera);
        } else {
            publish(true);
        }
    });
    layout->addRow(reset_camera);
    undo_button_ = button("撤销", "sdf_undo", [this] { restore_history(false); });
    redo_button_ = button("重做", "sdf_redo", [this] { restore_history(true); });
    layout->addRow(buttons(this, {undo_button_, redo_button_}));
}

void SdfPanel::build_node_controls(QFormLayout* layout) {
    nodes_ = new QComboBox(this);
    nodes_->setObjectName("sdf_nodes");
    root_ = new QComboBox(this);
    root_->setObjectName("sdf_root");
    layout->addRow("编辑节点", nodes_);
    layout->addRow("根节点", root_);
    auto* hint = new QLabel("新增节点后，可将它设为根节点，或与当前根节点进行布尔组合。", this);
    hint->setWordWrap(true);
    hint->setTextFormat(Qt::PlainText);
    layout->addRow(hint);
    connect(nodes_, &QComboBox::currentIndexChanged, this, [this] {
        if (!refreshing_) {
            refresh(false);
        }
    });
    connect(root_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!refreshing_) {
            edit([&](auto& doc) { doc.root = std::uint32_t(index); });
        }
    });
    name_ = new QLineEdit(this);
    name_->setObjectName("sdf_name");
    name_->setMaxLength(128);
    layout->addRow("名称", name_);
    connect(name_, &QLineEdit::editingFinished, this, [this] {
        if (!refreshing_ && name_->text().toStdString() != document_.nodes[selected_index()].name) {
            edit([&](auto& doc) { doc.nodes[selected_index()].name = name_->text().toStdString(); },
                 true);
        }
    });
    kind_ = new QLabel(this);
    kind_->setTextFormat(Qt::PlainText);
    layout->addRow("类型", kind_);
    auto* sphere =
        button("球", "sdf_add_sphere", [this] { append_primitive(minirender::SdfKind::sphere); });
    auto* box = button("盒", "sdf_add_box", [this] { append_primitive(minirender::SdfKind::box); });
    auto* torus =
        button("圆环", "sdf_add_torus", [this] { append_primitive(minirender::SdfKind::torus); });
    layout->addRow("新增", buttons(this, {sphere, box, torus}));
    auto* unite = button("并集", "sdf_union", [this] { combine(minirender::SdfKind::unite); });
    auto* intersect =
        button("交集", "sdf_intersection", [this] { combine(minirender::SdfKind::intersect); });
    auto* subtract =
        button("差集", "sdf_difference", [this] { combine(minirender::SdfKind::subtract); });
    layout->addRow("根与所选", buttons(this, {unite, intersect, subtract}));
    remove_ = button("删除未引用节点", "sdf_remove", [this] { invoke([&] { remove_node(); }); });
    layout->addRow(remove_);

    primitive_ = new QWidget(this);
    auto* geometry = form(primitive_);
    parameters_ = new SdfVectorField(primitive_, "sdf_parameters", 0, 1e6);
    position_ = new SdfVectorField(primitive_, "sdf_position", -1e6, 1e6);
    rotation_ = new SdfVectorField(primitive_, "sdf_rotation", -3600, 3600, 5);
    color_ = new SdfVectorField(primitive_, "sdf_color", 0, 1, .05);
    scale_ = scalar(primitive_, "sdf_scale", .000001, 1e6, .1, 6);
    geometry->addRow("半径 / 半长", parameters_);
    geometry->addRow("位置", position_);
    geometry->addRow("旋转 °", rotation_);
    geometry->addRow("均匀缩放", scale_);
    geometry->addRow("颜色 RGB", color_);
    // 每次只提交实际编辑的分量，保留加载文件中其他参数的完整精度。
    const auto bind_node_vector = [this](SdfVectorField* field,
                                         glm::vec3 minirender::SdfNode::* member) {
        field->changed = [this, field, member](int axis) {
            if (!refreshing_) {
                edit([&](auto& doc) {
                    (doc.nodes[selected_index()].*member)[axis] = field->value()[axis];
                });
            }
        };
    };
    bind_node_vector(parameters_, &minirender::SdfNode::parameters);
    bind_node_vector(position_, &minirender::SdfNode::translation);
    bind_node_vector(rotation_, &minirender::SdfNode::rotation_degrees);
    bind_node_vector(color_, &minirender::SdfNode::color);
    connect(scale_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (!refreshing_) {
            edit([&](auto& doc) { doc.nodes[selected_index()].scale = float(value); });
        }
    });
    layout->addRow(primitive_);
    operation_ = new QWidget(this);
    auto* operation = form(operation_);
    left_ = new QComboBox(operation_);
    left_->setObjectName("sdf_left");
    right_ = new QComboBox(operation_);
    right_->setObjectName("sdf_right");
    operation->addRow("左侧 A", left_);
    operation->addRow("右侧 B", right_);
    const auto children_changed = [this] {
        if (!refreshing_) {
            edit([&](auto& doc) {
                auto& node = doc.nodes[selected_index()];
                node.left = std::uint32_t(left_->currentIndex());
                node.right = std::uint32_t(right_->currentIndex());
            });
        }
    };
    connect(left_, &QComboBox::currentIndexChanged, this, children_changed);
    connect(right_, &QComboBox::currentIndexChanged, this, children_changed);
    layout->addRow(operation_);
}

void SdfPanel::build_render_controls(QFormLayout* layout) {
    light_ = new SdfVectorField(this, "sdf_light_direction", -1000, 1000);
    light_color_ = new SdfVectorField(this, "sdf_light_color", 0, 100, .05);
    ambient_ = new SdfVectorField(this, "sdf_ambient", 0, 100, .01);
    intensity_ = scalar(this, "sdf_light_intensity", 0, 1000);
    layout->addRow("光方向", light_);
    layout->addRow("光颜色", light_color_);
    layout->addRow("强度", intensity_);
    layout->addRow("环境 RGB", ambient_);
    light_->changed = [this](int axis) {
        if (!refreshing_) {
            edit([&](auto& doc) { doc.light.direction[axis] = light_->value()[axis]; });
        }
    };
    light_color_->changed = [this](int axis) {
        if (!refreshing_) {
            edit([&](auto& doc) { doc.light.color[axis] = light_color_->value()[axis]; });
        }
    };
    ambient_->changed = [this](int axis) {
        if (!refreshing_) {
            edit([&](auto& doc) { doc.ambient[axis] = ambient_->value()[axis]; });
        }
    };
    connect(intensity_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (!refreshing_) {
            edit([&](auto& doc) { doc.light.intensity = float(value); });
        }
    });
    steps_ = new QSpinBox(this);
    steps_->setObjectName("sdf_steps");
    steps_->setRange(1, 4096);
    steps_->setKeyboardTracking(false);
    shadow_steps_ = new QSpinBox(this);
    shadow_steps_->setObjectName("sdf_shadow_steps");
    shadow_steps_->setRange(1, 1024);
    shadow_steps_->setKeyboardTracking(false);
    epsilon_ = scalar(this, "sdf_epsilon", 1e-7, .1, .0001, 7);
    relative_ = scalar(this, "sdf_relative_epsilon", 0, .01, .00001, 7);
    normal_ = scalar(this, "sdf_normal_step", 1e-6, .1, .0001, 6);
    distance_ = scalar(this, "sdf_maximum_distance", .001, 1e6, 1);
    safety_ = scalar(this, "sdf_safety", .1, 1, .05);
    layout->addRow("最大步数", steps_);
    layout->addRow("阴影步数", shadow_steps_);
    layout->addRow("绝对容差 m", epsilon_);
    layout->addRow("相对容差", relative_);
    layout->addRow("法线差分 m", normal_);
    layout->addRow("最远距离 m", distance_);
    layout->addRow("安全系数", safety_);
    connect(steps_, &QSpinBox::valueChanged, this, [this](int value) {
        if (!refreshing_) {
            edit([&](auto& doc) { doc.trace.maximum_steps = std::uint32_t(value); });
        }
    });
    connect(shadow_steps_, &QSpinBox::valueChanged, this, [this](int value) {
        if (!refreshing_) {
            edit([&](auto& doc) { doc.trace.shadow_steps = std::uint32_t(value); });
        }
    });
    const auto bind_trace = [this](QDoubleSpinBox* spin,
                                   double minirender::SdfTraceSettings::* member) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, member](double value) {
            if (!refreshing_) {
                edit([&](auto& doc) { doc.trace.*member = value; });
            }
        });
    };
    bind_trace(epsilon_, &minirender::SdfTraceSettings::epsilon);
    bind_trace(relative_, &minirender::SdfTraceSettings::relative_epsilon);
    bind_trace(normal_, &minirender::SdfTraceSettings::normal_step);
    bind_trace(distance_, &minirender::SdfTraceSettings::maximum_distance);
    bind_trace(safety_, &minirender::SdfTraceSettings::safety_factor);
}

const minirender::SdfDocument& SdfPanel::document() const noexcept {
    return document_;
}
std::uint32_t SdfPanel::selected_index() const {
    return std::uint32_t(std::max(0, nodes_->currentIndex()));
}
std::string SdfPanel::new_id(const minirender::SdfDocument& document) const {
    for (std::uint32_t index = 1;; ++index) {
        auto id = "sdf-" + std::to_string(index);
        if (std::none_of(document.nodes.begin(), document.nodes.end(),
                         [&](const auto& node) { return node.id == id; })) {
            return id;
        }
    }
}
void SdfPanel::publish(bool reset_camera) {
    viewport_.set_sdf_document(std::make_shared<const minirender::SdfDocument>(document_),
                               reset_camera);
}
void SdfPanel::commit(minirender::SdfDocument next, bool reset_camera, bool rebuild_list) {
    next.validate();
    undo_.push_back(document_);
    if (undo_.size() > 64) {
        undo_.erase(undo_.begin());
    }
    redo_.clear();
    document_ = std::move(next);
    publish(reset_camera);
    refresh(rebuild_list);
    notice_->clear();
}
void SdfPanel::edit(const std::function<void(minirender::SdfDocument&)>& work, bool rebuild_list) {
    try {
        auto next = document_;
        work(next);
        commit(std::move(next), false, rebuild_list);
    } catch (const std::exception& error) {
        refresh(false);
        notice_->setText(QString::fromUtf8(error.what()));
    }
}
void SdfPanel::refresh(bool rebuild_list) {
    refreshing_ = true;
    auto selected = std::min(selected_index(), std::uint32_t(document_.nodes.size() - 1));
    if (rebuild_list) {
        for (auto* combo : {nodes_, root_, left_, right_}) {
            combo->clear();
            for (const auto& node : document_.nodes) {
                combo->addItem(QString::fromStdString(node.name + " [" + node.id + "]"));
            }
        }
        nodes_->setCurrentIndex(int(selected));
    }
    root_->setCurrentIndex(int(document_.root));
    const auto& node = document_.nodes[selected];
    name_->setText(QString::fromStdString(node.name));
    kind_->setText(QString::fromStdString(minirender::sdf_kind_name(node.kind)));
    const bool primitive = node.kind <= minirender::SdfKind::torus;
    primitive_->setVisible(primitive);
    operation_->setVisible(!primitive);
    parameters_->components(node.kind == minirender::SdfKind::sphere  ? 1
                            : node.kind == minirender::SdfKind::torus ? 2
                                                                      : 3);
    parameters_->set(node.parameters);
    position_->set(node.translation);
    rotation_->set(node.rotation_degrees);
    color_->set(node.color);
    scale_->setValue(node.scale);
    if (!primitive) {
        left_->setCurrentIndex(int(node.left));
        right_->setCurrentIndex(int(node.right));
    }
    light_->set(document_.light.direction);
    light_color_->set(document_.light.color);
    ambient_->set(document_.ambient);
    intensity_->setValue(document_.light.intensity);
    steps_->setValue(int(document_.trace.maximum_steps));
    shadow_steps_->setValue(int(document_.trace.shadow_steps));
    epsilon_->setValue(document_.trace.epsilon);
    relative_->setValue(document_.trace.relative_epsilon);
    normal_->setValue(document_.trace.normal_step);
    distance_->setValue(document_.trace.maximum_distance);
    safety_->setValue(document_.trace.safety_factor);
    undo_button_->setEnabled(!undo_.empty());
    redo_button_->setEnabled(!redo_.empty());
    refreshing_ = false;
}
void SdfPanel::append_primitive(minirender::SdfKind kind) {
    edit(
        [&](auto& doc) {
            minirender::SdfNode node;
            node.id = new_id(doc);
            node.name = minirender::sdf_kind_name(kind);
            node.kind = kind;
            node.parameters =
                kind == minirender::SdfKind::torus ? glm::vec3(.65F, .18F, 0) : glm::vec3(.5F);
            node.translation = {0, 1, 0};
            doc.nodes.push_back(std::move(node));
        },
        true);
    nodes_->setCurrentIndex(int(document_.nodes.size() - 1));
}
void SdfPanel::combine(minirender::SdfKind kind) {
    const auto right = selected_index();
    edit(
        [&](auto& doc) {
            minirender::SdfNode node;
            node.id = new_id(doc);
            node.name = minirender::sdf_kind_name(kind);
            node.kind = kind;
            node.left = doc.root;
            node.right = right;
            doc.nodes.push_back(std::move(node));
            doc.root = std::uint32_t(doc.nodes.size() - 1);
        },
        true);
    nodes_->setCurrentIndex(int(document_.root));
}
void SdfPanel::remove_node() {
    const auto index = selected_index();
    if (index == document_.root ||
        std::any_of(document_.nodes.begin(), document_.nodes.end(),
                    [&](const auto& node) { return node.left == index || node.right == index; })) {
        throw std::invalid_argument("请先更换根节点及其他节点对该节点的引用。");
    }
    edit(
        [&](auto& doc) {
            doc.nodes.erase(doc.nodes.begin() + index);
            if (doc.root > index) {
                --doc.root;
            }
            for (auto& node : doc.nodes) {
                if (node.left != minirender::invalid_sdf_node && node.left > index) {
                    --node.left;
                }
                if (node.right != minirender::invalid_sdf_node && node.right > index) {
                    --node.right;
                }
            }
        },
        true);
}
void SdfPanel::restore_history(bool redo) {
    auto& source = redo ? redo_ : undo_;
    auto& destination = redo ? undo_ : redo_;
    if (source.empty()) {
        return;
    }
    destination.push_back(document_);
    document_ = std::move(source.back());
    source.pop_back();
    publish();
    refresh(true);
    notice_->clear();
}
void SdfPanel::load_file(const QString& path) {
    commit(minirender::load_sdf(std::filesystem::path(path.toStdU16String())), true, true);
    viewport_.set_sdf_visible(true);
    notice_->setText("已打开独立距离场文档。");
}
void SdfPanel::save_file(const QString& path) {
    auto snapshot = document_;
    snapshot.camera = viewport_.sdf_camera();
    const auto destination = std::filesystem::path(path.toStdU16String());
    if (std::filesystem::exists(destination)) {
        (void)minirender::load_sdf(destination);
    }
    minirender::save_sdf(snapshot, destination);
    notice_->setText("距离场和当前视角已保存。");
}
