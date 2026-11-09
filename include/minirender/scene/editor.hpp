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
    // 接收场景载入任务的已校验记录，替换成功后清空历史。
    void replace_prevalidated(SceneDocument document, const MeshValidationCache& validated);
    void edit(const std::function<void(SceneDocument&)>& operation);
    // 后台网格已校验后合并其所有权缓存，再执行普通事务；其他文档参数仍完整校验。
    void edit_prevalidated(const std::function<void(SceneDocument&)>& operation,
                           const MeshValidationCache& validated);
    // 连续物体操纵：预览更新当前文档，提交时整次手势进入一条历史，取消时恢复启动状态。
    // 普通 edit 会先提交已有手势；replace 清除手势，undo 优先取消尚未提交的变化。
    void begin_transform_edit(const std::string& object_id);
    void preview_transform(const Transform& transform);
    bool commit_transform_edit();
    bool cancel_transform_edit();
    bool transforming() const noexcept;
    bool undo();
    bool redo();
    bool can_undo() const noexcept;
    bool can_redo() const noexcept;
    void update_navigation(const Camera& camera);
    // 快照仍校验全部参数与引用，已验证的不可变网格不再逐三角形扫描。
    std::shared_ptr<const SceneSnapshot> snapshot();

  private:
    SceneDocument document_;
    std::vector<SceneDocument> undo_;
    std::vector<SceneDocument> redo_;
    MeshValidationCache validated_meshes_;
    struct TransformEdit {
        std::string object_id;
        SceneDocument initial;
        bool changed = false;
    };
    std::optional<TransformEdit> transform_edit_;
};

// 角度增量以弧度表示。围绕 target 环绕或在 position 原地转向，限制 pitch 防止翻转。
Camera rotate_camera(Camera camera, float yaw_delta, float pitch_delta, bool orbit);
Camera pan_camera(Camera camera, glm::vec2 displacement);
Camera dolly_camera(Camera camera, float distance_factor);
Camera move_camera(Camera camera, glm::vec3 local_displacement);

// A1 的交互拾取使用世界空间暴力三角形求交；A2 的 BVH 将复用同一射线约定。
std::optional<std::string> pick_object(const SceneSnapshot& scene, const Ray& ray);

} // namespace minirender
