#include <minirender/assets/lightfield_io.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace minirender {
std::uint64_t lightfield_refocus_scratch_bytes(const RenderSettings& render) {
    render.validate();
    return std::uint64_t(render.width) * render.height *
               (sizeof(glm::dvec3) + sizeof(std::uint16_t) + sizeof(float) +
                2 * sizeof(glm::vec3)) +
           65536;
}

LightFieldReconstruction refocus_lightfield(LightFieldReader& reader,
                                            LightFieldRefocusSettings settings,
                                            CancellationToken cancellation,
                                            LightFieldCallback progress) {
    cancellation.check();
    const auto& c = reader.file().calibration();
    if (!std::isfinite(settings.focus_distance) || settings.focus_distance < 1e-6 ||
        settings.focus_distance > 1e6 || !std::isfinite(settings.aperture_radius) ||
        settings.aperture_radius < 0 || settings.aperture_radius > c.pupil_radius ||
        settings.memory_budget_bytes < 1024 * 1024 ||
        settings.memory_budget_bytes > 8ULL * 1024 * 1024 * 1024) {
        throw std::invalid_argument("Invalid light field focus, aperture or memory budget");
    }
    const auto pixels = c.render.width * c.render.height;
    const std::uint64_t required =
        lightfield_refocus_scratch_bytes(c.render) +
        std::min(reader.cache_budget_bytes(), c.view_bytes() * c.view_count());
    if (required > settings.memory_budget_bytes) {
        throw std::length_error("Light field refocus memory budget exceeded; reduce image/cache "
                                "size or increase the budget");
    }
    std::vector<std::size_t> selected;
    for (std::size_t v = 0; v < c.view_count(); ++v) {
        if (glm::length(c.pupils[v]) <= settings.aperture_radius) {
            selected.push_back(v);
        }
    }
    LightFieldReconstruction result(c.render.width, c.render.height);
    result.selected_views = selected.size();
    result.estimated_bytes = required;
    std::vector<glm::dvec3> sums(pixels, glm::dvec3(0));
    std::vector<std::uint16_t> counts(pixels, 0);
    std::uint64_t completed = 0;
    const auto total = std::uint64_t(selected.size()) * c.render.height;
    for (const auto view : selected) {
        cancellation.check();
        const auto image = reader.view(view, cancellation);
        const auto shift = lightfield_shift(c, view, settings.focus_distance);
        for (std::size_t y = 0; y < c.render.height; ++y) {
            cancellation.check();
            for (std::size_t x = 0; x < c.render.width; ++x) {
                const double sx = double(x) + shift.x, sy = double(y) + shift.y;
                if (sx < 0 || sy < 0 || sx > double(c.render.width - 1) ||
                    sy > double(c.render.height - 1)) {
                    continue;
                }
                const auto ix = std::size_t(std::floor(sx)), iy = std::size_t(std::floor(sy));
                const auto jx = std::min(ix + 1, c.render.width - 1),
                           jy = std::min(iy + 1, c.render.height - 1);
                const double tx = sx - double(ix), ty = sy - double(iy);
                const auto color = (1 - ty) * ((1 - tx) * glm::dvec3(image->at(ix, iy)) +
                                               tx * glm::dvec3(image->at(jx, iy))) +
                                   ty * ((1 - tx) * glm::dvec3(image->at(ix, jy)) +
                                         tx * glm::dvec3(image->at(jx, jy)));
                const auto pixel = y * c.render.width + x;
                sums[pixel] += color;
                ++counts[pixel];
            }
            ++completed;
            if (progress) {
                progress({view, completed, total});
            }
        }
    }
    for (std::size_t y = 0; y < c.render.height; ++y) {
        cancellation.check();
        for (std::size_t x = 0; x < c.render.width; ++x) {
            const auto p = y * c.render.width + x;
            result.image.at(x, y) =
                counts[p] ? glm::vec3(sums[p] / double(counts[p])) : glm::vec3(0);
            result.coverage[p] = float(counts[p]) / float(selected.size());
        }
    }
    return result;
}

} // namespace minirender
