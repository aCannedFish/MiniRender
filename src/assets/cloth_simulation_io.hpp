#pragma once

#include <minirender/simulation/cloth_component.hpp>
#include <nlohmann/json.hpp>

namespace minirender::detail {

// 与对象的普通网格一起恢复，组件构造时检查局部位置、拓扑和保存状态的一致性。
std::shared_ptr<const ClothComponent> read_cloth_component(const nlohmann::json& value,
                                                           std::shared_ptr<const MeshData> mesh);
nlohmann::json write_cloth_component(const ClothComponent& component);
ClothCollider read_cloth_collider(const nlohmann::json& value);
nlohmann::json write_cloth_collider(const ClothCollider& collider);

} // namespace minirender::detail
