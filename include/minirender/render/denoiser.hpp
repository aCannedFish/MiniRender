// 几何引导的单帧滤波与运动重投影。颜色为线性 RGB，位置和法线位于世界空间。
#pragma once
#include <functional>
#include <minirender/scene/scene.hpp>

namespace minirender {
struct GuidePixel {
    glm::vec3 position{}, normal{};
    float     depth  = 0;
    int       object = -1;
};

// world_to_pixel 经过齐次除法后得到左上原点的像素坐标，像素中心为 (x+.5,y+.5)。
struct DenoiseGuides {
    std::size_t             width = 0, height = 0;
    std::vector<GuidePixel> pixels;
    std::vector<glm::mat4>  objects;
    glm::mat4               world_to_pixel{1}, world_to_view{1};
};
enum class SpatialFilter { bilateral, atrous };

struct DenoiseSettings {
    SpatialFilter filter = SpatialFilter::atrous;
    int           radius = 2, iterations = 3;
    float color_sigma = .65F, normal_power = 32, position_sigma = .04F, history_weight = .9F,
          clamp_sigma = 1.F;
};

using DenoiseProgress = std::function<void(std::size_t, std::size_t)>;
// 针孔主射线取得与离线线性图同分辨率的首个交点，普通材质编辑沿用同一场景快照。
DenoiseGuides make_guides(std::shared_ptr<const SceneSnapshot> scene, std::size_t width,
                          std::size_t height, CancellationToken token = {},
                          DenoiseProgress progress = {});
Image filter_spatial(const Image& input, const DenoiseGuides& guides, DenoiseSettings settings = {},
                     CancellationToken token = {}, DenoiseProgress progress = {});

// 调用方在跳帧或切镜后传入空历史。accepted 记录通过可见性测试的像素数。
struct TemporalResult {
    Image       image;
    std::size_t accepted;
};

TemporalResult filter_temporal(const Image& current, const DenoiseGuides& guides,
                               const Image& previous, const DenoiseGuides& previous_guides,
                               DenoiseSettings settings = {}, CancellationToken token = {},
                               DenoiseProgress progress = {});
} // namespace minirender
