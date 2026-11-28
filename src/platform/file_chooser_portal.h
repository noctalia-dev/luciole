#pragma once

#include "fs/file_filter.h"
#include "fs/location.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using GVariant = struct _GVariant;
using GDBusConnection = struct _GDBusConnection;
using GDBusMethodInvocation = struct _GDBusMethodInvocation;
using GDBusNodeInfo = struct _GDBusNodeInfo;

namespace luciole {
  struct FileChooserFilter {
    std::string name;
    std::vector<FilePattern> patterns;
    bool operator==(const FileChooserFilter&) const = default;
  };

  struct FileChooserChoice {
    std::string id, label;
    // (id, label) pairs; empty for a boolean choice.
    std::vector<std::pair<std::string, std::string>> options;
    std::string initial;
  };

  // The portal method that asked: OpenFile, SaveFile or SaveFiles.
  enum class FileChooserMode : std::uint8_t { Open, Save, SaveFiles };

  struct FileChooserRequest {
    FileChooserMode mode = FileChooserMode::Open;
    std::string handle, appId, parentWindow, title, acceptLabel;
    bool modal = true, multiple = false, directory = false;
    std::vector<FileChooserFilter> filters;
    std::optional<FileChooserFilter> currentFilter;
    std::vector<FileChooserChoice> choices;
    std::optional<Location> currentFolder;
    // SaveFile: the suggested name, raw bytes; `current_file` sets it and the folder from one path.
    std::string currentName;
    // SaveFiles: the names to save, raw bytes, in request order.
    std::vector<std::string> files;
  };

  struct FileChooserResult {
    std::vector<std::string> uris;
    std::optional<FileChooserFilter> currentFilter;
    std::vector<std::pair<std::string, std::string>> choices;
  };

  // Parses the (o s s s a{sv}) body of a `mode` call; wrong-typed options are ignored, unknown pattern kinds skipped.
  FileChooserRequest parseFileChooserRequest(FileChooserMode mode, GVariant* parameters);
  // Floating a{sv}: "uris" always; "current_filter" and "choices" only when present/non-empty.
  GVariant* fileChooserResults(const FileChooserResult& result);

  // One pending FileChooser invocation plus its exported org.freedesktop.impl.portal.Request object.
  // Only FileChooserService creates calls. A call holds its own connection reference, so it may
  // outlive the service.
  class FileChooserCall {
  public:
    ~FileChooserCall(); // answers 2 if still pending
    FileChooserCall(const FileChooserCall&) = delete;
    FileChooserCall& operator=(const FileChooserCall&) = delete;

    void complete(const FileChooserResult& result); // response 0
    void cancel();                                  // response 1
    void fail();                                    // response 2, empty results
    bool finished() const;
    // Delays all results until modal cleanup is acknowledged. The continuation owns the reply independently
    // of this call; invoke it exactly once, with false when cleanup cannot complete. Empty means no barrier.
    void setReplyBarrier(std::function<void(std::function<void(bool)>)> barrier);
    // Runs after a Close from the original sender has queued response 2 through the reply barrier.
    void setCloseCallback(std::function<void()> callback);

  private:
    friend class FileChooserService;
    FileChooserCall(GDBusMethodInvocation* invocation, GDBusConnection* connection, std::string sender);
    bool exportRequest(const std::string& handle, GDBusNodeInfo* node);
    void finish(unsigned response, GVariant* results, GDBusMethodInvocation* close = nullptr);
    static void closeCall(
        GDBusConnection* connection, const char* sender, const char* path, const char* interface, const char* method,
        GVariant* parameters, GDBusMethodInvocation* invocation, void* data
    );

    GDBusMethodInvocation* m_invocation = nullptr;
    GDBusConnection* m_connection = nullptr;
    std::string m_sender;
    unsigned m_registration = 0;
    std::function<void()> m_closeCallback;
    std::function<void(std::function<void(bool)>)> m_replyBarrier;
  };

  // Owns org.freedesktop.impl.portal.desktop.luciole for one bus-activated chooser process.
  class FileChooserService {
  public:
    using Handler = std::function<void(FileChooserRequest, std::shared_ptr<FileChooserCall>)>;

    explicit FileChooserService(Handler handler);
    ~FileChooserService(); // release() then flushes so replies leave before exit
    FileChooserService(const FileChooserService&) = delete;
    FileChooserService& operator=(const FileChooserService&) = delete;

    // True from start until the first request, the activation timeout, or the name is lost.
    bool awaitingRequest() const { return m_awaiting; }
    void release();

  private:
    static void busAcquired(GDBusConnection* connection, const char* name, void* data);
    static void nameLost(GDBusConnection* connection, const char* name, void* data);
    static void methodCall(
        GDBusConnection* connection, const char* sender, const char* path, const char* interface, const char* method,
        GVariant* parameters, GDBusMethodInvocation* invocation, void* data
    );
    void stopAwaiting();

    Handler m_handler;
    GDBusNodeInfo* m_node = nullptr;
    GDBusNodeInfo* m_requestNode = nullptr;
    GDBusConnection* m_connection = nullptr;
    unsigned m_owner = 0;
    unsigned m_registration = 0;
    unsigned m_timeout = 0;
    bool m_awaiting = false;
  };
} // namespace luciole
