#include "ball_geometry.hpp"
#include "point_cloud_utils.hpp"
#include "point_index.hpp"
#include <minirender/geometry/ball_pivot.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <numbers>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace minirender {
namespace {

using Face = std::array<std::uint32_t, 3>;
std::uint64_t edge_key(std::uint32_t a, std::uint32_t b) {
    return (static_cast<std::uint64_t>(std::min(a, b)) << 32U) | std::max(a, b);
}
Face face_key(Face face) {
    std::sort(face.begin(), face.end());
    return face;
}
struct FaceHash {
    std::size_t operator()(const Face& face) const noexcept {
        std::size_t hash = 0;
        for (const auto point : face) {
            hash ^= std::hash<std::uint32_t>{}(point) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
        }
        return hash;
    }
};

// 保留首个面中的有向边及对顶点，供下一半径重新构造接触球。
// 第二个面加入后标为内部边；队列中的旧索引通过 active/interior 状态跳过。
struct FrontEdge {
    std::uint32_t a;
    std::uint32_t b;
    std::uint32_t opposite;
    glm::dvec3 center;
    bool interior = false;
    bool active = true;
};
struct Contact {
    std::uint32_t point;
    glm::dvec3 center;
    glm::dvec3 normal;
    glm::dvec3 circumcenter;
    double angle;
};

// 共享边表与顶点面扇执行 join/glue，哈希表只查询，输出顺序由前沿队列确定。
// 每个候选先检查法线、边方向和顶点 link，再将三角形及新前沿一起写入。
class BallPivot final {
  public:
    BallPivot(std::shared_ptr<const PointCloud> cloud, BallPivotSettings settings,
              CancellationToken cancellation, BallPivotCallback progress, BallPivotResult& result)
        : cloud_(std::move(cloud)), settings_(std::move(settings)), cancellation_(cancellation),
          progress_(std::move(progress)), result_(result), index_(cloud_->positions, cancellation),
          incident_(cloud_->positions.size()) {}

    void run(const std::vector<std::uint32_t>& sources) {
        for (pass_ = 0; pass_ < settings_.radii.size(); ++pass_) {
            radius_ = settings_.radii[pass_];
            const auto before = faces_.size();
            BallPivotPass pass;
            pass.radius = radius_;
            report(BallPivotStage::reactivation, 0, edges_.size());
            for (std::size_t i = 0; i < edges_.size(); ++i) {
                if (!edges_[i].interior) {
                    auto& edge = edges_[i];
                    const Face face{edge.a, edge.b, edge.opposite};
                    const auto balls = geometry(face);
                    if (balls && empty_ball(face, balls->outward)) {
                        edge.center = balls->outward;
                        edge.active = true;
                        pending_.push_back(static_cast<std::uint32_t>(i));
                        ++pass.restarted_edges;
                    }
                }
                if (i % 256 == 0) {
                    report(BallPivotStage::reactivation, i + 1, edges_.size());
                }
            }
            drain();
            for (std::uint32_t i = 0; i < cloud_->positions.size(); ++i) {
                report(BallPivotStage::seeds, i, cloud_->positions.size());
                if (incident_[i].empty()) {
                    seed(i);
                    drain();
                }
            }
            pass.new_triangles = faces_.size() - before;
            pass.total_triangles = faces_.size();
            pass.boundary_edges = static_cast<std::size_t>(std::count_if(
                edges_.begin(), edges_.end(), [](const auto& edge) { return !edge.interior; }));
            result_.passes.push_back(pass);
        }
        pass_ = settings_.radii.size() - 1;
        publish(sources);
    }

  private:
    void report(BallPivotStage stage, std::size_t done, std::size_t total) {
        stage_ = stage;
        completed_ = done;
        total_ = total;
        cancellation_.check();
        if (progress_) {
            progress_({stage, done, total, pass_, settings_.radii.size(), faces_.size(),
                       result_.candidate_tests});
        }
        cancellation_.check();
    }
    void work() {
        if (++result_.candidate_tests > settings_.maximum_candidate_tests) {
            throw std::length_error("Ball pivoting exceeds its candidate work budget");
        }
        if (result_.candidate_tests % 1024 == 0) {
            report(stage_, completed_, total_);
        }
    }
    glm::dvec3 point(std::uint32_t index) const {
        return glm::dvec3(cloud_->positions[index]);
    }
    std::optional<detail::ContactBalls> geometry(const Face& face) const {
        return detail::contact_balls(point(face[0]), point(face[1]), point(face[2]), radius_);
    }
    bool compatible(const Face& face, glm::dvec3 normal) {
        for (const auto vertex : face) {
            // 切向或反向法线无法支持该表面面片，避免跨越缺口连接到另一侧。
            if (glm::dot(normal, glm::dvec3(cloud_->normals[vertex])) <= 1e-8) {
                ++result_.rejected_normals;
                return false;
            }
        }
        return true;
    }
    bool empty_ball(const Face& face, glm::dvec3 center) {
        const auto neighbors = index_.radius(center, radius_, settings_.maximum_neighbors);
        const double interior = radius_ * radius_ * (1 - 1e-8);
        for (const auto& neighbor : neighbors) {
            work();
            if (neighbor.index != face[0] && neighbor.index != face[1] &&
                neighbor.index != face[2] && neighbor.squared_distance < interior) {
                return false;
            }
        }
        return true;
    }
    // 前沿在同一点可暂时包含多条链，随后 join/glue 将它们连接。
    // link 入/出度保持唯一；已有点须仍在前沿上，全部面扇已闭合的点不再复用。
    bool valid_fan(std::uint32_t vertex, const Face& proposed) {
        std::unordered_map<std::uint32_t, std::uint32_t> next;
        std::unordered_set<std::uint32_t> previous;
        const auto add = [&](const Face& face) {
            const auto corner = static_cast<std::size_t>(
                std::find(face.begin(), face.end(), vertex) - face.begin());
            const auto a = face[(corner + 1) % 3];
            const auto b = face[(corner + 2) % 3];
            return next.emplace(a, b).second && previous.insert(b).second;
        };
        for (const auto face : incident_[vertex]) {
            work();
            if (!add(faces_[face])) {
                throw std::logic_error("Ball pivoting stored an invalid vertex fan");
            }
        }
        const bool on_front =
            next.empty() || std::any_of(next.begin(), next.end(), [&](const auto& link) {
                return !previous.contains(link.first);
            });
        if (!on_front) {
            return false;
        }
        return add(proposed);
    }
    bool valid_add(const Face& face) {
        if (face_lookup_.contains(face_key(face))) {
            ++result_.rejected_topology;
            return false;
        }
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto a = face[corner];
            const auto b = face[(corner + 1) % 3];
            const auto found = edge_lookup_.find(edge_key(a, b));
            if (found != edge_lookup_.end()) {
                const auto& edge = edges_[found->second];
                if (edge.interior || edge.a == a) {
                    ++result_.rejected_topology;
                    return false;
                }
            }
        }
        for (const auto vertex : face) {
            if (!valid_fan(vertex, face)) {
                ++result_.rejected_topology;
                return false;
            }
        }
        return true;
    }
    void add_face(const Face& face, glm::dvec3 center) {
        cancellation_.check();
        if (faces_.size() == settings_.maximum_triangles) {
            throw std::length_error("Ball pivoting exceeds its triangle budget");
        }
        const auto index = static_cast<std::uint32_t>(faces_.size());
        faces_.push_back(face);
        face_lookup_.insert(face_key(face));
        for (const auto vertex : face) {
            incident_[vertex].push_back(index);
        }
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto a = face[corner];
            const auto b = face[(corner + 1) % 3];
            const auto key = edge_key(a, b);
            const auto existing = edge_lookup_.find(key);
            if (existing == edge_lookup_.end()) {
                const auto edge = static_cast<std::uint32_t>(edges_.size());
                edges_.push_back({a, b, face[(corner + 2) % 3], center});
                edge_lookup_.emplace(key, edge);
                pending_.push_back(edge);
            } else {
                auto& edge = edges_[existing->second];
                edge.interior = true;
                edge.active = false;
            }
        }
    }
    void seed(std::uint32_t first) {
        const auto neighbors =
            index_.radius(point(first), 2 * radius_, settings_.maximum_neighbors);
        for (std::size_t a = 0; a < neighbors.size(); ++a) {
            const auto second = neighbors[a].index;
            if (second == first || !incident_[second].empty()) {
                continue;
            }
            for (std::size_t b = a + 1; b < neighbors.size(); ++b) {
                work();
                const auto third = neighbors[b].index;
                if (third == first || !incident_[third].empty()) {
                    continue;
                }
                Face face{first, second, third};
                auto balls = geometry(face);
                if (!balls) {
                    continue;
                }
                const auto direction = glm::dvec3(cloud_->normals[first]) +
                                       glm::dvec3(cloud_->normals[second]) +
                                       glm::dvec3(cloud_->normals[third]);
                if (glm::dot(balls->normal, direction) < 0) {
                    std::swap(face[1], face[2]);
                    balls = geometry(face);
                }
                if (compatible(face, balls->normal) && empty_ball(face, balls->outward) &&
                    valid_add(face)) {
                    add_face(face, balls->outward);
                    ++result_.seed_triangles;
                    return;
                }
            }
        }
    }
    void pivot(const FrontEdge& edge) {
        const auto a = point(edge.a);
        const auto b = point(edge.b);
        const auto middle = a + (b - a) * .5;
        const auto axis = glm::normalize(b - a);
        const auto start = edge.center - middle;
        const auto neighbors = index_.radius(middle, 2 * radius_, settings_.maximum_neighbors);
        std::vector<Contact> contacts;
        for (const auto& neighbor : neighbors) {
            work();
            const auto next = neighbor.index;
            if (next == edge.a || next == edge.b || next == edge.opposite) {
                continue;
            }
            const Face face{edge.a, next, edge.b};
            const auto balls = geometry(face);
            if (!balls) {
                continue;
            }
            for (const auto center : {balls->outward, balls->inward}) {
                // 同一接触圆有进入和离开球体的两个事件。只保留进入或相切，
                // 避免近共球点因舍入落在起始球内时，将随后的离开事件当作新碰撞。
                const auto velocity = glm::cross(axis, center - middle);
                if (glm::dot(velocity, point(next) - center) < -radius_ * radius_ * 1e-12) {
                    continue;
                }
                double angle = detail::contact_angle(axis, start, center - middle);
                if (std::abs(angle) <= 1e-10) {
                    // 共圆的平面四点可在零角度接触，候选须位于原三角形的另一侧。
                    const auto original = glm::cross(b - a, point(edge.opposite) - a);
                    const auto candidate = glm::cross(b - a, point(next) - a);
                    if (glm::dot(original, candidate) >= 0) {
                        continue;
                    }
                    angle = 0;
                } else if (angle < 0) {
                    angle += 2 * std::numbers::pi;
                }
                contacts.push_back({next, center, balls->normal, balls->circumcenter, angle});
            }
        }
        std::sort(contacts.begin(), contacts.end(), [](const auto& x, const auto& y) {
            return std::tie(x.angle, x.point, x.center.x, x.center.y, x.center.z) <
                   std::tie(y.angle, y.point, y.center.x, y.center.y, y.center.z);
        });
        if (contacts.empty()) {
            return;
        }
        // 所有几何接触先排序。首个接触不可用时保留边界；同角度接触按索引确定顺序。
        for (const auto& contact : contacts) {
            if (contact.angle > contacts.front().angle + 1e-10) {
                break;
            }
            const Face face{edge.a, contact.point, edge.b};
            if (glm::dot(contact.center - contact.circumcenter, contact.normal) <
                    -radius_ * 1e-10 ||
                !compatible(face, contact.normal)) {
                continue;
            }
            if (empty_ball(face, contact.center) && valid_add(face)) {
                add_face(face, contact.center);
                ++result_.pivot_triangles;
                return;
            }
        }
    }
    void drain() {
        std::size_t processed = 0;
        report(BallPivotStage::pivoting, 0, pending_.size());
        while (!pending_.empty()) {
            cancellation_.check();
            const auto id = pending_.front();
            pending_.pop_front();
            const auto edge = edges_[id];
            if (edge.active && !edge.interior) {
                edges_[id].active = false;
                pivot(edge);
            }
            if (++processed % 128 == 0) {
                report(BallPivotStage::pivoting, processed, processed + pending_.size());
            }
        }
        report(BallPivotStage::pivoting, processed, processed);
    }
    void publish(const std::vector<std::uint32_t>& sources) {
        report(BallPivotStage::output, 0, faces_.size());
        if (faces_.empty()) {
            result_.unused_points = cloud_->positions.size();
            return;
        }
        auto mesh = std::make_shared<MeshData>();
        std::vector<Face> output_faces(faces_.size());
        // 前沿终止后按共享边连接每个输入点的面扇。剩余开放分支分别输出，
        // 保留原始位置及法线，并在统计中报告副本，保证后续半边工具可直接使用。
        for (std::uint32_t vertex = 0; vertex < incident_.size(); ++vertex) {
            cancellation_.check();
            const auto& incident = incident_[vertex];
            if (incident.empty()) {
                ++result_.unused_points;
                continue;
            }
            ++result_.used_points;
            std::vector<std::size_t> parents(incident.size());
            for (std::size_t i = 0; i < parents.size(); ++i) {
                parents[i] = i;
            }
            const auto root = [&](std::size_t point) {
                while (parents[point] != point) {
                    parents[point] = parents[parents[point]];
                    point = parents[point];
                }
                return point;
            };
            std::unordered_map<std::uint32_t, std::size_t> neighbor_face;
            for (std::size_t i = 0; i < incident.size(); ++i) {
                if (i % 1024 == 0) {
                    cancellation_.check();
                }
                for (const auto point : faces_[incident[i]]) {
                    if (point == vertex) {
                        continue;
                    }
                    const auto [entry, inserted] = neighbor_face.emplace(point, i);
                    if (!inserted) {
                        parents[root(i)] = root(entry->second);
                    }
                }
            }
            std::vector<std::uint32_t> mapping(incident.size(), detail::no_point);
            bool first = true;
            for (std::size_t i = 0; i < incident.size(); ++i) {
                if (i % 1024 == 0) {
                    cancellation_.check();
                }
                auto& output = mapping[root(i)];
                if (output == detail::no_point) {
                    output = static_cast<std::uint32_t>(mesh->positions.size());
                    mesh->positions.push_back(cloud_->positions[vertex]);
                    result_.source_indices.push_back(sources[vertex]);
                    result_.split_vertices += first ? 0 : 1;
                    first = false;
                }
                const auto face = incident[i];
                const auto corner = static_cast<std::size_t>(
                    std::find(faces_[face].begin(), faces_[face].end(), vertex) -
                    faces_[face].begin());
                output_faces[face][corner] = output;
            }
        }
        for (std::size_t i = 0; i < faces_.size(); ++i) {
            MeshTriangle triangle;
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const auto source = faces_[i][corner];
                triangle.corners[corner] = {
                    output_faces[i][corner], cloud_->normals[source], {0, 0}};
            }
            mesh->triangles.push_back(triangle);
            if (i % 1024 == 0) {
                report(BallPivotStage::output, i + 1, faces_.size());
            }
        }
        const HalfedgeMesh topology(*mesh, cancellation_);
        result_.topology = topology.statistics();
        result_.mesh = std::move(mesh);
        report(BallPivotStage::output, faces_.size(), faces_.size());
    }
    std::shared_ptr<const PointCloud> cloud_;
    BallPivotSettings settings_;
    CancellationToken cancellation_;
    BallPivotCallback progress_;
    BallPivotResult& result_;
    detail::PointIndex index_;
    std::vector<std::vector<std::uint32_t>> incident_;
    std::vector<Face> faces_;
    std::unordered_set<Face, FaceHash> face_lookup_;
    std::vector<FrontEdge> edges_;
    std::unordered_map<std::uint64_t, std::uint32_t> edge_lookup_;
    std::deque<std::uint32_t> pending_;
    std::size_t pass_ = 0;
    double radius_ = 0;
    BallPivotStage stage_ = BallPivotStage::validation;
    std::size_t completed_ = 0;
    std::size_t total_ = 1;
};

} // namespace

void BallPivotSettings::validate() const {
    if (radii.empty() || radii.size() > 32 || maximum_triangles == 0 ||
        maximum_triangles > 16000000 || maximum_neighbors < 3 || maximum_candidate_tests == 0) {
        throw std::invalid_argument(
            "Ball pivoting requires radii and positive triangle/neighbor/work budgets");
    }
    double previous = 0;
    for (const auto radius : radii) {
        if (!std::isfinite(radius) || radius <= previous ||
            radius > std::numeric_limits<float>::max()) {
            throw std::invalid_argument(
                "Ball radii must be finite positive values in increasing order");
        }
        previous = radius;
    }
}

BallPivotResult reconstruct_ball_pivot(std::shared_ptr<const PointCloud> input,
                                       BallPivotSettings settings, CancellationToken cancellation,
                                       BallPivotCallback progress) {
    cancellation.check();
    settings.validate();
    if (!input || input->positions.size() > 2000000) {
        throw std::invalid_argument("Ball pivot input is missing or exceeds two million points");
    }
    BallPivotResult result;
    const auto report = [&](BallPivotStage stage, std::size_t done, std::size_t total) {
        cancellation.check();
        if (progress) {
            progress({stage, done, total,
                      stage == BallPivotStage::complete ? settings.radii.size() - 1 : 0,
                      settings.radii.size(), result.mesh ? result.mesh->triangles.size() : 0,
                      result.candidate_tests});
        }
        cancellation.check();
    };
    report(BallPivotStage::validation, 0, 1);
    input->validate(cancellation);
    if (input->normals.empty()) {
        throw std::invalid_argument("Ball pivoting requires point normals");
    }
    report(BallPivotStage::validation, 1, 1);
    auto unique = detail::unique_points(*input, cancellation, [&](auto done, auto total) {
        report(BallPivotStage::duplicates, done, total);
    });
    result.input_points = input->positions.size();
    result.unique_points = unique.cloud.positions.size();
    result.duplicate_points = result.input_points - result.unique_points;
    for (std::size_t i = 0; i < unique.cloud.normals.size(); ++i) {
        if (i % 1024 == 0) {
            cancellation.check();
        }
        unique.cloud.normals[i] = glm::vec3(glm::normalize(glm::dvec3(unique.cloud.normals[i])));
    }
    report(BallPivotStage::index, 0, 1);
    BallPivot reconstruction(std::make_shared<const PointCloud>(std::move(unique.cloud)), settings,
                             cancellation, progress, result);
    report(BallPivotStage::index, 1, 1);
    reconstruction.run(unique.source_indices);
    report(BallPivotStage::complete, 1, 1);
    return result;
}

} // namespace minirender
