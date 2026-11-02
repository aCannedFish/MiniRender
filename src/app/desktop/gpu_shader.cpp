#include "gpu_shader.hpp"
#include "gpu_buffer.hpp"

#include <QFile>

#include <cstring>
#include <stdexcept>
#include <vector>

GpuShader::GpuShader(VkDevice device, const char* resource) : device_(device) {
    QFile file(QString::fromUtf8(resource));
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("Cannot open embedded SPIR-V");
    }
    const auto bytes = file.readAll();
    if (bytes.isEmpty() || bytes.size() % 4 != 0) {
        throw std::runtime_error("Invalid SPIR-V size");
    }
    std::vector<std::uint32_t> words(static_cast<std::size_t>(bytes.size()) / 4);
    std::memcpy(words.data(), bytes.constData(), static_cast<std::size_t>(bytes.size()));
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = static_cast<std::size_t>(bytes.size());
    info.pCode = words.data();
    check_vk(vkCreateShaderModule(device_, &info, nullptr, &module_), "create shader module");
}
GpuShader::~GpuShader() {
    vkDestroyShaderModule(device_, module_, nullptr);
}
VkShaderModule GpuShader::handle() const noexcept {
    return module_;
}
