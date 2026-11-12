#include "lightfield_smoke.hpp"
#include "geometry_smoke.hpp"
#include "lightfield_panel.hpp"
#include "offline_panel.hpp"
#include "ui_response_probe.hpp"
#include "workbench.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <fstream>

namespace {
using namespace minirender;
using Job = LightFieldController::Job;
void require(bool value, const char* msg) {
    if (!value) {
        throw std::runtime_error(msg);
    }
}
template <class T> T* get(Workbench& w, const char* name) {
    auto* c = w.findChild<T*>(name);
    if (!c) {
        throw std::runtime_error(std::string("Missing control: ") + name);
    }
    return c;
}
void click(Workbench& w, const char* name) {
    auto* c = get<QPushButton>(w, name);
    if (!c->isEnabled()) {
        throw std::runtime_error(std::string("Disabled control: ") + name);
    }
    c->click();
}
auto path(const QString& text) {
    return std::filesystem::path(text.toStdU16String());
}
void compare(const Image& a, const Image& b) {
    require(a.width() == b.width() && a.height() == b.height(), "LF image dimensions differ");
    for (std::size_t y = 0; y < a.height(); ++y) {
        for (std::size_t x = 0; x < a.width(); ++x) {
            require(a.at(x, y) == b.at(x, y), "LF pixels differ from independent reader");
        }
    }
}
double contrast(const Image& image, int x0, int x1, int channel) {
    double total = 0;
    for (int y = 20; y < 44; ++y) {
        for (int x = x0; x < x1; ++x) {
            const auto px = std::size_t(x), py = std::size_t(y);
            auto v = image.at(px, py)[channel];
            total += std::abs(v - image.at(px + 1, py)[channel]) +
                     std::abs(v - image.at(px, py + 1)[channel]);
        }
    }
    return total;
}
// 从视口回读显示像素，检查正确的来源、曝光及 letterbox 几何。
void display(Workbench& w, const Image& source) {
    auto* vp = w.viewport();
    auto target = vp->grab();
    auto expected = display_image(source, vp->exposure());
    const double scale = std::min(double(target.width()) / double(source.width()),
                                  double(target.height()) / double(source.height()));
    const int width = int(std::floor(double(source.width()) * scale)),
              height = int(std::floor(double(source.height()) * scale));
    for (std::size_t y = 0; y < source.height(); ++y) {
        for (std::size_t x = 0; x < source.width(); ++x) {
            const auto q =
                target.pixelColor((target.width() - width) / 2 +
                                      int((double(x) + .5) * width / double(source.width())),
                                  (target.height() - height) / 2 +
                                      int((double(y) + .5) * height / double(source.height())));
            const std::array channels{q.red(), q.green(), q.blue()};
            for (int c = 0; c < 3; ++c) {
                require(std::abs(channels[std::size_t(c)] -
                                 encode_unorm8(linear_to_srgb(expected.at(x, y)[c]))) <= 2,
                        "LF Vulkan display differs from source");
            }
        }
    }
}
void layout(Workbench& w, const QString& dir) {
    auto* area = qobject_cast<QScrollArea*>(get<QTabWidget>(w, "inspector_tabs")->currentWidget());
    auto* page = w.lightfield_panel();
    require(area, "LF tab is not scrollable");
    for (auto size : {QSize(1200, 800), QSize(960, 700)}) {
        w.resize(size);
        w.layout()->activate();
        if (area->layout()) {
            area->layout()->activate();
        }
        page->layout()->activate();
        require(area->widget()->width() <= area->viewport()->width(),
                "LF panel needs horizontal scrolling");
        std::vector<QRect> boxes;
        for (auto* field : page->findChildren<QWidget*>()) {
            if (!field->objectName().startsWith("lf_") || !field->isVisibleTo(page) ||
                field->height() == 0) {
                continue;
            }
            const QRect box(field->mapTo(page, QPoint()), field->size());
            require(box.left() >= 0 && box.right() < page->width(), "LF field exceeds page");
            for (const auto& other : boxes) {
                auto overlap = box.intersected(other);
                require(overlap.width() <= 1 || overlap.height() <= 1, "LF fields overlap");
            }
            boxes.push_back(box);
        }
        require(boxes.size() >= 27, "LF layout omitted controls");
        area->ensureWidgetVisible(get<QPushButton>(w, "lf_start"));
        capture_workbench_ui(w, dir + QString("/capture-%1.png").arg(size.width()));
        auto* bottom = get<QPushButton>(w, "lf_png");
        area->ensureWidgetVisible(bottom);
        require(area->viewport()->rect().contains(
                    bottom->mapTo(area->viewport(), bottom->rect().center())),
                "LF export inaccessible");
        capture_workbench_ui(w, dir + QString("/focus-%1.png").arg(size.width()));
        qInfo() << "Lightfield layout" << size << boxes.size();
    }
}
enum class Stage {
    import,
    ready,
    running,
    paused,
    completed,
    near,
    far,
    subview,
    saved,
    broken,
    reopened,
    pt_running,
    pt_shown,
    lf_shown,
    exported,
    cancel_start,
    cancelling,
    cancelled,
    exit_active
};
struct State {
    Stage stage = Stage::import;
    int ticks = 0, frame = 0, settle = 0;
    std::uint64_t samples = 0, job = 0, revision = 0;
    LightFieldCaptureSettings settings;
    std::shared_ptr<const SceneSnapshot> original;
    SceneUploadStatistics uploads;
    std::shared_ptr<const Image> near;
    std::shared_ptr<const LightFieldFile> file;
    UiResponseProbe* probe = nullptr;
    QElapsedTimer cancellation;
};
} // namespace
void start_lightfield_smoke(Workbench& w, const QString& dir, bool enforce) {
    require(QDir().mkpath(dir), "Cannot create LF test directory");
    auto s = std::make_shared<State>();
    s->probe = new UiResponseProbe(&w, "Lightfield");
    auto* timer = new QTimer(&w);
    timer->setInterval(40);
    QObject::connect(timer, &QTimer::timeout, &w, [s, timer, &w, dir, enforce] {
        try {
            auto* lf = w.lightfield_controller();
            auto* panel = w.lightfield_panel();
            auto* vp = w.viewport();
            auto* pt = w.offline_panel();
            auto p = lf->capture().progress();
            auto ready = [&] { return !lf->acquiring() && lf->job() == Job::idle; };
            require(++s->ticks < 5000 && vp->failure().isEmpty(), "LF regression timed out");
            if (!vp->can_render() || vp->rendered_frames() < s->frame + 2) {
                return;
            }
            auto next = [&] {
                s->stage = static_cast<Stage>(int(s->stage) + 1);
                s->frame = vp->rendered_frames();
                s->settle = 0;
                qInfo() << "Lightfield stage" << int(s->stage);
            };
            switch (s->stage) {
            case Stage::import:
                get<QAction>(w, "builtin_lightfield")->trigger();
                next();
                return;
            case Stage::ready:
                if (w.asset_loading() ||
                    get<QTabWidget>(w, "inspector_tabs")->currentIndex() != 11) {
                    return;
                }
                s->settings = panel->capture_settings();
                require(s->settings.columns == 5 && s->settings.aperture_radius == .2 &&
                            w.document().objects.size() == 2,
                        "LF preset missing");
                get<QSpinBox>(w, "lf_columns")->setValue(4);
                click(w, "lf_start");
                require(!get<QLabel>(w, "lf_notice")->text().isEmpty() && !lf->acquiring(),
                        "Invalid LF grid was accepted");
                s->settings.render = {64, 64, 32, 101};
                s->settings.threads = 2;
                panel->set_capture_settings(s->settings);
                s->original = make_snapshot(w.document());
                s->probe->verify([&] { layout(w, dir); });
                click(w, "lf_start");
                next();
                return;
            case Stage::running:
                require(p.state != RenderState::failed, p.error.c_str());
                if (p.state != RenderState::running || p.recorded_samples < 100 ||
                    !get<QPushButton>(w, "lf_pause")->isEnabled()) {
                    return;
                }
                click(w, "lf_pause");
                next();
                return;
            case Stage::paused:
                if (p.state != RenderState::paused) {
                    return;
                }
                if (!s->settle) {
                    s->samples = p.recorded_samples;
                }
                require(s->samples == p.recorded_samples, "Paused LF sampling advanced");
                if (++s->settle < 3) {
                    return;
                }
                w.select_object("near");
                get<QDoubleSpinBox>(w, "translation.x")->setValue(-.6);
                require(lf->stale(), "LF edit did not mark capture stale");
                s->job = pt->session().progress().job_id;
                pt->start();
                require(pt->session().progress().job_id == s->job,
                        "PT started during LF acquisition");
                click(w, "lf_pause");
                next();
                return;
            case Stage::completed: {
                if (!ready()) {
                    return;
                }
                require(lf->file() && lf->stale() && p.state == RenderState::completed,
                        "LF acquisition did not publish its snapshot");
                s->file = lf->file();
                s->samples = p.recorded_samples;
                require(s->samples == 64ULL * 64 * 32 * 25 && s->file->traced_paths() == s->samples,
                        "LF capture path count differs");
                s->probe->verify([&] {
                    LightFieldReader r(s->file);
                    auto center = r.view(12);
                    auto pin = std::make_shared<SceneSnapshot>(*s->original);
                    pin->camera.aperture_radius = 0;
                    compare(*center, PathTracer(pin, s->file->calibration().integrator)
                                         .render(s->settings.render));
                });
                if (!get<QDoubleSpinBox>(w, "lf_focus")->isEnabled()) {
                    return;
                }
                click(w, "lf_near_focus");
                next();
                return;
            }
            case Stage::near: {
                if (!ready() || ++s->settle < 3) {
                    return;
                }
                s->uploads = vp->upload_statistics();
                s->near = lf->image();
                require(bool(s->near), "Missing near focus");
                s->probe->verify([&] {
                    LightFieldReader r(s->file);
                    compare(*s->near, refocus_lightfield(r, {2, .2}).image);
                    display(w, *s->near);
                    vp->grab().save(dir + "/near.png");
                });
                for (int i = 0; i < 24; ++i) {
                    get<QSlider>(w, "lf_focus_slider")->setValue(100 + i * 27);
                }
                click(w, "lf_far_focus");
                next();
                return;
            }
            case Stage::far: {
                if (!ready() || ++s->settle < 3) {
                    return;
                }
                auto far = lf->image();
                require(vp->upload_statistics().mesh_uploads == s->uploads.mesh_uploads &&
                            vp->upload_statistics().descriptor_rebuilds ==
                                s->uploads.descriptor_rebuilds,
                        "Refocus rebuilt scene resources");
                s->probe->verify([&] {
                    LightFieldReader r(s->file);
                    compare(*far, refocus_lightfield(r, {6, .2}).image);
                    display(w, *far);
                    vp->grab().save(dir + "/far.png");
                });
                const auto nr = contrast(*s->near, 12, 25, 0) / contrast(*far, 12, 25, 0),
                           fr = contrast(*far, 39, 52, 2) / contrast(*s->near, 39, 52, 2);
                qInfo() << "Lightfield ROI ratios" << nr << fr;
                require(nr > 1.8 && fr > 1.8, "LF focus did not restore both depth planes");
                require(lf->capture().progress().recorded_samples == s->samples,
                        "Refocus retraced capture");
                get<QComboBox>(w, "lf_mode")->setCurrentIndex(1);
                get<QSpinBox>(w, "lf_row")->setValue(1);
                get<QSpinBox>(w, "lf_column")->setValue(2);
                next();
                return;
            }
            case Stage::subview: {
                if (!ready() || ++s->settle < 3) {
                    return;
                }
                s->probe->verify([&] {
                    LightFieldReader r(s->file);
                    compare(*lf->image(), *r.view(1));
                });
                lf->save(dir + "/重聚焦 数据.mrlf");
                next();
                return;
            }
            case Stage::saved: {
                if (!ready()) {
                    return;
                }
                require(std::filesystem::exists(path(dir + "/重聚焦 数据.mrlf")),
                        "LF file was not saved");
                std::ofstream(path(dir + "/broken.mrlf"), std::ios::binary) << "broken";
                lf->show(false);
                lf->open(dir + "/broken.mrlf", 256ULL << 20, 0);
                next();
                return;
            }
            case Stage::broken:
                if (!ready()) {
                    return;
                }
                require(!vp->offline_visible() && lf->file() == s->file &&
                            lf->selected_views() == 1,
                        "Failed LF open replaced old data");
                s->revision = lf->data_revision();
                lf->open(dir + "/重聚焦 数据.mrlf", 256ULL << 20, 0);
                next();
                return;
            case Stage::reopened:
                if (!ready() || lf->data_revision() == s->revision || ++s->settle < 3) {
                    return;
                }
                require(!lf->stale() && lf->file()->traced_paths() == s->samples,
                        "Opened LF inherited scene edits");
                get<QDoubleSpinBox>(w, "lf_aperture")->setValue(0);
                get<QDoubleSpinBox>(w, "lf_exposure")->setValue(1.5);
                pt->set_settings({16, 12, 1000000, 101}, {}, {.threads = 2});
                pt->start();
                s->job = pt->session().progress().job_id;
                next();
                return;
            case Stage::pt_running: {
                if (!pt->session().progress().recorded_samples || !ready() || ++s->settle < 7) {
                    return;
                }
                require(vp->offline_source() == OfflineImageSource::render && vp->offline_visible(),
                        "Late LF result replaced PT display");
                require(lf->selected_views() == 1, "Aperture zero did not select a single pupil");
                const auto capture_id = lf->capture().progress().job_id;
                click(w, "lf_start");
                require(lf->capture().progress().job_id == capture_id &&
                            !get<QLabel>(w, "lf_notice")->text().isEmpty(),
                        "LF started during PT sampling");
                lf->show(true);
                next();
                return;
            }
            case Stage::pt_shown:
                if (++s->settle < 8) {
                    return;
                }
                require(vp->offline_source() == OfflineImageSource::lightfield &&
                            vp->offline_image() == lf->image(),
                        "PT poll replaced LF display");
                s->probe->verify([&] {
                    LightFieldReader r(lf->file());
                    compare(*lf->image(), *r.view(12));
                    display(w, *lf->image());
                });
                get<QCheckBox>(w, "offline_show")->setChecked(true);
                next();
                return;
            case Stage::lf_shown:
                if (++s->settle < 5) {
                    return;
                }
                require(vp->offline_source() == OfflineImageSource::render &&
                            pt->session().progress().job_id == s->job,
                        "PT could not reclaim its task image");
                click(w, "offline_cancel");
                lf->show(true);
                w.export_display_hdr(dir + "/display.exr");
                next();
                return;
            case Stage::exported:
                if (!ready() || !render_terminal(pt->session().progress().state)) {
                    return;
                }
                compare(*lf->image(), Image::read_exr(path(dir + "/display.exr")));
                lf->export_image(dir + "/display.png", false);
                next();
                return;
            case Stage::cancel_start:
                if (!ready() || !get<QPushButton>(w, "lf_start")->isEnabled()) {
                    return;
                }
                require(std::filesystem::exists(path(dir + "/display.png")),
                        "LF PNG export failed");
                s->settings.render.samples_per_pixel = 1000000;
                panel->set_capture_settings(s->settings);
                click(w, "lf_start");
                next();
                return;
            case Stage::cancelling:
                if (!p.recorded_samples || !get<QPushButton>(w, "lf_cancel")->isEnabled()) {
                    return;
                }
                s->cancellation.start();
                click(w, "lf_cancel");
                next();
                return;
            case Stage::cancelled:
                if (!ready() || !get<QPushButton>(w, "lf_start")->isEnabled()) {
                    return;
                }
                require(p.state == RenderState::cancelled && s->cancellation.elapsed() < 2000,
                        "LF cancel failed");
                qInfo() << "Lightfield cancel ms" << s->cancellation.elapsed();
                click(w, "lf_start");
                next();
                return;
            case Stage::exit_active:
                if (p.state != RenderState::running || p.recorded_samples == 0) {
                    return;
                }
                s->probe->report(enforce);
                timer->stop();
                qInfo() << "Lightfield smoke PASS: capture, files, focus, source ownership, "
                           "export, response and shutdown";
                QApplication::exit(0);
                return;
            }
        } catch (const std::exception& e) {
            timer->stop();
            qCritical() << "Lightfield smoke FAIL:" << e.what();
            QApplication::exit(1);
        }
    });
    timer->start();
}
