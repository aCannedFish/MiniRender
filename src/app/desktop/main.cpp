#include "geometry_smoke.hpp"
#include "gizmo_smoke.hpp"
#include "ibl_smoke.hpp"
#include "interaction_smoke.hpp"
#include "offline_smoke.hpp"
#include "parameter_smoke.hpp"
#include "scene_smoke.hpp"
#include "sides_smoke.hpp"
#include "ssao_smoke.hpp"
#include "texture_smoke.hpp"
#include "vulkan_viewport.hpp"
#include "wire_smoke.hpp"
#include "workbench.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QDockWidget>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QVulkanInstance>

#include <atomic>
#include <cmath>
#include <string_view>

namespace {

// 自动回归只接受程序内测试事件，避免用户在其他工作中误触测试窗口改变相机/控件。
// 普通启动不安装此过滤器；窗口关闭等系统事件仍正常处理。
class RegressionInputGuard final : public QObject {
  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        // Qt 在应用切换时也会生成非 spontaneous 的失焦事件。自动手势不随桌面焦点中断，
        // 合成 UngrabMouse 仍正常送达，用于测试捕获恢复。
        if (event->type() == QEvent::WindowDeactivate) {
            return true;
        }
        if (!event->spontaneous()) {
            return false;
        }
        switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::ShortcutOverride:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::Wheel:
        case QEvent::WindowDeactivate:
        case QEvent::UngrabMouse:
            return true;
        default:
            return false;
        }
    }
};

// 用独立的颜色计数和中心点判断结果，避免只检查文件存在就认定 GPU 渲染通过。
bool validate_capture(const QImage& image) {
    if (image.isNull() || image.width() < 100 || image.height() < 100) {
        return false;
    }
    int cyan = 0;
    int cream = 0;
    int red = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            cyan += color.red() < 70 && color.green() > 140 && color.blue() > 180;
            cream += color.red() > 200 && color.green() > 180 && color.blue() > 100 &&
                     color.blue() < 160;
            red += color.red() > 180 && color.green() < 70 && color.blue() < 70;
        }
    }
    const QColor center = image.pixelColor(image.width() / 2, image.height() / 2);
    const int minimum = image.width() * image.height() / 100;
    const bool depth_occludes_red = center.green() > 140;
    qInfo() << "Capture:" << image.size() << "cyan" << cyan << "cream" << cream << "red" << red
            << "center" << center;
    return cyan > minimum && cream > minimum && red > minimum && depth_occludes_red;
}

} // namespace

// 桌面入口管理窗口与 Vulkan instance 生命周期：窗口必须先于 instance 销毁。
// 自动回归默认使用离屏 Vulkan 图像，保持 Qt 布局与事件循环运行。
// --onscreen-regression 供人工检查窗口呈现；没有 GPU 时回归失败。
int main(int argc, char** argv) {
    bool regression_startup = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        regression_startup = regression_startup || argument == "--smoke" ||
                             (argument.starts_with("--") && argument.ends_with("-smoke"));
    }
#ifdef Q_OS_MACOS
    if (regression_startup) {
        qputenv("QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM", "1");
    }
    // 自有 Vulkan 提交由 timer 调度，不参与 Qt RHI 的事务内呈现协议。
    // 使用普通 CAMetalLayer，避免 QMetalLayer 暂存 drawable，阻塞后台 nextDrawable。
    qputenv("QT_MTL_NO_TRANSACTION", "1");
    // MoltenVK 默认在调用 vkQueueSubmit 的线程中等待 Metal drawable。
    // 拖拽期间 Cocoa 也需要主线程处理显示事务，故使用官方异步提交模式。
    qputenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "0");
#endif
    QApplication application(argc, argv);
    QApplication::setApplicationName("MiniRender");
    QCommandLineParser parser;
    parser.setApplicationDescription("MiniRender — Vulkan scene workbench");
    parser.addHelpOption();
    parser.addOption({"smoke", "Run resize/readback checks and save a PNG.", "output"});
    parser.addOption({"validation", "Require Vulkan Validation Layers."});
    parser.addOption({"onscreen-regression",
                      "Use a visible non-activating window for manual presentation checks."});
    parser.addOption(
        {"timing-only",
         "Measure regression responsiveness without Vulkan validation instrumentation."});
    parser.addOption({"scene", "Open a scene JSON file.", "path"});
    parser.addOption({"scene-smoke", "Run scene editing and GPU checks.", "directory"});
    parser.addOption({"texture-smoke", "Run texture import and sampling checks.", "directory"});
    parser.addOption(
        {"interaction-smoke", "Run repeated drag and inspector layout checks.", "directory"});
    parser.addOption({"parameter-smoke", "Measure parameter edit responsiveness.", "directory"});
    parser.addOption({"wire-smoke", "Verify wireframe depth and HDR isolation.", "directory"});
    parser.addOption(
        {"offline-smoke", "Verify CPU task controls and Vulkan display.", "directory"});
    parser.addOption(
        {"ssao-smoke", "Verify SSAO sampling, filtering and lighting isolation.", "directory"});
    parser.addOption(
        {"ibl-smoke", "Verify IBL tasks, environment editing and CPU/GPU results.", "directory"});
    parser.addOption({"geometry-smoke",
                      "Verify background geometry, file tasks and CPU/GPU updates.", "directory"});
    parser.addOption(
        {"gizmo-smoke", "Verify object manipulation, history and GPU overlays.", "directory"});
    parser.addOption(
        {"sides-smoke", "Verify material sidedness and coplanar contact views.", "directory"});
    parser.process(application);
    const bool smoke = parser.isSet("smoke");
    const bool regression = smoke || parser.isSet("scene-smoke") || parser.isSet("texture-smoke") ||
                            parser.isSet("interaction-smoke") || parser.isSet("parameter-smoke") ||
                            parser.isSet("wire-smoke") || parser.isSet("offline-smoke") ||
                            parser.isSet("ssao-smoke") || parser.isSet("ibl-smoke") ||
                            parser.isSet("geometry-smoke") || parser.isSet("gizmo-smoke") ||
                            parser.isSet("sides-smoke");
    const bool timing_only = parser.isSet("timing-only");
    const bool offscreen = regression && !parser.isSet("onscreen-regression");
    if (parser.isSet("onscreen-regression") && !regression) {
        qCritical() << "--onscreen-regression requires a regression mode.";
        return 1;
    }
    qInfo() << "Vulkan rendering target:" << (offscreen ? "offscreen" : "window");
    if (timing_only && (!regression || parser.isSet("validation"))) {
        qCritical() << "--timing-only requires a regression mode and excludes --validation.";
        return 1;
    }
    const bool validation = parser.isSet("validation") || (regression && !timing_only);
    qInfo() << "Vulkan validation mode:" << (validation ? "enabled" : "disabled");
    RegressionInputGuard input_guard;
    if (regression) {
        application.installEventFilter(&input_guard);
    }
    std::atomic_int validation_errors = 0;
    int result = 0;
    QElapsedTimer geometry_cleanup;

    QVulkanInstance instance;
    instance.setApiVersion(QVersionNumber(1, 2));
    if (validation) {
        if (!instance.supportedLayers().contains("VK_LAYER_KHRONOS_validation")) {
            qCritical() << "Validation layer is required but unavailable.";
            return 1;
        }
        instance.setLayers({"VK_LAYER_KHRONOS_validation"});
    }
    instance.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags severity,
                                          QVulkanInstance::DebugMessageTypeFlags, const void*) {
        if (severity.testFlag(QVulkanInstance::ErrorSeverity)) {
            ++validation_errors;
        }
        return false;
    });
    if (!instance.create()) {
        qCritical() << "Cannot create Vulkan instance:" << instance.errorCode();
        return 1;
    }

    {
        Workbench window(&instance, smoke);
        if (regression) {
            QObject::connect(&window, &QWidget::windowTitleChanged, &window,
                             [&window](const QString& title) {
                                 if (!title.contains("自动回归")) {
                                     window.setWindowTitle(title + " [自动回归]");
                                 }
                             });
            window.setWindowTitle(window.windowTitle() + " [自动回归]");
        }
        auto* viewport = window.viewport();
        viewport->set_offscreen_rendering(offscreen);
        if (!smoke) {
#ifdef Q_OS_MACOS
            const QString bundled_scene =
                QCoreApplication::applicationDirPath() + "/../Resources/scenes/studio.json";
#else
            const QString bundled_scene =
                QCoreApplication::applicationDirPath() + "/scenes/studio.json";
#endif
            const QString scene_path =
                parser.isSet("scene") ? parser.value("scene") : bundled_scene;
            try {
                if (regression) {
                    window.load_scene_file(scene_path);
                } else {
                    window.open_scene_file(scene_path);
                }
            } catch (const std::exception& error) {
                qCritical() << error.what();
                return 1;
            }
        }
        if (parser.isSet("scene-smoke")) {
            start_scene_smoke(window, parser.value("scene-smoke"));
        }

        if (parser.isSet("texture-smoke")) {
            start_texture_smoke(window, parser.value("texture-smoke"));
        }

        if (parser.isSet("interaction-smoke")) {
            start_interaction_smoke(window, parser.value("interaction-smoke"));
        }
        if (parser.isSet("parameter-smoke")) {
            start_parameter_smoke(window, parser.value("parameter-smoke"));
        }

        if (parser.isSet("wire-smoke")) {
            start_wire_smoke(window, parser.value("wire-smoke"));
        }
        if (parser.isSet("offline-smoke")) {
            start_offline_smoke(window, parser.value("offline-smoke"));
        }
        if (parser.isSet("ssao-smoke")) {
            start_ssao_smoke(window, parser.value("ssao-smoke"));
        }
        if (parser.isSet("ibl-smoke")) {
            start_ibl_smoke(window, parser.value("ibl-smoke"));
        }
        if (parser.isSet("geometry-smoke")) {
            start_geometry_smoke(window, parser.value("geometry-smoke"), timing_only);
        }
        if (parser.isSet("gizmo-smoke")) {
            start_gizmo_smoke(window, parser.value("gizmo-smoke"), timing_only);
        }

        if (parser.isSet("sides-smoke")) {
            start_sides_smoke(window, parser.value("sides-smoke"));
        }

        int ticks = 0;
        bool resized = false;
        QSize initial_size;
        int initial_generation = 0;
        QTimer timer;
        timer.setInterval(100);
        QObject::connect(&timer, &QTimer::timeout, &window, [&] {
            ++ticks;
            if (offscreen && (window.isActiveWindow() || viewport->isExposed())) {
                qCritical() << "Offscreen regression exposed or activated a window.";
                application.exit(1);
                return;
            }
            if (!viewport->failure().isEmpty()) {
                application.exit(1);
                return;
            }
            if (smoke && viewport->isValid()) {
                window.statusBar()->showMessage(
                    QString("GPU: %1 | 帧 %2 | 交换链 %3")
                        .arg(QString::fromUtf8(viewport->physicalDeviceProperties()->deviceName))
                        .arg(viewport->rendered_frames())
                        .arg(viewport->resource_generations()));
            }
            if (!smoke) {
                return;
            }
            if (ticks > 200) {
                qCritical() << "GPU smoke test timed out; no successful readback.";
                application.exit(1);
                return;
            }
            if (!resized && viewport->rendered_frames() > 5) {
                initial_size = viewport->swapChainImageSize();
                initial_generation = viewport->resource_generations();
                window.resize(1120, 720);
                resized = true;
                return;
            }
            if (resized && viewport->resource_generations() > initial_generation &&
                viewport->swapChainImageSize() != initial_size &&
                viewport->rendered_frames() > 15) {
                timer.stop();
                if (!viewport->supportsGrab()) {
                    qCritical() << "Actual swapchain does not support readback.";
                    application.exit(1);
                    return;
                }
                const QImage image = viewport->grab();
                const auto output = parser.value("smoke");
                const bool saved =
                    QDir().mkpath(QFileInfo(output).absolutePath()) && image.save(output);
                const bool passed = saved && validate_capture(image);
                qInfo() << "GPU smoke" << (passed ? "PASS" : "FAIL") << "swapchain generations"
                        << viewport->resource_generations() << "output" << output;
                application.exit(passed ? 0 : 1);
            }
        });
        if (smoke) {
            window.resize(960, 640);
        }
        if (regression) {
            window.setAttribute(Qt::WA_ShowWithoutActivating);
            window.setWindowFlag(Qt::WindowDoesNotAcceptFocus);
            window.setAttribute(Qt::WA_DontShowOnScreen, offscreen);
            qInfo() << "Regression window policy:"
                    << (offscreen ? "hidden, no activation" : "visible, no activation");
        }
        window.show();
        if (offscreen) {
            viewport->requestUpdate();
        }
        if (!regression) {
            window.raise();
            window.activateWindow();
        }
        timer.start();
        result = application.exec();
        if (parser.isSet("geometry-smoke")) {
            geometry_cleanup.start();
        }
    }
    // 销毁窗口及 GPU 资源后再判断，才能捕获 shutdown 期间的 Validation 错误。
    instance.destroy();
    if (geometry_cleanup.isValid()) {
        qInfo() << "Geometry shutdown ms:" << geometry_cleanup.elapsed();
        if (geometry_cleanup.elapsed() > 2000) {
            result = 1;
        }
    }
    qInfo() << "Vulkan validation errors:" << validation_errors.load();
    return validation_errors.load() == 0 ? result : 1;
}
