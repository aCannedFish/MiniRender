// Qt 工作台的视口、面板、编辑事务及后台资产任务协调。
// 本模块接口见 workbench.hpp。

#include "builtin_scenes.hpp"
#include "catalog_browser.hpp"
#include "cloth_controller.hpp"
#include "cloth_panel.hpp"
#include "editor_theme.hpp"
#include "environment_panel.hpp"
#include "geometry_panel.hpp"
#include "lightfield_panel.hpp"
#include "offline_panel.hpp"
#include "point_cloud_panel.hpp"
#include "realtime_panel.hpp"
#include "sdf_panel.hpp"
#include "volume_panel.hpp"
#include "workbench_internal.hpp"
#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QScreen>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVulkanInstance>
#include <algorithm>
#include <array>
#include <minirender/assets/scene_io.hpp>

using namespace workbench_detail;

Workbench::Workbench(QVulkanInstance* instance, bool diagnostic) : diagnostic_(diagnostic) {
    setWindowTitle("MiniRender");
    viewport_ = new VulkanViewport;
    viewport_->setVulkanInstance(instance);
    auto* container = QWidget::createWindowContainer(viewport_, this);
    container->setFocusPolicy(Qt::StrongFocus);
    container->setMinimumSize(280, 240);
    auto* viewport_frame  = new QWidget(this);
    auto* viewport_layout = new QVBoxLayout(viewport_frame);
    viewport_layout->setContentsMargins(0, 0, 0, 0);
    viewport_layout->setSpacing(0);
    viewport_layout->addWidget(container, 1);
    setCentralWidget(viewport_frame);
    // 先建立文件与资产操作入口，回调把耗时工作交给后台控制器。
    auto* toolbar = addToolBar("项目");
    toolbar->setObjectName("project_toolbar");
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(16, 16));
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    auto* brand = new QLabel("MiniRender", toolbar);
    brand->setProperty("role", "brand");
    toolbar->addWidget(brand);
    toolbar->addSeparator();
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
    builtin_button_ = new QToolButton;
    builtin_button_->setText("快速导入");
    builtin_button_->setObjectName("builtin_scenes");
    builtin_button_->setPopupMode(QToolButton::InstantPopup);
    auto* builtin_menu = new QMenu(builtin_button_);
    builtin_menu->setToolTipsVisible(true);
    auto* browse = builtin_menu->addAction("资源目录…");
    browse->setObjectName("browse_catalog");
    connect(browse, &QAction::triggered, this, [this] {
        try {
            auto* browser    = new CatalogBrowser(this);
            browser->on_open = [this](const auto& entry) { import_resource(entry); };
            browser->setAttribute(Qt::WA_DontShowOnScreen, testAttribute(Qt::WA_DontShowOnScreen));
            browser->open();
        } catch (const std::exception& error) {
            report_error(error);
        }
    });
    builtin_menu->addSeparator();
    builtin_menu->addAction("覆盖当前场景，导入后可继续编辑")->setEnabled(false);
    builtin_menu->addSeparator();
    for (const auto& preset : builtin_scenes()) {
        auto* action = builtin_menu->addAction(preset.name);
        action->setObjectName("builtin_" + preset.id);
        action->setToolTip(preset.description + " 覆盖当前场景后可继续编辑，离线渲染手动启动。");
        connect(action, &QAction::triggered, this, [this, id = preset.id] {
            try {
                import_builtin_scene(id);
            } catch (const std::exception& error) {
                report_error(error);
            }
        });
    }
    builtin_button_->setMenu(builtin_menu);
    toolbar->addWidget(builtin_button_);
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
        if (cloth_controller_) {
            cloth_controller_->pause();
        }
        if (editor_.undo()) {
            ++document_revision_;
            refresh();
        }
    });
    connect(redo_, &QAction::triggered, this, [this] {
        if (cloth_controller_) {
            cloth_controller_->pause();
        }
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
                export_display_hdr(path);
            } catch (const std::exception& error) {
                report_error(error);
            }
        }
    });

    // 场景树驱动当前选择与可见状态，编辑器事务统一维护撤销历史。
    tree_ = new QTreeWidget;
    tree_->setObjectName("scene_tree");
    tree_->setHeaderLabel("物体");
    tree_->setHeaderHidden(true);
    tree_->setUniformRowHeights(true);
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
        const auto id      = item->data(0, Qt::UserRole).toString().toStdString();
        const bool visible = item->checkState(0) == Qt::Checked;
        perform_edit([&](minirender::SceneDocument& scene) {
            for (auto& object : scene.objects) {
                if (object.id == id) {
                    object.visible = visible;
                }
            }
        });
    });

    // 注册对象与全局工具页，各工具通过回调访问场景。
    build_object_panel();
    build_light_panel();
    build_camera_panel();
    panels_          = new WorkbenchPanels(*this);
    auto* right_tabs = panels_;
    right_tabs->addTab(scroll_panel(object_panel_), "物体");
    right_tabs->addTab(scroll_panel(light_panel_), "灯光");
    right_tabs->addTab(scroll_panel(camera_panel_), "相机");
    offline_ = new OfflinePanel(
        *viewport_,
        [this] {
            if (lightfield_ && lightfield_->capture_active()) {
                throw std::logic_error("请先结束光场采集，再开始离线渲染。");
            }
            return offline_snapshot();
        },
        this);
    right_tabs->addTab(scroll_panel(offline_), "离线");
    auto* realtime = new RealtimePanel(*viewport_, this);
    right_tabs->addTab(scroll_panel(realtime), "实时");
    ibl_         = new IblController(*viewport_, this);
    environment_ = new EnvironmentPanel(*viewport_, *ibl_, this);
    right_tabs->addTab(scroll_panel(environment_), "环境");
    geometry_tasks_ = new GeometryController(this);
    geometry_       = new GeometryPanel(*geometry_tasks_, this);
    right_tabs->addTab(scroll_panel(geometry_), "几何");
    points_ = new PointCloudPanel(*geometry_tasks_, this);
    right_tabs->addTab(scroll_panel(points_), "点云");
    connect_point_panel();
    sdf_ = new SdfPanel(*viewport_, this);
    right_tabs->addTab(scroll_panel(sdf_), "距离场");
    cloth_controller_ = new ClothController(this);
    cloth_panel_      = new ClothPanel(*cloth_controller_, this);
    right_tabs->addTab(scroll_panel(cloth_panel_), "布料");
    cloth_controller_->on_frame = [this](const auto& frame) { return accept_cloth_frame(frame); };
    cloth_panel_->on_demo       = [this](int kind) {
        try {
            create_cloth_demo(kind);
        } catch (const std::exception& error) {
            report_error(error);
        }
    };
    cloth_panel_->on_target = [this](const std::string& id) {
        cloth_target_ = id;
        sync_cloth_context();
    };
    cloth_panel_->on_apply = [this](auto definition, bool reset) {
        apply_cloth_definition(std::move(definition), reset);
    };
    cloth_panel_->on_freeze  = [this] { freeze_cloth(); };
    cloth_panel_->on_preview = [this] { show_cloth_preview(); };
    volume_                  = new VolumePanel(this);
    right_tabs->addTab(scroll_panel(volume_), "介质");
    volume_->on_changed = [this](std::vector<minirender::MediumRegion> regions) {
        editor_.edit([&](auto& scene) { scene.media = std::move(regions); });
        ++document_revision_;
        refresh();
    };
    volume_->on_demo   = [this](const QString& id) { import_builtin_scene(id); };
    volume_->on_render = [this, right_tabs] {
        right_tabs->setCurrentIndex(3);
        if (minirender::render_terminal(offline_->session().progress().state) &&
            !offline_->exporting()) {
            offline_->start();
        }
    };
    volume_->render_active = [this] {
        return !minirender::render_terminal(offline_->session().progress().state) ||
               offline_->exporting();
    };
    lightfield_                      = new LightFieldController(*viewport_, this);
    lightfield_->other_render_active = [this] {
        return !minirender::render_terminal(offline_->session().progress().state) ||
               offline_->exporting();
    };
    lightfield_panel_             = new LightFieldPanel(*lightfield_, *viewport_, this);
    lightfield_panel_->integrator = [this] {
        auto s = offline_->integrator_settings();
        s.kind = minirender::IntegratorKind::path_tracing;
        return s;
    };
    lightfield_panel_->snapshot = [this] { return offline_snapshot(); };
    lightfield_panel_->on_demo  = [this] { import_builtin_scene("lightfield"); };
    right_tabs->addTab(scroll_panel(lightfield_panel_), "光场");
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
            document.environment     = constant;
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
    // 显示来源变化时同步控件启用状态，避免修改当前不可用的显示参数。
    viewport_->on_display_changed = [this, realtime](bool offline) {
        findChild<QComboBox*>("view_mode")->setEnabled(!offline);
        if (auto* quick = findChild<QComboBox*>("viewport_view_mode")) {
            quick->setEnabled(!offline);
        }
        findChild<QCheckBox*>("fxaa_enabled")->setEnabled(!offline);
        findChild<QCheckBox*>("shadows_enabled")->setEnabled(!offline);
        realtime->setEnabled(!offline);
    };
    scene_dock->setObjectName("scene_dock");
    scene_dock->setMinimumWidth(160);
    editor::decorate_dock(scene_dock);
    // 把页面分配到独立停靠面板，再连接相机与布局恢复操作。
    panels_->build(scene_dock);
    auto* inspector              = panels_->objectDock();
    viewport_->on_camera_changed = [this](const minirender::Camera& camera) {
        editor_.update_navigation(camera);
        ++document_revision_;
        mark_snapshot_changed();
        if (camera_panel_->isVisible() && !camera_refresh_pending_) {
            camera_refresh_pending_ = true;
            QTimer::singleShot(33, this, [this] {
                camera_refresh_pending_ = false;
                refresh_camera();
            });
        }
    };
    panels_->on_current_changed = [this](int index) {
        if (index == 2) {
            refresh_camera();
        }
    };
    auto* restore = toolbar->addAction("恢复布局");
    restore->setObjectName("restore_layout");
    connect(restore, &QAction::triggered, this, [this] { panels_->restore(); });
    const std::array<std::pair<QAction*, editor::Icon>, 12> action_icons{
        {{open_action_, editor::Icon::open},
         {save_action_, editor::Icon::save},
         {save_as_action_, editor::Icon::save_as},
         {undo_, editor::Icon::undo},
         {redo_, editor::Icon::redo},
         {import_object_action_, editor::Icon::import_mesh},
         {import_texture_action_, editor::Icon::texture},
         {duplicate, editor::Icon::duplicate},
         {remove, editor::Icon::remove},
         {screenshot, editor::Icon::camera},
         {hdr_export, editor::Icon::export_image},
         {restore, editor::Icon::layout}}};
    for (const auto& [action, kind] : action_icons) {
        action->setIcon(editor::icon(kind));
        const auto shortcut = action->shortcut().toString(QKeySequence::NativeText);
        action->setToolTip(action->text() + (shortcut.isEmpty() ? "" : " (" + shortcut + ")"));
    }
    auto* render_toggle = panels_->sceneDock()->toggleViewAction();
    render_toggle->setText("渲染设置");
    render_toggle->setToolTip("显示 / 收起场景与渲染面板");
    render_toggle->setObjectName("toggle_render_dock");
    render_toggle->setIcon(editor::icon(editor::Icon::settings));
    auto* render_button = new QToolButton(toolbar);
    render_button->setDefaultAction(render_toggle);
    render_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->addSeparator();
    toolbar->addWidget(render_button);
    connect(render_toggle, &QAction::triggered, this, [this, scene_dock](bool visible) {
        if (!panels_->sceneDock()->isFloating()) {
            resizeDocks({scene_dock}, {visible ? 320 : 190}, Qt::Horizontal);
        }
    });
    builtin_button_->setIcon(editor::icon(editor::Icon::cube));
    builtin_button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->insertSeparator(import_object_action_);
    toolbar->insertSeparator(duplicate);
    toolbar->insertSeparator(screenshot);
    toolbar->insertSeparator(restore);
    viewport_->on_object_picked = [this](const std::string& id) { select_object(id); };
    auto* manipulation          = new QToolBar("物体操纵", viewport_frame);
    viewport_layout->insertWidget(0, manipulation);
    manipulation->setIconSize(QSize(16, 16));
    // 图标工具为视图选择保留空间，窄窗口也能直接切换原色与光照。
    manipulation->setToolButtonStyle(Qt::ToolButtonIconOnly);
    manipulation->setObjectName("gizmo_toolbar");
    manipulation->setMovable(false);
    auto*                             tool_group = new QActionGroup(manipulation);
    const std::array<QString, 3>      tool_names{"选择", "移动", "旋转"};
    const std::array<editor::Icon, 3> tool_icons{editor::Icon::select, editor::Icon::move,
                                                 editor::Icon::rotate};
    const std::array<QString, 3>      shortcuts{"V", "T", "R"};
    const std::array<QString, 3>      tool_ids{"gizmo_select", "gizmo_translate", "gizmo_rotate"};
    const std::array<minirender::GizmoMode, 3> tool_modes{minirender::GizmoMode::select,
                                                          minirender::GizmoMode::translate,
                                                          minirender::GizmoMode::rotate};
    std::array<QAction*, 3>                    tools{};
    for (std::size_t index = 0; index < tools.size(); ++index) {
        tools[index] = manipulation->addAction(tool_names[index]);
        tools[index]->setObjectName(tool_ids[index]);
        tools[index]->setIcon(editor::icon(tool_icons[index]));
        tools[index]->setToolTip(tool_names[index] + " (" + shortcuts[index] + ")");
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
    manipulation->setToolTip("世界坐标轴：拖动轴移动物体，拖动圆环旋转；Esc 取消。");
    auto* tool_space = new QWidget(manipulation);
    tool_space->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    manipulation->addWidget(tool_space);
    auto* view_selector = new QComboBox(manipulation);
    view_selector->setObjectName("viewport_view_mode");
    view_selector->setAccessibleName("视口显示模式");
    view_selector->setToolTip("切换原色、光照及诊断视图");
    view_selector->setMinimumWidth(100);
    view_selector->setMaximumWidth(136);
    auto* camera_mode = findChild<QComboBox*>("view_mode");
    view_selector->setModel(camera_mode->model());
    view_selector->setCurrentIndex(camera_mode->currentIndex());
    connect(view_selector, &QComboBox::currentIndexChanged, camera_mode,
            &QComboBox::setCurrentIndex);
    connect(camera_mode, &QComboBox::currentIndexChanged, view_selector,
            &QComboBox::setCurrentIndex);
    manipulation->addWidget(view_selector);
    viewport_->on_gizmo_mode_changed = [tools, tool_modes](auto mode) {
        for (std::size_t index = 0; index < tools.size(); ++index) {
            tools[index]->setChecked(tool_modes[index] == mode);
        }
    };
    viewport_->on_transform_begin   = [this] { return begin_transform(); };
    viewport_->on_transform_preview = [this](const auto& transform) {
        return preview_transform(transform);
    };
    viewport_->on_transform_end     = [this](bool commit) { finish_transform(commit); };
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
    statusBar()->setSizeGripEnabled(false);
    statusBar()->setToolTip(
        "左拖环绕 · 右拖转向 · 中拖平移 · WASDQE 移动；V 选择 / T 移动 / R 旋转。");
    statusBar()->addPermanentWidget(task_label_);
    statusBar()->addPermanentWidget(task_progress_);
    task_label_->hide();
    task_progress_->hide();
    // 按固定频率汇总后台进度和视口状态，避免每个采样事件更新 GUI。
    auto* status_timer = new QTimer(this);
    status_timer->setInterval(100);
    connect(status_timer, &QTimer::timeout, this, [this] {
        const bool waiting = viewport_->preview_wait_ms() >= 150;
        const bool busy    = asset_loading_ || waiting;
        task_label_->setVisible(busy);
        task_progress_->setVisible(busy);
        const auto geometry_status  = geometry_tasks_->status();
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
                statusBar()->showMessage(viewport_->offline_source() ==
                                                 OfflineImageSource::lightfield
                                             ? "光场图像  ·  曝光只影响显示"
                                             : "CPU 离线结果  ·  曝光只影响显示");
                return;
            }
            if (viewport_->point_preview_visible()) {
                statusBar()->showMessage(
                    QString("点云预览 | %1 个点 | 拖拽查看 · 返回场景后编辑物体")
                        .arg(viewport_->point_preview_data()->points.size()));
                return;
            }
            if (viewport_->sdf_visible()) {
                statusBar()->showMessage(
                    QString("距离场  ·  %1 个节点").arg(viewport_->sdf_document()->nodes.size()));
                return;
            }
            auto message =
                QString("%1  ·  %2 个物体  ·  %3")
                    .arg(QString::fromUtf8(viewport_->physicalDeviceProperties()->deviceName))
                    .arg(editor_.document().objects.size())
                    .arg(viewport_->view_mode() == VulkanViewport::base_color_preview_mode
                             ? "原色预览"
                             : "实时预览");
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
        panels_->sceneDock()->hide();
    } else {
        viewport_->set_scene(editor_.snapshot());
    }
}

void Workbench::refresh() {
    if (!editor_.transforming()) {
        viewport_->finish_gizmo_drag(false);
    }
    mark_snapshot_changed();
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
    sync_cloth_context();
    volume_->set_document(editor_.document().media, document_generation_);
}

OfflinePanel* Workbench::offline_panel() const noexcept {
    return offline_;
}

void Workbench::perform_edit(const std::function<void(minirender::SceneDocument&)>& operation,
                             const minirender::MeshValidationCache*                 validated) {
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
            copy.id   = unique_id(scene.objects, "object_");
            if (copy.cloth) {
                const auto asset =
                    std::find_if(scene.meshes.begin(), scene.meshes.end(),
                                 [&](const auto& value) { return value.id == copy.mesh_id; });
                auto mesh    = *asset;
                mesh.id      = unique_id(scene.meshes, "cloth_mesh_");
                copy.mesh_id = mesh.id;
                scene.meshes.push_back(std::move(mesh));
            }
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
    builtin_button_->setEnabled(false);
    save_action_->setEnabled(false);
    save_as_action_->setEnabled(false);
    import_object_action_->setEnabled(false);
    import_texture_action_->setEnabled(false);
    environment_->set_import_enabled(false);
    geometry_->set_asset_task_busy(true);
    points_->set_asset_task_busy(true);
    volume_->set_asset_busy(true);
    lightfield_panel_->set_asset_busy(true);
}

void Workbench::finish_asset_task() {
    asset_loading_   = false;
    texture_loading_ = false;
    open_action_->setEnabled(true);
    builtin_button_->setEnabled(true);
    save_action_->setEnabled(true);
    save_as_action_->setEnabled(true);
    import_object_action_->setEnabled(true);
    import_texture_action_->setEnabled(true);
    environment_->set_import_enabled(true);
    geometry_->set_asset_task_busy(false);
    points_->set_asset_task_busy(false);
    volume_->set_asset_busy(false);
    lightfield_panel_->set_asset_busy(false);
}

bool Workbench::asset_loading() const noexcept {
    return asset_loading_;
}

GeometryController* Workbench::geometry_controller() const noexcept {
    return geometry_tasks_;
}

GeometryPanel* Workbench::geometry_panel() const noexcept {
    return geometry_;
}

// 每个结束分支都释放资产互斥状态，失败信息由几何面板和短时状态栏消息显示。
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

LightFieldController* Workbench::lightfield_controller() const noexcept {
    return lightfield_;
}

LightFieldPanel* Workbench::lightfield_panel() const noexcept {
    return lightfield_panel_;
}

void Workbench::mark_snapshot_changed() {
    offline_->scene_changed();
    if (lightfield_) {
        lightfield_->scene_changed();
    }
}

void Workbench::export_display_hdr(const QString& path) {
    if (viewport_->offline_visible()) {
        if (viewport_->offline_source() == OfflineImageSource::lightfield) {
            lightfield_->export_image(path, true);
        } else {
            offline_->save_image(path, true);
        }
    } else {
        viewport_->grab_hdr().write_exr(native_path(path));
    }
}

Workbench::~Workbench() {
    // 先断开面板导航回调，随后 QMainWindow 可以销毁停靠布局和子窗口。
    delete panels_;
    panels_ = nullptr;
    if (lightfield_) {
        lightfield_->shutdown();
    }
    if (cloth_controller_) {
        cloth_controller_->on_frame = {};
        cloth_controller_->pause();
    }
    // 原生子窗口晚于派生类成员销毁；先结束手势，断开持有编辑器引用的回调。
    viewport_->finish_gizmo_drag(false);
    viewport_->on_transform_begin       = {};
    viewport_->on_transform_preview     = {};
    viewport_->on_transform_end         = {};
    viewport_->on_gizmo_mode_changed    = {};
    viewport_->on_interaction_error     = {};
    viewport_->on_point_preview_changed = {};
    viewport_->on_sdf_visible_changed   = {};
    viewport_->on_sdf_display_changed   = {};
}

SdfPanel* Workbench::sdf_panel() const noexcept {
    return sdf_;
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
        if (cloth_controller_) {
            cloth_controller_->pause();
        }
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
        mark_snapshot_changed();
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
        mark_snapshot_changed();
    }
    sync_cloth_context();
}

VulkanViewport* Workbench::viewport() const noexcept {
    return viewport_;
}

const minirender::SceneDocument& Workbench::document() const noexcept {
    return editor_.document();
}
