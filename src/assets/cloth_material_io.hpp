#pragma once

#include <minirender/scene/cloth_material.hpp>
#include <nlohmann/json.hpp>

namespace minirender::detail {

// 场景材质的嵌套参数，使用原始角度/曲线保存，加载后重新编译不可变数据。
std::shared_ptr<const ClothAppearance> read_cloth_appearance(const nlohmann::json& value);
nlohmann::json write_cloth_appearance(const ClothAppearance& appearance);

} // namespace minirender::detail
