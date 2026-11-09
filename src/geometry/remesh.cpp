#include "remesh_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <tuple>

namespace minirender {
namespace {

using detail::RemeshEdge;
using detail::RemeshMesh;

// 最近点查询同时返回原三角面上插值的 UV。
struct ReferenceSample {
    glm::dvec3 position{0};
    glm::vec2 uv{0};
};

// 保护边将输入划分为查询片区，每片拥有紧凑的原始三角形与只读 AABB 树。
// 投影同时返回原面重心插值的 UV，避免切向移动后纹理留在旧顶点位置。
class ReferencePatches {
  public:
    ReferencePatches(const MeshData& input, const RemeshMesh& work, CancellationToken token) {
        const auto count = work.component_count();
        std::vector<std::vector<MeshIndex>> faces(count);
        for (MeshIndex i = 0; i < work.faces().size(); ++i) {
            if (i % 1024 == 0) {
                token.check();
            }
            faces[work.faces()[i].component].push_back(i);
        }
        std::vector<MeshIndex> mapping(input.positions.size(), invalid_mesh_index);
        for (MeshIndex patch = 0; patch < count; ++patch) {
            token.check();
            auto mesh = std::make_shared<MeshData>();
            mesh->triangles.reserve(faces[patch].size());
            std::vector<MeshIndex> touched;
            for (std::size_t i = 0; i < faces[patch].size(); ++i) {
                if (i % 1024 == 0) {
                    token.check();
                }
                auto triangle = input.triangles[faces[patch][i]];
                for (auto& corner : triangle.corners) {
                    const auto vertex = corner.position;
                    if (mapping[vertex] == invalid_mesh_index) {
                        mapping[vertex] = static_cast<MeshIndex>(mesh->positions.size());
                        mesh->positions.push_back(input.positions[vertex]);
                        touched.push_back(vertex);
                    }
                    corner.position = mapping[vertex];
                }
                mesh->triangles.push_back(triangle);
            }
            for (const auto vertex : touched) {
                mapping[vertex] = invalid_mesh_index;
            }
            meshes_.push_back(mesh);
            surfaces_.push_back(std::make_unique<MeshSurface>(mesh, token));
        }
    }
    ReferenceSample sample(MeshIndex patch, glm::dvec3 position, CancellationToken token) const {
        const auto hit = surfaces_.at(patch)->closest(position, token);
        const auto& c = meshes_[patch]->triangles[hit.triangle].corners;
        const auto uv = hit.barycentric.x * glm::dvec2(c[0].uv) +
                        hit.barycentric.y * glm::dvec2(c[1].uv) +
                        hit.barycentric.z * glm::dvec2(c[2].uv);
        return {hit.position, glm::vec2(uv)};
    }

  private:
    std::vector<std::shared_ptr<const MeshData>> meshes_;
    std::vector<std::unique_ptr<MeshSurface>> surfaces_;
};

MeshQuality measure_quality(const RemeshMesh& mesh, double length, CancellationToken token,
                            const SurfaceProgress& progress) {
    const auto edges = mesh.edges();
    const auto total = edges.size() + mesh.triangle_count();
    std::size_t completed = 0;
    const auto report = [&] {
        token.check();
        if (progress) {
            progress(completed, total);
        }
        token.check();
    };
    report();
    std::vector<double> lengths;
    lengths.reserve(edges.size());
    for (const auto edge : edges) {
        const auto [a, b] = detail::remesh_endpoints(edge);
        lengths.push_back(glm::length(mesh.vertices()[a].position - mesh.vertices()[b].position));
        if (++completed % 1024 == 0) {
            report();
        }
    }
    std::size_t comparisons = 0;
    std::sort(lengths.begin(), lengths.end(), [&](double a, double b) {
        if (++comparisons % 4096 == 0) {
            token.check();
        }
        return a < b;
    });
    const auto quantile = [&](double fraction) {
        const auto coordinate = fraction * static_cast<double>(lengths.size() - 1);
        const auto low = static_cast<std::size_t>(coordinate);
        const auto high = std::min(low + 1, lengths.size() - 1);
        return lengths[low] +
               (coordinate - static_cast<double>(low)) * (lengths[high] - lengths[low]);
    };
    MeshQuality result;
    result.edges = edges.size();
    result.minimum_edge = lengths.front();
    result.maximum_edge = lengths.back();
    result.mean_edge =
        std::accumulate(lengths.begin(), lengths.end(), 0.0) / static_cast<double>(lengths.size());
    result.median_edge = quantile(0.5);
    result.edge_p10 = quantile(0.1);
    result.edge_p90 = quantile(0.9);
    result.in_target_range =
        static_cast<std::size_t>(std::count_if(lengths.begin(), lengths.end(), [&](double value) {
            return value >= length * 4 / 5 && value <= length * 4 / 3;
        }));
    result.minimum_angle = 180;
    for (const auto& face : mesh.faces()) {
        if (!face.alive) {
            continue;
        }
        for (std::size_t i = 0; i < 3; ++i) {
            const auto& c = face.triangle.corners;
            const auto p = mesh.vertices()[c[i].position].position;
            const auto a = mesh.vertices()[c[(i + 1) % 3].position].position - p;
            const auto b = mesh.vertices()[c[(i + 2) % 3].position].position - p;
            const auto cosine =
                std::clamp(glm::dot(a, b) / (glm::length(a) * glm::length(b)), -1.0, 1.0);
            const auto angle = std::acos(cosine) * 180 / std::numbers::pi;
            result.minimum_angle = std::min(result.minimum_angle, angle);
            result.mean_angle_deviation += std::abs(angle - 60);
        }
        if (++completed % 1024 == 0) {
            report();
        }
    }
    result.mean_angle_deviation /= 3 * static_cast<double>(mesh.triangle_count());
    report();
    return result;
}

// 候选保存端点版本，局部修改后据此丢弃失效的队列项。
struct EdgeCandidate {
    RemeshEdge edge = 0;
    double length = 0;
    std::uint64_t a_version = 0;
    std::uint64_t b_version = 0;
};
// 长度优先；等长时按边编号排序，保持重复运行的结果一致。
struct LongestFirst {
    bool operator()(const EdgeCandidate& a, const EdgeCandidate& b) const {
        return a.length == b.length ? a.edge > b.edge : a.length < b.length;
    }
};
// 短边队列先处理最短边，等长时沿用相同的编号顺序。
struct ShortestFirst {
    bool operator()(const EdgeCandidate& a, const EdgeCandidate& b) const {
        return a.length == b.length ? a.edge > b.edge : a.length > b.length;
    }
};

// 顺序执行局部修改和 Jacobi 平滑；输入参照表面始终固定。
// 每轮清理失效面/顶点，限制工作集随历史迭代次数增长。
class Remesher {
  public:
    Remesher(std::unique_ptr<RemeshMesh> work, RemeshSettings settings, CancellationToken token,
             RemeshProgressCallback progress, RemeshResult& result)
        : mesh_(std::move(work)), settings_(settings), token_(token),
          progress_(std::move(progress)), result_(result) {
        result_.protected_vertices = static_cast<std::size_t>(
            std::count_if(mesh_->vertices().begin(), mesh_->vertices().end(),
                          [](const auto& vertex) { return vertex.fixed; }));
    }

    void prepare_reference(const MeshData& input) {
        if (settings_.project_to_surface) {
            report(RemeshStage::reference_surface, 0, 1);
            reference_ = std::make_unique<ReferencePatches>(input, *mesh_, token_);
            report(RemeshStage::reference_surface, 1, 1);
        }
    }
    void run() {
        for (iteration_ = 1; iteration_ <= settings_.iterations; ++iteration_) {
            const auto before =
                result_.splits + result_.collapses + result_.flips + result_.vertex_updates;
            edit_edges<LongestFirst>(true);
            edit_edges<ShortestFirst>(false);
            const auto edges = mesh_->edges();
            report(RemeshStage::flip, 0, edges.size());
            for (std::size_t i = 0; i < edges.size(); ++i) {
                const auto [a, b] = detail::remesh_endpoints(edges[i]);
                if (!mesh_->has_edge(a, b)) {
                    continue;
                }
                const auto edited = mesh_->flip(edges[i]);
                result_.flips += edited.applied ? 1 : 0;
                result_.rejected_flips += edited.applied ? 0 : 1;
                if ((i + 1) % 1024 == 0) {
                    report(RemeshStage::flip, i + 1, edges.size());
                }
            }
            report(RemeshStage::flip, edges.size(), edges.size());
            smooth();
            report(RemeshStage::output, 0, 1);
            mesh_->compact();
            report(RemeshStage::output, 1, 1);
            result_.iterations_completed = iteration_;
            if (before ==
                result_.splits + result_.collapses + result_.flips + result_.vertex_updates) {
                break;
            }
        }
        iteration_ = result_.iterations_completed;
        result_.protected_vertices = static_cast<std::size_t>(
            std::count_if(mesh_->vertices().begin(), mesh_->vertices().end(),
                          [](const auto& v) { return v.fixed; }));
    }
    MeshQuality quality() {
        return measure_quality(
            *mesh_, settings_.target_edge_length, token_,
            [&](auto done, auto total) { report(RemeshStage::quality, done, total); });
    }
    std::shared_ptr<const MeshData> publish() {
        report(RemeshStage::output, 0, 1);
        auto result = mesh_->publish(true);
        report(RemeshStage::output, 1, 1);
        return result;
    }

  private:
    void report(RemeshStage stage, std::size_t done, std::size_t total) {
        token_.check();
        if (progress_) {
            progress_({stage, iteration_, done, total, mesh_->triangle_count()});
        }
        token_.check();
    }
    bool valid(const EdgeCandidate& candidate) const {
        const auto [a, b] = detail::remesh_endpoints(candidate.edge);
        return mesh_->vertices()[a].alive && mesh_->vertices()[b].alive &&
               mesh_->vertices()[a].version == candidate.a_version &&
               mesh_->vertices()[b].version == candidate.b_version && mesh_->has_edge(a, b);
    }
    template <class Compare> void edit_edges(bool splitting) {
        std::priority_queue<EdgeCandidate, std::vector<EdgeCandidate>, Compare> pending;
        const double threshold = settings_.target_edge_length * (splitting ? 4.0 / 3 : 4.0 / 5);
        const auto enqueue = [&](RemeshEdge edge) {
            const auto [a, b] = detail::remesh_endpoints(edge);
            if (!mesh_->has_edge(a, b)) {
                return;
            }
            const auto& va = mesh_->vertices()[a];
            const auto& vb = mesh_->vertices()[b];
            const auto length = glm::length(va.position - vb.position);
            if (splitting ? length > threshold : length < threshold) {
                pending.push({edge, length, va.version, vb.version});
            }
        };
        const auto rebuild = [&] {
            pending = {};
            report(RemeshStage::edge_queue, 0, 1);
            const auto edges = mesh_->edges();
            report(RemeshStage::edge_queue, 0, edges.size());
            for (std::size_t i = 0; i < edges.size(); ++i) {
                enqueue(edges[i]);
                if ((i + 1) % 1024 == 0) {
                    report(RemeshStage::edge_queue, i + 1, edges.size());
                }
            }
            report(RemeshStage::edge_queue, edges.size(), edges.size());
        };
        rebuild();
        const auto stage = splitting ? RemeshStage::split : RemeshStage::collapse;
        std::size_t processed = 0;
        report(stage, 0, pending.size());
        while (!pending.empty()) {
            token_.check();
            const auto candidate = pending.top();
            pending.pop();
            ++processed;
            if (!valid(candidate)) {
                if (processed % 256 == 0) {
                    report(stage, processed, processed + pending.size());
                }
                continue;
            }
            const auto [a, b] = detail::remesh_endpoints(candidate.edge);
            const auto& va = mesh_->vertices()[a];
            const auto& vb = mesh_->vertices()[b];
            auto position = va.position + (vb.position - va.position) / 2.0;
            std::optional<glm::vec2> uv;
            const bool fixed =
                splitting ? mesh_->protected_edge(candidate.edge) : va.fixed || vb.fixed;
            if (reference_ && !fixed) {
                const auto faces = mesh_->edge_faces(a, b);
                const auto sample =
                    reference_->sample(mesh_->faces()[faces.front()].component, position, token_);
                position = sample.position;
                uv = sample.uv;
            }
            const auto edited = splitting
                                    ? mesh_->split(candidate.edge, position, uv)
                                    : mesh_->collapse(candidate.edge, position,
                                                      settings_.target_edge_length * 4 / 3, uv);
            if (splitting) {
                result_.splits += edited.applied ? 1 : 0;
                result_.rejected_splits += edited.applied ? 0 : 1;
            } else {
                result_.collapses += edited.applied ? 1 : 0;
                result_.rejected_collapses += edited.applied ? 0 : 1;
            }
            if (edited.applied) {
                std::vector<RemeshEdge> edges;
                std::size_t visited = 0;
                for (const auto vertex : edited.affected) {
                    if (++visited % 1024 == 0) {
                        token_.check();
                    }
                    if (!mesh_->vertices()[vertex].alive) {
                        continue;
                    }
                    for (const auto neighbor : mesh_->neighbors(vertex)) {
                        edges.push_back(detail::remesh_edge(vertex, neighbor));
                    }
                }
                std::size_t comparisons = 0;
                std::sort(edges.begin(), edges.end(), [&](auto left, auto right) {
                    if (++comparisons % 4096 == 0) {
                        token_.check();
                    }
                    return left < right;
                });
                edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
                for (const auto edge : edges) {
                    enqueue(edge);
                }
            }
            if (processed % 256 == 0) {
                report(stage, processed, processed + pending.size());
            }
            if (pending.size() > std::max<std::size_t>(4096, mesh_->triangle_count() * 12)) {
                rebuild();
            }
        }
        report(stage, processed, processed);
    }

    void smooth() {
        const auto vertices = mesh_->vertices();
        std::vector<glm::dvec3> directions(vertices.size(), glm::dvec3(0));
        std::vector<glm::dvec2> uv_directions(vertices.size(), glm::dvec2(0));
        report(RemeshStage::smooth, 0, vertices.size());
        for (MeshIndex i = 0; i < vertices.size(); ++i) {
            const auto& vertex = vertices[i];
            if (vertex.alive && !vertex.fixed && !vertex.faces.empty()) {
                const auto neighbors = mesh_->neighbors(i);
                glm::dvec3 relative(0);
                glm::dvec2 uv_relative(0);
                for (const auto neighbor : neighbors) {
                    relative += vertices[neighbor].position - vertex.position;
                    uv_relative += glm::dvec2(vertices[neighbor].uv) - glm::dvec2(vertex.uv);
                }
                const auto average = relative / static_cast<double>(neighbors.size());
                const auto normal = mesh_->vertex_normal(i);
                directions[i] = average - normal * glm::dot(normal, average);
                uv_directions[i] = uv_relative / static_cast<double>(neighbors.size());
            }
            if ((i + 1) % 1024 == 0) {
                report(RemeshStage::smooth, i + 1, vertices.size());
            }
        }
        report(RemeshStage::smooth, vertices.size(), vertices.size());
        std::vector<glm::dvec3> positions(vertices.size());
        std::vector<glm::vec2> uv_values(vertices.size());
        std::size_t proposed = 0;
        // 全局回退保持 Jacobi 同时更新的合法性；每次试探均重新投影到原表面。
        for (int trial = 0; trial < 8; ++trial) {
            const double step = std::ldexp(settings_.smoothing_step, -trial);
            const auto stage = reference_ ? RemeshStage::projection : RemeshStage::smooth;
            report(stage, 0, vertices.size());
            for (MeshIndex i = 0; i < vertices.size(); ++i) {
                const auto& vertex = vertices[i];
                positions[i] = vertex.position;
                uv_values[i] = vertex.uv;
                if (vertex.alive && !vertex.fixed && !vertex.faces.empty()) {
                    const auto candidate = vertex.position + step * directions[i];
                    if (reference_) {
                        const auto sample = reference_->sample(vertex.component, candidate, token_);
                        positions[i] = sample.position;
                        uv_values[i] = sample.uv;
                    } else {
                        positions[i] = candidate;
                        uv_values[i] = glm::vec2(glm::dvec2(vertex.uv) + step * uv_directions[i]);
                    }
                    positions[i] = glm::dvec3(glm::vec3(positions[i]));
                    if (trial == 0) {
                        proposed +=
                            positions[i] != vertex.position || uv_values[i] != vertex.uv ? 1 : 0;
                    }
                }
                if ((i + 1) % 1024 == 0) {
                    report(stage, i + 1, vertices.size());
                }
            }
            report(stage, vertices.size(), vertices.size());
            if (mesh_->valid_positions(positions, uv_values)) {
                result_.vertex_updates += mesh_->relocate(positions, uv_values);
                return;
            }
        }
        result_.rejected_moves += proposed;
    }

    std::unique_ptr<RemeshMesh> mesh_;
    RemeshSettings settings_;
    CancellationToken token_;
    RemeshProgressCallback progress_;
    RemeshResult& result_;
    std::unique_ptr<ReferencePatches> reference_;
    std::uint32_t iteration_ = 0;
};

} // namespace

void RemeshSettings::validate() const {
    if (!std::isfinite(target_edge_length) || target_edge_length <= 0 ||
        target_edge_length > std::numeric_limits<float>::max()) {
        throw std::invalid_argument(
            "Remeshing requires a positive finite edge length representable as float");
    }
    if (iterations > 20 || maximum_triangles == 0 || maximum_triangles > 16000000) {
        throw std::invalid_argument(
            "Remeshing requires iterations 0..20 and a triangle budget 1..16000000");
    }
    if (!std::isfinite(smoothing_step) || smoothing_step < 0 || smoothing_step > 1 ||
        !std::isfinite(crease_angle_degrees) || crease_angle_degrees <= 0 ||
        crease_angle_degrees > 180) {
        throw std::invalid_argument(
            "Remeshing requires smoothing step 0..1 and crease angle in (0, 180]");
    }
}

RemeshResult remesh_isotropic(std::shared_ptr<const MeshData> input, RemeshSettings settings,
                              CancellationToken cancellation, RemeshProgressCallback progress) {
    cancellation.check();
    settings.validate();
    if (!input) {
        throw std::invalid_argument("Remeshing input mesh is missing");
    }
    if (input->triangles.size() > settings.maximum_triangles) {
        throw std::length_error("Remeshing input exceeds the triangle budget");
    }
    const auto report = [&](RemeshStage stage, std::uint32_t iteration, std::size_t done,
                            std::size_t total, std::size_t faces) {
        cancellation.check();
        if (progress) {
            progress({stage, iteration, done, total, faces});
        }
        cancellation.check();
    };
    RemeshResult result;
    result.mesh = input;
    result.target_edge_length = settings.target_edge_length;
    report(RemeshStage::input_topology, 0, 0, 1, input->triangles.size());
    std::unique_ptr<RemeshMesh> work;
    {
        const HalfedgeMesh topology(*input, cancellation);
        result.input_topology = result.topology = topology.statistics();
        work = std::make_unique<RemeshMesh>(*input, topology, settings, cancellation);
    }
    report(RemeshStage::input_topology, 0, 1, 1, input->triangles.size());
    Remesher remesher(std::move(work), settings, cancellation, progress, result);
    result.before = remesher.quality();
    result.after = result.before;
    if (settings.iterations > 0) {
        remesher.prepare_reference(*input);
        remesher.run();
        result.after = remesher.quality();
        if (result.splits + result.collapses + result.flips + result.vertex_updates > 0) {
            result.mesh = remesher.publish();
            report(RemeshStage::output, result.iterations_completed, 0, 1,
                   result.mesh->triangles.size());
            const HalfedgeMesh output(*result.mesh, cancellation);
            result.topology = output.statistics();
            if (result.topology.euler_characteristic !=
                    result.input_topology.euler_characteristic ||
                result.topology.boundary_loops != result.input_topology.boundary_loops ||
                result.topology.face_components != result.input_topology.face_components ||
                result.topology.isolated_vertices != result.input_topology.isolated_vertices) {
                throw std::logic_error("Remeshing changed surface topology");
            }
            report(RemeshStage::output, result.iterations_completed, 1, 1,
                   result.mesh->triangles.size());
            report(RemeshStage::error, result.iterations_completed, 0, 1, result.topology.faces);
            result.error =
                compare_surfaces(input, result.mesh, cancellation, [&](auto done, auto total) {
                    report(RemeshStage::error, result.iterations_completed, done, total,
                           result.topology.faces);
                });
        }
    }
    report(RemeshStage::complete, result.iterations_completed, 1, 1, result.topology.faces);
    return result;
}

} // namespace minirender
