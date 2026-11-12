#pragma once

#include <minirender/assets/lightfield_io.hpp>

namespace minirender::detail {
// 采集器在发布结果前绑定自有临时存储；文件索引和像素内容仍不可变。
struct LightFieldAccess {
    static std::shared_ptr<const LightFieldFile> retain(std::shared_ptr<const LightFieldFile> file,
                                                        std::shared_ptr<const void> lifetime) {
        auto result = std::shared_ptr<LightFieldFile>(new LightFieldFile(*file));
        result->lifetime_ = std::move(lifetime);
        return result;
    }
};
} // namespace minirender::detail
