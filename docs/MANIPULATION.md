# 物体操纵

视口支持直接移动和旋转选中物体。工具栏提供“选择”“移动”“旋转”，在视口中可按 V、T、R 切换。

## 使用方法

1. 点击场景树或视口中的物体，选中需要操作的对象。
2. 选择“移动”，拖动红、绿、蓝轴，分别沿世界 X、Y、Z 方向移动。拖动白色中心点可在屏幕平面内移动。
3. 选择“旋转”，拖动彩色圆环绕对应世界轴旋转。旋转中心为物体的局部原点。
4. 松开鼠标提交，按 Esc 取消本次拖拽。整次拖拽占用一条撤销记录。

位置与旋转数字会同步更新，可继续精确输入。原有左拖环绕、右拖转向、Shift+左拖/中拖平移和 WASDQE 相机导航继续使用；命中操纵器的左键拖拽交给物体操作。

工具只在实时视口显示。离线图像和线性 EXR 保持渲染内容；返回实时预览后恢复操纵器。窗口隐藏或失去激活时取消当前手势，释放鼠标捕获时提交最后一次有效预览。

## 坐标约定

场景变换使用

$$
M=T R_z R_y R_x S.
$$

移动约束使用世界轴，物体原有旋转和缩放保持不变。对轴方向 $a$，取包含该轴且尽量朝向相机的平面，将鼠标射线与平面的交点差 $\Delta p$ 投影到轴上：

$$
\Delta t=a(a^T\Delta p).
$$

中心点移动采用与视线垂直的平面。启动时固定相机、平面和原始变换，后续更新始终相对启动状态计算，避免逐帧累计浮点漂移。

旋转先将鼠标射线与圆环平面相交，得到起始方向 $u$ 和当前方向 $v$，计算

$$
\theta=\operatorname{atan2}\bigl(a^T(u\times v),u^Tv\bigr),\qquad
R'=R(a,\theta)R.
$$

每次鼠标更新将角度差折回 $[-\pi,\pi]$ 再累加，支持连续多圈。接近侧视的圆环按屏幕投影切向位移计算角度。约束射线临近平行或没有有效交点时，保留最后一次有效变换。

计算后将旋转矩阵提取为场景使用的 ZYX 欧拉角。奇异姿态采用 $z=0$ 的等价表示，显示角度可能切换分支，实际旋转矩阵连续。非均匀与负缩放单独保留。

## 命中与绘制

操纵器的显示与命中共用同一组投影线段，鼠标位置使用左上角为原点的逻辑像素。根据相机深度、视场角和视口高度计算世界半径，使操纵器保持约 85 个逻辑像素的尺寸。Qt 高 DPI 下按交换链实际尺寸绘制，线宽与命中范围仍按逻辑像素定义。

鼠标到线段的距离用于轴/环命中，交叉处参考相机深度选择靠前的部分。中心点使用独立圆形区域。渲染器将线段展开为带暗色边框的三角形，悬停和拖拽时突出显示对应轴。

覆盖层在色调映射后、交换链 renderpass 结束前绘制。HDR 读回发生在此之前，离线显示不绘制覆盖层。顶点颜色按显示 sRGB 指定，片元着色器先将其解码为线性值，再由 sRGB 附件编码保存，遵循 [Vulkan 颜色附件规则](https://docs.vulkan.org/spec/latest/chapters/framebuffer.html)。

## 历史事务

`SceneEditor::begin_transform_edit` 保存启动文档。`preview_transform` 校验并发布当前预览，复用不可变网格的校验缓存。`commit_transform_edit` 将整次操作压入一条历史；`cancel_transform_edit` 恢复启动状态，不占用历史。

普通编辑会先提交已进行的手势，再创建自己的事务。撤销优先取消未提交的预览，场景替换清除手势状态。相机导航更新同时写入手势初态，取消物体操作时保留最新相机。

`Workbench` 在每次鼠标更新中仅同步变换字段、不可变快照和渲染参数，保持网格/材质缓冲复用。释放时整理历史按钮，取消时恢复字段和视口。原生子窗口销毁前结束手势并断开回调，保证编辑器和窗口的生命周期一致。

## 代码位置

- `include/minirender/scene/gizmo.hpp`、`src/scene/gizmo.cpp`：投影、命中与约束拖拽。
- `include/minirender/scene/editor.hpp`、`src/scene/editor.cpp`：连续预览与一次历史事务。
- `src/app/desktop/gizmo_renderer.*`、`shaders/gizmo.*`：屏幕空间覆盖层。
- `src/app/desktop/vulkan_viewport.*`：工具模式、鼠标/键盘和捕获状态。
- `src/app/desktop/workbench.*`：选中物体、参数同步、场景预览与生命周期。
- `tests/gizmo_tests.cpp`、`tests/scene_tests.cpp`：解析运动、旋转组合、多圈、奇异姿态与历史。
- `src/app/desktop/gizmo_smoke.cpp`：真实 Qt 事件、GPU 读回、撤销、文件与离线回归。

## 运行验证

```sh
cmake --build --preset macos-core-release
build/macos-core-release/minirender_tests '[gizmo],[gesture]'
cmake --build --preset macos-desktop-release
python3 scripts/verify_gpu.py \
  build/macos-desktop-release/src/app/desktop/minirender_desktop.app \
  --scene scenes/studio.json --gizmo-check --sync-validation \
  --output outputs/ui3/sync --timeout 150
```

普通模式响应测试使用 `--timing-only`，与同步验证分别运行。密集场景可通过 `scripts/prepare_parameter_probe.py` 生成，再将其 JSON 路径传给相同命令。
