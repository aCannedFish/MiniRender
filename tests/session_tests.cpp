#include <minirender/assets/scene_io.hpp>
#include <minirender/render/render_session.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <thread>

using namespace minirender;
using namespace std::chrono_literals;

namespace {

template <typename Predicate> void wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            FAIL("Timed out waiting for a render state");
        }
        std::this_thread::sleep_for(1ms);
    }
}

std::shared_ptr<const SceneSnapshot> studio() {
    return make_snapshot(
        load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/studio.json"));
}
std::shared_ptr<const SceneSnapshot> sky() {
    SceneDocument scene;
    scene.environment = {0.2F, 0.4F, 0.6F};
    return make_snapshot(scene);
}
void equal_images(const Image& first, const Image& second) {
    REQUIRE(first.width() == second.width());
    REQUIRE(first.height() == second.height());
    for (std::size_t y = 0; y < first.height(); ++y) {
        for (std::size_t x = 0; x < first.width(); ++x) {
            REQUIRE(first.at(x, y) == second.at(x, y));
        }
    }
}
RenderPreview preview(RenderSession& session, std::size_t dimension) {
    RenderPreview result;
    wait_until([&] {
        result = session.preview(dimension);
        return result.image != nullptr;
    });
    return result;
}

} // namespace

TEST_CASE("PT and SPPM sessions retain paused state during rapid resume pause toggles",
          "[session]") {
    for (const auto kind : {IntegratorKind::path_tracing, IntegratorKind::sppm}) {
        IntegratorSettings integrator;
        integrator.kind = kind;
        integrator.maximum_depth = 1;
        integrator.sppm.photons_per_iteration = 1000;
        RenderSession session;
        session.start(sky(), {8, 8, 1000000, 221}, integrator, {2, 4, 1});
        wait_until([&] {
            const auto p = session.progress();
            return p.state == RenderState::running && p.recorded_samples > 0;
        });
        REQUIRE(session.pause());
        wait_until([&] { return session.progress().state == RenderState::paused; });
        for (int cycle = 0; cycle < 20; ++cycle) {
            REQUIRE(session.resume());
            REQUIRE(session.pause());
            wait_until([&] { return session.progress().state == RenderState::paused; });
        }
        session.cancel();
        REQUIRE(session.wait_for(2s));
        session.wait();
        REQUIRE(session.progress().state == RenderState::cancelled);
    }
}

TEST_CASE("Render sessions agree bitwise with serial kernels across thread and batch counts",
          "[session]") {
    const auto scene = studio();
    const RenderSettings render{17, 13, 9, 413};
    const IntegratorSettings integrator{SamplingMode::mis, 4, false, 3};
    const auto reference = PathTracer(scene, integrator).render(render);
    RenderSession session;
    for (const auto settings :
         {SessionSettings{1, 7, 1}, SessionSettings{4, 3, 4}, SessionSettings{3, 8, 8}}) {
        session.start(scene, render, integrator, settings);
        REQUIRE(session.wait_for(10s));
        session.wait();
        const auto progress = session.progress();
        INFO(progress.error);
        REQUIRE(progress.state == RenderState::completed);
        REQUIRE(progress.recorded_samples ==
                render.width * render.height * render.samples_per_pixel);
        REQUIRE(progress.completed_pixels == render.width * render.height);
        REQUIRE(progress.statistics.paths == progress.recorded_samples);
        REQUIRE(session.result());
        equal_images(reference, *session.result());
        const auto full = preview(session, 0);
        REQUIRE(full.job_id == progress.job_id);
        REQUIRE(full.recorded_samples == progress.total_samples);
        equal_images(reference, *full.image);
        const auto reduced = preview(session, 8);
        REQUIRE(reduced.image->width() <= 8);
        REQUIRE(reduced.image->height() <= 8);
    }
    REQUIRE(session.progress().job_id == 3);
    const auto frozen = session.result();
    const auto pixel = frozen->at(8, 6);
    session.start(sky(), {4, 4, 1, 2});
    REQUIRE(session.wait_for(10s));
    session.wait();
    REQUIRE(frozen->at(8, 6) == pixel);
}

TEST_CASE("Pause parks all workers and cancellation publishes only normalized recorded samples",
          "[session]") {
    RenderSession session;
    const RenderSettings render{64, 40, 1000000, 31};
    session.start(sky(), render, {}, {4, 8, 4});
    wait_until([&] { return session.progress().recorded_samples > 0; });
    REQUIRE_THROWS_AS(session.start(sky(), render), std::logic_error);
    REQUIRE(session.pause());
    wait_until([&] { return session.progress().state == RenderState::paused; });
    auto count = session.progress().recorded_samples;
    std::this_thread::sleep_for(20ms);
    REQUIRE(session.progress().recorded_samples == count);
    const auto image = preview(session, 32);
    REQUIRE(image.recorded_samples == count);
    bool lit = false;
    for (std::size_t y = 0; y < image.image->height(); ++y) {
        for (std::size_t x = 0; x < image.image->width(); ++x) {
            const auto pixel = image.image->at(x, y);
            REQUIRE((pixel == glm::vec3(0) || pixel == glm::vec3(0.2F, 0.4F, 0.6F)));
            lit = lit || pixel.x > 0;
        }
    }
    REQUIRE(lit);
    // 快速恢复/再次暂停，包含 worker 尚未从上次条件变量唤醒的窗口。
    for (int cycle = 0; cycle < 12; ++cycle) {
        REQUIRE(session.resume());
        REQUIRE(session.pause());
        wait_until([&] { return session.progress().state == RenderState::paused; });
    }
    REQUIRE(session.resume());
    wait_until([&] { return session.progress().recorded_samples > count; });
    REQUIRE(image.recorded_samples == count);
    session.cancel();
    REQUIRE(session.wait_for(10s));
    session.wait();
    const auto progress = session.progress();
    REQUIRE(progress.state == RenderState::cancelled);
    REQUIRE(progress.recorded_samples < progress.total_samples);
    REQUIRE(progress.recorded_samples >= image.recorded_samples);
    REQUIRE(progress.completed_pixels * render.samples_per_pixel <= progress.recorded_samples);
    REQUIRE_FALSE(session.result());
    REQUIRE(preview(session, 0).recorded_samples == progress.recorded_samples);
}

TEST_CASE("Render preparation and queued tasks support cancellation and restart", "[session]") {
    CancellationSource source;
    const auto token = source.token();
    source.request();
    REQUIRE(token.requested());
    REQUIRE_THROWS_AS(SceneBvh(studio(), token), OperationCancelled);
    REQUIRE_THROWS_AS(LightSampler(studio(), token), OperationCancelled);
    REQUIRE_THROWS_AS(PathTracer(studio(), {}, token), OperationCancelled);
    REQUIRE_FALSE(CancellationSource().token().requested());
    CancellationSource query_source;
    const SceneBvh prepared(studio());
    const PathTracer tracer(sky(), {}, query_source.token());
    query_source.request();
    Sampler random(3);
    REQUIRE_THROWS_AS(prepared.intersect({}, {}, nullptr, query_source.token()),
                      OperationCancelled);
    REQUIRE_THROWS_AS(prepared.occluded({}, {}, nullptr, query_source.token()), OperationCancelled);
    REQUIRE_THROWS_AS(tracer.trace({}, random), OperationCancelled);
    RenderSession session;
    for (int cycle = 0; cycle < 8; ++cycle) {
        session.start(sky(), {64, 64, 1000000, 1}, {}, {2, 8, 4});
        session.cancel();
        REQUIRE(session.wait_for(10s));
        session.wait();
        REQUIRE(session.progress().state == RenderState::cancelled);
    }
    session.start(sky(), {8, 6, 5, 11}, {}, {2, 4, 2});
    REQUIRE(session.wait_for(10s));
    session.wait();
    REQUIRE(session.progress().state == RenderState::completed);
    REQUIRE(session.progress().recorded_samples == 240);
    REQUIRE(session.progress().job_id == 9);
}

TEST_CASE("Worker failure does not publish a partially invalid tile", "[session]") {
    SceneDocument scene;
    scene.environment = glm::vec3(2e38F);
    scene.camera.position = {0, -1, 0};
    scene.camera.target = {0, 0, 0};
    scene.camera.up = {0, 0, 1};
    scene.materials.push_back({"glass", "glass"});
    scene.materials[0].kind = MaterialKind::dielectric;
    scene.materials[0].base_color = glm::vec3(1);
    const auto file = std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/assets/plane.obj";
    scene.meshes.push_back({"plane", {}, std::make_shared<const MeshData>(load_obj(file))});
    scene.objects.push_back({"glass", "glass", "plane", "glass", {}, true});
    const auto snapshot = make_snapshot(scene);
    const IntegratorSettings integrator{SamplingMode::mis, 2, false, 3};
    const PathTracer tracer(snapshot, integrator);
    RenderSettings render{4, 4, 1, 0};
    // 先记录一个可表达的反射样本，随后透射的 eta^2 使结果超出 float 图像范围。
    for (; render.seed < 512; ++render.seed) {
        const auto first = tracer.sample_pixel(0, 0, 0, render).x;
        const auto second = tracer.sample_pixel(1, 0, 0, render).x;
        if (first <= std::numeric_limits<float>::max() &&
            second > std::numeric_limits<float>::max()) {
            break;
        }
    }
    REQUIRE(render.seed < 512);
    RenderSession session;
    session.start(snapshot, render, integrator, {1, 4, 1});
    REQUIRE(session.wait_for(10s));
    session.wait();
    REQUIRE(session.progress().state == RenderState::failed);
    REQUIRE(session.progress().recorded_samples == 0);
    REQUIRE_FALSE(session.progress().error.empty());
    const auto image = preview(session, 0);
    for (std::size_t y = 0; y < 4; ++y) {
        for (std::size_t x = 0; x < 4; ++x) {
            REQUIRE(image.image->at(x, y) == glm::vec3(0));
        }
    }
}

TEST_CASE("Paused jobs resume without changing the serial image", "[session]") {
    const auto scene = studio();
    const RenderSettings render{32, 24, 32, 39};
    const IntegratorSettings integrator{SamplingMode::mis, 4, false, 3};
    RenderSession session;
    session.start(scene, render, integrator, {2, 8, 4});
    wait_until([&] { return session.progress().state != RenderState::preparing; });
    if (session.pause()) {
        wait_until([&] {
            return session.progress().state == RenderState::paused ||
                   render_terminal(session.progress().state);
        });
        if (session.progress().state == RenderState::paused) {
            const auto saved = session.progress().recorded_samples;
            std::this_thread::sleep_for(10ms);
            REQUIRE(saved == session.progress().recorded_samples);
            REQUIRE(session.resume());
        }
    }
    REQUIRE(session.wait_for(10s));
    session.wait();
    REQUIRE(session.progress().state == RenderState::completed);
    equal_images(*session.result(), PathTracer(scene, integrator).render(render));
}

TEST_CASE("Asynchronous failures stay distinct from cancellation and permit recovery",
          "[session]") {
    auto scene = load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/studio.json");
    for (auto& object : scene.objects) {
        object.transform.translation = glm::vec3(1e10F);
        object.transform.scale = glm::vec3(1e-4F);
    }
    RenderSession session;
    session.start(make_snapshot(scene), {8, 8, 1, 1});
    REQUIRE(session.wait_for(10s));
    session.wait();
    REQUIRE(session.progress().state == RenderState::failed);
    REQUIRE_FALSE(session.progress().error.empty());
    REQUIRE_FALSE(session.result());
    session.start(sky(), {8, 8, 1, 1});
    REQUIRE(session.wait_for(10s));
    session.wait();
    REQUIRE(session.progress().state == RenderState::completed);
    REQUIRE(session.progress().error.empty());
}

TEST_CASE("Session idle controls and destruction join paused workers", "[session]") {
    RenderSession idle;
    REQUIRE(idle.progress().state == RenderState::idle);
    REQUIRE(idle.wait_for(1ms));
    REQUIRE_FALSE(idle.pause());
    REQUIRE_FALSE(idle.resume());
    REQUIRE_FALSE(idle.preview().image);
    REQUIRE_THROWS(idle.start(nullptr, {}));
    REQUIRE_THROWS(idle.start(sky(), {0, 1, 1, 1}));
    REQUIRE_THROWS(idle.start(sky(), {}, {}, {33, 1, 1}));
    {
        RenderSession session;
        session.start(sky(), {32, 32, 1000000, 1}, {}, {3, 4, 1});
        wait_until([&] { return session.progress().state == RenderState::running; });
        REQUIRE(session.pause());
        wait_until([&] { return session.progress().state == RenderState::paused; });
    }
}

TEST_CASE("Live previews stay immutable while worker threads keep committing", "[session]") {
    const auto scene = make_snapshot(
        load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/path-room.json"));
    RenderSession session;
    const RenderSettings settings{48, 32, 10000, 73};
    session.start(scene, settings, {}, {4, 8, 1});
    wait_until([&] { return session.progress().recorded_samples > 0; });
    const auto frozen = preview(session, 48);
    std::vector<glm::vec3> pixels;
    for (std::size_t y = 0; y < frozen.image->height(); ++y) {
        for (std::size_t x = 0; x < frozen.image->width(); ++x) {
            pixels.push_back(frozen.image->at(x, y));
        }
    }
    auto previous_samples = frozen.recorded_samples;
    auto previous_revision = frozen.revision;
    for (int iteration = 0; iteration < 32; ++iteration) {
        const auto current = session.preview(24);
        if (current.image) {
            REQUIRE(current.job_id == frozen.job_id);
            REQUIRE(current.recorded_samples >= previous_samples);
            REQUIRE(current.revision >= previous_revision);
            for (std::size_t y = 0; y < current.image->height(); ++y) {
                for (std::size_t x = 0; x < current.image->width(); ++x) {
                    const auto value = current.image->at(x, y);
                    REQUIRE(std::isfinite(value.x));
                    REQUIRE(std::isfinite(value.y));
                    REQUIRE(std::isfinite(value.z));
                }
            }
            previous_samples = current.recorded_samples;
            previous_revision = current.revision;
        }
        const auto progress = session.progress();
        REQUIRE(progress.recorded_samples <= progress.total_samples);
        REQUIRE(progress.completed_pixels * settings.samples_per_pixel <=
                progress.recorded_samples);
        std::this_thread::sleep_for(1ms);
    }
    session.cancel();
    REQUIRE(session.wait_for(10s));
    session.wait();
    for (std::size_t y = 0; y < frozen.image->height(); ++y) {
        for (std::size_t x = 0; x < frozen.image->width(); ++x) {
            REQUIRE(frozen.image->at(x, y) == pixels[y * frozen.image->width() + x]);
        }
    }
}
