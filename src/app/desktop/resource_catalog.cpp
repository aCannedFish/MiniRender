// 解析资源目录，限制文件在包内，并在后台导入时检查内容完整性。
#include "resource_catalog.hpp"
#include "builtin_scenes.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <stdexcept>

namespace catalog {
namespace {
[[noreturn]] void fail(const QString& why) {
    throw std::runtime_error(why.toStdString());
}

QJsonDocument json(const QString& path) {
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) {
        fail(input.errorString() + " · " + path);
    }
    QJsonParseError error;
    auto            doc = QJsonDocument::fromJson(input.readAll(), &error);
    if (error.error != QJsonParseError::NoError) {
        fail(path + " · " + error.errorString());
    }
    return doc;
}

QString field(const QJsonObject& object, const char* key) {
    auto value = object.value(key).toString();
    if (value.isEmpty()) {
        fail(QString("缺少资源字段 %1").arg(key));
    }
    return value;
}

File parse_file(const QJsonObject& object) {
    File result{field(object, "path"), field(object, "sha256").toLatin1().toLower()};
    if (result.sha256.size() != 64 || QByteArray::fromHex(result.sha256).toHex() != result.sha256) {
        fail("SHA-256 格式无效 · " + result.path);
    }
    return result;
}
} // namespace

QString resolve(const QString& root, const QString& relative) {
    QDir       directory(QFileInfo(root).canonicalFilePath());
    const auto real  = QFileInfo(directory.filePath(relative)).canonicalFilePath();
    const auto local = directory.relativeFilePath(real);
    if (relative.isEmpty() || QDir::isAbsolutePath(relative) || real.isEmpty() || local == ".." ||
        local.startsWith("../") || QDir::isAbsolutePath(local) || !QFileInfo(real).isFile()) {
        fail("包内文件缺失或路径无效 · " + relative);
    }
    return real;
}

QList<Entry> read(const QString& root, bool bundled) {
    auto doc = json(resolve(root, "resource_catalog.json")).object();
    if (doc.value("version").toInt() != 1 || !doc.value("entries").isArray()) {
        fail("不支持的资源目录格式");
    }
    QList<Entry>  entries;
    QSet<QString> ids;
    for (auto value : doc.value("entries").toArray()) {
        auto  item = value.toObject();
        Entry e;
        e.root = QFileInfo(root).canonicalFilePath();
        e.id   = field(item, "id");
        if (ids.contains(e.id)) {
            fail("目录标识重复 · " + e.id);
        }
        ids.insert(e.id);
        e.name          = field(item, "name");
        e.file          = parse_file(item);
        e.source        = field(item, "source");
        e.rights        = field(item, "rights");
        e.thumbnail     = resolve(e.root, field(item, "thumbnail"));
        e.description   = item.value("description").toString();
        const auto kind = field(item, "kind");
        if (kind == "scene") {
            e.kind = Kind::scene;
        } else if (kind == "model") {
            e.kind = Kind::model;
        } else if (kind == "sequence") {
            e.kind = Kind::sequence;
        } else {
            fail("未知资源类型 · " + kind);
        }
        e.recommended = item.value("recommended").toString();
        if (e.id == "mary-studio" && e.recommended.isEmpty()) {
            e.recommended = "blinn-phong";
        }
        e.builtin = bundled && item.value("builtin").toBool();
        if (e.builtin) {
            const auto& presets = builtin_scenes();
            const auto  found   = std::find_if(presets.begin(), presets.end(), [&](const auto& p) {
                return p.id == e.id && p.filename == e.file.path;
            });
            if (found == presets.end()) {
                fail("内置模板与目录不一致 · " + e.id);
            }
            e.name        = found->name;
            e.description = found->description + QString("  推荐 %1 × %2，%3 spp。")
                                                     .arg(found->render.width)
                                                     .arg(found->render.height)
                                                     .arg(found->render.samples_per_pixel);
        }
        for (auto dep : item.value("dependencies").toArray()) {
            e.dependencies.push_back(parse_file(dep.toObject()));
        }
        entries.push_back(std::move(e));
    }
    if (entries.empty()) {
        fail("资源目录为空");
    }
    return entries;
}

void verify(const QString& root, const File& file, const std::function<void(qint64)>& advanced) {
    QFile input(resolve(root, file.path));
    if (!input.open(QIODevice::ReadOnly)) {
        fail(input.errorString());
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    char               block[65536];
    for (;;) {
        const auto size = input.read(block, sizeof block);
        if (size < 0) {
            fail(input.errorString());
        }
        if (size == 0) {
            break;
        }
        hash.addData(QByteArrayView(block, size));
        if (advanced) {
            advanced(size);
        }
    }
    if (hash.result().toHex() != file.sha256) {
        fail("文件校验失败 · " + file.path);
    }
}

QString verify_scene(const Entry& entry, const std::function<void(qint64)>& advanced) {
    verify(entry.root, entry.file, advanced);
    const auto    scene = resolve(entry.root, entry.file.path);
    QSet<QString> verified;
    for (const auto& dep : entry.dependencies) {
        verify(entry.root, dep, advanced);
        verified.insert(resolve(entry.root, dep.path));
    }
    const auto doc    = json(scene).object();
    const auto folder = QFileInfo(scene).dir();
    for (const auto key : {"meshes", "textures"}) {
        for (auto item : doc.value(key).toArray()) {
            const auto asset = field(item.toObject(), "source");
            if (QDir::isAbsolutePath(asset) ||
                !verified.contains(QFileInfo(folder.filePath(asset)).canonicalFilePath())) {
                fail("场景引用了清单外的文件 · " + asset);
            }
        }
    }
    return scene;
}

QList<File> frames(const Entry& entry) {
    verify(entry.root, entry.file);
    QList<File> result;
    for (auto value : json(resolve(entry.root, entry.file.path)).array()) {
        result.push_back(parse_file(value.toObject()));
    }
    if (result.empty()) {
        fail("序列中缺少帧");
    }
    return result;
}
} // namespace catalog
