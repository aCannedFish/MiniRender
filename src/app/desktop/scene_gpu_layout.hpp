// 场景着色器与主机共享的缓冲区布局及字节偏移检查。

#pragma once
#include "gpu_cloth.hpp"
#include <array>
#include <cstddef>
#include <glm/mat4x4.hpp>
#include <minirender/render/ssao.hpp>

namespace scene_gpu {
// std430 按 16 字节对齐。normal 使用 mat4，避免 CPU mat3 的紧密布局与 GLSL 不一致。
struct alignas(16) GpuInstance {
    glm::mat4  object_to_world; // 当前物体的局部到世界矩阵。
    glm::mat4  normal_to_world; // 逆转置法线矩阵，按 mat4 的 std430 步长上传。
    glm::vec4  base_color;      // 线性 RGB 底色。
    glm::vec4  emission;        // RGB 为自发光，w 编码粗糙导体的补偿模式。
    glm::vec4  surface;         // roughness, metallic, texture filter, double sided
    GpuCloth   cloth;           // 与 GLSL 一致的经纬纱材质参数。
    glm::uvec4 geometry;        // 首个 PRT 面角、稳定物体 ID、法线贴图强度位模式、保留位。
    glm::mat4  previous_model;  // 刚体重投影使用的上一版模型矩阵。
};

static_assert(sizeof(GpuInstance) == 880);
static_assert(offsetof(GpuInstance, normal_to_world) == 64);
static_assert(offsetof(GpuInstance, base_color) == 128);
static_assert(offsetof(GpuInstance, surface) == 160);
static_assert(offsetof(GpuInstance, cloth) == 176);

// 顶点与片元阶段共用的推送常量，光源索引按绘制通道解释。
struct DrawIndex {
    std::int32_t light;
    std::int32_t instance;
};

// 一个方向光的传播方向与线性 RGB 辐亮度，对应 GLSL DirectionalLight。
struct alignas(16) GpuLight {
    glm::vec4 direction;
    glm::vec4 radiance;
};

// 对应 scene_frame.glsl 的 std140 块。修改字段时同步检查以下偏移与总字节数。
struct alignas(16) FrameUniform {
    glm::mat4                view_projection; // 世界坐标到 Vulkan 裁剪空间。
    glm::vec4                camera_position; // 世界空间的相机位置。
    glm::vec4                environment;     // 场景常量环境辐亮度。
    std::array<GpuLight, 8>  lights;
    glm::ivec4               counts;  // 方向光数量、环境背景开关、诊断光源索引、右侧补偿设置。
    glm::vec4                display; // 曝光、视图编号、FXAA 开关、保留位。
    std::array<glm::mat4, 8> shadow_view_projection; // 每层方向光深度图的世界到裁剪空间变换。
    glm::vec4                shadow_settings;        // 阴影开关、固定半径、过滤方式和光源半角正切。
    glm::vec4                viewport; // 前两项为像素宽高的倒数，后两项为近远裁剪距离。
    glm::mat4                view;
    glm::mat4                projection;
    glm::mat4                inverse_projection;
    glm::vec4                ao_settings; // enabled, radius/m, bias/m, strength
    glm::vec4                ao_filter;   // samples, depth sigma/m, normal exponent, use filtered
    std::array<glm::vec4, minirender::ssao_kernel_size> ao_kernel;
    glm::vec4 ibl;              // BRDF ready, maps ready, strength, rotation/radians
    glm::vec4 preview_edges;    // effective strength, physical pixels per logical pixel, reserved
    glm::vec4 teaching_light;   // 教学点光源的世界空间坐标和强度。
    glm::vec4 teaching_surface; // Blinn–Phong 高光指数、镜面系数和纹理开关。
    glm::vec4 area_settings;    // 矩形光源数、采样网格边长及单个阴影纹素的归一化宽度。
    std::array<glm::vec4, 9> prt_light;        // 九项环境球谐系数，w 标记传输数据是否可用。
    glm::vec4                screen_trace;     // 屏幕射线最大距离、厚度、步数与方向样本数。
    glm::vec4                screen_options;   // Hi-Z 开关和时域采样的帧序号。
    glm::mat4                previous_vp;      // 同一侧历史图像的世界到裁剪空间矩阵。
    glm::vec4                history_settings; // 历史有效、刚体移动、混合权重、邻域限幅倍数。
};

static_assert(offsetof(FrameUniform, lights) == 96);
static_assert(offsetof(FrameUniform, shadow_view_projection) == 384);
static_assert(offsetof(FrameUniform, view) == 928);
static_assert(offsetof(FrameUniform, ao_kernel) == 1152);
static_assert(offsetof(FrameUniform, ibl) == 2176);
static_assert(offsetof(FrameUniform, preview_edges) == 2192);
static_assert(sizeof(FrameUniform) == 2512);

} // namespace scene_gpu
