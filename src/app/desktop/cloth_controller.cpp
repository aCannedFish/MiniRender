#include "cloth_controller.hpp"

#include <QtConcurrent/QtConcurrentRun>

namespace {
bool same_transform(const minirender::Transform& a, const minirender::Transform& b) {
    return a.translation == b.translation && a.rotation_degrees == b.rotation_degrees &&
           a.scale == b.scale;
}
bool same_source(const ClothRuntimeContext& a, const ClothRuntimeContext& b) {
    return a.object_id == b.object_id && a.document_generation == b.document_generation &&
           a.component == b.component && same_transform(a.transform, b.transform) &&
           a.environment == b.environment;
}
} // namespace

ClothController::ClothController(QObject* parent)
    : QObject(parent), progress_(std::make_shared<Progress>()) {
    watcher_ = new QFutureWatcher<std::shared_ptr<Result>>(this);
    connect(watcher_, &QFutureWatcher<std::shared_ptr<Result>>::finished, this,
            [this] { finished(); });
    timer_ = new QTimer(this);
    timer_->setInterval(16);
    connect(timer_, &QTimer::timeout, this, [this] {
        if (status_.playing && !status_.busy && context_) {
            launch({});
        }
    });
    timer_->start();
}
ClothController::~ClothController() {
    timer_->stop();
    on_frame = {};
    cancellation_.request();
    watcher_->disconnect(this);
    watcher_->future().waitForFinished();
}

void ClothController::bind(std::optional<ClothRuntimeContext> context) {
    if (context && context_ && same_source(*context, *context_)) {
        return;
    }
    if (!context && !context_) {
        return;
    }
    pause();
    context_ = std::move(context);
    simulation_.reset();
    status_.bound = context_.has_value();
    status_.failed = false;
    status_.message = context_ ? "布料已暂停" : "选择或创建可模拟的布料";
    status_.frame = context_ ? context_->component->state().frame : 0;
    status_.statistics = {};
    status_.effective_substeps = 0;
    status_.last_steps = {};
    status_.last_work_ms = 0;
    status_.statistics.time_seconds =
        context_ ? static_cast<double>(status_.frame) *
                       context_->component->definition()->parameters.frame_seconds
                 : 0;
}
void ClothController::play() {
    if (!context_ || status_.failed) {
        return;
    }
    status_.playing = true;
    status_.message = "正在模拟";
    if (!status_.busy) {
        launch({});
    }
}
void ClothController::pause() {
    status_.playing = false;
    pending_.reset();
    ++epoch_;
    if (status_.busy) {
        cancellation_.request();
    }
    status_.message = status_.busy ? "正在暂停" : "布料已暂停";
}
void ClothController::step() {
    request({});
}
void ClothController::reset() {
    request({Operation::reset, {}, {}, true});
}
void ClothController::configure(std::shared_ptr<const minirender::ClothDefinition> definition,
                                minirender::ClothEnvironment environment, bool reset_geometry) {
    request({Operation::configure, std::move(definition), std::move(environment), reset_geometry});
}
void ClothController::request(Request value) {
    if (!context_) {
        return;
    }
    pause();
    status_.failed = false;
    if (status_.busy) {
        pending_ = std::move(value);
    } else {
        launch(std::move(value));
    }
}

ClothRuntimeStatus ClothController::status() const {
    auto result = status_;
    std::lock_guard lock(progress_->mutex);
    result.stage = progress_->stage;
    result.completed = progress_->completed;
    result.total = progress_->total;
    return result;
}

void ClothController::launch(Request request) {
    if (!context_ || status_.busy) {
        return;
    }
    const auto source = *context_;
    auto simulation = std::move(simulation_);
    cancellation_ = minirender::CancellationSource();
    const auto cancellation = cancellation_.token();
    const auto progress = progress_;
    status_.busy = true;
    job_epoch_ = epoch_;
    elapsed_.start();
    {
        std::lock_guard lock(progress->mutex);
        progress->stage = "准备模拟";
        progress->completed = progress->total = 0;
    }
    watcher_->setFuture(QtConcurrent::run([source, simulation = std::move(simulation),
                                           request = std::move(request), cancellation,
                                           progress]() mutable {
        auto result = std::make_shared<Result>();
        result->value.source = source;
        result->simulation = std::move(simulation);
        const auto report = [&](QString stage, std::size_t completed = 0, std::size_t total = 0) {
            cancellation.check();
            std::lock_guard lock(progress->mutex);
            progress->stage = std::move(stage);
            progress->completed = completed;
            progress->total = total;
        };
        try {
            auto component = source.component;
            auto environment = source.environment;
            if (request.operation == Operation::configure) {
                if (request.reset_geometry) {
                    component = minirender::make_cloth_component(*request.definition, cancellation);
                } else {
                    component = std::make_shared<const minirender::ClothComponent>(
                        request.definition, component->state(), component->mesh(), cancellation);
                }
                environment = request.environment;
                result->simulation.reset();
            } else if (request.operation == Operation::reset) {
                component =
                    minirender::make_cloth_component(*component->definition(), cancellation);
                result->simulation.reset();
            }
            if (!result->simulation) {
                result->simulation = minirender::make_cloth_simulation(*component, source.transform,
                                                                       environment, cancellation);
            }
            result->environment = environment;
            if (request.operation == Operation::advance) {
                result->value.steps = result->simulation->advance(
                    1, cancellation, [&](std::size_t completed, std::size_t total) {
                        report("推进子步", completed, total);
                    });
            }
            report("准备网格");
            result->value.frame = minirender::prepare_cloth_frame(
                *result->simulation, component->definition(), source.transform, cancellation);
            const auto mesh = result->value.frame.component->mesh();
            result->value.validated.validate(mesh, cancellation);
            result->value.validated.validate_cloth_uv(mesh, cancellation);
            result->value.raster = minirender::prepare_raster_mesh(
                *mesh, cancellation, [&](std::size_t completed, std::size_t total) {
                    report("准备显示", completed, total);
                });
            result->value.history = request.operation != Operation::advance;
            cancellation.check();
        } catch (const minirender::OperationCancelled&) {
            result->cancelled = true;
        } catch (const std::exception& error) {
            result->error = QString::fromUtf8(error.what());
        }
        return result;
    }));
}

void ClothController::finished() {
    const auto result = watcher_->result();
    status_.busy = false;
    status_.last_work_ms = double(elapsed_.nsecsElapsed()) / 1e6;
    if (job_epoch_ == epoch_ && context_ && same_source(result->value.source, *context_) &&
        !result->cancelled && result->error.isEmpty()) {
        try {
            if (on_frame && on_frame(result->value)) {
                context_->component = result->value.frame.component;
                if (result->value.history) {
                    context_->environment = result->environment;
                }
                simulation_ = std::move(result->simulation);
                status_.frame = context_->component->state().frame;
                status_.effective_substeps = result->value.frame.effective_substeps;
                status_.statistics = result->value.frame.statistics;
                status_.last_steps = result->value.steps;
                status_.message = status_.playing ? "正在模拟" : "布料已暂停";
            } else {
                simulation_.reset();
                status_.playing = false;
                status_.message = "来源已变化，帧未应用";
            }
        } catch (const std::exception& error) {
            status_.failed = true;
            status_.playing = false;
            status_.message = QString::fromUtf8(error.what());
        }
    } else if (!result->error.isEmpty() && job_epoch_ == epoch_) {
        status_.failed = true;
        status_.playing = false;
        status_.message = result->error;
    } else {
        simulation_.reset();
        status_.message = "布料已暂停";
    }
    if (pending_ && context_) {
        auto next = std::move(*pending_);
        pending_.reset();
        launch(std::move(next));
    }
}
