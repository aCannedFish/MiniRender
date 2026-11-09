#include "adaptive_surface.hpp"
#include "height_field.hpp"
#include <minirender/geometry/normals.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <queue>
#include <set>
#include <stdexcept>

namespace minirender {
namespace {

using detail::AdaptiveCorner;
using detail::AdaptiveFace;
using detail::AdaptiveSurface;
constexpr std::size_t absent = std::numeric_limits<std::size_t>::max();

// 面键用于匹配虚拟均匀拓扑，实际输出保留 AdaptiveFace 中的有向角点次序。
struct FaceKey {
    std::array<MeshIndex, 3> vertices;
    std::uint32_t level;
    bool operator==(const FaceKey&) const = default;
};
struct FaceHash {
    std::size_t operator()(const FaceKey& key) const noexcept {
        std::size_t result = key.level;
        for (const auto vertex : key.vertices) {
            result ^=
                std::hash<MeshIndex>{}(vertex) + 0x9e3779b9U + (result << 6U) + (result >> 2U);
        }
        return result;
    }
};
FaceKey face_key(std::array<MeshIndex, 3> vertices, std::uint32_t level) {
    std::sort(vertices.begin(), vertices.end());
    return {vertices, level};
}
FaceKey face_key(const AdaptiveFace& face) {
    return face_key({face.corners[0].point, face.corners[1].point, face.corners[2].point},
                    face.level);
}

// red 结点始终对应虚拟均匀三角形，过渡三角形仅在最终提取时创建。
struct Node {
    AdaptiveFace face;
    std::array<std::size_t, 4> children{absent, absent, absent, absent};
};
struct Candidate {
    double error;
    std::size_t node;
    bool operator<(const Candidate& other) const {
        return error == other.error ? node > other.node : error < other.error;
    }
};
struct Terminal {
    AdaptiveFace face;
    std::size_t owner;
    bool transition;
};

// 误差队列驱动 red 分裂，邻接闭合维持 1-irregular，再提取共享中点的终端模板。
class Refiner final {
  public:
    Refiner(std::shared_ptr<const MeshData> input, const HalfedgeMesh& topology,
            std::shared_ptr<const Texture> height, AdaptiveSettings settings,
            CancellationToken cancellation, AdaptiveProgressCallback progress,
            AdaptiveResult& result)
        : input_(std::move(input)), settings_(std::move(settings)), cancellation_(cancellation),
          progress_(std::move(progress)), result_(result),
          surface_(input_, topology, std::move(height), settings_, cancellation) {
        for (MeshIndex root = 0; root < input_->triangles.size(); ++root) {
            add_node(surface_.root_face(root));
        }
        leaves_ = nodes_.size();
    }

    std::shared_ptr<const MeshData> run() {
        report(AdaptiveStage::sampling, 0, nodes_.size());
        const auto roots = nodes_.size();
        for (std::size_t i = 0; i < roots; ++i) {
            evaluate(i);
            report(AdaptiveStage::sampling, i + 1, roots);
        }
        std::vector<Terminal> terminal;
        for (;;) {
            while (!pending_.empty()) {
                cancellation_.check();
                const auto candidate = pending_.top();
                pending_.pop();
                if (!leaf(candidate.node)) {
                    continue;
                }
                refine_balanced(candidate.node);
                report(AdaptiveStage::refinement, result_.red_refinements,
                       result_.red_refinements + pending_.size());
            }
            terminal.clear();
            std::set<std::size_t> refine_again;
            result_.maximum_sampled_error = 0;
            result_.maximum_level_reached = false;
            report(AdaptiveStage::transitions, 0, leaves_);
            std::size_t completed = 0;
            for (std::size_t i = 0; i < nodes_.size(); ++i) {
                if (!leaf(i)) {
                    continue;
                }
                auto triangles = transitions(i);
                for (const auto& triangle : triangles) {
                    const auto error = measure(triangle.face);
                    result_.maximum_sampled_error = std::max(result_.maximum_sampled_error, error);
                    if (error > settings_.tolerance) {
                        if (nodes_[i].face.level < settings_.maximum_level) {
                            refine_again.insert(i);
                        } else {
                            result_.maximum_level_reached = true;
                        }
                    }
                }
                terminal.insert(terminal.end(), triangles.begin(), triangles.end());
                if (terminal.size() > settings_.maximum_triangles) {
                    throw std::length_error("Adaptive transitions exceed the triangle budget");
                }
                report(AdaptiveStage::transitions, ++completed, leaves_);
            }
            if (refine_again.empty()) {
                break;
            }
            for (const auto node : refine_again) {
                pending_.push({std::numeric_limits<double>::infinity(), node});
            }
        }
        return publish(terminal);
    }

  private:
    void report(AdaptiveStage stage, std::size_t completed, std::size_t total) {
        cancellation_.check();
        if (progress_) {
            progress_({stage, completed, total, leaves_, surface_.hierarchy().point_count()});
        }
        cancellation_.check();
    }
    bool leaf(std::size_t index) const {
        return nodes_[index].children[0] == absent;
    }
    std::size_t add_node(const AdaptiveFace& face) {
        const auto index = nodes_.size();
        nodes_.push_back({face});
        if (!lookup_.emplace(face_key(face), index).second) {
            throw std::logic_error("Adaptive hierarchy created a duplicate uniform face");
        }
        return index;
    }
    double distance(glm::dvec3 exact, glm::dvec3 approximation) const {
        if (settings_.metric == AdaptiveMetric::local_distance) {
            return glm::length(exact - approximation);
        }
        if (glm::length(exact - approximation) <= 1e-12 * (1 + glm::length(exact))) {
            return 0;
        }
        const auto& projection = *settings_.projection;
        const auto a = projection.clip_from_object * glm::dvec4(exact, 1);
        const auto b = projection.clip_from_object * glm::dvec4(approximation, 1);
        if (a.w <= 1e-12 || b.w <= 1e-12) {
            return std::numeric_limits<double>::infinity();
        }
        return glm::length((glm::dvec2(a) / a.w - glm::dvec2(b) / b.w) *
                           projection.viewport_pixels * 0.5);
    }
    double measure(const AdaptiveFace& face) {
        std::array<glm::dvec3, 3> positions;
        for (std::size_t i = 0; i < 3; ++i) {
            positions[i] = surface_.position(face.corners[i], face.root);
        }
        double maximum = 0;
        const auto query = [&](glm::dvec3 weights) {
            const auto parameter = weights.x * face.corners[0].barycentric +
                                   weights.y * face.corners[1].barycentric +
                                   weights.z * face.corners[2].barycentric;
            const auto target = surface_.sample(face.root, parameter);
            const auto linear =
                weights.x * positions[0] + weights.y * positions[1] + weights.z * positions[2];
            maximum = std::max(maximum, distance(target.position, linear));
        };
        // 边中点、四分点和面内点共同检查弯曲，纹理特征另由 texel 检查补充。
        for (const auto weights :
             {glm::dvec3(0.5, 0.5, 0), glm::dvec3(0, 0.5, 0.5), glm::dvec3(0.5, 0, 0.5),
              glm::dvec3(0.25, 0.75, 0), glm::dvec3(0.75, 0.25, 0), glm::dvec3(0, 0.25, 0.75),
              glm::dvec3(0, 0.75, 0.25), glm::dvec3(0.25, 0, 0.75), glm::dvec3(0.75, 0, 0.25),
              glm::dvec3(1.0 / 3), glm::dvec3(0.6, 0.2, 0.2), glm::dvec3(0.2, 0.6, 0.2),
              glm::dvec3(0.2, 0.2, 0.6)}) {
            query(weights);
        }
        // visitor 返回根参数，转换回当前终端三角形的仿射权重。
        const auto p0 = face.corners[0].barycentric;
        const auto e1 = face.corners[1].barycentric - p0;
        const auto e2 = face.corners[2].barycentric - p0;
        const double d11 = glm::dot(e1, e1);
        const double d12 = glm::dot(e1, e2);
        const double d22 = glm::dot(e2, e2);
        const double determinant = d11 * d22 - d12 * d12;
        surface_.visit_height_features(
            face, settings_.metric == AdaptiveMetric::local_distance ? settings_.tolerance : 0,
            [&](glm::dvec3 parameter) {
                const auto r = parameter - p0;
                const double a = (glm::dot(r, e1) * d22 - glm::dot(r, e2) * d12) / determinant;
                const double b = (glm::dot(r, e2) * d11 - glm::dot(r, e1) * d12) / determinant;
                query({1 - a - b, a, b});
            });
        const auto center =
            surface_.sample(face.root, (face.corners[0].barycentric + face.corners[1].barycentric +
                                        face.corners[2].barycentric) /
                                           3.0);
        if (glm::dot(glm::cross(positions[1] - positions[0], positions[2] - positions[0]),
                     center.normal) <= 0) {
            maximum = std::numeric_limits<double>::infinity();
        }
        return maximum;
    }
    void evaluate(std::size_t index) {
        if (nodes_[index].face.level == settings_.maximum_level) {
            return;
        }
        const auto error = nodes_[index].face.level < settings_.minimum_level
                               ? std::numeric_limits<double>::infinity()
                               : measure(nodes_[index].face);
        if (error > settings_.tolerance) {
            pending_.push({error, index});
        }
    }
    // 逆向归并虚拟子面的父点集合，在已生成的 red 树中查找覆盖邻域的结点。
    std::size_t covering(FaceKey key) {
        auto& hierarchy = surface_.hierarchy();
        for (;;) {
            const auto found = lookup_.find(key);
            if (found != lookup_.end()) {
                return found->second;
            }
            if (key.level == 0) {
                throw std::logic_error("Adaptive neighbor has no base face");
            }
            std::vector<MeshIndex> parents;
            for (const auto vertex : key.vertices) {
                if (hierarchy.birth_level(vertex) < key.level) {
                    parents.push_back(vertex);
                } else {
                    const auto edge = hierarchy.parent_edge(vertex);
                    parents.insert(parents.end(), edge.begin(), edge.end());
                }
            }
            std::sort(parents.begin(), parents.end());
            parents.erase(std::unique(parents.begin(), parents.end()), parents.end());
            if (parents.size() != 3) {
                throw std::logic_error("Adaptive virtual face has an invalid parent");
            }
            key = face_key({parents[0], parents[1], parents[2]}, key.level - 1);
        }
    }
    std::size_t neighbor(const AdaptiveFace& face, std::size_t edge) {
        const auto a = face.corners[edge].point;
        const auto b = face.corners[(edge + 1) % 3].point;
        const auto opposite = surface_.hierarchy().opposites(a, b, face.level);
        if (opposite[0] != face.corners[(edge + 2) % 3].point) {
            throw std::logic_error("Adaptive face winding differs from virtual Loop topology");
        }
        return opposite[1] == invalid_mesh_index
                   ? absent
                   : covering(face_key({b, a, opposite[1]}, face.level));
    }
    void split(std::size_t index, bool balancing, std::deque<std::size_t>& checks) {
        if (!leaf(index)) {
            return;
        }
        if (nodes_[index].face.level >= settings_.maximum_level) {
            throw std::logic_error("Adaptive balancing exceeded the maximum level");
        }
        if (settings_.maximum_triangles - leaves_ < 3) {
            throw std::length_error("Adaptive refinement exceeds the triangle budget");
        }
        const auto children = surface_.children(nodes_[index].face);
        for (std::size_t i = 0; i < 4; ++i) {
            const auto child = add_node(children[i]);
            nodes_[index].children[i] = child;
            checks.push_back(child);
            evaluate(child);
        }
        leaves_ += 3;
        ++result_.red_refinements;
        result_.balancing_refinements += balancing ? 1 : 0;
    }
    void refine_balanced(std::size_t index) {
        std::deque<std::size_t> checks;
        split(index, false, checks);
        std::size_t completed = 0;
        while (!checks.empty()) {
            cancellation_.check();
            const auto current = checks.front();
            checks.pop_front();
            if (!leaf(current)) {
                continue;
            }
            for (std::size_t edge = 0; edge < 3; ++edge) {
                const auto other = neighbor(nodes_[current].face, edge);
                if (other != absent && nodes_[other].face.level + 1 < nodes_[current].face.level) {
                    split(other, true, checks);
                }
            }
            ++completed;
            report(AdaptiveStage::balance, completed, completed + checks.size());
        }
    }
    std::vector<Terminal> transitions(std::size_t owner) {
        const auto face = nodes_[owner].face;
        std::array<bool, 3> divided{};
        std::size_t count = 0;
        for (std::size_t edge = 0; edge < 3; ++edge) {
            const auto other = neighbor(face, edge);
            divided[edge] = other != absent && !leaf(other);
            count += divided[edge] ? 1 : 0;
        }
        if (count == 0) {
            return {{face, owner, false}};
        }
        const auto children = surface_.children(face);
        const std::array<AdaptiveCorner, 3> middles{children[0].corners[1], children[1].corners[1],
                                                    children[2].corners[1]};
        std::vector<Terminal> result;
        const auto add = [&](std::array<AdaptiveCorner, 3> c, bool transition = true) {
            result.push_back({{c, face.root, face.level}, owner, transition});
        };
        if (count == 3) {
            for (const auto& child : children) {
                result.push_back({child, owner, false});
            }
        } else if (count == 1) {
            const auto e = static_cast<std::size_t>(
                std::find(divided.begin(), divided.end(), true) - divided.begin());
            add({face.corners[e], middles[e], face.corners[(e + 2) % 3]});
            add({middles[e], face.corners[(e + 1) % 3], face.corners[(e + 2) % 3]});
        } else {
            const auto e = static_cast<std::size_t>(
                std::find(divided.begin(), divided.end(), false) - divided.begin());
            const auto a = face.corners[e];
            const auto b = face.corners[(e + 1) % 3];
            const auto c = face.corners[(e + 2) % 3];
            const auto bc = middles[(e + 1) % 3];
            const auto ca = middles[(e + 2) % 3];
            add({c, ca, bc});
            const auto diagonal1 =
                glm::length(surface_.position(a, face.root) - surface_.position(bc, face.root));
            const auto diagonal2 =
                glm::length(surface_.position(b, face.root) - surface_.position(ca, face.root));
            if (diagonal1 <= diagonal2) {
                add({a, b, bc});
                add({a, bc, ca});
            } else {
                add({a, b, ca});
                add({b, bc, ca});
            }
        }
        return result;
    }
    std::shared_ptr<const MeshData> publish(const std::vector<Terminal>& terminal) {
        auto mesh = std::make_shared<MeshData>();
        std::unordered_map<MeshIndex, MeshIndex> mapping;
        std::vector<bool> referenced(input_->positions.size(), false);
        for (const auto& face : input_->triangles) {
            for (const auto& c : face.corners) {
                referenced[c.position] = true;
            }
        }
        // 未被面引用的原位置单独保留，生成点只在真正输出时写入。
        for (std::size_t i = 0; i < referenced.size(); ++i) {
            if (!referenced[i]) {
                mesh->positions.push_back(input_->positions[i]);
            }
        }
        result_.triangle_levels.reserve(terminal.size());
        report(AdaptiveStage::output, 0, terminal.size());
        for (std::size_t i = 0; i < terminal.size(); ++i) {
            const auto& item = terminal[i];
            MeshTriangle triangle;
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const auto& c = item.face.corners[corner];
                const auto point = surface_.position(c, item.face.root);
                const auto [entry, inserted] =
                    mapping.emplace(c.point, static_cast<MeshIndex>(mesh->positions.size()));
                if (inserted) {
                    mesh->positions.emplace_back(point);
                }
                triangle.corners[corner] = {
                    entry->second, {0, 1, 0}, surface_.uv(item.face.root, c.barycentric)};
            }
            mesh->triangles.push_back(triangle);
            const auto level = nodes_[item.owner].face.level;
            result_.triangle_levels.push_back(static_cast<std::uint8_t>(level));
            ++result_.triangles_per_level[level];
            result_.deepest_level = std::max(result_.deepest_level, level);
            result_.transition_triangles += item.transition ? 1 : 0;
            report(AdaptiveStage::output, i + 1, terminal.size());
        }
        report(AdaptiveStage::normals, 0, 1);
        recompute_smooth_normals(*mesh, cancellation_, [&](auto done, auto total) {
            report(AdaptiveStage::normals, done, total);
        });
        result_.evaluation_points = surface_.hierarchy().point_count();
        result_.reference_queries = surface_.queries();
        result_.maximum_displacement = surface_.maximum_displacement();
        result_.maximum_seam_difference =
            std::max(result_.maximum_seam_difference, surface_.maximum_seam_difference());
        return mesh;
    }
    std::shared_ptr<const MeshData> input_;
    AdaptiveSettings settings_;
    CancellationToken cancellation_;
    AdaptiveProgressCallback progress_;
    AdaptiveResult& result_;
    AdaptiveSurface surface_;
    std::deque<Node> nodes_;
    std::unordered_map<FaceKey, std::size_t, FaceHash> lookup_;
    std::priority_queue<Candidate> pending_;
    std::size_t leaves_ = 0;
};

} // namespace

void AdaptiveProjection::validate() const {
    if (!std::isfinite(viewport_pixels.x) || !std::isfinite(viewport_pixels.y) ||
        viewport_pixels.x <= 0 || viewport_pixels.y <= 0) {
        throw std::invalid_argument("Adaptive projection requires a positive finite viewport");
    }
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(clip_from_object[column][row])) {
                throw std::invalid_argument("Adaptive projection matrix must be finite");
            }
        }
    }
}
void AdaptiveSettings::validate() const {
    if (maximum_level > 8 || minimum_level > maximum_level || !std::isfinite(tolerance) ||
        tolerance <= 0 || maximum_triangles == 0 || maximum_triangles > 16000000 ||
        maximum_evaluation_points == 0 || maximum_evaluation_points >= invalid_mesh_index) {
        throw std::invalid_argument(
            "Adaptive settings require levels 0..8, positive tolerance and valid budgets");
    }
    if (metric != AdaptiveMetric::local_distance && metric != AdaptiveMetric::screen_pixels) {
        throw std::invalid_argument("Unknown adaptive error metric");
    }
    if (metric == AdaptiveMetric::screen_pixels && !projection) {
        throw std::invalid_argument("Pixel adaptive error requires a fixed projection");
    }
    if (projection) {
        projection->validate();
    }
    if (displacement) {
        displacement->validate();
    }
}
AdaptiveResult subdivide_adaptive(std::shared_ptr<const MeshData> input,
                                  std::shared_ptr<const Texture> height, AdaptiveSettings settings,
                                  CancellationToken cancellation,
                                  AdaptiveProgressCallback progress) {
    cancellation.check();
    settings.validate();
    if (!input) {
        throw std::invalid_argument("Adaptive input is missing");
    }
    if (input->triangles.size() > settings.maximum_triangles) {
        throw std::length_error("Adaptive input exceeds the triangle budget");
    }
    AdaptiveResult result;
    const auto report = [&](AdaptiveStage stage, std::size_t done, std::size_t total) {
        cancellation.check();
        if (progress) {
            progress({stage, done, total,
                      result.mesh ? result.mesh->triangles.size() : input->triangles.size(),
                      result.evaluation_points});
        }
        cancellation.check();
    };
    result.metric = settings.metric;
    result.applied_tolerance = settings.tolerance;
    report(AdaptiveStage::topology, 0, 1);
    const HalfedgeMesh topology(*input, cancellation);
    result.input_topology = result.topology = topology.statistics();
    report(AdaptiveStage::topology, 1, 1);
    if (settings.displacement && settings.displacement->scale != 0) {
        if (!height) {
            throw std::invalid_argument("Adaptive displacement requires a height texture");
        }
        const auto samples = detail::inspect_height_field(
            *input, topology, height->level(0), *settings.displacement, cancellation,
            [&](auto p) { report(AdaptiveStage::seams, p.completed, p.total); });
        result.maximum_seam_difference = samples.maximum_seam_difference;
    }
    if (settings.maximum_level == 0 &&
        (!settings.displacement || settings.displacement->scale == 0)) {
        result.mesh = input;
        result.triangles_per_level[0] = input->triangles.size();
        result.triangle_levels.resize(input->triangles.size(), 0);
    } else {
        Refiner refiner(input, topology, std::move(height), settings, cancellation, progress,
                        result);
        result.mesh = refiner.run();
        const HalfedgeMesh output(*result.mesh, cancellation);
        result.topology = output.statistics();
        if (result.topology.euler_characteristic != result.input_topology.euler_characteristic ||
            result.topology.boundary_loops != result.input_topology.boundary_loops ||
            result.topology.face_components != result.input_topology.face_components ||
            result.topology.isolated_vertices != result.input_topology.isolated_vertices) {
            throw std::logic_error("Adaptive refinement changed surface topology");
        }
    }
    report(AdaptiveStage::complete, 1, 1);
    return result;
}

} // namespace minirender
