# 方向光阴影

## 算法

每个方向光先从光源方向渲染场景深度，再在主相机着色时比较片段的光空间深度。阴影只乘到对应光源的直接光照，不改变环境项、自发光或几何形状。

`directional_shadow_matrix` 遍历可见实例的世界位置，建立包围盒，将八个角转换到光源视空间，拟合带余量的正交投影。矩阵使用 Vulkan 的 Z∈[0,1] 约定，并进行一次 Y 翻转。投影不依赖主相机，避免仅移动相机就改变阴影范围。

ShadowMap 分配八层 1024×1024 D32 深度图，最多对应八个方向光。每层有独立视图与 framebuffer，共享数组视图供着色器读取。SceneRenderer 在相机 pass 前按光源录制深度 pass。

## 过滤与偏移

片段着色器读取邻近 3×3 个深度值，对每个采样点先进行深度比较，再平均比较结果，即 PCF。它过滤阴影边缘的采样锯齿，不模拟有限面积光源的真实半影，也不包含 PCSS 或级联阴影。

光源 pass 使用常量和斜率 depth bias；主 pass 的比较再使用小的法线相关偏移，减少自遮挡。偏移以投影后的深度为单位，过大会让阴影与接触处脱离，因此没有将它用作掩盖投影错误的手段。

本机设备创建时没有启用 portability 的 mutableComparisonSamplers。实现使用普通深度采样器，在 GLSL 中显式执行比较，避免依赖该可选能力；没有禁用 Validation 检查。PCF 仍然是九个二值比较的平均。

## 资源与同步

首次使用先把整个深度数组转换为只读布局，包括没有光源使用的层，保证 descriptor 引用的子资源布局有效。每个 active layer 的 render pass 在深度附件布局写入，结束后恢复只读布局。外部依赖声明 fragment shader 读取与深度测试读写之间的访问关系。

窗口本身仍管理提交 fence。SceneRenderer 仅在前一帧完成后更新 uniform 和几何。ShadowMap 独占 image、内存、视图、采样器、render pass 和 framebuffer，禁用复制并使用析构释放。

## 验证

CPU 测试验证普通、垂直和水平方向光的投影都覆盖场景几何，投影后的深度处于 [0,1]，并检查改变主相机不会改变光源矩阵。

真实 GPU 检查在“相机”面板切换方向光阴影，比较打开/关闭时的图像。多光源测试额外将第一盏灯强度设为零，用 CPU 三角形阴影射线核对第二层深度图。只比较相机可见的地面点，并剔除遮挡边界附近样本，避免二值射线与 PCF 覆盖率产生不合理比较。

```sh
python3 scripts/verify_gpu.py build/macos-desktop-debug/src/app/desktop/minirender_desktop.app --scene scenes/studio.json --output outputs/a1.3
python3 scripts/verify_gpu.py build/macos-desktop-debug/src/app/desktop/minirender_desktop.app --scene scenes/studio-two-lights.json --output outputs/a1.3-multi
```

本机记录：第二盏灯 127/127 遮挡点、982/982 受光点与几何射线一致，74,860 像素因阴影变暗。单灯和多灯测试均为 0 个 Vulkan Validation 错误。

## 接收面偏移

位移曲面使用局部接收面平面修正 PCF 比较深度。片元着色器先在统一控制流中计算世界位置的屏幕导数，再将其变换到当前光源的阴影 UV 与深度空间。通过 UV 导数的 2×2 矩阵求出深度梯度 $g=(\partial z/\partial u,\partial z/\partial v)$，每个邻近采样点比较 $z+g\cdot\Delta uv-bias$。

这样，3×3 PCF 的各样本按接收面的局部深度比较，可消除斜面和粗细分曲面的细碎自阴影条纹。实现参考 Microsoft 的 [Per-Texel Depth Bias](https://learn.microsoft.com/en-us/windows/desktop/DxTechArts/cascaded-shadow-maps#calculating-a-per-texel-depth-bias-with-ddx-and-ddy-for-large-pcfs)。几何回归另用 CPU 遮挡射线检查直接受光曲面与地面阴影。

## 参考

PCF 原始论文：[Reeves、Salesin、Cook，Rendering Antialiased Shadows with Depth Maps，1987](https://graphics.pixar.com/library/ShadowMaps/paper.pdf)。本项目使用固定 3×3 核，不复现原论文全部采样策略。
