#pragma once

#include <minirender/assets/lightfield_io.hpp>
#include <minirender/render/render_session.hpp>

namespace minirender {

struct LightFieldCaptureSettings {
    RenderSettings render{128, 128, 32, 101};
    std::uint32_t columns = 5, rows = 5;
    double aperture_radius = .04, reference_focus = 4;
    std::uint64_t memory_budget_bytes = 256ULL * 1024 * 1024;
    std::uint32_t threads = 0, tile_size = 16;
};
enum class LightFieldPhase { preparing, rendering, writing, finalizing };
struct LightFieldCaptureProgress {
    RenderState state = RenderState::idle;
    LightFieldPhase phase = LightFieldPhase::preparing;
    std::uint64_t job_id = 0, recorded_samples = 0, total_samples = 0;
    std::size_t current_view = 0, completed_views = 0, total_views = 0;
    std::uint64_t work_completed = 0, work_total = 0, estimated_bytes = 0, payload_bytes = 0;
    std::uint32_t workers = 0;
    double elapsed_seconds = 0;
    PathStatistics statistics;
    std::string error;
};
struct LightFieldCapturePreview {
    std::uint64_t job_id = 0;
    std::size_t view = 0, completed_views = 0;
    std::shared_ptr<const Image> image;
};

// 场景固定为启动快照，后台共享一个 PT/BVH，逐视图采集和写盘。
// 暂停等待所有 worker 停稳，取消不阻塞调用线程，result 只提供完整文件。
// 生命周期操作由一个控制线程串行调用，progress/preview/result 可并发读取。
class LightFieldCaptureSession final {
  public:
    LightFieldCaptureSession();
    ~LightFieldCaptureSession();
    LightFieldCaptureSession(const LightFieldCaptureSession&) = delete;
    LightFieldCaptureSession& operator=(const LightFieldCaptureSession&) = delete;
    void start(std::shared_ptr<const SceneSnapshot> scene, LightFieldCaptureSettings settings,
               IntegratorSettings integrator = {});
    bool pause();
    bool resume();
    void cancel();
    void wait();
    bool wait_for(std::chrono::milliseconds timeout) const;
    LightFieldCaptureProgress progress() const;
    LightFieldCapturePreview preview() const;
    std::shared_ptr<const LightFieldFile> result() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace minirender
