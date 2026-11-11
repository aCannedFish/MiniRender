#pragma once

#include <minirender/simulation/cloth.hpp>

namespace minirender {

// 网格和已保存状态位于物体局部坐标，重力、风和环境中的附加碰撞体位于世界空间。
// 配方在帧之间共享；运行时将子步数作为下限，按世界尺寸计算固定的实际子步数。
struct ClothDefinition {
    ClothGrid grid;
    ClothParameters parameters;
    ClothEnvironment environment;
    bool use_scene_colliders = true;
    void validate() const;
};

// 不可变的场景组件。状态与对应网格一起验证，文档可以用指针身份检查二者的一致性。
// 创建新帧不修改旧组件，因此保存、历史和离线任务可共享旧值。
class ClothComponent final {
  public:
    ClothComponent(std::shared_ptr<const ClothDefinition> definition, ClothState local_state,
                   std::shared_ptr<const MeshData> mesh, CancellationToken cancellation = {});
    const std::shared_ptr<const ClothDefinition>& definition() const noexcept;
    const ClothState& state() const noexcept;
    const std::shared_ptr<const MeshData>& mesh() const noexcept;

  private:
    std::shared_ptr<const ClothDefinition> definition_;
    ClothState state_;
    std::shared_ptr<const MeshData> mesh_;
};

// 将场景碰撞体变换到世界坐标，保留配方中的固定外力与附加静态碰撞体。
ClothEnvironment cloth_environment(const ClothDefinition& definition, const SceneDocument& scene);

// 创建世界空间模拟器并从局部状态恢复。返回对象由单个调用线程持有。
// 世界变换后的质量和刚度决定实际固定子步，最大 4096。
std::unique_ptr<ClothSimulation> make_cloth_simulation(const ClothComponent& component,
                                                       const Transform& transform,
                                                       ClothEnvironment environment,
                                                       CancellationToken cancellation = {});

struct ClothPreparedFrame {
    std::shared_ptr<const ClothComponent> component;
    ClothStatistics statistics;
    std::uint32_t effective_substeps = 0;
};

// 将世界状态映回物体局部坐标，计算可显示的网格，并生成可保存的组件快照。
ClothPreparedFrame prepare_cloth_frame(const ClothSimulation& simulation,
                                       std::shared_ptr<const ClothDefinition> definition,
                                       const Transform& transform,
                                       CancellationToken cancellation = {});

// 静止网格与零速度，用于创建和重置。物体变换由场景实例独立保存。
std::shared_ptr<const ClothComponent> make_cloth_component(ClothDefinition definition,
                                                           CancellationToken cancellation = {});

} // namespace minirender
