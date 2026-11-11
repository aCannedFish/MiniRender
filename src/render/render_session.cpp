#include <minirender/render/render_session.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace minirender {
namespace {

using Clock = std::chrono::steady_clock;

// 一个 tile 始终由同一 worker 持有。提交的是该像素从样本 0 开始的完整和及计数，
// Film 不改变累加顺序。预览锁不会阻塞 UI，最终转换由 controller 负责。
class Film {
  public:
    explicit Film(RenderSettings settings, bool accumulate_samples = true)
        : settings_(settings),
          sums_(accumulate_samples ? settings.width * settings.height : 0, glm::dvec3(0)),
          counts_(sums_.size(), 0) {}

    // SPPM 已生成当前完整迭代的估计，直接替换不可变图像，避免把半径变化当作逐样本加和。
    void publish_image(std::shared_ptr<const Image> image, std::uint32_t iterations) {
        std::lock_guard lock(mutex_);
        final_ = std::move(image);
        ++revision_;
        recorded_.store(std::uint64_t(settings_.width) * settings_.height * iterations,
                        std::memory_order_release);
        completed_pixels_.store(
            iterations == settings_.samples_per_pixel ? settings_.width * settings_.height : 0,
            std::memory_order_release);
    }

    void commit(std::size_t x, std::size_t y, std::size_t width, std::size_t height,
                const std::vector<glm::dvec3>& sums, const std::vector<std::uint32_t>& counts) {
        // 先验证局部批次，成功后再改工作缓冲和计数，失败不发布半个批次。
        for (std::size_t index = 0; index < counts.size(); ++index) {
            if (counts[index] == 0) {
                continue;
            }
            const auto value = sums[index] / static_cast<double>(counts[index]);
            if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z) ||
                glm::any(glm::greaterThan(value, glm::dvec3(std::numeric_limits<float>::max())))) {
                throw std::overflow_error("Render sample exceeds the float image range");
            }
        }
        std::lock_guard lock(mutex_);
        std::uint64_t added = 0, finished = 0;
        for (std::size_t row = 0; row < height; ++row) {
            for (std::size_t column = 0; column < width; ++column) {
                const auto local = row * width + column;
                if (counts[local] == 0) {
                    continue;
                }
                const auto destination = (y + row) * settings_.width + x + column;
                added += counts[local] - counts_[destination];
                finished += counts[local] == settings_.samples_per_pixel &&
                            counts_[destination] != settings_.samples_per_pixel;
                sums_[destination] = sums[local];
                counts_[destination] = counts[local];
            }
        }
        if (added) {
            ++revision_;
            recorded_.fetch_add(added, std::memory_order_release);
            completed_pixels_.fetch_add(finished, std::memory_order_release);
        }
    }

    RenderPreview preview(std::size_t maximum_dimension) const {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            return {};
        }
        return {make_image(maximum_dimension), 0, revision_, recorded()};
    }
    std::shared_ptr<const Image> finish(CancellationToken cancellation) {
        // workers 已全部收回；UI 仍可读取预览，因此转换期间用同一锁保护工作缓冲。
        // preview 使用 try_lock，不会等待这次完整分辨率转换。
        std::lock_guard lock(mutex_);
        auto image = make_image(0, cancellation);
        std::vector<glm::dvec3>().swap(sums_);
        std::vector<std::uint32_t>().swap(counts_);
        final_ = image;
        return image;
    }
    std::uint64_t recorded() const noexcept {
        return recorded_.load(std::memory_order_acquire);
    }
    std::uint64_t completed_pixels() const noexcept {
        return completed_pixels_.load(std::memory_order_acquire);
    }

  private:
    std::shared_ptr<const Image> make_image(std::size_t maximum_dimension,
                                            CancellationToken cancellation = {}) const {
        if (final_ && (maximum_dimension == 0 ||
                       std::max(settings_.width, settings_.height) <= maximum_dimension)) {
            return final_;
        }
        const double scale =
            maximum_dimension
                ? std::max(1.0, static_cast<double>(std::max(settings_.width, settings_.height)) /
                                    static_cast<double>(maximum_dimension))
                : 1;
        const auto width = std::max<std::size_t>(
            1, static_cast<std::size_t>(static_cast<double>(settings_.width) / scale));
        const auto height = std::max<std::size_t>(
            1, static_cast<std::size_t>(static_cast<double>(settings_.height) / scale));
        auto image = std::make_shared<Image>(width, height);
        for (std::size_t y = 0; y < height; ++y) {
            cancellation.check();
            const auto source_y = std::min(settings_.height - 1,
                                           (y * settings_.height + settings_.height / 2) / height);
            for (std::size_t x = 0; x < width; ++x) {
                const auto source_x = std::min(settings_.width - 1,
                                               (x * settings_.width + settings_.width / 2) / width);
                const auto pixel = source_y * settings_.width + source_x;
                image->at(x, y) =
                    final_ ? final_->at(source_x, source_y)
                           : (!counts_.empty() && counts_[pixel]
                                  ? glm::vec3(sums_[pixel] / static_cast<double>(counts_[pixel]))
                                  : glm::vec3(0));
            }
        }
        return image;
    }
    RenderSettings settings_;
    mutable std::mutex mutex_;
    std::vector<glm::dvec3> sums_;
    std::vector<std::uint32_t> counts_;
    std::shared_ptr<const Image> final_;
    std::atomic<std::uint64_t> recorded_{0}, completed_pixels_{0};
    std::uint64_t revision_ = 0;
};

void add_statistics(PathStatistics& result, const PathStatistics& value) {
    result.paths += value.paths;
    result.shadow_rays += value.shadow_rays;
    result.scattering_events += value.scattering_events;
    result.roulette_terminations += value.roulette_terminations;
    result.medium_scattering_events += value.medium_scattering_events;
    result.geometry.bounds_tests += value.geometry.bounds_tests;
    result.geometry.triangle_tests += value.geometry.triangle_tests;
}

} // namespace

bool render_terminal(RenderState state) noexcept {
    return state == RenderState::idle || state == RenderState::cancelled ||
           state == RenderState::completed || state == RenderState::failed;
}
std::string render_state_name(RenderState state) {
    switch (state) {
    case RenderState::idle:
        return "idle";
    case RenderState::preparing:
        return "preparing";
    case RenderState::running:
        return "running";
    case RenderState::pausing:
        return "pausing";
    case RenderState::paused:
        return "paused";
    case RenderState::finalizing:
        return "finalizing";
    case RenderState::cancelling:
        return "cancelling";
    case RenderState::cancelled:
        return "cancelled";
    case RenderState::completed:
        return "completed";
    case RenderState::failed:
        return "failed";
    }
    throw std::invalid_argument("Unknown render state");
}
void SessionSettings::validate() const {
    if (threads > 32 || tile_size == 0 || tile_size > 128 || batch_samples == 0 ||
        batch_samples > 1024) {
        throw std::invalid_argument(
            "Session requires <=32 threads, tile size in [1,128], batch in [1,1024]");
    }
}

// control 只保护状态和暂停协议；Film 自行保护像素。两种锁不同时持有，避免锁顺序反转。
struct RenderSession::Impl {
    mutable std::mutex control;
    mutable std::condition_variable changed;
    RenderState state = RenderState::idle;
    std::uint64_t job = 0, total = 0;
    std::uint32_t workers = 0, live = 0, parked = 0;
    bool pause_requested = false;
    std::atomic<bool> pause_fast{false};
    std::string error;
    Clock::time_point started{}, finished{};
    CancellationSource cancellation;
    std::thread controller;
    std::shared_ptr<Film> film;
    std::shared_ptr<const Image> result;
    std::shared_ptr<Film> retired_film;
    std::shared_ptr<const Image> retired_result;
    PathStatistics statistics;
    IntegratorKind kind = IntegratorKind::path_tracing;
    CancellationSource iteration_cancellation;
    SppmStatistics sppm;
    SppmProgress sppm_work;

    bool checkpoint(const CancellationToken& token) {
        if (token.requested()) {
            return false;
        }
        if (!pause_fast.load(std::memory_order_acquire)) {
            return true;
        }
        std::unique_lock lock(control);
        if (token.requested()) {
            return false;
        }
        if (pause_requested) {
            ++parked;
            if (parked == live) {
                state = RenderState::paused;
                changed.notify_all();
            }
            changed.wait(lock, [&] { return !pause_requested || token.requested(); });
            --parked;
        }
        return !token.requested();
    }
    void worker_finished() {
        std::lock_guard lock(control);
        --live;
        if (pause_requested && live != 0 && parked == live) {
            state = RenderState::paused;
        }
        changed.notify_all();
    }
    void fail(const std::string& message) {
        std::lock_guard lock(control);
        if (error.empty()) {
            error = message;
        }
        cancellation.request();
        iteration_cancellation.request();
        pause_requested = false;
        pause_fast.store(false, std::memory_order_release);
        state = RenderState::cancelling;
        changed.notify_all();
    }
    void complete(RenderState terminal) {
        std::lock_guard lock(control);
        state = terminal == RenderState::completed && cancellation.token().requested()
                    ? RenderState::cancelled
                    : terminal;
        finished = Clock::now();
        changed.notify_all();
    }

    // 暂停取消尚未提交的迭代；恢复从同一随机索引重算，Film 始终对应完整的统计状态。
    void run_sppm(std::shared_ptr<const SceneSnapshot> scene, RenderSettings render,
                  IntegratorSettings integrator, SessionSettings settings,
                  CancellationToken token) {
        SppmIntegrator renderer(std::move(scene), render, integrator, settings.threads, token);
        auto target = std::make_shared<Film>(render, false);
        {
            std::lock_guard lock(control);
            film = target;
            workers = renderer.statistics().workers;
            sppm = renderer.statistics();
            live = 1;
            if (!token.requested()) {
                state = RenderState::running;
            }
        }
        struct Lifetime {
            Impl& owner;
            ~Lifetime() {
                owner.worker_finished();
            }
        } lifetime{*this};
        while (renderer.statistics().iterations < render.samples_per_pixel) {
            if (!checkpoint(token)) {
                token.check();
            }
            CancellationToken iteration_token;
            {
                std::lock_guard lock(control);
                if (pause_requested) {
                    continue;
                }
                iteration_cancellation = CancellationSource();
                iteration_token = iteration_cancellation.token();
            }
            try {
                renderer.advance(iteration_token, [&](SppmProgress progress) {
                    std::lock_guard lock(control);
                    sppm_work = progress;
                });
            } catch (const OperationCancelled&) {
                token.check();
                continue;
            }
            token.check();
            auto image = std::make_shared<const Image>(renderer.image(token));
            target->publish_image(std::move(image),
                                  std::uint32_t(renderer.statistics().iterations));
            {
                std::lock_guard lock(control);
                sppm = renderer.statistics();
                statistics.paths = sppm.camera_paths;
                statistics.geometry = sppm.geometry;
                statistics.shadow_rays = sppm.shadow_rays;
                statistics.scattering_events =
                    sppm.camera_scattering_events + sppm.photon_scattering_events;
            }
        }
        {
            std::lock_guard lock(control);
            if (!token.requested()) {
                state = RenderState::finalizing;
            }
        }
        auto image = target->finish(token);
        {
            std::lock_guard lock(control);
            result = std::move(image);
        }
    }

    void run(std::shared_ptr<const SceneSnapshot> scene, RenderSettings render,
             IntegratorSettings integrator, SessionSettings settings,
             CancellationToken token) noexcept {
        try {
            // 上一任务的大缓冲由 controller 释放，start 不在 UI 线程执行大块析构。
            std::shared_ptr<Film> retired;
            std::shared_ptr<const Image> retired_image;
            {
                std::lock_guard lock(control);
                retired = std::move(retired_film);
                retired_image = std::move(retired_result);
            }
            retired.reset();
            retired_image.reset();
            if (integrator.kind == IntegratorKind::sppm) {
                run_sppm(std::move(scene), render, integrator, settings, token);
                complete(RenderState::completed);
                return;
            }
            {
                const PathTracer tracer(std::move(scene), integrator, token);
                token.check();
                auto target = std::make_shared<Film>(render);
                const auto columns = (render.width + settings.tile_size - 1) / settings.tile_size;
                const auto rows = (render.height + settings.tile_size - 1) / settings.tile_size;
                const auto tile_count = columns * rows;
                const auto hardware = std::max(1U, std::thread::hardware_concurrency());
                const auto automatic = hardware > 1 ? hardware - 1 : 1;
                const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(
                    settings.threads ? settings.threads : std::min(automatic, 32U), tile_count));
                {
                    std::lock_guard lock(control);
                    film = target;
                    workers = live = count;
                    if (!token.requested()) {
                        state = RenderState::running;
                    }
                }
                token.check();
                std::atomic<std::size_t> next_tile{0};
                // M3 的缓存行为 128 字节；统计写入不与相邻 worker 共用缓存行。
                struct alignas(128) WorkerStatistics {
                    PathStatistics value;
                };
                std::vector<WorkerStatistics> local_statistics(count);
                std::vector<std::thread> threads;
                threads.reserve(count);
                const auto work = [&](std::uint32_t worker) {
                    try {
                        while (!token.requested()) {
                            const auto tile = next_tile.fetch_add(1, std::memory_order_relaxed);
                            if (tile >= tile_count) {
                                break;
                            }
                            const auto x = (tile % columns) * settings.tile_size;
                            const auto y = (tile / columns) * settings.tile_size;
                            const auto width =
                                std::min<std::size_t>(settings.tile_size, render.width - x);
                            const auto height =
                                std::min<std::size_t>(settings.tile_size, render.height - y);
                            std::vector<glm::dvec3> sums(width * height, glm::dvec3(0));
                            std::vector<std::uint32_t> counts(width * height, 0);
                            bool active = true;
                            for (std::uint64_t base = 0; base < render.samples_per_pixel && active;
                                 base += settings.batch_samples) {
                                const auto end = std::min<std::uint64_t>(
                                    render.samples_per_pixel, base + settings.batch_samples);
                                for (std::size_t pixel = 0; pixel < sums.size() && active;
                                     ++pixel) {
                                    for (auto sample = base; sample < end; ++sample) {
                                        if (!checkpoint(token)) {
                                            active = false;
                                            break;
                                        }
                                        try {
                                            sums[pixel] += tracer.sample_pixel(
                                                x + pixel % width, y + pixel / width, sample,
                                                render, &local_statistics[worker].value);
                                        } catch (const OperationCancelled&) {
                                            active = false;
                                            break;
                                        }
                                        ++counts[pixel];
                                    }
                                }
                                target->commit(x, y, width, height, sums, counts);
                            }
                        }
                    } catch (const std::exception& exception) {
                        fail(exception.what());
                    } catch (...) {
                        fail("Unknown render worker failure");
                    }
                    worker_finished();
                };
                try {
                    for (std::uint32_t worker = 0; worker < count; ++worker) {
                        threads.emplace_back(work, worker);
                    }
                } catch (...) {
                    fail("Cannot start render worker thread");
                    for (auto& thread : threads) {
                        thread.join();
                    }
                    throw;
                }
                for (auto& thread : threads) {
                    thread.join();
                }
                {
                    std::lock_guard lock(control);
                    for (const auto& value : local_statistics) {
                        add_statistics(statistics, value.value);
                    }
                }
                token.check();
                {
                    std::lock_guard lock(control);
                    if (!token.requested()) {
                        state = RenderState::finalizing;
                    }
                }
                auto final_image = target->finish(token);
                {
                    std::lock_guard lock(control);
                    result = std::move(final_image);
                }
            } // 加速结构和 worker 资源先销毁，再发布 terminal 状态。
            complete(RenderState::completed);
        } catch (const OperationCancelled&) {
            std::lock_guard lock(control);
            state = error.empty() ? RenderState::cancelled : RenderState::failed;
            finished = Clock::now();
            changed.notify_all();
        } catch (const std::exception& exception) {
            fail(exception.what());
            complete(RenderState::failed);
        } catch (...) {
            fail("Unknown render controller failure");
            complete(RenderState::failed);
        }
    }
};

RenderSession::RenderSession() : impl_(std::make_unique<Impl>()) {}
RenderSession::~RenderSession() {
    cancel();
    wait();
}
void RenderSession::start(std::shared_ptr<const SceneSnapshot> scene, RenderSettings render,
                          IntegratorSettings integrator, SessionSettings settings) {
    if (!scene) {
        throw std::invalid_argument("Render session requires a scene snapshot");
    }
    render.validate();
    integrator.validate();
    settings.validate();
    {
        std::lock_guard lock(impl_->control);
        if (!render_terminal(impl_->state)) {
            throw std::logic_error("Render session is still active");
        }
    }
    wait();
    CancellationSource cancellation;
    {
        std::lock_guard lock(impl_->control);
        if (impl_->film) {
            impl_->retired_film = std::move(impl_->film);
        }
        if (impl_->result) {
            impl_->retired_result = std::move(impl_->result);
        }
        impl_->state = RenderState::preparing;
        ++impl_->job;
        impl_->total =
            static_cast<std::uint64_t>(render.width) * render.height * render.samples_per_pixel;
        impl_->workers = impl_->live = impl_->parked = 0;
        impl_->pause_requested = false;
        impl_->pause_fast.store(false, std::memory_order_release);
        impl_->error.clear();
        impl_->statistics = {};
        impl_->kind = integrator.kind;
        impl_->sppm = {};
        impl_->sppm_work = {};
        impl_->iteration_cancellation = CancellationSource();
        impl_->started = Clock::now();
        impl_->finished = {};
        impl_->cancellation = std::move(cancellation);
    }
    try {
        impl_->controller = std::thread(&Impl::run, impl_.get(), std::move(scene), render,
                                        integrator, settings, impl_->cancellation.token());
    } catch (...) {
        impl_->fail("Cannot start render controller thread");
        impl_->complete(RenderState::failed);
        throw;
    }
}
bool RenderSession::pause() {
    std::lock_guard lock(impl_->control);
    if (impl_->state != RenderState::running) {
        return false;
    }
    impl_->pause_requested = true;
    impl_->pause_fast.store(true, std::memory_order_release);
    if (impl_->kind == IntegratorKind::sppm) {
        impl_->iteration_cancellation.request();
    }
    impl_->state = impl_->live != 0 && impl_->parked == impl_->live ? RenderState::paused
                                                                    : RenderState::pausing;
    return true;
}
bool RenderSession::resume() {
    std::lock_guard lock(impl_->control);
    if (impl_->state != RenderState::paused && impl_->state != RenderState::pausing) {
        return false;
    }
    impl_->pause_requested = false;
    impl_->pause_fast.store(false, std::memory_order_release);
    impl_->state = RenderState::running;
    impl_->changed.notify_all();
    return true;
}
void RenderSession::cancel() {
    std::lock_guard lock(impl_->control);
    if (render_terminal(impl_->state)) {
        return;
    }
    impl_->cancellation.request();
    impl_->iteration_cancellation.request();
    impl_->pause_requested = false;
    impl_->pause_fast.store(false, std::memory_order_release);
    impl_->state = RenderState::cancelling;
    impl_->changed.notify_all();
}
void RenderSession::wait() {
    if (impl_->controller.joinable()) {
        impl_->controller.join();
    }
}
bool RenderSession::wait_for(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(impl_->control);
    return impl_->changed.wait_for(lock, timeout, [&] { return render_terminal(impl_->state); });
}
RenderProgress RenderSession::progress() const {
    std::lock_guard lock(impl_->control);
    const auto end = render_terminal(impl_->state) ? impl_->finished : Clock::now();
    // 先读像素计数，再读样本计数，与 Film 的 release 写入次序对应。
    const auto pixels = impl_->film ? impl_->film->completed_pixels() : 0;
    const auto samples = impl_->film ? impl_->film->recorded() : 0;
    return {impl_->state,
            impl_->job,
            samples,
            impl_->total,
            pixels,
            impl_->workers,
            impl_->state == RenderState::idle
                ? 0
                : std::chrono::duration<double>(end - impl_->started).count(),
            impl_->error,
            impl_->statistics,
            impl_->kind,
            impl_->sppm,
            impl_->sppm_work};
}
RenderPreview RenderSession::preview(std::size_t maximum_dimension) const {
    std::shared_ptr<Film> film;
    std::uint64_t job;
    {
        std::lock_guard lock(impl_->control);
        film = impl_->film;
        job = impl_->job;
    }
    auto preview = film ? film->preview(maximum_dimension) : RenderPreview{};
    preview.job_id = job;
    return preview;
}
std::shared_ptr<const Image> RenderSession::result() const {
    std::lock_guard lock(impl_->control);
    return impl_->state == RenderState::completed ? impl_->result : nullptr;
}

} // namespace minirender
