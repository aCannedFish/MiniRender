#pragma once

#include <atomic>
#include <exception>
#include <memory>
#include <utility>

namespace minirender {

// 工作线程以专用异常离开长循环；任务控制器将其映射为 cancelled，不当作算法失败。
class OperationCancelled final : public std::exception {
  public:
    const char* what() const noexcept override {
        return "Operation cancelled";
    }
};

// 轻量只读取消标记。默认 token 不会取消，复制 token 共享同一原子状态。
// 取消状态通过原子读写传播，沿用项目现有工具链。
class CancellationToken {
  public:
    CancellationToken() = default;
    bool requested() const noexcept {
        return state_ && state_->load(std::memory_order_acquire);
    }
    void check() const {
        if (requested()) {
            throw OperationCancelled();
        }
    }

  private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<std::atomic<bool>> state)
        : state_(std::move(state)) {}
    std::shared_ptr<std::atomic<bool>> state_;
};

// 每个任务创建新 source，旧 token 的状态不会被下一任务复用或重置。
class CancellationSource {
  public:
    CancellationSource() : state_(std::make_shared<std::atomic<bool>>(false)) {}
    CancellationToken token() const {
        return CancellationToken(state_);
    }
    void request() noexcept {
        state_->store(true, std::memory_order_release);
    }

  private:
    std::shared_ptr<std::atomic<bool>> state_;
};

} // namespace minirender
