#include <minirender/geometry/halfedge.hpp>
#include <minirender/render/bsdf.hpp>
#include <minirender/render/subsurface.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <unordered_set>

namespace minirender {
namespace {
constexpr std::array axis_probability{.25, .25, .5};

void check_sample(double u) {
    if (!(u >= 0 && u < 1)) {
        throw std::invalid_argument("BSSRDF random coordinates must be in [0,1)");
    }
}
std::array<glm::dvec3, 3> axes(const SurfaceHit& from) {
    const ShadingFrame frame(glm::dvec3(from.geometric_normal));
    return {frame.to_world({1, 0, 0}), frame.to_world({0, 1, 0}), frame.normal()};
}

// 共享位置拓扑检查不受硬法线/UV 接缝影响。局部参考点减少体积求和中的平移误差。
void validate_closed(const MeshData& mesh, CancellationToken cancellation) {
    const HalfedgeMesh topology(mesh, cancellation);
    if (topology.statistics().boundary_edges != 0 || topology.statistics().face_components != 1) {
        throw std::invalid_argument("SSS requires one closed connected oriented mesh per object");
    }
    const auto origin = glm::dvec3(mesh.positions[mesh.triangles[0].corners[0].position]);
    double volume = 0;
    for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
        if ((i & 1023U) == 0) {
            cancellation.check();
        }
        const auto& corners = mesh.triangles[i].corners;
        const auto a = glm::dvec3(mesh.positions[corners[0].position]) - origin;
        const auto b = glm::dvec3(mesh.positions[corners[1].position]) - origin;
        const auto c = glm::dvec3(mesh.positions[corners[2].position]) - origin;
        volume += glm::dot(a, glm::cross(b, c)) / 6;
    }
    if (!std::isfinite(volume) || volume <= 0) {
        throw std::invalid_argument("SSS mesh must enclose positive volume with outward winding");
    }
}
} // namespace

struct SubsurfaceSampler::Instance {
    DipoleProfile profile;
    double radius;
    glm::dvec3 channel_mass;
    double mass;
    Instance(const Material& material, double bound)
        : profile(*material.subsurface, material.ior), radius(bound),
          channel_mass(profile.cumulative(radius)),
          mass(channel_mass.x + channel_mass.y + channel_mass.z) {}
};

SubsurfaceSampler::SubsurfaceSampler(const SceneBvh& bvh, CancellationToken cancellation)
    : bvh_(bvh) {
    std::unordered_set<const MeshData*> checked;
    const auto& source = bvh.scene().instances;
    for (std::size_t i = 0; i < source.size(); ++i) {
        cancellation.check();
        const auto& object = source[i];
        if (object.material.kind != MaterialKind::subsurface) {
            continue;
        }
        object.material.validate();
        if (checked.insert(object.mesh.get()).second) {
            validate_closed(*object.mesh, cancellation);
        }
        glm::dvec3 low(std::numeric_limits<double>::infinity()), high(-low);
        for (std::size_t p = 0; p < object.mesh->positions.size(); ++p) {
            if ((p & 1023U) == 0) {
                cancellation.check();
            }
            // 与 BVH 使用相同的 float 世界顶点，再转 double 计算距离和包围范围。
            const glm::dvec3 world(
                glm::vec3(object.object_to_world * glm::vec4(object.mesh->positions[p], 1)));
            low = glm::min(low, world);
            high = glm::max(high, world);
        }
        const double radius =
            std::nextafter(glm::length(high - low), std::numeric_limits<double>::infinity());
        if (!std::isfinite(radius) || radius <= 0) {
            throw std::invalid_argument("SSS object has invalid world bounds");
        }
        if (instances_.empty()) {
            instances_.resize(source.size());
        }
        instances_[i] = std::make_unique<const Instance>(object.material, radius);
    }
}
SubsurfaceSampler::~SubsurfaceSampler() = default;
const SubsurfaceSampler::Instance& SubsurfaceSampler::instance(std::uint32_t index) const {
    if (index >= instances_.size() || !instances_[index]) {
        throw std::invalid_argument("Object has no subsurface transport parameters");
    }
    return *instances_[index];
}
const DipoleProfile& SubsurfaceSampler::profile(std::uint32_t index) const {
    return instance(index).profile;
}
double SubsurfaceSampler::maximum_radius(std::uint32_t index) const {
    return instance(index).radius;
}

std::vector<SurfaceHit>
SubsurfaceSampler::intersections(const SurfaceHit& from, glm::dvec3 offset, glm::dvec3 axis,
                                 double radius, TraceStatistics* statistics,
                                 const CancellationToken& cancellation) const {
    const double distance = glm::length(offset);
    if (distance >= radius) {
        return {};
    }
    const double half_length = std::sqrt((radius - distance) * (radius + distance));
    const Ray ray{from.position + offset - half_length * axis, axis};
    RayInterval interval{0, 2 * half_length};
    std::vector<SurfaceHit> result;
    while (interval.minimum < interval.maximum) {
        cancellation.check();
        const auto hit =
            bvh_.intersect_instance(ray, from.instance_index, interval, statistics, cancellation);
        if (!hit) {
            break;
        }
        if (result.size() == maximum_intersections) {
            throw std::length_error("SSS probe exceeds the 4096 surface intersection budget");
        }
        result.push_back(*hit);
        // 按坐标运算误差向前推进，合并共享三角边的同位置命中；不使用世界空间固定偏移。
        const double error = glm::dot(glm::abs(axis), hit->position_error) +
                             8 * std::numeric_limits<double>::epsilon() *
                                 (std::abs(hit->distance) + glm::length(ray.origin));
        interval.minimum =
            std::nextafter(hit->distance + error, std::numeric_limits<double>::infinity());
    }
    return result;
}

double SubsurfaceSampler::mixture_pdf(const SurfaceHit& from, const SurfaceHit& to,
                                      int sampled_axis, std::size_t sampled_count,
                                      TraceStatistics* statistics,
                                      const CancellationToken& cancellation) const {
    cancellation.check();
    if (from.instance_index != to.instance_index) {
        return 0;
    }
    const auto& data = instance(from.instance_index);
    if (data.mass == 0) {
        return 0;
    }
    const auto basis = axes(from);
    const auto delta = to.position - from.position;
    const auto normal = glm::normalize(glm::dvec3(to.geometric_normal));
    double result = 0;
    for (int a = 0; a < 3; ++a) {
        const auto& axis = basis[std::size_t(a)];
        const double jacobian = std::abs(glm::dot(normal, axis));
        if (jacobian == 0) {
            continue;
        }
        const auto offset = delta - glm::dot(delta, axis) * axis;
        const double projected_radius = glm::length(offset);
        if (projected_radius >= data.radius) {
            continue;
        }
        const auto count =
            a == sampled_axis
                ? sampled_count
                : intersections(from, offset, axis, data.radius, statistics, cancellation).size();
        if (count == 0) {
            continue;
        }
        const auto value = data.profile.evaluate(projected_radius);
        const double disk_pdf = (value.x + value.y + value.z) / data.mass;
        result += axis_probability[std::size_t(a)] * disk_pdf * jacobian / double(count);
    }
    return result;
}
double SubsurfaceSampler::pdf(const SurfaceHit& from, const SurfaceHit& to,
                              TraceStatistics* statistics,
                              const CancellationToken& cancellation) const {
    return mixture_pdf(from, to, -1, 0, statistics, cancellation);
}

std::optional<SubsurfaceSample>
SubsurfaceSampler::sample(const SurfaceHit& from, double axis_u, double channel_u,
                          glm::dvec2 disk_u, double intersection_u, TraceStatistics* statistics,
                          const CancellationToken& cancellation) const {
    cancellation.check();
    for (const double u : {axis_u, channel_u, disk_u.x, disk_u.y, intersection_u}) {
        check_sample(u);
    }
    const auto& data = instance(from.instance_index);
    if (data.mass == 0) {
        return std::nullopt;
    }
    const int a = axis_u < .5 ? 2 : axis_u < .75 ? 0 : 1;
    int channel = 0;
    double selection = channel_u * data.mass;
    while (channel < 2 && selection >= data.channel_mass[channel]) {
        selection -= data.channel_mass[channel++];
    }
    const double radius = data.profile.sample_radius(channel, disk_u.x, data.radius);
    const double angle = 2 * std::numbers::pi * disk_u.y;
    const auto basis = axes(from);
    const auto offset = radius * (std::cos(angle) * basis[std::size_t((a + 1) % 3)] +
                                  std::sin(angle) * basis[std::size_t((a + 2) % 3)]);
    const auto hits =
        intersections(from, offset, basis[std::size_t(a)], data.radius, statistics, cancellation);
    if (hits.empty()) {
        return std::nullopt;
    }
    const auto selected =
        std::min(std::size_t(intersection_u * double(hits.size())), hits.size() - 1);
    const auto& hit = hits[selected];
    const double density = mixture_pdf(from, hit, a, hits.size(), statistics, cancellation);
    if (!(density > 0) || !std::isfinite(density)) {
        throw std::runtime_error("SSS sampled a surface with invalid area density");
    }
    return SubsurfaceSample{
        hit, data.profile.evaluate(glm::length(hit.position - from.position)) / density, density};
}

} // namespace minirender
