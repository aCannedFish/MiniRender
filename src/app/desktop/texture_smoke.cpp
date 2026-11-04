#include "texture_smoke.hpp"
#include "ibl_controller.hpp"
#include "workbench.hpp"

#include <minirender/assets/scene_io.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int changed_pixels(const QImage& first, const QImage& second) {
    require(first.size() == second.size(), "Texture captures have different dimensions");
    int changed = 0;
    for (int y = 0; y < first.height(); ++y) {
        for (int x = 0; x < first.width(); ++x) {
            const auto a = first.pixelColor(x, y), b = second.pixelColor(x, y);
            changed += std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) +
                           std::abs(a.blue() - b.blue()) >
                       3;
        }
    }
    return changed;
}

std::filesystem::path path(const QString& value) {
    return std::filesystem::path(value.toStdU16String());
}

void prepare_probe(Workbench& window, const QString& directory, float repeat,
                   const std::filesystem::path& image_path, minirender::TextureFilter filter) {
    auto scene = window.document();
    scene.environment = glm::vec3(1);
    scene.directional_lights.clear();
    scene.camera.position = {0, 4, 0};
    scene.camera.target = {0, 0, 0};
    scene.camera.up = {0, 0, -1};
    std::erase_if(scene.objects,
                  [](const minirender::SceneObject& object) { return object.id != "ground"; });
    require(scene.objects.size() == 1, "Texture probe requires the studio ground");
    scene.objects[0].visible = true;
    scene.objects[0].transform = {};
    scene.objects[0].transform.scale = {3, 1, 3};
    const auto obj_path = path(directory + "/probe-plane.obj");
    std::ofstream(obj_path) << "v -1 0 -1\nv -1 0 1\nv 1 0 1\nv 1 0 -1\n"
                            << "vt 0 0\nvt 0 " << repeat << "\nvt " << repeat << ' ' << repeat
                            << "\nvt " << repeat << " 0\n"
                            << "f 1/1 2/2 3/3 4/4\n";
    for (auto& mesh : scene.meshes) {
        if (mesh.id == scene.objects[0].mesh_id) {
            mesh.source = obj_path;
            mesh.data =
                std::make_shared<const minirender::MeshData>(minirender::load_obj(obj_path));
        }
    }
    scene.textures.clear();
    scene.textures.push_back(
        {"probe", image_path, minirender::ColorEncoding::srgb,
         std::make_shared<const minirender::Texture>(minirender::Image::read(image_path))});
    for (auto& material : scene.materials) {
        material.base_color_texture.clear();
        if (material.id == scene.objects[0].material_id) {
            material.kind = minirender::MaterialKind::diffuse;
            material.base_color = glm::vec3(1);
            material.emission = glm::vec3(0);
            material.base_color_texture = "probe";
            material.texture_filter = filter;
        }
    }
    const auto filename = directory + "/probe.json";
    minirender::save_scene(scene, path(filename));
    window.load_scene_file(filename);
    window.select_object("ground");
}

// 顶视平面在屏幕上是仿射映射；相邻像素射线给出与 GPU quad 导数对应的 UV footprint。
void verify_sampling(Workbench& window, float repeat, minirender::TextureFilter filter) {
    const auto image = window.viewport()->grab_hdr();
    const auto& texture = *window.document().textures[0].data;
    const auto& camera = window.viewport()->camera();
    const float aspect = static_cast<float>(image.width()) / static_cast<float>(image.height());
    const auto plane_uv = [&](glm::vec2 screen) {
        const auto ray = camera.generate_ray(screen, aspect);
        const auto point = ray.origin + ray.direction * (-ray.origin.y / ray.direction.y);
        return glm::vec2((point.x + 3) / 6, (point.z + 3) / 6) * repeat;
    };
    float maximum_error = 0;
    float maximum_lod = 0;
    int compared = 0;
    for (int row = 2; row < 11; ++row) {
        for (int column = 2; column < 11; ++column) {
            const std::size_t x = image.width() * static_cast<std::size_t>(column) / 13;
            const std::size_t y = image.height() * static_cast<std::size_t>(row) / 13;
            const glm::vec2 screen{
                (static_cast<float>(x) + 0.5F) / static_cast<float>(image.width()),
                (static_cast<float>(y) + 0.5F) / static_cast<float>(image.height())};
            const auto uv = plane_uv(screen);
            const glm::vec2 dimensions{static_cast<float>(texture.level(0).width()),
                                       static_cast<float>(texture.level(0).height())};
            if (filter == minirender::TextureFilter::nearest) {
                const auto coordinate = glm::fract(uv * dimensions);
                if (coordinate.x < 0.005F || coordinate.y < 0.005F || coordinate.x > 0.995F ||
                    coordinate.y > 0.995F) {
                    continue;
                }
            }
            const auto dx =
                (plane_uv(screen + glm::vec2(1.0F / static_cast<float>(image.width()), 0)) - uv) *
                dimensions;
            const auto dy =
                (plane_uv(screen + glm::vec2(0, 1.0F / static_cast<float>(image.height()))) - uv) *
                dimensions;
            const float lod = std::max(0.0F, std::log2(std::max(glm::length(dx), glm::length(dy))));
            const auto expected = texture.sample(uv, lod, filter);
            maximum_error = std::max(maximum_error, glm::length(image.at(x, y) - expected));
            maximum_lod = std::max(maximum_lod, lod);
            ++compared;
        }
    }
    qInfo() << "Texture GPU/CPU:" << QString::fromStdString(minirender::texture_filter_name(filter))
            << "samples" << compared << "max error" << maximum_error << "LOD" << maximum_lod;
    require(compared >= 40, "Insufficient texture reference samples");
    require(maximum_error < (repeat > 1 ? 0.005F : 0.0002F),
            "GPU texture filter differs from CPU reference");
    if (repeat > 1) {
        require(maximum_lod > 0.1F && maximum_lod < 1.0F, "Probe must exercise fractional mip LOD");
    }
}

struct State {
    int stage = 0;
    int ticks = 0;
    int frame_before_import = 0;
    QImage original;
    QImage textured;
    std::filesystem::path checker;
};

} // namespace

void start_texture_smoke(Workbench& window, const QString& directory) {
    require(QDir().mkpath(directory), "Cannot create texture output directory");
    auto state = std::make_shared<State>();
    state->checker = window.document().asset_directory / "assets/textures/checker.png";
    auto* timer = new QTimer(&window);
    timer->setInterval(150);
    QObject::connect(timer, &QTimer::timeout, &window, [state, timer, &window, directory] {
        try {
            require(++state->ticks < 250, "Texture test timed out");
            require(window.viewport()->failure().isEmpty(), "Texture renderer failed");
            if (window.viewport()->rendered_frames() < 5) {
                return;
            }
            if (!window.viewport()->isExposed()) {
                window.raise();
                window.activateWindow();
                return;
            }
            if (state->stage == 0 &&
                window.ibl_controller()->status().state != IblTaskState::ready) {
                return;
            }
            const auto capture = [&](const char* name) {
                const auto image = window.viewport()->grab();
                require(!image.isNull() && image.save(directory + "/" + name + ".png"),
                        "Texture capture failed");
                return image;
            };
            switch (state->stage++) {
            case 0:
                window.select_object("blue_cube");
                state->original = capture("before-import");
                state->frame_before_import = window.viewport()->rendered_frames();
                window.import_texture_file(QString::fromStdU16String(state->checker.u16string()),
                                           minirender::ColorEncoding::srgb);
                require(window.texture_loading(), "Import did not start asynchronously");
                break;
            case 1:
                if (window.texture_loading()) {
                    --state->stage;
                    return;
                }
                require(window.viewport()->rendered_frames() > state->frame_before_import,
                        "Viewport did not continue during import");
                require(window.document().textures.size() == 1, "Texture was not registered");
                state->textured = capture("imported");
                require(changed_pixels(state->original, state->textured) > 500,
                        "Binding texture did not change image");
                window.findChild<QAction*>("undo")->trigger();
                break;
            case 2:
                require(changed_pixels(state->original, capture("undo")) == 0,
                        "Texture undo did not restore image");
                window.findChild<QAction*>("redo")->trigger();
                break;
            case 3:
                require(changed_pixels(state->textured, capture("redo")) == 0,
                        "Texture redo did not restore image");
                window.save_scene_file(directory + "/saved.json");
                window.load_scene_file(directory + "/saved.json");
                break;
            case 4:
                require(changed_pixels(state->textured, capture("reloaded")) == 0,
                        "Texture scene reload changed image");
                prepare_probe(window, directory, 1, state->checker,
                              minirender::TextureFilter::nearest);
                break;
            case 5:
                verify_sampling(window, 1, minirender::TextureFilter::nearest);
                capture("nearest");
                window.findChild<QComboBox*>("texture_filter")->setCurrentIndex(1);
                break;
            case 6:
                verify_sampling(window, 1, minirender::TextureFilter::bilinear);
                capture("bilinear");
                prepare_probe(window, directory, 256, state->checker.parent_path() / "ramp.png",
                              minirender::TextureFilter::trilinear);
                break;
            case 7:
                verify_sampling(window, 256, minirender::TextureFilter::trilinear);
                capture("trilinear");
                timer->stop();
                qInfo() << "Texture smoke PASS: async import, history, persistence and CPU/GPU "
                           "filtering";
                QApplication::exit(0);
                break;
            default:
                break;
            }
        } catch (const std::exception& error) {
            timer->stop();
            qCritical() << "Texture smoke FAIL:" << error.what();
            QApplication::exit(1);
        }
    });
    timer->start();
}
