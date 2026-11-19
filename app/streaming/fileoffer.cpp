#include "fileoffer.h"
#include "clipboardlogging.h"
#include "backend/pairedcertificate.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QSslConfiguration>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>

namespace {

// Largest integer a JSON number carries exactly.
constexpr double MAX_JSON_INTEGER = 9007199254740992.0;

bool fail(QString* error, const QString& message)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool readUnsigned(const QJsonValue& value, quint64& out)
{
    if (!value.isDouble()) {
        return false;
    }
    const double d = value.toDouble();
    if (!std::isfinite(d) || d < 0 || d > MAX_JSON_INTEGER || std::floor(d) != d) {
        return false;
    }
    out = static_cast<quint64>(d);
    return true;
}

// A name Windows refuses or treats as a device, whatever the extension.
bool isReservedName(const QString& component)
{
    static const QSet<QString> reserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),  QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"), QStringLiteral("COM4"),
        QStringLiteral("COM5"), QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"), QStringLiteral("LPT7"),
        QStringLiteral("LPT8"), QStringLiteral("LPT9"), QStringLiteral("CONIN$"), QStringLiteral("CONOUT$"),
    };
    const QString base = component.section(QLatin1Char('.'), 0, 0).trimmed().toUpper();
    return reserved.contains(base);
}

bool isValidComponent(const QString& component)
{
    if (component.isEmpty() || component == QStringLiteral(".") || component == QStringLiteral("..")) {
        return false;
    }
    for (const QChar c : component) {
        if (c.unicode() < 0x20 || QStringLiteral("<>:\"/\\|?*").contains(c)) {
            return false;
        }
    }
    // Windows strips a trailing dot or space, so "a." would land as "a".
    if (component.endsWith(QLatin1Char('.')) || component.endsWith(QLatin1Char(' '))) {
        return false;
    }
    return !isReservedName(component);
}

} // namespace

bool FileOffer::parse(const QByteArray& json, FileOffer& out, QString* error)
{
    out = FileOffer();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return fail(error, QStringLiteral("not a JSON object"));
    }
    const QJsonObject obj = doc.object();

    out.id = obj.value(QStringLiteral("id")).toString();
    static const QRegularExpression idPattern(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    if (!idPattern.match(out.id).hasMatch()) {
        return fail(error, QStringLiteral("bad id"));
    }
    out.type = obj.value(QStringLiteral("type")).toString() == QStringLiteral("send")
                   ? QStringLiteral("send")
                   : QStringLiteral("clipboard");

    const QJsonValue filesValue = obj.value(QStringLiteral("files"));
    if (!filesValue.isArray()) {
        return fail(error, QStringLiteral("no files"));
    }
    const QJsonArray files = filesValue.toArray();
    if (files.isEmpty() || files.size() > MAX_ENTRIES) {
        return fail(error, QStringLiteral("bad entry count"));
    }

    QSet<QString> seen;     // every path, case-folded (Windows names are case-insensitive)
    QSet<QString> folders;  // folder paths, case-folded
    out.entries.reserve(static_cast<size_t>(files.size()));
    for (const QJsonValue& value : files) {
        if (!value.isObject()) {
            return fail(error, QStringLiteral("bad entry"));
        }
        const QJsonObject file = value.toObject();
        const QString path = file.value(QStringLiteral("path")).toString();
        const bool dir = file.value(QStringLiteral("dir")).toBool(false);
        quint64 size = 0;
        if (!readUnsigned(file.value(QStringLiteral("size")), size) || (dir && size != 0)) {
            return fail(error, QStringLiteral("bad size"));
        }
        quint64 mtime = 0;
        if (file.contains(QStringLiteral("mtime")) && !readUnsigned(file.value(QStringLiteral("mtime")), mtime)) {
            mtime = 0;
        }

        const QStringList components = path.split(QLatin1Char('/'));
        for (const QString& component : components) {
            if (!isValidComponent(component)) {
                return fail(error, QStringLiteral("bad path"));
            }
        }
        const QString windowsPath = components.join(QLatin1Char('\\'));
        if (windowsPath.size() > MAX_PATH_UNITS) {
            return fail(error, QStringLiteral("path too long"));
        }
        const QString folded = windowsPath.toCaseFolded();
        if (seen.contains(folded)) {
            return fail(error, QStringLiteral("duplicate path"));
        }
        if (components.size() > 1) {
            const QString parent = components.mid(0, components.size() - 1).join(QLatin1Char('\\')).toCaseFolded();
            if (!folders.contains(parent)) {
                return fail(error, QStringLiteral("entry before its folder"));
            }
        }
        seen.insert(folded);
        if (dir) {
            folders.insert(folded);
        }

        VirtualFiles::Entry entry;
        entry.path = windowsPath.toStdU16String();
        entry.size = size;
        entry.dir = dir;
        entry.mtime = static_cast<std::int64_t>(mtime);
        out.entries.push_back(std::move(entry));
        if (out.totalSize > std::numeric_limits<quint64>::max() - size) {
            return fail(error, QStringLiteral("bad size"));
        }
        out.totalSize += size;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Downloader

namespace {

// Bytes held per open stream before the socket is left to wait.
constexpr qint64 HIGH_WATER = 4 * 1024 * 1024;
constexpr qint64 LOW_WATER = 1 * 1024 * 1024;
constexpr qint64 REPLY_BUFFER = 1 * 1024 * 1024;
// A paste that sees no byte for this long fails instead of hanging the target.
constexpr std::chrono::seconds STALL_TIMEOUT{60};

struct StreamState
{
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<QByteArray> chunks;
    qint64 headOffset = 0;   // bytes of chunks.front() already handed out
    qint64 buffered = 0;     // bytes waiting in chunks
    quint64 expected = 0;    // bytes this response must carry
    quint64 received = 0;
    bool done = false;
    bool failed = false;

    // Worker thread only.
    QPointer<QNetworkReply> reply;
};

} // namespace

struct FileOfferDownloader::Core
{
    ClipboardSyncHostContext host;
    QString offerId;
    std::vector<VirtualFiles::Entry> entries;

    QThread thread;
    QObject* context = nullptr;          // lives on `thread`
    QNetworkAccessManager* nam = nullptr; // child of context

    Core(const ClipboardSyncHostContext& h, const FileOffer& offer)
        : host(h), offerId(offer.id), entries(offer.entries)
    {
        thread.setObjectName(QStringLiteral("file-offer-download"));
        context = new QObject();
        context->moveToThread(&thread);
        thread.start();
        QMetaObject::invokeMethod(
            context,
            [this]() {
                nam = new QNetworkAccessManager(context);
                const QSslCertificate pinned = host.serverCertificate;
                PairedCertificate::enforce(nam, context, pinned);
                QObject::connect(nam, &QNetworkAccessManager::sslErrors, context,
                                 [pinned](QNetworkReply* reply, const QList<QSslError>& errors) {
                                     if (PairedCertificate::canIgnoreErrors(pinned, errors)) {
                                         reply->ignoreSslErrors(errors);
                                     }
                                 });
            },
            Qt::BlockingQueuedConnection);
    }

    ~Core()
    {
        // Deleting the context on its thread aborts and frees every reply.
        QObject* doomed = context;
        QMetaObject::invokeMethod(
            doomed, [doomed]() { delete doomed; }, Qt::BlockingQueuedConnection);
        thread.quit();
        thread.wait();
    }

    // Worker thread: move what the reply holds into the stream, up to HIGH_WATER.
    static void pump(const std::shared_ptr<StreamState>& st)
    {
        QNetworkReply* reply = st->reply;
        if (reply == nullptr) {
            return;
        }
        bool overrun = false;
        {
            std::lock_guard<std::mutex> lock(st->mutex);
            if (st->failed) {
                return;
            }
            while (st->buffered < HIGH_WATER && reply->bytesAvailable() > 0) {
                QByteArray chunk = reply->read(HIGH_WATER - st->buffered);
                if (chunk.isEmpty()) {
                    break;
                }
                st->received += static_cast<quint64>(chunk.size());
                st->buffered += chunk.size();
                st->chunks.push_back(std::move(chunk));
            }
            if (st->received > st->expected) {
                st->failed = true;
                overrun = true;
            }
        }
        st->changed.notify_all();
        if (overrun) {
            reply->abort();
        }
    }

    static void failStream(const std::shared_ptr<StreamState>& st)
    {
        {
            std::lock_guard<std::mutex> lock(st->mutex);
            st->failed = true;
        }
        st->changed.notify_all();
        if (st->reply != nullptr) {
            st->reply->abort();
        }
    }

    // Worker thread.
    void start(const std::shared_ptr<StreamState>& st, std::size_t index, quint64 offset)
    {
        QUrl url;
        url.setScheme(QStringLiteral("https"));
        url.setHost(host.address);
        url.setPort(host.httpsPort);
        url.setPath(QStringLiteral("/api/v1/file-transfer/%1/%2").arg(offerId).arg(index));

        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        QSslConfiguration ssl(QSslConfiguration::defaultConfiguration());
        ssl.setLocalCertificate(host.clientCertificate);
        ssl.setPrivateKey(host.clientPrivateKey);
        request.setSslConfiguration(ssl);
        if (offset != 0) {
            request.setRawHeader("Range", QByteArrayLiteral("bytes=") + QByteArray::number(offset) + '-');
        }

        QNetworkReply* reply = nam->get(request);
        reply->setReadBufferSize(REPLY_BUFFER);
        st->reply = reply;
        const int wanted = offset != 0 ? 206 : 200;
        const QSslCertificate pinned = host.serverCertificate;

        QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [st, reply, wanted]() {
            const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
            if (status.isValid() && status.toInt() != wanted) {
                ClipboardLog::warn("FileOffer: download answered HTTP %d", status.toInt());
                failStream(st);
            }
        });
        QObject::connect(reply, &QNetworkReply::readyRead, reply, [st]() { pump(st); });
        QObject::connect(reply, &QNetworkReply::finished, reply, [st, reply, wanted, pinned]() {
            const bool transportOk =
                reply->error() == QNetworkReply::NoError &&
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == wanted &&
                PairedCertificate::matches(pinned, reply->sslConfiguration().peerCertificate());
            {
                std::lock_guard<std::mutex> lock(st->mutex);
                if (transportOk && !st->failed) {
                    // The reply holds at most REPLY_BUFFER more; take it all.
                    while (reply->bytesAvailable() > 0) {
                        QByteArray chunk = reply->readAll();
                        if (chunk.isEmpty()) {
                            break;
                        }
                        st->received += static_cast<quint64>(chunk.size());
                        st->buffered += chunk.size();
                        st->chunks.push_back(std::move(chunk));
                    }
                }
                if (!transportOk || st->failed || st->received != st->expected) {
                    if (!st->failed) {
                        ClipboardLog::warn("FileOffer: download failed (%s)",
                                           reply->errorString().toUtf8().constData());
                    }
                    st->failed = true;
                } else {
                    st->done = true;
                }
            }
            st->changed.notify_all();
            reply->deleteLater();
        });
    }
};

namespace {

class FailedReader : public VirtualFiles::Reader
{
public:
    std::int64_t read(void*, std::uint32_t) override { return -1; }
};

class HttpReader : public VirtualFiles::Reader
{
public:
    HttpReader(std::shared_ptr<FileOfferDownloader::Core> core, std::shared_ptr<StreamState> st)
        : m_Core(std::move(core)), m_State(std::move(st))
    {
    }

    ~HttpReader() override
    {
        // A paste target that stops early (cancel, error) must not leave the transfer running.
        const auto st = m_State;
        QMetaObject::invokeMethod(
            m_Core->context,
            [st]() {
                if (st->reply != nullptr) {
                    st->reply->abort();
                }
            },
            Qt::QueuedConnection);
    }

    std::int64_t read(void* buffer, std::uint32_t size) override
    {
        if (size == 0) {
            return 0;
        }
        std::unique_lock<std::mutex> lock(m_State->mutex);
        const bool ready = m_State->changed.wait_for(lock, STALL_TIMEOUT, [this]() {
            return m_State->buffered > 0 || m_State->done || m_State->failed;
        });
        if (!ready) {
            m_State->failed = true;
            lock.unlock();
            ClipboardLog::warn("FileOffer: download stalled");
            const auto st = m_State;
            QMetaObject::invokeMethod(
                m_Core->context, [st]() { FileOfferDownloader::Core::failStream(st); }, Qt::QueuedConnection);
            return -1;
        }
        if (m_State->buffered == 0) {
            return m_State->failed ? -1 : 0;
        }

        auto* out = static_cast<char*>(buffer);
        std::int64_t copied = 0;
        while (copied < size && !m_State->chunks.empty()) {
            const QByteArray& front = m_State->chunks.front();
            const qint64 available = front.size() - m_State->headOffset;
            const qint64 take = std::min<qint64>(available, static_cast<qint64>(size) - copied);
            std::memcpy(out + copied, front.constData() + m_State->headOffset, static_cast<size_t>(take));
            copied += take;
            m_State->headOffset += take;
            if (m_State->headOffset == front.size()) {
                m_State->chunks.pop_front();
                m_State->headOffset = 0;
            }
        }
        m_State->buffered -= copied;
        const bool refill = m_State->buffered < LOW_WATER && !m_State->done && !m_State->failed;
        lock.unlock();
        if (refill) {
            const auto st = m_State;
            QMetaObject::invokeMethod(
                m_Core->context, [st]() { FileOfferDownloader::Core::pump(st); }, Qt::QueuedConnection);
        }
        return copied;
    }

private:
    std::shared_ptr<FileOfferDownloader::Core> m_Core;
    std::shared_ptr<StreamState> m_State;
};

} // namespace

FileOfferDownloader::FileOfferDownloader(const ClipboardSyncHostContext& host, const FileOffer& offer)
    : m_Core(std::make_shared<Core>(host, offer))
{
}

FileOfferDownloader::~FileOfferDownloader() = default;

std::unique_ptr<VirtualFiles::Reader> FileOfferDownloader::open(std::size_t index, std::uint64_t offset)
{
    if (index >= m_Core->entries.size() || m_Core->entries[index].dir ||
        offset > m_Core->entries[index].size) {
        return std::make_unique<FailedReader>();
    }
    auto st = std::make_shared<StreamState>();
    st->expected = m_Core->entries[index].size - offset;
    if (st->expected == 0) {
        // Nothing to fetch (an empty file, or a seek to its end).
        st->done = true;
        return std::make_unique<HttpReader>(m_Core, st);
    }
    Core* core = m_Core.get();
    QMetaObject::invokeMethod(
        core->context, [core, st, index, offset]() { core->start(st, index, offset); }, Qt::QueuedConnection);
    return std::make_unique<HttpReader>(m_Core, st);
}
