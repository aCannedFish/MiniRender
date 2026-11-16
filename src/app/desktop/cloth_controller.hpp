// 布料后台固定步模拟、暂停与完整帧发布。

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
    // 当前模拟物体的稳定标识。
    std::string object_id;
    // 来源场景的替换代号，用于拒绝过期帧。
    std::uint64_t document_generation = 0;
    // 来源布料定义与状态的共享只读引用。
    std::shared_ptr<const minirender::ClothComponent> component;
    // 固定的物体变换，用于世界空间模拟与目标校验。
    minirender::Transform transform;
    // 本次任务使用的碰撞与风环境。
    minirender::ClothEnvironment environment;
};

struct ClothRuntimeFrame {
    ClothRuntimeContext                               source;
    minirender::ClothPreparedFrame                    frame;
    std::shared_ptr<const minirender::RasterMeshData> raster;
    minirender::MeshValidationCache                   validated;
    minirender::ClothStepReport                       steps;
    // 是否把该帧作为一次显式可撤销操作提交。
    bool history = false;
};

struct ClothRuntimeStatus {
    // 当前是否绑定可模拟的布料目标。
    bool bound = false;
    // 是否持续请求物理帧。
    bool playing = false;
    // 是否有后台模拟操作尚未完成。
    bool busy = false;
    // 当前模拟是否处于失败状态。
    bool          failed = false;
    std::uint64_t frame  = 0;
    // 根据稳定性设置实际采用的每帧子步数。
    std::uint32_t effective_substeps = 0;
    std::size_t   completed          = 0;
    std::size_t   total              = 0;
    QString       stage;
    QString       message;
    // 最近后台操作耗时，单位为毫秒。
    double                      last_work_ms = 0;
    minirender::ClothStatistics statistics;
    minirender::ClothStepReport last_steps;
};

// 一个后台任务独占模拟器。Qt timer 只请求固定帧，不向物理系统传递墙钟时间。
// 暂停和来源变化取消未发布帧；丢弃后的模拟器从文档最后显示状态重新构造。
class ClothController final : public QObject {
  public:
    explicit ClothController(QObject* parent = nullptr);
    ~ClothController() override;
    // 更新模拟目标和文档来源，来源改变时取消尚未发布的旧帧。
    void bind(std::optional<ClothRuntimeContext> context);
    // 启动定时推进，每次后台操作使用固定物理帧步长。
    void play();
    // 停止继续请求帧并取消尚未发布的计算。
    void pause();
    // 请求单步推进，完成后发布一份完整的模拟姿态。
    void step();
    // 请求恢复当前布料定义的初始状态。
    void reset();
    // 固定新的布料定义和碰撞环境，按 reset_geometry 决定是否恢复静止网格。
    void configure(std::shared_ptr<const minirender::ClothDefinition> definition,
                   minirender::ClothEnvironment environment, bool reset_geometry);
    // 读取 GUI 控制状态及后台阶段进度。
    ClothRuntimeStatus status() const;
    std::function<bool(const ClothRuntimeFrame&)> on_frame;

  private:
    enum class Operation { advance, reset, configure };

    struct Request {
        Operation                                          operation = Operation::advance;
        std::shared_ptr<const minirender::ClothDefinition> definition;
        minirender::ClothEnvironment                       environment;
        bool                                               reset_geometry = false;
    };

    struct Progress {
        mutable std::mutex mutex;
        QString            stage;
        std::size_t        completed = 0;
        std::size_t        total     = 0;
    };

    struct Result {
        std::unique_ptr<minirender::ClothSimulation> simulation;
        ClothRuntimeFrame                            value;
        minirender::ClothEnvironment                 environment;
        QString                                      error;
        bool                                         cancelled = false;
    };

    // 把一个模拟操作交给后台，worker 独占模拟器。
    void launch(Request request);
    // 核对完成帧的来源，在 GUI 线程应用结果或丢弃过期任务。
    void finished();
    // 提交操作；有旧任务时保存最新请求并取消旧工作。
    void request(Request request);
    // 当前 GUI 选择和文档形成的任务来源。
    std::optional<ClothRuntimeContext> context_;
    // 旧工作退出后要执行的最新操作。
    std::optional<Request>                       pending_;
    std::unique_ptr<minirender::ClothSimulation> simulation_;
    minirender::CancellationSource               cancellation_;
    std::shared_ptr<Progress>                    progress_;
    QFutureWatcher<std::shared_ptr<Result>>*     watcher_ = nullptr;
    QTimer*                                      timer_   = nullptr;
    QElapsedTimer                                elapsed_;
    ClothRuntimeStatus                           status_;
    std::uint64_t                                epoch_     = 0;
    std::uint64_t                                job_epoch_ = 0;
};
