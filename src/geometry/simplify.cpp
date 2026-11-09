#include <minirender/geometry/normals.hpp>
#include <minirender/geometry/simplify.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <numbers>
#include <optional>
#include <queue>
#include <stdexcept>
#include <tuple>

namespace minirender {
namespace {

// 对称 4x4 二次型的上三角，共 10 个 double。所有位置先减去输入包围盒中心。
// 平移减少大坐标下展开距离平方的消去误差，输出仍按场景约定存为 float。
struct Quadric {
    std::array<double, 10> q{};
    void plane(glm::dvec4 p) {
        std::size_t k = 0;
        for (int row = 0; row < 4; ++row) {
            for (int column = row; column < 4; ++column) {
                q[k++] += p[row] * p[column];
            }
        }
    }
    Quadric operator+(const Quadric& other) const {
        auto result = *this;
        for (std::size_t i = 0; i < q.size(); ++i) {
            result.q[i] += other.q[i];
        }
        return result;
    }
    double cost(glm::dvec3 p) const {
        return std::max(0.0, p.x * (q[0] * p.x + 2 * q[1] * p.y + 2 * q[2] * p.z + 2 * q[3]) +
                                 p.y * (q[4] * p.y + 2 * q[5] * p.z + 2 * q[6]) +
                                 p.z * (q[7] * p.z + 2 * q[8]) + q[9]);
    }
    double directional_curvature(glm::dvec3 d) const {
        return d.x * (q[0] * d.x + 2 * q[1] * d.y + 2 * q[2] * d.z) +
               d.y * (q[4] * d.y + 2 * q[5] * d.z) + q[7] * d.z * d.z;
    }
    glm::dvec3 gradient_half(glm::dvec3 p) const {
        return {q[0] * p.x + q[1] * p.y + q[2] * p.z + q[3],
                q[1] * p.x + q[4] * p.y + q[5] * p.z + q[6],
                q[2] * p.x + q[5] * p.y + q[7] * p.z + q[8]};
    }
    // 带部分选主元的 3x3 高斯消元。奇异平面/直线情况由线段和端点候选处理。
    std::optional<glm::dvec3> minimum() const {
        std::array<std::array<double, 4>, 3> a{
            {{q[0], q[1], q[2], -q[3]}, {q[1], q[4], q[5], -q[6]}, {q[2], q[5], q[7], -q[8]}}};
        const auto scale = std::max({std::abs(q[0]), std::abs(q[4]), std::abs(q[7])});
        if (scale == 0) {
            return {};
        }
        for (std::size_t column = 0; column < 3; ++column) {
            auto pivot = column;
            for (auto row = column + 1; row < 3; ++row) {
                if (std::abs(a[row][column]) > std::abs(a[pivot][column])) {
                    pivot = row;
                }
            }
            if (std::abs(a[pivot][column]) <= scale * 1e-12) {
                return {};
            }
            std::swap(a[column], a[pivot]);
            for (auto row = column + 1; row < 3; ++row) {
                const auto factor = a[row][column] / a[column][column];
                for (auto j = column; j < 4; ++j) {
                    a[row][j] -= factor * a[column][j];
                }
            }
        }
        glm::dvec3 result(0);
        for (int row = 2; row >= 0; --row) {
            auto value = a[static_cast<std::size_t>(row)][3];
            for (int column = row + 1; column < 3; ++column) {
                value -= a[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)] *
                         result[column];
            }
            result[row] = value / a[static_cast<std::size_t>(row)][static_cast<std::size_t>(row)];
        }
        return result;
    }
};

struct Vertex {
    glm::dvec3 position{0};
    glm::vec2 uv{0};
    Quadric quadric;
    std::vector<MeshIndex> faces;
    std::uint64_t version = 0;
    bool alive = true;
    bool fixed = false;
    bool attribute_locked = false;
    bool normal_seam = false;
};
struct Face {
    MeshTriangle triangle;
    bool alive = true;
};
struct Candidate {
    MeshIndex a = 0, b = 0;
    std::uint64_t version_a = 0, version_b = 0;
    glm::dvec3 position{0};
    double cost = 0;
    double length_squared = 0;
};
struct MoreExpensive {
    bool operator()(const Candidate& a, const Candidate& b) const {
        return std::tie(a.cost, a.length_squared, a.a, a.b) >
               std::tie(b.cost, b.length_squared, b.a, b.b);
    }
};

template <class T> void unique(std::vector<T>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

bool finite(glm::dvec3 p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

// 动态工作网格仅属于调用线程。面编号稳定，折叠后清理局部邻接，不改动输入数组。
// 堆元素带端点版本；邻域改变后旧元素自动失效，局部重建同时恢复以前被约束的候选。
class Decimator {
  public:
    Decimator(const MeshData& input, const HalfedgeMesh& topology, SimplifySettings settings,
              CancellationToken token, SimplifyProgressCallback progress, SimplifyResult& result)
        : settings_(settings), token_(token), progress_(std::move(progress)), result_(result),
          remaining_(input.triangles.size()) {
        glm::dvec3 low(input.positions.front()), high(low);
        for (std::size_t i = 0; i < input.positions.size(); ++i) {
            if (i % 1024 == 0) {
                token_.check();
            }
            low = glm::min(low, glm::dvec3(input.positions[i]));
            high = glm::max(high, glm::dvec3(input.positions[i]));
        }
        origin_ = low + (high - low) / 2.0;
        vertices_.resize(input.positions.size());
        for (std::size_t i = 0; i < input.positions.size(); ++i) {
            vertices_[i].position = glm::dvec3(input.positions[i]) - origin_;
        }
        faces_.reserve(input.triangles.size());
        report(SimplifyStage::quadrics, 0, input.triangles.size());
        std::vector<bool> seen(vertices_.size(), false);
        std::vector<glm::dvec3> first_normal(vertices_.size());
        for (MeshIndex i = 0; i < input.triangles.size(); ++i) {
            faces_.push_back({input.triangles[i], true});
            const auto& corners = input.triangles[i].corners;
            const auto a = vertices_[corners[0].position].position;
            const auto normal = glm::normalize(face_normal(input.triangles[i]));
            const glm::dvec4 plane(normal, -glm::dot(normal, a));
            for (const auto& corner : corners) {
                auto& vertex = vertices_[corner.position];
                vertex.faces.push_back(i);
                vertex.quadric.plane(plane);
                const auto n = glm::normalize(glm::dvec3(corner.normal));
                if (seen[corner.position]) {
                    const bool uv_seam =
                        glm::length(glm::dvec2(corner.uv) - glm::dvec2(vertex.uv)) > 1e-6;
                    const bool normal_seam = glm::dot(n, first_normal[corner.position]) < 1 - 1e-6;
                    vertex.normal_seam = vertex.normal_seam || normal_seam;
                    vertex.attribute_locked = vertex.attribute_locked ||
                                              (settings.preserve_uv_seams && uv_seam) ||
                                              (settings.preserve_hard_normals && normal_seam);
                } else {
                    seen[corner.position] = true;
                    vertex.uv = corner.uv;
                    first_normal[corner.position] = n;
                }
            }
            if ((i + 1) % 1024 == 0) {
                report(SimplifyStage::quadrics, i + 1, input.triangles.size());
            }
        }
        const auto hs = topology.halfedges();
        const double cosine = std::cos(settings.crease_angle_degrees * std::numbers::pi / 180.0);
        for (std::size_t i = 0; i < topology.edges().size(); ++i) {
            if (i % 1024 == 0) {
                token_.check();
            }
            const auto h = topology.edges()[i].halfedge;
            const auto twin = hs[h].twin;
            bool fixed = settings.preserve_boundary && topology.is_boundary(twin);
            if (settings.preserve_creases && !topology.is_boundary(twin)) {
                const auto n1 = glm::normalize(face_normal(faces_[hs[h].face].triangle));
                const auto n2 = glm::normalize(face_normal(faces_[hs[twin].face].triangle));
                fixed = fixed || glm::dot(n1, n2) < cosine;
            }
            if (fixed) {
                vertices_[hs[h].origin].fixed = true;
                vertices_[hs[twin].origin].fixed = true;
            }
        }
        for (const auto& vertex : vertices_) {
            result_.protected_vertices += vertex.fixed || vertex.attribute_locked ? 1 : 0;
        }
        report(SimplifyStage::quadrics, input.triangles.size(), input.triangles.size());
    }

    void run() {
        rebuild_heap();
        report(SimplifyStage::collapse, 0, reduction_goal());
        while (remaining_ > settings_.target_triangles) {
            token_.check();
            if (heap_.empty()) {
                result_.stop = SimplifyStop::constrained;
                break;
            }
            const auto candidate = heap_.top();
            heap_.pop();
            if (!valid(candidate)) {
                continue;
            }
            if (candidate.cost > settings_.maximum_quadric_error) {
                result_.stop = SimplifyStop::error_limit;
                break;
            }
            collapse(candidate);
            ++result_.collapses;
            result_.maximum_quadric_error = std::max(result_.maximum_quadric_error, candidate.cost);
            if (result_.collapses % 128 == 0 || remaining_ <= settings_.target_triangles) {
                report(SimplifyStage::collapse,
                       std::min(reduction_goal(), result_.input_topology.faces - remaining_),
                       reduction_goal());
            }
            // 清除失效元素，限制堆内存随历史折叠次数增长。重建只在达到阈值时进行。
            if (heap_.size() > std::max<std::size_t>(4096, remaining_ * 12)) {
                rebuild_heap();
            }
        }
    }

    std::shared_ptr<const MeshData> output() {
        report(SimplifyStage::output, 0, vertices_.size() + faces_.size());
        auto mesh = std::make_shared<MeshData>();
        std::vector<MeshIndex> mapping(vertices_.size(), invalid_mesh_index);
        std::vector<bool> normal_seams;
        for (std::size_t i = 0; i < vertices_.size(); ++i) {
            if (vertices_[i].alive) {
                mapping[i] = static_cast<MeshIndex>(mesh->positions.size());
                mesh->positions.emplace_back(vertices_[i].position + origin_);
                normal_seams.push_back(settings_.preserve_hard_normals && vertices_[i].normal_seam);
            }
            if (i % 1024 == 0) {
                report(SimplifyStage::output, i, vertices_.size() + faces_.size());
            }
        }
        mesh->triangles.reserve(remaining_);
        for (std::size_t i = 0; i < faces_.size(); ++i) {
            if (faces_[i].alive) {
                auto face = faces_[i].triangle;
                for (auto& corner : face.corners) {
                    corner.position = mapping[corner.position];
                }
                mesh->triangles.push_back(face);
            }
            if (i % 1024 == 0) {
                report(SimplifyStage::output, vertices_.size() + i,
                       vertices_.size() + faces_.size());
            }
        }
        const auto normals = smooth_vertex_normals(*mesh, token_);
        for (std::size_t i = 0; i < mesh->triangles.size(); ++i) {
            if (i % 1024 == 0) {
                token_.check();
            }
            for (auto& corner : mesh->triangles[i].corners) {
                if (!normal_seams[corner.position]) {
                    corner.normal = normals[corner.position];
                }
            }
        }
        report(SimplifyStage::output, vertices_.size() + faces_.size(),
               vertices_.size() + faces_.size());
        return mesh;
    }

  private:
    std::size_t reduction_goal() const {
        return result_.input_topology.faces - settings_.target_triangles;
    }
    void report(SimplifyStage stage, std::size_t done, std::size_t total) {
        token_.check();
        if (progress_) {
            progress_({stage, done, total, remaining_});
        }
        token_.check();
    }
    bool contains(const Face& face, MeshIndex vertex) const {
        return face.alive && std::any_of(face.triangle.corners.begin(), face.triangle.corners.end(),
                                         [&](const auto& c) { return c.position == vertex; });
    }
    std::vector<MeshIndex> incident(MeshIndex vertex) const {
        std::vector<MeshIndex> result;
        std::size_t visited = 0;
        for (const auto face : vertices_[vertex].faces) {
            if (++visited % 1024 == 0) {
                token_.check();
            }
            if (contains(faces_[face], vertex)) {
                result.push_back(face);
            }
        }
        return result;
    }
    std::vector<MeshIndex> neighbors(MeshIndex vertex) const {
        std::vector<MeshIndex> result;
        for (const auto f : incident(vertex)) {
            for (const auto& c : faces_[f].triangle.corners) {
                if (c.position != vertex) {
                    result.push_back(c.position);
                }
            }
        }
        unique(result);
        return result;
    }
    glm::dvec3 face_normal(const MeshTriangle& face) const {
        const auto a = vertices_[face.corners[0].position].position;
        const auto b = vertices_[face.corners[1].position].position;
        const auto c = vertices_[face.corners[2].position].position;
        return glm::cross(b - a, c - a);
    }

    // 共同一环必须恰好等于边对面的顶点集合。另查替换后的重复面，排除四面体退化。
    // 两个边界顶点之间的内边不能折叠，否则会夹断边界。至少保留一个邻接三角形。
    bool topology_legal(MeshIndex a, MeshIndex b, std::vector<MeshIndex>& local) const {
        const auto na = neighbors(a), nb = neighbors(b);
        std::vector<MeshIndex> common, opposite;
        std::set_intersection(na.begin(), na.end(), nb.begin(), nb.end(),
                              std::back_inserter(common));
        const auto fa = incident(a), fb = incident(b);
        local = fa;
        local.insert(local.end(), fb.begin(), fb.end());
        unique(local);
        std::size_t shared_faces = 0;
        for (const auto face : fa) {
            if (contains(faces_[face], b)) {
                ++shared_faces;
                for (const auto& c : faces_[face].triangle.corners) {
                    if (c.position != a && c.position != b) {
                        opposite.push_back(c.position);
                    }
                }
            }
        }
        unique(opposite);
        if (shared_faces == 0 || shared_faces > 2 || common != opposite ||
            local.size() <= shared_faces) {
            return false;
        }
        const bool boundary_a = na.size() != fa.size(), boundary_b = nb.size() != fb.size();
        if (shared_faces == 2 && boundary_a && boundary_b) {
            return false;
        }
        std::vector<std::array<MeshIndex, 3>> keys;
        for (const auto i : local) {
            const auto& face = faces_[i];
            if (contains(face, a) && contains(face, b)) {
                continue;
            }
            std::array<MeshIndex, 3> key;
            for (std::size_t j = 0; j < 3; ++j) {
                const auto v = face.triangle.corners[j].position;
                key[j] = v == b ? a : v;
            }
            std::sort(key.begin(), key.end());
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());
        return std::adjacent_find(keys.begin(), keys.end()) == keys.end();
    }

    bool geometry_legal(MeshIndex a, MeshIndex b, glm::dvec3 position,
                        const std::vector<MeshIndex>& local) const {
        std::size_t visited = 0;
        for (const auto index : local) {
            if (++visited % 1024 == 0) {
                token_.check();
            }
            const auto& face = faces_[index];
            if (contains(face, a) && contains(face, b)) {
                continue;
            }
            std::array<glm::dvec3, 3> p;
            for (std::size_t j = 0; j < 3; ++j) {
                const auto vertex = face.triangle.corners[j].position;
                p[j] = vertex == a || vertex == b ? position : vertices_[vertex].position;
            }
            const auto normal = glm::cross(p[1] - p[0], p[2] - p[0]);
            if (glm::dot(normal, normal) <= 1e-20 ||
                glm::dot(normal, face_normal(face.triangle)) <= 0) {
                return false;
            }
        }
        return true;
    }

    void enqueue(MeshIndex a, MeshIndex b) {
        if (++result_.candidate_evaluations % 1024 == 0) {
            token_.check();
        }
        if (a > b) {
            std::swap(a, b);
        }
        if (!vertices_[a].alive || !vertices_[b].alive || vertices_[a].attribute_locked ||
            vertices_[b].attribute_locked || (vertices_[a].fixed && vertices_[b].fixed)) {
            return;
        }
        if (vertices_[b].fixed) {
            std::swap(a, b);
        }
        std::vector<MeshIndex> local;
        if (!topology_legal(a, b, local)) {
            return;
        }
        const auto& va = vertices_[a];
        const auto& vb = vertices_[b];
        const auto q = va.quadric + vb.quadric;
        std::vector<glm::dvec3> positions{va.position};
        if (!va.fixed) {
            positions.push_back(vb.position);
            positions.push_back((va.position + vb.position) / 2.0);
            if (const auto minimum = q.minimum()) {
                positions.push_back(*minimum);
            }
            const auto direction = vb.position - va.position;
            const auto curvature = q.directional_curvature(direction);
            if (curvature > 0) {
                const auto t = std::clamp(
                    -glm::dot(direction, q.gradient_half(va.position)) / curvature, 0.0, 1.0);
                positions.push_back(va.position + t * direction);
            }
        }
        std::optional<Candidate> best;
        for (auto position : positions) {
            // 合法性使用实际存储后的 float 位置，防止舍入后产生新的退化面。
            position = glm::dvec3(glm::vec3(position + origin_)) - origin_;
            if (!finite(position)) {
                continue;
            }
            const auto cost = q.cost(position);
            if (std::isfinite(cost) && (!best || cost < best->cost) &&
                geometry_legal(a, b, position, local)) {
                const auto edge = vb.position - va.position;
                best =
                    Candidate{a, b, va.version, vb.version, position, cost, glm::dot(edge, edge)};
            }
        }
        if (best) {
            heap_.push(*best);
        }
    }
    bool valid(const Candidate& c) const {
        return vertices_[c.a].alive && vertices_[c.b].alive &&
               vertices_[c.a].version == c.version_a && vertices_[c.b].version == c.version_b;
    }
    void rebuild_heap() {
        heap_ = {};
        std::vector<std::pair<MeshIndex, MeshIndex>> edges;
        edges.reserve(remaining_ * 3);
        for (std::size_t i = 0; i < faces_.size(); ++i) {
            if (i % 1024 == 0) {
                token_.check();
            }
            if (!faces_[i].alive) {
                continue;
            }
            const auto& c = faces_[i].triangle.corners;
            for (std::size_t j = 0; j < 3; ++j) {
                const auto a = c[j].position, b = c[(j + 1) % 3].position;
                edges.emplace_back(std::min(a, b), std::max(a, b));
            }
        }
        unique(edges);
        report(SimplifyStage::candidates, 0, edges.size());
        for (std::size_t i = 0; i < edges.size(); ++i) {
            enqueue(edges[i].first, edges[i].second);
            if ((i + 1) % 1024 == 0) {
                report(SimplifyStage::candidates, i + 1, edges.size());
            }
        }
        report(SimplifyStage::candidates, edges.size(), edges.size());
    }

    void collapse(const Candidate& c) {
        auto& a = vertices_[c.a];
        auto& b = vertices_[c.b];
        const auto direction = b.position - a.position;
        const auto length_squared = glm::dot(direction, direction);
        const auto t =
            length_squared > 0
                ? std::clamp(glm::dot(c.position - a.position, direction) / length_squared, 0.0,
                             1.0)
                : 0;
        const auto uv = glm::vec2(glm::dvec2(a.uv) + t * (glm::dvec2(b.uv) - glm::dvec2(a.uv)));
        auto local = incident(c.a);
        const auto other = incident(c.b);
        local.insert(local.end(), other.begin(), other.end());
        unique(local);
        auto affected = neighbors(c.a);
        const auto nb = neighbors(c.b);
        affected.insert(affected.end(), nb.begin(), nb.end());
        affected.push_back(c.a);
        affected.push_back(c.b);
        unique(affected);
        a.position = c.position;
        a.quadric = a.quadric + b.quadric;
        a.uv = uv;
        b.alive = false;
        for (const auto i : local) {
            auto& face = faces_[i];
            if (contains(face, c.a) && contains(face, c.b)) {
                face.alive = false;
                --remaining_;
                continue;
            }
            for (auto& corner : face.triangle.corners) {
                if (corner.position == c.a || corner.position == c.b) {
                    corner.position = c.a;
                    corner.uv = uv;
                }
            }
            a.faces.push_back(i);
        }
        for (const auto v : affected) {
            ++vertices_[v].version;
            auto& adjacent = vertices_[v].faces;
            std::erase_if(adjacent, [&](auto face) { return !contains(faces_[face], v); });
            unique(adjacent);
        }
        // 邻居之间的合法性也可能改变，因此更新受影响顶点的全部相邻边。
        std::vector<std::pair<MeshIndex, MeshIndex>> edges;
        for (const auto v : affected) {
            if (vertices_[v].alive) {
                for (const auto neighbor : neighbors(v)) {
                    edges.emplace_back(std::min(v, neighbor), std::max(v, neighbor));
                }
            }
        }
        unique(edges);
        for (const auto [u, v] : edges) {
            enqueue(u, v);
        }
    }

    SimplifySettings settings_;
    CancellationToken token_;
    SimplifyProgressCallback progress_;
    SimplifyResult& result_;
    glm::dvec3 origin_{0};
    std::vector<Vertex> vertices_;
    std::vector<Face> faces_;
    std::priority_queue<Candidate, std::vector<Candidate>, MoreExpensive> heap_;
    std::size_t remaining_;
};

} // namespace

void SimplifySettings::validate() const {
    if (target_triangles == 0 || target_triangles > 16000000) {
        throw std::invalid_argument("Simplification target must be 1..16000000 triangles");
    }
    if (std::isnan(maximum_quadric_error) || maximum_quadric_error < 0) {
        throw std::invalid_argument("Maximum quadric error must be nonnegative");
    }
    if (!std::isfinite(crease_angle_degrees) || crease_angle_degrees <= 0 ||
        crease_angle_degrees > 180) {
        throw std::invalid_argument("Crease angle must be in (0, 180] degrees");
    }
}

const char* simplify_stop_name(SimplifyStop stop) noexcept {
    switch (stop) {
    case SimplifyStop::target_reached:
        return "target_reached";
    case SimplifyStop::constrained:
        return "constrained";
    case SimplifyStop::error_limit:
        return "error_limit";
    }
    return "constrained";
}

SimplifyResult simplify_qem(std::shared_ptr<const MeshData> input, SimplifySettings settings,
                            CancellationToken cancellation, SimplifyProgressCallback progress) {
    cancellation.check();
    settings.validate();
    if (!input) {
        throw std::invalid_argument("Simplification input mesh is missing");
    }
    const auto report = [&](SimplifyStage stage, std::size_t done, std::size_t total,
                            std::size_t faces) {
        cancellation.check();
        if (progress) {
            progress({stage, done, total, faces});
        }
        cancellation.check();
    };
    report(SimplifyStage::input_topology, 0, 1, input->triangles.size());
    const HalfedgeMesh topology(*input, cancellation);
    SimplifyResult result;
    result.mesh = input;
    result.input_topology = topology.statistics();
    result.topology = result.input_topology;
    result.requested_triangles = settings.target_triangles;
    report(SimplifyStage::input_topology, 1, 1, input->triangles.size());
    if (input->triangles.size() > settings.target_triangles) {
        Decimator decimator(*input, topology, settings, cancellation, progress, result);
        decimator.run();
        if (result.collapses) {
            result.mesh = decimator.output();
            const HalfedgeMesh output(*result.mesh, cancellation);
            result.topology = output.statistics();
            if (result.topology.euler_characteristic !=
                    result.input_topology.euler_characteristic ||
                result.topology.boundary_loops != result.input_topology.boundary_loops ||
                result.topology.face_components != result.input_topology.face_components) {
                throw std::logic_error("Edge collapse changed surface topology");
            }
            report(SimplifyStage::error, 0, 1, result.topology.faces);
            result.error =
                compare_surfaces(input, result.mesh, cancellation, [&](auto done, auto total) {
                    report(SimplifyStage::error, done, total, result.topology.faces);
                });
        }
    }
    report(SimplifyStage::complete, 1, 1, result.topology.faces);
    return result;
}

} // namespace minirender
