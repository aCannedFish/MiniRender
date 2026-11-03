// 只在本目标编译 stb 实现。上游版本与哈希由 CMake 固定，许可证保留在依赖头文件。
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>
