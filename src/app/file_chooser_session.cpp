#include "app/file_chooser_session.h"

#include "app/page/browser_page.h"
#include "app/workspace_layout.h"
#include "config/keybinds.h"
#include "config/state_store.h"
#include "core/input/key_symbols.h"
#include "core/log.h"
#include "fs/background_job.h"
#include "fs/file_error.h"
#include "platform/desktop_integration.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/select.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <ranges>

namespace {
  const Logger kLog("file-chooser");

  // Listings report links as links, without their target's kind; the session resolves the selected ones on a worker,
  // so every filter keeps links (and entries of unread kind) rather than hiding a linked folder or file.
  bool unresolved(const luciole::EntryBrief& brief) {
    return brief.symlink || brief.type == luciole::FileType::Symlink || brief.type == luciole::FileType::Unknown;
  }

  luciole::DirectoryModel::EntryFilter entryFilter(bool directory, const std::optional<luciole::FileFilter>& filter) {
    using luciole::FileType;
    if (directory) {
      return [](const luciole::EntryBrief& brief, const luciole::DirectoryEntry*) {
        return brief.type == FileType::Directory || unresolved(brief);
      };
    }
    if (!filter) {
      return {};
    }
    return [filter = *filter](const luciole::EntryBrief& brief, const luciole::DirectoryEntry* known) {
      if (brief.type == FileType::Directory || unresolved(brief)) {
        return true;
      }
      const char* type = known != nullptr && known->contentType ? known->contentType->c_str() : nullptr;
      return filter.matches(brief.name, type);
    };
  }
} // namespace

FileChooserSession::FileChooserSession(
    luciole::FileChooserRequest request, std::shared_ptr<luciole::FileChooserCall> call, WaylandConnection& connection,
    RenderContext& renderer
)
    : m_request(std::move(request)), m_call(std::move(call)), m_connection(connection),
      m_dropdown(connection, renderer) {
  m_filters = m_request.filters;
  if (m_request.currentFilter) {
    const auto found = std::ranges::find(m_filters, *m_request.currentFilter);
    if (found == m_filters.end()) {
      m_filters.push_back(*m_request.currentFilter);
      m_activeFilter = m_filters.size() - 1;
    } else {
      m_activeFilter = static_cast<std::size_t>(found - m_filters.begin());
    }
  }
  if (!m_filters.empty()) {
    m_filter.emplace(m_filters[m_activeFilter].patterns);
  }
  for (const auto& choice : m_request.choices) {
    if (choice.options.empty()) {
      m_choices.emplace_back(choice.id, choice.initial == "true" ? "true" : "false");
    } else {
      const auto found = std::ranges::find(choice.options, choice.initial, &std::pair<std::string, std::string>::first);
      m_choices.emplace_back(choice.id, found != choice.options.end() ? found->first : choice.options.front().first);
    }
  }
}

FileChooserSession::~FileChooserSession() {
  m_nameLookup.cancel();
  m_typeToken.cancel();
  m_saveToken.cancel();
  m_dropdown.closeSelectDropdown();
}

std::string FileChooserSession::windowTitle() const {
  if (!m_request.title.empty()) {
    return m_request.title;
  }
  switch (m_request.mode) {
  case luciole::FileChooserMode::Save:
    return "Save File";
  case luciole::FileChooserMode::SaveFiles:
    return "Save Files";
  case luciole::FileChooserMode::Open:
    break;
  }
  return m_request.directory ? "Open Folder" : m_request.multiple ? "Open Files" : "Open File";
}

luciole::ChooserMode FileChooserSession::mode() const {
  return {.directory = m_request.directory, .multiple = m_request.multiple};
}

luciole::ChooserDecision
FileChooserSession::decision(const std::vector<luciole::DirectoryEntry>& entries, bool refresh) {
  const auto folder = m_page->location();
  if (!std::ranges::any_of(entries, &luciole::isLink)) {
    m_typeToken.cancel();
    m_typesPending = false;
    m_typeSources.clear();
    m_typeResolved.clear();
    m_openAfterTypes.reset();
    return luciole::chooserDecision(mode(), folder, entries);
  }
  if (std::ranges::any_of(entries, &luciole::DirectoryEntry::metadataPending)) {
    // Preserve an in-flight lookup and queued activation across transient rescans of the same selected links.
    const bool samePending = m_typesPending
        && folder == m_typeFolder
        && std::ranges::equal(entries, m_typeSources, [](const auto& a, const auto& b) {
                               return a.id == b.id && a.location() == b.location();
                             });
    if (!samePending) {
      m_typeToken.cancel();
      m_typesPending = false;
      m_typeSources.clear();
      m_typeResolved.clear();
      m_openAfterTypes.reset();
    }
    return luciole::chooserDecision(mode(), folder, entries);
  }
  const bool same =
      folder == m_typeFolder && std::ranges::equal(entries, m_typeSources, [](const auto& a, const auto& b) {
        return a.id == b.id
            && a.location() == b.location()
            && a.type == b.type
            && a.symlink == b.symlink
            && a.identity() == b.identity()
            && a.etag() == b.etag()
            && a.contentRevision == b.contentRevision;
      });
  if (!same || (refresh && !m_typesPending)) {
    if (!same) {
      m_openAfterTypes.reset();
    }
    m_typeToken.cancel();
    m_typeToken = luciole::CancelToken{};
    m_typeSources = entries;
    m_typeFolder = folder;
    m_typeResolved.clear();
    m_typesPending = true;
    const std::weak_ptr<int> lifetime = m_lifetime;
    luciole::runJob<std::vector<luciole::DirectoryEntry>>(
        m_typeToken,
        [entries = std::vector(entries), token = m_typeToken] mutable {
          for (auto& entry : entries) {
            if (token.cancelled()) {
              luciole::throwFileError(luciole::FileErrorKind::Cancelled, "Cancelled.");
            }
            if (luciole::isLink(entry)) {
              entry.type = luciole::chosenType(entry);
              entry.symlink = false;
            }
          }
          return entries;
        },
        [this, lifetime, token = m_typeToken, folder](auto result) {
          if (lifetime.expired() || token.cancelled()) {
            return;
          }
          m_typesPending = false;
          if (m_page->location() != folder || !result) {
            m_openAfterTypes.reset();
            m_typeSources.clear();
            if (!result) {
              m_surface->requestUpdate();
            }
            return;
          }
          m_typeResolved = std::move(*result);
          m_surface->requestUpdate();
          auto activation = std::exchange(m_openAfterTypes, std::nullopt);
          if (activation && !m_call->finished() && activation->selection == m_page->browser().model().selectedIds()) {
            if (activation->acceptButton) {
              apply(luciole::chooserDecision(mode(), folder, m_typeResolved));
            } else {
              finishOpen(activation->targets, m_typeResolved);
            }
          }
        }
    );
  }
  if (m_typesPending) {
    return {.kind = luciole::ChooserDecision::Kind::Unavailable, .locations = {}, .reason = "Reading item metadata…"};
  }
  return luciole::chooserDecision(mode(), folder, m_typeResolved);
}

std::unique_ptr<Node> FileChooserSession::attach(
    BrowserPage& page, luciole::WindowSurface& surface, InputDispatcher& dispatcher, std::function<void()> close
) {
  m_page = &page;
  m_surface = &surface;
  m_dispatcher = &dispatcher;
  m_close = std::move(close);
  m_call->setCloseCallback(m_close);
  page.setChooser(
      {.open = [this](const std::vector<luciole::DirectoryEntry>& targets) { open(targets); },
       .cancel = [this] { cancel(); }}
  );
  // Saving names one destination; SaveFiles chooses a folder, like a folder chooser.
  const bool folders = m_request.directory || m_request.mode == luciole::FileChooserMode::SaveFiles;
  page.browser().model().setSingleSelection(!m_request.multiple || saving());
  if (auto filter = entryFilter(folders, m_filter)) {
    page.browser().model().setEntryFilter(std::move(filter));
  }

  auto footer = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingH = Style::spaceMd, .height = kChooserFooterHeight}
  );
  const auto plain = [](Label& label) { label.setUseMarkup(false); };
  if (m_request.mode == luciole::FileChooserMode::Save) {
    footer->addChild(
        ui::input(
            {.out = &m_name,
             .value = m_request.currentName,
             .placeholder = "Name",
             .width = 280.0F,
             .onChange =
                 [this](const std::string&) {
                   m_nameLookup.cancel();
                   m_openAfterTypes.reset();
                   m_confirm.clear();
                   m_saveError.clear();
                   m_saveToken.cancel();
                   m_savePending = false;
                 },
             .onSubmit = [this](const std::string&) { save(); }}
        )
    );
  }
  for (std::size_t index = 0; index < m_request.choices.size(); ++index) {
    const auto& choice = m_request.choices[index];
    if (choice.options.empty()) {
      footer->addChild(
          ui::checkbox({.checked = m_choices[index].second == "true", .onChange = [this, index](bool checked) {
                          m_choices[index].second = checked ? "true" : "false";
                        }})
      );
      footer->addChild(ui::label({.text = choice.label, .configure = plain}));
      continue;
    }
    std::vector<std::string> labels;
    labels.reserve(choice.options.size());
    std::size_t selected = 0;
    for (std::size_t option = 0; option < choice.options.size(); ++option) {
      labels.push_back(choice.options[option].second);
      if (choice.options[option].first == m_choices[index].second) {
        selected = option;
      }
    }
    footer->addChild(ui::label({.text = choice.label, .configure = plain}));
    footer->addChild(
        ui::select(
            {.options = std::move(labels),
             .selectedIndex = selected,
             .onSelectionChanged = [this, index](std::size_t option, std::string_view) {
               m_choices[index].second = m_request.choices[index].options.at(option).first;
             }}
        )
    );
  }
  if (!m_filters.empty()) {
    std::vector<std::string> names;
    names.reserve(m_filters.size());
    for (const auto& filter : m_filters) {
      names.push_back(filter.name);
    }
    footer->addChild(
        ui::select(
            {.options = std::move(names),
             .selectedIndex = m_activeFilter,
             .onSelectionChanged = [this](std::size_t index, std::string_view) { selectFilter(index); }}
        )
    );
  }
  footer->addChild(
      ui::label(
          {.out = &m_reason,
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxLines = 1,
           .textAlign = TextAlign::End,
           .ellipsize = TextEllipsize::End,
           .flexGrow = 1.0F,
           .configure = plain}
      )
  );
  footer->addChild(ui::button({.text = "Cancel", .variant = ButtonVariant::Ghost, .minWidth = 80.0F, .onClick = [this] {
                                 cancel();
                               }}));
  const std::string action = !m_request.acceptLabel.empty() ? m_request.acceptLabel
      : saving()                                            ? "Save"
      : m_request.directory                                 ? "Select"
                                                            : "Open";
  m_acceptText = action;
  footer->addChild(
      ui::button(
          {.out = &m_accept,
           .text = action,
           .enabled = false,
           .variant = ButtonVariant::Primary,
           .minWidth = 88.0F,
           .onClick = [this] {
             if (saving()) {
               save();
             } else {
               const auto selection = luciole::chooserSelection(m_page->browser().model());
               const auto current = decision(selection, true);
               if (m_typesPending) {
                 m_openAfterTypes.emplace(selection, m_page->browser().model().selectedIds(), true);
               } else {
                 apply(current);
               }
             }
           }}
      )
  );
  footer->setPopupContext(&m_dropdown);
  return footer;
}

void FileChooserSession::sync() {
  if (m_page == nullptr) {
    return;
  }
  // The dropdown opens on the window's output, which is known only once the window entered one.
  if (auto* output = m_connection.outputForSurface(m_surface->wlSurface()); output && output != m_dropdownOutput) {
    m_dropdownOutput = output;
    m_dropdown.setParent(m_surface->xdgSurface(), m_surface->wlSurface(), output);
  }
  if (saving()) {
    syncSave();
    return;
  }
  const auto current = decision(luciole::chooserSelection(m_page->browser().model()));
  m_accept->setEnabled(current.kind != luciole::ChooserDecision::Kind::Unavailable);
  m_reason->setText(current.reason);
}

void FileChooserSession::syncSave() {
  const auto folder = m_page->location();
  if (folder != m_folder) {
    m_folder = folder;
    m_namedFrom.reset();
    m_nameLookup.cancel();
    m_openAfterTypes.reset();
    m_confirm.clear();
    m_saveError.clear();
    m_saveToken.cancel();
    m_savePending = false;
  }
  if (m_name != nullptr) {
    // Only once the page is active: activating it focuses its file list.
    if (const PageContext& page = *m_page; !m_nameFocused && page.active()) {
      // Type a name at once; the stem is selected so typing keeps the extension.
      m_nameFocused = true;
      m_dispatcher->setFocus(m_name->inputArea());
      const auto& value = m_name->value();
      const auto dot = value.rfind('.');
      m_name->selectRange(0, dot != std::string::npos && dot > 0 ? dot : value.size());
    }
    const auto selection = luciole::chooserSelection(m_page->browser().model());
    if (selection.size() == 1) {
      if (selection.front().id != m_namedFrom) {
        m_namedFrom = selection.front().id;
        offerName(selection.front());
      }
    } else if (m_namedFrom) {
      m_namedFrom.reset();
      m_nameLookup.cancel();
    }
  }
  std::string_view problem;
  if (!folder || !folder->isNative()) {
    problem = "Only local folders can be chosen.";
  } else if (m_name != nullptr) {
    problem = luciole::fileNameProblem(m_name->value());
  }
  m_accept->setEnabled(problem.empty() && !m_savePending);
  m_accept->setText(m_confirm.empty() ? std::string_view(m_acceptText) : std::string_view("Replace"));
  m_reason->setText(
      !m_confirm.empty()     ? std::string_view(m_reasonText)
          : !problem.empty() ? problem
                             : std::string_view(m_saveError)
  );
}

void FileChooserSession::offerName(const luciole::DirectoryEntry& entry) {
  m_nameLookup.cancel();
  const auto offer = [this](luciole::FileType chosen, const std::string& name) {
    // Selecting a file offers to save over it.
    if (luciole::offersSaveName(chosen)) {
      m_name->setValue(name);
      m_confirm.clear();
      m_saveError.clear();
      m_saveToken.cancel();
      m_savePending = false;
    }
  };
  if (!luciole::isLink(entry)) {
    offer(entry.type, entry.name);
    return;
  }
  // A link's target may sit on a slow or hung mount, so it is followed on a worker. The answer is dropped when the
  // selection changed or the name was edited meanwhile.
  m_nameLookup = luciole::CancelToken{};
  const auto token = m_nameLookup;
  const std::weak_ptr<int> lifetime = m_lifetime;
  luciole::runJob<luciole::FileType>(
      token, [entry] { return luciole::chosenType(entry); },
      [this, lifetime, token, offer, id = entry.id, name = entry.name, typed = m_name->value()](auto result) {
        if (!lifetime.expired() && !token.cancelled() && result && m_namedFrom == id && m_name->value() == typed) {
          offer(*result, name);
          m_surface->requestUpdate();
        }
      }
  );
}

void FileChooserSession::open(const std::vector<luciole::DirectoryEntry>& targets) {
  if (saving()) {
    m_saveToken.cancel();
    m_savePending = false;
  }
  // Resolve the same selection the footer displays, including when only one selected link was double-clicked.
  const auto selection = luciole::chooserSelection(m_page->browser().model());
  const bool inSelection = !saving()
      && targets.size() == 1
      && std::ranges::any_of(selection, [&](const auto& entry) { return entry.id == targets.front().id; });
  const auto& entries = inSelection ? selection : targets;
  (void)decision(entries, true);
  if (m_typesPending) {
    m_openAfterTypes.emplace(targets, m_page->browser().model().selectedIds(), false);
    return;
  }
  if (m_typeSources.empty()) {
    finishOpen(targets, entries);
  } else {
    finishOpen(targets, m_typeResolved);
  }
}

void FileChooserSession::finishOpen(
    const std::vector<luciole::DirectoryEntry>& targets, const std::vector<luciole::DirectoryEntry>& resolved
) {
  if (targets.size() == 1) {
    const auto target = std::ranges::find(resolved, targets.front().id, &luciole::DirectoryEntry::id);
    if (target != resolved.end() && !target->metadataPending && target->type == luciole::FileType::Directory) {
      apply(luciole::chooserDecision({}, m_page->location(), std::span(&*target, 1)));
      return;
    }
  }
  if (saving()) {
    // Enter with nothing selected saves; activating a file saves over it.
    if (targets.size() == 1 && m_name != nullptr) {
      m_name->setValue(targets.front().name);
      m_namedFrom = targets.front().id;
    }
    if (targets.size() <= 1) {
      save();
    }
    return;
  }
  apply(luciole::chooserDecision(mode(), m_page->location(), resolved));
}

void FileChooserSession::save() {
  if (m_call->finished()) {
    return;
  }
  const auto folder = m_page->location();
  if (!folder || !folder->isNative()) {
    return;
  }
  const std::vector<std::string> names =
      m_name != nullptr ? std::vector<std::string>{m_name->value()} : m_request.files;
  std::vector<luciole::Location> targets;
  targets.reserve(names.size());
  for (const auto& name : names) {
    if (!luciole::fileNameProblem(name).empty()) {
      if (m_name != nullptr) {
        return;
      }
      // SaveFiles names come from the application; one that is not a plain name cannot be honoured.
      kLog.warn("SaveFiles asked for an invalid name");
      m_call->fail();
      m_close();
      return;
    }
    targets.push_back(folder->child(name));
  }
  if (m_savePending) {
    return;
  }
  m_nameLookup.cancel();
  m_openAfterTypes.reset();
  m_typeToken.cancel();
  m_typesPending = false;
  m_typeSources.clear();
  m_typeResolved.clear();
  m_saveToken.cancel();
  m_saveToken = luciole::CancelToken{};
  m_savePending = true;
  m_saveError.clear();
  struct Checked {
    std::vector<luciole::Location> targets;
    std::vector<luciole::Location> existing;
    std::optional<luciole::Location> folder;
  };
  const std::weak_ptr<int> lifetime = m_lifetime;
  luciole::runJob<Checked>(
      m_saveToken,
      [targets = std::move(targets), enterFolders = m_name != nullptr, token = m_saveToken] mutable {
        Checked result;
        result.targets = std::move(targets);
        for (const auto& target : result.targets) {
          if (token.cancelled()) {
            luciole::throwFileError(luciole::FileErrorKind::Cancelled, "Cancelled.");
          }
          std::error_code error;
          const auto status = std::filesystem::symlink_status(target.nativePath().value_or(""), error);
          if (error && error != std::errc::no_such_file_or_directory) {
            luciole::throwErrno(error.value());
          }
          if (!std::filesystem::exists(status)) {
            continue;
          }
          result.existing.push_back(target);
          // A folder named like the file is entered, as typing a folder's name does in a location.
          if (enterFolders) {
            std::error_code targetError;
            if (status.type() == std::filesystem::file_type::directory
                || (status.type() == std::filesystem::file_type::symlink
                    && std::filesystem::is_directory(std::filesystem::status(*target.nativePath(), targetError))
                    && !targetError)) {
              result.folder = target;
              break;
            }
          }
        }
        return result;
      },
      [this, lifetime, token = m_saveToken, folder, names](auto result) {
        if (lifetime.expired() || token.cancelled()) {
          return;
        }
        m_savePending = false;
        if (m_call->finished() || m_page->location() != folder || (m_name && m_name->value() != names.front())) {
          return;
        }
        m_surface->requestUpdate();
        if (!result) {
          m_confirm.clear();
          m_saveError = result.error().message;
        } else if (result->folder) {
          m_page->open(*result->folder);
          m_name->setValue({});
          m_confirm.clear();
        } else {
          savedTargets(std::move(result->targets), std::move(result->existing));
        }
      }
  );
}

void FileChooserSession::savedTargets(std::vector<luciole::Location> targets, std::vector<luciole::Location> existing) {
  if (!existing.empty() && m_confirm != existing) {
    m_confirm = std::move(existing);
    m_reasonText = m_name != nullptr ? std::format("“{}” already exists.", m_name->value())
        : m_confirm.size() == 1      ? std::string("1 file already exists.")
                                     : std::format("{} files already exist.", m_confirm.size());
    return;
  }
  apply({.kind = luciole::ChooserDecision::Kind::Accept, .locations = std::move(targets), .reason = {}});
}

void FileChooserSession::selectFilter(std::size_t index) {
  if (index >= m_filters.size()) {
    return;
  }
  m_activeFilter = index;
  m_filter.emplace(m_filters[index].patterns);
  const bool folders = m_request.directory || m_request.mode == luciole::FileChooserMode::SaveFiles;
  m_page->browser().model().setEntryFilter(entryFilter(folders, m_filter));
}

void FileChooserSession::apply(const luciole::ChooserDecision& decision) {
  if (m_call->finished()) {
    return;
  }
  switch (decision.kind) {
  case luciole::ChooserDecision::Kind::Unavailable:
    return;
  case luciole::ChooserDecision::Kind::Enter:
    m_page->open(decision.locations.front());
    return;
  case luciole::ChooserDecision::Kind::Accept: {
    luciole::FileChooserResult result;
    for (const auto& location : decision.locations) {
      result.uris.push_back(location.uri());
    }
    if (!m_filters.empty()) {
      result.currentFilter = m_filters[m_activeFilter];
    }
    result.choices = m_choices;
    kLog.debug("accepting {} item(s)", result.uris.size());
    if (auto folder = m_page->location()) {
      // Remembered for chooser_start_folder = "last"; a lost write only costs the next picker's start folder.
      luciole::runJob<bool>(
          [folder = std::move(*folder)] mutable {
            luciole::StateStore store(luciole::StateStore::pathFromEnvironment());
            return store.load()
                && store.saveChooserFolder(std::move(folder)) != luciole::AtomicWriteState::NotCommitted;
          },
          [](luciole::JobResult<bool> saved) {
            if (!saved || !*saved) {
              kLog.warn("cannot remember the file picker folder");
            }
          }
      );
    }
    m_call->complete(result);
    m_close();
    return;
  }
  }
}

void FileChooserSession::cancel() {
  m_call->cancel();
  m_close();
}

bool FileChooserSession::pointer(const PointerEvent& event) {
  if (!m_dropdown.isSelectDropdownOpen()) {
    return false;
  }
  if (event.surface == m_dropdown.wlSurface()) {
    (void)m_dropdown.onPointerEvent(event);
    return true;
  }
  if (event.type == PointerEvent::Type::Button && event.pressed) {
    // Like a native select: a press outside only closes the dropdown.
    m_dropdown.closeSelectDropdown();
    return true;
  }
  return false;
}

bool FileChooserSession::keyboard(const KeyboardEvent& event) {
  if (m_dropdown.isSelectDropdownOpen()) {
    m_dropdown.onKeyboardEvent(event);
    return true;
  }
  // The page's shortcuts (Backspace, Ctrl+A, typeahead) must not act while the name is being typed; Tab still
  // moves focus through the workspace.
  if (m_name == nullptr || m_dispatcher->focusedArea() != m_name->inputArea() || KeySymbol::isTab(event.sym)) {
    return false;
  }
  if (event.pressed
      && !event.preedit
      && luciole::keybindMatches(luciole::KeybindCommand::Cancel, event.sym, event.modifiers)) {
    cancel();
    return true;
  }
  m_dispatcher->keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  m_surface->requestUpdate();
  return true;
}

void FileChooserSession::windowClosed() {
  m_nameLookup.cancel();
  m_typeToken.cancel();
  m_saveToken.cancel();
  m_dropdown.closeSelectDropdown();
  m_call->cancel();
}
