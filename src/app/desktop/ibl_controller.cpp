#include "ibl_controller.hpp"

#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

namespace {
bool same_owner(const std::shared_ptr<const minirender::Texture>& first,
                const std::shared_ptr<const minirender::Texture>& second) {
    return !first.owner_before(second) && !second.owner_before(first);
}
} // namespace

IblController::IblController(VulkanViewport& viewport, QObject* parent)
    : QObject(parent), viewport_(viewport) {
    auto* timer = new QTimer(this);
    timer->setInterval(50);
    connect(timer, &QTimer::timeout, this, [this] { poll(); });
    timer->start();
}
IblController::~IblController() {
    cancellation_.request();
    if (watcher_) {
        disconnect(watcher_, nullptr, this, nullptr);
        watcher_->waitForFinished();
    }
}
std::shared_ptr<const minirender::IblEnvironment>
IblController::cached(const std::shared_ptr<const minirender::Texture>& source) {
    std::erase_if(cache_, [](const auto& entry) { return entry.source.expired(); });
    if (!source) {
        return nullptr;
    }
    for (auto& entry : cache_) {
        if (same_owner(entry.source.lock(), source)) {
            entry.used = ++clock_;
            ++status_.cache_hits;
            return entry.maps;
        }
    }
    return nullptr;
}
void IblController::cache(const Result& result) {
    if (!result.maps || !result.source) {
        return;
    }
    for (auto& entry : cache_) {
        if (same_owner(entry.source.lock(), result.source)) {
            entry.maps = result.maps;
            entry.used = ++clock_;
            return;
        }
    }
    cache_.push_back({result.source, result.maps, ++clock_});
    if (cache_.size() > 2) {
        const auto oldest =
            std::min_element(cache_.begin(), cache_.end(),
                             [](const auto& a, const auto& b) { return a.used < b.used; });
        cache_.erase(oldest);
    }
}
void IblController::poll() {
    if (!viewport_.scene()) {
        return;
    }
    const auto current = viewport_.scene()->environment_texture;
    if (!initialized_ || !same_owner(source_, current)) {
        cancellation_.request();
        ++generation_;
        initialized_ = true;
        source_ = current;
        maps_ = cached(source_);
        cancelled_ = false;
        status_.error.clear();
    }
    const bool ready = lut_ && (!source_ || maps_);
    if (ready) {
        status_.state = IblTaskState::ready;
    } else if (watcher_) {
        status_.state = cancelled_ ? IblTaskState::cancelling : IblTaskState::preparing;
    } else if (cancelled_) {
        status_.state = IblTaskState::cancelled;
    } else if (!status_.error.isEmpty()) {
        status_.state = IblTaskState::failed;
    } else {
        launch();
    }
    viewport_.set_ibl_data(maps_, lut_);
}
void IblController::launch() {
    cancellation_ = minirender::CancellationSource();
    progress_ = std::make_shared<Progress>();
    status_.state = IblTaskState::preparing;
    ++status_.jobs_started;
    const bool bake_environment = source_ && !maps_;
    if (bake_environment) {
        ++status_.environment_started;
    }
    const auto generation = generation_;
    active_generation_ = generation;
    const auto token = cancellation_.token();
    const auto progress = progress_;
    const auto source = source_;
    const auto lut = lut_;
    watcher_ = new QFutureWatcher<Result>(this);
    auto* watcher = watcher_;
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, generation] {
        const auto result = watcher->result();
        watcher_ = nullptr;
        watcher->deleteLater();
        if (result.lut && !lut_) {
            lut_ = result.lut;
        }
        cache(result);
        const auto current = viewport_.scene() ? viewport_.scene()->environment_texture : nullptr;
        const bool applicable =
            generation == generation_ && same_owner(result.source, current) && !cancelled_;
        if (applicable) {
            if (result.maps) {
                maps_ = result.maps;
            }
            if (!result.cancelled) {
                status_.error = result.error;
            }
        }
        poll();
    });
    watcher->setFuture(QtConcurrent::run([source, lut, token, progress, bake_environment] {
        Result result;
        result.source = source;
        result.lut = lut;
        const auto callback = [progress](const minirender::IblProgress& value) {
            std::lock_guard lock(progress->mutex);
            progress->value = value;
        };
        try {
            if (!result.lut) {
                result.lut = minirender::bake_ibl_brdf({}, token, callback);
            }
            if (bake_environment) {
                result.maps = minirender::bake_ibl_environment(source, {}, token, callback);
            }
        } catch (const minirender::OperationCancelled&) {
            result.cancelled = true;
        } catch (const std::exception& error) {
            result.error = QString::fromUtf8(error.what());
        }
        return result;
    }));
}
void IblController::cancel() {
    if (status_.state != IblTaskState::preparing && status_.state != IblTaskState::cancelling) {
        return;
    }
    cancelled_ = true;
    cancellation_.request();
    poll();
}
void IblController::retry() {
    cancelled_ = false;
    status_.error.clear();
    if (!watcher_ && !maps_) {
        maps_ = cached(source_);
    }
    poll();
}
IblTaskStatus IblController::status() const {
    auto result = status_;
    if (watcher_ && active_generation_ != generation_) {
        result.waiting_for_previous = true;
        result.progress = {minirender::IblStage::source, 0, 0};
        return result;
    }
    if (progress_) {
        std::lock_guard lock(progress_->mutex);
        result.progress = progress_->value;
    }
    return result;
}
