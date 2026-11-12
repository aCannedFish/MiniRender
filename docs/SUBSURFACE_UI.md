# SSS 界面

在“物体”页选中对象，将材质设为“次表面散射（SSS）”。光学面板提供 RGB 吸收、约化散射和密度倍率，也可选择大理石或蜡预置。模型与公式见[次表面散射](SUBSURFACE.md)。

## 参数编辑

吸收和约化散射使用 $m^{-1}$。大理石预置来自测量系数，蜡预置使用项目自定的展示参数。折射率沿用材质页控件，范围为 1–3。

数值编辑先保存在草稿中。“应用光学参数”校验整组参数，并产生一次可撤销的场景编辑。“恢复草稿”取回当前场景值。“使用预置”同时更新光学参数和折射率，产生一次历史记录。

无关的灯光、相机或折射率编辑保留光学草稿。切换到其他材质或重新打开文件时，面板按材质和文档代数同步。未编辑的 double 分量保留完整精度，悬停可以查看完整数值。

非法参数在面板中显示原因，场景保留已提交值。草稿尚未应用时，面板的渲染入口停用。已有离线任务仍可查看。

## 纹理与预览

SSS 的离线颜色由光学系数决定。底色、底色纹理、纹理过滤和粗糙度控件在 SSS 下停用。带纹理的普通材质切换到 SSS 时，面板提示先移除纹理；已有纹理保留在场景中。选中 SSS 对象导入纹理时，纹理加入资源列表，面板说明该材质不使用底色纹理。

实时视口使用表面 PBR 近似：底色取 dipole 总反射率乘外侧半球透射率，预览粗糙度固定为 0.35。该颜色按材质缓存，光学参数或折射率改变时更新。相同材质的多个实例复用同一结果。

密度改变扩散距离，总反射率保持不变，因此实时近似底色保持不变。真实的跨位置扩散、透光边缘和材质内部的颜色变化通过离线渲染观察。

## 离线任务

点击“用 PT 开始离线渲染”会明确选择 Path Tracing，并使用离线页中的分辨率、样本数、深度和线程设置。已有任务或导出正在运行时，按钮显示“查看离线任务”，保留原任务和启动快照。

任务运行期间可以应用新的材质参数。当前图像会标记为过期，后台继续计算旧快照。任务结束后重新开始，使用新参数。查看按钮保留点击时的操作意图，任务刚结束时也不会误启动另一轮。

SSS 需要闭合、连通、朝外绕序的网格。拓扑校验在后台准备阶段执行，错误显示在离线页。进度、暂停、恢复、取消和图像导出沿用[离线界面](OFFLINE_UI.md)。

## 快速导入

工具栏新增四个可编辑模板：

| 场景 | 内容 | 推荐设置 |
| --- | --- | --- |
| SSS · 大理石球 | 10cm Cornell 房间、测量光学系数 | 320×320、128 spp、深度 8 |
| SSS · 蜡球 | 暖色蜡与可调密度 | 320×320、128 spp、深度 8 |
| SSS · 大理石侧面扩散 | 顶部方向光照射40mm方块 | 320×320、256 spp、深度 8 |
| SSS · 蜡侧面扩散 | 从顶部受光区域向侧面扩散 | 320×320、256 spp、深度 8 |

导入会覆盖当前场景并选中 SSS 对象，填入 PT/MIS 推荐参数。球场景使用法线视图便于编辑，方块场景可直接查看方向光下的表面近似。离线渲染由用户点击启动。保存使用用户路径，内置模板保留原值。

## 实现

`SubsurfaceMaterialPanel` 持有源参数和完整 double 草稿。每次输入只修改相应分量，显示刷新屏蔽信号。Workbench 将通过校验的参数作为新的不可变对象交给 SceneEditor，历史和旧任务保留各自版本。

`SubsurfacePreviewCache` 按材质 ID、参数共享对象和 IOR 缓存近似颜色。每次场景上传标记实际引用，完成后移除旧项。SceneRenderer 将颜色写入现有实例缓冲，网格、纹理描述符和 shader 布局保持原有结构。

`subsurface_smoke.cpp` 使用隐藏窗口操作真实控件，比较独立 PBR 场景、串行离线图和 Vulkan 显示。原有模板和新增模板共同通过 `presets_smoke.cpp` 验证覆盖导入、手动编辑、历史、场景文件和模板保护。

## 复验

```sh
cmake --preset macos-desktop-release
cmake --build --preset macos-desktop-release --parallel 4
python3 scripts/verify_gpu.py \
  build/macos-desktop-release/src/app/desktop/minirender_desktop.app \
  --scene scenes/studio.json --subsurface-check --timing-only \
  --output outputs/a7.6/ui --timeout 300
python3 scripts/verify_gpu.py \
  build/macos-desktop-release/src/app/desktop/minirender_desktop.app \
  --scene scenes/studio.json --presets-check --sync-validation \
  --output outputs/a7.6/presets --timeout 300
```
