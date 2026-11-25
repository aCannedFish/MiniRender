// 几何引导的单帧滤波与运动重投影。颜色为线性 RGB，位置和法线位于世界空间。
#pragma once
#include <functional>
#include <minirender/scene/scene.hpp>

namespace minirender {
// 首交点引导样本。object 对应当前帧 objects 数组，背景像素的编号为 -1。
struct GuidePixel {
    glm::vec3 position{}; // 世界空间交点。
    glm::vec3 normal{};   // 单位着色法线。
    float     depth  = 0; // 交点的正视空间深度，单位为米。
    int       object = -1;
};

// 逐像素记录可见表面，并保存同帧的物体与相机变换供重投影查询。
struct DenoiseGuides {
    std::size_t             width = 0, height = 0;
    std::vector<GuidePixel> pixels;            // 行主序，坐标原点在左上角。
    std::vector<glm::mat4>  objects;           // 按物体编号索引的局部到世界变换。
    glm::mat4               world_to_pixel{1}; // 齐次除法后的像素中心为 (x+.5, y+.5)。
    glm::mat4               world_to_view{1};  // 用于比较预测的前帧深度与实际引导深度。
};

enum class SpatialFilter { bilateral, atrous };

// 空间滤波使用前景几何和颜色权重，时间累积使用同对象邻域的限幅与历史权重。
struct DenoiseSettings {
    SpatialFilter filter         = SpatialFilter::atrous;
    int           radius         = 2;    // 联合双边滤波半径，单位为像素。
    int           iterations     = 3;    // À-Trous 尺度数，层间步长按 2 的幂增长。
    float         color_sigma    = .65F; // 控制亮度差对空间权重的影响。
    float         normal_power   = 32;   // 法线点积的指数，值越大越保留折痕。
    float         position_sigma = .04F; // 到中心切平面的距离尺度，单位为米。
    float         history_weight = .9F;  // 通过重投影验证后分配给历史颜色的权重。
    float         clamp_sigma    = 1.F;  // 7×7 邻域限幅使用的标准差倍数。
};

// 两个参数为已处理行数与总行数，空间滤波的总量包括所有尺度。
using DenoiseProgress = std::function<void(std::size_t, std::size_t)>;
// 针孔主射线取得与离线线性图同分辨率的首个交点，普通材质编辑沿用同一场景快照。
DenoiseGuides make_guides(std::shared_ptr<const SceneSnapshot> scene, std::size_t width,
                          std::size_t height, CancellationToken token = {},
                          DenoiseProgress progress = {});
// 输入和结果均为线性颜色。各空间尺度读取上一轮完整结果，保留输入图像。
Image filter_spatial(const Image& input, const DenoiseGuides& guides, DenoiseSettings settings = {},
                     CancellationToken token = {}, DenoiseProgress progress = {});

// image 保存当前累积结果，accepted 记录通过可见性测试的像素数。
struct TemporalResult {
    Image       image;
    std::size_t accepted;
};

// 用相邻帧的两套引导变换同一表面点。跳帧或切镜时，调用方以当前空间滤波结果开始新的历史。
TemporalResult filter_temporal(const Image& current, const DenoiseGuides& guides,
                               const Image& previous, const DenoiseGuides& previous_guides,
                               DenoiseSettings settings = {}, CancellationToken token = {},
                               DenoiseProgress progress = {});
} // namespace minirender
