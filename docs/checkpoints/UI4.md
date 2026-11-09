# 表面朝向

日期：2026-11-09。本次修复示例场景从地面下方观察时，地面与方块底面共面造成的条纹和闪烁。实现与阅读顺序见 [表面朝向](../SURFACE_SIDES.md)。

## 修改范围

- 材质增加 `double_sided`，旧文件省略时保持双面。
- 三个 studio 示例明确使用单面地面和实体表面，方块底面与地面继续接触于 `y=0`。
- Qt 材质页提供“双面渲染”，支持历史、不可变快照和保存重开。
- 实时表面与阴影按普通、镜像、双面选择管线。
- 线框使用所属三角形的几何法线，隐藏面不留下边线。
- CPU 最近命中、阴影 any-hit、拾取、面积光采样和 PDF 采用相同朝向。
- 自动 GPU 回归默认隐藏窗口并使用离屏 Vulkan 图像，不置顶、不激活窗口。

## CPU 检查

Release 与 ASan/UBSan 均为 143/143，通过。新增 4 个用例检查：

1. 正反面、非均匀及镜像缩放下，BVH、阴影、拾取和路径发射一致。
2. 剔除近处背面后，继续命中较远的可见面，有限阴影区间仍正确。
3. 单/双面面积光的有效侧、采样和 PDF，以及镜像后的外侧。
4. 场景文件往返、旧字段兼容、快照冻结和撤销/重做。

既有 BVH 随机射线参考同时加入材质朝向规则。Raster 顶点检查核对面角保留的几何法线，与着色法线独立。

## GPU 检查

`--sides-check` 使用 10 个视角与设置，分别检查颜色、法线、深度和线框，再连续改变相机位置 24 次。稳定采样点避开轮廓和三角形边界，GPU 画面、线性 HDR、CPU 路径发射及拾取全部一致。

| 视角/设置 | 每种颜色、法线、深度检查 |
| --- | --- |
| 地面上方 | 3459/3459 |
| 地面下方 | 3475/3475 |
| 下方移动后 | 3446/3446 |
| 距地面 0.02m 的下方视角 | 3491/3491 |
| 镜像实例 | 3486/3486 |
| 单面平面背侧 | 3528/3528，平面不可见 |
| 单面平面正侧 | 3500/3500 |
| 双面平面背侧 | 3492/3492 |
| Y 镜像平面正侧 | 3492/3492 |
| Y 镜像平面背侧 | 3528/3528，平面不可见 |

下方标准视角包含 204 个蓝色方块样本、158 个金属方块样本，地面样本为 0。单面平面背侧和 Y 镜像后的背侧，线框白色像素均为 0；正侧与双面背侧保留线条。

连续 24 次相机更新中，全部颜色、法线、深度样本通过；单面地面始终没有混入下方图像。复验也覆盖 Qt 开关、撤销、重做和场景保存重开。

上述完整朝向流程在 Release 和 ASan/UBSan 下分别运行，Vulkan 同步验证错误均为 0。ASan/UBSan 使用 `-O1`、`detect_leaks=0:abort_on_error=1` 和 `halt_on_error=1`；Catch2 从固定源码构建并使用相同的插桩参数。

## 关联回归

下列自动回归均在隐藏窗口下运行，Vulkan 同步验证错误为 0：

- A0 实际 GPU 像素、尺寸变化、读回和资源释放。
- 标准场景编辑、阴影、HDR、EXR、曝光、FXAA 和深度。
- 双光源场景，118/118 个遮挡样本、1166/1166 个受光样本通过。
- 线框遮挡、物体移动/旋转、连续拖拽及布局。
- 纹理、CPU 离线任务、SSAO、IBL 和几何处理。

日志包含 `Vulkan rendering target: offscreen` 与 `Regression window policy: hidden, no activation`。运行期间检查主窗口未激活、视口未暴露。离屏测试执行实际 GPU 渲染；人工窗口呈现检查通过显式 `--onscreen` 入口运行。

## 文件占用

Windows 的并发保存检查覆盖跨盘复制：目标文件已创建但写入句柄仍排他占用时，另一任务可能暂时无法比较内容。导出流程将该目标按已占名处理，改用新文件名，保留现有文件；源文件读取失败仍报错。Windows 专用用例用排他句柄固定重现这一状态。

## 应用更新

正式应用固定为 `/Applications/MiniRender.app`。安装脚本先在临时目录部署 Qt 依赖并执行离屏 GPU 检查，通过后替换正式应用，再清理同 bundle ID 的旧构建包和旧安装包。安装器的 6 个 Python 用例覆盖归属、清理范围、符号链接、替换、失败恢复、活动应用和构建检查。

## 复验

```sh
cmake --preset macos-desktop-release
cmake --build --preset macos-desktop-release
ctest --preset macos-desktop-release --output-on-failure
python3 scripts/verify_gpu.py \
  build/macos-desktop-release/src/app/desktop/minirender_desktop.app \
  --scene scenes/studio.json --sides-check --sync-validation \
  --output outputs/sides/contact --timeout 120
```

图像和日志保留在本地 `outputs/sides` 与 `work`，不加入普通 Git 历史。本次修复独立于进行中的重网格和软阴影改动。Windows CPU 行为通过 PR CI 检查，Windows/NVIDIA 桌面检查继续按 B 阶段执行。
