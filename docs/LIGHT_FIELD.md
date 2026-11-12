# 光场核心

光场保存瞳孔位置与图像位置上的四维采样。CPU 先采集二维相机阵列，再从保存的子视图重建不同焦点和孔径的图像。重聚焦只读取文件中的线性 RGB，不需要原场景或路径积分器。

方法参考 [Ng 等人的光场摄影原文](https://graphics.stanford.edu/papers/lfcamera/lfcamera-150dpi.pdf)第 4 节的平移叠加方法。二维子视图表示也可参见 [Levoy 与 Hanrahan 的光场渲染](https://graphics.stanford.edu/papers/light/)。下面按本项目的世界焦面和相机约定推导重建变换。

## 标定

`LightFieldCalibration` 保存原始相机、图像尺寸、采样参数、参考焦距 $F_0$、瞳孔半径和各视图的二维瞳孔坐标。距离单位为米，焦面沿相机 forward 测量，瞳孔坐标沿 right/up 测量。

相机阵列为 3–15 的奇数行列，中心格点精确位于瞳孔原点。方形格中心经过既有 Shirley–Chiu 同心圆盘映射，每个格点对应一张固定瞳孔位置的针孔子视图。数组按行保存，首行位于相机上方，首列位于左侧。

所有视图使用相同的相机方向、FOV 和参考焦面。设瞳孔偏移为 $a=(a_x,a_y)$，原针孔射线为 $(o,d)$，则：

$$p_0=o+d\frac{F_0}{d\cdot f},\qquad
o_a=o+a_xr+a_yu,\qquad d_a=\operatorname{normalize}(p_0-o_a).$$

其中 $f,r,u$ 为相机 forward/right/up。瞳孔为零时直接返回原针孔射线，保留中心视图与现有 PT 的精确一致性。

各视图使用相同的 seed/pixel/sample 随机流。每个样本仍消费原 PT 的 film 和 lens 维度，再追踪固定瞳孔射线。近远裁剪距离除以射线与 forward 的余弦，使所有瞳孔使用同一组世界裁剪平面。

## 重聚焦

目标焦面位于距离 $F$。在相机的横向坐标中，把参考点记为 $q_0$。保持原视场时，同一输出像素在目标焦面上的横向坐标为 $q_F=(F/F_0)q_0$。从瞳孔 $a$ 经过该焦点的射线，在参考焦面处的横向坐标为：

$$q'_0=q_0+a\left(1-\frac{F_0}{F}\right).$$

正方形像素的焦距像素数为

$$f_{px}=\frac{h}{2\tan(\mathrm{FOV}_y/2)}.$$

每个子视图的读取偏移为

$$\Delta x=f_{px}a_x\left(\frac1{F_0}-\frac1F\right),\qquad
\Delta y=-f_{px}a_y\left(\frac1{F_0}-\frac1F\right).$$

图像原点在左上角，纵轴因此取负号。整数图像索引对应像素中心，重建读取 $(x+\Delta x,y+\Delta y)$，使用双线性插值。

孔径选择包含其半径内的瞳孔样本，半径为零时只选择中心视图。每个输出像素按实际有效视图数归一，并返回有效视图覆盖率。孔径改变时沿用既有薄透镜的亮度平均约定。

## 流式存储

`.mrlf` 是独立文件，按小端序保存 JSON 标定和逐视图 RGB32f 像素。图像均为线性 HDR，像素原点在左上角，通道顺序为 RGB。文件保存重建所需的成像标定，重开文件后的重聚焦不加载原场景。

| 字节偏移 | 内容 |
| --- | --- |
| 0 | 8 字节 magic，`MRLF`、CR、LF、两个零字节 |
| 8 | uint32 格式版本，当前为 1 |
| 12 | uint32 JSON 字节数，最多 1MiB |
| 16 | uint64 像素数据字节数，最多 64GiB |
| 24 | uint64 已记录的采集主路径数 |
| 32 | uint64 完整标记，发布后为 1 |
| 40 | UTF-8 JSON，随后为视图像素数据 |

JSON 保存相机、图像宽高、spp、完整 uint64 seed、阵列行列、瞳孔位置、参考焦距，以及 PT 的采样模式、深度和 RR 参数。主路径数属于采集来源记录，重聚焦不会改变它。

`LightFieldWriter` 可按任意视图顺序写入对应位置，仅保留行块缓冲和完成标记。所有视图成功后才关闭、原子提交目标文件。重复视图、尺寸不匹配和非有限像素被拒绝，失败或取消保留已有目标。

`LightFieldFile` 只持有不可变标定和文件索引。读取时检查格式、长度、整数范围和完整标记，像素在按需解码时检查有限性。保存通过固定大小缓冲复制容器，期间没有重采集。

## 内存

采集工作缓冲只包含当前视图、发布预览和 worker 临时数据。完整数据集逐视图写盘。准备前估算工作量、像素文件大小和内存，超过预算时返回原因。场景资产、BVH 及其准备阶段另计。

`LightFieldReader` 使用有界 LRU。小数据集可在预算内全部缓存，大数据集按需解码和回收视图。缓存为零时逐视图读取。调用方额外保留的历史图像不属于像素缓存预算。

重聚焦预检包含输出累计、覆盖计数、最终图像、一个解码视图、缓存和 IO 缓冲。重建内核每次仅持有一张输入视图；数据总量可以超过工作内存预算。

## 后台任务

`LightFieldCaptureSession` 固定启动快照，共享一个 PT/BVH，先采集中心视图，再采集其余视图。每张图像的 tile 分给不同线程，像素内按固定样本顺序累加；线程数不改变图像结果。

进度区分准备、采集、写盘和完成，报告视图数、样本数、工作内存和文件像素量。完成一张视图后发布不可变预览。暂停等待所有 worker 或写盘控制线程停稳，恢复保留样本位置，取消只发出请求。析构或 `wait` 收回线程。

完整结果共享其自有临时文件的生命周期。捕获器销毁后，结果仍可读写；保存到用户路径后可以独立重开。未完成的文件不作为结果发布。

## 命令行

```sh
cmake --preset macos-core-release
cmake --build --preset macos-core-release --parallel 4
build/macos-core-release/minirender_lightfield_example
build/macos-core-release/minirender_cli --capture-lightfield \
  outputs/a8.1/scene.json outputs/a8.1/capture-cli.mrlf \
  --width 128 --height 128 --spp 32 --columns 5 --rows 5 \
  --radius 0.2 --reference-focus 4 --depth 1 --threads 3 --no-rr
build/macos-core-release/minirender_cli --refocus \
  outputs/a8.1/capture-cli.mrlf outputs/a8.1/near-cli \
  --focus 2 --aperture 0.2 --cache-mb 0
```

未指定采集参考焦距和孔径时，CLI 从场景相机取值；相机孔径为零时使用场景相机对焦距离的 1%。`--memory-mb` 控制工作预算，`--cache-mb` 控制重建缓存，`--exposure` 仅影响 PNG 显示输出。

例程生成近红、远蓝两个棋盘和可编辑场景，保存 25 张子视图的单个容器，再生成近焦、远焦、参考焦距和针孔图像。界面在 A8.2 接入。

## 阅读顺序

| 文件 | 内容 |
| --- | --- |
| `lightfield.hpp`、`lightfield_camera.cpp` | 标定、固定瞳孔射线与重建偏移 |
| `assets/lightfield_io.*` | 容器、逐视图写盘和有界 LRU |
| `lightfield_refocus.cpp` | 双线性平移、孔径与有效权重 |
| `lightfield_capture.*` | 后台采集、暂停取消、预览和临时存储所有权 |
| `lightfield_cli.cpp`、`examples/lightfield.cpp` | 独立运行、展示与清晰度测量 |
| `lightfield_tests.cpp` | 射线几何、解析图案、文件、内存与任务验证 |
