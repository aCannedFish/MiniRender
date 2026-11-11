#include "cloth_material_io.hpp"

#include <stdexcept>

namespace minirender::detail {

std::shared_ptr<const ClothAppearance> read_cloth_appearance(const nlohmann::json& value) {
    if (!value.at("threads").is_array() || value.at("threads").size() != 2 ||
        !value.at("coverage").is_array() || value.at("coverage").size() != 2) {
        throw std::invalid_argument("Cloth appearance requires two thread directions");
    }
    const auto& count = value.at("samples_per_thread");
    if (!count.is_number_integer() || count < 1 || count > 16) {
        throw std::invalid_argument("Cloth sample count must be an integer in 1..16");
    }
    ClothAppearanceSettings settings;
    settings.samples_per_thread = count.get<std::uint32_t>();
    settings.rotation_degrees = value.at("rotation_degrees").get<double>();
    settings.correlation_width_degrees = value.at("correlation_width_degrees").get<double>();
    for (std::size_t i = 0; i < 2; ++i) {
        settings.coverage[i] = value.at("coverage").at(i).get<double>();
        const auto& input = value.at("threads").at(i);
        const auto& albedo = input.at("albedo");
        if (!albedo.is_array() || albedo.size() != 3) {
            throw std::invalid_argument("Cloth thread albedo requires three values");
        }
        auto& thread = settings.threads[i];
        thread.albedo = {albedo.at(0).get<double>(), albedo.at(1).get<double>(),
                         albedo.at(2).get<double>()};
        thread.kd = input.at("kd").get<double>();
        thread.surface_width_degrees = input.at("surface_width_degrees").get<double>();
        thread.volume_width_degrees = input.at("volume_width_degrees").get<double>();
        if (!input.at("angles").is_array() || input.at("angles").size() < 2 ||
            input.at("angles").size() > 16 || !input.at("lengths").is_array() ||
            input.at("lengths").size() + 1 != input.at("angles").size()) {
            throw std::invalid_argument("Invalid cloth tangent curve dimensions");
        }
        thread.curve.angles = input.at("angles").get<std::vector<double>>();
        thread.curve.lengths = input.at("lengths").get<std::vector<double>>();
    }
    return std::make_shared<const ClothAppearance>(std::move(settings));
}

nlohmann::json write_cloth_appearance(const ClothAppearance& appearance) {
    using Json = nlohmann::json;
    const auto& settings = appearance.settings();
    Json result{{"samples_per_thread", settings.samples_per_thread},
                {"rotation_degrees", settings.rotation_degrees},
                {"correlation_width_degrees", settings.correlation_width_degrees},
                {"coverage", settings.coverage},
                {"threads", Json::array()}};
    for (const auto& thread : settings.threads) {
        result["threads"].push_back(
            {{"albedo", Json::array({thread.albedo.x, thread.albedo.y, thread.albedo.z})},
             {"kd", thread.kd},
             {"surface_width_degrees", thread.surface_width_degrees},
             {"volume_width_degrees", thread.volume_width_degrees},
             {"angles", thread.curve.angles},
             {"lengths", thread.curve.lengths}});
    }
    return result;
}

} // namespace minirender::detail
