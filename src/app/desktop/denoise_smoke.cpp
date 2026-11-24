// 通过工作台真实入口改变滤波方式、跳帧和切换相机，检查历史使用与重置。
#include "denoise_smoke.hpp"
#include "builtin_scenes.hpp"
#include "offline_panel.hpp"
#include "resource_catalog.hpp"
#include "workbench.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTimer>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

struct State {
    int stage = 0;
};

void live(Workbench& window, const QString& directory) {
    auto* vp = window.viewport();
    vp->set_offline_visible(false);
    vp->set_screen_trace({4, .03F, 160, 2}, true);
    auto* mode = window.findChild<QComboBox*>("view_mode");
    mode->setCurrentIndex(19);
    vp->reset_history();
    auto first = vp->grab_hdr();
    require(vp->history_age() == 1, "First live frame reused a history");
    for (int i = 0; i < 3; ++i) {
        vp->grab_hdr();
    }
    require(vp->history_age() >= 4, "Static SSGI history did not accumulate");
    mode->setCurrentIndex(20);
    auto        mask   = vp->grab_hdr();
    std::size_t reused = 0, tested = 0;
    for (std::size_t y = 0; y < mask.height(); y += 6) {
        for (std::size_t x = 0; x < mask.width(); x += 6) {
            reused += mask.at(x, y).r > .5;
            ++tested;
        }
    }
    qInfo() << "Live SSGI accepted/tested" << reused << tested;
    require(reused > tested / 4, "Reprojection rejected the static Cornell surfaces");
    mode->setCurrentIndex(19);
    auto camera = vp->camera();
    camera.position.x += .04F;
    vp->set_camera(camera);
    vp->grab_hdr();
    require(vp->history_age() >= 5, "Small camera motion unexpectedly reset temporal transport");
    window.select_object("short_box");
    auto* x = window.findChild<QDoubleSpinBox*>("translation.x");
    x->setValue(x->value() + .1);
    auto moved = vp->grab_hdr();
    require(vp->history_age() > 1, "Rigid motion discarded the whole temporal history");
    double energy = 0;
    for (std::size_t y = 0; y < moved.height(); y += 8) {
        for (std::size_t px = 0; px < moved.width(); px += 8) {
            const auto c = moved.at(px, y);
            require(std::isfinite(c.x + c.y + c.z), "Non-finite live temporal image");
            energy += glm::length(c);
        }
    }
    require(energy > 1, "Live temporal view is black");
    vp->grab().save(directory + "/live-temporal.png");
    camera        = vp->camera();
    camera.target = camera.position + (camera.position - camera.target);
    vp->set_camera(camera);
    vp->grab_hdr();
    require(vp->history_age() == 1, "Camera cut preserved the old SSGI history");
    mode->setCurrentIndex(0);
}
} // namespace

void start_denoise_smoke(Workbench& window, const QString& directory) {
    auto  state = std::make_shared<State>();
    auto* timer = new QTimer(&window);
    QObject::connect(timer, &QTimer::timeout, &window, [state, timer, &window, directory] {
        try {
            if (!window.viewport()->isValid() || window.viewport()->rendered_frames() < 4) {
                return;
            }
            if (state->stage == 0) {
                const auto course  = qEnvironmentVariable("MINIRENDER_TEST_COURSE_ROOT");
                const auto entries = catalog::read(
                    course.isEmpty() ? builtin_scene_directory() : course, course.isEmpty());
                const auto id    = course.isEmpty() ? "motion" : "box";
                auto       entry = std::find_if(entries.begin(), entries.end(),
                                                [id](const auto& e) { return e.id == id; });
                require(entry != entries.end(), "Denoiser sequence missing");
                window.import_resource(*entry, catalog::ImportAction::open_sequence);
            } else if (state->stage >= 1 && state->stage <= 8) {
                auto* viewer = window.findChild<QDialog*>("sequence_viewer");
                require(viewer, "Sequence viewer missing");
                auto* status = viewer->findChild<QLabel*>("frame_status");
                auto* method = viewer->findChild<QComboBox*>("denoise_method");
                auto* slider = viewer->findChild<QSlider*>();
                if (!status->text().contains(" / ")) {
                    return;
                }
                if (state->stage <= 3) {
                    method->setCurrentIndex(state->stage);
                }
                if (state->stage == 4) {
                    slider->setValue(1);
                }
                if (state->stage == 5) {
                    require(status->text().contains("复用历史") &&
                                !status->text().endsWith(" 0 像素"),
                            "Sequential frame has no valid reprojection");
                    qInfo() << status->text();
                    slider->setValue(slider->maximum());
                }
                if (state->stage == 6) {
                    require(status->text().endsWith(" 0 像素"),
                            "Seeking reused nonadjacent history");
                    method->setCurrentIndex(4);
                }
                if (state->stage == 7) {
                    require(method->currentData().toInt() == 4, "Reference frame missing");
                    method->setCurrentIndex(0);
                }
                if (state->stage == 8) {
                    viewer->close();
                    window.offline_panel()->set_settings({48, 48, 2, 101}, {.maximum_depth = 4},
                                                         {.threads = 2});
                    window.offline_panel()->start();
                }
            } else if (state->stage == 9) {
                if (window.offline_panel()->session().progress().state !=
                    minirender::RenderState::completed) {
                    return;
                }
                auto* inspect = window.findChild<QPushButton*>("offline_denoise");
                if (!inspect->isEnabled()) {
                    return;
                }
                inspect->click();
            } else {
                auto* viewer = window.findChild<QDialog*>("sequence_viewer");
                require(viewer, "Offline denoising did not open");
                auto* status = viewer->findChild<QLabel*>("frame_status");
                if (!status->text().contains(" / ")) {
                    return;
                }
                require(status->text().contains("48 × 48"), "Offline guide dimensions changed");
                viewer->close();
                live(window, directory);
                timer->stop();
                qInfo() << "Denoise smoke PASS";
                QApplication::exit(0);
                return;
            }
            ++state->stage;
        } catch (const std::exception& error) {
            timer->stop();
            qCritical() << "Denoise smoke FAIL:" << error.what();
            QApplication::exit(1);
        }
    });
    timer->start(80);
}
