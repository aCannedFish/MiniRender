# 布料材质

A6.2 接入 Sadeghi 等人的微圆柱表面模型。CPU 路径追踪和 Vulkan 直接光使用相同的纱线参数、方向和求积样本，材质可随场景保存和编辑。模拟几何见 [布料模拟](CLOTH_SIMULATION.md)，检查结果见 [A6.2](checkpoints/A6.2.md)。

## 使用

选中有有效 UV 的物体，在“物体”页将材质切换为“布料（微圆柱）”。布料参数显示在普通材质字段下方。

“使用预置”提供 linen、silk、satin 和 shot 四种配置，更新纱线参数、折射率和底色倍率。它们对应论文 Table II 中的平纹亚麻、丝绸、聚酯缎面和经纬异色丝绸。

- 方向旋转改变经纬高光的朝向。
- 总覆盖率控制两组纱线的覆盖面积，经向占比控制两组的相对比例。
- 经、纬颜色用于内部散射，底色和纹理作为其颜色倍率。表面反射保持白色。
- 表面宽度与内部宽度以度为单位，分别控制两种散射的角度分布。
- 各向同性比例控制内部散射中方向性波瓣与各向同性项的比例。
- 切向采样控制每条曲线的求积点数，范围为 1–16，独立于离线 spp。

曲线由切向角和分段长度两列描述。相邻两个角对应一段，长度数量比角度数量少一。长度为零表示方向跳变。修改两列后点击“应用曲线”，两列一起验证和提交。其他字段的编辑保留尚未应用的曲线文本，切换场景后加载新文件中的曲线。

修改参数形成普通场景历史，可使用顶部撤销/重做。曲线无效时保留当前材质并显示原因。普通粗糙度在布料模式中禁用，改用两组纱线的宽度参数。

## 数据

`ClothAppearanceSettings` 保存两组纱线的线性 RGB、各向同性比例、宽度、覆盖率和切向曲线。`ClothAppearance` 验证参数后，在曲线相对长度上均匀取中点，缓存倾角的正弦、余弦和权重。

材质通过 `shared_ptr<const ClothAppearance>` 引用编译结果。编辑构造新对象，旧场景快照继续持有原值。折射率沿用 `Material::ior`，场景文件使用 `kind: cloth` 和嵌套的 `cloth` 参数。

网格的 UV 线性映射给出经纬方向。方向通过物体线性变换进入世界空间，再投影到着色切平面。两条方向分别归一化，保留剪切后的夹角。平移、非均匀缩放、镜像和背面都使用同一约定。

布料需要非退化 UV。场景提交时检查并缓存结果，缓存按不可变网格所有权标识。普通参数编辑复用已有结果，网格替换后重新检查。

## 求值

设宏观法线为 $N$，某组 UV 方向为 $e$，曲线倾角为 $\alpha$。纱线切向和局部法线为

$$t=\cos\alpha\,e+\sin\alpha\,N,\qquad n_t=\cos\alpha\,N-\sin\alpha\,e.$$

侧向为 $b=N\times e$。方向 $\omega$ 的三个角采用

$$\theta=\operatorname{atan2}(\omega\cdot t,\sqrt{(\omega\cdot n_t)^2+(\omega\cdot b)^2}),$$
$$\phi=\operatorname{atan2}(\omega\cdot b,\omega\cdot n_t),\qquad
\psi=\operatorname{atan2}(\omega\cdot t,\omega\cdot n_t).$$

表面反射使用 Fresnel、方位半角余弦和单位面积 Gaussian。内部散射使用两次 Fresnel 透射、方向性 Gaussian、各向同性项和纱线颜色。内部透射的界面法线取对应方向在圆柱法平面中的投影，这是本实现对论文式 (3) 的角度约定。

遮蔽 $M$ 与投影 $P$ 分别根据 $\phi$、$\psi$ 计算，并按论文式 (9)、(13) 混合独立与相关遮蔽。设样本权重为 $w_j=a_j/N_j$，总纱线散射为 $S_j$，则单位入射辐射对应的出射响应为

$$R(\omega_o,\omega_i)=\frac{\sum_jw_jS_jM_jP_j\cos\theta_i}{Q},$$
$$Q=\sum_jw_jP_j+(1-a_0-a_1)(N\cdot\omega_o).$$

`ClothBsdf::response` 返回 $R$。表面接口采用 $f=R/(N\cdot\omega_i)$，从而现有路径积分器中的 $f\cos\theta$ 与上述响应一致。Shader 直接使用 $R$ 计算直接光。

法平面余弦通过两个投影分量的长度求得。半角余弦平方通过两个单位圆方向的和求得，减少掠射角处的消去误差。CPU 和 GPU 对半角分母使用相同的 $10^{-12}$ 下界。

## 采样

纱线提议采用 Wang 等人的截断 Cauchy 方法。给定出射纵向角，先在 $[-\pi/2,\pi/2]$ 中归一化，再用逆 CDF 生成入射纵向角；方位项按半角余弦采样。内部散射按各向同性比例拆分，其各向同性提议覆盖球面。

本实现加入 20% 的宏观余弦提议，其他 80% 在纱线、切向样本和散射项中选择。表面项的选择比例根据 Fresnel 法向反射率和颜色亮度估计。最终 PDF 加总所有分量，并通过

$$d\omega=\cos\theta\,d\theta\,d\phi$$

转换为立体角密度。落入宏观背半球或精确地平方向的候选作为空事件保留，PDF 使用原分布。路径追踪的 MIS 接口保持同一测度。

## 实时管线

`RasterVertex` 增加两条 UV 方向，大小为 68 字节。每个实例包含 624 字节布料数据，完整实例记录为 800 字节，C++ 用静态断言检查 std430 大小与偏移。

Shader 复用 CPU 编译后的切向样本。曲线、颜色和宽度编辑更新实例缓冲，原顶点、边索引和纹理描述符保持复用。

实时直接光使用完整模型。实时环境漫反射采用覆盖率加权的平均纱线颜色，界面中显示这一近似；CPU 离线渲染对环境光执行完整路径采样。

## 运行

```sh
cmake --preset macos-core-release
cmake --build --preset macos-core-release --parallel 3
build/macos-core-release/minirender_tests '[cloth-bsdf]'
build/macos-core-release/minirender_cloth_material_example
```

示例在 `outputs/a6.2` 保存四种材质的 JSON、PNG 和线性 EXR。它使用实际模拟后的布料网格，160×160、64 spp、seed=101，多线程渲染并报告累计样本。每个场景保存重开后再次渲染以检查往返。

GPU 与界面回归：

```sh
python3 -B scripts/verify_gpu.py \
  build/macos-desktop-release/src/app/desktop/minirender_desktop.app \
  --scene scenes/studio.json --cloth-check --sync-validation \
  --output outputs/a6.2/validation --timeout 240
```

响应测量单独使用 `--timing-only`，去掉 `--sync-validation`。自动检查使用隐藏窗口和离屏 Vulkan 图像。

## 阅读顺序

1. `scene/cloth_material.hpp`、`src/scene/cloth_material.cpp`：参数、预置及曲线编译。
2. `render/cloth_bsdf.hpp`、`src/render/cloth_bsdf.cpp`：响应、角度、Fresnel、PDF 和采样。
3. `geometry/uv_tangents.*`：经纬方向；`path_tracer.cpp`：局部坐标与表面 BSDF 接入。
4. `src/assets/cloth_material_io.*`：原始参数保存与重编译。
5. `gpu_cloth.*`、`shaders/cloth.glsl`：布局、打包与 GPU 求值。
6. `cloth_material_panel.*`：逐字段编辑、曲线事务和预置。
7. `cloth_bsdf_tests.cpp`、`cloth_smoke.cpp`、`cloth_material_editor_smoke.cpp`：数学、图像和界面检查。

## 参考资料

- Sadeghi、Bisker、de Deken、Jensen，2013，*A Practical Microcylinder Appearance Model for Cloth Rendering*，ACM TOG 32(2)，Article 14，DOI `10.1145/2451236.2451240`。[作者原文](https://cseweb.ucsd.edu/~henrik/papers/practical_microcylinder_appearance_model_for_cloth_rendering.pdf)。参数与求值对应其表 I–II、式 (1)–(15)。
- Wang、Xie、Krishnamachari，2014，*Importance Sampling for a Microcylinder Based Cloth BSDF*，SIGGRAPH Talks，DOI `10.1145/2614106.2614134`。[作者机构原文](https://research.dreamworks.com/wp-content/uploads/2018/07/clothIS.pdf)。采样依据其纵向、方位和内部散射分解。
