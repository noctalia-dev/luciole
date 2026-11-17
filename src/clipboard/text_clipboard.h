#pragma once

#include "clipboard/file_payload.h"
#include "core/text_clipboard.h"
#include "render/core/image_decoder.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <wayland-client.h>

namespace luciole {
  struct ClipboardFiles {
    FilePayload payload;
    std::uint64_t revision;
  };
  using DropCompletion = std::function<void(bool)>;
  enum class DragAction { Copy, Move, Ask };
  struct DropOptions {
    DragAction action = DragAction::Copy;
    bool canCopy = false, canMove = false;
    // Select once before starting work. Link uses the non-destructive Copy wire action.
    std::function<bool(TransferAction)> select;
  };
  using FileDropHandler = std::function<void(FilePayload, DropOptions, DropCompletion)>;
  struct FileDropTarget {
    FileDropHandler receive = nullptr;
    // The source application performs extraction; completion acknowledges only the request handoff.
    std::function<void(ExtractionOffer, DropCompletion)> extract = nullptr;
    std::function<void(std::string)> failed = nullptr;
    DragAction preferred = DragAction::Move;
    bool copyOnly = false;
    bool moveOnly = false;
  };

  // Text, file selection and drag/drop share the ordinary application data device.
  class WaylandTextClipboard final : public TextClipboard {
  public:
    WaylandTextClipboard(
        wl_display* display, wl_data_device_manager* manager, wl_seat* seat, std::function<std::uint32_t()> serial,
        wl_compositor* compositor, wl_shm* shm
    );
    ~WaylandTextClipboard() override;
    WaylandTextClipboard(const WaylandTextClipboard&) = delete;
    WaylandTextClipboard& operator=(const WaylandTextClipboard&) = delete;
    bool available() const;
    std::optional<std::string> clipboardText() override;
    void setClipboardText(std::string text) override;
    void requestClipboardText(std::function<void(std::optional<std::string>)> callback) override;

    bool setFiles(FilePayload payload);
    bool hasFiles() const;
    bool isCut(const Location& location) const;
    void requestFiles(std::function<void(std::optional<ClipboardFiles>)> callback);
    bool claimMove(std::uint64_t revision);
    void finishMove(const ClipboardFiles& selection, const std::vector<TransferResult>& results);
    // Selection notifications precede keyboard enter. Resume only after both.
    void keyboardFocusEntered();
    void setChangedCallback(std::function<void()> callback);
    bool startDrag(
        FilePayload payload, wl_surface* origin, std::uint32_t pressSerial, const LoadedImageFile* image,
        std::function<void()> finished, std::optional<DragAction> forcedAction,
        std::function<void(std::optional<DragAction>)> actionChanged
    );
    const FilePayload* dragFiles() const;
    void refreshDrag();
    void cancelDrag();
    void setDropCallbacks(std::function<FileDropTarget(wl_surface*, float, float)> hover, std::function<void()> leave);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
