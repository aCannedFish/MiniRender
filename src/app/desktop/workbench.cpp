#include "workbench.hpp"

#include <minirender/assets/scene_io.hpp>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVulkanInstance>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>

namespace {

// 工作线程只处理文件和 CPU 数据，通过 watcher 在 UI 线程提交结果。
struct TextureLoadResult {
    std::shared_ptr<const minirender::Texture> texture;
    std::string error;
};

std::filesystem::path native_path(const QString& path) {
    const auto bytes = path.toUtf8();
    return std::filesystem::path(std::u8string(bytes.begin(), bytes.end()));
}

template <typename Item>
std::string unique_id(const std::vector<Item>& values, const std::string& prefix) {
    for (unsigned index = 1;; ++index) {
        const auto id = prefix + std::to_string(index);
        if (std::none_of(values.begin(), values.end(),
                         [&](const Item& value) { return value.id == id; })) {
            return id;
        }
    }
}

QDoubleSpinBox* spin(QWidget* parent, const QString& name, double minimum, double maximum,
                     double step) {
    auto* value = new QDoubleSpinBox(parent);
    value->setObjectName(name);
    value->setRange(minimum, maximum);
    value->setDecimals(4);
    value->setSingleStep(step);
    value->setKeyboardTracking(false);
    value->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return value;
}

QScrollArea* scroll_panel(QWidget* panel) {
    auto* area = new QScrollArea;
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    area->setWidget(panel);
    return area;
}

void configure_form(QFormLayout* form) {
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignTop | Qt::AlignLeft);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);
    form->setContentsMargins(10, 12, 10, 12);
}

} // namespace

// 三分量编辑控件只处理数值输入，不依赖场景。editingFinished 才发布一次事务。
class VectorField final : public QWidget {
  public:
    VectorField(QWidget* parent, const QString& prefix, double minimum = -10000,
                double maximum = 10000)
        : QWidget(parent) {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        const std::array<QString, 3> axes{"x", "y", "z"};
        for (std::size_t index = 0; index < values_.size(); ++index) {
            values_[index] = spin(this, prefix + "." + axes[index], minimum, maximum, 0.1);
            values_[index]->setPrefix(axes[index] + " ");
            values_[index]->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            values_[index]->setMinimumWidth(88);
            layout->addWidget(values_[index], 1);
            connect(values_[index], &QDoubleSpinBox::editingFinished, this, [this] {
                if (on_changed) {
                    on_changed();
                }
            });
        }
    }
    glm::vec3 value() const {
        return {static_cast<float>(values_[0]->value()), static_cast<float>(values_[1]->value()),
                static_cast<float>(values_[2]->value())};
    }
    void set_value(glm::vec3 value) {
        for (int index = 0; index < 3; ++index) {
            const QSignalBlocker blocker(values_[static_cast<std::size_t>(index)]);
            values_[static_cast<std::size_t>(index)]->setValue(value[index]);
        }
    }
    std::function<void()> on_changed;

  private:
    std::array<QDoubleSpinBox*, 3> values_;
};

Workbench::Workbench(QVulkanInstance* instance, bool diagnostic) : diagnostic_(diagnostic) {
    setWindowTitle("MiniRender");
    viewport_ = new VulkanViewport;
    viewport_->setVulkanInstance(instance);
    auto* container = QWidget::createWindowContainer(viewport_, this);
    container->setFocusPolicy(Qt::StrongFocus);
    container->setMinimumSize(320, 240);
    setCentralWidget(container);
    auto* toolbar = addToolBar("项目");
    auto* open = toolbar->addAction("打开");
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, "打开场景", {}, "Scene (*.json)");
        if (!path.isEmpty()) {
            try {
                load_scene_file(path);
            } catch (const std::exception& error) {
                report_error(error);
            }
        }
    });
    auto* save = toolbar->addAction("保存");
    save->setShortcut(QKeySequence::Save);
    connect(save, &QAction::triggered, this, [this] {
        auto path = scene_path_;
        if (path.isEmpty()) {
            path = QFileDialog::getSaveFileName(this, "保存场景", {}, "Scene (*.json)");
        }
        if (!path.isEmpty()) {
            try {
                save_scene_file(path);
            } catch (const std::exception& error) {
                report_error(error);
            }
        }
    });
    auto* save_as = toolbar->addAction("另存");
    connect(save_as, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, "另存场景", {}, "Scene (*.json)");
        if (!path.isEmpty()) {
            try {
                save_scene_file(path);
            } catch (const std::exception& error) {
                report_error(error);
            }
        }
    });
    toolbar->addSeparator();
    undo_ = toolbar->addAction("撤销");
    undo_->setObjectName("undo");
    undo_->setShortcut(QKeySequence::Undo);
    redo_ = toolbar->addAction("重做");
    redo_->setObjectName("redo");
    redo_->setShortcut(QKeySequence::Redo);
    connect(undo_, &QAction::triggered, this, [this] {
        if (editor_.undo()) {
            refresh();
        }
    });
    connect(redo_, &QAction::triggered, this, [this] {
        if (editor_.redo()) {
            refresh();
        }
    });
    auto* import = toolbar->addAction("导入 OBJ");
    connect(import, &QAction::triggered, this, [this] { import_object(); });
    import_texture_action_ = toolbar->addAction("导入纹理");
    connect(import_texture_action_, &QAction::triggered, this, [this] { import_texture(); });
    auto* duplicate = toolbar->addAction("复制物体");
    duplicate->setObjectName("duplicate");
    connect(duplicate, &QAction::triggered, this, [this] { duplicate_object(); });
    auto* remove = toolbar->addAction("删除物体");
    remove->setObjectName("delete");
    connect(remove, &QAction::triggered, this, [this] { delete_object(); });
    auto* screenshot = toolbar->addAction("截图");
    connect(screenshot, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, "保存视口", {}, "PNG (*.png)");
        if (!path.isEmpty() && !viewport_->grab().save(path)) {
            statusBar()->showMessage("截图保存失败", 5000);
        }
    });

    auto* hdr_export = toolbar->addAction("导出 EXR");
    hdr_export->setObjectName("export_hdr");
    connect(hdr_export, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, "导出线性 HDR", {}, "OpenEXR (*.exr)");
        if (!path.isEmpty()) {
            try {
                viewport_->grab_hdr().write_exr(native_path(path));
            } catch (const std::exception& error) {
                report_error(error);
            }
        }
    });

    tree_ = new QTreeWidget;
    tree_->setObjectName("scene_tree");
    tree_->setHeaderLabel("物体");
    assets_ = new QTreeWidget;
    assets_->setHeaderLabels({"资源", "规模"});
    auto* left_tabs = new QTabWidget;
    left_tabs->setDocumentMode(true);
    left_tabs->addTab(tree_, "场景");
    left_tabs->addTab(assets_, "资源");
    auto* scene_dock = new QDockWidget("场景", this);
    scene_dock->setWidget(left_tabs);
    addDockWidget(Qt::LeftDockWidgetArea, scene_dock);
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (!refreshing_) {
            selected_id_ = item ? item->data(0, Qt::UserRole).toString().toStdString() : "";
            refresh_properties();
        }
    });
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item) {
        if (refreshing_) {
            return;
        }
        const auto id = item->data(0, Qt::UserRole).toString().toStdString();
        const bool visible = item->checkState(0) == Qt::Checked;
        perform_edit([&](minirender::SceneDocument& scene) {
            for (auto& object : scene.objects) {
                if (object.id == id) {
                    object.visible = visible;
                }
            }
        });
    });

    build_object_panel();
    build_light_panel();
    build_camera_panel();
    auto* right_tabs = new QTabWidget;
    right_tabs->setObjectName("inspector_tabs");
    right_tabs->setDocumentMode(true);
    right_tabs->addTab(scroll_panel(object_panel_), "物体");
    right_tabs->addTab(scroll_panel(light_panel_), "灯光");
    right_tabs->addTab(scroll_panel(camera_panel_), "相机");
    auto* inspector = new QDockWidget("属性", this);
    inspector->setMinimumWidth(340);
    inspector->setMaximumWidth(620);
    inspector->setWidget(right_tabs);
    addDockWidget(Qt::RightDockWidgetArea, inspector);
    inspector->setObjectName("inspector_dock");
    scene_dock->setObjectName("scene_dock");
    scene_dock->setMinimumWidth(160);
    resizeDocks({scene_dock, inspector}, {200, 430}, Qt::Horizontal);
    viewport_->on_camera_changed = [this](const minirender::Camera& camera) {
        editor_.update_navigation(camera);
        if (camera_panel_->isVisible() && !camera_refresh_pending_) {
            camera_refresh_pending_ = true;
            QTimer::singleShot(33, this, [this] {
                camera_refresh_pending_ = false;
                refresh_camera();
            });
        }
    };
    connect(right_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 2) {
            refresh_camera();
        }
    });
    auto* restore = toolbar->addAction("恢复布局");
    restore->setObjectName("restore_layout");
    connect(restore, &QAction::triggered, this, [this, scene_dock, inspector] {
        scene_dock->setFloating(false);
        inspector->setFloating(false);
        addDockWidget(Qt::LeftDockWidgetArea, scene_dock);
        addDockWidget(Qt::RightDockWidgetArea, inspector);
        scene_dock->show();
        inspector->show();
        resizeDocks({scene_dock, inspector}, {200, 430}, Qt::Horizontal);
    });
    viewport_->on_object_picked = [this](const std::string& id) { select_object(id); };
    auto* status_timer = new QTimer(this);
    status_timer->setInterval(500);
    connect(status_timer, &QTimer::timeout, this, [this] {
        if (texture_loading_) {
            statusBar()->showMessage("正在读取纹理并生成 mipmap…");
            return;
        }
        if (viewport_->isValid()) {
            statusBar()->showMessage(
                QString(
                    "%1 | %2 个物体 | 直接光照预览 | 左拖环绕 · 右拖转向 · 中拖平移 · WASDQE 移动")
                    .arg(QString::fromUtf8(viewport_->physicalDeviceProperties()->deviceName))
                    .arg(editor_.document().objects.size()));
        }
    });
    status_timer->start();
    const QSize available = screen()->availableGeometry().size();
    resize(std::min(1280, available.width() - 40), std::min(800, available.height() - 80));
    undo_->setEnabled(false);
    redo_->setEnabled(false);
    if (diagnostic_) {
        scene_dock->hide();
        inspector->hide();
    }
}

void Workbench::build_object_panel() {
    object_panel_ = new QWidget;
    auto* layout = new QFormLayout(object_panel_);
    configure_form(layout);
    object_name_ = new QLineEdit;
    object_name_->setObjectName("object_name");
    translation_ = new VectorField(object_panel_, "translation");
    rotation_ = new VectorField(object_panel_, "rotation", -360, 360);
    scale_ = new VectorField(object_panel_, "scale", -100, 100);
    base_color_ = new VectorField(object_panel_, "base_color", 0, 1);
    emission_ = new VectorField(object_panel_, "emission", 0, 100);
    material_kind_ = new QComboBox;
    material_kind_->addItems({"漫反射", "金属", "镜面（预览近似）", "玻璃（预览近似）"});
    texture_selector_ = new QComboBox;
    texture_selector_->setObjectName("base_texture");
    texture_filter_ = new QComboBox;
    texture_filter_->setObjectName("texture_filter");
    texture_filter_->addItems({"最近邻", "双线性", "三线性 mipmap"});
    roughness_ = spin(object_panel_, "roughness", 0, 1, 0.05);
    ior_ = spin(object_panel_, "ior", 0.1, 5, 0.05);
    layout->addRow("名称", object_name_);
    layout->addRow("位置 / m", translation_);
    layout->addRow("旋转 / °", rotation_);
    layout->addRow("缩放", scale_);
    layout->addRow("材质", material_kind_);
    layout->addRow("底色 / 线性 RGB", base_color_);
    layout->addRow("底色纹理 / RGB", texture_selector_);
    layout->addRow("纹理过滤", texture_filter_);
    layout->addRow("自发光", emission_);
    layout->addRow("粗糙度", roughness_);
    layout->addRow("折射率", ior_);
    auto* hint = new QLabel("同一材质的参数会同时影响引用它的物体。");
    hint->setWordWrap(true);
    layout->addRow(hint);
    translation_->on_changed = [this] {
        edit_object_property([&](minirender::SceneObject& object) {
            object.transform.translation = translation_->value();
        });
    };
    rotation_->on_changed = [this] {
        edit_object_property([&](minirender::SceneObject& object) {
            object.transform.rotation_degrees = rotation_->value();
        });
    };
    scale_->on_changed = [this] {
        edit_object_property(
            [&](minirender::SceneObject& object) { object.transform.scale = scale_->value(); });
    };
    base_color_->on_changed = [this] {
        edit_material_property(
            [&](minirender::Material& material) { material.base_color = base_color_->value(); });
    };
    emission_->on_changed = [this] {
        edit_material_property(
            [&](minirender::Material& material) { material.emission = emission_->value(); });
    };
    connect(object_name_, &QLineEdit::editingFinished, this, [this] {
        edit_object_property([&](minirender::SceneObject& object) {
            object.name = object_name_->text().toStdString();
        });
    });
    connect(roughness_, &QDoubleSpinBox::editingFinished, this, [this] {
        edit_material_property([&](minirender::Material& material) {
            material.roughness = static_cast<float>(roughness_->value());
        });
    });
    connect(ior_, &QDoubleSpinBox::editingFinished, this, [this] {
        edit_material_property([&](minirender::Material& material) {
            material.ior = static_cast<float>(ior_->value());
        });
    });
    connect(material_kind_, &QComboBox::currentIndexChanged, this, [this] {
        edit_material_property([&](minirender::Material& material) {
            material.kind = static_cast<minirender::MaterialKind>(material_kind_->currentIndex());
        });
    });
    connect(texture_selector_, &QComboBox::currentIndexChanged, this, [this] {
        edit_material_property([&](minirender::Material& material) {
            material.base_color_texture = texture_selector_->currentData().toString().toStdString();
        });
    });
    connect(texture_filter_, &QComboBox::currentIndexChanged, this, [this] {
        edit_material_property([&](minirender::Material& material) {
            material.texture_filter =
                static_cast<minirender::TextureFilter>(texture_filter_->currentIndex());
        });
    });
    object_panel_->setEnabled(false);
}

void Workbench::build_light_panel() {
    light_panel_ = new QWidget;
    auto* layout = new QFormLayout(light_panel_);
    configure_form(layout);
    light_selector_ = new QComboBox;
    light_direction_ = new VectorField(light_panel_, "light_direction", -100, 100);
    light_color_ = new VectorField(light_panel_, "light_color", 0, 100);
    light_intensity_ = spin(light_panel_, "light_intensity", 0, 1000, 0.1);
    layout->addRow("方向光", light_selector_);
    layout->addRow("传播方向", light_direction_);
    layout->addRow("颜色", light_color_);
    layout->addRow("强度", light_intensity_);
    connect(light_selector_, &QComboBox::currentIndexChanged, this, [this] {
        if (!refreshing_) {
            refresh_properties();
        }
    });
    const auto update = [this] {
        if (refreshing_ || light_selector_->currentIndex() < 0) {
            return;
        }
        perform_edit([&](minirender::SceneDocument& scene) {
            auto& light = scene.directional_lights.at(
                static_cast<std::size_t>(light_selector_->currentIndex()));
            light.direction = light_direction_->value();
            light.color = light_color_->value();
            light.intensity = static_cast<float>(light_intensity_->value());
        });
    };
    light_direction_->on_changed = light_color_->on_changed = update;
    connect(light_intensity_, &QDoubleSpinBox::editingFinished, this, update);
}

void Workbench::build_camera_panel() {
    camera_panel_ = new QWidget;
    auto* layout = new QFormLayout(camera_panel_);
    configure_form(layout);
    camera_position_ = new VectorField(camera_panel_, "camera_position");
    camera_target_ = new VectorField(camera_panel_, "camera_target");
    fov_ = spin(camera_panel_, "fov", 5, 150, 1);
    auto* exposure = spin(camera_panel_, "exposure", -10, 10, 0.25);
    auto* mode = new QComboBox;
    mode->setObjectName("view_mode");
    mode->addItems({"直接光照", "法线", "线性深度"});
    layout->addRow("位置", camera_position_);
    layout->addRow("目标", camera_target_);
    layout->addRow("垂直 FOV / °", fov_);
    layout->addRow("曝光 / EV", exposure);
    layout->addRow("显示", mode);
    auto* fxaa = new QCheckBox("启用 FXAA");
    fxaa->setObjectName("fxaa_enabled");
    fxaa->setChecked(true);
    layout->addRow(fxaa);
    connect(fxaa, &QCheckBox::toggled, this,
            [this](bool enabled) { viewport_->set_fxaa_enabled(enabled); });
    auto* shadows = new QCheckBox("启用方向光阴影");
    shadows->setObjectName("shadows_enabled");
    shadows->setChecked(true);
    layout->addRow(shadows);
    connect(shadows, &QCheckBox::toggled, this,
            [this](bool enabled) { viewport_->set_shadows_enabled(enabled); });
    const auto update = [this] {
        if (refreshing_) {
            return;
        }
        perform_edit([&](minirender::SceneDocument& scene) {
            scene.camera.position = camera_position_->value();
            scene.camera.target = camera_target_->value();
            scene.camera.vertical_fov_degrees = static_cast<float>(fov_->value());
        });
    };
    camera_position_->on_changed = camera_target_->on_changed = update;
    connect(fov_, &QDoubleSpinBox::editingFinished, this, update);
    connect(exposure, &QDoubleSpinBox::valueChanged, this,
            [this](double value) { viewport_->set_exposure(static_cast<float>(value)); });
    connect(mode, &QComboBox::currentIndexChanged, this,
            [this](int index) { viewport_->set_view_mode(index); });
}

void Workbench::refresh() {
    refreshing_ = true;
    tree_->clear();
    assets_->clear();
    for (const auto& object : editor_.document().objects) {
        auto* item = new QTreeWidgetItem(tree_, {QString::fromStdString(object.name)});
        item->setData(0, Qt::UserRole, QString::fromStdString(object.id));
        item->setCheckState(0, object.visible ? Qt::Checked : Qt::Unchecked);
        if (object.id == selected_id_) {
            tree_->setCurrentItem(item);
        }
    }
    for (const auto& mesh : editor_.document().meshes) {
        new QTreeWidgetItem(assets_, {QString::fromStdString(mesh.id),
                                      QString::number(mesh.data->triangles.size())});
    }
    for (const auto& texture : editor_.document().textures) {
        new QTreeWidgetItem(
            assets_, {QString::fromStdString(texture.id), QString("%1×%2 / %3 mip")
                                                              .arg(texture.data->level(0).width())
                                                              .arg(texture.data->level(0).height())
                                                              .arg(texture.data->level_count())});
    }
    if (!tree_->currentItem() && tree_->topLevelItemCount() > 0) {
        tree_->setCurrentItem(tree_->topLevelItem(0));
        selected_id_ = tree_->currentItem()->data(0, Qt::UserRole).toString().toStdString();
    }
    const int light_index = light_selector_->currentIndex();
    light_selector_->clear();
    for (const auto& light : editor_.document().directional_lights) {
        light_selector_->addItem(QString::fromStdString(light.id));
    }
    if (light_index >= 0 && light_index < light_selector_->count()) {
        light_selector_->setCurrentIndex(light_index);
    }
    undo_->setEnabled(editor_.can_undo());
    redo_->setEnabled(editor_.can_redo());
    refreshing_ = false;
    refresh_properties();
    refresh_camera();
    viewport_->set_scene(minirender::make_snapshot(editor_.document()));
}

void Workbench::refresh_properties() {
    refreshing_ = true;
    const auto& document = editor_.document();
    const auto object = std::find_if(
        document.objects.begin(), document.objects.end(),
        [&](const minirender::SceneObject& value) { return value.id == selected_id_; });
    texture_selector_->clear();
    texture_selector_->addItem("无", QString());
    for (const auto& texture : document.textures) {
        texture_selector_->addItem(QString::fromStdString(texture.id),
                                   QString::fromStdString(texture.id));
    }
    object_panel_->setEnabled(object != document.objects.end());
    if (object != document.objects.end()) {
        object_name_->setText(QString::fromStdString(object->name));
        translation_->set_value(object->transform.translation);
        rotation_->set_value(object->transform.rotation_degrees);
        scale_->set_value(object->transform.scale);
        for (const auto& material : document.materials) {
            if (material.id == object->material_id) {
                base_color_->set_value(material.base_color);
                emission_->set_value(material.emission);
                material_kind_->setCurrentIndex(static_cast<int>(material.kind));
                roughness_->setValue(material.roughness);
                ior_->setValue(material.ior);
                texture_selector_->setCurrentIndex(texture_selector_->findData(
                    QString::fromStdString(material.base_color_texture)));
                texture_filter_->setCurrentIndex(static_cast<int>(material.texture_filter));
            }
        }
    }
    const int index = light_selector_->currentIndex();
    light_direction_->setEnabled(index >= 0);
    light_color_->setEnabled(index >= 0);
    light_intensity_->setEnabled(index >= 0);
    if (index >= 0) {
        const auto& light = document.directional_lights.at(static_cast<std::size_t>(index));
        light_direction_->set_value(light.direction);
        light_color_->set_value(light.color);
        light_intensity_->setValue(light.intensity);
    }
    refreshing_ = false;
}

void Workbench::refresh_camera() {
    camera_position_->set_value(editor_.document().camera.position);
    camera_target_->set_value(editor_.document().camera.target);
    const QSignalBlocker blocker(fov_);
    fov_->setValue(editor_.document().camera.vertical_fov_degrees);
}

void Workbench::perform_edit(const std::function<void(minirender::SceneDocument&)>& operation) {
    try {
        editor_.edit(operation);
        refresh();
    } catch (const std::exception& error) {
        refresh_properties();
        refresh_camera();
        report_error(error);
    }
}

minirender::SceneObject* Workbench::selected(minirender::SceneDocument& document) const {
    const auto found = std::find_if(
        document.objects.begin(), document.objects.end(),
        [&](const minirender::SceneObject& object) { return object.id == selected_id_; });
    return found == document.objects.end() ? nullptr : &*found;
}

void Workbench::edit_object_property(const std::function<void(minirender::SceneObject&)>& edit) {
    if (refreshing_ || selected_id_.empty()) {
        return;
    }
    perform_edit([&](minirender::SceneDocument& scene) {
        if (auto* object = selected(scene)) {
            edit(*object);
        }
    });
}

void Workbench::edit_material_property(const std::function<void(minirender::Material&)>& edit) {
    if (refreshing_ || selected_id_.empty()) {
        return;
    }
    perform_edit([&](minirender::SceneDocument& scene) {
        if (const auto* object = selected(scene)) {
            for (auto& material : scene.materials) {
                if (material.id == object->material_id) {
                    edit(material);
                }
            }
        }
    });
}

void Workbench::duplicate_object() {
    perform_edit([&](minirender::SceneDocument& scene) {
        if (const auto* original = selected(scene)) {
            auto copy = *original;
            copy.id = unique_id(scene.objects, "object_");
            copy.name += " 副本";
            copy.transform.translation.x += 0.3F;
            selected_id_ = copy.id;
            scene.objects.push_back(copy);
        }
    });
}

void Workbench::delete_object() {
    perform_edit([&](minirender::SceneDocument& scene) {
        std::erase_if(scene.objects, [&](const minirender::SceneObject& object) {
            return object.id == selected_id_;
        });
        selected_id_.clear();
    });
}

void Workbench::import_object() {
    const auto path = QFileDialog::getOpenFileName(this, "导入网格", {}, "Wavefront (*.obj)");
    if (path.isEmpty()) {
        return;
    }
    try {
        auto mesh =
            std::make_shared<const minirender::MeshData>(minirender::load_obj(native_path(path)));
        perform_edit([&](minirender::SceneDocument& scene) {
            if (scene.materials.empty()) {
                scene.materials.push_back({"default", "默认材质"});
            }
            const auto mesh_id = unique_id(scene.meshes, "mesh_");
            const auto object_id = unique_id(scene.objects, "object_");
            scene.meshes.push_back({mesh_id, native_path(path), mesh});
            scene.objects.push_back({object_id,
                                     QFileInfo(path).baseName().toStdString(),
                                     mesh_id,
                                     scene.materials.front().id,
                                     {},
                                     true});
            selected_id_ = object_id;
        });
    } catch (const std::exception& error) {
        report_error(error);
    }
}

void Workbench::load_scene_file(const QString& path) {
    auto document = minirender::load_scene(native_path(path));
    if (document.directional_lights.size() > 8) {
        throw std::runtime_error("实时视口目前最多支持 8 个方向光。");
    }
    ++document_generation_;
    editor_.replace(std::move(document));
    scene_path_ = path;
    selected_id_.clear();
    diagnostic_ = false;
    refresh();
    setWindowTitle("MiniRender — " + QFileInfo(path).fileName());
}

void Workbench::save_scene_file(const QString& path) {
    minirender::save_scene(editor_.document(), native_path(path));
    scene_path_ = path;
    setWindowTitle("MiniRender — " + QFileInfo(path).fileName());
}

void Workbench::select_object(const std::string& id) {
    selected_id_ = id;
    for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
        auto* item = tree_->topLevelItem(index);
        if (item->data(0, Qt::UserRole).toString().toStdString() == id) {
            tree_->setCurrentItem(item);
            return;
        }
    }
    tree_->setCurrentItem(nullptr);
    refresh_properties();
}

void Workbench::report_error(const std::exception& error) {
    QMessageBox::warning(this, "操作失败", QString::fromUtf8(error.what()));
}
VulkanViewport* Workbench::viewport() const noexcept {
    return viewport_;
}
const minirender::SceneDocument& Workbench::document() const noexcept {
    return editor_.document();
}

void Workbench::import_texture() {
    const auto path = QFileDialog::getOpenFileName(this, "导入底色纹理", {},
                                                   "Images (*.png *.jpg *.jpeg *.hdr *.exr)");
    if (path.isEmpty()) {
        return;
    }
    const auto suffix = QFileInfo(path).suffix().toLower();
    const auto encoding = suffix == "hdr" || suffix == "exr" ? minirender::ColorEncoding::linear
                                                             : minirender::ColorEncoding::srgb;
    import_texture_file(path, encoding);
}

void Workbench::import_texture_file(const QString& path, minirender::ColorEncoding encoding) {
    if (texture_loading_) {
        throw std::logic_error("A texture import is already running");
    }
    std::string material_id;
    for (const auto& object : editor_.document().objects) {
        if (object.id == selected_id_) {
            material_id = object.material_id;
        }
    }
    const auto generation = document_generation_;
    const auto source = native_path(path);
    texture_loading_ = true;
    import_texture_action_->setEnabled(false);
    auto* watcher = new QFutureWatcher<TextureLoadResult>(this);
    connect(watcher, &QFutureWatcher<TextureLoadResult>::finished, this,
            [this, watcher, generation, source, encoding, material_id] {
                const auto result = watcher->result();
                watcher->deleteLater();
                texture_loading_ = false;
                import_texture_action_->setEnabled(true);
                if (generation != document_generation_) {
                    return;
                }
                if (!result.error.empty()) {
                    report_error(std::runtime_error(result.error));
                    return;
                }
                perform_edit([&](minirender::SceneDocument& scene) {
                    const auto id = unique_id(scene.textures, "texture_");
                    scene.textures.push_back({id, source, encoding, result.texture});
                    for (auto& material : scene.materials) {
                        if (material.id == material_id) {
                            material.base_color_texture = id;
                        }
                    }
                });
            });
    watcher->setFuture(QtConcurrent::run([source, encoding] {
        TextureLoadResult result;
        try {
            result.texture = std::make_shared<const minirender::Texture>(
                minirender::Image::read(source, encoding));
        } catch (const std::exception& error) {
            result.error = error.what();
        }
        return result;
    }));
}

bool Workbench::texture_loading() const noexcept {
    return texture_loading_;
}
