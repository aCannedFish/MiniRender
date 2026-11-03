#pragma once

// --render 的无窗口入口。接收 UTF-8 参数，解析设置并使用 core 积分器输出 EXR/PNG。
int run_render_cli(int argc, char** argv);
