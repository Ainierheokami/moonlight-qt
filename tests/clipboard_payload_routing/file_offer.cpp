// Files the host copied (clipboard kind 4): offer validation and the
// downloader that feeds a paste, against a loopback TLS server.

#include "streaming/fileoffer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTextStream>
#include <QThread>

#include <atomic>
#include <functional>
#include <thread>

namespace {

bool check(bool value, const char* name, QTextStream& err)
{
    if (!value) {
        err << "FAIL: " << name << '\n';
    }
    return value;
}

QByteArray fixture(const char* name)
{
    QFile file(QStringLiteral(":/clipboard-test/") + QString::fromLatin1(name));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

const QString kId = QStringLiteral("0b5e3a8c-1d2f-4a6b-8c9d-0e1f2a3b4c5d");

QByteArray offerJson(const QJsonArray& files, const QString& id = kId)
{
    return QJsonDocument(QJsonObject{{"id", id}, {"type", "clipboard"}, {"files", files}})
        .toJson(QJsonDocument::Compact);
}

QJsonObject file(const QString& path, double size, bool dir = false)
{
    return QJsonObject{{"path", path}, {"size", size}, {"dir", dir}, {"mtime", 1700000000}};
}

bool parses(const QByteArray& json)
{
    FileOffer offer;
    return FileOffer::parse(json, offer);
}

// Serves GET /api/v1/file-transfer/<id>/<index> from `files`, honouring "Range: bytes=N-".
class FileServer : public QTcpServer
{
public:
    QSslCertificate certificate{fixture("server.pem")};
    QSslKey key{fixture("server-key.pem"), QSsl::Rsa};
    QList<QByteArray> files;
    QList<QByteArray> ranges;     // Range header of each request, empty when none
    std::function<QByteArray(int index, const QByteArray& body, qint64 offset)> respond;

protected:
    void incomingConnection(qintptr descriptor) override
    {
        auto* socket = new QSslSocket(this);
        socket->setSocketDescriptor(descriptor);
        socket->setLocalCertificate(certificate);
        socket->setPrivateKey(key);
        socket->setPeerVerifyMode(QSslSocket::VerifyNone);
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QSslSocket::readyRead, this, [this, socket, buffer]() {
            *buffer += socket->readAll();
            const int end = buffer->indexOf("\r\n\r\n");
            if (end < 0) {
                return;
            }
            const QByteArray head = buffer->left(end);
            buffer->remove(0, end + 4);
            const QList<QByteArray> lines = head.split('\n');
            const QByteArray path = lines.value(0).split(' ').value(1);
            QByteArray range;
            for (const QByteArray& line : lines) {
                if (line.toLower().startsWith("range:")) {
                    range = line.mid(line.indexOf(':') + 1).trimmed();
                }
            }
            ranges.append(range);
            const int index = path.mid(path.lastIndexOf('/') + 1).toInt();
            const QByteArray body = files.value(index);
            qint64 offset = 0;
            if (range.startsWith("bytes=")) {
                offset = range.mid(6, range.size() - 7).toLongLong();
            }
            QByteArray response;
            if (respond) {
                response = respond(index, body, offset);
            } else if (offset > 0) {
                const QByteArray part = body.mid(offset);
                response = "HTTP/1.1 206 Partial Content\r\nContent-Length: " + QByteArray::number(part.size()) +
                           "\r\nContent-Range: bytes " + QByteArray::number(offset) + "-" +
                           QByteArray::number(body.size() - 1) + "/" + QByteArray::number(body.size()) +
                           "\r\n\r\n" + part;
            } else {
                response = "HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
            }
            socket->write(response);
            socket->flush();
        });
        socket->startServerEncryption();
    }
};

// Reads entry `index` from `offset` to the end on a plain thread (a paste
// target blocks like this), while this thread keeps the server running.
struct ReadResult
{
    QByteArray bytes;
    bool failed = false;
};

ReadResult readAll(VirtualFiles::Source& source, std::size_t index, std::uint64_t offset = 0, std::uint32_t chunk = 65536)
{
    ReadResult result;
    std::atomic_bool finished = false;
    std::thread reader([&]() {
        auto stream = source.open(index, offset);
        QByteArray buffer(static_cast<int>(chunk), Qt::Uninitialized);
        while (true) {
            const std::int64_t got = stream->read(buffer.data(), chunk);
            if (got < 0) {
                result.failed = true;
                break;
            }
            if (got == 0) {
                break;
            }
            result.bytes.append(buffer.constData(), static_cast<int>(got));
        }
        finished = true;
    });
    QElapsedTimer timer;
    timer.start();
    while (!finished && timer.elapsed() < 20000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    reader.join();
    return result;
}

} // namespace

bool runFileOfferRegression(QTextStream& err)
{
    bool ok = true;

    // ---- Validation ----
    {
        FileOffer offer;
        const bool parsed = FileOffer::parse(
            offerJson({file("a.txt", 5), file("dir", 0, true), file("dir/sub", 0, true), file("dir/sub/b.bin", 1000)}),
            offer);
        ok &= check(parsed && offer.entries.size() == 4 && offer.totalSize == 1005 &&
                        offer.entries[3].path == u"dir\\sub\\b.bin" && offer.entries[1].dir &&
                        offer.entries[0].mtime == 1700000000 && offer.type == QStringLiteral("clipboard"),
                    "valid offer parses with Windows separators", err);
    }
    ok &= check(!parses(offerJson({file("../a.txt", 1)})), "traversal rejected", err);
    ok &= check(!parses(offerJson({file("dir", 0, true), file("dir/../../a", 1)})), "nested traversal rejected", err);
    ok &= check(!parses(offerJson({file("a\\b.txt", 1)})), "backslash in a name rejected", err);
    ok &= check(!parses(offerJson({file("C:/a.txt", 1)})), "drive letter rejected", err);
    ok &= check(!parses(offerJson({file("/a.txt", 1)})), "absolute path rejected", err);
    ok &= check(!parses(offerJson({file("CON.txt", 1)})), "reserved device name rejected", err);
    ok &= check(!parses(offerJson({file("lpt1", 1)})), "reserved name in lower case rejected", err);
    ok &= check(!parses(offerJson({file("a.", 1)})), "trailing dot rejected", err);
    ok &= check(!parses(offerJson({file("dir/a.txt", 1)})), "file before its folder rejected", err);
    ok &= check(!parses(offerJson({file("A.txt", 1), file("a.TXT", 1)})), "case-insensitive duplicate rejected", err);
    ok &= check(!parses(offerJson({file("a.txt", -1)})), "negative size rejected", err);
    ok &= check(!parses(offerJson({file("a.txt", 1.5)})), "fractional size rejected", err);
    ok &= check(!parses(offerJson({file("d", 3, true)})), "folder with a size rejected", err);
    ok &= check(!parses(offerJson({file(QString(260, 'x'), 1)})), "path over MAX_PATH rejected", err);
    ok &= check(parses(offerJson({file(QString(259, 'x'), 1)})), "path at MAX_PATH - 1 accepted", err);
    ok &= check(!parses(offerJson({file("a.txt", 1)}, QStringLiteral("../../x"))), "bad id rejected", err);
    ok &= check(!parses(offerJson({})), "empty offer rejected", err);
    ok &= check(!parses("[1]"), "non-object rejected", err);

    // ---- Downloads ----
    FileServer server;
    if (!check(!server.certificate.isNull() && !server.key.isNull() && server.listen(QHostAddress::LocalHost),
               "file server listening", err)) {
        return false;
    }
    QByteArray big(3 * 1024 * 1024 + 123, Qt::Uninitialized);
    for (int i = 0; i < big.size(); ++i) {
        big[i] = static_cast<char>((i * 131) & 0xFF);
    }
    server.files = {big, QByteArray("hello"), QByteArray()};

    ClipboardSyncHostContext host;
    host.address = QStringLiteral("127.0.0.1");
    host.httpsPort = server.serverPort();
    host.serverCertificate = server.certificate;

    FileOffer offer;
    FileOffer::parse(offerJson({file("big.bin", big.size()), file("small.txt", 5), file("empty", 0)}), offer);
    {
        FileOfferDownloader source(host, offer);
        const ReadResult whole = readAll(source, 0);
        ok &= check(!whole.failed && whole.bytes == big, "whole file arrives intact", err);
        ok &= check(server.ranges.value(0).isEmpty(), "first read sends no Range", err);

        const ReadResult tail = readAll(source, 0, 1000000);
        ok &= check(!tail.failed && tail.bytes == big.mid(1000000), "resumed read gets the rest", err);
        ok &= check(server.ranges.last() == "bytes=1000000-", "resumed read asks for bytes=N-", err);

        const ReadResult tiny = readAll(source, 1, 0, 1);
        ok &= check(!tiny.failed && tiny.bytes == "hello", "one-byte reads", err);

        const int before = server.ranges.size();
        const ReadResult empty = readAll(source, 2);
        ok &= check(!empty.failed && empty.bytes.isEmpty() && server.ranges.size() == before,
                    "empty file needs no request", err);
    }
    {
        server.respond = [](int, const QByteArray&, qint64) {
            return QByteArray("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nhel");
        };
        FileOfferDownloader source(host, offer);
        const ReadResult shortBody = readAll(source, 1);
        ok &= check(shortBody.failed, "a body shorter than the offer fails the read", err);

        server.respond = [](int, const QByteArray&, qint64) {
            return QByteArray("HTTP/1.1 409 Conflict\r\nContent-Length: 0\r\n\r\n");
        };
        ok &= check(readAll(source, 1).failed, "an HTTP error fails the read", err);

        server.respond = [](int, const QByteArray& body, qint64) {
            // Ignores the Range and sends the whole file with 200.
            return "HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        };
        ok &= check(readAll(source, 0, 10).failed, "a resumed read answered with 200 fails", err);
        server.respond = nullptr;
    }
    {
        auto wrongHost = host;
        wrongHost.serverCertificate = QSslCertificate(fixture("other.pem"));
        FileOfferDownloader source(wrongHost, offer);
        const int before = server.ranges.size();
        ok &= check(readAll(source, 1).failed && server.ranges.size() == before,
                    "an unpaired server certificate gets no request", err);
    }
    {
        FileOfferDownloader source(host, offer);
        ok &= check(readAll(source, 7).failed, "an index outside the offer fails", err);
    }
    return ok;
}
