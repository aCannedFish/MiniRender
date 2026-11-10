#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace minirender::desktop {

// 资产与物体使用独立前缀，按最小可用序号生成稳定的文档内 ID。
template <typename Item>
std::string unique_id(const std::vector<Item>& values, const std::string& prefix) {
    for (unsigned index = 1;; ++index) {
        const auto id = prefix + std::to_string(index);
        if (std::none_of(values.begin(), values.end(),
                         [&](const Item& value) { return value.id == id; })) {
            return id;
        }
    }
}

} // namespace minirender::desktop
