// SPIR-V 资源加载、着色器模块与 Vulkan 错误处理。

#pragma once

#include <vulkan/vulkan.h>

// 读取嵌入的 SPIR-V，按 uint32 对齐后创建 shader module；只需存活到 pipeline 创建结束。
class GpuShader {
  public:
    GpuShader(VkDevice device, const char* resource);
    ~GpuShader();
    GpuShader(const GpuShader&)            = delete;
    GpuShader& operator=(const GpuShader&) = delete;
    // 返回借用的 Vulkan 句柄，所有权与销毁责任仍由本对象保留。
    VkShaderModule handle() const noexcept;

  private:
    // 借用的逻辑设备句柄，所有资源必须在设备销毁前释放。
    VkDevice device_;
    // 独占的 SPIR-V 着色器模块句柄。
    VkShaderModule module_ = VK_NULL_HANDLE;
};
