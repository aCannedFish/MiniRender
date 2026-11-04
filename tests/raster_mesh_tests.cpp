#include <minirender/render/raster_mesh.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace minirender;

namespace {

// 共享位置、独立面角属性，以及一个不参与三角形的边界位置。
std::shared_ptr<const MeshData> fixture() {
    auto mesh = std::make_shared<MeshData>();
    mesh->positions = {{0, 0, 0}, {2, 0, 0}, {2, 2, 1}, {0, 2, 0}, {9, -3, 7}};
    for (const auto& indices : {std::array{0U, 1U, 2U}, std::array{0U, 2U, 3U}}) {
        MeshTriangle face;
        for (std::size_t corner = 0; corner < 3; ++corner) {
            face.corners[corner].position = indices[corner];
            face.corners[corner].uv = {static_cast<float>(corner),
                                       static_cast<float>(mesh->triangles.size())};
        }
        mesh->triangles.push_back(face);
    }
    mesh->triangles[1].corners[0].normal = {0, 0, 1};
    mesh->validate();
    return mesh;
}

} // namespace

TEST_CASE("Raster preparation preserves face attributes line indices and local bounds",
          "[raster]") {
    const auto mesh = fixture();
    const auto raster = prepare_raster_mesh(*mesh);
    REQUIRE(raster->vertices.size() == 6);
    REQUIRE(raster->edges == std::vector<std::uint32_t>{0, 1, 1, 2, 2, 0, 3, 4, 4, 5, 5, 3});
    REQUIRE(raster->minimum == glm::vec3(0, -3, 0));
    REQUIRE(raster->maximum == glm::vec3(9, 2, 7));
    for (std::size_t face = 0; face < mesh->triangles.size(); ++face) {
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto& original = mesh->triangles[face].corners[corner];
            const auto& actual = raster->vertices[face * 3 + corner];
            REQUIRE(actual.position == mesh->positions[original.position]);
            REQUIRE(actual.normal == original.normal);
            REQUIRE(actual.uv == original.uv);
        }
    }
    REQUIRE_THROWS_AS(prepare_raster_mesh(MeshData{}), std::invalid_argument);
}

TEST_CASE("Raster preparation cancels without modifying source or prior data", "[raster]") {
    auto mesh = std::make_shared<MeshData>(*fixture());
    mesh->triangles.resize(3000, mesh->triangles[0]);
    const auto previous = prepare_raster_mesh(*mesh);
    CancellationSource early;
    early.request();
    REQUIRE_THROWS_AS(prepare_raster_mesh(*mesh, early.token()), OperationCancelled);
    CancellationSource cancellation;
    bool reached = false;
    REQUIRE_THROWS_AS(prepare_raster_mesh(*mesh, cancellation.token(),
                                          [&](auto done, auto total) {
                                              REQUIRE(done <= total);
                                              if (done >= 1024) {
                                                  reached = true;
                                                  cancellation.request();
                                              }
                                          }),
                      OperationCancelled);
    REQUIRE(reached);
    REQUIRE(previous->vertices.size() == 9000);
    REQUIRE(mesh->positions.size() == 5);
    std::size_t completed = 0;
    prepare_raster_mesh(*mesh, {}, [&](auto done, auto total) {
        REQUIRE(done >= completed);
        REQUIRE(total == 3005);
        completed = done;
    });
    REQUIRE(completed == 3005);
}

TEST_CASE("Raster cache follows ownership and retains active meshes plus two recent entries",
          "[raster]") {
    RasterMeshCache cache;
    std::array<std::shared_ptr<const MeshData>, 5> meshes;
    for (auto& mesh : meshes) {
        mesh = fixture();
        cache.supply(mesh, prepare_raster_mesh(*mesh));
    }
    RenderInstance active;
    active.mesh = meshes[0];
    cache.retain(std::span(&active, 1));
    REQUIRE(cache.find(meshes[0]));
    REQUIRE_FALSE(cache.find(meshes[1]));
    REQUIRE_FALSE(cache.find(meshes[2]));
    REQUIRE(cache.find(meshes[3]));
    REQUIRE(cache.find(meshes[4]));
    cache.retain({});
    REQUIRE_FALSE(cache.find(meshes[0]));

    std::weak_ptr<const RasterMeshData> prepared;
    {
        const auto temporary = fixture();
        const auto data = prepare_raster_mesh(*temporary);
        prepared = data;
        cache.supply(temporary, data);
        REQUIRE_FALSE(prepared.expired());
    }
    cache.retain({});
    REQUIRE(prepared.expired());
}
