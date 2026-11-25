// Qt 显示选择器和 GLSL 帧常量共用的模式编号。顺序与相机面板一致，参考图仅用于分屏。
#ifndef RENDERBOX_VIEW_MODES_H
#define RENDERBOX_VIEW_MODES_H

const int VIEW_LIGHTING      = 0;
const int VIEW_NORMAL        = 1;
const int VIEW_DEPTH         = 2;
const int VIEW_WIRE          = 3;
const int VIEW_AO_RAW        = 4;
const int VIEW_AO_FILTERED   = 5;
const int VIEW_BASE_COLOR    = 6;
const int VIEW_BLINN_PHONG   = 7;
const int VIEW_SHADOW_DEPTH  = 8;
const int VIEW_BLOCKER_DEPTH = 9;
const int VIEW_PENUMBRA      = 10;
const int VIEW_VISIBILITY    = 11;
const int VIEW_PRT           = 12;
const int VIEW_MIRROR        = 13;
const int VIEW_RAY_HIT       = 14;
const int VIEW_INDIRECT      = 15;
const int VIEW_COMBINED      = 16;
const int VIEW_ALBEDO        = 17;
const int VIEW_TRACE_STEPS   = 18;
const int VIEW_TEMPORAL      = 19;
const int VIEW_HISTORY_MASK  = 20;
const int VIEW_REFERENCE     = 21;

#endif
