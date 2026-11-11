#pragma once

#include <minirender/scene/scene.hpp>

#include <limits>
#include <span>

namespace minirender {

inline constexpr std::uint32_t invalid_sdf_node = std::numeric_limits<std::uint32_t>::max();
enum class SdfKind { sphere, box, torus, unite, intersect, subtract };

// 球使用 parameters.x，盒使用三轴半长，绕 Y 轴的圆环使用主/管半径 x/y。
// 平移、Rz*Ry*Rx 与正均匀缩放作用于图元，组合节点通过子节点定义空间。
struct SdfNode {
    std::string id;
    std::string name;
    SdfKind kind = SdfKind::sphere;
    glm::vec3 parameters{.7F};
    glm::vec3 translation{0};
    glm::vec3 rotation_degrees{0};
    float scale = 1;
    glm::vec3 color{.6F, .7F, .8F};
    std::uint32_t left = invalid_sdf_node;
    std::uint32_t right = invalid_sdf_node;
};

struct SdfTraceSettings {
    double epsilon = 1e-4;
    double relative_epsilon = 1e-5;
    double normal_step = 2e-4;
    double maximum_distance = 100;
    std::uint32_t maximum_steps = 256;
    std::uint32_t shadow_steps = 128;
    double safety_factor = .9;
    void validate() const;
};

// 有界 CSG 图，最多 64 节点。图元可复用，引用顺序不限，所有节点均检查循环与引用。
// 独立 SDF 文档不改变三角网格场景，保存自己的相机、光照与步进参数。
struct SdfDocument {
    std::vector<SdfNode> nodes;
    std::uint32_t root = 0;
    Camera camera;
    DirectionalLight light;
    glm::vec3 ambient{.04F};
    SdfTraceSettings trace;
    void validate(CancellationToken cancellation = {}) const;
};

struct SdfBounds {
    glm::dvec3 minimum{0};
    glm::dvec3 maximum{0};
    bool empty = false;
};

// 后序编译记录可直接打包至 GPU。组合的子索引总在父节点前，sample 无动态分配。
struct CompiledSdfNode {
    SdfKind kind = SdfKind::sphere;
    glm::dmat4 world_to_local{1};
    glm::dvec3 parameters{0};
    glm::vec3 color{1};
    double scale = 1;
    std::uint32_t left = invalid_sdf_node;
    std::uint32_t right = invalid_sdf_node;
    std::uint32_t source_index = 0;
    SdfBounds bounds;
};
struct SdfSample {
    double distance = 0;
    std::uint32_t material = 0; // 编译记录索引；差集切面继承被减图元颜色。
};

// 图元返回精确距离；min/max CSG 保持 1-Lipschitz 距离界，供安全步进使用。
class SdfField final {
  public:
    explicit SdfField(const SdfDocument& document, CancellationToken cancellation = {});
    SdfSample sample(glm::dvec3 point) const;
    glm::dvec3 normal(glm::dvec3 point, double step) const;
    const SdfBounds& bounds() const noexcept;
    std::span<const CompiledSdfNode> nodes() const noexcept;
    std::uint32_t root() const noexcept;

  private:
    std::vector<CompiledSdfNode> nodes_;
    std::uint32_t root_ = 0;
};

enum class SdfTraceState { hit, miss, step_limit };
struct SdfTraceResult {
    SdfTraceState state = SdfTraceState::miss;
    double distance = 0;
    double residual = 0;
    std::uint32_t steps = 0;
    std::uint32_t material = invalid_sdf_node;
    glm::dvec3 position{0};
};

// 单位方向射线从给定区间步进，先用根 AABB 剪枝；内部起点用绝对距离寻找出口。
// step_limit 与 miss 分开报告，便于步数热图和参数检查。
SdfTraceResult trace_sdf(const SdfField& field, const Ray& ray, SdfTraceSettings settings = {},
                         double minimum_distance = 0,
                         double maximum_distance = std::numeric_limits<double>::infinity(),
                         CancellationToken cancellation = {});

SdfDocument make_sdf_example();
std::string sdf_kind_name(SdfKind kind);
SdfKind parse_sdf_kind(const std::string& text);

} // namespace minirender
