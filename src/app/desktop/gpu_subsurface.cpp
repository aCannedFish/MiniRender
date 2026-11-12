#include "gpu_subsurface.hpp"

#include <minirender/render/dipole.hpp>

void SubsurfacePreviewCache::begin_scene() {
    used_.clear();
}
glm::vec3 SubsurfacePreviewCache::color(const minirender::Material& material) {
    used_.insert(material.id);
    auto& entry = entries_[material.id];
    if (entry.source != material.subsurface || entry.ior != material.ior) {
        const minirender::DipoleProfile profile(*material.subsurface, material.ior);
        entry.source = material.subsurface;
        entry.ior = material.ior;
        entry.color = glm::vec3(profile.reflectance() * profile.external_transmission());
    }
    return entry.color;
}
void SubsurfacePreviewCache::end_scene() {
    std::erase_if(entries_, [this](const auto& item) { return !used_.contains(item.first); });
}
