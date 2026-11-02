#include "scene_smoke.hpp"
#include "workbench.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QTreeWidget>
#include <QWheelEvent>

#include <cmath>
#include <memory>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 像素比较不重写渲染算法；用编辑前后差异和 undo 的可逆性检查 GPU 更新链路。
int changed_pixels(const QImage& a, const QImage& b) {
    require(!a.isNull() && a.size() == b.size(), "Capture extents must agree");
    int changed = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const auto first = a.pixelColor(x, y);
            const auto second = b.pixelColor(x, y);
            changed += std::abs(first.red() - second.red()) +
                           std::abs(first.green() - second.green()) +
                           std::abs(first.blue() - second.blue()) >
                       8;
        }
    }
    return changed;
}

struct SmokeState {
    int stage = 0;
    int ticks = 0;
    float original_x = 0;
    float camera_distance = 0;
    QImage baseline;
    QImage edited;
    minirender::Camera before_navigation;
};

} // namespace

void start_scene_smoke(Workbench& window, const QString& directory) {
    require(QDir().mkpath(directory), "Cannot create scene smoke output");
    const auto state = std::make_shared<SmokeState>();
    auto* timer = new QTimer(&window);
    timer->setInterval(150);
    QObject::connect(timer, &QTimer::timeout, &window, [state, timer, &window, directory] {
        try {
            auto* viewport = window.viewport();
            require(viewport->failure().isEmpty(), "Vulkan scene renderer failed");
            require(++state->ticks < 150, "Scene smoke timed out");
            if (viewport->rendered_frames() < 5) {
                return;
            }
            const auto capture = [&](const char* name) {
                auto image = viewport->grab();
                require(!image.isNull(), "Scene readback is empty");
                require(image.save(directory + "/" + QString::fromUtf8(name) + ".png"),
                        "Cannot save scene readback");
                return image;
            };
            switch (state->stage++) {
            case 0:
                window.select_object("blue_cube");
                require(window.findChild<QDoubleSpinBox*>("translation.x") != nullptr,
                        "Missing transform widget");
                break;
            case 1: {
                state->baseline = capture("baseline");
                int blue = 0;
                int gold = 0;
                for (int y = 0; y < state->baseline.height(); ++y) {
                    for (int x = 0; x < state->baseline.width(); ++x) {
                        const auto color = state->baseline.pixelColor(x, y);
                        blue += color.blue() > color.red() + 20 &&
                                color.blue() > color.green() + 10 && color.blue() > 70;
                        gold += color.red() > color.blue() + 30 &&
                                color.green() > color.blue() + 10 && color.red() > 60;
                    }
                }
                qInfo() << "Scene pixels: blue" << blue << "gold" << gold;
                require(blue > 100 && gold > 100, "Expected scene material colors are missing");
                auto* x = window.findChild<QDoubleSpinBox*>("translation.x");
                state->original_x = static_cast<float>(x->value());
                x->setValue(state->original_x - 0.7);
                QMetaObject::invokeMethod(x, "editingFinished", Qt::DirectConnection);
                break;
            }
            case 2:
                state->edited = capture("edited");
                require(changed_pixels(state->baseline, state->edited) > 500,
                        "Transform edit did not change image");
                window.findChild<QAction*>("undo")->trigger();
                break;
            case 3:
                require(changed_pixels(state->baseline, capture("undo")) == 0,
                        "Undo did not restore original image");
                window.findChild<QAction*>("redo")->trigger();
                break;
            case 4:
                require(changed_pixels(state->edited, capture("redo")) == 0,
                        "Redo did not restore edited image");
                window.save_scene_file(directory + "/saved.json");
                window.load_scene_file(directory + "/saved.json");
                break;
            case 5:
                require(changed_pixels(state->edited, capture("reloaded")) == 0,
                        "Saved/reloaded scene differs");
                window.findChild<QComboBox*>("view_mode")->setCurrentIndex(1);
                break;
            case 6: {
                require(changed_pixels(state->edited, capture("normals")) > 1000,
                        "Normal view did not change image");
                window.findChild<QComboBox*>("view_mode")->setCurrentIndex(0);
                state->before_navigation = viewport->camera();
                state->camera_distance =
                    glm::length(viewport->camera().position - viewport->camera().target);
                const QPointF center(viewport->width() * 0.5, viewport->height() * 0.5);
                QWheelEvent event(center, center, {}, {0, 120}, Qt::NoButton, Qt::NoModifier,
                                  Qt::NoScrollPhase, false);
                QCoreApplication::sendEvent(viewport, &event);
                require(glm::length(viewport->camera().position - viewport->camera().target) <
                            state->camera_distance,
                        "Wheel navigation did not dolly camera");
                break;
            }
            case 7: {
                require(changed_pixels(state->edited, capture("dolly")) > 500,
                        "Camera navigation did not change image");
                viewport->set_camera(state->before_navigation);
                window.select_object("blue_cube");
                const auto count = window.document().objects.size();
                window.findChild<QAction*>("duplicate")->trigger();
                require(window.document().objects.size() == count + 1, "Duplicate action failed");
                window.findChild<QAction*>("delete")->trigger();
                require(window.document().objects.size() == count, "Delete action failed");
                const auto position = viewport->camera().position;
                QKeyEvent key(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier);
                QCoreApplication::sendEvent(viewport, &key);
                require(glm::length(position - viewport->camera().position) > 0.01F,
                        "Free camera key movement failed");
                break;
            }
            case 8:
                capture("final");
                timer->stop();
                qInfo() << "Scene smoke PASS: material, transform, undo/redo, save/load, normals, "
                           "camera, duplicate/delete";
                QApplication::exit(0);
                break;
            default:
                break;
            }
        } catch (const std::exception& error) {
            timer->stop();
            qCritical() << "Scene smoke FAIL:" << error.what();
            QApplication::exit(1);
        }
    });
    timer->start();
}
