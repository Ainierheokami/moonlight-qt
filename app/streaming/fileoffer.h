#pragma once

// Files the host copied, offered through the clipboard (wire kind 4).
//
// The host agent registers copied paths with Sunshine and sends the offer it
// gets back, a UTF-8 JSON object:
//
//   {"id": "<uuid>", "type": "clipboard" | "send", "total_size": N,
//    "expires_in": S, "files": [{"path": "dir/a.txt", "size": N,
//    "dir": false, "mtime": T}, ...]}
//
// Paths are relative and '/'-separated, folders before what they contain.
// Each file is fetched from GET /api/v1/file-transfer/<id>/<index> on the
// paired HTTPS port, only when a paste target reads it.

#include "clipboardsync.h"
#include "virtualfilesource.h"

#include <QByteArray>
#include <QString>

#include <memory>
#include <vector>

struct FileOffer
{
    // Bounds shared with Sunshine (src/file_transfer.h).
    static constexpr int MAX_ENTRIES = 10000;
    static constexpr int MAX_PATH_UNITS = 259;

    QString id;
    QString type;
    quint64 totalSize = 0;
    std::vector<VirtualFiles::Entry> entries;

    // Validates everything a paste target would trust: ids, sizes and every
    // path component (no traversal, no names Windows reserves or rejects, no
    // entry whose folder was not listed before it, no duplicates).
    static bool parse(const QByteArray& json, FileOffer& out, QString* error = nullptr);
};

// Fetches the files of one offer from Sunshine on a thread of its own, so a
// paste target can block in Reader::read() on any thread, the GUI thread
// included (Windows delivers a paste's stream reads there).
class FileOfferDownloader : public VirtualFiles::Source
{
public:
    FileOfferDownloader(const ClipboardSyncHostContext& host, const FileOffer& offer);
    ~FileOfferDownloader() override;

    std::unique_ptr<VirtualFiles::Reader> open(std::size_t index, std::uint64_t offset) override;

    // Shared with readers so a stream outlives the clipboard object that made it.
    struct Core;

private:
    std::shared_ptr<Core> m_Core;
};
