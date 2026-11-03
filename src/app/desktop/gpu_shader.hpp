#pragma once

#include <vulkan/vulkan.h>

// 读取嵌入的 SPIR-V，按 uint32 对齐后创建 shader module；只需存活到 pipeline 创建结束。
class GpuShader {
  public:
    GpuShader(VkDevice device, const char* resource);
    ~GpuShader();
    GpuShader(const GpuShader&) = delete;
    GpuShader& operator=(const GpuShader&) = delete;
    VkShaderModule handle() const noexcept;

  private:
    VkDevice device_;
    VkShaderModule module_ = VK_NULL_HANDLE;
};
