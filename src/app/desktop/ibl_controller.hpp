#pragma once

#include "vulkan_viewport.hpp"
#include <QFutureWatcher>
#include <QObject>
#include <mutex>

enum class IblTaskState { idle, preparing, ready, cancelling, cancelled, failed };
struct IblTaskStatus {
    IblTaskState state = IblTaskState::idle;
    minirender::IblProgress progress{minirender::IblStage::source};
    QString error;
    std::uint64_t jobs_started = 0;
    std::uint64_t environment_started = 0;
    std::uint64_t cache_hits = 0;
    bool waiting_for_previous = false;
};

// GUI 线程拥有任务生命周期与小型 LRU；worker 只访问固定输入和自己的进度对象。
// 来源改变时取消旧工作，只保留最新请求。独立 BRDF 结果可跨来源/取消复用。
class IblController final : public QObject {
  public:
    IblController(VulkanViewport& viewport, QObject* parent = nullptr);
    ~IblController() override;
    void cancel();
    void retry();
    IblTaskStatus status() const;

  private:
    struct Progress {
        std::mutex mutex;
        minirender::IblProgress value{minirender::IblStage::source};
    };
    struct Result {
        std::shared_ptr<const minirender::Texture> source;
        std::shared_ptr<const minirender::IblEnvironment> maps;
        std::shared_ptr<const minirender::Image> lut;
        QString error;
        bool cancelled = false;
    };
    struct CacheEntry {
        std::weak_ptr<const minirender::Texture> source;
        std::shared_ptr<const minirender::IblEnvironment> maps;
        std::uint64_t used;
    };
    void poll();
    void launch();
    void cache(const Result& result);
    std::shared_ptr<const minirender::IblEnvironment>
    cached(const std::shared_ptr<const minirender::Texture>& source);
    VulkanViewport& viewport_;
    std::shared_ptr<const minirender::Texture> source_;
    std::shared_ptr<const minirender::IblEnvironment> maps_;
    std::shared_ptr<const minirender::Image> lut_;
    std::shared_ptr<Progress> progress_;
    QFutureWatcher<Result>* watcher_ = nullptr;
    minirender::CancellationSource cancellation_;
    std::vector<CacheEntry> cache_;
    std::uint64_t generation_ = 0, clock_ = 0;
    std::uint64_t active_generation_ = 0;
    IblTaskStatus status_;
    bool initialized_ = false, cancelled_ = false;
};
