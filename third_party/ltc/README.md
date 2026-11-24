# LTC 拟合表

`ltc_1.dds` 和 `ltc_2.dds` 是 64 × 64 的 RGBA16F 数据。CMake 验证原始文件的 SHA-256，从 DDS 头之后读取半精度纹素。CPU 和 Vulkan 使用同一份拟合结果。原版表位于提交 `31e5e96b54f98f33098f8503003119ba2231a1c6` 的 `fit/results/` 目录。[原始数据](https://github.com/selfshadow/ltc_code/tree/31e5e96b54f98f33098f8503003119ba2231a1c6/fit/results)

Eric Heitz、Jonathan Dupuy、Stephen Hill 和 David Neubelt，*Real-Time Polygonal-Light Shading with Linearly Transformed Cosines*，ACM Transactions on Graphics，2016。发布应用时随包提供原作者的许可声明。[论文与补充材料](https://eheitzresearch.wordpress.com/415-2/) · [许可](LICENSE)
