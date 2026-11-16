// 场景编辑事务、历史撤销重做与相机导航。

#pragma once

#include <minirender/scene/scene.hpp>

#include <functional>
#include <optional>

namespace minirender {

// 编辑事务在文档副本上执行；验证成功后一次性替换，失败不改变当前场景。
// 历史共享不可变网格，只拷贝参数。UI 和测试使用同一套撤销/重做逻辑。
class SceneEditor {
  public:
    // 校验初始文档，并建立空的撤销重做历史。
    explicit SceneEditor(SceneDocument document);
    // 返回当前文档的只读引用，引用随编辑器状态更新。
    const SceneDocument& document() const noexcept;
    // 校验并替换整个文档，清空历史和未完成的操纵事务。
    void replace(SceneDocument document);
    // 接收场景载入任务的已校验记录，替换成功后清空历史。
    void replace_prevalidated(SceneDocument document, const MeshValidationCache& validated);
    // 在副本上执行操作，验证成功后发布并保存撤销状态。
    void edit(const std::function<void(SceneDocument&)>& operation);
    // 后台网格已校验后合并其所有权缓存，再执行普通事务；其他文档参数仍完整校验。
    void edit_prevalidated(const std::function<void(SceneDocument&)>& operation,
                           const MeshValidationCache&                 validated);
    // 连续物体操纵：预览更新当前文档，提交时整次手势进入一条历史，取消时恢复启动状态。
    // 普通 edit 会先提交已有手势；replace 清除手势，undo 优先取消尚未提交的变化。
    void begin_transform_edit(const std::string& object_id);
    // 更新当前操纵事务的临时变换，不逐帧增加历史。
    void preview_transform(const Transform& transform);
    // 将已变化的操纵事务提交为一条历史，返回是否发生提交。
    bool commit_transform_edit();
    // 恢复操纵开始时的文档，返回是否存在可取消事务。
    bool cancel_transform_edit();
    // 查询是否有未结束的物体操纵事务。
    bool transforming() const noexcept;
    // 恢复上一条历史；已有未提交操纵时先取消该手势。
    bool undo();
    // 重放最近撤销的文档状态。
    bool redo();
    // 查询是否存在可撤销历史或未提交手势。
    bool can_undo() const noexcept;
    // 查询是否可以重做；当前存在已变化的操纵手势时不提供重做。
    bool can_redo() const noexcept;
    // 更新相机导航状态，避免每次拖拽产生普通属性编辑历史。
    void update_navigation(const Camera& camera);
    // 运行时完整帧不增加历史。来源组件与变换须仍相同，共享网格资产会先为目标拆分。
    bool update_cloth_frame(const std::string&                           object_id,
                            const std::shared_ptr<const ClothComponent>& expected,
                            const Transform&                             expected_transform,
                            std::shared_ptr<const ClothComponent>        frame,
                            const MeshValidationCache& validated = {}, bool history = false);
    // 快照仍校验全部参数与引用，已验证的不可变网格不再逐三角形扫描。
    std::shared_ptr<const SceneSnapshot> snapshot();

  private:
    // 当前已验证的场景状态。
    SceneDocument document_;
    // 可恢复的历史文档栈，共享不可变资产。
    std::vector<SceneDocument> undo_;
    // 最近撤销的文档栈，新编辑时清空。
    std::vector<SceneDocument> redo_;
    // 已通过校验的不可变网格所有权缓存。
    MeshValidationCache validated_meshes_;

    struct TransformEdit {
        std::string   object_id;
        SceneDocument initial;
        bool          changed = false;
    };

    // 当前一次操纵手势的起始状态和变化标记。
    std::optional<TransformEdit> transform_edit_;
};

// 角度增量以弧度表示。围绕 target 环绕或在 position 原地转向，限制 pitch 防止翻转。
Camera rotate_camera(Camera camera, float yaw_delta, float pitch_delta, bool orbit);
// 沿相机局部平面平移位置与目标。
Camera pan_camera(Camera camera, glm::vec2 displacement);
// 按比例调整相机到目标的距离。
Camera dolly_camera(Camera camera, float distance_factor);
// 将相机局部位移转换到世界空间并同步移动目标。
Camera move_camera(Camera camera, glm::vec3 local_displacement);

// 交互拾取使用世界空间三角形求交，返回最近可见物体的稳定标识。
std::optional<std::string> pick_object(const SceneSnapshot& scene, const Ray& ray);

} // namespace minirender
