#pragma once

#include <glm/glm.hpp>
#include <minirender/core/image.hpp>

#include <string>
#include <vector>

namespace minirender {

enum class TextureFilter { nearest, bilinear, trilinear };
std::string texture_filter_name(TextureFilter filter);
TextureFilter parse_texture_filter(const std::string& name);

// 不可变线性 RGB mip 链。输入图像原点在左上；sample 接收 OBJ 的左下原点 UV。
// Repeat 寻址允许负 UV；所有过滤在解码后的线性空间执行。
class Texture {
  public:
    explicit Texture(Image image);
    const Image& level(std::size_t index) const;
    std::size_t level_count() const noexcept;
    glm::vec3 sample(glm::vec2 uv, float lod = 0,
                     TextureFilter filter = TextureFilter::trilinear) const;

  private:
    glm::vec3 bilinear(glm::vec2 uv, std::size_t level) const;
    std::vector<Image> levels_;
};

} // namespace minirender
