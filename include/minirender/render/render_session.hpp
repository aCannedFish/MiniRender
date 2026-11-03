#pragma once

#include <minirender/render/path_tracer.hpp>

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
std::string render_state_name(RenderState state);
bool render_terminal(RenderState state) noexcept;

struct SessionSettings {
    std::uint32_t threads = 0; // 0 自动选择并保留一个 CPU 核，最多 32 个工作线程。
    std::uint32_t tile_size = 16;
    std::uint32_t batch_samples = 4;
    void validate() const;
};

struct RenderProgress {
    RenderState state = RenderState::idle;
    std::uint64_t job_id = 0;
    std::uint64_t recorded_samples = 0;
    std::uint64_t total_samples = 0;
    std::uint64_t completed_pixels = 0;
    std::uint32_t worker_count = 0;
    double elapsed_seconds = 0;
    std::string error;
    PathStatistics statistics; // 完成或取消、收回全部线程后归并。
};

// 不可变线性预览及对应的实际累计信息。忙时返回空 image，调用方可保留上一帧。
struct RenderPreview {
    std::shared_ptr<const Image> image;
    std::uint64_t job_id = 0;
    std::uint64_t revision = 0;
    std::uint64_t recorded_samples = 0;
};

// 异步 CPU 任务。生命周期操作由同一控制线程串行调用；progress/preview 可并发读取。
// 准备、采样和最终图像生成均在后台。暂停等全部存活 worker 停稳后才报告 paused。
// cancel 不 join；wait 和析构会收回线程，UI 只应在销毁或任务终止后调用 wait。
class RenderSession {
  public:
    RenderSession();
    ~RenderSession();
    RenderSession(const RenderSession&) = delete;
    RenderSession& operator=(const RenderSession&) = delete;
    void start(std::shared_ptr<const SceneSnapshot> scene, RenderSettings render,
               IntegratorSettings integrator = {}, SessionSettings session = {});
    bool pause();
    bool resume();
    void cancel();
    void wait();
    bool wait_for(std::chrono::milliseconds timeout) const;
    RenderProgress progress() const;
    // 默认限制预览最长边为 1024，maximum_dimension=0 请求完整分辨率。
    // 使用 try_lock；导出完整中间结果宜在后台调用。
    RenderPreview preview(std::size_t maximum_dimension = 1024) const;
    std::shared_ptr<const Image> result() const; // 仅 completed 提供完整结果。

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace minirender
