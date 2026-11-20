#include "app/file_chooser_session.h"

#include "app/page/browser_page.h"
#include "app/workspace_layout.h"
#include "core/input/key_symbols.h"
#include "core/log.h"
#include "fs/file_query.h"
#include "fs/network_info.h"
#include "fs/network_job.h"
#include "platform/desktop_integration.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/select.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <format>
#include <ranges>

namespace {
  const Logger kLog("file-chooser");

  // Listings report links as links, without their target's kind; the chooser policy resolves the selected ones, so
  // every filter keeps links (and entries of unread kind) rather than hiding a linked folder or file.
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

luciole::ChooserDecision FileChooserSession::decision(const std::vector<luciole::DirectoryEntry>& entries) {
  const auto folder = m_page->location();
  if (std::ranges::any_of(entries, &luciole::DirectoryEntry::metadataPending)
      || !std::ranges::any_of(entries, [](const auto& entry) {
           return entry.symlink || entry.type == luciole::FileType::Symlink;
         })) {
    m_typeToken.cancel();
    m_typesPending = false;
    m_typeSources.clear();
    m_typeResolved.clear();
    m_typeError.clear();
    m_openAfterTypes.reset();
    return luciole::chooserDecision(mode(), folder, entries);
  }
  const auto generation = m_page->browser().model().snapshot().generation;
  const bool same = generation == m_typeGeneration
      && folder == m_typeFolder
      && std::ranges::equal(entries, m_typeSources, [](const auto& a, const auto& b) {
                      return a.id == b.id
                          && a.location() == b.location()
                          && a.type == b.type
                          && a.symlink == b.symlink
                          && a.identity() == b.identity()
                          && a.etag() == b.etag()
                          && a.contentRevision == b.contentRevision;
                    });
  if (!same) {
    m_openAfterTypes.reset();
    m_typeToken.cancel();
    m_typeToken = luciole::CancelToken{};
    m_typeSources = entries;
    m_typeFolder = folder;
    m_typeGeneration = generation;
    m_typeResolved.clear();
    m_typeError.clear();
    m_typesPending = true;
    luciole::runIoJob<std::vector<luciole::DirectoryEntry>>(
        folder ? luciole::ioLane(*folder) : luciole::IoLane::Native, m_typeToken,
        [entries = std::vector(entries), token = m_typeToken] mutable {
          for (auto& entry : entries) {
            if (entry.symlink || entry.type == luciole::FileType::Symlink) {
              try {
                entry.type = luciole::readFileType(entry.location(), token);
              } catch (const luciole::FileErrorException& error) {
                if (error.error().kind != luciole::FileErrorKind::Missing) {
                  throw;
                }
                entry.type = luciole::FileType::Symlink;
              }
              entry.symlink = false;
            }
          }
          return entries;
        },
        [this, token = m_typeToken, folder, generation](auto result) {
          if (token.cancelled()) {
            return;
          }
          m_typesPending = false;
          if (m_page->location() != folder || m_page->browser().model().snapshot().generation != generation) {
            m_openAfterTypes.reset();
            m_typeSources.clear();
            return;
          }
          if (result) {
            m_typeResolved = std::move(*result);
          } else {
            m_typeError = result.error().message;
          }
          m_surface->requestUpdate();
          auto activation = std::exchange(m_openAfterTypes, std::nullopt);
          if (activation
              && m_typeError.empty()
              && !m_call->finished()
              && activation->selection == m_page->browser().model().selectedIds()) {
            open(activation->targets);
          }
        }
    );
  }
  if (m_typesPending || !m_typeError.empty()) {
    return {
        .kind = luciole::ChooserDecision::Kind::Unavailable,
        .locations = {},
        .reason = m_typesPending ? std::string_view("Reading item metadata…") : std::string_view(m_typeError)
    };
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
               apply(decision(luciole::chooserSelection(m_page->browser().model())));
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
    // Selecting a file offers to save over it.
    const auto selection = luciole::chooserSelection(m_page->browser().model());
    if (selection.size() == 1
        && selection.front().type != luciole::FileType::Directory
        && selection.front().id != m_namedFrom) {
      m_namedFrom = selection.front().id;
      m_name->setValue(selection.front().name);
      m_confirm.clear();
      m_saveError.clear();
      m_saveToken.cancel();
      m_savePending = false;
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

void FileChooserSession::open(const std::vector<luciole::DirectoryEntry>& targets) {
  // Resolve the same selection the footer displays, including when only one selected link was double-clicked.
  const auto selection = luciole::chooserSelection(m_page->browser().model());
  const bool inSelection = !saving()
      && targets.size() == 1
      && std::ranges::any_of(selection, [&](const auto& entry) { return entry.id == targets.front().id; });
  const auto& entries = inSelection ? selection : targets;
  const auto current = decision(entries);
  if (m_typesPending) {
    m_openAfterTypes.emplace(targets, m_page->browser().model().selectedIds());
    return;
  }
  if (!m_typeError.empty()) {
    return;
  }
  if (targets.size() == 1) {
    const auto& resolved = m_typeSources.empty() ? entries : m_typeResolved;
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
  apply(current);
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
  m_saveToken.cancel();
  m_saveToken = luciole::CancelToken{};
  m_savePending = true;
  m_saveError.clear();
  struct Checked {
    std::vector<luciole::Location> targets;
    std::vector<luciole::Location> existing;
    std::optional<luciole::Location> folder;
  };
  luciole::runIoJob<Checked>(
      luciole::ioLane(*folder), m_saveToken,
      [targets = std::move(targets), enterFolders = m_name != nullptr, token = m_saveToken] mutable {
        Checked result;
        result.targets = std::move(targets);
        for (const auto& target : result.targets) {
          try {
            const auto type = luciole::readFileType(target, token, luciole::FollowSymlinks::No);
            result.existing.push_back(target);
            if (enterFolders
                && (type == luciole::FileType::Directory
                    || (type == luciole::FileType::Symlink
                        && luciole::readFileType(target, token) == luciole::FileType::Directory))) {
              result.folder = target;
              break;
            }
          } catch (const luciole::FileErrorException& error) {
            if (error.error().kind != luciole::FileErrorKind::Missing) {
              throw;
            }
          }
        }
        return result;
      },
      [this, token = m_saveToken, folder, names](auto result) {
        if (token.cancelled()) {
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
  if (event.pressed && !event.preedit && event.modifiers == 0 && KeySymbol::isEscape(event.sym)) {
    cancel();
    return true;
  }
  m_dispatcher->keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  m_surface->requestUpdate();
  return true;
}

void FileChooserSession::windowClosed() {
  m_typeToken.cancel();
  m_saveToken.cancel();
  m_dropdown.closeSelectDropdown();
  m_call->cancel();
}
