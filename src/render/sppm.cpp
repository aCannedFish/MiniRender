#include "sppm_grid.hpp"
#include "sppm_surface.hpp"
#include <minirender/render/sppm.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <mutex>
#include <numbers>
#include <thread>

namespace minirender {
namespace {
using detail::SppmGrid;
using detail::SppmGridPoint;
using detail::SppmSurface;
double maximum(glm::dvec3 value) {
    return std::max({value.x, value.y, value.z});
}
double luminance(glm::dvec3 value) {
    return glm::dot(value, glm::dvec3(.2126, .7152, .0722));
}
bool finite(glm::dvec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
void check(glm::dvec3 value) {
    if (!finite(value) || glm::any(glm::lessThan(value, glm::dvec3(0)))) {
        throw std::runtime_error("SPPM contribution is nonfinite or negative");
    }
}
struct CameraPoint {
    std::optional<SppmSurface> surface;
    glm::dvec3 beta{0}, direct{0};
    std::uint32_t depth = 0;
    bool specular_prefix = false;
};
struct PhotonAccum {
    glm::dvec3 phi{0};
    std::uint64_t count = 0;
};

// 独立索引决定随机流，线程只决定由谁计算。异常先收回全部线程，再交还控制线程。
template <class Function, class Progress>
void parallel(std::size_t count, std::uint32_t workers, CancellationToken token, Function function,
              Progress progress) {
    std::atomic<std::uint64_t> completed{0};
    std::atomic<bool> failed{false};
    std::exception_ptr error;
    std::mutex error_mutex;
    const auto work = [&](std::uint32_t worker) {
        try {
            for (std::size_t i = worker; i < count; i += workers) {
                if (failed.load(std::memory_order_relaxed)) {
                    break;
                }
                token.check();
                function(i, worker);
                const auto done = completed.fetch_add(1, std::memory_order_relaxed) + 1;
                if (worker == 0 && ((done & 63U) == 0 || i + workers >= count)) {
                    progress(done, count);
                }
            }
        } catch (...) {
            std::lock_guard lock(error_mutex);
            if (!error) {
                error = std::current_exception();
            }
            failed.store(true, std::memory_order_relaxed);
        }
    };
    std::vector<std::thread> threads;
    const auto join = [&] {
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    };
    try {
        for (std::uint32_t worker = 1; worker < workers; ++worker) {
            threads.emplace_back(work, worker);
        }
    } catch (...) {
        failed.store(true, std::memory_order_relaxed);
        join();
        throw;
    }
    work(0);
    join();
    if (error) {
        std::rethrow_exception(error);
    }
    token.check();
    progress(count, count);
}
} // namespace

void SppmOptions::validate() const {
    if (photons_per_iteration == 0 || photons_per_iteration > 100000000 ||
        !std::isfinite(initial_radius) || initial_radius < 1e-6 || initial_radius > 1e6 ||
        !std::isfinite(alpha) || alpha <= 0 || alpha >= 1 || memory_budget_bytes < 1024 * 1024 ||
        memory_budget_bytes > 8ULL * 1024 * 1024 * 1024) {
        throw std::invalid_argument("Invalid SPPM photons, radius, alpha or memory budget");
    }
}
std::string_view sppm_phase_name(SppmPhase phase) {
    switch (phase) {
    case SppmPhase::camera:
        return "camera";
    case SppmPhase::grid:
        return "grid";
    case SppmPhase::photons:
        return "photons";
    case SppmPhase::update:
        return "update";
    }
    throw std::invalid_argument("Unknown SPPM phase");
}
SppmPixel update_sppm_pixel(SppmPixel previous, std::uint64_t photons, glm::dvec3 phi,
                            glm::dvec3 direct, double alpha) {
    if (!std::isfinite(previous.radius) || previous.radius <= 0 ||
        !std::isfinite(previous.effective_photons) || previous.effective_photons < 0 ||
        !std::isfinite(alpha) || alpha <= 0 || alpha >= 1) {
        throw std::invalid_argument("Invalid persistent SPPM pixel");
    }
    check(phi);
    check(direct);
    check(previous.tau);
    check(previous.direct_sum);
    previous.direct_sum += direct;
    if (photons > 0) {
        const double n = previous.effective_photons + alpha * double(photons);
        const double ratio = n / (previous.effective_photons + double(photons));
        previous.radius *= std::sqrt(ratio);
        previous.tau = (previous.tau + phi) * ratio;
        previous.effective_photons = n;
    }
    check(previous.tau);
    check(previous.direct_sum);
    return previous;
}

struct SppmIntegrator::Impl {
    std::shared_ptr<const SceneSnapshot> scene;
    RenderSettings film;
    IntegratorSettings settings;
    std::unique_ptr<SceneBvh> bvh;
    std::unique_ptr<LightSampler> lights;
    std::vector<SppmPixel> pixels;
    SppmStatistics statistics;

    Impl(std::shared_ptr<const SceneSnapshot> input, RenderSettings render,
         IntegratorSettings options, std::uint32_t threads, CancellationToken token)
        : scene(std::move(input)), film(render), settings(options) {
        film.validate();
        settings.validate();
        token.check();
        if (!scene || settings.kind != IntegratorKind::sppm || threads > 32) {
            throw std::invalid_argument(
                "SPPM requires a scene, SPPM settings and at most 32 workers");
        }
        if (scene->camera.aperture_radius != 0) {
            throw std::invalid_argument("SPPM currently requires a pinhole camera");
        }
        for (const auto& region : scene->media) {
            if (region.enabled && maximum(region.medium.extinction()) > 0) {
                throw std::invalid_argument(
                    "SPPM estimates surface transport; use path tracing for active media");
            }
        }
        const auto hardware = std::max(1U, std::thread::hardware_concurrency());
        statistics.workers =
            threads == 0 ? std::min(32U, hardware > 1 ? hardware - 1 : 1) : threads;
        const auto count = film.width * film.height;
        statistics.workers = std::uint32_t(std::min(std::size_t(statistics.workers), count));
        statistics.estimated_bytes =
            std::uint64_t(count) *
                (2 * sizeof(SppmPixel) + sizeof(CameraPoint) +
                 statistics.workers * sizeof(PhotonAccum) + sizeof(glm::vec3) + 128) +
            SppmGrid::estimated_bytes(count);
        if (statistics.estimated_bytes > settings.sppm.memory_budget_bytes) {
            throw std::length_error(
                "SPPM memory budget exceeded; reduce resolution/workers or increase the budget");
        }
        bvh = std::make_unique<SceneBvh>(scene, token);
        lights = std::make_unique<LightSampler>(scene, token);
        pixels.resize(count);
        for (auto& pixel : pixels) {
            pixel.radius = settings.sppm.initial_radius;
        }
        statistics.minimum_radius = statistics.maximum_radius = statistics.mean_radius =
            settings.sppm.initial_radius;
    }

    glm::dvec3 visibility(const SurfaceHit& hit, const LightSample& light, TraceStatistics& trace,
                          CancellationToken token) const {
        auto ray = spawn_ray(hit, light.direction);
        double limit = std::numeric_limits<double>::infinity();
        if (std::isfinite(light.distance)) {
            SurfaceHit endpoint;
            endpoint.position = light.position;
            endpoint.geometric_normal = glm::vec3(light.normal);
            endpoint.position_error = light.position_error;
            const auto delta = offset_origin(endpoint, -light.direction) - ray.origin;
            limit = glm::length(delta);
            if (limit == 0) {
                return glm::dvec3(0);
            }
            ray.direction = delta / limit;
            limit = std::nextafter(limit, 0.0);
        }
        return bvh->occluded(ray, {0, limit}, &trace, token) ? glm::dvec3(0) : light.radiance;
    }

    CameraPoint camera(std::size_t pixel, std::uint64_t iteration, SppmStatistics& stats,
                       CancellationToken token) const {
        auto sampler = Sampler::for_sample(film.seed, pixel, iteration);
        const auto sample = sampler.next_2d();
        const auto lens = sampler.next_2d();
        const glm::dvec2 uv{(double(pixel % film.width) + sample.x) / double(film.width),
                            (double(pixel / film.width) + sample.y) / double(film.height)};
        auto ray =
            scene->camera.generate_lens_ray(uv, double(film.width) / double(film.height), lens);
        const auto forward =
            glm::normalize(glm::dvec3(scene->camera.target) - glm::dvec3(scene->camera.position));
        const double cosine = glm::dot(ray.direction, forward);
        RayInterval interval{scene->camera.near_plane / cosine, scene->camera.far_plane / cosine};
        CameraPoint result;
        glm::dvec3 beta(1), previous(0);
        double previous_pdf = 0, eta_scale = 1;
        bool delta = true;
        for (std::uint32_t depth = 0;; ++depth) {
            token.check();
            const auto hit = bvh->intersect(ray, interval, &stats.geometry, token);
            const auto emission_weight = [&](double pdf) {
                return depth == 0 || delta ? 1.0 : power_heuristic(previous_pdf, pdf);
            };
            if (!hit) {
                result.direct += beta * lights->environment().evaluate(ray.direction) *
                                 emission_weight(lights->environment_pdf(ray.direction));
                break;
            }
            const auto& material = scene->instances[hit->instance_index].material;
            result.direct += beta * glm::dvec3(material.emission) *
                             emission_weight(lights->area_pdf(previous, *hit));
            if (depth == settings.maximum_depth) {
                break;
            }
            SppmSurface surface(*scene, *hit, -ray.direction);
            const bool stop =
                !surface.delta() && (!surface.glossy() || depth + 1 == settings.maximum_depth);
            const double light_u = sampler.next_1d();
            const auto light_surface = sampler.next_2d();
            const double component = sampler.next_1d();
            const auto direction = sampler.next_2d();
            if (!surface.delta()) {
                const auto light = lights->sample(hit->position, light_u, light_surface);
                if (light) {
                    const auto response = surface.response(light->direction);
                    if (maximum(response) > 0) {
                        const double weight = stop || light->measure == SampleMeasure::discrete
                                                  ? 1
                                                  : power_heuristic(light->probability,
                                                                    surface.pdf(light->direction));
                        ++stats.shadow_rays;
                        result.direct += beta * response *
                                         visibility(*hit, *light, stats.geometry, token) *
                                         (weight / light->probability);
                    }
                }
            }
            if (stop) {
                result.surface = std::move(surface);
                result.beta = beta;
                result.depth = depth;
                break;
            }
            const auto bsdf = surface.sample(component, direction, TransportMode::radiance);
            if (!bsdf || maximum(bsdf->weight) == 0) {
                break;
            }
            beta *= bsdf->weight;
            ++stats.camera_scattering_events;
            check(beta);
            previous = hit->position;
            previous_pdf = bsdf->probability;
            delta = bsdf->measure == SampleMeasure::discrete;
            result.specular_prefix = result.specular_prefix || delta;
            eta_scale *= bsdf->eta * bsdf->eta;
            ray = spawn_ray(*hit, bsdf->direction);
            interval = {0, std::numeric_limits<double>::infinity()};
            const double rr = sampler.next_1d();
            if (settings.russian_roulette && depth + 1 >= settings.roulette_depth) {
                const double survival = std::clamp(maximum(beta * eta_scale), .05, .95);
                if (rr >= survival) {
                    break;
                }
                beta /= survival;
            }
        }
        check(result.direct);
        return result;
    }

    void photon(std::size_t index, std::uint64_t iteration, const SppmGrid& grid,
                const std::vector<CameraPoint>& points, std::vector<PhotonAccum>& accum,
                SppmStatistics& stats, CancellationToken token) const {
        auto sampler = Sampler::for_sample(film.seed ^ 0x5350504d50484f54ULL, index, iteration);
        const double choice = sampler.next_1d();
        const auto position = sampler.next_2d();
        const auto direction = sampler.next_2d();
        const auto emission = lights->sample_emission(choice, position, direction);
        if (!emission || maximum(emission->flux) == 0) {
            return;
        }
        auto beta = emission->flux;
        auto ray = emission->ray;
        bool specular_prefix = false;
        for (std::uint32_t depth = 0; depth < settings.maximum_depth; ++depth) {
            token.check();
            const auto hit = bvh->intersect(ray, {0, std::numeric_limits<double>::infinity()},
                                            &stats.geometry, token);
            if (!hit) {
                break;
            }
            ++stats.photon_hits;
            if (depth > 0) {
                grid.query(hit->position, [&](std::size_t pixel) {
                    const auto& point = points[pixel];
                    if (point.depth + depth + 1 > settings.maximum_depth ||
                        point.surface->hit.instance_index != hit->instance_index ||
                        glm::dot(point.surface->hit.geometric_normal, hit->geometric_normal) <
                            .5F) {
                        return;
                    }
                    const auto value = point.surface->gather(-ray.direction) * beta;
                    check(value);
                    accum[pixel].phi += value;
                    ++accum[pixel].count;
                    ++stats.gathered_contributions;
                    if (specular_prefix && maximum(value) > 0) {
                        ++stats.specular_light_contributions;
                        if (point.specular_prefix) {
                            ++stats.sds_contributions;
                        }
                    }
                });
            }
            if (depth + 1 == settings.maximum_depth) {
                break;
            }
            const SppmSurface surface(*scene, *hit, -ray.direction);
            const double component = sampler.next_1d();
            const auto direction_u = sampler.next_2d();
            const auto bsdf = surface.sample(component, direction_u, TransportMode::importance);
            if (!bsdf || maximum(bsdf->weight) == 0) {
                break;
            }
            auto next = beta * bsdf->weight;
            check(next);
            if (maximum(next) == 0) {
                break;
            }
            ++stats.photon_scattering_events;
            specular_prefix = specular_prefix || bsdf->measure == SampleMeasure::discrete;
            const double rr = sampler.next_1d();
            if (settings.russian_roulette && depth + 1 >= settings.roulette_depth) {
                const double survival = std::clamp(luminance(next) / luminance(beta), .05, .95);
                if (rr >= survival) {
                    break;
                }
                next /= survival;
            }
            beta = next;
            ray = spawn_ray(*hit, bsdf->direction);
        }
    }
};

SppmIntegrator::SppmIntegrator(std::shared_ptr<const SceneSnapshot> scene, RenderSettings render,
                               IntegratorSettings settings, std::uint32_t threads,
                               CancellationToken cancellation)
    : impl_(std::make_unique<Impl>(std::move(scene), render, settings, threads, cancellation)) {}
SppmIntegrator::~SppmIntegrator() = default;
const SppmStatistics& SppmIntegrator::statistics() const noexcept {
    return impl_->statistics;
}
std::span<const SppmPixel> SppmIntegrator::pixels() const noexcept {
    return impl_->pixels;
}

void SppmIntegrator::advance(CancellationToken token, SppmCallback callback) {
    auto& self = *impl_;
    token.check();
    if (self.statistics.iterations >= self.film.samples_per_pixel) {
        throw std::logic_error("All requested SPPM iterations are complete");
    }
    const auto report = [&](SppmPhase phase, std::uint64_t completed, std::uint64_t total) {
        token.check();
        if (callback) {
            callback({phase, completed, total});
        }
    };
    const auto count = self.pixels.size();
    const auto workers = self.statistics.workers;
    std::vector<CameraPoint> points(count);
    std::vector<SppmStatistics> local(workers);
    report(SppmPhase::camera, 0, count);
    parallel(
        count, workers, token,
        [&](auto index, auto worker) {
            points[index] = self.camera(index, self.statistics.iterations, local[worker], token);
        },
        [&](auto n, auto total) { report(SppmPhase::camera, n, total); });
    std::vector<SppmGridPoint> grid_points;
    grid_points.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if ((i & 255U) == 0) {
            report(SppmPhase::grid, i, count);
        }
        if (points[i].surface) {
            grid_points.push_back({points[i].surface->hit.position, self.pixels[i].radius, i});
        }
    }
    const SppmGrid grid(grid_points, token);
    grid_points.clear();
    grid_points.shrink_to_fit();
    std::vector<std::vector<PhotonAccum>> accum(workers);
    for (auto& values : accum) {
        values.resize(count);
    }
    const auto photons = self.settings.sppm.photons_per_iteration;
    report(SppmPhase::photons, 0, photons);
    if (!grid.empty()) {
        parallel(
            photons, workers, token,
            [&](auto index, auto worker) {
                self.photon(index, self.statistics.iterations, grid, points, accum[worker],
                            local[worker], token);
            },
            [&](auto n, auto total) { report(SppmPhase::photons, n, total); });
    }
    auto pixels = self.pixels;
    auto stats = self.statistics;
    stats.minimum_radius = std::numeric_limits<double>::infinity();
    stats.maximum_radius = stats.mean_radius = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if ((i & 255U) == 0) {
            report(SppmPhase::update, i, count);
        }
        PhotonAccum combined;
        for (const auto& values : accum) {
            combined.phi += values[i].phi;
            combined.count += values[i].count;
        }
        pixels[i] = update_sppm_pixel(pixels[i], combined.count, points[i].beta * combined.phi,
                                      points[i].direct, self.settings.sppm.alpha);
        stats.minimum_radius = std::min(stats.minimum_radius, pixels[i].radius);
        stats.maximum_radius = std::max(stats.maximum_radius, pixels[i].radius);
        stats.mean_radius += pixels[i].radius / double(count);
    }
    ++stats.iterations;
    stats.emitted_photons += photons;
    stats.camera_paths += count;
    for (const auto& value : local) {
        stats.photon_hits += value.photon_hits;
        stats.gathered_contributions += value.gathered_contributions;
        stats.specular_light_contributions += value.specular_light_contributions;
        stats.sds_contributions += value.sds_contributions;
        stats.camera_scattering_events += value.camera_scattering_events;
        stats.photon_scattering_events += value.photon_scattering_events;
        stats.shadow_rays += value.shadow_rays;
        stats.geometry.bounds_tests += value.geometry.bounds_tests;
        stats.geometry.triangle_tests += value.geometry.triangle_tests;
    }
    report(SppmPhase::update, count, count);
    token.check();
    self.pixels = std::move(pixels);
    self.statistics = stats;
}
Image SppmIntegrator::image(CancellationToken token) const {
    const auto& self = *impl_;
    Image image(self.film.width, self.film.height);
    if (self.statistics.iterations == 0) {
        return image;
    }
    for (std::size_t i = 0; i < self.pixels.size(); ++i) {
        if ((i & 255U) == 0) {
            token.check();
        }
        const auto& pixel = self.pixels[i];
        const auto value = pixel.direct_sum / double(self.statistics.iterations) +
                           pixel.tau / (double(self.statistics.emitted_photons) * std::numbers::pi *
                                        pixel.radius * pixel.radius);
        check(value);
        if (maximum(value) > std::numeric_limits<float>::max()) {
            throw std::overflow_error("SPPM radiance exceeds the float image range");
        }
        image.at(i % self.film.width, i / self.film.width) = glm::vec3(value);
    }
    return image;
}
} // namespace minirender
