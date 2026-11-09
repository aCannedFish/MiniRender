#include "wire_smoke.hpp"
#include "workbench.hpp"

#include <minirender/assets/scene_io.hpp>

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QTimer>

#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 前方正方形遮挡后方三角形的中段，三角形顶部仍可见。
// 采样位置来自世界空间几何，与线段光栅化实现无关。
void prepare_scene(Workbench& window, const QString& directory) {
    const auto path = std::filesystem::path(directory.toStdU16String());
    const auto front = path / "front.obj";
    const auto back = path / "back.obj";
    std::ofstream(front) << "v -0.8 -0.8 0\nv 0.8 -0.8 0\nv 0.8 0.8 0\nv -0.8 0.8 0\n"
                            "f 1 2 3\nf 1 3 4\n";
    std::ofstream(back) << "v -1.2 -1.4 -0.8\nv 1.2 -1.4 -0.8\nv 0 1.4 -0.8\nf 1 2 3\n";
    minirender::SceneDocument scene;
    scene.camera.position = {0, 0, 4};
    scene.camera.target = {0, 0, 0};
    scene.environment = glm::vec3(0);
    scene.materials.push_back({"test", "test"});
    scene.materials[0].emission = {3, 1, 0.5F};
    scene.meshes.push_back(
        {"front", front,
         std::make_shared<const minirender::MeshData>(minirender::load_obj(front))});
    scene.meshes.push_back(
        {"back", back, std::make_shared<const minirender::MeshData>(minirender::load_obj(back))});
    scene.objects.push_back({"front", "front", "front", "test", {}, true});
    scene.objects.push_back({"back", "back", "back", "test", {}, true});
    const auto file = directory + "/wire.json";
    minirender::save_scene(scene, std::filesystem::path(file.toStdU16String()));
    window.load_scene_file(file);
}

void verify_edges(Workbench& window, const QImage& image) {
    const auto& camera = window.viewport()->camera();
    const float aspect = static_cast<float>(image.width()) / static_cast<float>(image.height());
    const auto matrix = camera.projection_matrix(aspect) * camera.view_matrix();
    const auto sample = [&](glm::vec3 point, bool visible, const char* object) {
        const auto clip = matrix * glm::vec4(point, 1);
        const glm::vec2 uv = glm::vec2(clip) / clip.w * 0.5F + 0.5F;
        // CPU 查询略向几何内部移动，避免精确边界的浮点舍入导致漏交。
        // 光栅像素检查仍投影原始边线位置。
        const auto inside = uv * 0.9999F + glm::vec2(0.00005F);
        const auto picked = minirender::pick_object(*window.viewport()->scene(),
                                                    camera.generate_ray(inside, aspect));
        require(picked && *picked == object, "Wire probe has unexpected CPU occlusion");
        const QPoint pixel(static_cast<int>(uv.x * static_cast<float>(image.width())),
                           static_cast<int>(uv.y * static_cast<float>(image.height())));
        bool white = false;
        for (int y = -3; y <= 3; ++y) {
            for (int x = -3; x <= 3; ++x) {
                require(image.rect().contains(pixel + QPoint(x, y)), "Wire probe is outside image");
                const auto color = image.pixelColor(pixel + QPoint(x, y));
                white = white || (color.red() > 220 && color.green() > 220 && color.blue() > 220);
            }
        }
        require(white == visible, visible ? "Visible mesh edge is missing"
                                          : "Occluded mesh edge leaked through surface");
    };
    sample({-0.8F, 0, 0}, true, "front");
    sample({0.8F, 0, 0}, true, "front");
    sample({-0.06F, 1.26F, -0.8F}, true, "back");
    sample({0.06F, 1.26F, -0.8F}, true, "back");
    sample({-0.6F, 0, -0.8F}, false, "front");
    sample({0.6F, 0, -0.8F}, false, "front");
    qInfo() << "Wire geometry checks: 4 visible edges, 2 occluded edges";
}

struct State {
    int step = 0;
    int ticks = 0;
    int generation = 0;
    std::optional<minirender::Image> hdr;
    SceneUploadStatistics uploads;
};

} // namespace

void start_wire_smoke(Workbench& window, const QString& directory) {
    require(QDir().mkpath(directory), "Cannot create wire output directory");
    const auto state = std::make_shared<State>();
    auto* timer = new QTimer(&window);
    timer->setInterval(150);
    QObject::connect(timer, &QTimer::timeout, &window, [state, timer, &window, directory] {
        try {
            auto* viewport = window.viewport();
            require(++state->ticks < 180 && viewport->failure().isEmpty(),
                    "Wire test timed out or renderer failed");
            if (viewport->rendered_frames() < 5) {
                return;
            }
            if (!viewport->can_render()) {
                return;
            }
            const auto capture = [&](const char* name) {
                const auto image = viewport->grab();
                require(!image.isNull() && image.save(directory + "/" + name + ".png"),
                        "Wire capture failed");
                return image;
            };
            switch (state->step++) {
            case 0:
                prepare_scene(window, directory);
                break;
            case 1:
                state->hdr = viewport->grab_hdr();
                state->uploads = viewport->upload_statistics();
                window.findChild<QComboBox*>("view_mode")->setCurrentIndex(3);
                break;
            case 2: {
                verify_edges(window, capture("wire"));
                const auto hdr = viewport->grab_hdr();
                for (std::size_t y = 0; y < hdr.height(); ++y) {
                    for (std::size_t x = 0; x < hdr.width(); ++x) {
                        require(glm::length(hdr.at(x, y) - state->hdr->at(x, y)) < 1e-6F,
                                "Wire display changed raw HDR output");
                    }
                }
                require(viewport->upload_statistics().mesh_uploads == state->uploads.mesh_uploads &&
                            viewport->upload_statistics().descriptor_rebuilds ==
                                state->uploads.descriptor_rebuilds,
                        "Wire display rebuilt scene assets");
                state->generation = viewport->resource_generations();
                window.resize(window.width() - 90, window.height() - 60);
                break;
            }
            case 3:
                if (viewport->resource_generations() <= state->generation) {
                    --state->step;
                    return;
                }
                verify_edges(window, capture("wire-resized"));
                window.findChild<QComboBox*>("view_mode")->setCurrentIndex(0);
                break;
            case 4:
                capture("surface-restored");
                timer->stop();
                qInfo() << "Wire smoke PASS: visible/hidden edges, HDR isolation, cached assets "
                           "and resize";
                QApplication::exit(0);
                break;
            }
        } catch (const std::exception& error) {
            timer->stop();
            qCritical() << "Wire smoke FAIL:" << error.what();
            QApplication::exit(1);
        }
    });
    timer->start();
}
