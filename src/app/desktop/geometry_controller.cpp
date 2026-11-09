#include "geometry_controller.hpp"

#include <QtConcurrent/QtConcurrentRun>
#include <stdexcept>

namespace {

std::string utf8_path(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

const char* loop_stage(minirender::LoopStage stage) {
    using Stage = minirender::LoopStage;
    switch (stage) {
    case Stage::input_topology:
        return "检查输入拓扑";
    case Stage::vertices:
        return "更新旧顶点";
    case Stage::edges:
        return "生成边顶点";
    case Stage::faces:
        return "生成子三角形";
    case Stage::normals:
        return "计算平滑法线";
    case Stage::output_topology:
        return "检查输出拓扑";
    case Stage::complete:
        return "细分完成";
    }
    return "细分";
}

const char* displacement_stage(minirender::DisplacementStage stage) {
    using Stage = minirender::DisplacementStage;
    switch (stage) {
    case Stage::topology:
        return "检查位移拓扑";
    case Stage::directions:
        return "计算位移方向";
    case Stage::samples:
        return "采样顶点高度";
    case Stage::seams:
        return "检查高度接缝";
    case Stage::positions:
        return "移动顶点";
    case Stage::normals:
        return "计算输出法线";
    case Stage::complete:
        return "位移完成";
    }
    return "位移";
}

const char* save_stage(minirender::SceneSaveStage stage) {
    using Stage = minirender::SceneSaveStage;
    switch (stage) {
    case Stage::validation:
        return "检查场景数据";
    case Stage::mesh:
        return "写出生成网格";
    case Stage::hash:
        return "计算资产内容标识";
    case Stage::compare:
        return "检查已有资产";
    case Stage::copy:
        return "发布资产文件";
    case Stage::metadata:
        return "写出场景参数";
    case Stage::commit:
        return "提交场景文件";
    }
    return "保存";
}

const char* simplify_stage(minirender::SimplifyStage stage) {
    using Stage = minirender::SimplifyStage;
    switch (stage) {
    case Stage::input_topology:
        return "检查输入拓扑";
    case Stage::quadrics:
        return "构建二次误差与保护条件";
    case Stage::candidates:
        return "计算候选边";
    case Stage::collapse:
        return "执行合法边折叠";
    case Stage::output:
        return "生成输出网格与法线";
    case Stage::error:
        return "测量双向表面距离";
    case Stage::complete:
        return "简化完成";
    }
    return "简化";
}

const char* remesh_stage(minirender::RemeshStage stage) {
    using Stage = minirender::RemeshStage;
    switch (stage) {
    case Stage::input_topology:
        return "检查输入拓扑";
    case Stage::reference_surface:
        return "建立原表面查询";
    case Stage::edge_queue:
        return "建立边候选";
    case Stage::split:
        return "分裂长边";
    case Stage::collapse:
        return "折叠短边";
    case Stage::flip:
        return "调整顶点价数";
    case Stage::smooth:
        return "计算切向平滑";
    case Stage::projection:
        return "投影并传递 UV";
    case Stage::output:
        return "准备输出网格";
    case Stage::quality:
        return "测量网格质量";
    case Stage::error:
        return "测量双向表面距离";
    case Stage::complete:
        return "重网格完成";
    }
    return "重网格";
}

const char* adaptive_stage(minirender::AdaptiveStage stage) {
    using Stage = minirender::AdaptiveStage;
    switch (stage) {
    case Stage::topology:
        return "检查控制网格";
    case Stage::seams:
        return "检查高度接缝";
    case Stage::sampling:
        return "采样参照表面";
    case Stage::refinement:
        return "细化误差区域";
    case Stage::balance:
        return "平衡相邻层级";
    case Stage::transitions:
        return "构造过渡三角形";
    case Stage::normals:
        return "计算输出法线";
    case Stage::output:
        return "准备并校验结果";
    case Stage::complete:
        return "自适应细分完成";
    }
    return "自适应细分";
}

} // namespace

GeometryController::GeometryController(QObject* parent) : QObject(parent) {}
GeometryController::~GeometryController() {
    cancellation_.request();
    if (watcher_) {
        disconnect(watcher_, nullptr, this, nullptr);
        watcher_->waitForFinished();
    }
}

bool GeometryController::busy() const noexcept {
    return watcher_ != nullptr;
}
GeometryTaskStatus GeometryController::status() const {
    auto result = status_;
    if (progress_) {
        std::lock_guard lock(progress_->mutex);
        result.progress = progress_->value;
    }
    if (watcher_) {
        result.elapsed_ms = elapsed_.elapsed();
    }
    return result;
}
void GeometryController::cancel() {
    if (watcher_) {
        cancellation_.request();
        status_.state = GeometryTaskState::cancelling;
    }
}

void GeometryController::launch(GeometryTaskKind kind, Work work, Completion completion) {
    if (watcher_) {
        throw std::logic_error("A geometry or asset task is already running");
    }
    const auto jobs = status_.jobs_started;
    status_ = {};
    status_.kind = kind;
    status_.state = GeometryTaskState::running;
    status_.jobs_started = jobs + 1;
    cancellation_ = minirender::CancellationSource();
    progress_ = std::make_shared<Progress>();
    const auto progress = progress_;
    const auto token = cancellation_.token();
    elapsed_.start();
    watcher_ = new QFutureWatcher<GeometryTaskResult>(this);
    auto* watcher = watcher_;
    connect(watcher, &QFutureWatcher<GeometryTaskResult>::finished, this,
            [this, watcher, token, completion = std::move(completion)] {
                auto result = watcher->result();
                watcher_ = nullptr;
                watcher->deleteLater();
                status_.elapsed_ms = elapsed_.elapsed();
                if (token.requested() && !result.file_committed) {
                    result.cancelled = true;
                }
                if (!result.error.empty()) {
                    status_.state = GeometryTaskState::failed;
                    status_.message = result.error;
                } else if (result.cancelled) {
                    status_.state = GeometryTaskState::cancelled;
                    status_.message = "任务已取消";
                } else {
                    status_.state = GeometryTaskState::ready;
                    status_.message = result.summary;
                }
                // 完成回调也负责释放 Workbench 的互斥状态；取消和失败同样通知调用方。
                try {
                    if (completion && !completion(result) &&
                        status_.state == GeometryTaskState::ready) {
                        status_.state = GeometryTaskState::discarded;
                        status_.message = "目标网格或场景已变化，本次结果未应用";
                    }
                } catch (const std::exception& error) {
                    status_.state = GeometryTaskState::failed;
                    status_.message = error.what();
                }
            });
    watcher->setFuture(QtConcurrent::run([work = std::move(work), token, progress] {
        const Reporter report = [progress](GeometryTaskProgress value) {
            std::lock_guard lock(progress->mutex);
            progress->value = std::move(value);
        };
        try {
            token.check();
            return work(token, report);
        } catch (const minirender::OperationCancelled&) {
            GeometryTaskResult result;
            result.cancelled = true;
            return result;
        } catch (const std::exception& error) {
            GeometryTaskResult result;
            result.error = error.what();
            return result;
        }
    }));
}

void GeometryController::build_mesh(GeometryTaskInput input, Completion completion) {
    launch(
        GeometryTaskKind::mesh,
        [input = std::move(input)](auto token, const Reporter& report) {
            GeometryTaskResult result;
            const auto prefix = input.target_name + " | ";
            const int tools = static_cast<int>(input.remesh.has_value()) +
                              static_cast<int>(input.simplify.has_value()) +
                              static_cast<int>(input.adaptive.has_value());
            if (tools > 1) {
                throw std::invalid_argument("Select one geometry operation per task");
            }
            if (input.adaptive) {
                result.adaptation = minirender::subdivide_adaptive(
                    input.mesh, input.height, *input.adaptive, token, [&](auto p) {
                        report({prefix + adaptive_stage(p.stage) + " · red 叶子 " +
                                    std::to_string(p.current_triangles) + " · 求值点 " +
                                    std::to_string(p.evaluation_points),
                                p.completed, p.total});
                    });
                result.mesh = result.adaptation->mesh;
                result.topology = result.adaptation->topology;
                result.maximum_displacement = result.adaptation->maximum_displacement;
            } else if (input.remesh) {
                result.remeshing =
                    minirender::remesh_isotropic(input.mesh, *input.remesh, token, [&](auto p) {
                        report({prefix + "重网格 " + std::to_string(p.iteration) + "/" +
                                    std::to_string(input.remesh->iterations) + " · " +
                                    remesh_stage(p.stage) + " · 当前 " +
                                    std::to_string(p.current_triangles) + " 面",
                                p.completed, p.total});
                    });
                result.mesh = result.remeshing->mesh;
                result.topology = result.remeshing->topology;
            } else if (input.simplify) {
                result.simplification =
                    minirender::simplify_qem(input.mesh, *input.simplify, token, [&](auto p) {
                        report({prefix + simplify_stage(p.stage) + " · 当前 " +
                                    std::to_string(p.current_triangles) + " 面",
                                p.completed, p.total});
                    });
                result.mesh = result.simplification->mesh;
                result.topology = result.simplification->topology;
            } else {
                auto refined =
                    minirender::subdivide_loop(input.mesh, input.loop, token, [&](auto p) {
                        report({prefix + "Loop " + std::to_string(p.level) + "/" +
                                    std::to_string(input.loop.levels) + " · " + loop_stage(p.stage),
                                p.completed, p.total});
                    });
                result.mesh = std::move(refined.mesh);
                result.topology = refined.levels.back();
                if (input.height_settings && input.height_settings->scale != 0) {
                    auto displaced = minirender::displace_mesh(
                        result.mesh, input.height, *input.height_settings, token, [&](auto p) {
                            report({prefix + displacement_stage(p.stage), p.completed, p.total});
                        });
                    result.mesh = std::move(displaced.mesh);
                    result.topology = displaced.topology;
                    result.maximum_displacement = displaced.maximum_displacement;
                }
            }
            report({prefix + "检查发布数据", 0, 1});
            result.validated_meshes.validate(result.mesh, token);
            report({prefix + "检查发布数据", 1, 1});
            result.raster =
                minirender::prepare_raster_mesh(*result.mesh, token, [&](auto done, auto total) {
                    report({prefix + "准备实时网格", done, total});
                });
            result.summary = input.target_name + " | 已生成 " +
                             std::to_string(result.mesh->triangles.size()) + " 个三角形";
            if (result.simplification &&
                result.simplification->stop != minirender::SimplifyStop::target_reached) {
                result.summary +=
                    result.simplification->stop == minirender::SimplifyStop::error_limit
                        ? "，已达 QEM 代价上限"
                        : "，剩余边受拓扑或属性约束";
            }
            return result;
        },
        std::move(completion));
}

void GeometryController::import_height(std::filesystem::path source, Completion completion) {
    launch(
        GeometryTaskKind::height_import,
        [source = std::move(source)](auto token, const Reporter& report) {
            GeometryTaskResult result;
            report({"读取线性高度图", 0, 1});
            auto image = minirender::Image::read(source, minirender::ColorEncoding::linear);
            token.check();
            report({"生成高度 mip", 0, 1});
            result.height = std::make_shared<const minirender::Texture>(std::move(image), token);
            result.saved_path = source;
            result.summary = "高度图已读取：" + utf8_path(source.filename());
            return result;
        },
        std::move(completion));
}

void GeometryController::export_obj(minirender::SceneDocument sources,
                                    std::shared_ptr<const minirender::MeshData> mesh,
                                    std::filesystem::path destination, Completion completion) {
    launch(
        GeometryTaskKind::obj_export,
        [sources = std::move(sources), mesh = std::move(mesh),
         destination = std::move(destination)](auto token, const Reporter& report) {
            if (!mesh) {
                throw std::invalid_argument("No mesh is selected for export");
            }
            GeometryTaskResult result;
            report({"检查导出目标", 0, 1});
            minirender::ensure_asset_destination(sources, destination, token);
            minirender::save_obj(*mesh, destination, token,
                                 [&](auto done, auto total) { report({"写出 OBJ", done, total}); });
            result.file_committed = true;
            result.saved_path = destination;
            result.summary = "OBJ 已保存：" + utf8_path(destination);
            return result;
        },
        std::move(completion));
}

void GeometryController::save_scene(minirender::SceneDocument scene,
                                    std::filesystem::path destination, Completion completion) {
    launch(
        GeometryTaskKind::scene_save,
        [scene = std::move(scene), destination = std::move(destination)](auto token,
                                                                         const Reporter& report) {
            GeometryTaskResult result;
            minirender::save_scene(scene, destination, token, [&](auto p) {
                const auto asset = p.stage == minirender::SceneSaveStage::validation ||
                                           p.stage == minirender::SceneSaveStage::metadata ||
                                           p.stage == minirender::SceneSaveStage::commit
                                       ? std::string{}
                                       : " " + std::to_string(p.asset_index + 1) + "/" +
                                             std::to_string(p.asset_count);
                report({save_stage(p.stage) + asset, p.completed, p.total});
            });
            result.file_committed = true;
            result.saved_path = destination;
            result.summary = "场景快照已保存：" + utf8_path(destination);
            return result;
        },
        std::move(completion));
}
