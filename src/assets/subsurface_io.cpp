#include "subsurface_io.hpp"

#include <stdexcept>

namespace minirender::detail {
namespace {
glm::dvec3 coefficients(const nlohmann::json& value) {
    if (!value.is_array() || value.size() != 3) {
        throw std::invalid_argument("Subsurface coefficients require three RGB numbers");
    }
    return {value.at(0).get<double>(), value.at(1).get<double>(), value.at(2).get<double>()};
}
} // namespace

std::shared_ptr<const SubsurfaceParameters> read_subsurface(const nlohmann::json& value) {
    SubsurfaceParameters parameters;
    parameters.absorption = coefficients(value.at("absorption"));
    parameters.reduced_scattering = coefficients(value.at("reduced_scattering"));
    parameters.density = value.at("density").get<double>();
    parameters.validate();
    return std::make_shared<const SubsurfaceParameters>(parameters);
}
nlohmann::json write_subsurface(const SubsurfaceParameters& parameters) {
    parameters.validate();
    return {
        {"absorption", {parameters.absorption.x, parameters.absorption.y, parameters.absorption.z}},
        {"reduced_scattering",
         {parameters.reduced_scattering.x, parameters.reduced_scattering.y,
          parameters.reduced_scattering.z}},
        {"density", parameters.density}};
}
} // namespace minirender::detail
