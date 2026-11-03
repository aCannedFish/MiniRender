# 随机采样

## 随机流

`Sampler` 实现 PCG32 XSH-RR。状态为 64 位无符号整数，递推的溢出按模 $2^{64}$ 处理；移位与旋转生成 32 位输出。初始化接受状态种子与随机流编号。

`next_1d` 合并两次输出的 27 位与 26 位，得到 $[0,1)$ 内的 double。`next_2d` 顺序生成两个标量。`for_sample(seed,pixel,sample)` 用整数混合派生状态和流编号，线程编号及任务调度不参与其中。

每条路径使用自己的采样器，禁止多个线程共享一个可变随机流。显式分配随机维度，避免依赖 C++ 函数实参求值顺序：

```cpp
auto sampler = minirender::Sampler::for_sample(seed, pixel, sample_index);
const double component = sampler.next_1d();
const auto direction_u = sampler.next_2d();
const auto sample = bsdf.sample(wo, component, direction_u);
```

PCG 的初始化和输出变换依据 [Melissa O'Neill 的参考实现](https://github.com/imneme/pcg-c-basic/blob/master/pcg_basic.c)。项目用固定参考序列检查该约定，没有引入外部随机库。

## 几何映射

输入均为 $[0,1)^2$，方向为单位向量，半球位于局部 +Z。

| 函数 | 分布 | 密度或权重 |
| --- | --- | --- |
| `sample_disk` | 单位圆盘，同心方形映射 | 面积密度 $1/\pi$ |
| `sample_sphere` | 均匀球面 | 立体角密度 $1/(4\pi)$ |
| `sample_hemisphere` | 均匀半球 | 立体角密度 $1/(2\pi)$ |
| `sample_cosine_hemisphere` | 余弦半球 | $p_\omega=\max(0,z)/\pi$ |
| `sample_triangle` | 三角形均匀面积 | 返回重心权重，实际面积密度为 $1/A$ |

余弦半球把均匀圆盘投影到单位半球：$z=\sqrt{1-x^2-y^2}$。三角形使用 $r=\sqrt{u_x}$，重心坐标为 $(1-r,r(1-u_y),ru_y)$。平方根抵消面积随半径增长的 Jacobian。

同心圆盘和相关映射的推导见 [PBRT 4 采样章节](https://pbr-book.org/4ed/Sampling_Algorithms/Sampling_Multidimensional_Functions)。

## 离散分布

`DiscreteDistribution` 接收非负有限权重，先除以最大权重，再构造归一化 CDF，避免直接求和溢出。最终概率由相邻 CDF 值之差定义，查询与采样保持一致。

二分搜索跳过零宽度区间。返回的 `remapped` 是选中区间内重新映射的随机数，可用于条件采样。全零权重使用均匀分布；空分布返回空样本。

`SampleMeasure` 区分 `solid_angle` 和 `discrete`，分别表示方向密度与事件概率质量。光源选择 PMF 可以乘入对应条件概率；离散质量不能直接与立体角密度进行 MIS 比较。

## 验证

测试检查 PCG 参考序列、线程调度无关的流、样本取值范围、解析矩和分桶频次。圆盘的 $E[r^2]=1/2$，余弦半球的 $E[z]=2/3$，三角形的 $E[b_i]=1/3$、$E[b_i b_j]=1/12$。频次阈值在测试中按二项分布的五个标准差预先确定。

阅读顺序为 `sampling.hpp` 的约定、`Sampler`、各几何映射、`DiscreteDistribution`，最后阅读 `sampling_tests.cpp`。
