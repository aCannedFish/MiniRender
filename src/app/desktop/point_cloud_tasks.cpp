#include "geometry_controller.hpp"

#include <minirender/assets/point_io.hpp>

#include <chrono>
#include <stdexcept>

namespace {

// 单个 worker 的进度限频，避免点数很大时频繁构造字符串和争用状态锁。
class ProgressRelay final {
  public:
    bool ready() {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_ < std::chrono::milliseconds(25)) {
            return false;
        }
        last_ = now;
        return true;
    }

  private:
    std::chrono::steady_clock::time_point last_{};
};

const char* normal_stage(minirender::PointNormalStage stage) {
    using Stage = minirender::PointNormalStage;
    switch (stage) {
    case Stage::validation:
        return "检查点云";
    case Stage::duplicates:
        return "合并重复点";
    case Stage::index:
        return "建立邻域索引";
    case Stage::estimation:
        return "准备法线与邻域";
    case Stage::orientation:
        return "统一法线方向";
    case Stage::complete:
        return "法线就绪";
    }
    return "准备法线";
}
const char* ball_stage(minirender::BallPivotStage stage) {
    using Stage = minirender::BallPivotStage;
    switch (stage) {
    case Stage::validation:
        return "检查重建输入";
    case Stage::duplicates:
        return "合并重复点";
    case Stage::index:
        return "建立空间索引";
    case Stage::reactivation:
        return "重新激活边界";
    case Stage::seeds:
        return "搜索空球种子";
    case Stage::pivoting:
        return "扩展滚球前沿";
    case Stage::output:
        return "整理并校验面扇";
    case Stage::complete:
        return "重建完成";
    }
    return "重建";
}

using Reporter = std::function<void(GeometryTaskProgress)>;
std::shared_ptr<const PointNormalCache>
prepare_normals(std::shared_ptr<const minirender::PointCloud> cloud,
                minirender::PointNormalSettings settings, minirender::CancellationToken token,
                const Reporter& report, ProgressRelay& relay) {
    auto cache = std::make_shared<PointNormalCache>();
    cache->source = std::move(cloud);
    cache->settings = settings;
    cache->value = minirender::prepare_point_normals(cache->source, settings, token, [&](auto p) {
        if (relay.ready()) {
            report({normal_stage(p.stage), p.completed, p.total});
        }
    });
    return cache;
}

// 点、法线和边界缓冲均在 worker 内准备，GUI 完成回调只发布共享所有权。
void prepare_preview(GeometryTaskResult& result, const minirender::PointCloud& cloud,
                     const minirender::MeshData* mesh, minirender::CancellationToken token,
                     const Reporter& report, ProgressRelay& relay) {
    report({"准备点云与边界预览", 0, 1});
    result.point_preview =
        minirender::prepare_point_preview(cloud, mesh, token, [&](auto done, auto total) {
            if (relay.ready()) {
                report({"准备点云与边界预览", done, total});
            }
        });
}
} // namespace

void GeometryController::import_points(std::filesystem::path source, Completion completion) {
    launch(
        GeometryTaskKind::point_import,
        [source = std::move(source)](auto token, const Reporter& report) {
            GeometryTaskResult result;
            ProgressRelay relay;
            report({"读取 PLY 点云", 0, 1});
            result.point_cloud = std::make_shared<const minirender::PointCloud>(
                minirender::load_point_cloud(source, {}, token, [&](auto done, auto total) {
                    if (relay.ready()) {
                        report({"读取 PLY 记录", done, total});
                    }
                }));
            prepare_preview(result, *result.point_cloud, nullptr, token, report, relay);
            result.saved_path = source;
            result.summary =
                "已读取 " + std::to_string(result.point_cloud->positions.size()) + " 个点";
            return result;
        },
        std::move(completion));
}

void GeometryController::sample_points(std::shared_ptr<const minirender::MeshData> mesh,
                                       std::size_t count, std::uint64_t seed,
                                       Completion completion) {
    launch(
        GeometryTaskKind::point_sample,
        [mesh = std::move(mesh), count, seed](auto token, const Reporter& report) {
            if (!mesh) {
                throw std::invalid_argument("Point sampling requires a source mesh");
            }
            GeometryTaskResult result;
            ProgressRelay relay;
            report({"按表面面积采样", 0, 1});
            result.point_cloud =
                std::make_shared<const minirender::PointCloud>(minirender::sample_mesh_surface(
                    *mesh, count, seed, token, [&](auto done, auto total) {
                        if (relay.ready()) {
                            report({"按表面面积采样", done, total});
                        }
                    }));
            prepare_preview(result, *result.point_cloud, nullptr, token, report, relay);
            result.summary = "已采样 " + std::to_string(count) + " 个点";
            return result;
        },
        std::move(completion));
}

void GeometryController::prepare_points(std::shared_ptr<const minirender::PointCloud> cloud,
                                        minirender::PointNormalSettings settings,
                                        Completion completion) {
    launch(
        GeometryTaskKind::point_normals,
        [cloud = std::move(cloud), settings](auto token, const Reporter& report) {
            GeometryTaskResult result;
            ProgressRelay relay;
            report({"准备点云法线", 0, 1});
            result.point_normals = prepare_normals(cloud, settings, token, report, relay);
            prepare_preview(result, *result.point_normals->value.cloud, nullptr, token, report,
                            relay);
            result.summary = "法线准备完成";
            return result;
        },
        std::move(completion));
}

void GeometryController::reconstruct_points(PointReconstructionInput input, Completion completion) {
    launch(
        GeometryTaskKind::point_reconstruct,
        [input = std::move(input)](auto token, const Reporter& report) {
            GeometryTaskResult result;
            ProgressRelay relay;
            report({"准备点云重建", 0, 1});
            if (input.cached_normals && input.cached_normals->matches(input.cloud, input.normals)) {
                result.point_normals = input.cached_normals;
                report({"复用已准备的法线", 1, 1});
            } else {
                result.point_normals =
                    prepare_normals(input.cloud, input.normals, token, report, relay);
            }
            auto settings = input.balls;
            if (input.automatic_radii) {
                const double spacing = result.point_normals->value.spacing_p90;
                if (!(spacing > 0)) {
                    throw std::runtime_error("未找到可用的点间距，请增大邻域距离或手动设置半径。");
                }
                settings.radii = {2 * spacing, 3 * spacing, 4 * spacing};
            }
            auto reconstruction = minirender::reconstruct_ball_pivot(
                result.point_normals->value.cloud, settings, token, [&](auto p) {
                    if (relay.ready()) {
                        report({std::string(ball_stage(p.stage)) + " · 第 " +
                                    std::to_string(p.radius_index + 1) + "/" +
                                    std::to_string(p.radius_count) + " 轮 · " +
                                    std::to_string(p.triangles) + " 面 · 候选 " +
                                    std::to_string(p.candidate_tests),
                                p.completed, p.total});
                    }
                });
            if (!reconstruction.mesh) {
                throw std::runtime_error("当前半径未找到可重建的三角形，请调整半径。");
            }
            result.point_reconstruction =
                std::make_shared<const minirender::BallPivotResult>(std::move(reconstruction));
            result.mesh = result.point_reconstruction->mesh;
            result.topology = result.point_reconstruction->topology;
            report({"检查发布数据", 0, 1});
            result.validated_meshes.validate(result.mesh, token);
            result.raster =
                minirender::prepare_raster_mesh(*result.mesh, token, [&](auto done, auto total) {
                    if (relay.ready()) {
                        report({"准备实时网格缓冲", done, total});
                    }
                });
            prepare_preview(result, *result.point_normals->value.cloud, result.mesh.get(), token,
                            report, relay);
            result.summary = "已重建 " + std::to_string(result.topology.faces) + " 面，边界环 " +
                             std::to_string(result.topology.boundary_loops);
            return result;
        },
        std::move(completion));
}

void GeometryController::export_points(minirender::SceneDocument sources,
                                       std::shared_ptr<const minirender::PointCloud> cloud,
                                       std::filesystem::path input_file,
                                       std::filesystem::path destination, Completion completion) {
    launch(
        GeometryTaskKind::point_export,
        [sources = std::move(sources), cloud = std::move(cloud), input_file = std::move(input_file),
         destination = std::move(destination)](auto token, const Reporter& report) {
            if (!cloud) {
                throw std::invalid_argument("Point export requires a point cloud");
            }
            report({"检查点云导出路径", 0, 1});
            minirender::ensure_asset_destination(sources, destination, token);
            if (!input_file.empty() &&
                (std::filesystem::weakly_canonical(input_file) ==
                     std::filesystem::weakly_canonical(destination) ||
                 (std::filesystem::exists(input_file) && std::filesystem::exists(destination) &&
                  std::filesystem::equivalent(input_file, destination)))) {
                throw std::invalid_argument("点云导出路径不能覆盖输入文件。");
            }
            ProgressRelay relay;
            minirender::save_point_cloud(*cloud, destination, minirender::PlyFormat::little_endian,
                                         token, [&](auto done, auto total) {
                                             if (relay.ready()) {
                                                 report({"导出 PLY 点云", done, total});
                                             }
                                         });
            GeometryTaskResult result;
            result.file_committed = true;
            result.saved_path = destination;
            result.summary = "点云已导出";
            return result;
        },
        std::move(completion));
}
