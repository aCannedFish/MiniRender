// 线性纹理 mip 链与过滤采样。

#pragma once

#include <glm/glm.hpp>
#include <minirender/core/cancellation.hpp>
#include <minirender/core/image.hpp>

#include <string>
#include <vector>

namespace minirender {

enum class TextureFilter { nearest, bilinear, trilinear };
// 返回过滤模式的序列化名称。
std::string texture_filter_name(TextureFilter filter);
// 解析过滤模式名称，拒绝未知名称。
TextureFilter parse_texture_filter(const std::string& name);

// 不可变线性 RGB mip 链。输入图像原点在左上；sample 接收 OBJ 的左下原点 UV。
// Repeat 寻址允许负 UV；所有过滤在解码后的线性空间执行。
class Texture {
  public:
    // 生成 mip 时逐行检查取消；默认 token 保持普通资产构建行为。
    explicit Texture(Image image, CancellationToken cancellation = {});
    // 返回指定 mip 层的只读图像。
    const Image& level(std::size_t index) const;
    // 返回 mip 链的层数，包含原始分辨率。
    std::size_t level_count() const noexcept;
    // 按 Repeat 寻址与指定过滤方式读取线性 RGB；lod 用于选择 mip 层。
    glm::vec3 sample(glm::vec2 uv, float lod = 0,
                     TextureFilter filter = TextureFilter::trilinear) const;

  private:
    // 在单个 mip 层进行环绕寻址与双线性插值。
    glm::vec3 bilinear(glm::vec2 uv, std::size_t level) const;
    // 由原图逐层降采样的线性图像，构造后只读。
    std::vector<Image> levels_;
};

} // namespace minirender
