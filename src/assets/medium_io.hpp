#pragma once

#include <minirender/scene/medium.hpp>
#include <nlohmann/json.hpp>
#include <vector>

namespace minirender::detail {
// 媒体字段独立编解码，保留 double 系数与世界边界；旧场景缺省为空数组。
std::vector<MediumRegion> read_media(const nlohmann::json& value);
nlohmann::json write_media(std::span<const MediumRegion> regions);
} // namespace minirender::detail
