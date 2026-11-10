#pragma once

#include <minirender/assets/scene_io.hpp>
#include <minirender/geometry/adaptive.hpp>
#include <minirender/geometry/ball_pivot.hpp>
#include <minirender/geometry/displacement.hpp>
#include <minirender/geometry/loop.hpp>
#include <minirender/geometry/point_normals.hpp>
#include <minirender/geometry/remesh.hpp>
#include <minirender/geometry/simplify.hpp>
#include <minirender/render/point_preview.hpp>
#include <minirender/render/raster_mesh.hpp>

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QObject>

#include <functional>
#include <mutex>
#include <optional>

enum class GeometryTaskState { idle, running, cancelling, ready, cancelled, failed, discarded };
enum class GeometryTaskKind {
    mesh,
    height_import,
    obj_export,
    scene_save,
    point_import,
    point_sample,
    point_normals,
    point_reconstruct,
    point_export
};

// 法线结果固定来源所有权及完整参数，重建可复用同一份已完成的估计结果。
struct PointNormalCache {
    std::shared_ptr<const minirender::PointCloud> source;
    minirender::PointNormalSettings settings;
    minirender::PointNormalResult value;
    bool matches(const std::shared_ptr<const minirender::PointCloud>& input,
                 const minirender::PointNormalSettings& parameters) const noexcept {
        return source == input && settings == parameters;
    }
};

struct PointReconstructionInput {
    std::shared_ptr<const minirender::PointCloud> cloud;
    std::shared_ptr<const PointNormalCache> cached_normals;
    minirender::PointNormalSettings normals;
    minirender::BallPivotSettings balls;
    bool automatic_radii = true;
};

// 每阶段独立计数，total<=1 的校验/解码/提交阶段适合用不确定进度条显示。
struct GeometryTaskProgress {
    std::string stage;
    std::size_t completed = 0;
    std::size_t total = 0;
};

// 启动时固定参数和数据所有权。工具选项互斥，均空时执行均匀 Loop/位移。
struct GeometryTaskInput {
    std::shared_ptr<const minirender::MeshData> mesh;
    std::shared_ptr<const minirender::Texture> height;
    minirender::LoopSettings loop;
    std::optional<minirender::DisplacementSettings> height_settings;
    std::optional<minirender::SimplifySettings> simplify;
    std::optional<minirender::RemeshSettings> remesh;
    std::optional<minirender::AdaptiveSettings> adaptive;
    std::string target_name;
};

struct GeometryTaskResult {
    std::shared_ptr<const minirender::MeshData> mesh;
    std::shared_ptr<const minirender::Texture> height;
    minirender::TopologyStatistics topology;
    std::shared_ptr<const minirender::RasterMeshData> raster;
    minirender::MeshValidationCache validated_meshes;
    std::filesystem::path saved_path;
    double maximum_displacement = 0;
    std::optional<minirender::SimplifyResult> simplification;
    std::optional<minirender::RemeshResult> remeshing;
    std::optional<minirender::AdaptiveResult> adaptation;
    std::shared_ptr<const minirender::PointCloud> point_cloud;
    std::shared_ptr<const minirender::PointPreviewData> point_preview;
    std::shared_ptr<const PointNormalCache> point_normals;
    std::shared_ptr<const minirender::BallPivotResult> point_reconstruction;
    std::string error;
    std::string summary;
    bool cancelled = false;
    bool file_committed = false;
};

struct GeometryTaskStatus {
    GeometryTaskState state = GeometryTaskState::idle;
    GeometryTaskKind kind = GeometryTaskKind::mesh;
    GeometryTaskProgress progress;
    std::string message;
    std::uint64_t jobs_started = 0;
    qint64 elapsed_ms = 0;
};

// GUI 线程拥有单个后台任务；worker 只处理固定 CPU 数据，不访问窗口或 Vulkan。
// 完成回调在 GUI 线程执行，返回 false 表示来源已变化、结果未应用。
// 文件提交成功后迟到的取消不改为 cancelled，避免把已写出的文件报告成未写出。
class GeometryController final : public QObject {
  public:
    using Completion = std::function<bool(const GeometryTaskResult&)>;
    explicit GeometryController(QObject* parent = nullptr);
    ~GeometryController() override;
    void build_mesh(GeometryTaskInput input, Completion completion);
    void import_height(std::filesystem::path source, Completion completion);
    void export_obj(minirender::SceneDocument sources,
                    std::shared_ptr<const minirender::MeshData> mesh,
                    std::filesystem::path destination, Completion completion);
    void save_scene(minirender::SceneDocument scene, std::filesystem::path destination,
                    Completion completion);
    void cancel();
    void import_points(std::filesystem::path source, Completion completion);
    void sample_points(std::shared_ptr<const minirender::MeshData> mesh, std::size_t count,
                       std::uint64_t seed, Completion completion);
    void prepare_points(std::shared_ptr<const minirender::PointCloud> cloud,
                        minirender::PointNormalSettings settings, Completion completion);
    void reconstruct_points(PointReconstructionInput input, Completion completion);
    void export_points(minirender::SceneDocument sources,
                       std::shared_ptr<const minirender::PointCloud> cloud,
                       std::filesystem::path input_file, std::filesystem::path destination,
                       Completion completion);
    bool busy() const noexcept;
    GeometryTaskStatus status() const;

  private:
    using Reporter = std::function<void(GeometryTaskProgress)>;
    using Work = std::function<GeometryTaskResult(minirender::CancellationToken, const Reporter&)>;
    struct Progress {
        std::mutex mutex;
        GeometryTaskProgress value;
    };
    void launch(GeometryTaskKind kind, Work work, Completion completion);
    QFutureWatcher<GeometryTaskResult>* watcher_ = nullptr;
    std::shared_ptr<Progress> progress_;
    minirender::CancellationSource cancellation_;
    GeometryTaskStatus status_;
    QElapsedTimer elapsed_;
};
