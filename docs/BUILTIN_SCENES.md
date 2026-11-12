# 内置场景

工具栏“快速导入”提供可编辑的离线渲染示例。选择后覆盖当前场景的物体、材质、灯光和相机，并填入推荐渲染参数。导入后可直接编辑，离线渲染由用户点击启动。

## 场景列表

| 场景 | 观察内容 | 默认设置 |
| --- | --- | --- |
| Cornell Box · 经典 | 间接照明、红绿墙颜色渗透、方块软阴影 | 320×320、64 spp、深度 10 |
| Cornell Box · 玻璃与金属 | 玻璃透射、反射、粗糙金属 | 320×320、128 spp、深度 14 |
| 材质球阵列 | 漫反射、金属和不同粗糙度 | 480×320、64 spp、深度 8 |
| Cornell Box · 小光源 | 比较 BSDF、光源采样与 MIS 的噪声 | 320×320、128 spp、深度 10 |
| 有界雾 | RGB 吸收、散射和带介质衰减的阴影 | 320×320、128 spp、深度 12 |
| Cornell Box · 雾 | 面积光、墙面与均匀介质之间的传输 | 320×320、128 spp、深度 12 |
| 透光孔光束 | 方向光穿过真实顶板孔洞形成光束 | 320×320、128 spp、深度 12 |
| SPPM · 玻璃焦散 | 玻璃球聚光及地面焦散 | 320×320、128 轮、每轮 40000 光子、深度 8 |
| SPPM · 镜中焦散 | 通过镜面后墙观察玻璃球及焦散 | 320×320、128 轮、每轮 40000 光子、深度 8 |
| SSS · 大理石球 | 10cm Cornell 房间中的测量光学参数 | 320×320、128 spp、深度 8 |
| SSS · 蜡球 | 比较吸收、约化散射和密度 | 320×320、128 spp、深度 8 |
| SSS · 大理石侧面扩散 | 顶部受光、侧面出射的40mm方块 | 320×320、256 spp、深度 8 |
| SSS · 蜡侧面扩散 | 暖色蜡的跨位置散射 | 320×320、256 spp、深度 8 |

所有示例使用 seed=101 和 Russian roulette。普通与 SSS 示例默认 PT/MIS，两个焦散示例默认 SPPM、初始半径 0.12m 和 α=0.7。参数可在“离线”页调整。界面显示实际完成的样本或迭代，并报告当前阶段进度。

## 编辑与保存

导入后，物体列表中的墙壁、方块、球体和面积光均为普通场景对象。可使用移动/旋转工具调整物体，也可编辑变换、材质、相机和灯光参数。面积光通过“顶部面积光”物体的“自发光”和变换字段调整。不同物体使用独立材质，修改一个方块或球体的材质不会同时改变其他物体。

“保存”首次要求选择用户路径，内置模板保留原值。“另存”也可用于创建独立版本。重新快速导入会恢复模板。

Cornell 场景由顶部面积光照明，环境设为黑色。实时预览暂不计算面积光的全局照明，导入时切换到法线视图，便于编辑几何；离线渲染使用场景实际光照和材质。已有离线任务会异步取消，旧离线图收起，推荐参数在任务结束后应用。

体积场景导入后转到“介质”页，可编辑区域、RGB 系数和 g。体积效果通过离线渲染显示。方向光在“灯光”页调整，透光顶板可以使用视口操纵器移动、旋转。操作见 [介质界面](VOLUME_UI.md)。

## 数据来源

这些场景使用项目自建几何，Cornell 房间边长为 2 米，颜色为可编辑的线性 RGB。Cornell 场景的布局参考经典红绿墙、区域光和遮挡物配置。Cornell University 的 [官方介绍与数据](https://bowers.cornell.edu/cornell-box) 说明了该场景用于比较光传输模拟与实际测量的背景。

列表中的场景 JSON、球体和透光顶板 OBJ 由 `scripts/generate_builtin_scenes.py` 生成，脚本可重复执行。球体包含共享位置、径向法线和独立面角 UV；顶板由四块平面围成 0.7 米方孔。场景随程序打包，运行不依赖源码目录。

## 实现位置

- `scenes/cornell-*.json`、`scenes/material-balls.json`：场景和相机。
- `scenes/assets/demo-sphere.obj`：闭合单位球。
- `scenes/volume-*.json`、`scenes/assets/volume-aperture.obj`：介质场景和透光顶板。
- `scenes/sppm-*.json`：玻璃焦散和镜面观察场景。
- `scenes/sss-*.json`：大理石、蜡和侧面扩散场景，操作见 [SSS 界面](SUBSURFACE_UI.md)。
- `src/app/desktop/builtin_scenes.*`：菜单信息、推荐参数和包内路径。
- `Workbench::import_builtin_scene`：后台覆盖导入、编辑状态和模板保存路径。
- `OfflinePanel::prepare_import`：异步取消旧任务并应用新参数。
- `presets_smoke.cpp`：真实菜单动作、手动编辑、历史、文件和渲染回归。

## 复验

```sh
cmake --preset macos-desktop-release
cmake --build --preset macos-desktop-release
ctest --preset macos-desktop-release --output-on-failure
python3 scripts/verify_gpu.py \
  build/macos-desktop-release/src/app/desktop/minirender_desktop.app \
  --scene scenes/studio.json --presets-check --sync-validation \
  --output outputs/presets/ui --timeout 180
```

自动回归使用隐藏窗口，不激活或置顶。每个模板按推荐的 PT 或 SPPM 后端实际渲染，并与独立串行积分器逐像素比较。保存的 EXR 保留线性数据，PNG 应用显示变换。
