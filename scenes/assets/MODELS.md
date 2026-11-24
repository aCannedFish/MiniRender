# 经典模型

## Spot

Keenan Crane 的 Spot 小牛使用原始三角网格和斑纹贴图。包内的 `spot.obj` 与 `textures/spot.png` 分别对应上游压缩包中的 `spot_triangulated.obj` 和 `spot_texture.png`。模型与贴图采用 CC0 1.0。展示相机、灯光和物体变换保存在 `../catalog/spot.json`。[作者的模型下载页](https://www.cs.cmu.edu/~kmcrane/Projects/ModelRepository/#spot)

## Suzanne

`suzanne.obj` 来自 Khronos glTF Sample Assets 收录的 Blender 猴头，作者为 Norbert Nopper / UX3D，许可为 CC0 1.0。转换时将三角形写为 OBJ，共享同一坐标的位置索引，保留面角法线和 UV，并将纹理纵坐标换为 OBJ 约定。RenderBox 的预设使用灰色漫反射材质。[源模型与许可](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/e124e8d8d7330a5372bc2b4d00d39a4a9d7997ea/Models/Suzanne)
