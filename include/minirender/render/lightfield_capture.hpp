// 光场逐视图后台采集、内存预算、进度和取消。

#pragma once

#include <minirender/assets/lightfield_io.hpp>
#include <minirender/render/render_session.hpp>

namespace minirender {

// 固定单张渲染预算与瞳孔阵列，整次采集中各子视图共享这些参数。
struct LightFieldCaptureSettings {
    // 单张子视图的图像尺寸、样本数和随机种子。
    RenderSettings render{128, 128, 32, 101};
    // 瞳孔阵列列数，采用包含中心格点的奇数。
    std::uint32_t columns = 5;
    // 瞳孔阵列行数，采用包含中心格点的奇数。
    std::uint32_t rows = 5;
    // 所用瞳孔圆盘半径，单位为米。
    double aperture_radius = .04;
    // 采集相机的参考焦面距离，沿相机前向测量，单位为米。
    double reference_focus = 4;
    // 本任务允许使用的工作缓冲预算，单位为字节。
    std::uint64_t memory_budget_bytes = 256ULL * 1024 * 1024;
    // 采集工作线程数，0 表示自动选择。
    std::uint32_t threads = 0;
    // 子视图内像素工作块的边长。
    std::uint32_t tile_size = 16;
};
enum class LightFieldPhase { preparing, rendering, writing, finalizing };

// 采集任务某一版本的进度快照，区分渲染、写入和整理阶段。
struct LightFieldCaptureProgress {
    // 采集会话的生命周期状态。
    RenderState state = RenderState::idle;
    // 当前准备、渲染、写盘或整理阶段。
    LightFieldPhase phase = LightFieldPhase::preparing;
    // 采集任务代号，用于排除旧任务更新。
    std::uint64_t job_id = 0;
    // 已完成的像素样本总数。
    std::uint64_t recorded_samples = 0;
    // 所有子视图的目标像素样本总数。
    std::uint64_t total_samples = 0;
    // 当前处理的子视图索引。
    std::size_t current_view = 0;
    // 已完成的子视图数量。
    std::size_t completed_views = 0;
    // 瞳孔阵列中的子视图总数。
    std::size_t total_views = 0;
    // 当前阶段已完成的工作项数。
    std::uint64_t work_completed = 0;
    // 当前阶段的工作项总数。
    std::uint64_t work_total = 0;
    // 预计的采集工作缓冲用量。
    std::uint64_t estimated_bytes = 0;
    // 完整光场的 RGB32f 像素数据大小。
    std::uint64_t payload_bytes = 0;
    // 实际参与采集的工作线程数。
    std::uint32_t workers = 0;
    // 从任务启动到当前的经过时间，单位为秒。
    double elapsed_seconds = 0;
    // 已归并的路径追踪计数。
    PathStatistics statistics;
    // 采集失败时的错误说明。
    std::string error;
};

// 标记预览图所属的任务和子视图，防止旧任务刷新当前面板。
struct LightFieldCapturePreview {
    // 采集任务代号，用于排除旧任务更新。
    std::uint64_t job_id = 0;
    // 当前工作或预览对应的子视图索引。
    std::size_t view = 0;
    // 已完成的子视图数量。
    std::size_t completed_views = 0;
    // 共享只读线性图像，由结果对象保有其存储。
    std::shared_ptr<const Image> image;
};

// 场景固定为启动快照，后台共享一个 PT/BVH，逐视图采集和写盘。
// 暂停等待所有 worker 停稳，取消不阻塞调用线程，result 只提供完整文件。
// 生命周期操作由一个控制线程串行调用，progress/preview/result 可并发读取。
class LightFieldCaptureSession final {
  public:
    // 建立空的后台光场采集控制器。
    LightFieldCaptureSession();
    // 请求终止并等待线程退出，释放采集器持有的临时数据。
    ~LightFieldCaptureSession();
    LightFieldCaptureSession(const LightFieldCaptureSession&)            = delete;
    LightFieldCaptureSession& operator=(const LightFieldCaptureSession&) = delete;
    // 固定场景与采集参数，后台逐视图渲染并写出临时容器。
    void start(std::shared_ptr<const SceneSnapshot> scene, LightFieldCaptureSettings settings,
               IntegratorSettings integrator = {});
    // 请求暂停采集，所有 worker 和写盘控制线程停稳后报告 paused。
    bool pause();
    // 继续同一采集任务，保留已经写出的视图与样本序列。
    bool resume();
    // 请求停止当前采集并唤醒等待线程，调用本身不等待任务结束。
    void cancel();
    // 等待当前采集退出并收回线程。
    void wait();
    // 限时等待任务结束，返回时限内是否已经终止。
    bool wait_for(std::chrono::milliseconds timeout) const;
    // 读取线程安全的任务与阶段进度快照。
    LightFieldCaptureProgress progress() const;
    // 读取最近发布的不可变子视图预览。
    LightFieldCapturePreview preview() const;
    // 取得完整光场文件的共享所有权；未完成时不发布部分文件。
    std::shared_ptr<const LightFieldFile> result() const;

  private:
    struct Impl;
    // 独占的线程、同步状态和临时文件所有权。
    std::unique_ptr<Impl> impl_;
};

} // namespace minirender
