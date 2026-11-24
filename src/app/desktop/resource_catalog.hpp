// 资源包索引、包内相对路径和 SHA-256 校验。读取阶段不修改编辑器文档。
#pragma once
#include <QList>
#include <QString>
#include <functional>

namespace catalog {
// 文件路径相对于包根目录。摘要对应磁盘上的原始字节。
struct File {
    QString    path;
    QByteArray sha256;
};

enum class Kind { scene, model, sequence };

// 一个可浏览的资源。dependencies 包含场景 JSON 引用的网格和纹理。
struct Entry {
    QString     root, id, name, description, source, rights, thumbnail;
    Kind        kind = Kind::scene;
    File        file;
    QList<File> dependencies;
    bool        builtin = false;
};

// 读取索引及元数据，保留清单顺序。文件内容在用户选择导入时校验。
QList<Entry> read(const QString& root, bool bundled = false);
// 返回已验证位于资源包内的真实文件路径。
QString resolve(const QString& root, const QString& relative);
// 按块读取文件并校验摘要，调用方可在回调中报告进度或取消。
void verify(const QString& root, const File& file,
            const std::function<void(qint64)>& advanced = {});
// 读取帧清单，帧内容在播放或跳转到该帧时加载。
QList<File> frames(const Entry& entry);
// 校验场景及其直接引用，返回场景路径；清单缺项时在解析模型前报错。
QString verify_scene(const Entry& entry, const std::function<void(qint64)>& advanced);
} // namespace catalog
