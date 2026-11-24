// 将经过校验的目录条目提交到工作台。模型追加保留目标场景的相机、环境和灯光。
#include "builtin_scenes.hpp"
#include "resource_catalog.hpp"
#include "sequence_viewer.hpp"
#include "workbench_internal.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <atomic>
#include <map>
using namespace workbench_detail;

namespace {
// 每次导入拥有独立的进度和取消状态。工作线程只写计数，GUI 定时读取。
struct Progress {
    std::atomic<qint64> checked{0}, total{0};
    std::atomic<bool>   parsing{false}, cancelled{false};

    void check() const {
        if (cancelled.load()) {
            throw std::runtime_error("已取消导入");
        }
    }
};

// 在同一个 SceneEditor 事务中重命名资源和物体，返回首个新物体供检查器选中。
std::string append_model(minirender::SceneDocument&       target,
                         const minirender::SceneDocument& source) {
    std::map<std::string, std::string> meshes, materials, textures;
    for (auto texture : source.textures) {
        texture.source = source.asset_directory / texture.source;
        texture.id = textures[texture.id] = unique_id(target.textures, "texture_");
        target.textures.push_back(std::move(texture));
    }
    for (auto material : source.materials) {
        if (!material.normal_texture.empty()) {
            material.normal_texture = textures.at(material.normal_texture);
        }
        if (!material.base_color_texture.empty()) {
            material.base_color_texture = textures.at(material.base_color_texture);
        }
        material.id = materials[material.id] = unique_id(target.materials, "material_");
        target.materials.push_back(std::move(material));
    }
    for (auto mesh : source.meshes) {
        mesh.source = source.asset_directory / mesh.source;
        mesh.id = meshes[mesh.id] = unique_id(target.meshes, "mesh_");
        target.meshes.push_back(std::move(mesh));
    }
    std::string first;
    for (auto object : source.objects) {
        object.id          = unique_id(target.objects, "object_");
        object.material_id = materials.at(object.material_id);
        object.mesh_id     = meshes.at(object.mesh_id);
        if (first.empty()) {
            first = object.id;
        }
        target.objects.push_back(std::move(object));
    }
    return first;
}
} // namespace

void Workbench::import_resource(const catalog::Entry& entry, bool recommended) {
    if (entry.kind == catalog::Kind::sequence) {
        auto* viewer = new SequenceViewer(entry, this);
        viewer->setAttribute(Qt::WA_DontShowOnScreen, testAttribute(Qt::WA_DontShowOnScreen));
        viewer->show();
        return;
    }
    if (asset_loading()) {
        return;
    }
    begin_asset_task("正在导入 " + entry.name);
    const auto generation = document_generation_;
    auto       progress   = std::make_shared<Progress>();
    auto*      feedback   = new QProgressDialog("正在校验资源…", "取消", 0, 100, this);
    feedback->setObjectName("catalog_progress");
    feedback->setWindowTitle(entry.name);
    feedback->setAutoClose(false);
    feedback->setAutoReset(false);
    feedback->setMinimumDuration(350);
    feedback->setAttribute(Qt::WA_DontShowOnScreen, testAttribute(Qt::WA_DontShowOnScreen));
    feedback->setAttribute(Qt::WA_ShowWithoutActivating);
    feedback->setValue(0);
    connect(feedback, &QProgressDialog::canceled, this, [progress] { progress->cancelled = true; });
    auto* timer = new QTimer(feedback);
    connect(timer, &QTimer::timeout, feedback, [feedback, progress] {
        if (progress->cancelled) {
            return;
        }
        if (progress->parsing) {
            feedback->setRange(0, 0);
            feedback->setLabelText("正在解析模型并准备视口…");
        } else if (auto total = progress->total.load(); total > 0) {
            feedback->setValue(static_cast<int>(100 * progress->checked.load() / total));
        }
    });
    timer->start(100);
    run_background(
        this,
        [entry, progress] {
            qint64 total = QFileInfo(catalog::resolve(entry.root, entry.file.path)).size();
            for (const auto& file : entry.dependencies) {
                total += QFileInfo(catalog::resolve(entry.root, file.path)).size();
            }
            progress->total   = total;
            const auto path   = catalog::verify_scene(entry, [&](qint64 bytes) {
                progress->check();
                progress->checked += bytes;
            });
            progress->parsing = true;
            auto result       = prepare_scene(native_path(path));
            progress->check();
            if (entry.kind == catalog::Kind::model && result.document.objects.empty()) {
                throw std::runtime_error("模型中没有可追加的物体");
            }
            for (auto& mesh : result.document.meshes) {
                mesh.package_on_save = true;
            }
            for (auto& texture : result.document.textures) {
                texture.package_on_save = true;
            }
            return result;
        },
        [this, entry, progress, feedback, generation, recommended](const auto& result) {
            const bool cancelled = progress->cancelled.load() || feedback->wasCanceled();
            // 已结束的进度窗口等待 Qt 延迟销毁，先移除任务标识，确保新任务只对应一个取消控件。
            feedback->setObjectName({});
            feedback->reset();
            feedback->deleteLater();
            finish_asset_task();
            if (cancelled || document_generation_ != generation) {
                return;
            }
            if (!result.error.empty()) {
                auto* error =
                    new QMessageBox(QMessageBox::Warning, "导入失败",
                                    QString::fromStdString(result.error), QMessageBox::Ok, this);
                error->setObjectName("catalog_error");
                error->setAttribute(Qt::WA_DeleteOnClose);
                error->setAttribute(Qt::WA_DontShowOnScreen,
                                    testAttribute(Qt::WA_DontShowOnScreen));
                error->open();
                return;
            }
            try {
                for (const auto& [mesh, raster] : result.value.raster) {
                    viewport_->supply_raster_mesh(mesh, raster);
                }
                const auto& source = result.value.document;
                if (entry.kind == catalog::Kind::model) {
                    perform_edit([&](auto& target) { selected_id_ = append_model(target, source); },
                                 &result.value.validated);
                    return;
                }
                install_scene(source, {}, &result.value.validated);
                if (entry.builtin) {
                    for (const auto& preset : builtin_scenes()) {
                        if (preset.id == entry.id) {
                            apply_builtin_settings(preset);
                            break;
                        }
                    }
                } else {
                    int view = VulkanViewport::base_color_preview_mode;
                    if (recommended && entry.recommended == "blinn-phong") {
                        view = VulkanViewport::blinn_phong_mode;
                    }
                    if (recommended && entry.recommended == "comparison") {
                        view = 19;
                        findChild<QCheckBox*>("area_shadows")->setChecked(true);
                    }
                    if (recommended && entry.recommended == "ssgi") {
                        view = 16;
                    }
                    if (recommended && entry.recommended == "energy") {
                        view = 0;
                    }
                    if (recommended && entry.recommended == "pcss") {
                        view = 0;
                        findChild<QCheckBox*>("shadows_enabled")->setChecked(true);
                        findChild<QComboBox*>("shadow_filter")
                            ->setCurrentIndex(static_cast<int>(ShadowFilter::pcss));
                    }
                    findChild<QComboBox*>("view_mode")->setCurrentIndex(view);
                    if (recommended && entry.recommended == "comparison") {
                        findChild<QPushButton*>("reference_render")->click();
                    }
                    if (recommended && entry.recommended == "prt") {
                        findChild<QPushButton*>("prt_bake")->click();
                    }
                    if (!source.objects.empty()) {
                        select_object(source.objects.front().id);
                    }
                    setWindowTitle("MiniRender — " + entry.name + "（示例副本）");
                }
            } catch (const std::exception& error) {
                report_error(error);
            }
        });
}
