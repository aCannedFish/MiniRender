// 用独立 CPU 路径追踪校验参考图，用 Vulkan 读回校验 B 侧显示，再搬移场景文件夹。
#include "integration_smoke.hpp"
#include "builtin_scenes.hpp"
#include "resource_catalog.hpp"
#include "workbench_internal.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <minirender/assets/scene_io.hpp>

namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

void verify(Workbench& window, const QString& directory) {
    auto*      vp    = window.viewport();
    const auto image = vp->reference_image();
    require(image && vp->comparison_mode() == VulkanViewport::offline_reference_mode &&
                vp->view_mode() == 19,
            "Expected temporal Cornell and path tracing A/B");
    const auto before = vp->camera();
    auto       scene  = std::make_shared<minirender::SceneSnapshot>(*vp->scene());
    scene->camera     = before;
    const auto cpu    = minirender::PathTracer(scene, {.maximum_depth = 10})
                            .render({image->width(), image->height(), 8, 101});
    const auto raw    = vp->grab_comparison_hdr(1);
    require(raw.width() == cpu.width() && raw.height() == cpu.height(),
            "Raw reference resolution changed");
    double maximum = 0;
    for (std::size_t y = 0; y < cpu.height(); ++y) {
        for (std::size_t x = 0; x < cpu.width(); ++x) {
            maximum = std::max(maximum, double(glm::length(raw.at(x, y) - cpu.at(x, y))));
        }
    }
    require(maximum < 5e-5, "Reference session differs from the independent CPU integral");
    const auto a = vp->grab_comparison_side(0), b = vp->grab_comparison_side(1);
    require(!b.isNull() && a != b, "Reference view missing");
    // 按着色器的纹素中心约定重建显示图，检查参考图纵向、曝光与插值。
    double      error  = 0;
    std::size_t tested = 0;
    for (int y = 3; y < b.height(); y += 23) {
        for (int x = 3; x < b.width(); x += 23) {
            const double sx = (double(x) + .5) * double(cpu.width()) / b.width() - .5;
            const double sy = (double(y) + .5) * double(cpu.height()) / b.height() - .5;
            const int    ix = int(std::floor(sx)), iy = int(std::floor(sy));
            const auto   sample = [&](int cx, int cy) {
                return glm::dvec3(cpu.at(std::size_t(std::clamp(cx, 0, int(cpu.width()) - 1)),
                                         std::size_t(std::clamp(cy, 0, int(cpu.height()) - 1))));
            };
            auto value =
                glm::mix(glm::mix(sample(ix, iy), sample(ix + 1, iy), sx - ix),
                         glm::mix(sample(ix, iy + 1), sample(ix + 1, iy + 1), sx - ix), sy - iy) *
                std::exp2(double(vp->exposure()));
            value                  = value / (1. + value);
            auto             pixel = b.pixelColor(x, y);
            const glm::dvec3 actual{pixel.redF(), pixel.greenF(), pixel.blueF()};
            for (int j = 0; j < 3; ++j) {
                error =
                    std::max(error, std::abs(double(minirender::linear_to_srgb(float(value[j]))) -
                                             actual[j]));
            }
            ++tested;
        }
    }
    qInfo() << "Reference native/max linear error/display samples/max sRGB error" << cpu.width()
            << cpu.height() << maximum << tested << error;
    require(error < .015, "Offline B display differs from raw reference");
    require(a.save(directory + "/cornell-realtime.png") &&
                b.save(directory + "/cornell-reference.png"),
            "Cannot export A/B");
    raw.write_exr(workbench_detail::native_path(directory + "/cornell-reference.exr"));
    vp->set_split_position(.45F);
    vp->grab().save(directory + "/cornell-split.png");
    window.select_object("lamp");
    auto* angle = window.findChild<QDoubleSpinBox*>("rotation.z");
    angle->setValue(angle->value() + 12.);
    require(!vp->reference_image(), "Moving the lamp kept a stale reference");
    QTemporaryDir folder(directory + "/portable-XXXXXX");
    require(folder.isValid() && QDir().mkpath(folder.filePath("original")),
            "Cannot create portable folder");
    window.save_scene_file(folder.filePath("original/scene.json"));
    require(QDir().rename(folder.filePath("original"), folder.filePath("移动后")),
            "Cannot relocate portable scene");
    const auto path = folder.filePath("移动后/scene.json");
    window.load_scene_file(path);
    const auto& document = window.document();
    for (const auto& mesh : document.meshes) {
        const auto relative = mesh.source;
        require(!relative.empty() && !relative.is_absolute() && *relative.begin() != ".." &&
                    std::filesystem::is_regular_file(document.asset_directory / relative),
                "Model reference escaped portable save");
    }
    require(vp->scene()->rectangles.size() == 1, "Saved Cornell top light was lost");
}
} // namespace

void start_integration_smoke(Workbench& window, const QString& directory) {
    auto  stage = std::make_shared<int>(0);
    auto* timer = new QTimer(&window);
    QObject::connect(timer, &QTimer::timeout, &window, [&window, stage, timer, directory] {
        try {
            if (!window.viewport()->isValid() || window.asset_loading() ||
                window.viewport()->rendered_frames() < 3) {
                return;
            }
            if (*stage == 0) {
                // 在任何时域通道创建之前读取右侧参考图，验证参考模式只走普通合成。
                auto* viewport = window.viewport();
                viewport->set_view_mode(VIEW_LIGHTING);
                auto reference      = std::make_shared<minirender::Image>(1, 1);
                reference->at(0, 0) = {1, 0, 0};
                viewport->set_reference_image(reference);
                viewport->set_comparison_mode(VIEW_REFERENCE);
                const auto probe = viewport->grab_comparison_side(1);
                require(!probe.isNull(),
                        "Could not display an offline reference before temporal initialization");
                const auto center = probe.pixelColor(probe.width() / 2, probe.height() / 2);
                require(center.red() > 100 && center.green() < 5 && center.blue() < 5,
                        "Offline reference entered the temporal filter");
                viewport->set_comparison_mode(-1);
                viewport->set_reference_image({});
                window.findChild<QCheckBox*>("fxaa_enabled")->setChecked(false);
                window.findChild<QSpinBox*>("reference_size")->setValue(96);
                window.findChild<QSpinBox*>("reference_spp")->setValue(8);
                window.findChild<QAction*>("browse_catalog")->trigger();
                auto* browser = window.findChild<QDialog*>("resource_browser");
                require(browser, "Missing resource directory");
                browser->findChild<QLineEdit*>()->setText("cornell-comparison");
                auto* recommended = browser->findChild<QPushButton*>("catalog_recommended");
                require(recommended && recommended->text() == "覆盖并生成对照",
                        "Integrated preset missing its recommended action");
                recommended->click();
                ++*stage;
                return;
            }
            if (!window.viewport()->reference_image()) {
                return;
            }
            verify(window, directory);
            timer->stop();
            qInfo() << "Integration smoke PASS";
            QApplication::exit(0);
        } catch (const std::exception& error) {
            timer->stop();
            qCritical() << "Integration smoke FAIL:" << error.what();
            QApplication::exit(1);
        }
    });
    timer->start(100);
}
