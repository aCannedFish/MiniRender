# 纹理

## 数据与颜色

Texture 保存不可变的线性 RGB mip 链。PNG/JPEG 默认按 sRGB 解码，16 位 PNG 可声明为 linear；HDR/EXR 必须使用 linear。解码由固定提交的 stb 与既有 TinyEXR 承担，过滤和 mip 生成由项目实现。

Image 使用左上原点，OBJ UV 使用左下原点，采样时只翻转一次 V。寻址模式为 Repeat，允许负 UV。PNG 输出执行 sRGB 编码，不包含 tone mapping；HDR 数值可以继续通过 EXR 保存。

当前底色纹理使用 RGB，alpha 不参与透明度。源纹理保留有限 HDR/负数样本，反射底色在着色时限制到 [0,1]。数据纹理可以在线性编码下读取，位移等专门用途将在后续模块消费。

## Mipmap

先在线性空间生成 mip，再进行采样。降采样按照源像素与目标像素覆盖区域的面积加权，因此奇数尺寸最后一行/列也参与平均。测试用 3×5 图像检查最后一层均值，避免简单 2×2 循环漏掉边缘。

最近邻使用第 0 层，双线性插值第 0 层四个 texel，三线性在相邻两层的双线性结果之间插值。像素中心约定为：

\[
x = \operatorname{fract}(u)W-\tfrac12,
\qquad y = (1-\operatorname{fract}(v))H-\tfrac12.
\]

GPU 用 UV 的屏幕导数估计 footprint，取两个方向长度的最大值后求 log2 得到 LOD。CPU 接口显式接收 LOD，便于后续路径追踪使用射线微分或其他 footprint 估计。

## GPU 资源

所有 mip 层一次上传到 RGBA32F 图像，着色器显式实现最近邻、双线性和三线性采样。这样可以保留源数据精度，也不依赖 float32 硬件线性过滤的可选能力。

SceneRenderer 为各绘制实例绑定 descriptor set，不要求动态纹理数组索引功能。未绑定纹理时使用 1×1 白图。纹理按只读 Texture 的共享所有权缓存，变换或材质参数编辑不会重新上传相同纹理。旧 descriptor 回收后移除不用的 GPU 纹理；上传 staging 在对应帧 fence 完成后释放。

## 编辑与保存

选中物体后使用“导入纹理”，会把图像加入资源列表并绑定到该物体当前材质。“底色纹理”可以选择已有资源或“无”，“纹理过滤”提供三种采样方式。修改共享材质会影响所有引用它的物体。

读取、解码和 mip 生成通过 QtConcurrent 在工作线程执行，GPU 资源上传仍在渲染线程。场景在导入期间被替换时，旧任务结果不应用到新场景。导入、绑定和过滤参数均支持撤销/重做。

场景 JSON 新增可选的 textures 数组，以及材质的 base_color_texture、texture_filter。旧场景仍可加载。另存时保持相对资产引用；Windows 跨盘符时复制资产到目标场景旁，并保留文件扩展名。

## 验证

```sh
python3 scripts/verify_gpu.py build/macos-desktop-debug/src/app/desktop/minirender_desktop.app --scene scenes/studio.json --texture-check --output outputs/a1.5 --timeout 60
```

测试依次验证异步导入、绑定产生图像变化、undo/redo 与重载一致，再在顶视平面上对照 CPU/GPU 采样。GPU 值直接取自线性 HDR，避免色调映射和 FXAA 掩盖数值差异。

本机每种过滤比较 81 个点：最近邻最大误差 0，双线性约 1.33×10⁻⁶，三线性在 LOD≈0.557 下约 2.76×10⁻⁴。后者使用梯度与棋盘混合图，让相邻 mip 的结果不同，检查实际的层间插值。

测试资产均由程序生成：checker.png 为 RGB/白色棋盘，ramp.png 为渐变与高频棋盘混合，linear16.png 包含已知 16 位值，gray.jpg 为恒定灰色 JPEG，radiance.hdr 包含 (8,2,0.5)。没有使用外部摄影图片。

## 阅读顺序

1. core/texture：线性 mip 与过滤约定。
2. core/raster_io：文件解码、颜色转换和 PNG 输出。
3. scene / scene_io：材质引用、只读快照和相对路径。
4. GpuTexture / SceneRenderer：上传、缓存和逐实例绑定。
5. scene.frag：UV 方向、footprint 与采样。
6. Workbench / texture_smoke：异步导入及完整验证链路。

参考：[PBRT 图像纹理章节](https://pbr-book.org/4ed/Textures_and_Materials/Image_Texture)；文件 codec 使用 [stb 固定提交](https://github.com/nothings/stb/tree/d14d46f6b44456eb88beac58cba833a4135d4c89)，版本和 SHA256 见 cmake/Dependencies.cmake。
