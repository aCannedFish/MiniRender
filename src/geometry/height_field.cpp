#include "height_field.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace minirender::detail {

// 标量高度独立使用 double UV/插值，使接缝的分段二次检查与实际位移完全一致。
// 图像仍为线性 float 资产，方向和纹素中心约定与 Texture 相同。
double height_value(const Image& image, glm::dvec2 uv) {
    const auto coordinate = uv - glm::floor(uv);
    const double x = coordinate.x * static_cast<double>(image.width()) - 0.5;
    const double y = (1 - coordinate.y) * static_cast<double>(image.height()) - 0.5;
    const auto ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    const double tx = x - std::floor(x), ty = y - std::floor(y);
    const auto texel = [&](int px, int py) -> double {
        const auto width = static_cast<int>(image.width()),
                   height = static_cast<int>(image.height());
        return image
            .at(static_cast<std::size_t>((px % width + width) % width),
                static_cast<std::size_t>((py % height + height) % height))
            .r;
    };
    const double a = texel(ix, iy), b = texel(ix, iy + 1);
    const double top = a + tx * (texel(ix + 1, iy) - a);
    const double bottom = b + tx * (texel(ix + 1, iy + 1) - b);
    return top + ty * (bottom - top);
}

namespace {

double offset(const Image& image, glm::dvec2 uv, const DisplacementSettings& settings) {
    const double value = settings.scale * (height_value(image, uv) - settings.midlevel);
    if (!std::isfinite(value)) {
        throw std::invalid_argument("Displacement distance is not finite");
    }
    return value;
}

// UV 起点先减去整数平移，保持 Repeat 结果并避免大整数部分损失精度。
std::array<glm::dvec2, 2> edge_uv(const MeshData& mesh, const HalfedgeMesh& topology,
                                  MeshIndex halfedge, bool reverse) {
    const auto& h = topology.halfedges()[halfedge];
    const auto& corners = mesh.triangles[h.face].corners;
    glm::dvec2 a = corners[halfedge % 3].uv;
    glm::dvec2 b = corners[(halfedge % 3 + 1) % 3].uv;
    if (reverse) {
        std::swap(a, b);
    }
    const auto shift = glm::floor(a);
    return {a - shift, b - shift};
}

// 双线性高度沿 UV 线段是分段二次函数，在纹素中心格线处换段。
// 将两侧 u/v 的全部换段参数合并后，每个区间只需端点和一个可能的内部极值。
void add_knots(double start, double end, std::vector<double>& knots, std::size_t budget,
               const CancellationToken& cancellation) {
    const double low = std::min(start, end), high = std::max(start, end);
    if (low == high) {
        return;
    }
    const double count = std::max(0.0, std::ceil(high) - std::floor(low) - 1);
    if (!std::isfinite(count) || count >= static_cast<double>(budget)) {
        throw std::length_error("Displacement seam scan exceeds its segment budget");
    }
    for (double knot = std::floor(low) + 1; knot < high; knot += 1) {
        if (knots.size() % 1024 == 0) {
            cancellation.check();
        }
        knots.push_back((knot - start) / (end - start));
    }
}

[[noreturn]] void seam_error(const char* element, std::size_t index) {
    throw std::invalid_argument(std::string("Discontinuous displacement at ") + element + " " +
                                std::to_string(index));
}

} // namespace

HeightFieldSamples inspect_height_field(const MeshData& input, const HalfedgeMesh& topology,
                                        const Image& image, const DisplacementSettings& settings,
                                        CancellationToken cancellation,
                                        DisplacementProgressCallback progress) {
    HeightFieldSamples result;
    const auto report = [&](DisplacementStage stage, std::size_t done, std::size_t total) {
        cancellation.check();
        if (progress) {
            progress({stage, done, total});
        }
        cancellation.check();
    };
    result.offsets.resize(input.positions.size(), 0);
    std::vector<double> minimums(input.positions.size(), std::numeric_limits<double>::infinity());
    std::vector<double> maximums(input.positions.size(), -std::numeric_limits<double>::infinity());
    const auto samples = input.triangles.size() * 3;
    report(DisplacementStage::samples, 0, samples);
    for (const auto& face : input.triangles) {
        for (const auto& corner : face.corners) {
            const auto vertex = corner.position;
            const double value = offset(image, glm::dvec2(corner.uv), settings);
            if (!std::isfinite(minimums[vertex])) {
                result.offsets[vertex] = value;
            }
            minimums[vertex] = std::min(minimums[vertex], value);
            maximums[vertex] = std::max(maximums[vertex], value);
            const double difference = maximums[vertex] - minimums[vertex];
            if (difference > settings.seam_tolerance) {
                seam_error("vertex", vertex);
            }
            result.maximum_seam_difference = std::max(result.maximum_seam_difference, difference);
            if (++result.sampled_corners % 1024 == 0) {
                report(DisplacementStage::samples, result.sampled_corners, samples);
            }
        }
    }
    report(DisplacementStage::samples, samples, samples);
    const auto hs = topology.halfedges();
    report(DisplacementStage::seams, 0, topology.edges().size());
    for (std::size_t edge = 0; edge < topology.edges().size(); ++edge) {
        const auto h = topology.edges()[edge].halfedge;
        const auto twin = hs[h].twin;
        if (!topology.is_boundary(twin)) {
            const auto first = edge_uv(input, topology, h, false);
            const auto second = edge_uv(input, topology, twin, true);
            if (first != second) {
                ++result.seam_edges;
                std::vector<double> knots{0, 1};
                const auto remaining = settings.maximum_seam_segments - result.seam_segments;
                if (remaining == 0) {
                    throw std::length_error("Displacement seam scan exceeds its segment budget");
                }
                const auto append = [&](const auto& uv) {
                    add_knots(uv[0].x * static_cast<double>(image.width()) - 0.5,
                              uv[1].x * static_cast<double>(image.width()) - 0.5, knots, remaining,
                              cancellation);
                    add_knots((1 - uv[0].y) * static_cast<double>(image.height()) - 0.5,
                              (1 - uv[1].y) * static_cast<double>(image.height()) - 0.5, knots,
                              remaining, cancellation);
                };
                append(first);
                append(second);
                std::sort(knots.begin(), knots.end());
                knots.erase(std::unique(knots.begin(), knots.end()), knots.end());
                if (knots.size() - 1 > remaining) {
                    throw std::length_error("Displacement seam scan exceeds its segment budget");
                }
                result.seam_segments += knots.size() - 1;
                const auto difference = [&](double t) {
                    return offset(image, first[0] + t * (first[1] - first[0]), settings) -
                           offset(image, second[0] + t * (second[1] - second[0]), settings);
                };
                for (std::size_t interval = 0; interval + 1 < knots.size(); ++interval) {
                    if (interval % 1024 == 0) {
                        report(DisplacementStage::seams, edge, topology.edges().size());
                    }
                    const double a = knots[interval], b = knots[interval + 1];
                    const double q0 = difference(a), q1 = difference(b),
                                 qm = difference((a + b) / 2);
                    double maximum = std::max({std::abs(q0), std::abs(q1), std::abs(qm)});
                    if (!std::isfinite(maximum)) {
                        seam_error("edge", edge);
                    }
                    const double normalization = maximum > 0 ? maximum : 1;
                    const double quadratic =
                        2 * (q1 / normalization + q0 / normalization - 2 * qm / normalization);
                    const double linear = q1 / normalization - q0 / normalization - quadratic;
                    if (quadratic != 0) {
                        const double t = -linear / (2 * quadratic);
                        if (t > 0 && t < 1) {
                            maximum = std::max(maximum, std::abs(difference(a + t * (b - a))));
                        }
                    }
                    result.maximum_seam_difference =
                        std::max(result.maximum_seam_difference, maximum);
                    if (maximum > settings.seam_tolerance) {
                        seam_error("edge", edge);
                    }
                }
            }
        }
        if ((edge + 1) % 1024 == 0) {
            report(DisplacementStage::seams, edge + 1, topology.edges().size());
        }
    }
    report(DisplacementStage::seams, topology.edges().size(), topology.edges().size());

    return result;
}

} // namespace minirender::detail
