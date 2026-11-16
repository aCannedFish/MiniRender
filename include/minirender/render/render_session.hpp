// CPU 渲染任务的线程调度、暂停取消、进度与不可变预览发布。

#pragma once

#include <minirender/render/path_tracer.hpp>
#include <minirender/render/sppm.hpp>

#include <chrono>

namespace minirender {

enum class RenderState {
    idle,
    preparing,
    running,
    pausing,
    paused,
    finalizing,
    cancelling,
    cancelled,
    completed,
    failed
};
// 返回任务状态的文本名称，用于进度输出。
std::string render_state_name(RenderState state);
// 判断任务是否已停止工作，可以进行结果处理或重新启动。
bool render_terminal(RenderState state) noexcept;

struct SessionSettings {
    std::uint32_t threads = 0; // 0 自动选择并保留一个 CPU 核，最多 32 个工作线程。
    // 方形工作块的边长，单位为像素。
    std::uint32_t tile_size = 16;
    // 每批对工作块执行的像素样本数。
    std::uint32_t batch_samples = 4;
    // 检查线程、tile 和批次预算，非法值抛出参数异常。
    void validate() const;
};

struct RenderProgress {
    // 当前任务状态；暂停请求与已停稳状态分别表示。
    RenderState state = RenderState::idle;
    // 任务代号，用于拒绝旧任务迟到的 UI 更新。
    std::uint64_t job_id = 0;
    // 实际累计并可报告的像素样本数。
    std::uint64_t recorded_samples = 0;
    // 任务预计执行的像素样本总数。
    std::uint64_t total_samples = 0;
    // 达到目标采样数的像素数量。
    std::uint64_t completed_pixels = 0;
    // 本次任务实际使用的工作线程数。
    std::uint32_t worker_count = 0;
    // 任务累计经过时间，单位为秒。
    double elapsed_seconds = 0;
    // 任务失败时记录的错误信息。
    std::string    error;
    PathStatistics statistics; // 完成或取消、收回全部线程后归并。
    // 本次任务使用的积分器类型。
    IntegratorKind integrator = IntegratorKind::path_tracing;
    // SPPM 的光子数、可见点和半径等统计。
    SppmStatistics sppm{};
    // SPPM 当前相机、网格、光子或统计阶段的进度。
    SppmProgress sppm_progress{};
};

// 不可变线性预览及对应的实际累计信息。忙时返回空 image，调用方可保留上一帧。
struct RenderPreview {
    // 该进度版本对应的不可变线性图像。
    std::shared_ptr<const Image> image;
    // 任务代号，用于拒绝旧任务迟到的 UI 更新。
    std::uint64_t job_id = 0;
    // 同一任务中的预览版本，用于跳过重复上传。
    std::uint64_t revision = 0;
    // 实际累计并可报告的像素样本数。
    std::uint64_t recorded_samples = 0;
};

// 异步 CPU 任务。生命周期操作由同一控制线程串行调用；progress/preview 可并发读取。
// 准备、采样和最终图像生成均在后台。暂停等全部存活 worker 停稳后才报告 paused。
// cancel 不 join；wait 和析构会收回线程，UI 只应在销毁或任务终止后调用 wait。
class RenderSession {
  public:
    // 建立任务控制对象；复制被禁用，避免多个控制器管理同一组线程。
    RenderSession();
    // 请求取消并等待后台线程退出，释放内部任务状态。
    ~RenderSession();
    RenderSession(const RenderSession&)            = delete;
    RenderSession& operator=(const RenderSession&) = delete;
    // 固定场景和设置，启动后台准备及采样；结果按新的 job_id 发布。
    void start(std::shared_ptr<const SceneSnapshot> scene, RenderSettings render,
               IntegratorSettings integrator = {}, SessionSettings session = {});
    // 请求暂停；返回是否接受请求，全部工作线程停稳后状态变为 paused。
    bool pause();
    // 恢复同一任务的随机序列和累计数据，返回是否接受请求。
    bool resume();
    // 发布取消请求并唤醒等待线程；调用后通过状态或 wait 等待退出。
    void cancel();
    // 阻塞调用线程，直到任务完成并收回工作线程。
    void wait();
    // 在给定时限内等待任务终止，返回是否已结束。
    bool wait_for(std::chrono::milliseconds timeout) const;
    // 读取线程安全的进度快照，供 UI 定时轮询。
    RenderProgress progress() const;
    // 默认限制预览最长边为 1024，maximum_dimension=0 请求完整分辨率。
    // 使用 try_lock；导出完整中间结果宜在后台调用。
    RenderPreview preview(std::size_t maximum_dimension = 1024) const;
    // 取得完整图像的共享只读所有权；任务未完成时返回空指针。
    std::shared_ptr<const Image> result() const; // 仅 completed 提供完整结果。

  private:
    struct Impl;
    // 独占任务内部状态、同步原语及工作线程。
    std::unique_ptr<Impl> impl_;
};

} // namespace minirender
