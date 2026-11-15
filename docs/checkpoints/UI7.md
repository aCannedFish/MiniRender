# 预览轮廓

日期：2026-11-15。原色预览增加默认开启的轮廓和折角线，区分前景白块与同色背景墙。设置在“场景与渲染 → 实时”，提供开关与强度，默认 0.35。方法与模块入口见[原色预览](../BASE_COLOR_PREVIEW.md#轮廓显示)。

## 几何对照

回归创建同色方块与平行白墙，关闭接触遮蔽，使用 CPU BVH 查找相邻像素的最近命中。再调整相机和方块方向，分别检查遮挡轮廓、明显折角以及斜平面内部。

| 像素密度 | 正视边界匹配 | 斜视边界匹配 | 折角匹配 | 斜视平面内部误描 |
| --- | --- | --- | --- | --- |
| 2× | 740/740 | 804/805 | 330/330 | 0/394423 |
| 3× | 1611/1611 | 1851/1851 | 717/717 | 0/851817 |

全黑前景和背景也检查轮廓对比。正视图中，2× 的 740 个边界探针及 3× 的 1611 个探针均匹配。深度探针检查前后实例变化，折角探针检查同一实例的法线转折，内部探针排除周围的几何边界。

## 状态检查

- 相机静止时，连续图像完全相同。
- 提高强度可以增强线条；关闭轮廓或设置零强度，逐像素恢复未描边的图像。
- 轮廓设置不重新上传网格或重建场景描述符。
- 切换到光照模式后，轮廓开关不改变图像。
- 开关轮廓前后，线性 HDR 最大通道误差为 0。
- FXAA 可以平滑轮廓；六次连续相机拖拽、窄窗口和 Cornell Box 导入均完成。

普通 Release 的实际交互事件排队最大 23ms。初始化结束后开始计时，独立 CPU 对照和显式 GPU 读回不计入操作延迟。Vulkan 同步验证在单独运行中检查。

## 复验

```sh
cmake --preset macos-desktop-release
cmake --build --preset macos-desktop-release --parallel 4
python3 scripts/verify_gpu.py build/macos-desktop-release/src/app/desktop/minirender_desktop.app --scene scenes/studio.json --outline-check --sync-validation --output outputs/outline --timeout 240
QT_SCALE_FACTOR=1.5 python3 scripts/verify_gpu.py build/macos-desktop-release/src/app/desktop/minirender_desktop.app --scene scenes/studio.json --outline-check --sync-validation --output outputs/outline-dpi --timeout 240
```

计时使用 `--timing-only`。所有自动检查均为隐藏窗口，不激活应用。

CPU 核心 298/298 通过。Release 和 ASan/UBSan 分别通过轮廓、原色与全部 14 个模板检查，Vulkan 同步验证错误为 0。ASan/UBSan 轮廓专项的事件排队最大 96ms。光照、SSAO、IBL、工作台布局、离线显示与操纵器关联回归均通过。Debug 和 Release 编译数据库包含新增模块。
