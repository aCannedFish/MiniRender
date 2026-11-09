#include "workbench.hpp"
#include "environment_panel.hpp"
#include "geometry_panel.hpp"
#include "offline_panel.hpp"
#include "realtime_panel.hpp"

#include <minirender/assets/scene_io.hpp>

#include <QAction>
#include <QActionGroup>
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
#include <QProgressBar>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVulkanInstance>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>

namespace {

// 工作线程不访问 UI 或 Vulkan。结果和异常均在 watcher 的 UI 线程回调处理。
template <typename T> struct TaskResult {
    T value{};
    std::string error;
};

// 文件解析和完整网格校验一起完成，GUI 事务复用校验缓存。
struct LoadedMesh {
    std::shared_ptr<const minirender::MeshData> data;
    std::shared_ptr<const minirender::RasterMeshData> raster;
    minirender::MeshValidationCache validated;
};
struct LoadedScene {
    minirender::SceneDocument document;
    minirender::MeshValidationCache validated;
    std::vector<std::pair<std::shared_ptr<const minirender::MeshData>,
                          std::shared_ptr<const minirender::RasterMeshData>>>
        raster;
};

template <typename Work, typename Done> void run_background(QObject* owner, Work work, Done done) {
    using Result = TaskResult<decltype(work())>;
    auto* watcher = new QFutureWatcher<Result>(owner);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, owner, [watcher, done] {
        const auto result = watcher->result();
        watcher->deleteLater();
        done(result);
    });
    watcher->setFuture(QtConcurrent::run([work] {
        Result result;
        try {
            result.value = work();
        } catch (const std::exception& error) {
            result.error = error.what();
        }
        return result;
    }));
}

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

// 三分量控件在数值改变时发布事务。键盘输入在 Enter/失焦时提交，箭头立即生效。
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
            connect(values_[index], &QDoubleSpinBox::valueChanged, this, [this] {
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
    open_action_ = toolbar->addAction("打开");
    open_action_->setShortcut(QKeySequence::Open);
    connect(open_action_, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, "打开场景", {}, "Scene (*.json)");
        if (!path.isEmpty()) {
            try {
                open_scene_file(path);
            } catch (const std::exception& error) {
                report_error(error);
            }
        }
    });
    save_action_ = toolbar->addAction("保存");
    save_action_->setObjectName("save_scene");
    save_action_->setShortcut(QKeySequence::Save);
    connect(save_action_, &QAction::triggered, this, [this] {
        auto path = scene_path_;
        if (path.isEmpty()) {
            path = QFileDialog::getSaveFileName(this, "保存场景", {}, "Scene (*.json)");
        }
        if (!path.isEmpty()) {
            try {
                save_scene_async(path);
            } catch (const std::exception& error) {
                report_error(error);
            }
        }
    });
    save_as_action_ = toolbar->addAction("另存");
    save_as_action_->setObjectName("save_scene_as");
    connect(save_as_action_, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, "另存场景", {}, "Scene (*.json)");
        if (!path.isEmpty()) {
            try {
                save_scene_async(path);
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
            ++document_revision_;
            refresh();
        }
    });
    connect(redo_, &QAction::triggered, this, [this] {
        if (editor_.redo()) {
            ++document_revision_;
            refresh();
        }
    });
    import_object_action_ = toolbar->addAction("导入 OBJ");
    connect(import_object_action_, &QAction::triggered, this, [this] { import_object(); });
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
                if (viewport_->offline_visible()) {
                    offline_->save_image(path, true);
                } else {
                    viewport_->grab_hdr().write_exr(native_path(path));
                }
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
            viewport_->finish_gizmo_drag(false);
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
    offline_ = new OfflinePanel(*viewport_, [this] { return editor_.snapshot(); }, this);
    right_tabs->addTab(scroll_panel(offline_), "离线");
    auto* realtime = new RealtimePanel(*viewport_, this);
    right_tabs->addTab(scroll_panel(realtime), "实时");
    ibl_ = new IblController(*viewport_, this);
    environment_ = new EnvironmentPanel(*viewport_, *ibl_, this);
    right_tabs->addTab(scroll_panel(environment_), "环境");
    geometry_tasks_ = new GeometryController(this);
    geometry_ = new GeometryPanel(*geometry_tasks_, this);
    right_tabs->addTab(scroll_panel(geometry_), "几何");
    geometry_->on_apply = [this] {
        try {
            apply_geometry();
        } catch (const std::exception& error) {
            report_error(error);
        }
    };
    geometry_->on_import = [this] {
        const auto path = QFileDialog::getOpenFileName(this, "导入高度图（线性 R 通道）", {},
                                                       "Images (*.exr *.hdr *.png *.jpg *.jpeg)");
        if (!path.isEmpty()) {
            try {
                import_height_file(path);
            } catch (const std::exception& error) {
                report_error(error);
            }
        }
    };
    geometry_->on_export = [this] {
        const auto path =
            QFileDialog::getSaveFileName(this, "导出当前网格", {}, "Wavefront (*.obj)");
        if (!path.isEmpty()) {
            try {
                export_geometry_file(path);
            } catch (const std::exception& error) {
                report_error(error);
            }
        }
    };
    environment_->on_edited = [this](glm::vec3 constant, minirender::EnvironmentMap map) {
        perform_edit([&](auto& document) {
            document.environment = constant;
            document.environment_map = map;
        });
    };
    environment_->on_import = [this] {
        const auto path = QFileDialog::getOpenFileName(this, "导入环境图", {},
                                                       "Images (*.hdr *.exr *.png *.jpg *.jpeg)");
        if (!path.isEmpty()) {
            try {
                import_environment_file(path);
            } catch (const std::exception& error) {
                report_error(error);
            }
        }
    };
    viewport_->on_display_changed = [this, realtime](bool offline) {
        findChild<QComboBox*>("view_mode")->setEnabled(!offline);
        findChild<QCheckBox*>("fxaa_enabled")->setEnabled(!offline);
        findChild<QCheckBox*>("shadows_enabled")->setEnabled(!offline);
        realtime->setEnabled(!offline);
    };
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
        ++document_revision_;
        offline_->scene_changed();
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
    addToolBarBreak();
    auto* manipulation = addToolBar("物体操纵");
    manipulation->setObjectName("gizmo_toolbar");
    manipulation->setMovable(false);
    auto* tool_group = new QActionGroup(manipulation);
    const std::array<QString, 3> tool_names{"选择 V", "移动 T", "旋转 R"};
    const std::array<QString, 3> tool_ids{"gizmo_select", "gizmo_translate", "gizmo_rotate"};
    const std::array<minirender::GizmoMode, 3> tool_modes{minirender::GizmoMode::select,
                                                          minirender::GizmoMode::translate,
                                                          minirender::GizmoMode::rotate};
    std::array<QAction*, 3> tools{};
    for (std::size_t index = 0; index < tools.size(); ++index) {
        tools[index] = manipulation->addAction(tool_names[index]);
        tools[index]->setObjectName(tool_ids[index]);
        tools[index]->setCheckable(true);
        tool_group->addAction(tools[index]);
        connect(tools[index], &QAction::toggled, this,
                [this, mode = tool_modes[index]](bool checked) {
                    if (checked) {
                        viewport_->set_gizmo_mode(mode);
                    }
                });
    }
    tools[0]->setChecked(true);
    auto* tool_hint =
        new QLabel("  世界坐标轴 · 拖动轴移动，拖动圆环旋转 · Esc 取消", manipulation);
    tool_hint->setObjectName("gizmo_hint");
    manipulation->addWidget(tool_hint);
    viewport_->on_gizmo_mode_changed = [tools, tool_modes](auto mode) {
        for (std::size_t index = 0; index < tools.size(); ++index) {
            tools[index]->setChecked(tool_modes[index] == mode);
        }
    };
    viewport_->on_transform_begin = [this] { return begin_transform(); };
    viewport_->on_transform_preview = [this](const auto& transform) {
        return preview_transform(transform);
    };
    viewport_->on_transform_end = [this](bool commit) { finish_transform(commit); };
    viewport_->on_interaction_error = [this](const auto& error) {
        statusBar()->showMessage(QString::fromStdString(error), 5000);
    };
    task_label_ = new QLabel(this);
    task_label_->setMaximumWidth(360);
    task_label_->setTextFormat(Qt::PlainText);
    task_label_->setObjectName("task_label");
    task_progress_ = new QProgressBar(this);
    task_progress_->setObjectName("task_progress");
    task_progress_->setRange(0, 0);
    task_progress_->setFixedWidth(110);
    task_progress_->setTextVisible(false);
    statusBar()->addPermanentWidget(task_label_);
    statusBar()->addPermanentWidget(task_progress_);
    task_label_->hide();
    task_progress_->hide();
    auto* status_timer = new QTimer(this);
    status_timer->setInterval(100);
    connect(status_timer, &QTimer::timeout, this, [this] {
        const bool waiting = viewport_->preview_wait_ms() >= 150;
        const bool busy = asset_loading_ || waiting;
        task_label_->setVisible(busy);
        task_progress_->setVisible(busy);
        const auto geometry_status = geometry_tasks_->status();
        const bool geometry_running = geometry_tasks_->busy();
        const auto task_text = geometry_running && !geometry_status.progress.stage.empty()
                                   ? QString::fromUtf8(geometry_status.progress.stage.c_str())
                               : asset_loading_ ? task_message_
                                                : "正在更新预览…";
        task_label_->setText(task_label_->fontMetrics().elidedText(task_text, Qt::ElideRight, 360));
        task_label_->setToolTip(task_text);
        if (geometry_running && geometry_status.progress.total > 1) {
            task_progress_->setRange(0, 1000);
            task_progress_->setValue(static_cast<int>(1000 * geometry_status.progress.completed /
                                                      geometry_status.progress.total));
        } else {
            task_progress_->setRange(0, 0);
        }
        if (!viewport_->failure().isEmpty()) {
            task_label_->hide();
            task_progress_->hide();
            statusBar()->showMessage(viewport_->failure());
        } else if (asset_loading_) {
            statusBar()->showMessage(task_text);
        } else if (geometry_notice_.isValid() && geometry_notice_.elapsed() < 5000) {
            statusBar()->showMessage(QString::fromUtf8(geometry_status.message.c_str()) +
                                     geometry_notice_extra_);
        } else if (viewport_->isValid()) {
            if (viewport_->offline_visible()) {
                statusBar()->showMessage(
                    "CPU 路径追踪结果 | 曝光只影响显示 | 返回实时预览后可操作相机");
                return;
            }
            auto message =
                QString("%1 | %2 个物体 | 实时预览 | 左拖环绕 · 右拖转向 · 中拖平移 · WASDQE 移动")
                    .arg(QString::fromUtf8(viewport_->physicalDeviceProperties()->deviceName))
                    .arg(editor_.document().objects.size());
            QStringList offline_features;
            if (viewport_->ibl_maps()) {
                message += " | IBL 环境光";
            }
            if (editor_.document().camera.aperture_radius > 0) {
                offline_features.append("景深");
            }
            if (std::any_of(editor_.document().materials.begin(),
                            editor_.document().materials.end(), [](const auto& material) {
                                return glm::any(glm::greaterThan(material.emission, glm::vec3(0)));
                            })) {
                offline_features.append("面光源照明");
            }
            if (!offline_features.isEmpty()) {
                message += " | " + offline_features.join("、") + "用于离线渲染";
            }
            statusBar()->showMessage(message);
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
    } else {
        viewport_->set_scene(editor_.snapshot());
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
        if (!object_name_->isModified()) {
            return;
        }
        object_name_->setModified(false);
        edit_object_property([&](minirender::SceneObject& object) {
            object.name = object_name_->text().toStdString();
        });
    });
    connect(roughness_, &QDoubleSpinBox::valueChanged, this, [this] {
        edit_material_property([&](minirender::Material& material) {
            material.roughness = static_cast<float>(roughness_->value());
        });
    });
    connect(ior_, &QDoubleSpinBox::valueChanged, this, [this] {
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
    connect(light_intensity_, &QDoubleSpinBox::valueChanged, this, update);
}

void Workbench::build_camera_panel() {
    camera_panel_ = new QWidget;
    auto* layout = new QFormLayout(camera_panel_);
    configure_form(layout);
    camera_position_ = new VectorField(camera_panel_, "camera_position");
    camera_target_ = new VectorField(camera_panel_, "camera_target");
    fov_ = spin(camera_panel_, "fov", 5, 150, 1);
    exposure_ = spin(camera_panel_, "exposure", -10, 10, 0.25);
    aperture_ = spin(camera_panel_, "aperture_radius", 0, 10, 0.005);
    focus_ = spin(camera_panel_, "focus_distance", 0.0001, 100000, 0.1);
    auto* mode = new QComboBox;
    mode->setObjectName("view_mode");
    mode->addItems({"光照合成", "法线", "线性深度", "线框", "原始 AO", "滤波 AO"});
    layout->addRow("位置", camera_position_);
    layout->addRow("目标", camera_target_);
    layout->addRow("垂直 FOV / °", fov_);
    layout->addRow("光圈半径 / m", aperture_);
    layout->addRow("对焦距离 / m", focus_);
    layout->addRow("曝光 / EV", exposure_);
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
            scene.camera.aperture_radius = static_cast<float>(aperture_->value());
            scene.camera.focus_distance = static_cast<float>(focus_->value());
        });
    };
    camera_position_->on_changed = camera_target_->on_changed = update;
    connect(fov_, &QDoubleSpinBox::valueChanged, this, update);
    connect(aperture_, &QDoubleSpinBox::valueChanged, this, update);
    connect(focus_, &QDoubleSpinBox::valueChanged, this, update);
    connect(exposure_, &QDoubleSpinBox::valueChanged, this,
            [this](double value) { viewport_->set_exposure(static_cast<float>(value)); });
    connect(mode, &QComboBox::currentIndexChanged, this,
            [this](int index) { viewport_->set_view_mode(index); });
}

void Workbench::refresh() {
    if (!editor_.transforming()) {
        viewport_->finish_gizmo_drag(false);
    }
    offline_->scene_changed();
    environment_->set_document(editor_.document());
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
    viewport_->set_scene(editor_.snapshot());
}

void Workbench::refresh_properties() {
    refreshing_ = true;
    const auto& document = editor_.document();
    geometry_->set_context(document, selected_id_);
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
    viewport_->set_gizmo_target(
        object != document.objects.end() && object->visible ? object->id : "",
        object != document.objects.end() && object->visible ? std::optional(object->transform)
                                                            : std::nullopt);
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
    const QSignalBlocker aperture_blocker(aperture_);
    const QSignalBlocker focus_blocker(focus_);
    const QSignalBlocker exposure_blocker(exposure_);
    aperture_->setValue(editor_.document().camera.aperture_radius);
    focus_->setValue(editor_.document().camera.focus_distance);
    exposure_->setValue(viewport_->exposure());
}

OfflinePanel* Workbench::offline_panel() const noexcept {
    return offline_;
}

void Workbench::perform_edit(const std::function<void(minirender::SceneDocument&)>& operation,
                             const minirender::MeshValidationCache* validated) {
    try {
        if (validated) {
            editor_.edit_prevalidated(operation, *validated);
        } else {
            editor_.edit(operation);
        }
        ++document_revision_;
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
    import_object_file(path);
}

void Workbench::begin_asset_task(const QString& message) {
    if (asset_loading_) {
        throw std::logic_error("An asset task is already running");
    }
    asset_loading_ = true;
    geometry_notice_.invalidate();
    geometry_notice_extra_.clear();
    task_message_ = message;
    task_label_->setText(message);
    task_label_->show();
    task_progress_->show();
    task_progress_->setRange(0, 0);
    open_action_->setEnabled(false);
    save_action_->setEnabled(false);
    save_as_action_->setEnabled(false);
    import_object_action_->setEnabled(false);
    import_texture_action_->setEnabled(false);
    environment_->set_import_enabled(false);
    geometry_->set_asset_task_busy(true);
}

void Workbench::finish_asset_task() {
    asset_loading_ = false;
    texture_loading_ = false;
    open_action_->setEnabled(true);
    save_action_->setEnabled(true);
    save_as_action_->setEnabled(true);
    import_object_action_->setEnabled(true);
    import_texture_action_->setEnabled(true);
    environment_->set_import_enabled(true);
    geometry_->set_asset_task_busy(false);
}

bool Workbench::asset_loading() const noexcept {
    return asset_loading_;
}

void Workbench::import_object_file(const QString& path) {
    begin_asset_task("正在读取并校验网格…");
    const auto source = native_path(path);
    const auto generation = document_generation_;
    run_background(
        this,
        [source] {
            LoadedMesh result;
            result.data =
                std::make_shared<const minirender::MeshData>(minirender::load_obj(source));
            result.validated.validate(result.data);
            result.raster = minirender::prepare_raster_mesh(*result.data);
            return result;
        },
        [this, source, path, generation](const auto& result) {
            finish_asset_task();
            if (generation != document_generation_) {
                return;
            }
            if (!result.error.empty()) {
                report_error(std::runtime_error(result.error));
                return;
            }
            viewport_->supply_raster_mesh(result.value.data, result.value.raster);
            perform_edit(
                [&](minirender::SceneDocument& scene) {
                    if (scene.materials.empty()) {
                        scene.materials.push_back({"default", "默认材质"});
                    }
                    const auto mesh_id = unique_id(scene.meshes, "mesh_");
                    const auto object_id = unique_id(scene.objects, "object_");
                    scene.meshes.push_back({mesh_id, source, result.value.data});
                    scene.objects.push_back({object_id,
                                             QFileInfo(path).baseName().toStdString(),
                                             mesh_id,
                                             scene.materials.front().id,
                                             {},
                                             true});
                    selected_id_ = object_id;
                },
                &result.value.validated);
        });
}

void Workbench::open_scene_file(const QString& path) {
    begin_asset_task("正在读取场景与资产…");
    const auto source = native_path(path);
    const auto generation = ++document_generation_;
    run_background(
        this,
        [source] {
            LoadedScene result;
            result.document = minirender::load_scene(source);
            result.document.validate(&result.validated);
            for (const auto& mesh : result.document.meshes) {
                const bool visible = std::any_of(
                    result.document.objects.begin(), result.document.objects.end(),
                    [&](const auto& item) { return item.visible && item.mesh_id == mesh.id; });
                if (visible) {
                    result.raster.emplace_back(mesh.data,
                                               minirender::prepare_raster_mesh(*mesh.data));
                }
            }
            return result;
        },
        [this, path, generation](const auto& result) {
            finish_asset_task();
            if (generation != document_generation_) {
                return;
            }
            if (!result.error.empty()) {
                report_error(std::runtime_error(result.error));
                return;
            }
            try {
                for (const auto& [mesh, raster] : result.value.raster) {
                    viewport_->supply_raster_mesh(mesh, raster);
                }
                install_scene(result.value.document, path, &result.value.validated);
            } catch (const std::exception& error) {
                report_error(error);
            }
        });
}

void Workbench::load_scene_file(const QString& path) {
    install_scene(minirender::load_scene(native_path(path)), path);
}

void Workbench::install_scene(minirender::SceneDocument document, const QString& path,
                              const minirender::MeshValidationCache* validated) {
    if (document.directional_lights.size() > 8) {
        throw std::runtime_error("实时视口目前最多支持 8 个方向光。");
    }
    ++document_generation_;
    ++document_revision_;
    geometry_tasks_->cancel();
    if (validated) {
        editor_.replace_prevalidated(std::move(document), *validated);
    } else {
        editor_.replace(std::move(document));
    }
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

GeometryController* Workbench::geometry_controller() const noexcept {
    return geometry_tasks_;
}
GeometryPanel* Workbench::geometry_panel() const noexcept {
    return geometry_;
}

// 每个结束分支都释放资产互斥状态，失败信息由几何面板和短时状态栏消息显示。
bool Workbench::finish_geometry_task(const GeometryTaskResult& result) {
    finish_asset_task();
    geometry_notice_.restart();
    return result.error.empty() && !result.cancelled;
}

void Workbench::apply_geometry() {
    const auto& document = editor_.document();
    const auto object = std::find_if(document.objects.begin(), document.objects.end(),
                                     [&](const auto& item) { return item.id == selected_id_; });
    if (object == document.objects.end()) {
        throw std::invalid_argument("Select an object before generating geometry");
    }
    const auto mesh = std::find_if(document.meshes.begin(), document.meshes.end(),
                                   [&](const auto& item) { return item.id == object->mesh_id; });
    if (mesh == document.meshes.end()) {
        throw std::invalid_argument("Selected mesh is missing");
    }
    const auto params = geometry_->parameters();
    GeometryTaskInput input;
    input.mesh = mesh->data;
    input.loop = params.loop;
    input.height_settings = params.displacement;
    input.simplify = params.simplify;
    input.target_name = object->name;
    if (params.displacement && params.displacement->scale != 0) {
        const auto height = std::find_if(
            document.textures.begin(), document.textures.end(), [&](const auto& texture) {
                return texture.id == params.height_id &&
                       texture.encoding == minirender::ColorEncoding::linear;
            });
        if (height == document.textures.end()) {
            throw std::invalid_argument("Select a linear height texture before displacement");
        }
        input.height = height->data;
    }
    const auto generation = document_generation_;
    const auto target = object->id;
    const auto expected_mesh_id = mesh->id;
    const auto source = mesh->data;
    begin_asset_task("正在生成网格…");
    try {
        geometry_tasks_->build_mesh(std::move(input), [this, generation, target, expected_mesh_id,
                                                       source](const auto& result) {
            if (!finish_geometry_task(result)) {
                return true;
            }
            const auto& current = editor_.document();
            const auto current_object =
                std::find_if(current.objects.begin(), current.objects.end(),
                             [&](const auto& item) { return item.id == target; });
            const auto current_mesh =
                std::find_if(current.meshes.begin(), current.meshes.end(),
                             [&](const auto& item) { return item.id == expected_mesh_id; });
            if (generation != document_generation_ || current_object == current.objects.end() ||
                current_object->mesh_id != expected_mesh_id ||
                current_mesh == current.meshes.end() || current_mesh->data != source) {
                return false;
            }
            if (result.mesh != source) {
                editor_.edit_prevalidated(
                    [&](auto& scene) {
                        const auto id = unique_id(scene.meshes, "geometry_");
                        scene.meshes.push_back({id, {}, result.mesh});
                        for (auto& item : scene.objects) {
                            if (item.id == target) {
                                item.mesh_id = id;
                            }
                        }
                    },
                    result.validated_meshes);
                ++document_revision_;
                viewport_->supply_raster_mesh(result.mesh, result.raster);
                refresh();
            }
            if (selected_id_ == target) {
                geometry_->show_result(result);
            }
            return true;
        });
    } catch (...) {
        finish_asset_task();
        throw;
    }
}

void Workbench::import_height_file(const QString& path) {
    const auto generation = document_generation_;
    begin_asset_task("正在读取线性高度图…");
    try {
        geometry_tasks_->import_height(native_path(path), [this, generation](const auto& result) {
            if (!finish_geometry_task(result)) {
                return true;
            }
            if (generation != document_generation_) {
                return false;
            }
            std::string id;
            editor_.edit([&](auto& scene) {
                id = unique_id(scene.textures, "height_");
                scene.textures.push_back(
                    {id, result.saved_path, minirender::ColorEncoding::linear, result.height});
            });
            ++document_revision_;
            refresh();
            geometry_->select_height(id);
            return true;
        });
    } catch (...) {
        finish_asset_task();
        throw;
    }
}

void Workbench::export_geometry_file(const QString& path) {
    const auto& document = editor_.document();
    const auto object = std::find_if(document.objects.begin(), document.objects.end(),
                                     [&](const auto& item) { return item.id == selected_id_; });
    if (object == document.objects.end()) {
        throw std::invalid_argument("Select an object before exporting geometry");
    }
    const auto mesh = std::find_if(document.meshes.begin(), document.meshes.end(),
                                   [&](const auto& item) { return item.id == object->mesh_id; });
    if (mesh == document.meshes.end()) {
        throw std::invalid_argument("Selected mesh is missing");
    }
    begin_asset_task("正在导出 OBJ…");
    try {
        geometry_tasks_->export_obj(document, mesh->data, native_path(path),
                                    [this](const auto& result) {
                                        finish_geometry_task(result);
                                        return true;
                                    });
    } catch (...) {
        finish_asset_task();
        throw;
    }
}

void Workbench::save_scene_async(const QString& path) {
    const auto generation = document_generation_;
    const auto revision = document_revision_;
    begin_asset_task("正在保存场景快照…");
    try {
        geometry_tasks_->save_scene(editor_.document(), native_path(path),
                                    [this, generation, revision, path](const auto& result) {
                                        if (!finish_geometry_task(result)) {
                                            return true;
                                        }
                                        if (generation == document_generation_) {
                                            scene_path_ = path;
                                            const bool changed = revision != document_revision_;
                                            setWindowTitle("MiniRender — " +
                                                           QFileInfo(path).fileName() +
                                                           (changed ? " *" : ""));
                                            if (changed) {
                                                geometry_notice_extra_ = " | 当前还有未保存修改";
                                            }
                                        }
                                        return true;
                                    });
    } catch (...) {
        finish_asset_task();
        throw;
    }
}

void Workbench::select_object(const std::string& id) {
    viewport_->finish_gizmo_drag(false);
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

Workbench::~Workbench() {
    // 原生子窗口晚于派生类成员销毁；先结束手势，断开持有编辑器引用的回调。
    viewport_->finish_gizmo_drag(false);
    viewport_->on_transform_begin = {};
    viewport_->on_transform_preview = {};
    viewport_->on_transform_end = {};
    viewport_->on_gizmo_mode_changed = {};
    viewport_->on_interaction_error = {};
}

bool Workbench::begin_transform() {
    try {
        const auto& objects = editor_.document().objects;
        const auto object = std::find_if(objects.begin(), objects.end(),
                                         [&](const auto& item) { return item.id == selected_id_; });
        if (object == objects.end() || !object->visible) {
            return false;
        }
        editor_.begin_transform_edit(selected_id_);
        transform_preview_ = object->transform;
        return true;
    } catch (const std::exception& error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()), 5000);
        return false;
    }
}

bool Workbench::preview_transform(const minirender::Transform& transform) {
    if (!editor_.transforming() || !transform_preview_) {
        return false;
    }
    const auto& previous = *transform_preview_;
    if (previous.translation == transform.translation &&
        previous.rotation_degrees == transform.rotation_degrees &&
        previous.scale == transform.scale) {
        return true;
    }
    try {
        editor_.preview_transform(transform);
        transform_preview_ = transform;
        ++document_revision_;
        // 每个鼠标事件只更新变换控件与场景参数，不重建树或扫描网格。
        translation_->set_value(transform.translation);
        rotation_->set_value(transform.rotation_degrees);
        scale_->set_value(transform.scale);
        undo_->setEnabled(editor_.can_undo());
        redo_->setEnabled(editor_.can_redo());
        viewport_->set_gizmo_target(selected_id_, transform);
        viewport_->set_scene(editor_.snapshot());
        offline_->scene_changed();
        return true;
    } catch (const std::exception& error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()), 5000);
        return false;
    }
}

void Workbench::finish_transform(bool commit) {
    transform_preview_.reset();
    if (!editor_.transforming()) {
        return;
    }
    const bool changed = commit ? editor_.commit_transform_edit() : editor_.cancel_transform_edit();
    if (changed && !commit) {
        ++document_revision_;
    }
    undo_->setEnabled(editor_.can_undo());
    redo_->setEnabled(editor_.can_redo());
    if (changed && !commit) {
        refresh_properties();
        viewport_->set_scene(editor_.snapshot());
        offline_->scene_changed();
    }
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
    begin_asset_task("正在读取纹理并生成 mipmap…");
    std::string material_id;
    for (const auto& object : editor_.document().objects) {
        if (object.id == selected_id_) {
            material_id = object.material_id;
        }
    }
    const auto generation = document_generation_;
    const auto source = native_path(path);
    texture_loading_ = true;
    run_background(
        this,
        [source, encoding] {
            return std::make_shared<const minirender::Texture>(
                minirender::Image::read(source, encoding));
        },
        [this, generation, source, encoding, material_id](const auto& result) {
            finish_asset_task();
            if (generation != document_generation_) {
                return;
            }
            if (!result.error.empty()) {
                report_error(std::runtime_error(result.error));
                return;
            }
            perform_edit([&](minirender::SceneDocument& scene) {
                const auto id = unique_id(scene.textures, "texture_");
                scene.textures.push_back({id, source, encoding, result.value});
                for (auto& material : scene.materials) {
                    if (material.id == material_id) {
                        material.base_color_texture = id;
                    }
                }
            });
        });
}

bool Workbench::texture_loading() const noexcept {
    return texture_loading_;
}

IblController* Workbench::ibl_controller() const noexcept {
    return ibl_;
}
void Workbench::import_environment_file(const QString& path) {
    begin_asset_task("正在读取环境图…");
    const auto source = native_path(path);
    const auto suffix = QFileInfo(path).suffix().toLower();
    const auto encoding = suffix == "hdr" || suffix == "exr" ? minirender::ColorEncoding::linear
                                                             : minirender::ColorEncoding::srgb;
    const auto generation = document_generation_;
    run_background(
        this,
        [source, encoding] {
            return std::make_shared<const minirender::Texture>(
                minirender::Image::read(source, encoding));
        },
        [this, source, encoding, generation](const auto& result) {
            finish_asset_task();
            if (generation != document_generation_) {
                return;
            }
            if (!result.error.empty()) {
                report_error(std::runtime_error(result.error));
                return;
            }
            perform_edit([&](auto& scene) {
                const auto id = unique_id(scene.textures, "environment_");
                scene.textures.push_back({id, source, encoding, result.value});
                scene.environment_map.texture_id = id;
            });
        });
}
