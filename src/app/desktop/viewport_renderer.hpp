#pragma once

#include <minirender/core/image.hpp>
#include <stdexcept>

// 视口中的渲染算法只管理自身资源，窗口控制设备、交换链、帧 fence 和呈现。
// 诊断、实时场景和离线显示均在 startNextFrame 中录制并提交一帧。
class ViewportRenderer {
  public:
    virtual ~ViewportRenderer() = default;
    virtual void initResources() = 0;
    virtual void initSwapChainResources() = 0;
    virtual void releaseSwapChainResources() = 0;
    virtual void releaseResources() = 0;
    virtual void startNextFrame() = 0;
    virtual void request_hdr_capture() {
        throw std::logic_error("This renderer has no HDR output");
    }
    virtual minirender::Image read_hdr_capture() {
        throw std::logic_error("This renderer has no HDR output");
    }
};
