#include "../assets/lightfield_internal.hpp"
#include <minirender/render/lightfield_capture.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>

namespace minirender {
namespace {
using Clock = std::chrono::steady_clock;

// 只清理自己成功创建的单文件存储，完整结果以共享所有权延长其生命周期。
struct Storage {
    std::filesystem::path directory;
    Storage() {
        std::random_device random;
        for (int attempt = 0; attempt < 64; ++attempt) {
            const auto path = std::filesystem::temp_directory_path() /
                              ("minirender-lightfield-" + std::to_string(random()) + "-" +
                               std::to_string(random()));
            if (std::filesystem::create_directory(path)) {
                directory = path;
                return;
            }
        }
        throw std::runtime_error("Cannot reserve light field temporary storage");
    }
    ~Storage() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};
void add(PathStatistics& sum, const PathStatistics& value) {
    sum.paths += value.paths;
    sum.shadow_rays += value.shadow_rays;
    sum.scattering_events += value.scattering_events;
    sum.roulette_terminations += value.roulette_terminations;
    sum.medium_scattering_events += value.medium_scattering_events;
    sum.subsurface_events += value.subsurface_events;
    sum.subsurface_distance += value.subsurface_distance;
    sum.geometry.bounds_tests += value.geometry.bounds_tests;
    sum.geometry.triangle_tests += value.geometry.triangle_tests;
}
std::uint32_t worker_count(const LightFieldCaptureSettings& settings) {
    const auto hardware = std::thread::hardware_concurrency();
    const auto automatic = std::min(32U, hardware > 1 ? hardware - 1 : 1U);
    const auto columns = (settings.render.width + settings.tile_size - 1) / settings.tile_size;
    const auto rows = (settings.render.height + settings.tile_size - 1) / settings.tile_size;
    return std::uint32_t(
        std::min<std::size_t>(settings.threads ? settings.threads : automatic, columns * rows));
}
} // namespace

struct LightFieldCaptureSession::Impl {
    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    LightFieldCaptureProgress status;
    LightFieldCapturePreview preview;
    std::shared_ptr<const LightFieldFile> result;
    std::thread controller;
    CancellationSource cancellation;
    std::atomic<bool> pause_fast{false};
    std::atomic<std::uint64_t> recorded{0};
    bool pause_requested = false;
    bool controller_parked = false;
    std::uint32_t live = 0, parked = 0;
    std::uint64_t view_base = 0;
    Clock::time_point started{}, finished{};

    void checkpoint(const CancellationToken& token, bool worker) {
        token.check();
        if (!pause_fast.load(std::memory_order_acquire)) {
            return;
        }
        std::unique_lock lock(mutex);
        token.check();
        if (pause_requested) {
            if (worker) {
                ++parked;
            } else {
                controller_parked = true;
            }
            if (!worker || parked == live) {
                status.state = RenderState::paused;
                changed.notify_all();
            }
            changed.wait(lock, [&] { return !pause_requested || token.requested(); });
            if (worker) {
                --parked;
            } else {
                controller_parked = false;
            }
        }
        token.check();
    }
    void worker_finished() {
        std::lock_guard lock(mutex);
        --live;
        if (pause_requested && live && live == parked) {
            status.state = RenderState::paused;
        }
        changed.notify_all();
    }
    void fail(const std::string& error) {
        std::lock_guard lock(mutex);
        if (status.error.empty()) {
            status.error = error;
        }
        cancellation.request();
        pause_requested = false;
        pause_fast.store(false, std::memory_order_release);
        changed.notify_all();
    }
    void complete(RenderState state) {
        std::lock_guard lock(mutex);
        if (!status.error.empty()) {
            state = RenderState::failed;
        } else if (cancellation.token().requested()) {
            state = RenderState::cancelled;
        }
        status.state = state;
        live = parked = 0;
        controller_parked = false;
        if (state != RenderState::completed) {
            result.reset();
        }
        finished = Clock::now();
        changed.notify_all();
    }

    void render_view(const PathTracer& tracer, const LightFieldCalibration& c, std::size_t view,
                     Image& image, const LightFieldCaptureSettings& settings,
                     const CancellationToken& token) {
        const auto count = status.workers;
        const auto tile_size = settings.tile_size;
        const auto columns = (c.render.width + tile_size - 1) / tile_size;
        const auto rows = (c.render.height + tile_size - 1) / tile_size;
        std::atomic<std::size_t> next_tile{0};
        struct alignas(128) Statistics {
            PathStatistics value;
        };
        std::vector<Statistics> statistics(count);
        std::vector<std::thread> threads;
        threads.reserve(count);
        {
            std::lock_guard lock(mutex);
            live = count;
        }
        const auto forward =
            glm::normalize(glm::dvec3(c.camera.target) - glm::dvec3(c.camera.position));
        const auto work = [&](std::uint32_t worker) {
            try {
                for (;;) {
                    token.check();
                    const auto tile = next_tile.fetch_add(1, std::memory_order_relaxed);
                    if (tile >= columns * rows) {
                        break;
                    }
                    const auto x0 = tile % columns * tile_size, y0 = tile / columns * tile_size;
                    for (auto y = y0; y < std::min(y0 + tile_size, c.render.height); ++y) {
                        for (auto x = x0; x < std::min(x0 + tile_size, c.render.width); ++x) {
                            glm::dvec3 sum(0);
                            for (std::uint64_t sample = 0; sample < c.render.samples_per_pixel;
                                 ++sample) {
                                checkpoint(token, true);
                                // 共用随机数保留视图相干性；占用原 PT 的 film/lens 维度。
                                auto random = Sampler::for_sample(c.render.seed,
                                                                  y * c.render.width + x, sample);
                                const auto film = random.next_2d();
                                random.next_2d();
                                const auto ray = lightfield_ray(
                                    c, view,
                                    {(double(x) + film.x) / double(c.render.width),
                                     (double(y) + film.y) / double(c.render.height)});
                                const double cosine = glm::dot(ray.direction, forward);
                                sum += tracer.trace(
                                    ray, random,
                                    {c.camera.near_plane / cosine, c.camera.far_plane / cosine},
                                    &statistics[worker].value);
                                recorded.fetch_add(1, std::memory_order_relaxed);
                            }
                            const auto value = sum / double(c.render.samples_per_pixel);
                            if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
                                !std::isfinite(value.z) ||
                                glm::any(glm::greaterThan(
                                    value, glm::dvec3(std::numeric_limits<float>::max())))) {
                                throw std::overflow_error(
                                    "Light field sample exceeds float image range");
                            }
                            image.at(x, y) = glm::vec3(value);
                        }
                    }
                }
            } catch (const OperationCancelled&) {
            } catch (const std::exception& error) {
                fail(error.what());
            } catch (...) {
                fail("Unknown light field worker failure");
            }
            worker_finished();
        };
        try {
            for (std::uint32_t worker = 0; worker < count; ++worker) {
                threads.emplace_back(work, worker);
            }
        } catch (...) {
            fail("Cannot start light field worker");
            for (auto& thread : threads) {
                thread.join();
            }
            throw;
        }
        for (auto& thread : threads) {
            thread.join();
        }
        {
            std::lock_guard lock(mutex);
            for (const auto& value : statistics) {
                add(status.statistics, value.value);
            }
        }
        token.check();
    }

    void run(std::shared_ptr<const SceneSnapshot> scene, LightFieldCaptureSettings settings,
             LightFieldCalibration c, CancellationToken token) noexcept {
        try {
            {
                PathTracer tracer(std::move(scene), c.integrator, token);
                auto storage = std::make_shared<Storage>();
                LightFieldWriter writer(storage->directory / "capture.mrlf", c);
                std::vector<std::size_t> order{c.center_view()};
                for (std::size_t i = 0; i < c.view_count(); ++i) {
                    if (i != c.center_view()) {
                        order.push_back(i);
                    }
                }
                {
                    std::lock_guard lock(mutex);
                    if (!token.requested()) {
                        status.state = RenderState::running;
                    }
                }
                for (const auto view : order) {
                    checkpoint(token, false);
                    {
                        std::lock_guard lock(mutex);
                        status.current_view = view;
                        status.phase = LightFieldPhase::rendering;
                        view_base = recorded.load(std::memory_order_relaxed);
                        status.work_total = std::uint64_t(c.render.width) * c.render.height *
                                            c.render.samples_per_pixel;
                    }
                    auto image = std::make_shared<Image>(c.render.width, c.render.height);
                    render_view(tracer, c, view, *image, settings, token);
                    {
                        std::lock_guard lock(mutex);
                        status.phase = LightFieldPhase::writing;
                        status.work_completed = 0;
                        status.work_total = c.render.height;
                    }
                    writer.write_view(view, *image, token, [&](LightFieldWork work) {
                        checkpoint(token, false);
                        std::lock_guard lock(mutex);
                        status.work_completed = work.completed;
                    });
                    token.check();
                    {
                        std::lock_guard lock(mutex);
                        ++status.completed_views;
                        preview = {status.job_id, view, status.completed_views, image};
                    }
                }
                checkpoint(token, false);
                {
                    std::lock_guard lock(mutex);
                    status.state = RenderState::finalizing;
                    status.phase = LightFieldPhase::finalizing;
                }
                const auto file =
                    detail::LightFieldAccess::retain(writer.finish(recorded.load()), storage);
                token.check();
                {
                    std::lock_guard lock(mutex);
                    result = file;
                }
            }
            complete(RenderState::completed);
        } catch (const OperationCancelled&) {
            complete(RenderState::cancelled);
        } catch (const std::exception& error) {
            fail(error.what());
            complete(RenderState::failed);
        } catch (...) {
            fail("Unknown light field controller failure");
            complete(RenderState::failed);
        }
    }
};

LightFieldCaptureSession::LightFieldCaptureSession() : impl_(std::make_unique<Impl>()) {}
LightFieldCaptureSession::~LightFieldCaptureSession() {
    cancel();
    wait();
}
void LightFieldCaptureSession::start(std::shared_ptr<const SceneSnapshot> scene,
                                     LightFieldCaptureSettings settings,
                                     IntegratorSettings integrator) {
    if (!scene) {
        throw std::invalid_argument("Light field capture requires a scene snapshot");
    }
    SessionSettings{settings.threads, settings.tile_size, 1}.validate();
    LightFieldCalibration c{
        scene->camera,
        settings.render,
        integrator,
        settings.columns,
        settings.rows,
        settings.aperture_radius,
        settings.reference_focus,
        lightfield_pupils(settings.columns, settings.rows, settings.aperture_radius)};
    c.validate();
    const auto workers = worker_count(settings);
    const bool has_subsurface =
        std::any_of(scene->instances.begin(), scene->instances.end(), [](const auto& instance) {
            return instance.material.kind == MaterialKind::subsurface;
        });
    const std::uint64_t worker_scratch =
        has_subsurface
            ? 2ULL * SubsurfaceSampler::maximum_intersections * sizeof(SurfaceHit) + 65536
            : 1024;
    const auto estimated = 3 * c.view_bytes() + workers * worker_scratch + 131072;
    if (settings.memory_budget_bytes < 1024 * 1024 ||
        settings.memory_budget_bytes > 8ULL * 1024 * 1024 * 1024 ||
        estimated > settings.memory_budget_bytes) {
        throw std::length_error("Light field capture memory budget exceeded");
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (!render_terminal(impl_->status.state)) {
            throw std::logic_error("Light field capture is still active");
        }
    }
    wait();
    {
        std::lock_guard lock(impl_->mutex);
        const auto id = impl_->status.job_id + 1;
        impl_->status = {};
        impl_->status.job_id = id;
        impl_->status.state = RenderState::preparing;
        impl_->status.total_samples = c.total_paths();
        impl_->status.total_views = c.view_count();
        impl_->status.workers = workers;
        impl_->status.estimated_bytes = estimated;
        impl_->status.payload_bytes = c.view_bytes() * c.view_count();
        impl_->recorded.store(0);
        impl_->pause_fast.store(false);
        impl_->pause_requested = false;
        impl_->preview = {};
        impl_->result.reset();
        impl_->live = impl_->parked = 0;
        impl_->controller_parked = false;
        impl_->cancellation = CancellationSource();
        impl_->started = Clock::now();
        impl_->finished = {};
    }
    try {
        impl_->controller = std::thread(&Impl::run, impl_.get(), std::move(scene), settings,
                                        std::move(c), impl_->cancellation.token());
    } catch (...) {
        impl_->fail("Cannot start light field controller");
        impl_->complete(RenderState::failed);
        throw;
    }
}
bool LightFieldCaptureSession::pause() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->status.state != RenderState::running) {
        return false;
    }
    impl_->pause_requested = true;
    impl_->pause_fast.store(true, std::memory_order_release);
    // 恢复后立即再次暂停时，旧等待者可能尚未离开条件变量。
    impl_->status.state = impl_->controller_parked || (impl_->live && impl_->parked == impl_->live)
                              ? RenderState::paused
                              : RenderState::pausing;
    return true;
}
bool LightFieldCaptureSession::resume() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->status.state != RenderState::paused && impl_->status.state != RenderState::pausing) {
        return false;
    }
    impl_->pause_requested = false;
    impl_->pause_fast.store(false, std::memory_order_release);
    impl_->status.state = RenderState::running;
    impl_->changed.notify_all();
    return true;
}
void LightFieldCaptureSession::cancel() {
    std::lock_guard lock(impl_->mutex);
    if (render_terminal(impl_->status.state)) {
        return;
    }
    impl_->cancellation.request();
    impl_->pause_requested = false;
    impl_->pause_fast.store(false, std::memory_order_release);
    impl_->status.state = RenderState::cancelling;
    impl_->changed.notify_all();
}
void LightFieldCaptureSession::wait() {
    if (impl_->controller.joinable()) {
        impl_->controller.join();
    }
}
bool LightFieldCaptureSession::wait_for(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(impl_->mutex);
    return impl_->changed.wait_for(lock, timeout,
                                   [&] { return render_terminal(impl_->status.state); });
}
LightFieldCaptureProgress LightFieldCaptureSession::progress() const {
    std::lock_guard lock(impl_->mutex);
    auto result = impl_->status;
    result.recorded_samples = impl_->recorded.load(std::memory_order_relaxed);
    if (result.phase == LightFieldPhase::rendering) {
        result.work_completed = result.recorded_samples - impl_->view_base;
    }
    const auto end = render_terminal(result.state) ? impl_->finished : Clock::now();
    if (result.job_id) {
        result.elapsed_seconds = std::chrono::duration<double>(end - impl_->started).count();
    }
    return result;
}
LightFieldCapturePreview LightFieldCaptureSession::preview() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->preview;
}
std::shared_ptr<const LightFieldFile> LightFieldCaptureSession::result() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->status.state == RenderState::completed ? impl_->result : nullptr;
}

} // namespace minirender
