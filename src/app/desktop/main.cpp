#include "interaction_smoke.hpp"
#include "parameter_smoke.hpp"
#include "scene_smoke.hpp"
#include "texture_smoke.hpp"
#include "vulkan_viewport.hpp"
#include "wire_smoke.hpp"
#include "workbench.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QDockWidget>
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

namespace {

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
// --smoke 用真实窗口测试渲染、缩放和读回；没有 GPU 时失败，不能静默跳过。
int main(int argc, char** argv) {
#ifdef Q_OS_MACOS
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
    parser.addOption({"scene", "Open a scene JSON file.", "path"});
    parser.addOption({"scene-smoke", "Run scene editing and GPU checks.", "directory"});
    parser.addOption({"texture-smoke", "Run texture import and sampling checks.", "directory"});
    parser.addOption(
        {"interaction-smoke", "Run repeated drag and inspector layout checks.", "directory"});
    parser.addOption({"parameter-smoke", "Measure parameter edit responsiveness.", "directory"});
    parser.addOption({"wire-smoke", "Verify wireframe depth and HDR isolation.", "directory"});
    parser.process(application);
    const bool smoke = parser.isSet("smoke");
    const bool validation = parser.isSet("validation") || smoke || parser.isSet("scene-smoke") ||
                            parser.isSet("texture-smoke") || parser.isSet("interaction-smoke") ||
                            parser.isSet("parameter-smoke") || parser.isSet("wire-smoke");
    std::atomic_int validation_errors = 0;
    int result = 0;

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
        auto* viewport = window.viewport();
        if (!smoke) {
            const QString scene_path =
                parser.isSet("scene")
                    ? parser.value("scene")
                    : QCoreApplication::applicationDirPath() + "/scenes/studio.json";
            try {
                const bool regression =
                    parser.isSet("scene-smoke") || parser.isSet("texture-smoke") ||
                    parser.isSet("interaction-smoke") || parser.isSet("parameter-smoke") ||
                    parser.isSet("wire-smoke");
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

        int ticks = 0;
        bool resized = false;
        QSize initial_size;
        int initial_generation = 0;
        QTimer timer;
        timer.setInterval(100);
        QObject::connect(&timer, &QTimer::timeout, &window, [&] {
            ++ticks;
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
        window.show();
        window.raise();
        window.activateWindow();
        timer.start();
        result = application.exec();
    }
    // 销毁窗口及 GPU 资源后再判断，才能捕获 shutdown 期间的 Validation 错误。
    instance.destroy();
    qInfo() << "Vulkan validation errors:" << validation_errors.load();
    return validation_errors.load() == 0 ? result : 1;
}
