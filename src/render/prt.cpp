// 将单位漫反射材质的传输投影到九个 SH 基上，按共享位置和法线复用面角烘焙结果。
#include <bit>
#include <limits>
#include <map>
#include <minirender/render/path_tracer.hpp>
#include <minirender/render/prt.hpp>
#include <stdexcept>

namespace minirender {
namespace {
PrtTransfer vertex_transfer(const SceneBvh* bvh, const SceneSnapshot& scene,
                            const SurfaceHit& vertex, PrtSettings settings, std::size_t index,
                            CancellationToken token) {
    PrtTransfer result{};
    const auto  n = glm::dvec3(vertex.shading_normal);
    if (settings.mode == PrtMode::unoccluded) {
        // 归一化 Lambert 余弦核的三个频带因子为 1、2/3 和 1/4。
        const auto basis = sh_basis(n);
        for (std::size_t k = 0; k < 9; ++k) {
            result[k] = glm::vec3(float(basis[k] * (k == 0 ? 1. : k < 4 ? 2. / 3. : .25)));
        }
        return result;
    }
    Sampler random(settings.seed, index + 1);
    for (std::uint32_t i = 0; i < settings.samples; ++i) {
        if (i % 16 == 0) {
            token.check();
        }
        const auto l   = ShadingFrame(n).to_world(sample_cosine_hemisphere(random.next_2d()));
        auto       ray = spawn_ray(vertex, l);
        glm::vec3  weight(1);
        for (std::uint32_t depth = 0;; ++depth) {
            const auto hit = bvh->intersect(ray);
            if (!hit) {
                const auto basis = sh_basis(ray.direction);
                for (std::size_t k = 0; k < 9; ++k) {
                    result[k] += weight * float(basis[k] / double(settings.samples));
                }
                break;
            }
            if (settings.mode != PrtMode::interreflection || depth >= settings.bounces) {
                break;
            }
            const auto& instance = scene.instances[hit->instance_index];
            if (instance.material.kind != MaterialKind::diffuse) {
                break;
            }
            weight *= instance.material.base_color;
            if (instance.base_color_texture) {
                weight *= instance.base_color_texture->sample(hit->uv, 0,
                                                              instance.material.texture_filter);
            }
            glm::dvec3 normal = hit->shading_normal;
            if (glm::dot(normal, ray.direction) > 0) {
                normal = -normal;
            }
            ray = spawn_ray(
                *hit, ShadingFrame(normal).to_world(sample_cosine_hemisphere(random.next_2d())));
        }
    }
    return result;
}
} // namespace

PrtBake bake_prt(std::shared_ptr<const SceneSnapshot> scene, PrtSettings settings,
                 CancellationToken token, std::function<void(std::size_t, std::size_t)> progress) {
    if (!scene || !settings.samples || settings.samples > 65536 || settings.bounces > 8) {
        throw std::invalid_argument("Invalid PRT bake settings");
    }
    token.check();
    std::optional<SceneBvh> bvh;
    if (settings.mode != PrtMode::unoccluded) {
        bvh.emplace(scene, token);
    }
    std::size_t count = 0;
    for (const auto& instance : scene->instances) {
        count += instance.mesh->triangles.size() * 3;
    }
    if (count > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("PRT vertex count exceeds supported index range");
    }
    PrtBake bake;
    bake.vertices.reserve(count);
    for (const auto& instance : scene->instances) {
        bake.offsets.push_back(static_cast<std::uint32_t>(bake.vertices.size()));
        std::map<std::array<std::uint32_t, 4>, std::size_t> unique;
        for (const auto& tri : instance.mesh->triangles) {
            token.check();
            for (const auto& c : tri.corners) {
                const std::array key{c.position, std::bit_cast<std::uint32_t>(c.normal.x),
                                     std::bit_cast<std::uint32_t>(c.normal.y),
                                     std::bit_cast<std::uint32_t>(c.normal.z)};
                const auto [it, added] = unique.try_emplace(key, bake.vertices.size());
                if (added) {
                    SurfaceHit vertex;
                    vertex.position         = glm::dmat4(instance.object_to_world) *
                                              glm::dvec4(instance.mesh->positions[c.position], 1);
                    vertex.shading_normal   = glm::normalize(instance.normal_to_world * c.normal);
                    vertex.geometric_normal = vertex.shading_normal;
                    vertex.position_error =
                        glm::max(glm::abs(vertex.position) * 1e-6, glm::dvec3(1e-7));
                    bake.vertices.push_back(vertex_transfer(bvh ? &*bvh : nullptr, *scene, vertex,
                                                            settings, it->second, token));
                } else {
                    bake.vertices.push_back(bake.vertices[it->second]);
                }
            }
            if (progress && bake.vertices.size() % 192 == 0) {
                progress(bake.vertices.size(), count);
            }
        }
    }
    if (progress) {
        progress(count, count);
    }
    return bake;
}

bool compatible_prt(const SceneSnapshot& a, const SceneSnapshot& b, PrtMode mode) {
    if (a.instances.size() != b.instances.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.instances.size(); ++i) {
        const auto& x = a.instances[i];
        const auto& y = b.instances[i];
        if (x.mesh != y.mesh || x.object_to_world != y.object_to_world ||
            x.material.double_sided != y.material.double_sided) {
            return false;
        }
        if (mode == PrtMode::interreflection &&
            (x.material.base_color != y.material.base_color || x.material.kind != y.material.kind ||
             x.normal_texture != y.normal_texture ||
             x.material.normal_scale != y.material.normal_scale ||
             x.material.normal_flip_y != y.material.normal_flip_y ||
             x.base_color_texture != y.base_color_texture ||
             x.material.texture_filter != y.material.texture_filter)) {
            return false;
        }
    }
    return true;
}

std::uint64_t prt_cache_key(const SceneSnapshot& scene, PrtSettings settings,
                            CancellationToken token) {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto    word = [&](std::uint64_t value) {
        for (int j = 0; j < 8; ++j) {
            hash ^= value & 255U;
            hash *= 1099511628211ULL;
            value >>= 8U;
        }
    };
    const auto scalar = [&](float value) { word(std::bit_cast<std::uint32_t>(value)); };
    word(1);
    word(static_cast<unsigned>(settings.mode));
    word(settings.seed);
    word(settings.samples);
    word(settings.bounces);
    word(scene.instances.size());
    for (const auto& obj : scene.instances) {
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                scalar(obj.object_to_world[i][j]);
            }
        }
        word(obj.material.double_sided);
        word(obj.mesh->triangles.size());
        for (const auto& tri : obj.mesh->triangles) {
            token.check();
            for (const auto& c : tri.corners) {
                word(c.position);
                for (int j = 0; j < 3; ++j) {
                    scalar(obj.mesh->positions[c.position][j]);
                    scalar(c.normal[j]);
                }
                if (settings.mode == PrtMode::interreflection) {
                    scalar(c.uv.x);
                    scalar(c.uv.y);
                }
            }
        }
        if (settings.mode == PrtMode::interreflection) {
            word(static_cast<unsigned>(obj.material.kind));
            word(static_cast<unsigned>(obj.material.texture_filter));
            for (int j = 0; j < 3; ++j) {
                scalar(obj.material.base_color[j]);
            }
            scalar(obj.material.normal_scale);
            word(obj.material.normal_flip_y);
            for (const auto& texture : {obj.base_color_texture, obj.normal_texture}) {
                word(texture ? 1 : 0);
                if (texture) {
                    const auto& image = texture->level(0);
                    word(image.width());
                    word(image.height());
                    for (std::size_t y = 0; y < image.height(); ++y) {
                        token.check();
                        for (std::size_t x = 0; x < image.width(); ++x) {
                            for (int j = 0; j < 3; ++j) {
                                scalar(image.at(x, y)[j]);
                            }
                        }
                    }
                }
            }
        }
    }
    return hash;
}

ShLight scene_sh_light(const SceneSnapshot& scene) {
    auto light = constant_environment(scene.environment);
    if (scene.environment_texture) {
        const auto incoming =
            rotate_environment(project_environment(scene.environment_texture->level(0)),
                               glm::radians(double(scene.environment_rotation_degrees)));
        for (std::size_t k = 0; k < 9; ++k) {
            light[k] += incoming[k] * double(scene.environment_strength);
        }
    }
    return light;
}

glm::vec3 prt_irradiance(const PrtTransfer& transfer, const ShLight& light) {
    glm::dvec3 value(0);
    for (std::size_t k = 0; k < 9; ++k) {
        value += glm::dvec3(transfer[k]) * light[k];
    }
    return glm::vec3(glm::max(value, glm::dvec3(0)));
}
} // namespace minirender
