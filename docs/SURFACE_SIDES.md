# 表面朝向

右侧“物体”页的材质区提供“双面渲染”。关闭后，物体只显示三角形绕序定义的正面；开启后，两侧都可见。开关影响引用该材质的全部物体，支持撤销、重做和场景保存。

## 场景设置

JSON v1 材质增加可选字段：

```json
"double_sided": false
```

省略时为 `true`，旧场景保留双面行为。三个 studio 示例明确将地面和实体方块设为单面。地面几何正面朝 +Y，两个方块底面仍与地面接触于 `y=0`。

从地面上方观察时，地面正常可见；从下方观察时，跳过地面背面，显示方块朝外的底面。零厚度的单面地面从下方不可见。需要地板底面的场景可以使用封闭网格表达厚度。

## 深度冲突

原场景的地面与方块底面共面。双面光栅化让这些三角形同时参与深度测试，浮点变换与插值误差导致像素在不同物体之间切换。单面地面为这个接触关系确定了可见表面，消除了截图视角中的深度竞争。

用户创建的双面共面表面仍会同时参与深度测试。叠加贴花需要明确的覆盖关系和专门的深度偏移策略。

## 实时管线

`SceneRenderer` 在初始化时创建双面、普通单面和镜像单面三种表面管线，绘制时按实例选择。单面使用 `VK_CULL_MODE_BACK_BIT`。当前正高度 viewport 与相机投影的 Y 翻转对应 `VK_FRONT_FACE_COUNTER_CLOCKWISE`；实例变换行列式为负时切换为 clockwise，保留原网格外侧。

方向光阴影使用同样的三种朝向管线。深度和法线附件由表面 pass 生成，隐藏的背面不会写入这些附件，也不会进入 SSAO 的几何输入。材质切换更新实例参数并选择现有管线，复用网格与纹理描述符。

线框由线段绘制，线段没有可用于背面剔除的三角形绕序。`RasterVertex` 因此为每个面角保存所属面的几何法线；三个角使用同一方向。`wire.vert` 用逆转置后的几何法线判断该面是否朝向相机，`wire.frag` 丢弃隐藏面的线条。着色法线仍独立保存，平滑法线不会改变边线可见性。

## CPU 管线

`SceneBvh` 在接受候选命中之前检查材质和几何朝向。剔除背面后继续遍历，较远的可见面仍可命中；最近命中与阴影 any-hit 采用同一规则。`pick_object` 独立执行相同的朝向判断，保证隐藏面不会被鼠标选中。

负行列式会翻转世界空间顶点的绕序。CPU 用行列式符号修正叉积，GPU 通过切换 `frontFace` 保持一致。面积光采样、PDF 和发射功率同时遵守单/双面设置。

## 后台测试

所有自动 GPU 回归默认隐藏 Qt 窗口，在独立 Vulkan 图像中运行实际渲染管线。测试不置顶、不激活窗口。日志检查隐藏策略，运行时检查窗口未激活、视口未暴露。人工检查窗口呈现使用显式的 `--onscreen` 参数。

```sh
cmake --preset macos-desktop-release
cmake --build --preset macos-desktop-release
ctest --preset macos-desktop-release --output-on-failure
python3 scripts/verify_gpu.py \
  build/macos-desktop-release/src/app/desktop/minirender_desktop.app \
  --scene scenes/studio.json --sides-check --sync-validation \
  --output outputs/sides/contact --timeout 120
```

`tests/sided_tests.cpp` 用解析三角形检查朝向、镜像、继续遍历、阴影、发光和文件兼容。`sides_smoke.cpp` 核对地面上下、贴近地面、镜像、双面切换、线框、历史和保存重开，并连续改变相机位置。GPU 颜色、法线、深度和线性 HDR 与 CPU 可见性对照。

## 阅读顺序

1. `scene.hpp` 与 `scene_io.cpp`：材质字段、默认值和文件格式。
2. `bvh.cpp`、`editor.cpp`、`light.cpp`：CPU 朝向、拾取与面积光。
3. `scene_renderer.cpp`：正面定义、镜像管线与 draw 选择。
4. `raster_mesh.cpp`、`wire.vert`、`wire.frag`：线框所属面的朝向。
5. `workbench.cpp`、`sided_tests.cpp`、`sides_smoke.cpp`：编辑和验证链路。

剔除和正面定义参考 [Vulkan 光栅化规范](https://docs.vulkan.org/spec/latest/chapters/primsrast.html)。面积光密度转换参考 [PBRT 面积光](https://pbr-book.org/4ed/Light_Sources/Area_Lights)。
