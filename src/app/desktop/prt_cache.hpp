// 本地 PRT 烘焙缓存。文件按几何与参数的内容键命名，取消或读到旧文件时可重新烘焙。
#pragma once
#include <QString>
#include <minirender/render/prt.hpp>

// 烘焙结果及磁盘缓存命中标记，供界面解释准备耗时。
struct CachedPrt {
    std::shared_ptr<const minirender::PrtBake> data;
    bool                                       reused = false;
};

// 在工作线程读取缓存或烘焙。使用独占临时文件发布，支持多个窗口同时请求相同场景。
CachedPrt load_or_bake_prt(const QString&                                       directory,
                           std::shared_ptr<const minirender::SceneSnapshot>     scene,
                           minirender::PrtSettings                              settings,
                           minirender::CancellationToken                        cancellation,
                           const std::function<void(std::size_t, std::size_t)>& progress);
