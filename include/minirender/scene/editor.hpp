#pragma once

#include <minirender/scene/scene.hpp>

#include <functional>
#include <optional>

namespace minirender {

// 编辑事务在文档副本上执行；验证成功后一次性替换，失败不改变当前场景。
// 历史共享不可变网格，只拷贝参数。UI 和测试使用同一套撤销/重做逻辑。
class SceneEditor {
  public:
    explicit SceneEditor(SceneDocument document);
    const SceneDocument& document() const noexcept;
    void replace(SceneDocument document);
    void edit(const std::function<void(SceneDocument&)>& operation);
    bool undo();
    bool redo();
    bool can_undo() const noexcept;
    bool can_redo() const noexcept;
    void update_navigation(const Camera& camera);

  private:
    SceneDocument document_;
    std::vector<SceneDocument> undo_;
    std::vector<SceneDocument> redo_;
};

// 角度增量以弧度表示。围绕 target 环绕或在 position 原地转向，限制 pitch 防止翻转。
Camera rotate_camera(Camera camera, float yaw_delta, float pitch_delta, bool orbit);
Camera pan_camera(Camera camera, glm::vec2 displacement);
Camera dolly_camera(Camera camera, float distance_factor);
Camera move_camera(Camera camera, glm::vec3 local_displacement);

// A1 的交互拾取使用世界空间暴力三角形求交；A2 的 BVH 将复用同一射线约定。
std::optional<std::string> pick_object(const SceneSnapshot& scene, const Ray& ray);

} // namespace minirender
