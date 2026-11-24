// 检查内置场景的可编辑光照、球体拓扑，以及 Spot 和 Suzanne 的单模型几何与材质。

#include <minirender/assets/scene_io.hpp>
#include <minirender/geometry/halfedge.hpp>
#include <minirender/render/path_tracer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace minirender;

TEST_CASE("Built-in offline scenes load complete editable geometry and lighting", "[presets]") {
    for (const auto* name :
         {"cornell-box", "cornell-materials", "material-balls", "cornell-small-light"}) {
        const auto scene = load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes" /
                                      (std::string(name) + ".json"));
        scene.validate();
        const auto snapshot = make_snapshot(scene);
        REQUIRE(snapshot->instances.size() >= 8);
        REQUIRE(scene.camera.aperture_radius == 0.0F);
        REQUIRE(scene.directional_lights.empty());
        bool emitter = false;
        for (const auto& material : scene.materials) {
            emitter = emitter || glm::length(material.emission) > 1;
            if (material.kind == MaterialKind::dielectric) {
                REQUIRE(material.double_sided);
            }
        }
        REQUIRE(emitter);
        const PathTracer tracer(snapshot, {.maximum_depth = 8});
        const auto       image  = tracer.render({16, 16, 16, 101});
        double           energy = 0;
        for (std::size_t y = 0; y < image.height(); ++y) {
            for (std::size_t x = 0; x < image.width(); ++x) {
                const auto value = image.at(x, y);
                REQUIRE(std::isfinite(value.x));
                REQUIRE(std::isfinite(value.y));
                REQUIRE(std::isfinite(value.z));
                energy += glm::length(value);
            }
        }
        INFO(name);
        REQUIRE(energy > 1);
    }
}

TEST_CASE("Built-in sphere is closed with outward geometry and radial normals", "[presets]") {
    const auto mesh =
        load_obj(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/assets/demo-sphere.obj");
    const HalfedgeMesh topology(mesh);
    REQUIRE(topology.statistics().euler_characteristic == 2);
    REQUIRE(topology.statistics().boundary_edges == 0);
    for (const auto& triangle : mesh.triangles) {
        const auto a = mesh.positions[triangle.corners[0].position];
        const auto b = mesh.positions[triangle.corners[1].position];
        const auto c = mesh.positions[triangle.corners[2].position];
        REQUIRE(glm::dot(glm::cross(b - a, c - a), a + b + c) > 0.0F);
        for (const auto& corner : triangle.corners) {
            REQUIRE(glm::dot(corner.normal, mesh.positions[corner.position]) > 0.999F);
        }
    }
}

TEST_CASE("Classic model presets open as standalone scenes with editable mesh assets",
          "[presets]") {
    const auto catalog = std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes" / "catalog";
    for (const std::string id : {"spot", "suzanne"}) {
        CAPTURE(id);
        const auto scene = load_scene(catalog / (id + ".json"));
        scene.validate();
        REQUIRE(scene.objects.size() == 1);
        REQUIRE(scene.meshes.size() == 1);
        REQUIRE(scene.materials.size() == 1);
        REQUIRE(scene.objects.front().mesh_id == scene.meshes.front().id);
        REQUIRE(scene.objects.front().material_id == scene.materials.front().id);
        REQUIRE(scene.meshes.front().data->triangles.size() == (id == "spot" ? 5856 : 3936));
        if (id == "spot") {
            REQUIRE(scene.textures.size() == 1);
            REQUIRE(scene.materials.front().base_color_texture == scene.textures.front().id);
            REQUIRE(scene.textures.front().encoding == ColorEncoding::srgb);
        } else {
            REQUIRE(scene.textures.empty());
        }
    }
}
