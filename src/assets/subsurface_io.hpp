#pragma once

#include <minirender/scene/subsurface.hpp>
#include <nlohmann/json.hpp>

namespace minirender::detail {

// 光学系数使用 double 数组独立读写，不经过场景颜色的 float 转换。
std::shared_ptr<const SubsurfaceParameters> read_subsurface(const nlohmann::json& value);
nlohmann::json write_subsurface(const SubsurfaceParameters& parameters);

} // namespace minirender::detail
