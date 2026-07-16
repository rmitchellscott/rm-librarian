#include <cstdlib>
#include <cstring>
#include <functional>

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
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
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QUrl>
#include <QVariant>
#include <QWindow>
#include <QtDebug>

#include "xovi.h"

static QQmlEngine *g_engine = nullptr;
static QObject *g_library = nullptr;
static QObject *g_libraryController = nullptr;
static QObject *g_bridge = nullptr;
static QObject *g_documentImporter = nullptr;
static QHash<QString, QString> g_preTrashParents;

extern "C" {
    const char *buildTree(const char *rmPath);
    const char *convertToJson(const char *treeId);
    int destroyTree(const char *treeId);
}

static QString describeObject(const QObject *obj)
{
    if (!obj)
        return "null";
    const QMetaObject *mo = obj->metaObject();
    return QString("%1(name=%2, addr=%3)")
        .arg(mo ? mo->className() : "QObject",
             obj->objectName().isEmpty() ? "<empty>" : obj->objectName(),
             QString::number(reinterpret_cast<quintptr>(obj), 16));
}

static QString describeVariant(const QVariant &value)
{
    const char *typeName = value.typeName();
    QString rendered = value.toString();
    if (rendered.isEmpty())
        rendered = "<empty>";
    return QString("valid=%1, type=%2, value=%3")
        .arg(value.isValid() ? "true" : "false",
             typeName ? typeName : "<null>",
             rendered);
}

static void logObjectMethods(const char *label, const QObject *obj)
{
    if (!obj) {
        qInfo() << "[librarian]:" << label << "object is null";
        return;
    }

    const QMetaObject *mo = obj->metaObject();
    qInfo() << "[librarian]:" << label << "metaobject =" << (mo ? mo->className() : "<null>");
    if (!mo)
        return;

    for (int i = mo->methodOffset(); i < mo->methodCount(); ++i)
        qInfo() << "[librarian]:" << label << "method" << mo->method(i).methodSignature();
}

static bool shouldLogAllMethods()
{
    const QByteArray value = qgetenv("LIBRARIAN_LOG_ALL_METHODS").trimmed().toLower();
    return value == "1" || value == "true" || value == "yes";
}

static QObject *createHelperAndResolve(
    QQmlEngine *engine,
    const char *label,
    const char *qmlSource,
    QObject **outHolder,
    QVariant *outLibraryValue,
    QVariant *outControllerValue)
{
    QQmlComponent component(engine);
    component.setData(qmlSource, QUrl());
    qInfo() << "[librarian]:" << label << "component status after setData =" << component.status();
    const auto componentErrors = component.errors();
    for (const QQmlError &error : componentErrors)
        qWarning() << "[librarian]:" << label << "component diagnostic:" << error;

    QObject *holder = component.create(engine->rootContext());
    if (!holder) {
        const auto errors = component.errors();
        for (const QQmlError &error : errors)
            qWarning() << "[librarian]:" << label << "QML helper error:" << error;
        return nullptr;
    }

    holder->setParent(engine);
    qInfo() << "[librarian]:" << label << "helper object created:" << describeObject(holder);

    QVariant libValue = holder->property("lib");
    QVariant ctrlValue = holder->property("ctrl");
    qInfo() << "[librarian]:" << label << "helper property lib:" << describeVariant(libValue);
    qInfo() << "[librarian]:" << label << "helper property ctrl:" << describeVariant(ctrlValue);

    if (outHolder)
        *outHolder = holder;
    if (outLibraryValue)
        *outLibraryValue = libValue;
    if (outControllerValue)
        *outControllerValue = ctrlValue;
    return libValue.isValid() ? libValue.value<QObject *>() : nullptr;
}

static bool invokeBridge0(const char *method, QVariant *ret = nullptr)
{
    if (!g_bridge)
        return false;
    if (ret)
        return QMetaObject::invokeMethod(
            g_bridge, method, Qt::DirectConnection, Q_RETURN_ARG(QVariant, *ret));
    return QMetaObject::invokeMethod(g_bridge, method, Qt::DirectConnection);
}

static bool invokeBridge1(const char *method, const QVariant &arg1, QVariant *ret = nullptr)
{
    if (!g_bridge)
        return false;
    if (ret)
        return QMetaObject::invokeMethod(
            g_bridge, method, Qt::DirectConnection, Q_RETURN_ARG(QVariant, *ret), Q_ARG(QVariant, arg1));
    return QMetaObject::invokeMethod(g_bridge, method, Qt::DirectConnection, Q_ARG(QVariant, arg1));
}

static bool invokeBridge2(const char *method, const QVariant &arg1, const QVariant &arg2, QVariant *ret = nullptr)
{
    if (!g_bridge)
        return false;
    if (ret)
        return QMetaObject::invokeMethod(
            g_bridge, method, Qt::DirectConnection, Q_RETURN_ARG(QVariant, *ret),
            Q_ARG(QVariant, arg1), Q_ARG(QVariant, arg2));
    return QMetaObject::invokeMethod(
        g_bridge, method, Qt::DirectConnection, Q_ARG(QVariant, arg1), Q_ARG(QVariant, arg2));
}

static bool invokeBridge3(
    const char *method,
    const QVariant &arg1,
    const QVariant &arg2,
    const QVariant &arg3,
    QVariant *ret = nullptr)
{
    if (!g_bridge)
        return false;
    if (ret)
        return QMetaObject::invokeMethod(
            g_bridge, method, Qt::DirectConnection, Q_RETURN_ARG(QVariant, *ret),
            Q_ARG(QVariant, arg1), Q_ARG(QVariant, arg2), Q_ARG(QVariant, arg3));
    return QMetaObject::invokeMethod(
        g_bridge, method, Qt::DirectConnection,
        Q_ARG(QVariant, arg1), Q_ARG(QVariant, arg2), Q_ARG(QVariant, arg3));
}

static bool libraryHasEntry(const QString &uuid)
{
    QVariant hasEntryValue;
    return invokeBridge1("hasEntry", uuid, &hasEntryValue) && hasEntryValue.toBool();
}

static bool runOnEngineSync(const std::function<void()> &fn)
{
    if (!g_engine)
        return false;
    return QMetaObject::invokeMethod(g_engine, fn, Qt::BlockingQueuedConnection);
}

static QByteArray buildHelperQml(const char *importLine, const char *libExpr, const char *ctrlExpr)
{
    static const char *kHelperBody = R"QML(
        QtObject {
            property var lib: %1
            property var ctrl: %2
            property var importer: typeof DocumentImporter !== "undefined" ? DocumentImporter : undefined
            function normalizeEntryId(id) {
                if (!lib || id === null || id === undefined || id === "")
                    return id;
                const entry = lib.entryForId(id);
                return entry ? entry.id : id;
            }
            function normalizeFolderHandle(id) {
                if (!lib || id === null || id === undefined || id === "")
                    return id;
                const entry = lib.entryForId(id);
                if (!entry)
                    return id;
                const entityId = lib.entityIdForEntry(entry);
                return entityId && entityId.toString ? entityId.toString() : id;
            }
            function libraryIsReady() { return !!lib && !!lib.isReady }
            function hasEntry(id) { return !!lib && !!id && !!lib.entryForId(id) }
            function parentIdForId(id) { return lib ? lib.parentIdForId(normalizeEntryId(id)) : null }
            function pokeLibraryEntry(id) {
                if (!lib)
                    return false;
                const normalized = normalizeEntryId(id);
                if (lib.requestLoadEntry)
                    lib.requestLoadEntry(normalized);
                if (lib.entryAdded)
                    lib.entryAdded(normalized);
                return true;
            }
            function setVisibleName(id, name) {
                const normalized = lib ? normalizeEntryId(id) : "";
                if (!ctrl || !normalized)
                    return false;
                ctrl.setVisibleName(normalized, name);
                return true;
            }
            function moveEntry(id, parentId) {
                if (!ctrl)
                    return false;
                const normalized = normalizeEntryId(id);
                if (!normalized)
                    return false;
                return ctrl.moveEntries([normalized], parentId);
            }
            function moveSingleEntryToTrash(id) {
                const normalized = normalizeEntryId(id);
                return ctrl ? ctrl.moveEntriesToTrash([normalized]) : false;
            }
            function restoreSingleEntryFromTrash(id) {
                const normalized = normalizeEntryId(id);
                return ctrl ? ctrl.restoreEntriesFromTrash([normalized]) : false;
            }
            function deleteSingleEntry(id) {
                const normalized = normalizeEntryId(id);
                return ctrl ? ctrl.deleteEntries([normalized]) : false;
            }
            function cloneEntry(id, parentId) { return ctrl ? ctrl.cloneEntry(normalizeEntryId(id), parentId) : "" }
            function createDocument(parentId, name) {
                const normalizedParent = normalizeFolderHandle(parentId);
                return ctrl ? ctrl.createDocument(normalizedParent, name) : "";
            }
            function setPinned(id, pinned) { return ctrl ? ctrl.setPinned(normalizeEntryId(id), pinned) : false }
            function setCoverPageNumber(id, page) { return ctrl ? ctrl.setCoverPageNumber(normalizeEntryId(id), page) : false }
            function setOrientation(id, orientation) { return ctrl ? ctrl.setOrientation(normalizeEntryId(id), orientation) : false }
            function setEntryTags(id, tags, partial) { return ctrl ? ctrl.setEntryTags(normalizeEntryId(id), tags, partial) : false }
            function createFolder(parentId, name) { return lib ? lib.createCollection(parentId, name) : "" }
            function importKeyForPath(path) {
                return path ? (path.indexOf("file:") === 0 ? path : "file://" + path) : "";
            }
            function findImportKey(path) {
                const directKey = importKeyForPath(path);
                if (importResults[directKey] !== undefined)
                    return directKey;

                for (const key in importResults) {
                    if (!Object.prototype.hasOwnProperty.call(importResults, key))
                        continue;
                    if (key === directKey || key.endsWith("/" + path) || key.endsWith(path))
                        return key;
                    const result = importResults[key];
                    if (result && result.name === path)
                        return key;
                }
                return directKey;
            }
            function importFromFile(path, parentId) {
                const url = importKeyForPath(path);
                delete importResults[url];
                if (!importer) {
                    importResults[url] = {status: "missing-importer", id: "", finished: true};
                    return url;
                }
                const normalizedParent = parentId ? normalizeEntryId(parentId) : "";
                importer.importFromUrls([url], normalizedParent);
                return url;
            }
            function importStatus(path) {
                const url = findImportKey(path);
                const result = importResults[url];
                return result ? JSON.stringify(result) : "";
            }
            property var importResults: ({})
            property var importerConnections: Connections {
                target: importer
                ignoreUnknownSignals: true
                function onStarted(name, url) {
                    const key = url.toString();
                    importResults[key] = {status: "started", name: name, id: "", finished: false};
                }
                function onImported(document, collection, url) {
                    const key = url.toString();
                    const id = document && document.id !== undefined ? document.id.toString() : "";
                    const parentId = collection && collection.id !== undefined ? collection.id.toString() : "";
                    importResults[key] = {status: "imported", id: id, parentId: parentId, finished: false};
                }
                function onFailed(url) {
                    const key = url.toString();
                    importResults[key] = {status: "failed", id: "", finished: false};
                }
                function onFinished(url) {
                    const key = url.toString();
                    const result = importResults[key] || {};
                    result.finished = true;
                    if (!result.status)
                        result.status = "finished";
                    importResults[key] = result;
                }
            }
            property var libraryImportConnections: Connections {
                target: lib
                ignoreUnknownSignals: true
                function onEntryImported(path, id) {
                    const key = findImportKey(path);
                    const result = importResults[key] || {};
                    result.status = "imported";
                    result.id = id !== undefined && id !== null ? id.toString() : "";
                    importResults[key] = result;
                }
            }
        }
    )QML";

    return QString("import QtQml\n%1\n%2")
        .arg(QString::fromUtf8(importLine),
             QString::fromUtf8(kHelperBody).arg(
                 QString::fromUtf8(libExpr),
                 QString::fromUtf8(ctrlExpr)))
        .toUtf8();
}

static bool ensureEngine()
{
    if (g_engine)
        return true;
    const auto windows = QGuiApplication::allWindows();
    qInfo() << "[librarian]: ensureEngine scanning" << windows.size() << "windows";
    for (QWindow *window : windows) {
        QQmlEngine *engine = qmlEngine(window);
        qInfo() << "[librarian]: window"
                << window
                << "title=" << window->title()
                << "objectName=" << window->objectName()
                << "visible=" << window->isVisible()
                << "engine=" << engine;
        if (engine) {
            g_engine = engine;
            qInfo() << "[librarian]: selected engine" << engine
                    << "rootContext=" << engine->rootContext();
            return true;
        }
    }
    qWarning() << "[librarian]: ensureEngine found no QML engine";
    return false;
}

static bool resolveLibrary()
{
    if (g_library)
        return true;

    if (!ensureEngine())
        return false;

    const QByteArray directHelperQml = buildHelperQml(
        "import com.remarkable\nimport xofm.libs.library",
        "typeof Library !== \"undefined\" ? Library : undefined",
        "typeof LibraryController !== \"undefined\" ? LibraryController : undefined");
    const QByteArray legacyAliasHelperQml = buildHelperQml(
        "import com.remarkable 1.0 as RM",
        "typeof RM.Library !== \"undefined\" ? RM.Library : undefined",
        "typeof RM.LibraryController !== \"undefined\" ? RM.LibraryController : undefined");

    QVariant libValue;
    QVariant ctrlValue;
    QObject *holder = nullptr;
    QObject *lib = createHelperAndResolve(
        g_engine,
        "direct-helper",
        directHelperQml.constData(),
        &holder,
        &libValue,
        &ctrlValue);

    if (!lib) {
        qWarning() << "[librarian]: direct helper did not resolve Library; trying legacy aliased helper";
        QObject *legacyHolder = nullptr;
        QVariant legacyLibValue;
        QVariant legacyCtrlValue;
        lib = createHelperAndResolve(
            g_engine,
            "legacy-helper",
            legacyAliasHelperQml.constData(),
            &legacyHolder,
            &legacyLibValue,
            &legacyCtrlValue);
        if (lib) {
            holder = legacyHolder;
            libValue = legacyLibValue;
            ctrlValue = legacyCtrlValue;
        }
    }

    if (!lib) {
        qWarning() << "[librarian]: Library not found after all helper strategies; engine =" << g_engine
                   << "rootContext =" << g_engine->rootContext();
        return false;
    }

    g_library = lib;
    g_bridge = holder;
    qInfo() << "[librarian]: Library resolved:" << describeObject(g_library)
            << "via helper object" << describeObject(holder);

    QObject *ctrl = ctrlValue.isValid() ? ctrlValue.value<QObject *>() : nullptr;
    if (ctrl) {
        g_libraryController = ctrl;
        qInfo() << "[librarian]: LibraryController resolved:" << describeObject(g_libraryController);
    } else {
        qWarning() << "[librarian]: LibraryController not found";
    }

    QVariant importerValue = holder ? holder->property("importer") : QVariant();
    g_documentImporter = importerValue.isValid() ? importerValue.value<QObject *>() : nullptr;
    qInfo() << "[librarian]: DocumentImporter property:" << describeVariant(importerValue)
            << describeObject(g_documentImporter);

    if (shouldLogAllMethods()) {
        logObjectMethods("Library", g_library);
        logObjectMethods("LibraryController", g_libraryController);
        logObjectMethods("DocumentImporter", g_documentImporter);
    }

    return true;
}

static void ensureInitialization()
{
    if (!ensureEngine()) {
        qInfo() << "[librarian]: initialization retry scheduled after missing engine";
        QTimer::singleShot(200, []() { ensureInitialization(); });
        return;
    }
    if (!resolveLibrary()) {
        qInfo() << "[librarian]: initialization retry scheduled after unresolved library";
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
    if (!g_bridge || !g_engine)
        return;
    runOnEngineSync([&]() {
        invokeBridge1("pokeLibraryEntry", uuid);
    });
}

static bool waitForLibraryEntry(const QString &uuid, int timeoutMs = 5000)
{
    if (!g_library || !g_bridge || !g_engine)
        return false;

    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < timeoutMs) {
        notifyLibrary(uuid);

        bool present = false;
        if (runOnEngineSync([&]() {
                present = libraryHasEntry(uuid);
            }) && present) {
            qInfo() << "[librarian]: runtime library now recognizes" << uuid;
            return true;
        }

        QThread::msleep(100);
    }

    qWarning() << "[librarian]: timed out waiting for runtime library entry" << uuid;
    return false;
}

static bool startImporterImport(const QString &filePath, const QString &parentId, QString *outImportKey)
{
    if (!g_bridge || !g_engine)
        return false;

    QString importKey;
    if (!runOnEngineSync([&]() {
            QVariant keyValue;
            invokeBridge2("importFromFile", filePath, parentId, &keyValue);
            importKey = keyValue.toString();
        }))
        return false;

    if (outImportKey)
        *outImportKey = importKey;
    return !importKey.isEmpty();
}

static bool waitForImporterResult(
    const QString &filePath,
    QString *outId,
    QString *outError,
    int timeoutMs = 30000)
{
    if (!g_bridge || !g_engine) {
        if (outError)
            *outError = "import bridge not initialized";
        return false;
    }

    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < timeoutMs) {
        QString payload;
        bool invokeOk = runOnEngineSync([&]() {
            QVariant statusValue;
            if (invokeBridge1("importStatus", filePath, &statusValue))
                payload = statusValue.toString();
        });

        if (invokeOk && !payload.isEmpty()) {
            const QJsonObject obj = QJsonDocument::fromJson(payload.toUtf8()).object();
            const QString status = obj.value("status").toString();
            const QString id = obj.value("id").toString();
            const bool finished = obj.value("finished").toBool(false);

            if (status == "imported" && !id.isEmpty()) {
                if (outId)
                    *outId = id;
                return true;
            }

            if (status == "failed" || status == "missing-importer") {
                if (outError)
                    *outError = status.isEmpty() ? "import failed" : status;
                return false;
            }

            if (finished && !id.isEmpty()) {
                if (outId)
                    *outId = id;
                return true;
            }
        }

        QThread::msleep(100);
    }

    if (outError)
        *outError = "timed out waiting for importer";
    return false;
}

static bool postProcessImportedEntry(
    const QString &entryId,
    const QString &parentId,
    const QString &visibleName,
    QString *outError = nullptr)
{
    if (!g_bridge || !g_engine) {
        if (outError)
            *outError = "extension not initialized";
        return false;
    }

    bool ok = true;
    if (!visibleName.isEmpty()) {
        bool renameOk = false;
        if (!runOnEngineSync([&]() {
                QVariant okValue;
                renameOk = invokeBridge2("setVisibleName", entryId, visibleName, &okValue) && okValue.toBool();
            }) || !renameOk) {
            if (outError)
                *outError = "failed to rename imported entry";
            ok = false;
        }
    }

    if (ok && !parentId.isEmpty()) {
        bool moveOk = false;
        if (!runOnEngineSync([&]() {
                QVariant okValue;
                moveOk = invokeBridge2("moveEntry", entryId, parentId, &okValue) && okValue.toBool();
            }) || !moveOk) {
            if (outError)
                *outError = "failed to move imported entry";
            ok = false;
        }
    }

    return ok;
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

static QSet<QString> uuidsForParent(const QString &parentId)
{
    QSet<QString> uuids;
    const QList<MetaEntry> entries = loadMetadataIndex();
    for (const MetaEntry &e : entries) {
        if (e.parent == parentId)
            uuids.insert(e.uuid);
    }
    return uuids;
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
    QVariant readyValue;
    if (!invokeBridge0("libraryIsReady", &readyValue) || !readyValue.toBool())
        return error("library not ready");

    QList<MetaEntry> entries = loadMetadataIndex();
    int loaded = 0;

    QMetaObject::invokeMethod(g_engine, [&entries, &loaded]() {
        for (const MetaEntry &e : entries) {
            QVariant parentId;
            bool ok = invokeBridge1("parentIdForId", e.uuid, &parentId);
            if (ok && parentId.isNull()) {
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
        if (!waitForLibraryEntry(uuid))
            return error("runtime library did not load imported rmdoc: " + uuid);

        qInfo() << "[librarian]: imported rmdoc" << filePath << "uuid" << uuid;
        return result(uuid);
    }

    QString fileType = detectFileType(srcInfo.suffix());
    if (fileType.isEmpty())
        return error("unsupported file type: " + srcInfo.suffix());
    const QString visibleName = stripExtension(srcInfo.fileName());
    QString importKey;
    if (!startImporterImport(srcInfo.absoluteFilePath(), parent, &importKey))
        return error("failed to dispatch importer");

    QString importedId;
    QString importError;
    if (!waitForImporterResult(srcInfo.absoluteFilePath(), &importedId, &importError))
        return error("document importer failed: " + importError);
    if (!waitForLibraryEntry(importedId))
        return error("runtime library did not load imported document: " + importedId);
    if (!postProcessImportedEntry(importedId, parent, visibleName, &importError))
        return error("document importer post-process failed: " + importError);

    qInfo() << "[librarian]: imported" << filePath << "via DocumentImporter key" << importKey
            << "uuid" << importedId;
    return result(importedId);
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

    QString visibleName = stripExtension(QFileInfo(filePath).fileName());
    QString tempUuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    QString pdfPath = QString("%1/%2-import.pdf").arg(QDir::tempPath(), tempUuid);

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

    QString importKey;
    if (!startImporterImport(pdfPath, parent, &importKey)) {
        QFile::remove(pdfPath);
        return error("failed to dispatch importer");
    }

    QString importedId;
    QString importError;
    const bool imported = waitForImporterResult(pdfPath, &importedId, &importError);
    QFile::remove(pdfPath);
    if (!imported)
        return error("image importer failed: " + importError);
    if (!waitForLibraryEntry(importedId))
        return error("runtime library did not load imported image: " + importedId);
    if (!postProcessImportedEntry(importedId, parent, visibleName, &importError))
        return error("image importer post-process failed: " + importError);

    qInfo() << "[librarian]: imported image" << filePath << "as" << visibleName
            << "via DocumentImporter key" << importKey << "uuid" << importedId;
    return result(importedId);
}

#define REQUIRE_CTRL() do { \
    if (!g_bridge || !g_engine) return error("extension not initialized"); \
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
    if (!runOnEngineSync([&]() {
            QVariant okValue;
            ok = invokeBridge2("setVisibleName", id, newName, &okValue) && okValue.toBool();
            qInfo() << "[librarian]: rename" << id << "→" << newName << (ok ? "ok" : "FAILED");
        }))
        return error("failed to dispatch rename");
    if (!ok)
        return error("rename failed");
    return strdup("ok");
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
    if (!runOnEngineSync([&]() {
            QVariant okValue;
            ok = invokeBridge2("moveEntry", id, parentId, &okValue) && okValue.toBool();
            qInfo() << "[librarian]: move" << id << "→" << parentId << (ok ? "ok" : "FAILED");
        }))
        return error("failed to dispatch move");
    if (!ok)
        return error("move failed");
    return strdup("ok");
}

extern "C" char *trashEntry(const char *params)
{
    REQUIRE_CTRL();
    RESOLVE(id, resolveId(QString::fromUtf8(params ? params : "")));

    QString parentBeforeTrash;
    bool ok = false;
    if (!runOnEngineSync([&]() {
            QVariant parentValue;
            if (invokeBridge1("parentIdForId", id, &parentValue))
                parentBeforeTrash = parentValue.toString();
            QVariant okValue;
            ok = invokeBridge1("moveSingleEntryToTrash", id, &okValue) && okValue.toBool();
            qInfo() << "[librarian]: trash" << id << (ok ? "ok" : "FAILED");
        }))
        return error("failed to dispatch trash");
    if (!ok)
        return error("trash failed");
    g_preTrashParents.insert(id, parentBeforeTrash);
    return strdup("ok");
}

extern "C" char *restoreEntry(const char *params)
{
    REQUIRE_CTRL();
    RESOLVE(id, resolveIdInTrash(QString::fromUtf8(params ? params : "")));

    const QString restoreParentId = g_preTrashParents.value(id);
    bool ok = false;
    if (!runOnEngineSync([&]() {
            QVariant okValue;
            ok = invokeBridge1("restoreSingleEntryFromTrash", id, &okValue) && okValue.toBool();
            if (ok && !restoreParentId.isEmpty()) {
                QVariant moveOkValue;
                const bool moveOk = invokeBridge2("moveEntry", id, restoreParentId, &moveOkValue) && moveOkValue.toBool();
                if (!moveOk)
                    qWarning() << "[librarian]: restore post-move failed for" << id << "→" << restoreParentId;
                ok = ok && moveOk;
            }
            qInfo() << "[librarian]: restore" << id << (ok ? "ok" : "FAILED");
        }))
        return error("failed to dispatch restore");
    if (!ok)
        return error("restore failed");
    g_preTrashParents.remove(id);
    return strdup("ok");
}

extern "C" char *deleteEntry(const char *params)
{
    REQUIRE_CTRL();
    RESOLVE(id, resolveIdInTrash(QString::fromUtf8(params ? params : "")));

    bool ok = false;
    if (!runOnEngineSync([&]() {
            QVariant okValue;
            ok = invokeBridge1("deleteSingleEntry", id, &okValue) && okValue.toBool();
            qInfo() << "[librarian]: delete" << id << (ok ? "ok" : "FAILED");
        }))
        return error("failed to dispatch delete");
    if (!ok)
        return error("delete failed");
    return strdup("ok");
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

    const QString effectiveParentId = parentId;
    const QSet<QString> before = uuidsForParent(effectiveParentId);
    QString newId;
    if (!runOnEngineSync([&]() {
            QVariant newIdValue;
            invokeBridge2("cloneEntry", id, parentId, &newIdValue);
            newId = newIdValue.toString();
            qInfo() << "[librarian]: clone" << id << "→" << newId;
        }))
        return error("failed to dispatch clone");

    if (newId.isEmpty()) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000) {
            const QSet<QString> after = uuidsForParent(effectiveParentId);
            for (const QString &candidate : after) {
                if (!before.contains(candidate)) {
                    newId = candidate;
                    break;
                }
            }
            if (!newId.isEmpty())
                break;
            QThread::msleep(100);
        }
    }

    if (newId.isEmpty())
        return error("clone failed");
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
    if (!runOnEngineSync([&]() {
            QVariant newIdValue;
            invokeBridge2("createDocument", parentId, name, &newIdValue);
            newId = newIdValue.toString();
            qInfo() << "[librarian]: createNotebook" << name << "→" << newId;
        }))
        return error("failed to dispatch createNotebook");
    if (newId.isEmpty())
        return error("createNotebook failed");
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
    if (!runOnEngineSync([&]() {
            QVariant okValue;
            ok = invokeBridge2("setPinned", id, pinned, &okValue) && okValue.toBool();
            qInfo() << "[librarian]: setPinned" << id << pinned << (ok ? "ok" : "FAILED");
        }))
        return error("failed to dispatch setPinned");
    if (!ok)
        return error("setPinned failed");
    return strdup("ok");
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
    if (!runOnEngineSync([&]() {
            QVariant okValue;
            ok = invokeBridge2("setCoverPageNumber", id, page, &okValue) && okValue.toBool();
            qInfo() << "[librarian]: setCover" << id << page << (ok ? "ok" : "FAILED");
        }))
        return error("failed to dispatch setCover");
    if (!ok)
        return error("setCover failed");
    return strdup("ok");
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
    if (!runOnEngineSync([&]() {
            QVariant okValue;
            ok = invokeBridge2("setOrientation", id, orientation, &okValue) && okValue.toBool();
            qInfo() << "[librarian]: setOrientation" << id << orientation << (ok ? "ok" : "FAILED");
        }))
        return error("failed to dispatch setOrientation");
    if (!ok)
        return error("setOrientation failed");
    return strdup("ok");
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
    if (!runOnEngineSync([&]() {
            QStringList partial;
            QVariant okValue;
            ok = invokeBridge3("setEntryTags", id, tags, partial, &okValue) && okValue.toBool();
            qInfo() << "[librarian]: setTags" << id << tags << (ok ? "ok" : "FAILED");
        }))
        return error("failed to dispatch setTags");
    if (!ok)
        return error("setTags failed");
    return strdup("ok");
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
    if (!runOnEngineSync([&]() {
            QVariant newIdValue;
            invokeBridge2("createFolder", parentId, name, &newIdValue);
            newId = newIdValue.toString();
            qInfo() << "[librarian]: createFolder" << name << "→" << newId;
        }))
        return error("failed to dispatch createFolder");
    if (newId.isEmpty())
        return error("createFolder failed");
    return result(newId);
}

extern "C" char *ensureFolder(const char *params)
{
    REQUIRE_CTRL();
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
            QString newId;
            if (!runOnEngineSync([&]() {
                    QVariant newIdValue;
                    invokeBridge2("createFolder", parentId, name, &newIdValue);
                    newId = newIdValue.toString();
                }))
                return error("failed to dispatch folder creation: " + name);
            found = newId;
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
