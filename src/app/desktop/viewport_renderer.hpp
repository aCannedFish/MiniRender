// 视口渲染器的资源、尺寸和逐帧生命周期接口。

#pragma once

#include <minirender/core/image.hpp>
#include <stdexcept>

// 视口中的渲染算法只管理自身资源，窗口控制设备、交换链、帧 fence 和呈现。
// 诊断、实时场景和离线显示均在 startNextFrame 中录制并提交一帧。
class ViewportRenderer {
  public:
    virtual ~ViewportRenderer() = default;
    // 在设备建立后创建与窗口尺寸无关的 GPU 资源。
    virtual void initResources() = 0;
    // 按当前视口尺寸创建附件和相关管线，窗口缩放后重新调用。
    virtual void initSwapChainResources() = 0;
    // 销毁尺寸相关附件和管线；调用前 GPU 必须已结束使用。
    virtual void releaseSwapChainResources() = 0;
    // 释放设备级资源，必须早于所属 Vulkan 设备销毁。
    virtual void releaseResources() = 0;
    // 读取已发布状态并录制本帧命令，在帧完成后通知视口。
    virtual void startNextFrame() = 0;

    // 标记下一次渲染需要把线性图像复制到主机读回缓冲。
    virtual void request_hdr_capture() {
        throw std::logic_error("This renderer has no HDR output");
    }

    // 在提交完成后读取线性图像；调用方负责先等待 GPU 同步。
    virtual minirender::Image read_hdr_capture() {
        throw std::logic_error("This renderer has no HDR output");
    }
};
