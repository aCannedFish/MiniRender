# 光源采样

## 数据流

`LightSampler` 固定一个不可变场景快照，枚举发光三角形、方向光及环境光。光源选择按估计功率构造离散 CDF；采样结果的概率包含选择 PMF。返回值不执行遮挡查询，积分器根据距离发送有限或无限阴影射线。

双面面积光功率估计为 $2\pi A\,luminance(L_e)$，单面为 $\pi A\,luminance(L_e)$。方向光按场景包围球的投影面积估计，环境光再乘方向辐射的亮度积分。这些估计只影响选择效率，实际选择 PMF 始终计入结果。

## 面积光

材质 `emission` 对应漫发射，`double_sided` 控制单面或双面。单面光源的背侧返回空样本和零 PDF，与 BVH 的背面剔除一致。每个发光三角形使用变换后的世界面积，均匀采样重心坐标。面积密度换为参考点处的立体角密度：

$$p_\omega=p_{select}\frac{r^2}{A|\mathbf n_l\cdot(-\omega_i)|}.$$

`LightSample::radiance` 返回 $L_e$，距离和余弦已包含在密度转换中。积分器不应再对辐射值额外乘一次平方距离衰减。`area_pdf` 根据命中的实例/三角形索引计算同一密度，供 BSDF 路径到达发光面时做 MIS。

参考点与样本重合或与光源共面时，该面积样本没有有效方向/立体角贡献，返回空样本。面积光不进行重采样。

## 无限光

方向光方向为 `-normalize(direction)`，其值沿用预览的辐照度系数。它是离散方向事件，返回概率质量和无穷距离。

环境光采用

$$L_{env}(\omega)=environment+strength\,L_{map}(\omega).$$

无环境图或图像分布为零时使用均匀球面采样。其余情况使用环境图的重要性分布，始终混入 5% 均匀球面密度，覆盖双线性插值在零权重 texel 附近的支持集。

环境图采用 Latlong：图像上边对应 +Y，零经度为 +X，经度增加朝 +Z。绕 +Y 的正旋转遵循右手系。图像 U 周期寻址、V 在南北极夹取，使用独立的图像坐标查询，避免 OBJ 的 V 翻转。

## 环境分布

CDF 使用不超过 1024×512 的现有 mip 图，辐射查询使用原图。每个采样单元的精确立体角为

$$\Delta\omega=\Delta\phi(\cos\theta_0-\cos\theta_1).$$

权重为单元亮度乘 $\Delta\omega$。选中单元后，方位角和 $\cos\theta$ 分别均匀采样，因而单元内密度为 $P_{cell}/\Delta\omega$，不需要在极点除以 $\sin\theta$。最终密度为

$$p_\omega=0.05/(4\pi)+0.95\,P_{cell}/\Delta\omega.$$

负 RGB texel 在环境重建前夹为零。选择功率从原图的非负辐射估计，避免 mip 中正负值抵消后把仍有照明的环境删掉。若 mip 分布为零，保留环境并使用均匀采样。

这套分布与 [PBRT 环境光](https://pbr-book.org/4ed/Light_Sources/Infinite_Area_Lights) 的用途一致，具体投影使用项目定义的 Latlong 单元及上述混合密度。面积/立体角转换参考 [PBRT 面积光](https://pbr-book.org/4ed/Light_Sources/Area_Lights)。

## 场景设置

JSON v1 增加可选的 `environment_map`。纹理仍从现有 `textures` 数组引用，HDR/EXR 必须以 `linear` 编码加载。旧文件没有该对象时沿用常量环境。

```json
"environment_map": {
  "texture": "sky",
  "strength": 1.0,
  "rotation_degrees": 30.0
}
```

强度要求有限且非负，旋转要求有限，纹理 ID 必须存在。保存/重开保留设置，相对资产路径和中文文件名沿用既有规则。快照固定设置值，并共享不可变环境纹理。

本检查点提供 CPU 采样。实时 IBL 在 A3 接入；当前工作台加载带环境图的场景会显示对应提示。

## 验证

恒定环境图检查球面均匀性、极点和 $1/(4\pi)$ 密度。亮点环境检查密度归一化、采样频次、双线性支持集和旋转。额外检查负值 mip 抵消后仍保留发光环境。

面积光检查解析 Jacobian、双面行为、变换面积和 BVH 命中后的 PDF 对照。混合场景检查面积、方向及环境光的选择 PMF，区分离散与连续测度。

阅读顺序为 `EnvironmentLight` 的坐标约定、CDF 构建、`sample/pdf/evaluate`，再读 `LightSampler` 的功率估计、面积转换及 `light_tests.cpp`。
