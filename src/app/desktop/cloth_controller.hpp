#pragma once

#include <minirender/render/raster_mesh.hpp>
#include <minirender/simulation/cloth_component.hpp>

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QObject>
#include <QTimer>

#include <functional>
#include <mutex>

// 主线程固定的任务来源。材质和相机不在物理键中，编辑它们允许当前帧正常提交。
struct ClothRuntimeContext {
    std::string object_id;
    std::uint64_t document_generation = 0;
    std::shared_ptr<const minirender::ClothComponent> component;
    minirender::Transform transform;
    minirender::ClothEnvironment environment;
};

struct ClothRuntimeFrame {
    ClothRuntimeContext source;
    minirender::ClothPreparedFrame frame;
    std::shared_ptr<const minirender::RasterMeshData> raster;
    minirender::MeshValidationCache validated;
    minirender::ClothStepReport steps;
    bool history = false;
};

struct ClothRuntimeStatus {
    bool bound = false;
    bool playing = false;
    bool busy = false;
    bool failed = false;
    std::uint64_t frame = 0;
    std::uint32_t effective_substeps = 0;
    std::size_t completed = 0, total = 0;
    QString stage;
    QString message;
    double last_work_ms = 0;
    minirender::ClothStatistics statistics;
    minirender::ClothStepReport last_steps;
};

// 一个后台任务独占模拟器。Qt timer 只请求固定帧，不向物理系统传递墙钟时间。
// 暂停和来源变化取消未发布帧；丢弃后的模拟器从文档最后显示状态重新构造。
class ClothController final : public QObject {
  public:
    explicit ClothController(QObject* parent = nullptr);
    ~ClothController() override;
    void bind(std::optional<ClothRuntimeContext> context);
    void play();
    void pause();
    void step();
    void reset();
    void configure(std::shared_ptr<const minirender::ClothDefinition> definition,
                   minirender::ClothEnvironment environment, bool reset_geometry);
    ClothRuntimeStatus status() const;
    std::function<bool(const ClothRuntimeFrame&)> on_frame;

  private:
    enum class Operation { advance, reset, configure };
    struct Request {
        Operation operation = Operation::advance;
        std::shared_ptr<const minirender::ClothDefinition> definition;
        minirender::ClothEnvironment environment;
        bool reset_geometry = false;
    };
    struct Progress {
        mutable std::mutex mutex;
        QString stage;
        std::size_t completed = 0, total = 0;
    };
    struct Result {
        std::unique_ptr<minirender::ClothSimulation> simulation;
        ClothRuntimeFrame value;
        minirender::ClothEnvironment environment;
        QString error;
        bool cancelled = false;
    };
    void launch(Request request);
    void finished();
    void request(Request request);
    std::optional<ClothRuntimeContext> context_;
    std::optional<Request> pending_;
    std::unique_ptr<minirender::ClothSimulation> simulation_;
    minirender::CancellationSource cancellation_;
    std::shared_ptr<Progress> progress_;
    QFutureWatcher<std::shared_ptr<Result>>* watcher_ = nullptr;
    QTimer* timer_ = nullptr;
    QElapsedTimer elapsed_;
    ClothRuntimeStatus status_;
    std::uint64_t epoch_ = 0, job_epoch_ = 0;
};
