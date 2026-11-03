# 场景视口

## 运行

构建 desktop preset 后，macOS 生成应用包，可从 Finder 打开，或执行：

```sh
open build/macos-desktop-debug/src/app/desktop/minirender_desktop.app
```

应用旁附带 studio 示例，不依赖源码目录。通过“打开”加载其他 JSON 场景，或用“导入 OBJ”添加模型。窗口左侧显示物体与网格资源，右侧分别编辑物体、灯光和相机。

鼠标左键单击拾取物体，左键拖动环绕，右键拖动原地转向，中键或 Shift+左键拖动平移，滚轮调整到目标点的距离。视口获得焦点后，W/S 前后移动、A/D 左右移动、Q/E 上下移动，Shift 加速。

“相机”面板可以调整位置、目标、FOV、曝光，并切换直接光照和法线视图。“物体”面板编辑变换和材质参数，名称/数值输入完成时提交事务。撤销、重做、复制和删除均通过核心编辑器实现。

## 数据流

Workbench 只读写 SceneDocument；编辑事务先在副本上修改并验证，通过后再发布新的 SceneSnapshot。GPU 渲染器不读取控件。连续相机导航只更新相机，不反复上传网格。

SceneRenderer 将面角展开为局部空间顶点，并按不可变 MeshData 缓存 GPU 缓冲。同一网格的多个物体复用这些顶点。物体变换、法线逆转置、底色、自发光、粗糙度和过滤方式写入独立的 std430 SSBO，每次 draw 用实例索引读取。GpuBuffer 负责主机可见缓冲的分配和释放，更新在前一帧 fence 完成之后进行。

SceneEditor 缓存不可变网格的校验结果；参数与资产引用仍逐次校验。普通材质、灯光、变换和相机编辑不重新上传网格，纹理绑定、实例数量或缓冲容量变化时才重建描述符。修复过程与性能对照见 [参数响应](checkpoints/UI2.md)。

FrameUniform 按 std140 排列，包含 view-projection、相机位置、环境项、最多 8 个方向光和显示参数。代码用静态断言检查关键偏移和整体大小，避免 CPU/GLSL 对齐不一致。

## 光照模型

当前实时预览采用 GGX 分布、分离的 Smith G1 遮蔽乘积和 Schlick Fresnel。roughness 映射到 alpha = roughness²，预览中设置最小 roughness 避免零宽度高光。

漫反射和导体分别使用不同的 metallic 参数。均匀环境项暂时提供简单填充光；它不等同于完整 IBL。理想镜面和玻璃明确标记为预览近似，不提供真实镜面反射或折射。

方向光阴影已在 A1.3 加入，见 SHADOWS.md。A1.4 将光照改为浮点 HDR 输出，并把曝光、Reinhard 和 FXAA 移到独立显示 pass，见 HDR.md。纹理已在 A1.5 加入，见 TEXTURES.md；线框仍待后续实现。

模型参考 [Walter 等，2007](https://www.cs.cornell.edu/~srm/publications/EGSR07-btdf.pdf) 的 GGX/Smith 推导，以及 [PBRT 微表面章节](https://www.pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory)。本项目预览使用分离的 G1 乘积，未采用 PBRT 的相关高度 G 实现，也未实现该论文中的粗糙透射。

## 阅读顺序

1. 核心 `SceneEditor`：编辑事务、撤销/重做、相机操作和拾取。
2. `Workbench`：控件绑定、资源/物体列表、场景打开与保存。
3. `VulkanViewport`：事件调度、相机输入、场景版本和渲染器生命周期。
4. `SceneRenderer`：GPU 数据上传、uniform、管线及 draw。
5. `scene.vert` / `scene.frag`：坐标、法线、BRDF 和显示变换。
6. `scene_smoke.cpp`：通过真实控件回调及 GPU 图像检查编辑链路。

## 验证

```sh
python3 scripts/verify_gpu.py build/macos-desktop-debug/src/app/desktop/minirender_desktop.app --scene scenes/studio.json --output outputs/a1.2
```

测试保存 baseline、edited、undo、redo、reloaded、normals、dolly 和 final 图像，检查物体编辑改变像素，撤销/重做及保存重载恢复对应图像。它还发送 Qt 输入事件验证滚轮和键盘相机，并执行复制/删除动作。程序关闭后仍要求 Validation 错误数为 0。

这是程序内控件/渲染端到端测试。外部桌面控制工具未获 MiniRender 访问权限，本次没有完成外部鼠标操作的独立复核；不把它记录为已通过。

属性页支持滚动和窄窗口换行，工具栏“恢复布局”可以重新显示已关闭的侧栏。相机拖拽状态与窗口/GPU 调度修复详见 checkpoints/UI1.md。

打开场景和导入 OBJ/纹理在后台解析，状态栏显示当前阶段及忙碌条。预览等待超过 150ms 时显示更新提示，提交对应帧的 GPU fence 完成后收起。当前实时预览没有可计量的采样总数，忙碌条不显示百分比；A2 离线渲染再使用已完成样本数报告进度。
