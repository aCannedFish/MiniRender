# 环境界面

## 使用

右侧“环境”页提供常量 RGB、环境图、强度、绕 +Y 的旋转和背景显示。点击“导入环境图”注册并选中新资产，物体的底色纹理保持原有设置。HDR/EXR 按线性数据读取，PNG/JPEG 先按 sRGB 解码。

输入按 latlong 解释，建议使用 2:1 图像。环境照明为常量项与图像项之和。图像强度和旋转直接更新实时参数，不重新烘焙。常量 RGB、图像选择、强度和旋转属于场景，可撤销/重做并保存；背景开关属于当前视口。

预处理期间可以继续编辑和导航。面板显示实际阶段、完成像素数和百分比；mip 构建与任务切换使用忙碌条。点击“取消预处理”后使用常量环境光，点击“重试预处理”再开始。取消状态不会在每帧自动重新启动。

来源变化时先清除旧环境图的实时数据，准备期间显示常量照明。完成结果只有匹配当前来源才能发布，避免把旧任务的环境用于新场景。缓存中已有的环境可以立即复用。

## 线程

`IblController` 在 GUI 线程管理任务、来源和两个缓存项。worker 只接收固定源图、取消标记、不可变 BRDF 和进度对象，不访问 QWidget 或 Vulkan。GUI 以定时器读取进度，回调不会为每个像素向事件队列发送消息。

同一控制器最多运行一个 worker，来源连续变化时仅保留最新请求。旧工作退出后再处理新输入。BRDF 与环境图无关，可以跨来源和取消操作保留；缓存按不可变纹理所有权识别，不依赖可变文件名。重新从文件加载会创建新资产，按新来源预处理。

取消不在普通 UI 回调中等待线程。控制器销毁时请求取消、断开完成回调并等待 worker 退出。任务输出通过 `shared_ptr<const>` 共享，旧结果不会被下一次烘焙修改。

## GPU 数据

`GpuIbl` 独立维护漫反射、镜面、BRDF 和源图。BRDF 更新不要求重新上传环境图，环境图切换也不重复上传同一个 BRDF。普通材质、强度、旋转、相机或尺寸调整均保留已有纹理。

| binding | 输入 | 布局 |
| --- | --- | --- |
| 4 | 漫反射 | 2D，Y 包含极点端点 |
| 5 | 镜面粗糙度层 | 2D array，同尺寸，层间按 roughness 插值 |
| 6 | BRDF A/B | 2D，NdotV/roughness 参数端点 |
| 7 | 源环境图 | 普通纹素中心 latlong |

所有数据使用 RGBA32F。shader 手动执行双线性和层间插值，采样器为最近邻，避免依赖 float32 线性过滤能力。布局与核心查询函数一致，见 [IBL](IBL.md)。

每个 binding 始终有有效 fallback。数据到达后，资源创建、staging 写入、描述符更新和旧资源释放都发生在上一帧 fence 完成后。上传从 transfer-destination 转为 shader-read；staging 在对应帧完成后回收。环境更新只改 IBL 描述符，不重建网格或材质描述符池。

## 光照合成

Lambert 环境漫反射为

$$C_a=albedo\,[C_{constant}+sD(n)](1-metallic).$$

导体镜面为

$$C_s=[C_{constant}+sL_p(R,r)]\,[F_0A(N\cdot V,r)+B(N\cdot V,r)].$$

常量环境也使用同一 BRDF 表。准备尚未完成时，导体保留启动阶段的近似预览。实时镜面/玻璃继续采用界面中标注的预览近似；完整反射/折射由路径追踪承担。

背景在主场景 pass 中使用全屏三角形绘制，根据逆投影和 view 的逆旋转查询世界方向。它不写深度，法线 alpha 和环境漫反射输出为 0。关闭背景后保留照明，显示原有清屏色。

SSAO 只消减 $C_a$。直接光、自发光、$C_s$ 和环境背景保持原值。实时 EXR 读取合成后的线性 HDR，曝光与色调映射只影响显示。

## 验证

```sh
cmake --build --preset macos-desktop-debug
python3 scripts/verify_gpu.py build/macos-desktop-debug/src/app/desktop/minirender_desktop.app --scene scenes/studio.json --ibl-check --sync-validation --output outputs/ibl --timeout 180
```

回归通过专用导入入口和真实 Qt 控件操作任务，检查取消/重试、缓存、连续来源变化、撤销/重做、保存/重开、相机/缩放、背景关闭和活动任务退出。它使用 CPU BVH 求交，独立计算法线、反射与环境查询，对照 HDR 的背景、Lambert 和导体路径。

普通交互的 heartbeat 统计排除窗口初始化和显式截图/编码。自动回归窗口带有标识，隔离真实键鼠及桌面失焦事件，程序内的合成捕获事件仍用于验证恢复。普通启动使用完整交互行为。

环境面板还检查窄窗口的字段范围、兄弟控件重叠和滚动可达性。完整数值结果见 [A3.2b](checkpoints/A3.2b.md)。

## 阅读

1. `EnvironmentPanel`：场景事务、导入入口与阶段显示。
2. `IblController::poll/launch`：最新来源、串行后台工作和完成结果过滤。
3. `IblController::cache/cached`：资产所有权与两项 LRU。
4. `GpuFloatTexture`、`GpuIbl`：数组上传、fallback、staging 与 fence。
5. `ibl_sampling.glsl`、`environment.frag`：两种 latlong 布局和背景射线。
6. `scene.frag`：环境漫反射与导体 split-sum。
7. `ibl_smoke.cpp`：独立 CPU 几何对照与任务生命周期。
