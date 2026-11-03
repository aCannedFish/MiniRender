# 离线界面

## 使用

打开场景后，在右侧属性面板选择“离线”。设置分辨率、每像素样本数、采样方式、散射深度、随机种子和工作线程，再点击“开始”。线程数为 0 时保留一个 CPU 核给界面和其他程序。

“暂停”在所有工作线程停稳后显示“已暂停”。“恢复”继续同一任务，不改变随机序列。“取消”收回线程并保留已记录的部分结果。准备场景和整理最终图像时显示忙碌条，采样时显示实际记录数对应的百分比。

取消勾选“查看离线结果”可返回实时预览。离线显示期间，相机拖拽和实时法线、深度、线框、FXAA、方向光阴影控件停用。相机页的光圈半径和对焦距离供薄透镜采样使用，距离单位为米。

## 场景快照

任务使用启动时的场景快照。渲染期间可修改场景并返回实时预览，后台任务仍计算原快照。场景、材质、灯光、几何、相机或离线参数修改后，面板提示结果过期。点击“开始”启动新任务后清除提示。

曝光属于显示设置。修改曝光、窗口尺寸或显示模式不会重启采样，也不改变线性累计结果。窗口尺寸改变时，图像等比例显示，剩余区域为黑色。

## 图像导出

- EXR 保存原始线性 RGB，不应用曝光或色调映射。
- PNG 应用点击导出时的曝光及 Reinhard 变换，再编码为 sRGB。
- 渲染、暂停、取消和完成后都可以导出已有样本。部分图像逐像素按实际记录数归一化，未计算像素为黑色。
- 中间结果复制和文件编码都在后台执行。导出期间暂时禁用新任务启动，确保导出过程对应同一个任务。

视口预览最长边限制为 1024，导出使用完整分辨率。预览上传到 Vulkan 的浮点纹理，显示变换由 shader 执行。CPU 原图和显示纹理分别存储，修改曝光不会改写原图。

## 代码结构

`RenderSession` 是无 Qt 依赖的后台任务核心，详见[任务说明](RENDER_SESSION.md)。`OfflinePanel` 持有任务，通过 200ms 定时器读取状态和不可变预览。渲染线程不访问 Qt 控件或 Vulkan。场景编辑和相机导航由 `Workbench` 通知面板，将当前结果标记为过期。

`VulkanViewport` 保留实时和离线两种实现，在上一帧 fence 完成后选择本帧录制者。切换显示不重建设备，也不丢弃实时网格缓存。交换链重建时，两种实现分别回收和重建自己的管线。

`OfflineRenderer` 只上传最新发布的图像。同尺寸更新复用纹理和 host-visible staging；尺寸改变才重新分配。上传命令先将纹理转为 transfer-destination，再转为 shader-read。资源写入和回收均发生在上一帧完成后。

`offline.frag` 使用最近像素显示，避免依赖 float32 硬件线性过滤。显示变换与 `display_image` 一致：

$$C_{display}=\frac{\max(C,0)}{\max(C,0)+2^{-EV}}.$$

该形式避免先将大 HDR 数值乘以曝光系数造成 float 溢出。sRGB 交换链完成最后一次编码。有关线性颜色和显示变换的背景见 [HDR](HDR.md)。

## 验证

```sh
cmake --build --preset macos-desktop-debug
python3 scripts/verify_gpu.py build/macos-desktop-debug/src/app/desktop/minirender_desktop.app --scene scenes/path-room.json --offline-check --output outputs/offline --timeout 65
```

回归通过真实按钮启动、暂停、恢复和取消任务，检查中间导出、完整导出、窗口缩放、过期提示及切回实时后的资源复用。它还将每个源像素映射到屏幕中的位置，对照 CPU 显示变换。

回归目录保留 `snapshot.json`、`result.exr` 和 `result.png`。在独立 CLI 中使用相同参数，检查完整输出一致性：

```sh
build/macos-core-release/minirender_cli --render outputs/offline/snapshot.json outputs/offline/cli --width 24 --height 16 --spp 8 --depth 6 --seed 19 --threads 3 --exposure 0.75
```

图像对照在独立进程中执行，避免参考渲染占用 Qt 事件线程。命令行和界面使用不同线程数，结果仍应一致。

## 阅读顺序

1. `offline_panel.hpp`：Qt 控件、核心任务和快照提供者的职责。
2. `OfflinePanel::start/poll`：固定输入、状态映射、实际进度及预览更新。
3. `OfflinePanel::save_image`：后台导出与任务所有权。
4. `VulkanViewport::render_frame`：fence 完成后的实现选择。
5. `OfflineRenderer::upload/startNextFrame`：缓冲复用、布局转换和等比例绘制。
6. `offline_smoke.cpp`：控件操作、像素回读、导出与响应检查。
