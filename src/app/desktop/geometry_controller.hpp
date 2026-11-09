#pragma once

#include <minirender/assets/scene_io.hpp>
#include <minirender/geometry/displacement.hpp>
#include <minirender/geometry/loop.hpp>
#include <minirender/geometry/simplify.hpp>
#include <minirender/render/raster_mesh.hpp>

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QObject>

#include <functional>
#include <mutex>
#include <optional>

enum class GeometryTaskState { idle, running, cancelling, ready, cancelled, failed, discarded };
enum class GeometryTaskKind { mesh, height_import, obj_export, scene_save };

// 每阶段独立计数，total<=1 的校验/解码/提交阶段适合用不确定进度条显示。
struct GeometryTaskProgress {
    std::string stage;
    std::size_t completed = 0;
    std::size_t total = 0;
};

// 启动时固定参数和数据所有权。simplify 有值时执行独立简化，否则执行 Loop/位移。
struct GeometryTaskInput {
    std::shared_ptr<const minirender::MeshData> mesh;
    std::shared_ptr<const minirender::Texture> height;
    minirender::LoopSettings loop;
    std::optional<minirender::DisplacementSettings> height_settings;
    std::optional<minirender::SimplifySettings> simplify;
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
