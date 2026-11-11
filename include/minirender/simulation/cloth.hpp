#pragma once

#include <minirender/scene/scene.hpp>

#include <functional>
#include <span>

namespace minirender {

// origin 和两条完整边向量以世界空间米为单位。位置按 row*columns+column 存储，
// 正面朝 cross(span_u, span_v)，UV 从 (0,0) 到 (1,1)。固定索引引用静止位置。
struct ClothGrid {
    std::uint32_t columns = 25;
    std::uint32_t rows = 25;
    glm::dvec3 origin{-1, 1.7F, 1};
    glm::dvec3 span_u{2, 0, 0};
    glm::dvec3 span_v{0, 0, -2};
    std::vector<std::uint32_t> pinned;
    void validate() const;
};

// 刚度为 N/m，面密度为 kg/m²，速度阻尼为 s^-1。弹簧阻尼比无量纲，
// 每条弹簧使用两端有效质量计算 c=2*zeta*sqrt(k*mu)。固定帧内执行相同数量子步。
struct ClothParameters {
    double surface_density = .15;
    double structural_stiffness = 200;
    double shear_stiffness = 100;
    double bending_stiffness = 5;
    double spring_damping_ratio = .05;
    double velocity_damping = .2;
    double thickness = .006;
    double frame_seconds = 1.0 / 60;
    std::uint32_t substeps = 64;
    void validate() const;
};

// 平面法线指向允许粒子停留的半空间。摩擦系数采用库仑速度响应，接触不回弹。
struct ClothPlane {
    glm::dvec3 point{0};
    glm::dvec3 normal{0, 1, 0};
    double friction = .3;
    bool operator==(const ClothPlane&) const = default;
};
struct ClothSphere {
    glm::dvec3 center{0, .65, 0};
    double radius = .65;
    double friction = .3;
    bool operator==(const ClothSphere&) const = default;
};

// 风速为世界空间 m/s。空气压力按三角形面积和相对法向速度的平方分配到三个端点。
// 碰撞球半径和单面平面都会外扩 thickness，所有碰撞体保持静止。
struct ClothEnvironment {
    glm::dvec3 gravity{0, -9.81, 0};
    glm::dvec3 wind_velocity{0};
    double air_density = 1.225;
    double drag_coefficient = .8;
    std::vector<ClothPlane> planes;
    std::vector<ClothSphere> spheres;
    void validate() const;
    bool operator==(const ClothEnvironment&) const = default;
};

enum class ClothSpringKind { structural, shear, bending };
// 每条边只保存一次。弯曲弹簧连接相隔两个采样点的端点，rest_direction 是单位方向。
struct ClothSpring {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    ClothSpringKind kind = ClothSpringKind::structural;
    double rest_length = 0;
    double stiffness = 0;
    double damping = 0;
    glm::dvec3 rest_direction{1, 0, 0};
};

// 返回 a 端的胡克力与轴向阻尼力，b 端受相反的力。零长状态沿静止方向恢复。
glm::dvec3 cloth_spring_force(const ClothSpring& spring, glm::dvec3 a, glm::dvec3 b,
                              glm::dvec3 velocity_a, glm::dvec3 velocity_b);

// 世界空间 double 状态。frame 统计完整模拟帧，外部替换状态时检查固定点及数组长度。
struct ClothState {
    std::vector<glm::dvec3> positions;
    std::vector<glm::dvec3> velocities;
    std::uint64_t frame = 0;
};
// 能量单位为 J，动量单位为 kg*m/s；maximum_spring_ratio 统计全部邻接弹簧的长度比。
struct ClothStatistics {
    double time_seconds = 0;
    double kinetic_energy = 0;
    double elastic_energy = 0;
    double maximum_spring_ratio = 0;
    glm::dvec3 momentum{0};
};
// contacts 按粒子接触事件累计，contact_limits 统计达到八次事件上限后停止的粒子。
struct ClothStepReport {
    std::size_t substeps = 0;
    std::size_t contacts = 0;
    std::size_t contact_limits = 0;
};
// 可直接建立普通场景实例的不可变网格与变换。后续模拟不改变已冻结的数据。
struct FrozenCloth {
    std::shared_ptr<const MeshData> mesh;
    Transform transform;
};
using ClothProgress = std::function<void(std::size_t completed, std::size_t total)>;

// 单线程拥有的模拟器。advance 在副本上运行，成功后发布完整状态；取消或错误保留旧状态。
// 固定拓扑、质量和参数在构造时建立。forces/statistics 供数值检查与界面诊断读取。
class ClothSimulation final {
  public:
    explicit ClothSimulation(ClothGrid grid = {}, ClothParameters parameters = {},
                             ClothEnvironment environment = {},
                             CancellationToken cancellation = {});
    const ClothGrid& grid() const noexcept;
    const ClothParameters& parameters() const noexcept;
    const ClothEnvironment& environment() const noexcept;
    const ClothState& state() const noexcept;
    std::span<const double> masses() const noexcept;
    std::span<const ClothSpring> springs() const noexcept;
    std::span<const std::array<std::uint32_t, 3>> triangles() const noexcept;
    // 线性弹簧和阻尼行和估计出的保守步数，供参数界面解释过大的时间步。
    std::uint32_t minimum_substeps() const noexcept;
    void reset();
    void set_state(ClothState state, CancellationToken cancellation = {});
    void set_environment(ClothEnvironment environment);
    std::vector<glm::dvec3> forces(CancellationToken cancellation = {}) const;
    ClothStatistics statistics(CancellationToken cancellation = {}) const;
    ClothStepReport advance(std::uint32_t frames = 1, CancellationToken cancellation = {},
                            ClothProgress progress = {});
    // 网格坐标减去 grid.origin，随返回的平移变换恢复世界位置，保留 float 局部细节。
    // 折痕处的法线相消时采用最大面积邻面；退化三角形报错。
    FrozenCloth freeze(CancellationToken cancellation = {}) const;

  private:
    void validate_state(ClothState& state, CancellationToken cancellation) const;
    void calculate_forces(const ClothState& state, std::span<glm::dvec3> forces,
                          CancellationToken cancellation) const;
    void validate_anchors(const ClothEnvironment& environment) const;
    ClothGrid grid_;
    ClothParameters parameters_;
    ClothEnvironment environment_;
    std::vector<glm::dvec3> rest_;
    std::vector<double> masses_;
    std::vector<bool> pinned_;
    std::vector<ClothSpring> springs_;
    std::vector<std::array<std::uint32_t, 3>> triangles_;
    ClothState state_;
    std::uint32_t minimum_substeps_ = 1;
};

} // namespace minirender
