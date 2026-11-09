#include "builtin_scenes.hpp"

#include <QCoreApplication>

const std::array<BuiltinScene, 4>& builtin_scenes() {
    static const std::array<BuiltinScene, 4> scenes{
        {{"cornell",
          "Cornell Box · 经典",
          "cornell-box.json",
          "红绿墙、白色方块与顶部面积光，观察间接照明和颜色渗透。",
          "short_box",
          true,
          {320, 320, 64, 101},
          {.maximum_depth = 10}},
         {"glass",
          "Cornell Box · 玻璃与金属",
          "cornell-materials.json",
          "观察玻璃透射、反射和粗糙金属。",
          "glass_ball",
          true,
          {320, 320, 128, 101},
          {.maximum_depth = 14}},
         {"materials",
          "材质球阵列",
          "material-balls.json",
          "比较漫反射、金属和不同粗糙度。",
          "ball_4",
          false,
          {480, 320, 64, 101},
          {.maximum_depth = 8}},
         {"small_light",
          "Cornell Box · 小光源",
          "cornell-small-light.json",
          "可在离线页比较 BSDF、光源采样与 MIS 的噪声。",
          "short_box",
          true,
          {320, 320, 128, 101},
          {.maximum_depth = 10}}}};
    return scenes;
}
QString builtin_scene_directory() {
#ifdef Q_OS_MACOS
    return QCoreApplication::applicationDirPath() + "/../Resources/scenes";
#else
    return QCoreApplication::applicationDirPath() + "/scenes";
#endif
}
