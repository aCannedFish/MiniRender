#include <minirender/geometry/sdf.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <set>
#include <stdexcept>

namespace minirender {
namespace {

bool finite(glm::vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
bool primitive(SdfKind kind) {
    return kind == SdfKind::sphere || kind == SdfKind::box || kind == SdfKind::torus;
}
void require(bool value, const char* message) {
    if (!value) {
        throw std::invalid_argument(message);
    }
}
glm::dmat4 object_to_world(const SdfNode& node) {
    auto matrix = glm::translate(glm::dmat4(1), glm::dvec3(node.translation));
    const auto angle = glm::radians(glm::dvec3(node.rotation_degrees));
    matrix = glm::rotate(matrix, angle.z, glm::dvec3(0, 0, 1));
    matrix = glm::rotate(matrix, angle.y, glm::dvec3(0, 1, 0));
    matrix = glm::rotate(matrix, angle.x, glm::dvec3(1, 0, 0));
    return glm::scale(matrix, glm::dvec3(node.scale));
}
SdfBounds primitive_bounds(const SdfNode& node, const glm::dmat4& matrix) {
    glm::dvec3 half(node.parameters.x);
    if (node.kind == SdfKind::box) {
        half = node.parameters;
    }
    if (node.kind == SdfKind::torus) {
        half = {node.parameters.x + static_cast<double>(node.parameters.y), node.parameters.y,
                node.parameters.x + static_cast<double>(node.parameters.y)};
    }
    const auto center = glm::dvec3(matrix[3]);
    glm::dvec3 extent(0);
    for (int column = 0; column < 3; ++column) {
        extent += glm::abs(glm::dvec3(matrix[column])) * half[column];
    }
    return {center - extent, center + extent, false};
}
SdfBounds combine_bounds(SdfKind kind, const SdfBounds& a, const SdfBounds& b) {
    if (kind == SdfKind::subtract) {
        return a;
    }
    if (kind == SdfKind::unite) {
        if (a.empty) {
            return b;
        }
        if (b.empty) {
            return a;
        }
        return {glm::min(a.minimum, b.minimum), glm::max(a.maximum, b.maximum), false};
    }
    const auto minimum = glm::max(a.minimum, b.minimum);
    const auto maximum = glm::min(a.maximum, b.maximum);
    return {minimum, maximum, a.empty || b.empty || glm::any(glm::greaterThan(minimum, maximum))};
}

} // namespace

void SdfTraceSettings::validate() const {
    require(std::isfinite(epsilon) && epsilon >= 1e-7 && epsilon <= .1,
            "SDF epsilon must be in 1e-7..0.1 meters");
    require(std::isfinite(relative_epsilon) && relative_epsilon >= 0 && relative_epsilon <= .01,
            "SDF relative epsilon must be in 0..0.01");
    require(std::isfinite(normal_step) && normal_step >= 1e-6 && normal_step <= .1,
            "SDF normal step must be in 1e-6..0.1 meters");
    require(std::isfinite(maximum_distance) && maximum_distance > 0 && maximum_distance <= 1e6,
            "SDF maximum distance must be positive and at most one million meters");
    require(maximum_steps > 0 && maximum_steps <= 4096 && shadow_steps > 0 && shadow_steps <= 1024,
            "SDF step budgets are outside supported ranges");
    require(std::isfinite(safety_factor) && safety_factor >= .1 && safety_factor <= 1,
            "SDF step safety factor must be in 0.1..1");
}

void SdfDocument::validate(CancellationToken cancellation) const {
    cancellation.check();
    require(!nodes.empty() && nodes.size() <= 64 && root < nodes.size(),
            "SDF graph requires 1..64 nodes and a valid root");
    camera.validate();
    require(finite(light.direction) &&
                glm::dot(glm::dvec3(light.direction), glm::dvec3(light.direction)) > 1e-12 &&
                finite(light.color) && glm::all(glm::greaterThanEqual(light.color, glm::vec3(0))) &&
                std::isfinite(light.intensity) && light.intensity >= 0,
            "SDF light requires a nonzero direction and nonnegative finite radiance");
    trace.validate();
    require(finite(ambient) && glm::all(glm::greaterThanEqual(ambient, glm::vec3(0))),
            "SDF ambient radiance must be finite and nonnegative");
    std::set<std::string> ids;
    for (const auto& node : nodes) {
        cancellation.check();
        require(!node.id.empty() && ids.insert(node.id).second,
                "SDF node IDs must be nonempty and unique");
        require(node.id.size() <= 128 && node.name.size() <= 512,
                "SDF node ID or name exceeds the length limit");
        require(finite(node.parameters) && finite(node.translation) &&
                    finite(node.rotation_degrees) && finite(node.color),
                "SDF node values must be finite");
        require(glm::all(glm::lessThanEqual(glm::abs(node.translation), glm::vec3(1e6F))),
                "SDF translations exceed the working range");
        require(glm::all(glm::greaterThanEqual(node.color, glm::vec3(0))) &&
                    glm::all(glm::lessThanEqual(node.color, glm::vec3(1))),
                "SDF albedo must be in 0..1");
        if (primitive(node.kind)) {
            require(std::isfinite(node.scale) && node.scale >= 1e-6F && node.scale <= 1e6F,
                    "SDF uniform scale must be positive and in 1e-6..1e6");
            require(node.parameters.x > 0 && node.parameters.x <= 1e6F,
                    "SDF dimensions must be positive and at most one million");
            if (node.kind == SdfKind::box) {
                require(node.parameters.y > 0 && node.parameters.z > 0 &&
                            node.parameters.y <= 1e6F && node.parameters.z <= 1e6F,
                        "SDF box half extents must be positive");
            }
            if (node.kind == SdfKind::torus) {
                require(node.parameters.y > 0 && node.parameters.y < node.parameters.x,
                        "SDF ring torus requires major radius greater than tube radius");
            }
            require(node.left == invalid_sdf_node && node.right == invalid_sdf_node,
                    "SDF primitives cannot have children");
        } else {
            require(node.kind == SdfKind::unite || node.kind == SdfKind::intersect ||
                        node.kind == SdfKind::subtract,
                    "Unknown SDF node kind");
            require(node.left < nodes.size() && node.right < nodes.size(),
                    "SDF operation has an invalid child");
            require(node.translation == glm::vec3(0) && node.rotation_degrees == glm::vec3(0) &&
                        node.scale == 1,
                    "SDF transforms belong to primitive nodes");
        }
    }
    std::vector<int> colors(nodes.size(), 0);
    const auto visit = [&](auto&& self, std::uint32_t index) -> void {
        cancellation.check();
        require(colors[index] != 1, "SDF graph contains a cycle");
        if (colors[index] == 2) {
            return;
        }
        colors[index] = 1;
        if (!primitive(nodes[index].kind)) {
            self(self, nodes[index].left);
            self(self, nodes[index].right);
        }
        colors[index] = 2;
    };
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        visit(visit, i);
    }
}

SdfField::SdfField(const SdfDocument& document, CancellationToken cancellation) {
    document.validate(cancellation);
    std::vector<std::uint32_t> mapping(document.nodes.size(), invalid_sdf_node);
    const auto compile = [&](auto&& self, std::uint32_t index) -> std::uint32_t {
        cancellation.check();
        if (mapping[index] != invalid_sdf_node) {
            return mapping[index];
        }
        const auto& source = document.nodes[index];
        CompiledSdfNode node;
        node.kind = source.kind;
        node.parameters = source.parameters;
        node.color = source.color;
        node.scale = source.scale;
        node.source_index = index;
        if (primitive(source.kind)) {
            const auto matrix = object_to_world(source);
            node.world_to_local = glm::inverse(matrix);
            node.bounds = primitive_bounds(source, matrix);
        } else {
            node.left = self(self, source.left);
            node.right = self(self, source.right);
            node.bounds =
                combine_bounds(source.kind, nodes_[node.left].bounds, nodes_[node.right].bounds);
        }
        mapping[index] = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back(node);
        return mapping[index];
    };
    root_ = compile(compile, document.root);
}

SdfSample SdfField::sample(glm::dvec3 point) const {
    require(std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z),
            "SDF sample position must be finite");
    std::array<SdfSample, 64> samples{};
    for (std::uint32_t i = 0; i < nodes_.size(); ++i) {
        const auto& node = nodes_[i];
        if (primitive(node.kind)) {
            const auto p = glm::dvec3(node.world_to_local * glm::dvec4(point, 1));
            double distance = 0;
            if (node.kind == SdfKind::sphere) {
                distance = glm::length(p) - node.parameters.x;
            } else if (node.kind == SdfKind::box) {
                const auto q = glm::abs(p) - node.parameters;
                distance = glm::length(glm::max(q, glm::dvec3(0))) +
                           std::min(std::max({q.x, q.y, q.z}), 0.0);
            } else {
                distance = glm::length(glm::dvec2(
                               glm::length(glm::dvec2(p.x, p.z)) - node.parameters.x, p.y)) -
                           node.parameters.y;
            }
            samples[i] = {distance * node.scale, i};
        } else {
            const auto a = samples[node.left];
            auto b = samples[node.right];
            if (node.kind == SdfKind::subtract) {
                b.distance = -b.distance;
            }
            samples[i] = node.kind == SdfKind::unite ? (a.distance <= b.distance ? a : b)
                                                     : (a.distance >= b.distance ? a : b);
        }
    }
    return samples[root_];
}

glm::dvec3 SdfField::normal(glm::dvec3 point, double step) const {
    require(std::isfinite(step) && step > 0, "SDF normal step must be positive and finite");
    glm::dvec3 gradient;
    for (int axis = 0; axis < 3; ++axis) {
        glm::dvec3 offset(0);
        offset[axis] = step;
        gradient[axis] = sample(point + offset).distance - sample(point - offset).distance;
    }
    const double length = glm::length(gradient);
    if (!std::isfinite(length) || length <= 1e-15 * step) {
        throw std::runtime_error("SDF surface has no stable normal at this sample");
    }
    return gradient / length;
}
const SdfBounds& SdfField::bounds() const noexcept {
    return nodes_[root_].bounds;
}
std::span<const CompiledSdfNode> SdfField::nodes() const noexcept {
    return nodes_;
}
std::uint32_t SdfField::root() const noexcept {
    return root_;
}

SdfTraceResult trace_sdf(const SdfField& field, const Ray& ray, SdfTraceSettings settings,
                         double minimum_distance, double maximum_distance,
                         CancellationToken cancellation) {
    cancellation.check();
    settings.validate();
    require(std::isfinite(ray.origin.x) && std::isfinite(ray.origin.y) &&
                std::isfinite(ray.origin.z) && std::isfinite(ray.direction.x) &&
                std::isfinite(ray.direction.y) && std::isfinite(ray.direction.z) &&
                std::abs(glm::length(ray.direction) - 1) < 1e-8,
            "SDF ray requires a finite origin and unit direction");
    require(std::isfinite(minimum_distance) && minimum_distance >= 0 &&
                !std::isnan(maximum_distance) && maximum_distance >= minimum_distance,
            "SDF ray interval is invalid");
    SdfTraceResult result;
    double first = minimum_distance;
    double last = std::min(maximum_distance, settings.maximum_distance);
    if (field.bounds().empty || first > last) {
        return result;
    }
    const double padding = 4 * (settings.epsilon + settings.relative_epsilon * last);
    for (int axis = 0; axis < 3; ++axis) {
        const double low = field.bounds().minimum[axis] - padding;
        const double high = field.bounds().maximum[axis] + padding;
        if (ray.direction[axis] == 0) {
            if (ray.origin[axis] < low || ray.origin[axis] > high) {
                return result;
            }
        } else {
            double a = (low - ray.origin[axis]) / ray.direction[axis];
            double b = (high - ray.origin[axis]) / ray.direction[axis];
            if (a > b) {
                std::swap(a, b);
            }
            first = std::max(first, a);
            last = std::min(last, b);
            if (first > last) {
                return result;
            }
        }
    }
    double t = first;
    for (std::uint32_t step = 0; step < settings.maximum_steps; ++step) {
        cancellation.check();
        result.position = ray.origin + ray.direction * t;
        const auto sample = field.sample(result.position);
        result.distance = t;
        result.residual = std::abs(sample.distance);
        result.material = sample.material;
        result.steps = step + 1;
        const double epsilon = settings.epsilon + settings.relative_epsilon * t;
        if (result.residual <= epsilon) {
            result.state = SdfTraceState::hit;
            return result;
        }
        const double next = t + result.residual * settings.safety_factor;
        if (next > last) {
            return result;
        }
        if (!(next > t)) {
            break;
        }
        t = next;
    }
    result.state = SdfTraceState::step_limit;
    return result;
}

std::string sdf_kind_name(SdfKind kind) {
    switch (kind) {
    case SdfKind::sphere:
        return "sphere";
    case SdfKind::box:
        return "box";
    case SdfKind::torus:
        return "torus";
    case SdfKind::unite:
        return "union";
    case SdfKind::intersect:
        return "intersection";
    case SdfKind::subtract:
        return "difference";
    }
    throw std::invalid_argument("Unknown SDF kind");
}
SdfKind parse_sdf_kind(const std::string& text) {
    for (const auto kind : {SdfKind::sphere, SdfKind::box, SdfKind::torus, SdfKind::unite,
                            SdfKind::intersect, SdfKind::subtract}) {
        if (sdf_kind_name(kind) == text) {
            return kind;
        }
    }
    throw std::invalid_argument("Unknown SDF kind: " + text);
}

SdfDocument make_sdf_example() {
    SdfDocument scene;
    scene.nodes = {
        {"sphere", "球", SdfKind::sphere, {.8F, 0, 0}, {-.7F, .85F, 0}, {}, 1, {.75F, .25F, .15F}},
        {"box",
         "切割盒",
         SdfKind::box,
         {.65F, .5F, .65F},
         {-.35F, 1.25F, .15F},
         {0, 25, 0},
         1,
         {.2F, .7F, .8F}},
        {"cut", "球减盒", SdfKind::subtract, {}, {}, {}, 1, {}, 0, 1},
        {"torus",
         "圆环",
         SdfKind::torus,
         {.65F, .18F, 0},
         {1, .9F, 0},
         {65, 0, 15},
         1,
         {.85F, .65F, .15F}},
        {"models", "模型并集", SdfKind::unite, {}, {}, {}, 1, {}, 2, 3},
        {"ground", "地面盒", SdfKind::box, {3, .05F, 3}, {0, -.05F, 0}, {}, 1, {.55F, .57F, .6F}},
        {"root", "场景", SdfKind::unite, {}, {}, {}, 1, {}, 4, 5}};
    scene.root = 6;
    scene.camera.position = {4, 2.8F, 4.8F};
    scene.camera.target = {0, .7F, 0};
    scene.light.direction = {-1, -2, -1};
    scene.light.intensity = 3;
    // 默认视角含贴近地面轮廓的射线，为掠射留出更大的迭代预算。
    scene.trace.maximum_steps = 512;
    scene.validate();
    return scene;
}

} // namespace minirender
