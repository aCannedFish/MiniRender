#include "geometry/point_cloud_utils.hpp"
#include "geometry/point_index.hpp"
#include "geometry/symmetric_eigen.hpp"
#include <minirender/assets/point_io.hpp>
#include <minirender/core/sampling.hpp>
#include <minirender/geometry/point_normals.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <numbers>

using namespace minirender;

namespace {

// 每个 IO 用例独占临时目录，析构保留其他测试及用户文件。
class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        path = std::filesystem::temp_directory_path() /
               ("minirender-points-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};
void text_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary);
    stream << text;
}
std::string bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
std::shared_ptr<PointCloud> sphere(std::size_t count, glm::vec3 translation = {}) {
    auto cloud = std::make_shared<PointCloud>();
    for (std::size_t i = 0; i < count; ++i) {
        const double z = 1 - 2 * (static_cast<double>(i) + .5) / static_cast<double>(count);
        const double angle = static_cast<double>(i) * std::numbers::pi * (3 - std::sqrt(5.0));
        const double radius = std::sqrt(1 - z * z);
        const glm::vec3 point{radius * std::cos(angle), radius * std::sin(angle), z};
        cloud->positions.push_back(point + translation);
        cloud->normals.push_back(point);
    }
    return cloud;
}

} // namespace

TEST_CASE("PLY round trips positions normals and Unicode paths in all encodings", "[points][io]") {
    TemporaryDirectory directory;
    const auto file = directory.path / std::filesystem::path(u8"点云 空格.ply");
    auto input = *sphere(31);
    input.positions[0] = {.125F, -2.75F, 12345.25F};
    for (const auto format : {PlyFormat::ascii, PlyFormat::little_endian, PlyFormat::big_endian}) {
        std::size_t completed = 0;
        save_point_cloud(input, file, format, {}, [&](auto done, auto total) {
            REQUIRE(done <= total);
            completed = done;
        });
        REQUIRE(completed == input.positions.size());
        const auto loaded = load_point_cloud(file);
        REQUIRE(loaded.positions == input.positions);
        REQUIRE(loaded.normals == input.normals);
    }
    input.normals.clear();
    save_point_cloud(input, file);
    REQUIRE(load_point_cloud(file).normals.empty());
    const auto original = bytes(file);
    CancellationSource source;
    REQUIRE_THROWS_AS(save_point_cloud(input, file, PlyFormat::ascii, source.token(),
                                       [&](auto done, auto total) {
                                           if (done == total) {
                                               source.request();
                                           }
                                       }),
                      OperationCancelled);
    REQUIRE(bytes(file) == original);
    for (const auto& entry : std::filesystem::directory_iterator(directory.path)) {
        REQUIRE(entry.path() == file);
    }
}

TEST_CASE("PLY layout follows declared properties elements and scalar types", "[points][io]") {
    TemporaryDirectory directory;
    const auto file = directory.path / "layout.ply";
    text_file(file,
              "ply\r\nformat ascii 1.0\r\ncomment input layout\r\nobj_info extra\r\n"
              "element face 1\r\nproperty list uchar int vertex_indices\r\n"
              "element vertex 3\r\nproperty uchar red\r\nproperty double z\r\n"
              "property short y\r\nproperty float32 x\r\nproperty list uint8 float weights\r\n"
              "element edge 1\r\nproperty int8 signed\r\nproperty uint32 unsigned\r\nend_header\r\n"
              "3 0 1 2\r\n255 3.5 -2 +1.25 2 0.1 0.9\r\n0 7 4 -0.0 0\r\n"
              "128 -1e-2 -32768 5.75 1 0.3\r\n-128 4294967295\r\n");
    std::size_t completed = 0;
    const auto cloud = load_point_cloud(file, {}, {}, [&](auto done, auto total) {
        REQUIRE(total == 5);
        completed = done;
    });
    REQUIRE(completed == 5);
    REQUIRE(cloud.positions ==
            std::vector<glm::vec3>{{1.25F, -2, 3.5F}, {0, 4, 7}, {5.75F, -32768, -.01F}});
    REQUIRE(cloud.normals.empty());
    REQUIRE_THROWS_AS(load_point_cloud(file, {.maximum_points = 2}), std::length_error);
    REQUIRE_THROWS_AS(load_point_cloud(file, {.maximum_records = 4}), std::length_error);
    REQUIRE_THROWS_AS(load_point_cloud(file, {.maximum_values = 10}), std::length_error);
    REQUIRE_THROWS_AS(load_point_cloud(file, {.maximum_file_bytes = 10}), std::length_error);
    REQUIRE_THROWS_AS(load_point_cloud(file, {.maximum_header_bytes = 20}), std::length_error);
    // 手写大端 double 和有符号整数，避免读写使用同一错误端序仍往返通过。
    std::string binary = "ply\nformat binary_big_endian 1.0\nelement vertex 1\nproperty double x\n"
                         "property int y\nproperty float z\nend_header\n";
    for (const auto value :
         {0x3fU, 0xf4U, 0U, 0U, 0U, 0U, 0U, 0U, 0xffU, 0xffU, 0xffU, 0xfeU, 0x40U, 0x60U, 0U, 0U}) {
        binary.push_back(static_cast<char>(value));
    }
    text_file(file, binary);
    REQUIRE(load_point_cloud(file).positions[0] == glm::vec3(1.25F, -2, 3.5F));
}

TEST_CASE("PLY rejects incomplete invalid and over-budget records", "[points][io]") {
    TemporaryDirectory directory;
    const auto file = directory.path / "invalid.ply";
    const std::string header = "ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\n"
                               "property float y\nproperty float z\n";
    for (const auto& suffix :
         {"end_header\n1 2", "end_header\n1 nan 3", "end_header\n1 2 1e99",
          "property float nx\nend_header\n1 2 3 1", "property float x\nend_header\n1 2 3 4",
          "property list char int data\nend_header\n1 2 3 -1",
          "property uchar data\nend_header\n1 2 3 256"}) {
        text_file(file, header + suffix);
        REQUIRE_THROWS(load_point_cloud(file));
    }
    text_file(file, header + "property list uint int data\nend_header\n1 2 3 4294967295");
    REQUIRE_THROWS_AS(load_point_cloud(file), std::length_error);
    text_file(file, header + "end_header\n1 2 3");
    CancellationSource cancellation;
    REQUIRE_THROWS_AS(load_point_cloud(file, {}, cancellation.token(),
                                       [&](auto, auto) { cancellation.request(); }),
                      OperationCancelled);
    text_file(file, "ply\nformat binary_little_endian 1.0\nelement vertex 1\nproperty float x\n"
                    "property float y\nproperty float z\nend_header\nX");
    REQUIRE_THROWS_AS(load_point_cloud(file), std::invalid_argument);
}

TEST_CASE("Point queries match independent exhaustive distance ordering", "[points][index]") {
    auto cloud = sphere(137);
    cloud->positions.push_back(cloud->positions[0]);
    const detail::PointIndex index(cloud->positions);
    for (const auto point : {glm::dvec3(0), glm::dvec3(1, 0, 0), glm::dvec3(2, -.25, .5)}) {
        std::vector<std::pair<double, std::uint32_t>> expected;
        for (std::uint32_t i = 0; i < cloud->positions.size(); ++i) {
            const auto delta = point - glm::dvec3(cloud->positions[i]);
            expected.emplace_back(glm::dot(delta, delta), i);
        }
        std::sort(expected.begin(), expected.end());
        for (const std::size_t count : {1U, 9U, 200U}) {
            const auto nearest = index.nearest(point, count);
            REQUIRE(nearest.size() == std::min(count, expected.size()));
            for (std::size_t i = 0; i < nearest.size(); ++i) {
                REQUIRE(nearest[i].index == expected[i].second);
                REQUIRE(nearest[i].squared_distance == expected[i].first);
            }
        }
        const auto neighbors = index.radius(point, .75, 200);
        const auto count = std::count_if(expected.begin(), expected.end(),
                                         [](const auto& p) { return p.first <= .75 * .75; });
        REQUIRE(neighbors.size() == static_cast<std::size_t>(count));
        for (std::size_t i = 0; i < neighbors.size(); ++i) {
            REQUIRE(neighbors[i].index == expected[i].second);
        }
    }
    REQUIRE(index.nearest(glm::dvec3(cloud->positions[0]), 1, 0)[0].index == 137);
    REQUIRE(index.radius(glm::dvec3(cloud->positions[0]), 0, 2).size() == 2);
    REQUIRE_THROWS_AS(index.radius({}, 2, 1), std::length_error);
    REQUIRE(index.nearest({}, 1, detail::no_point, .01).empty());
    REQUIRE(index.nearest({}, 0).empty());
    CancellationSource cancellation;
    const detail::PointIndex cancellable(cloud->positions, cancellation.token());
    cancellation.request();
    REQUIRE_THROWS_AS(cancellable.nearest({}, 1), OperationCancelled);
    REQUIRE_THROWS_AS(cancellable.radius({}, 1, 200), OperationCancelled);
}

TEST_CASE("Symmetric eigen decomposition satisfies residual and orthogonality",
          "[points][normals]") {
    Sampler sampler(101);
    for (int trial = 0; trial < 50; ++trial) {
        glm::dmat3 basis;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                basis[i][j] = 2 * sampler.next_1d() - 1;
            }
        }
        const auto matrix = glm::transpose(basis) * basis;
        const auto eigen = detail::symmetric_eigen(matrix);
        REQUIRE(eigen.values.x <= eigen.values.y);
        REQUIRE(eigen.values.y <= eigen.values.z);
        for (int i = 0; i < 3; ++i) {
            REQUIRE(glm::length(matrix * eigen.vectors[i] - eigen.values[i] * eigen.vectors[i]) <
                    2e-12);
            for (int j = 0; j < 3; ++j) {
                REQUIRE(std::abs(glm::dot(eigen.vectors[i], eigen.vectors[j]) -
                                 (i == j ? 1.0 : 0.0)) < 2e-12);
            }
        }
    }
}

TEST_CASE("Point PCA normals agree with planar and spherical geometry", "[points][normals]") {
    auto plane = std::make_shared<PointCloud>();
    const auto normal = glm::normalize(glm::dvec3(-.3, -.2, 1));
    for (int y = 0; y < 9; ++y) {
        for (int x = 0; x < 9; ++x) {
            const double u = x * .2;
            const double v = y * .2;
            plane->positions.emplace_back(u + 10, v - 8, .3 * u + .2 * v + 7);
        }
    }
    plane->positions.push_back(plane->positions[0]);
    const auto planar = prepare_point_normals(plane);
    REQUIRE(planar.duplicate_points == 1);
    REQUIRE(planar.orientation_components == 1);
    REQUIRE(planar.spacing_median > .19);
    REQUIRE(planar.spacing_median < .22);
    for (const auto n : planar.cloud->normals) {
        REQUIRE(glm::dot(glm::dvec3(n), normal) > .99999);
    }
    const auto cloud = sphere(700);
    const auto spherical = prepare_point_normals(cloud);
    REQUIRE(spherical.orientation_components == 1);
    for (std::size_t i = 0; i < cloud->positions.size(); ++i) {
        REQUIRE(glm::dot(spherical.cloud->normals[i], cloud->normals[i]) > .985F);
    }
    REQUIRE(prepare_point_normals(cloud).cloud->normals == spherical.cloud->normals);
    REQUIRE(cloud->normals == sphere(700)->normals);
}

TEST_CASE("Point normal forests preserve components and explicit flip", "[points][normals]") {
    auto first = sphere(150);
    const auto second = sphere(150, {5, 0, 0});
    first->positions.insert(first->positions.end(), second->positions.begin(),
                            second->positions.end());
    first->normals.insert(first->normals.end(), second->normals.begin(), second->normals.end());
    for (std::size_t i = 0; i < first->normals.size(); ++i) {
        first->normals[i] *= i % 3 ? -2.0F : 3.0F;
    }
    PointNormalSettings settings;
    settings.estimate = false;
    settings.maximum_distance = .8;
    const auto result = prepare_point_normals(first, settings);
    REQUIRE(result.orientation_components == 2);
    settings.flip = true;
    const auto flipped = prepare_point_normals(first, settings);
    for (std::size_t i = 0; i < first->normals.size(); ++i) {
        const auto center = i < 150 ? glm::vec3(0) : glm::vec3(5, 0, 0);
        REQUIRE(glm::dot(result.cloud->normals[i], glm::normalize(first->positions[i] - center)) >
                .9999F);
        REQUIRE(flipped.cloud->normals[i] == -result.cloud->normals[i]);
    }
}

TEST_CASE("Point normal failures and each-stage cancellation preserve input", "[points][normals]") {
    const auto cloud = sphere(30);
    const auto original = *cloud;
    REQUIRE_THROWS_AS(prepare_point_normals(cloud, {.neighbors = 2}), std::invalid_argument);
    REQUIRE_THROWS_AS(prepare_point_normals(cloud, {.maximum_neighbor_links = 1}),
                      std::length_error);
    REQUIRE_THROWS_AS(prepare_point_normals(cloud, {.maximum_distance = .001}),
                      std::invalid_argument);
    auto line = std::make_shared<PointCloud>();
    line->positions = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}};
    REQUIRE_THROWS_AS(prepare_point_normals(line), std::invalid_argument);
    for (const auto stage : {PointNormalStage::validation, PointNormalStage::duplicates,
                             PointNormalStage::index, PointNormalStage::estimation,
                             PointNormalStage::orientation, PointNormalStage::complete}) {
        CancellationSource cancellation;
        bool reached = false;
        REQUIRE_THROWS_AS(prepare_point_normals(cloud, {}, cancellation.token(),
                                                [&](auto p) {
                                                    REQUIRE(p.completed <= p.total);
                                                    if (p.stage == stage) {
                                                        reached = true;
                                                        cancellation.request();
                                                    }
                                                }),
                          OperationCancelled);
        REQUIRE(reached);
    }
    REQUIRE(cloud->positions == original.positions);
    REQUIRE(cloud->normals == original.normals);
}

TEST_CASE("Surface point sampling follows triangle areas and reproducible barycentrics",
          "[points]") {
    MeshData mesh;
    mesh.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {3, 0, 0}, {5, 0, 0}, {3, 2, 0}};
    for (const auto ids : {std::array{0U, 1U, 2U}, std::array{3U, 4U, 5U}}) {
        MeshTriangle triangle;
        for (std::size_t i = 0; i < 3; ++i) {
            triangle.corners[i] = {ids[i], {0, 0, 1}, {0, 0}};
        }
        mesh.triangles.push_back(triangle);
    }
    const auto points = sample_mesh_surface(mesh, 10000, 7);
    std::size_t larger = 0;
    for (std::size_t i = 0; i < points.positions.size(); ++i) {
        const auto p = points.positions[i];
        REQUIRE(p.z == 0.0F);
        REQUIRE(points.normals[i] == glm::vec3(0, 0, 1));
        larger += p.x >= 3 ? 1 : 0;
        if (p.x >= 3.0F) {
            REQUIRE((p.x - 3) + p.y <= 2.000001F);
        } else {
            REQUIRE(p.x + p.y <= 1.000001F);
        }
    }
    REQUIRE(larger > 7800);
    REQUIRE(larger < 8200);
    REQUIRE(sample_mesh_surface(mesh, 10000, 7).positions == points.positions);
}
