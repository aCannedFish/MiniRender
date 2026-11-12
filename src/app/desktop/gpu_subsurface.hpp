#pragma once

#include <minirender/scene/scene.hpp>

#include <map>
#include <set>

// UI 线程的表面近似缓存。每个材质仅在光学参数或 IOR 改变时计算 dipole 反射率。
// 一次 scene upload 标记实际引用并回收旧项，避免保留已删除材质及旧参数。
class SubsurfacePreviewCache final {
  public:
    void begin_scene();
    glm::vec3 color(const minirender::Material& material);
    void end_scene();

  private:
    struct Entry {
        std::shared_ptr<const minirender::SubsurfaceParameters> source;
        float ior = 0;
        glm::vec3 color{0};
    };
    std::map<std::string, Entry> entries_;
    std::set<std::string> used_;
};
