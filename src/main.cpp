#include <cstdlib>
#include <cstring>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>
#include <QMetaObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>
#include <QVariant>
#include <QWindow>
#include <QtDebug>

#include "xovi.h"

static QQmlEngine *g_engine = nullptr;
static QObject *g_library = nullptr;
static QObject *g_libraryController = nullptr;
static QObject *g_invoker = nullptr;

extern "C" {
    const char *buildTree(const char *rmPath);
    const char *convertToJson(const char *treeId);
    int destroyTree(const char *treeId);
}


static bool ensureEngine()
{
    if (g_engine)
        return true;
    const auto windows = QGuiApplication::allWindows();
    for (QWindow *window : windows) {
        QQmlEngine *engine = qmlEngine(window);
        if (engine) {
            g_engine = engine;
            return true;
        }
    }
    return false;
}

static bool resolveLibrary()
{
    if (g_library && g_invoker)
        return true;

    if (!ensureEngine())
        return false;

    static const char *kHelperQml = R"QML(
        import QtQml
        import xofm.libs.library
        QtObject {
            property var lib: Library
            property var ctrl: LibraryController
            function callCtrl(m, a) { return LibraryController[m].apply(LibraryController, a); }
            function callLib(m, a) { return Library[m].apply(Library, a); }
            function idList(uuids) {
                var a = [];
                for (var i = 0; i < uuids.length; ++i) {
                    var w = Library.entryForId(uuids[i]);
                    if (w) a.push(w.id);
                }
                return a;
            }
            function callList(m, uuids, rest) {
                return LibraryController[m].apply(LibraryController, [idList(uuids)].concat(rest));
            }
            function notify(id) { Library.requestLoadEntry(id); Library.entryAdded(id); }
        }
    )QML";

    QQmlComponent component(g_engine);
    component.setData(kHelperQml, QUrl());
    QObject *holder = component.create(g_engine->rootContext());
    if (!holder) {
        const auto errors = component.errors();
        for (const QQmlError &error : errors)
            qWarning() << "[librarian]: QML helper error:" << error;
        return false;
    }
    holder->setParent(g_engine);

    QVariant v = holder->property("lib");
    QObject *lib = v.isValid() ? v.value<QObject *>() : nullptr;
    if (!lib) {
        qWarning() << "[librarian]: Library not found";
        return false;
    }
    g_invoker = holder;
    g_library = lib;
    qInfo() << "[librarian]: Library resolved";

    QVariant cv = holder->property("ctrl");
    QObject *ctrl = cv.isValid() ? cv.value<QObject *>() : nullptr;
    if (ctrl) {
        g_libraryController = ctrl;
        qInfo() << "[librarian]: LibraryController resolved";
    } else {
        qWarning() << "[librarian]: LibraryController not found";
    }

    return true;
}

static QVariant callInvoker(const char *fn, const QString &method, const QVariantList &args)
{
    QVariant ret;
    QMetaObject::invokeMethod(g_invoker, fn, Qt::DirectConnection,
        Q_RETURN_ARG(QVariant, ret),
        Q_ARG(QVariant, QVariant(method)),
        Q_ARG(QVariant, QVariant(args)));
    return ret;
}

static QVariant ctrlCall(const QString &method, const QVariantList &args)
{
    return callInvoker("callCtrl", method, args);
}

static QVariant libCall(const QString &method, const QVariantList &args)
{
    return callInvoker("callLib", method, args);
}

static QVariant ctrlCallList(const QString &method, const QStringList &uuids,
                             const QVariantList &rest = {})
{
    QVariant ret;
    QMetaObject::invokeMethod(g_invoker, "callList", Qt::DirectConnection,
        Q_RETURN_ARG(QVariant, ret),
        Q_ARG(QVariant, QVariant(method)),
        Q_ARG(QVariant, QVariant(uuids)),
        Q_ARG(QVariant, QVariant(rest)));
    return ret;
}

static QString variantToId(QVariant v)
{
    if (!v.isValid())
        return QString();
    if (v.typeId() == QMetaType::QString)
        return v.toString();
    if (v.convert(QMetaType(QMetaType::QString)))
        return v.toString();
    return QString();
}

static void ensureInitialization()
{
    if (!ensureEngine()) {
        QTimer::singleShot(200, []() { ensureInitialization(); });
        return;
    }
    if (!resolveLibrary()) {
        QTimer::singleShot(200, []() { ensureInitialization(); });
    }
}


static QString xochitlDir()
{
    QString dir = qgetenv("XOCHITL_DIR");
    if (dir.isEmpty())
        dir = "/home/root/.local/share/remarkable/xochitl";
    return dir;
}

static QString stripExtension(const QString &filename)
{
    int dot = filename.lastIndexOf('.');
    if (dot > 0)
        return filename.left(dot);
    return filename;
}

static QString detectFileType(const QString &suffix)
{
    if (suffix.compare("pdf", Qt::CaseInsensitive) == 0)
        return "pdf";
    if (suffix.compare("epub", Qt::CaseInsensitive) == 0)
        return "epub";
    return QString();
}

static void notifyLibrary(const QString &uuid)
{
    if (!g_invoker)
        return;

    QMetaObject::invokeMethod(g_invoker, "notify", Qt::QueuedConnection,
        Q_ARG(QVariant, QVariant(uuid)));
}

static QString createFolderOnDisk(const QString &name, const QString &parentId)
{
    QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    QString timestamp = QString::number(QDateTime::currentMSecsSinceEpoch());
    QString dir = xochitlDir();

    QJsonObject metadata;
    metadata["createdTime"] = timestamp;
    metadata["lastModified"] = timestamp;
    metadata["parent"] = parentId;
    metadata["pinned"] = false;
    metadata["type"] = "CollectionType";
    metadata["visibleName"] = name;

    QFile file(QString("%1/%2.metadata").arg(dir, uuid));
    if (!file.open(QIODevice::WriteOnly))
        return QString();
    file.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented));
    file.close();

    notifyLibrary(uuid);
    return uuid;
}


static const QRegularExpression kUuidRe(
    "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");

struct MetaEntry {
    QString uuid;
    QString visibleName;
    QString parent;
    QString type;
};

static QList<MetaEntry> loadMetadataIndex()
{
    QList<MetaEntry> entries;
    QString dir = xochitlDir();
    QDir d(dir);
    QStringList files = d.entryList(QStringList() << "*.metadata", QDir::Files);
    for (const QString &f : files) {
        QFile file(dir + "/" + f);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
        file.close();

        MetaEntry e;
        e.uuid = f.left(f.length() - 9); // strip ".metadata"
        e.visibleName = obj["visibleName"].toString();
        e.parent = obj["parent"].toString();
        e.type = obj["type"].toString();
        entries.append(e);
    }
    return entries;
}

static QString resolveId(const QString &input)
{
    if (input.isEmpty())
        return QString();

    if (kUuidRe.match(input).hasMatch())
        return input;

    bool anchored = input.startsWith('/');
    QString cleaned = anchored ? input.mid(1) : input;
    if (cleaned.isEmpty())
        return QString();

    QList<MetaEntry> entries = loadMetadataIndex();
    QStringList parts = cleaned.split('/');

    QString parentId;
    for (int i = 0; i < parts.size(); i++) {
        const QString &name = parts[i];
        bool isLast = (i == parts.size() - 1);
        QStringList matches;

        for (const MetaEntry &e : entries) {
            if (e.visibleName != name || e.parent != parentId)
                continue;
            if (!isLast && e.type != "CollectionType")
                continue;
            matches.append(e.uuid);
        }

        if (matches.isEmpty() && !anchored && parts.size() == 1) {
            for (const MetaEntry &e : entries) {
                if (e.visibleName == name && e.parent != "trash")
                    matches.append(e.uuid);
            }
        }

        if (matches.isEmpty()) {
            return "not found: " + name;
        }
        if (matches.size() > 1) {
            return "ambiguous: " + name + " matches " + matches.join(", ");
        }
        parentId = matches.first();
    }

    return parentId;
}

static QString resolveIdInTrash(const QString &input)
{
    if (input.isEmpty())
        return QString();

    if (kUuidRe.match(input).hasMatch())
        return input;

    QList<MetaEntry> entries = loadMetadataIndex();
    QStringList matches;

    for (const MetaEntry &e : entries) {
        if (e.visibleName == input && e.parent == "trash")
            matches.append(e.uuid);
    }

    if (matches.isEmpty()) {
        return "not found in trash: " + input;
    }
    if (matches.size() > 1) {
        return "ambiguous in trash: " + input + " matches " + matches.join(", ");
    }
    return matches.first();
}

static char *result(const QString &s)
{
    if (s.isEmpty())
        return nullptr;
    return strdup(s.toUtf8().constData());
}

static char *error(const QString &msg)
{
    qWarning() << "[librarian]:" << msg;
    return strdup(("ERROR: " + msg).toUtf8().constData());
}

static bool isError(const QString &resolved)
{
    return !resolved.isEmpty() && !kUuidRe.match(resolved).hasMatch();
}

#define RESOLVE(var, expr) \
    QString var = (expr); \
    if (isError(var)) return error(var)

static int findArgSeparator(const QString &input)
{
    if (input.length() > 36 && input[36] == ',' &&
        kUuidRe.match(input.left(36)).hasMatch())
        return 36;
    return input.lastIndexOf(',');
}


extern "C" char *rescanLibrary(const char *params)
{
    (void)params;
    if (!g_library)
        return error("library not resolved");
    if (!g_library->property("isReady").toBool())
        return error("library not ready");

    QList<MetaEntry> entries = loadMetadataIndex();
    int loaded = 0;

    QMetaObject::invokeMethod(g_engine, [&entries, &loaded]() {
        for (const MetaEntry &e : entries) {
            QString parentId = variantToId(libCall("parentIdForId", { e.uuid }));
            if (parentId.isEmpty()) {
                notifyLibrary(e.uuid);
                loaded++;
            }
        }
    }, Qt::BlockingQueuedConnection);

    qInfo() << "[librarian]: rescanned" << entries.size() << "entries," << loaded << "new";
    return result(QString::number(loaded));
}

extern "C" char *lookupEntry(const char *params)
{
    if (!params || params[0] == '\0')
        return error("empty input");

    QString input = QString::fromUtf8(params);

    if (kUuidRe.match(input).hasMatch())
        return result(input);

    bool anchored = input.startsWith('/');
    QString cleaned = anchored ? input.mid(1) : input;
    if (cleaned.isEmpty())
        return error("empty input");

    QList<MetaEntry> entries = loadMetadataIndex();
    QStringList matches;

    if (cleaned.contains('/')) {
        QString resolved = resolveId(input);
        if (isError(resolved))
            return error(resolved);
        return result(resolved);
    }

    for (const MetaEntry &e : entries) {
        if (e.visibleName == cleaned && e.parent != "trash") {
            if (!anchored || e.parent.isEmpty())
                matches.append(e.uuid);
        }
    }

    if (matches.isEmpty())
        return error("not found: " + cleaned);

    return result(matches.join("\n"));
}


extern "C" char *importDocument(const char *params)
{
    if (!params || params[0] == '\0')
        return error("empty input");

    QString input = QString::fromUtf8(params);
    QString filePath = input;
    QString parent;

    int sep = findArgSeparator(input);
    if (sep > 0) {
        filePath = input.left(sep);
        QString parentInput = input.mid(sep + 1);
        if (!parentInput.isEmpty()) {
            RESOLVE(p, resolveId(parentInput));
            parent = p;
        }
    }

    QFileInfo srcInfo(filePath);
    if (!srcInfo.exists())
        return error("file does not exist: " + filePath);

    QString dir = xochitlDir();
    bool isRmdoc = srcInfo.suffix().compare("rmdoc", Qt::CaseInsensitive) == 0;

    if (isRmdoc) {
        QProcess listProc;
        listProc.start("unzip", QStringList() << "-l" << srcInfo.absoluteFilePath());
        if (!listProc.waitForFinished(5000)) {
            if (listProc.error() == QProcess::FailedToStart)
                return error("unzip not found");
            return error("unzip listing timed out");
        }
        if (listProc.exitCode() != 0)
            return error("failed to list rmdoc: " + QString::fromUtf8(listProc.readAllStandardError()));

        QString uuid;
        QString listing = QString::fromUtf8(listProc.readAllStandardOutput());
        for (const QString &line : listing.split('\n')) {
            QString name = line.simplified().section(' ', -1);
            if (name.endsWith(".metadata") && !name.contains('/')) {
                uuid = name.chopped(9);
                break;
            }
        }
        if (uuid.isEmpty())
            return error("no .metadata found in rmdoc archive");

        QProcess extractProc;
        extractProc.start("unzip", QStringList() << "-o" << srcInfo.absoluteFilePath() << "-d" << dir);
        if (!extractProc.waitForFinished(30000)) {
            if (extractProc.error() == QProcess::FailedToStart)
                return error("unzip not found");
            return error("unzip extraction timed out");
        }
        if (extractProc.exitCode() != 0)
            return error("unzip failed: " + QString::fromUtf8(extractProc.readAllStandardError()));

        if (!parent.isEmpty()) {
            QString metaPath = QString("%1/%2.metadata").arg(dir, uuid);
            QFile metaFile(metaPath);
            if (metaFile.open(QIODevice::ReadOnly)) {
                QJsonObject meta = QJsonDocument::fromJson(metaFile.readAll()).object();
                metaFile.close();
                meta["parent"] = parent;
                if (metaFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    metaFile.write(QJsonDocument(meta).toJson(QJsonDocument::Indented));
                    metaFile.close();
                }
            }
        }

        notifyLibrary(uuid);

        qInfo() << "[librarian]: imported rmdoc" << filePath << "uuid" << uuid;
        return result(uuid);
    }

    QString fileType = detectFileType(srcInfo.suffix());
    if (fileType.isEmpty())
        return error("unsupported file type: " + srcInfo.suffix());

    QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    QString timestamp = QString::number(QDateTime::currentMSecsSinceEpoch());
    QString visibleName = stripExtension(srcInfo.fileName());

    if (!QFile::copy(srcInfo.absoluteFilePath(), QString("%1/%2.%3").arg(dir, uuid, fileType)))
        return error("failed to copy file");

    QJsonObject metadata;
    metadata["createdTime"] = timestamp;
    metadata["lastModified"] = timestamp;
    metadata["lastOpened"] = "0";
    metadata["lastOpenedPage"] = 0;
    metadata["parent"] = parent;
    metadata["pinned"] = false;
    metadata["type"] = "DocumentType";
    metadata["visibleName"] = visibleName;

    QFile metaFile(QString("%1/%2.metadata").arg(dir, uuid));
    if (!metaFile.open(QIODevice::WriteOnly))
        return nullptr;
    metaFile.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented));
    metaFile.close();

    QJsonObject content;
    content["coverPageNumber"] = -1;
    content["documentMetadata"] = QJsonObject();
    content["extraMetadata"] = QJsonObject();
    content["fileType"] = fileType;
    content["fontName"] = QString();
    content["formatVersion"] = 2;
    content["lineHeight"] = -1;
    content["margins"] = 125;
    content["orientation"] = "portrait";
    content["pageCount"] = 0;
    content["pageTags"] = QJsonArray();
    content["tags"] = QJsonArray();
    content["textScale"] = 1;
    content["transform"] = QJsonObject();

    QFile contentFile(QString("%1/%2.content").arg(dir, uuid));
    if (!contentFile.open(QIODevice::WriteOnly))
        return nullptr;
    contentFile.write(QJsonDocument(content).toJson(QJsonDocument::Indented));
    contentFile.close();

    notifyLibrary(uuid);

    qInfo() << "[librarian]: imported" << filePath << "as" << visibleName << "uuid" << uuid;
    return result(uuid);
}


extern "C" char *importImage(const char *params)
{
    if (!params || params[0] == '\0')
        return error("empty input");

    QString input = QString::fromUtf8(params);
    QString filePath = input;
    QString parent;

    int sep = findArgSeparator(input);
    if (sep > 0) {
        filePath = input.left(sep);
        QString parentInput = input.mid(sep + 1);
        if (!parentInput.isEmpty()) {
            RESOLVE(p, resolveId(parentInput));
            parent = p;
        }
    }

    QImage image(filePath);
    if (image.isNull())
        return error("failed to load image: " + filePath);

    QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    QString timestamp = QString::number(QDateTime::currentMSecsSinceEpoch());
    QString dir = xochitlDir();
    QString visibleName = stripExtension(QFileInfo(filePath).fileName());

    QString pdfPath = QString("%1/%2.pdf").arg(dir, uuid);

    QPdfWriter writer(pdfPath);
    writer.setPageSize(QPageSize(image.size(), QPageSize::Unit::Point));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));
    writer.setResolution(72);

    QPainter painter(&writer);
    if (!painter.isActive()) {
        QFile::remove(pdfPath);
        return error("failed to create PDF");
    }
    painter.drawImage(painter.viewport(), image);
    painter.end();

    QJsonObject metadata;
    metadata["createdTime"] = timestamp;
    metadata["lastModified"] = timestamp;
    metadata["lastOpened"] = "0";
    metadata["lastOpenedPage"] = 0;
    metadata["parent"] = parent;
    metadata["pinned"] = false;
    metadata["type"] = "DocumentType";
    metadata["visibleName"] = visibleName;

    QFile metaFile(QString("%1/%2.metadata").arg(dir, uuid));
    if (!metaFile.open(QIODevice::WriteOnly))
        return error("failed to write metadata");
    metaFile.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented));
    metaFile.close();

    QJsonObject content;
    content["coverPageNumber"] = 0;
    content["documentMetadata"] = QJsonObject();
    content["extraMetadata"] = QJsonObject();
    content["fileType"] = "pdf";
    content["fontName"] = QString();
    content["formatVersion"] = 2;
    content["lineHeight"] = -1;
    content["margins"] = 125;
    content["orientation"] = "portrait";
    content["pageCount"] = 1;
    content["pageTags"] = QJsonArray();
    content["tags"] = QJsonArray();
    content["textScale"] = 1;
    content["transform"] = QJsonObject();

    QFile contentFile(QString("%1/%2.content").arg(dir, uuid));
    if (!contentFile.open(QIODevice::WriteOnly))
        return error("failed to write content");
    contentFile.write(QJsonDocument(content).toJson(QJsonDocument::Indented));
    contentFile.close();

    notifyLibrary(uuid);

    qInfo() << "[librarian]: imported image" << filePath << "as" << visibleName << "uuid" << uuid;
    return result(uuid);
}

#define REQUIRE_CTRL() do { \
    if (!g_libraryController || !g_invoker || !g_engine) return error("extension not initialized"); \
} while(0)

static bool sceneHasInk(const QJsonObject &root)
{
    const QJsonValue rootText = root.value("rootText");
    if (rootText.isObject() && !rootText.toObject().isEmpty())
        return true;

    static const int kEraserTool = 6;
    static const int kEraserAreaTool = 8;

    const QJsonObject nodes = root.value("nodes").toObject();
    for (auto it = nodes.begin(); it != nodes.end(); ++it) {
        const QJsonObject node = it.value().toObject();
        if (!node.value("visible").toObject().value("value").toBool(true))
            continue;
        const QJsonValue childrenVal = node.value("children");
        if (!childrenVal.isArray())
            continue;
        for (const QJsonValue &cv : childrenVal.toArray()) {
            const QJsonObject item = cv.toObject();
            const QString type = item.value("_type").toString();
            if (type != QLatin1String("Line") && type != QLatin1String("Image")
                && type != QLatin1String("Text") && type != QLatin1String("GlyphRange"))
                continue;
            if (item.value("deletedLength").toInt() != 0)
                continue;
            const QJsonValue value = item.value("value");
            if (value.isNull() || value.isUndefined())
                continue;
            if (type == QLatin1String("Line")) {
                const QJsonObject line = value.toObject();
                const int tool = line.value("tool").toInt(-1);
                if (tool == kEraserTool || tool == kEraserAreaTool)
                    continue;
                if (line.value("points").toArray().isEmpty())
                    continue;
            }
            return true;
        }
    }
    return false;
}

static bool rmFileHasInk(const QString &rmPath)
{
    const QByteArray pathBytes = rmPath.toUtf8();
    const char *tid = buildTree(pathBytes.constData());
    if (!tid || !*tid)
        return false;
    const QByteArray treeId(tid);
    bool ink = false;
    const char *js = convertToJson(treeId.constData());
    if (js)
        ink = sceneHasInk(QJsonDocument::fromJson(QByteArray(js)).object());
    destroyTree(treeId.constData());
    return ink;
}

extern "C" char *getContentPages(const char *params)
{
    QString input = QString::fromUtf8(params ? params : "");
    if (input.isEmpty())
        return error("empty input");
    RESOLVE(uuid, resolveId(input));

    const QString dir = xochitlDir();
    QFile cf(QString("%1/%2.content").arg(dir, uuid));
    if (!cf.open(QIODevice::ReadOnly))
        return error("cannot read content for " + uuid);
    const QJsonObject content = QJsonDocument::fromJson(cf.readAll()).object();
    cf.close();

    QStringList pageIds;
    const QJsonObject cPages = content.value("cPages").toObject();
    if (cPages.contains("pages")) {
        for (const QJsonValue &pv : cPages.value("pages").toArray()) {
            const QJsonObject po = pv.toObject();
            if (po.contains("deleted"))
                continue;
            const QString id = po.value("id").toString();
            if (!id.isEmpty())
                pageIds << id;
        }
    } else {
        for (const QJsonValue &pv : content.value("pages").toArray())
            if (!pv.toString().isEmpty())
                pageIds << pv.toString();
    }

    const QString pdir = QString("%1/%2").arg(dir, uuid);
    QStringList inked;
    for (const QString &pid : pageIds) {
        const QString rm = QString("%1/%2.rm").arg(pdir, pid);
        if (QFile::exists(rm) && rmFileHasInk(rm))
            inked << pid;
    }

    qInfo() << "[librarian]: getContentPages" << uuid << inked.size() << "of" << pageIds.size();
    return strdup(inked.join('\n').toUtf8().constData());
}

extern "C" char *renameEntry(const char *params)
{
    REQUIRE_CTRL();
    QString input = QString::fromUtf8(params ? params : "");
    int sep = findArgSeparator(input);
    if (sep < 0) return error("expected: nameOrId,newName");

    RESOLVE(id, resolveId(input.left(sep)));
    QString newName = input.mid(sep + 1);
    if (newName.isEmpty()) return error("new name is empty");

    bool ok = false;
    QMetaObject::invokeMethod(g_engine, [&]() {
        ok = ctrlCall("setVisibleName", { id, newName }).toBool();
    }, Qt::BlockingQueuedConnection);
    qInfo() << "[librarian]: rename" << id << "→" << newName << (ok ? "ok" : "FAILED");
    return strdup(ok ? "ok" : "FAILED");
}

extern "C" char *moveEntry(const char *params)
{
    REQUIRE_CTRL();
    QString input = QString::fromUtf8(params ? params : "");
    int sep = findArgSeparator(input);
    if (sep < 0) return error("expected: nameOrId,parentNameOrId");

    RESOLVE(id, resolveId(input.left(sep)));
    QString destInput = input.mid(sep + 1);
    QString parentId;
    if (!destInput.isEmpty()) {
        RESOLVE(p, resolveId(destInput));
        parentId = p;
    }

    bool ok = false;
    QMetaObject::invokeMethod(g_engine, [&]() {
        ok = ctrlCallList("moveEntries", { id }, { parentId }).toBool();
    }, Qt::BlockingQueuedConnection);
    qInfo() << "[librarian]: move" << id << "→" << parentId << (ok ? "ok" : "FAILED");
    return strdup(ok ? "ok" : "FAILED");
}

extern "C" char *trashEntry(const char *params)
{
    REQUIRE_CTRL();
    RESOLVE(id, resolveId(QString::fromUtf8(params ? params : "")));

    bool ok = false;
    QMetaObject::invokeMethod(g_engine, [&]() {
        ok = ctrlCallList("moveEntriesToTrash", { id }).toBool();
    }, Qt::BlockingQueuedConnection);
    qInfo() << "[librarian]: trash" << id << (ok ? "ok" : "FAILED");
    return strdup(ok ? "ok" : "FAILED");
}

extern "C" char *restoreEntry(const char *params)
{
    REQUIRE_CTRL();
    RESOLVE(id, resolveIdInTrash(QString::fromUtf8(params ? params : "")));

    bool ok = false;
    QMetaObject::invokeMethod(g_engine, [&]() {
        ok = ctrlCallList("restoreEntriesFromTrash", { id }).toBool();
    }, Qt::BlockingQueuedConnection);
    qInfo() << "[librarian]: restore" << id << (ok ? "ok" : "FAILED");
    return strdup(ok ? "ok" : "FAILED");
}

extern "C" char *deleteEntry(const char *params)
{
    REQUIRE_CTRL();
    RESOLVE(id, resolveId(QString::fromUtf8(params ? params : "")));

    bool ok = false;
    QMetaObject::invokeMethod(g_engine, [&]() {
        ok = ctrlCallList("deleteEntries", { id }).toBool();
    }, Qt::BlockingQueuedConnection);
    qInfo() << "[librarian]: delete" << id << (ok ? "ok" : "FAILED");
    return strdup(ok ? "ok" : "FAILED");
}

extern "C" char *cloneEntry(const char *params)
{
    REQUIRE_CTRL();
    QString input = QString::fromUtf8(params ? params : "");
    int sep = findArgSeparator(input);
    if (sep < 0) return error("expected: nameOrId,parentNameOrId");

    RESOLVE(id, resolveId(input.left(sep)));
    QString destInput = input.mid(sep + 1);
    QString parentId;
    if (!destInput.isEmpty()) {
        RESOLVE(p, resolveId(destInput));
        parentId = p;
    }

    QString newId;
    QMetaObject::invokeMethod(g_engine, [&]() {
        newId = variantToId(ctrlCall("cloneEntry", { id, parentId }));
    }, Qt::BlockingQueuedConnection);
    qInfo() << "[librarian]: clone" << id << "→" << newId;
    return result(newId);
}

extern "C" char *createNotebook(const char *params)
{
    REQUIRE_CTRL();
    QString input = QString::fromUtf8(params ? params : "");
    QString name = input;
    QString parentId;

    int sep = findArgSeparator(input);
    if (sep >= 0) {
        name = input.left(sep);
        QString destInput = input.mid(sep + 1);
        if (!destInput.isEmpty()) {
            RESOLVE(p, resolveId(destInput));
            parentId = p;
        }
    }
    if (name.isEmpty()) return error("name is empty");

    QString newId;
    QMetaObject::invokeMethod(g_engine, [&]() {
        newId = variantToId(ctrlCall("createDocument", { parentId, name }));
    }, Qt::BlockingQueuedConnection);
    qInfo() << "[librarian]: createNotebook" << name << "→" << newId;
    return result(newId);
}

extern "C" char *setPinned(const char *params)
{
    REQUIRE_CTRL();
    QString input = QString::fromUtf8(params ? params : "");
    int sep = findArgSeparator(input);
    if (sep < 0) return error("expected: nameOrId,true/false");

    RESOLVE(id, resolveId(input.left(sep)));
    bool pinned = input.mid(sep + 1).trimmed().compare("true", Qt::CaseInsensitive) == 0;

    bool ok = false;
    QMetaObject::invokeMethod(g_engine, [&]() {
        ok = ctrlCall("setPinned", { id, pinned }).toBool();
    }, Qt::BlockingQueuedConnection);
    qInfo() << "[librarian]: setPinned" << id << pinned << (ok ? "ok" : "FAILED");
    return strdup(ok ? "ok" : "FAILED");
}

extern "C" char *setCover(const char *params)
{
    REQUIRE_CTRL();
    QString input = QString::fromUtf8(params ? params : "");
    int sep = findArgSeparator(input);
    if (sep < 0) return error("expected: nameOrId,pageNumber");

    RESOLVE(id, resolveId(input.left(sep)));
    int page = input.mid(sep + 1).trimmed().toInt();

    bool ok = false;
    QMetaObject::invokeMethod(g_engine, [&]() {
        ok = ctrlCall("setCoverPageNumber", { id, page }).toBool();
    }, Qt::BlockingQueuedConnection);
    qInfo() << "[librarian]: setCover" << id << page << (ok ? "ok" : "FAILED");
    return strdup(ok ? "ok" : "FAILED");
}

extern "C" char *setOrientation(const char *params)
{
    REQUIRE_CTRL();
    QString input = QString::fromUtf8(params ? params : "");
    int sep = findArgSeparator(input);
    if (sep < 0) return error("expected: nameOrId,portrait/landscape");

    RESOLVE(id, resolveId(input.left(sep)));
    QString val = input.mid(sep + 1).trimmed().toLower();
    int orientation = (val == "landscape" || val == "horizontal") ? Qt::Horizontal : Qt::Vertical;

    bool ok = false;
    QMetaObject::invokeMethod(g_engine, [&]() {
        ok = ctrlCall("setOrientation", { id, orientation }).toBool();
    }, Qt::BlockingQueuedConnection);
    qInfo() << "[librarian]: setOrientation" << id << orientation << (ok ? "ok" : "FAILED");
    return strdup(ok ? "ok" : "FAILED");
}

extern "C" char *setTags(const char *params)
{
    REQUIRE_CTRL();
    QString input = QString::fromUtf8(params ? params : "");
    int sep = findArgSeparator(input);
    if (sep < 0) return error("expected: nameOrId,tag1;tag2");

    RESOLVE(id, resolveId(input.left(sep)));
    QStringList tags = input.mid(sep + 1).split(';', Qt::SkipEmptyParts);

    bool ok = false;
    QMetaObject::invokeMethod(g_engine, [&]() {
        ok = ctrlCall("setEntryTags", { id, tags, QStringList() }).toBool();
    }, Qt::BlockingQueuedConnection);
    qInfo() << "[librarian]: setTags" << id << tags << (ok ? "ok" : "FAILED");
    return strdup(ok ? "ok" : "FAILED");
}

extern "C" char *createFolder(const char *params)
{
    REQUIRE_CTRL();
    QString input = QString::fromUtf8(params ? params : "");
    QString name = input;
    QString parentId;

    int sep = findArgSeparator(input);
    if (sep >= 0) {
        name = input.left(sep);
        QString destInput = input.mid(sep + 1);
        if (!destInput.isEmpty()) {
            RESOLVE(p, resolveId(destInput));
            parentId = p;
        }
    }
    if (name.isEmpty()) return error("name is empty");

    QString newId;
    QMetaObject::invokeMethod(g_engine, [&]() {
        newId = variantToId(libCall("createCollection", { parentId, name }));
    }, Qt::BlockingQueuedConnection);
    qInfo() << "[librarian]: createFolder" << name << "→" << newId;
    return result(newId);
}

extern "C" char *ensureFolder(const char *params)
{
    if (!params || params[0] == '\0')
        return error("empty input");

    QString input = QString::fromUtf8(params);
    if (kUuidRe.match(input).hasMatch())
        return result(input);

    QList<MetaEntry> entries = loadMetadataIndex();
    QStringList parts = input.split('/');

    QString parentId;
    for (const QString &name : parts) {
        if (name.isEmpty()) continue;

        QString found;
        for (const MetaEntry &e : entries) {
            if (e.visibleName == name && e.parent == parentId && e.type == "CollectionType") {
                found = e.uuid;
                break;
            }
        }

        if (found.isEmpty()) {
            found = createFolderOnDisk(name, parentId);
            if (found.isEmpty())
                return error("failed to create folder: " + name);

            MetaEntry newEntry;
            newEntry.uuid = found;
            newEntry.visibleName = name;
            newEntry.parent = parentId;
            newEntry.type = "CollectionType";
            entries.append(newEntry);

            qInfo() << "[librarian]: created folder" << name << "→" << found;
        }

        parentId = found;
    }

    return result(parentId);
}


extern "C" void _xovi_construct()
{
    qInfo() << "[librarian]: extension loaded";
    QTimer::singleShot(0, []() { ensureInitialization(); });
}

extern "C" char _xovi_shouldLoad()
{
    return 1;
}
