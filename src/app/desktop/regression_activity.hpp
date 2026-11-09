#pragma once

// macOS 回归期间保留 NSProcessInfo activity，避免 App Nap 合并后台定时器和采样线程的唤醒。
// 活动生命周期限定于自动回归；隐藏窗口运行，允许系统与显示器按用户设置休眠。
class RegressionActivity final {
  public:
    explicit RegressionActivity(bool enabled);
    ~RegressionActivity();
    RegressionActivity(const RegressionActivity&) = delete;
    RegressionActivity& operator=(const RegressionActivity&) = delete;

  private:
    void* token_ = nullptr;
};
