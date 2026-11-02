#include "vulkan_viewport.hpp"

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
    QApplication application(argc, argv);
    QApplication::setApplicationName("MiniRender");
    QCommandLineParser parser;
    parser.setApplicationDescription("MiniRender A0 — Vulkan texture and depth diagnostics");
    parser.addHelpOption();
    parser.addOption({"smoke", "Run resize/readback checks and save a PNG.", "output"});
    parser.addOption({"validation", "Require Vulkan Validation Layers."});
    parser.process(application);
    const bool smoke = parser.isSet("smoke");
    const bool validation = parser.isSet("validation") || smoke;
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
        QMainWindow window;
        window.setWindowTitle("MiniRender — A0 基础验证");
        auto* viewport = new VulkanViewport;
        viewport->setVulkanInstance(&instance);
        auto* container = QWidget::createWindowContainer(viewport, &window);
        container->setMinimumSize(320, 240);
        window.setCentralWidget(container);

        auto* dock = new QDockWidget("验证内容", &window);
        auto* description = new QLabel("<h3>Vulkan 基础验证</h3>"
                                       "<p>棋盘：SRGB 纹理上传与采样</p>"
                                       "<p>红边：远处三角形</p>"
                                       "<p>中心：深度测试保留近处纹理</p>"
                                       "<p>缩放窗口可检查交换链重建。</p>"
                                       "<p>场景编辑与离线渲染将在后续检查点加入。</p>",
                                       dock);
        description->setMargin(16);
        description->setWordWrap(true);
        dock->setWidget(description);
        window.addDockWidget(Qt::RightDockWidgetArea, dock);
        auto* toolbar = window.addToolBar("工具");
        auto* save = toolbar->addAction("保存视口 PNG");
        QObject::connect(save, &QAction::triggered, &window, [&] {
            if (!viewport->isValid() || !viewport->supportsGrab()) {
                QMessageBox::warning(&window, "无法截图", "当前交换链不支持图像回读。");
                return;
            }
            const auto path = QFileDialog::getSaveFileName(&window, "保存视口", {}, "PNG (*.png)");
            if (!path.isEmpty() && !viewport->grab().save(path)) {
                QMessageBox::warning(&window, "保存失败", "无法写入图像文件。");
            }
        });

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
            if (viewport->isValid()) {
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
        window.resize(960, 640);
        window.show();
        timer.start();
        result = application.exec();
    }
    // 销毁窗口及 GPU 资源后再判断，才能捕获 shutdown 期间的 Validation 错误。
    instance.destroy();
    qInfo() << "Vulkan validation errors:" << validation_errors.load();
    return validation_errors.load() == 0 ? result : 1;
}
