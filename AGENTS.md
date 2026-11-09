# 开发流程

- 自动 GPU 回归默认使用隐藏窗口和离屏 Vulkan 渲染，不置顶或激活窗口。
- 每次完成 macOS 桌面版本更新和测试后，运行 `python3 scripts/install_macos.py`，将正式应用固定为 `/Applications/MiniRender.app`。
- 新包先部署依赖并通过离屏检查，再替换正式应用，清理工作树构建目录及应用目录中同 bundle ID 的旧包。应用名称不添加版本号或检查点后缀。
- 构建包清理后，下次桌面构建先运行 `cmake --preset macos-desktop-release`，重新生成应用包元数据，再执行 build 和测试。
- 清理前确认相关构建和应用已退出。保留源码、场景、资产、测试日志及构建缓存。
- Windows 的固定目录更新和打包随 B 阶段实现，沿用只保留最新版的要求。
