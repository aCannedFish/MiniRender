# 原色预览

日期：2026-11-15。增加独立的原色模式，使没有实时光源的场景也能清楚显示模型。应用启动及全部内置模板默认进入原色模式。操作与计算规则见[原色预览](../BASE_COLOR_PREVIEW.md)。

## 完成范围

视口上方与相机页提供同步的显示选择。原色按材质底色、纹理和布料经纬纱线颜色显示，深度测试保留表面遮挡。自发光面显示归一化颜色。可关闭的接触遮蔽沿用 SSAO 采样与滤波，将最大暗化限制为 35%。

原色直接做 sRGB 显示编码。方向光、阴影、IBL、曝光和 Reinhard 均不参与原色计算。场景实际光源和材质仍由离线任务正常使用。既有模式编号保持，原色追加为 6。

## 数值检查

隐藏窗口回归载入实际 Cornell Box，使用 CPU BVH 找到每个采样像素的最近可见表面，比较其材质颜色与 GPU 浮点附件。8 个对象参与对照，最大通道误差为 5.96046e-8。显示像素同时按 CPU sRGB 编码检查，误差不超过 2 个 8 位量化级。

带纹理场景覆盖三个对象，包括金属和布料。常规尺寸有 1253 个纹理探针，960×700 下有 886 个，最大线性通道误差均为 1.49012e-8。CPU 检查使用最近命中的物体，因此前后遮挡也参与对照。

Cornell 接触遮蔽使 164990 个像素的线性最大通道暗化超过 0.01。全图满足原色的 0.65–1 倍范围，暗化保持各通道比例。修改方向光、环境亮度、曝光和阴影开关后，原色 HDR 最大误差小于 1e-6。切回光照模式可以观察到光源修改。

切换模式与接触遮蔽没有上传网格或重建描述符。切换前后的独立 CPU 路径追踪结果逐像素相同。开启 FXAA 不改变线性预览数据。窄窗口中的视口模式选择仍可直接操作。

## 回归

CPU 核心 298/298 通过。Release 和 ASan/UBSan 分别通过原色专项及全部 14 个模板的 Vulkan 同步验证，错误数为 0。

既有场景、纹理过滤、SSAO、IBL、编辑器布局、连续相机拖拽、离线任务、介质、SSS 和光场专项均通过。旧光照专项显式选择光照模式，原色专项保留真实的启动默认值。

复验命令：

```sh
cmake --preset macos-desktop-release
cmake --build --preset macos-desktop-release --parallel 4
python3 scripts/verify_gpu.py build/macos-desktop-release/src/app/desktop/minirender_desktop.app --scene scenes/studio.json --preview-check --sync-validation --output outputs/base-color --timeout 160
```
