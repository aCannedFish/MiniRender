#pragma once

#include <minirender/render/path_tracer.hpp>

#include <functional>
#include <span>
#include <string_view>

namespace minirender {

// 原文 Eq8–11 的逐像素持久统计。visible point 每轮更换，下面四项跨迭代保留。
struct SppmPixel {
    double radius = 0;
    double effective_photons = 0;
    glm::dvec3 tau{0};
    glm::dvec3 direct_sum{0};
};
// Phi 已包含当前相机路径吞吐；没有新光子时半径及 tau 保持不变。
SppmPixel update_sppm_pixel(SppmPixel previous, std::uint64_t photons, glm::dvec3 phi,
                            glm::dvec3 direct, double alpha);

enum class SppmPhase { camera, grid, photons, update };
std::string_view sppm_phase_name(SppmPhase phase);
struct SppmProgress {
    SppmPhase phase = SppmPhase::camera;
    std::uint64_t completed = 0;
    std::uint64_t total = 0;
};
using SppmCallback = std::function<void(SppmProgress)>;
struct SppmStatistics {
    std::uint64_t iterations = 0;
    std::uint64_t emitted_photons = 0;
    std::uint64_t photon_hits = 0;
    std::uint64_t gathered_contributions = 0;
    // 非零合并贡献中，光源侧含 delta、两侧都含 delta 的数量；后者验证 SDS 场景。
    std::uint64_t specular_light_contributions = 0, sds_contributions = 0;
    std::uint64_t camera_paths = 0;
    std::uint64_t camera_scattering_events = 0, photon_scattering_events = 0, shadow_rays = 0;
    TraceStatistics geometry;
    double minimum_radius = 0, maximum_radius = 0, mean_radius = 0;
    std::uint64_t estimated_bytes = 0;
    std::uint32_t workers = 0;
};

// 单个控制线程持有积分器，每轮内部可并行。advance 在完整迭代结束时提交新统计。
// 取消/异常保留上一次完整图像；scene、光源、BVH 构造后只读。
class SppmIntegrator final {
  public:
    SppmIntegrator(std::shared_ptr<const SceneSnapshot> scene, RenderSettings render,
                   IntegratorSettings settings, std::uint32_t threads = 1,
                   CancellationToken cancellation = {});
    ~SppmIntegrator();
    SppmIntegrator(const SppmIntegrator&) = delete;
    SppmIntegrator& operator=(const SppmIntegrator&) = delete;
    void advance(CancellationToken cancellation = {}, SppmCallback progress = {});
    Image image(CancellationToken cancellation = {}) const;
    const SppmStatistics& statistics() const noexcept;
    std::span<const SppmPixel> pixels() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace minirender
