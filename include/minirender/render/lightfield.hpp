#pragma once

#include <minirender/render/path_tracer.hpp>

#include <functional>

namespace minirender {

// 两平面标定：瞳孔偏移以相机 right/up 的米表示，参考焦面沿 forward 测量。
// 图像保持既有左上原点和 FOV，各视图共用坐标系，不旋转相机朝向。
struct LightFieldCalibration {
    Camera camera;
    RenderSettings render{128, 128, 32, 101};
    IntegratorSettings integrator;
    std::uint32_t columns = 5, rows = 5;
    double pupil_radius = .04;
    double reference_focus = 4;
    std::vector<glm::dvec2> pupils;
    void validate() const;
    std::size_t view_count() const noexcept;
    std::size_t center_view() const noexcept;
    std::uint64_t view_bytes() const;
    std::uint64_t total_paths() const;
    double focal_pixels() const;
};

// 奇数格点的 Shirley–Chiu 圆盘映射，中心格点精确落在瞳孔中心。
std::vector<glm::dvec2> lightfield_pupils(std::uint32_t columns, std::uint32_t rows, double radius);
Ray lightfield_ray(const LightFieldCalibration& calibration, std::size_t view, glm::dvec2 pixel_uv);
glm::dvec2 lightfield_shift(const LightFieldCalibration& calibration, std::size_t view,
                            double focus);

struct LightFieldRefocusSettings {
    double focus_distance = 4;
    double aperture_radius = .04;
    std::uint64_t memory_budget_bytes = 256ULL * 1024 * 1024;
};
struct LightFieldWork {
    std::size_t view = 0;
    std::uint64_t completed = 0, total = 0;
};
using LightFieldCallback = std::function<void(LightFieldWork)>;

class LightFieldReader;
struct LightFieldReconstruction {
    explicit LightFieldReconstruction(std::size_t width, std::size_t height)
        : image(width, height), coverage(width * height, 0) {}
    Image image;
    std::vector<float> coverage; // 有效视图数/选定孔径视图数，行主序。
    std::size_t selected_views = 0;
    std::uint64_t estimated_bytes = 0;
};

// 只访问文件读者和标定，不接收场景或积分器；改变焦点不会创建新射线。
LightFieldReconstruction refocus_lightfield(LightFieldReader& reader,
                                            LightFieldRefocusSettings settings,
                                            CancellationToken cancellation = {},
                                            LightFieldCallback progress = {});

} // namespace minirender
