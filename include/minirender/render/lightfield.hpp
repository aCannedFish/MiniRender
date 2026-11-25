// 光场标定、瞳孔阵列与重聚焦接口。

#pragma once

#include <minirender/render/path_tracer.hpp>

#include <functional>

namespace minirender {

// 两平面标定：瞳孔偏移以相机 right/up 的米表示，参考焦面沿 forward 测量。
// 图像保持既有左上原点和 FOV，各视图共用坐标系，不旋转相机朝向。
struct LightFieldCalibration {
    // 采集时固定的相机朝向、位置与视场角。
    Camera camera;
    // 单张子视图的图像尺寸、样本数和随机种子。
    RenderSettings render{128, 128, 32, 101};
    // 采集时的路径积分设置，写入文件以保留来源信息。
    IntegratorSettings integrator;
    // 瞳孔阵列列数，采用包含中心格点的奇数。
    std::uint32_t columns = 5;
    // 瞳孔阵列行数，采用包含中心格点的奇数。
    std::uint32_t rows = 5;
    // 采集瞳孔圆盘的标定半径，单位为米。
    double pupil_radius = .04;
    // 采集相机的参考焦面距离，沿相机前向测量，单位为米。
    double reference_focus = 4;
    // 每张子视图对应的相机 right/up 平面偏移，单位为米。
    std::vector<glm::dvec2> pupils;
    // 检查相机、视图网格、瞳孔标定和存储尺寸的合法性。
    void validate() const;
    // 返回完整标定中的子视图数量。
    std::size_t view_count() const noexcept;
    // 返回对应瞳孔中心的子视图索引。
    std::size_t center_view() const noexcept;
    // 返回单张 RGB32f 子视图的像素字节数，检查乘法溢出。
    std::uint64_t view_bytes() const;
    // 计算所有视图、像素和样本的总主路径数。
    std::uint64_t total_paths() const;
    // 由垂直视场角和图像高度计算像素单位的焦距。
    double focal_pixels() const;
};

// 奇数格点的 Shirley–Chiu 圆盘映射，中心格点精确落在瞳孔中心。
std::vector<glm::dvec2> lightfield_pupils(std::uint32_t columns, std::uint32_t rows, double radius);
// 从指定固定瞳孔发出通过参考焦面的射线，pixel_uv 采用图像归一化坐标。
Ray lightfield_ray(const LightFieldCalibration& calibration, std::size_t view, glm::dvec2 pixel_uv);
// 计算指定视图重聚焦到目标距离时的二维像素偏移。
glm::dvec2 lightfield_shift(const LightFieldCalibration& calibration, std::size_t view,
                            double focus);

// 指定焦面距离与有效孔径，决定哪些瞳孔样本参与重建。
struct LightFieldRefocusSettings {
    // 重聚焦目标焦面的前向距离，单位为米。
    double focus_distance = 4;
    // 所用瞳孔圆盘半径，单位为米。
    double aperture_radius = .04;
    // 本任务允许使用的工作缓冲预算，单位为字节。
    std::uint64_t memory_budget_bytes = 256ULL * 1024 * 1024;
};

// 报告正在处理的子视图及当前阶段的工作量。
struct LightFieldWork {
    // 当前工作或预览对应的子视图索引。
    std::size_t   view      = 0;
    std::uint64_t completed = 0;
    std::uint64_t total     = 0;
};

using LightFieldCallback = std::function<void(LightFieldWork)>;

class LightFieldReader;

// 拥有重建图像及逐像素覆盖数据，供检查孔径边缘的采样支持。
struct LightFieldReconstruction {
    explicit LightFieldReconstruction(std::size_t width, std::size_t height)
        : image(width, height), coverage(width * height, 0) {}

    // 共享只读线性图像，由结果对象保有其存储。
    Image              image;
    std::vector<float> coverage; // 有效视图数/选定孔径视图数，行主序。
    std::size_t        selected_views = 0;
    // 预计的采集工作缓冲用量。
    std::uint64_t estimated_bytes = 0;
};

// 计算重建图像、解码视图和行缓冲的工作字节数，缓存另计。
// 估计重聚焦累积、覆盖率和图像缓冲的工作内存。
std::uint64_t lightfield_refocus_scratch_bytes(const RenderSettings& render);

// 只访问文件读者和标定，不接收场景或积分器；改变焦点不会创建新射线。
LightFieldReconstruction refocus_lightfield(LightFieldReader&         reader,
                                            LightFieldRefocusSettings settings,
                                            CancellationToken         cancellation = {},
                                            LightFieldCallback        progress     = {});

} // namespace minirender
