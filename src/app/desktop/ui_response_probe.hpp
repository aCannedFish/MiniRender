#pragma once

#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QObject>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

// 回归辅助：独立线程投递事件，测量 GUI 排队时延。
// verify 排除显式参考计算和 GPU 读回，生产控件、任务发布与等待阶段保持计时。
class UiResponseProbe final : public QObject {
  public:
    explicit UiResponseProbe(QObject* parent, std::string label)
        : QObject(parent), label_(std::move(label)) {
        event_type_ = QEvent::registerEventType();
        worker_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                auto* event = new Event(event_type_);
                event->epoch = epoch_.load(std::memory_order_relaxed);
                event->sent = std::chrono::steady_clock::now();
                QCoreApplication::postEvent(this, event);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }
    template <class Work> void verify(Work work) {
        QCoreApplication::sendPostedEvents(this, event_type_);
        epoch_.fetch_add(1, std::memory_order_relaxed);
        work();
        epoch_.fetch_add(1, std::memory_order_relaxed);
    }
    void report(bool enforce) {
        QCoreApplication::sendPostedEvents(this, event_type_);
        qInfo() << label_.c_str() << "response samples" << samples_ << "maximum ms" << maximum_ms_;
        if (enforce && (samples_ < 10 || maximum_ms_ >= 500)) {
            throw std::runtime_error(label_ + " UI event latency failed the 500ms response check");
        }
    }

  protected:
    bool event(QEvent* event) override {
        if (event->type() != event_type_) {
            return QObject::event(event);
        }
        const auto& probe = *static_cast<Event*>(event);
        if (probe.epoch == epoch_.load(std::memory_order_relaxed)) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - probe.sent)
                                     .count();
            maximum_ms_ = std::max(maximum_ms_, elapsed);
            ++samples_;
        }
        return true;
    }

  private:
    struct Event final : QEvent {
        explicit Event(int type) : QEvent(static_cast<QEvent::Type>(type)) {}
        std::chrono::steady_clock::time_point sent;
        std::uint64_t epoch = 0;
    };
    std::string label_;
    int event_type_ = 0;
    std::atomic<std::uint64_t> epoch_{0};
    std::chrono::milliseconds::rep maximum_ms_ = 0;
    std::size_t samples_ = 0;
    // worker 最先销毁，QObject 清理前停止投递。
    std::jthread worker_;
};
