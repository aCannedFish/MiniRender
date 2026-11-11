#include "medium_io.hpp"

#include <stdexcept>

namespace minirender::detail {
namespace {
using Json = nlohmann::json;
glm::dvec3 vector(const Json& value) {
    if (!value.is_array() || value.size() != 3) {
        throw std::invalid_argument("Medium vector requires three numbers");
    }
    return {value.at(0).get<double>(), value.at(1).get<double>(), value.at(2).get<double>()};
}
Json array(glm::dvec3 value) {
    return Json::array({value.x, value.y, value.z});
}
} // namespace

std::vector<MediumRegion> read_media(const Json& value) {
    if (!value.is_array() || value.size() > maximum_medium_regions) {
        throw std::invalid_argument("Media must be an array of at most 32 regions");
    }
    std::vector<MediumRegion> result;
    for (const auto& item : value) {
        MediumRegion region;
        region.id = item.at("id").get<std::string>();
        region.name = item.value("name", region.id);
        region.minimum = vector(item.at("minimum"));
        region.maximum = vector(item.at("maximum"));
        region.medium.absorption = vector(item.at("absorption"));
        region.medium.scattering = vector(item.at("scattering"));
        region.medium.anisotropy = item.at("anisotropy").get<double>();
        region.enabled = item.value("enabled", true);
        result.push_back(std::move(region));
    }
    validate_medium_regions(result);
    return result;
}
Json write_media(std::span<const MediumRegion> regions) {
    Json result = Json::array();
    for (const auto& region : regions) {
        result.push_back({{"id", region.id},
                          {"name", region.name},
                          {"minimum", array(region.minimum)},
                          {"maximum", array(region.maximum)},
                          {"absorption", array(region.medium.absorption)},
                          {"scattering", array(region.medium.scattering)},
                          {"anisotropy", region.medium.anisotropy},
                          {"enabled", region.enabled}});
    }
    return result;
}
} // namespace minirender::detail
