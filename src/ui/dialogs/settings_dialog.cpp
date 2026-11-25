#include "ui/dialogs/settings_dialog.h"

#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/card_style.h"
#include "ui/controls/input.h"
#include "ui/controls/select.h"
#include "ui/controls/side_list_metrics.h"
#include "ui/controls/slider.h"
#include "ui/controls/stepper.h"
#include "ui/dialogs/dialog_chrome.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace {
  using P = luciole::BrowserPreference;
  // Like Noctalia's settings rows: every row is at least one control tall, so toggle, slider, dropdown and stepper
  // rows share one height whatever their control measures; descriptions grow a row past it.
  constexpr float kRowPaddingV = 2.0F;
  std::vector<int> widths(int minimum, int maximum, bool automatic = false) {
    std::vector<int> result;
    if (automatic) {
      result.push_back(0);
    }
    for (int value = minimum; value <= maximum; value += 16) {
      result.push_back(value);
    }
    return result;
  }
  std::string_view keybindRejectionMessage(KeybindRejection reason) {
    switch (reason) {
    case KeybindRejection::Super:
      return "Super shortcuts are reserved by the compositor.";
    case KeybindRejection::Modifier:
      return "This shortcut does not accept modifiers.";
    case KeybindRejection::Printable:
      return "Printable keys need Ctrl, Alt or another modifier.";
    }
    return "This shortcut cannot be used.";
  }
  std::string_view keybindScopeTitle(luciole::KeybindScope scope) {
    switch (scope) {
    case luciole::KeybindScope::Controls:
      return "Controls";
    case luciole::KeybindScope::FileBrowser:
      return "File Browser";
    case luciole::KeybindScope::TabsAndWindows:
      return "Tabs and Windows";
    }
    return {};
  }
  // Commands whose defaults include a bare printable key must stay recordable without a modifier.
  ModifierPolicy keybindModifierPolicy(luciole::KeybindCommand command) {
    return command == luciole::KeybindCommand::Validate || command == luciole::KeybindCommand::QuickPreview
        ? ModifierPolicy::Optional
        : ModifierPolicy::Required;
  }
  luciole::KeybindChord toKeybindChord(const KeyChord& chord) { return {chord.sym, chord.modifiers}; }
  std::optional<luciole::KeybindCommand> crossScopeConflict(
      const luciole::KeybindConfig& config, luciole::KeybindCommand command, const luciole::KeybindChord& chord
  ) {
    const auto target = luciole::normalizeKeybindChord(chord);
    for (const auto scope : luciole::kKeybindScopes) {
      if (scope == luciole::keybindCommandScope(command)) {
        continue;
      }
      for (const auto candidate : luciole::keybindCommands(scope)) {
        for (const auto& used : config.chords(candidate)) {
          if (used && luciole::normalizeKeybindChord(*used) == target) {
            return candidate;
          }
        }
      }
    }
    return std::nullopt;
  }
  std::optional<KeyChord> fromKeybindChord(std::optional<luciole::KeybindChord> chord) {
    if (!chord) {
      return std::nullopt;
    }
    return KeyChord{chord->sym, chord->modifiers};
  }
} // namespace

SettingsDialog::SettingsDialog(
    std::function<void(luciole::OverrideEdit)> change, std::function<void()> reset, std::function<void()> close,
    std::function<void()> clearThumbnails, bool compositorSupportsSSD
)
    : m_change(std::move(change)), m_resetAll(std::move(reset)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  luciole::applyDialogStyle(*this);
  auto actions = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm},
      ui::label(
          {.out = &m_status,
           .fontSize = Style::fontSizeCaption,
           .maxWidth = 260.0F,
           .maxLines = 1,
           .configure =
               [](Label& label) {
                 label.setUseMarkup(false);
                 label.setEllipsize(TextEllipsize::End);
               }}
      ),
      ui::button(
          {.out = &m_reset,
           .text = "Reset All Settings",
           .glyph = "restore",
           .controlHeight = Style::controlHeightSm,
           .variant = ButtonVariant::Ghost,
           .onClick = [this] { onResetClicked(); }}
      )
  );
  m_reset->setOnLeave([this] { disarmReset(); });
  addChild(luciole::dialogHeader("Settings", "settings", std::move(close), &m_title, std::move(actions)));
  auto navigation = ui::column(
      {.out = &m_navigation, .align = FlexAlign::Stretch, .width = 192.0F},
      ui::scrollView(
          {.out = &m_navigationScroll,
           .viewportPaddingH = 0.0F,
           .viewportPaddingV = luciole::kSideListViewportPaddingV,
           .fillWidth = true,
           .flexGrow = 1.0F}
      )
  );
  m_navigationScroll->content()->setAlign(FlexAlign::Stretch);
  m_navigationScroll->content()->setGap(Style::spaceXs);
  auto body = ui::scrollView(
      {.out = &m_body,
       .viewportPaddingH = 0.0F,
       .viewportPaddingV = luciole::kSideListViewportPaddingV,
       .fillWidth = true,
       .flexGrow = 1.0F}
  );
  m_body->content()->setAlign(FlexAlign::Stretch);
  addChild(
      ui::row(
          {.align = FlexAlign::Stretch, .gap = Style::spaceMd, .flexGrow = 1.0F}, std::move(navigation),
          ui::box({.fill = colorSpecFromRole(ColorRole::OutlineVariant), .width = Style::borderWidth}), std::move(body)
      )
  );
  section("General", "adjustments-horizontal");
  card("Behavior");
  addChoice(
      P::ActivationMode, "Open Items",
      {{"Single Click", luciole::ActivationMode::SingleClick}, {"Double Click", luciole::ActivationMode::DoubleClick}}
  );
  addChoice(
      P::TypingMode, "When Typing",
      {{"Search Subfolders", luciole::TypingMode::Search},
       {"Filter Current Folder", luciole::TypingMode::Filter},
       {"Select Names", luciole::TypingMode::Typeahead}}
  );
  addChoice(
      P::DropAction, "When Dropping Files",
      {{"Ask What to Do", luciole::DropAction::Menu},
       {"Copy Here", luciole::DropAction::Copy},
       {"Move Here", luciole::DropAction::Move}},
      "Without a modifier key"
  );
  addBool(P::SingleClickLocationEdit, "Single-Click Location Bar to Edit");
  addBool(P::ConfirmPermanentDeletion, "Confirm Permanent Deletion");
  addBool(P::ShowPermanentDeletion, "Show Delete Permanently in Context Menus");
  addBool(P::SwitchToNewTab, "Switch to New Folder Tabs");
  addBool(P::RestoreLastWindow, "Restore Tabs from Last Closed Window");
  addBool(
      P::SingleWindow, "Single Window",
      "Launches and Show Items requests open tabs in the existing window; New Window becomes a new tab"
  );
  card("Terminal");
  addTerminal();
  section("Appearance", "paint");
  card("Window");
  addChoice(
      P::ColorScheme, "Color Scheme",
      {{"Automatic", luciole::ColorScheme::Auto},
       {"Dark", luciole::ColorScheme::Dark},
       {"Light", luciole::ColorScheme::Light}},
      "Automatic follows Noctalia's theme mode"
  );
  addFraction(P::BackgroundOpacity, "Background Opacity", 1.0, 0.01);
  addFraction(P::CornerRadiusScale, "Corner Roundness", 2.0, 0.05, {}, [](double value) {
    Style::setCornerRadiusScale(static_cast<float>(value));
  });
  if (compositorSupportsSSD) {
    addBool(P::UseSSD, "Use System Title Bar And Borders");
  }
  addBool(P::AlwaysShowTabBar, "Always Show Tab Bar");
  addBool(P::AlwaysShowStatusBar, "Always Show Status Bar");
  addBool(P::CompactMenus, "Compact Context Menus");
  addBool(P::ContextMenuIcons, "Icons in Context Menus");
  addBool(P::ShowCloseButtons, "Show Close Buttons");
  addBool(P::ReduceAnimations, "Reduce Animations");
  card("Sidebar");
  addBool(P::ShowSidebar, "Show Sidebar");
  addBool(P::SidebarTree, "Show Folder Tree");
  addBool(P::ShowDrives, "Show Drives");
  addBool(P::ShowNetwork, "Show Network");
  addNumber(P::SidebarWidth, "Sidebar Width", widths(luciole::kMinSidebarWidth, luciole::kMaxSidebarWidth));
  card("Preview");
  addBool(P::ShowPreview, "Show Preview");
  addNumber(P::PreviewWidth, "Preview Width", widths(208, 800));
  section("Views", "layout-grid");
  card("View");
  addChoice(
      P::ViewMode, "View",
      {{"Grid", luciole::ViewMode::Grid}, {"List", luciole::ViewMode::List}, {"Columns", luciole::ViewMode::Miller}}
  );
  addBool(P::RememberFolderViews, "Remember View and Sort per Folder");
  addChoice(
      P::Density, "Density",
      {{"Compact", luciole::ViewDensity::Compact}, {"Comfortable", luciole::ViewDensity::Comfortable}}
  );
  addBool(P::ShowEmblems, "Link and Read-Only Emblems");
  addChoice(
      P::FilenameTruncation, "Long Filenames",
      {{"Shorten in the Middle", luciole::FilenameTruncation::Middle},
       {"Shorten at the End", luciole::FilenameTruncation::End}}
  );
  card("Files");
  addBool(P::ShowHidden, "Show Hidden Files");
  addBool(P::ShowBackups, "Show Backup Files (*~)");
  card("Sorting");
  const Choices<luciole::SortField> sortFields{
      {"Name", luciole::SortField::Name},
      {"Type", luciole::SortField::Type},
      {"Size", luciole::SortField::Size},
      {"Modified", luciole::SortField::Modified},
      {"Accessed", luciole::SortField::Accessed},
      {"Created", luciole::SortField::Created},
      {"Owner", luciole::SortField::Owner},
      {"Group", luciole::SortField::Group},
      {"Permissions", luciole::SortField::Permissions},
      {"Location", luciole::SortField::Location}
  };
  addChoice(P::SortBy, "Sort By", sortFields);
  addBool(P::SortDescending, "Reverse Order");
  addBool(P::DirectoriesFirst, "Folders First");
  addBool(P::HiddenLast, "Hidden Last");
  card("Grid");
  addNumber(P::GridIconSize, "Icon Size", {luciole::kGridIconSizes.begin(), luciole::kGridIconSizes.end()});
  addBool(P::ShowSizeCaption, "File Size Caption");
  addBool(P::ShowTypeCaption, "File Type Caption");
  addBool(P::ShowModifiedCaption, "Modified Date Caption");
  card("Column View");
  addNumber(P::MillerColumnWidth, "Column Width", {180, 208, 240, 260, 300, 360, 480, 640, 800});
  addBool(P::MillerShowParents, "Show Parent Folders");
  section("List Columns", "list-details");
  card("List");
  addBool(P::ExpandableFolders, "Expandable Folders");
  auto orderLabel = ui::label({.fontSize = Style::fontSizeCaption, .maxWidth = 180.0F});
  auto* orderText = orderLabel.get();
  addRow<luciole::DetailsColumnOrder>(
      P::ColumnOrder, "Column Order",
      [orderText](const auto& order) {
        orderText->setText(order == luciole::kDefaultDetailsColumnOrder ? "Default" : "Custom");
      },
      std::move(orderLabel)
  );
  card("Visible Columns");
  addBool(P::ShowTypeColumn, "Type");
  addBool(P::ShowSizeColumn, "Size");
  addBool(P::ShowModifiedColumn, "Modified");
  addBool(P::ShowAccessedColumn, "Accessed");
  addBool(P::ShowCreatedColumn, "Created");
  addBool(P::ShowOwnerColumn, "Owner");
  addBool(P::ShowGroupColumn, "Group");
  addBool(P::ShowPermissionsColumn, "Permissions");
  addBool(P::ShowLocationColumn, "Location");
  card("Column Widths");
  addNumber(P::NameColumnWidth, "Name (0 = auto)", widths(160, 16384, true));
  addNumber(P::TypeColumnWidth, "Type", widths(64, 2048));
  addNumber(P::SizeColumnWidth, "Size", widths(64, 2048));
  addNumber(P::ModifiedColumnWidth, "Modified", widths(64, 2048));
  addNumber(P::AccessedColumnWidth, "Accessed", widths(64, 2048));
  addNumber(P::CreatedColumnWidth, "Created", widths(64, 2048));
  addNumber(P::OwnerColumnWidth, "Owner", widths(64, 2048));
  addNumber(P::GroupColumnWidth, "Group", widths(64, 2048));
  addNumber(P::PermissionsColumnWidth, "Permissions", widths(64, 2048));
  addNumber(P::LocationColumnWidth, "Location", widths(64, 2048));
  section("File Picker", "folder-search");
  card("Opening");
  addChoice(
      P::ChooserStartFolder, "Start Folder",
      {{"Home", luciole::ChooserStartFolder::Home}, {"Last Used Folder", luciole::ChooserStartFolder::Last}},
      "When the application suggests no folder"
  );
  addBool(P::ChooserShowPreview, "Show Preview");
  card("View");
  addChoice(P::ChooserViewMode, "View", {{"Grid", luciole::ViewMode::Grid}, {"List", luciole::ViewMode::List}});
  addNumber(P::ChooserGridIconSize, "Grid Icon Size", {luciole::kGridIconSizes.begin(), luciole::kGridIconSizes.end()});
  addBool(P::ChooserShowHidden, "Show Hidden Files");
  card("Sorting");
  addChoice(P::ChooserSortBy, "Sort By", sortFields);
  addBool(P::ChooserSortDescending, "Reverse Order");
  addBool(P::ChooserDirectoriesFirst, "Folders First");
  addBool(P::ChooserHiddenLast, "Hidden Last");
  section("Thumbnails", "photo");
  card("Generation");
  addChoice(
      P::ThumbnailPolicy, "Show Thumbnails",
      {{"Never", luciole::ThumbnailPolicy::Never},
       {"Local Files", luciole::ThumbnailPolicy::Local},
       {"All Locations", luciole::ThumbnailPolicy::All}},
      "All locations can generate thumbnails on mounted shares. Unmounted locations use cached thumbnails."
  );
  addNumber(P::ThumbnailMaxFileMib, "Image/PDF Size Limit", {1, 1024}, "MiB");
  card("Cache");
  addNumber(P::ThumbnailCacheMib, "Disk Cache Limit", {16, 8192}, "MiB");
  m_section->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = 8.0F, .paddingV = kRowPaddingV, .minHeight = Style::controlHeight},
          ui::label({.text = "Cached Thumbnails", .fontWeight = FontWeight::SemiBold, .flexGrow = 1.0F}),
          ui::button(
              {.out = &m_clearThumbnails,
               .text = "Clear Cache",
               .variant = ButtonVariant::Default,
               .minWidth = 180.0F,
               .maxWidth = 180.0F,
               .onClick = std::move(clearThumbnails)}
          )
      )
  );
  section("Keybinds", "keyboard");
  addKeybinds();
  selectPage(0);
}

void SettingsDialog::section(const std::string& title, const std::string& glyph) {
  const auto index = m_pages.size();
  Button* button = nullptr;
  m_navigationScroll->content()->addChild(
      ui::button(
          {.out = &button,
           .text = title,
           .glyph = glyph,
           .controlHeight = luciole::kSideListRowHeight,
           .contentAlign = ButtonContentAlign::Start,
           .variant = ButtonVariant::Ghost,
           .paddingH = luciole::kSideListRowPaddingH,
           .gap = luciole::kSideListRowGap,
           .onClick = [this, index] { selectPage(index); }}
      )
  );
  m_navButtons.push_back(button);
  m_pageTitles.push_back(title);
  auto page = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceMd});
  m_section = nullptr;
  m_pages.push_back(page.get());
  m_body->content()->addChild(std::move(page));
}
void SettingsDialog::card(const std::string& title) { m_pages.back()->addChild(luciole::card(title, &m_section)); }
void SettingsDialog::selectPage(std::size_t selected) {
  disarmReset();
  m_title->setText(std::format("Settings - {}", m_pageTitles[selected]));
  for (std::size_t index = 0; index < m_pages.size(); ++index) {
    m_pages[index]->setVisible(index == selected);
    m_pages[index]->setParticipatesInLayout(index == selected);
    m_navButtons[index]->setSelected(index == selected);
  }
  m_body->requestScrollToOffset(0);
  m_navigationScroll->requestScrollIntoView(m_navButtons[selected]);
}
void SettingsDialog::setCompact(bool compact) {
  const float width = compact ? 152.0F : 192.0F;
  m_navigation->setMinWidth(width);
  m_navigation->setMaxWidth(width);
}
Node* SettingsDialog::navigation() const { return m_navigation; }
template <class T>
void SettingsDialog::addRow(
    luciole::PreferenceKey<T> key, const std::string& title, std::function<void(const T&)> show,
    std::unique_ptr<Node> control, std::string subtitle
) {
  auto titleLabel = ui::label({.text = title, .fontWeight = FontWeight::SemiBold, .maxLines = 2, .flexGrow = 1.0F});
  std::unique_ptr<Node> copy;
  if (subtitle.empty()) {
    copy = std::move(titleLabel);
  } else {
    copy = ui::column(
        {.align = FlexAlign::Stretch, .gap = 2.0F, .flexGrow = 1.0F}, std::move(titleLabel),
        ui::label(
            {.text = std::move(subtitle),
             .fontSize = Style::fontSizeCaption,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
             .maxLines = 3}
        )
    );
  }
  m_section->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = 8.0F, .paddingV = kRowPaddingV, .minHeight = Style::controlHeight},
          std::move(copy), std::move(control)
      )
  );
  m_rows[key.id()] = [this, key, show = std::move(show)] { show(luciole::preferenceValue(m_current, key)); };
}

template <class T>
void SettingsDialog::addChoice(
    luciole::PreferenceKey<T> key, const std::string& title, const Choices<T>& choices, std::string subtitle
) {
  auto control = std::make_unique<Select>();
  auto* select = control.get();
  std::vector<std::string> options;
  options.reserve(choices.size());
  for (const auto& choice : choices) {
    options.push_back(choice.first);
  }
  select->setOptions(std::move(options));
  select->setMinWidth(180.0F);
  select->setMaxWidth(180.0F);
  select->setFontSize(Style::fontSizeCaption);
  select->setOnSelectionChanged([this, key, choices](std::size_t index, std::string_view) {
    send(key, std::optional<T>(choices[index].second));
  });
  const auto show = [select, choices](const T& value) {
    const auto found = std::ranges::find(choices, value, &Choices<T>::value_type::second);
    select->setSelectedIndexSilently(static_cast<std::size_t>(found - choices.begin()));
  };
  addRow<T>(key, title, show, std::move(control), std::move(subtitle));
}

void SettingsDialog::addTerminal() {
  auto control = std::make_unique<Input>();
  auto* input = control.get();
  input->setMinLayoutWidth(180.0F);
  input->setSize(180.0F, Style::controlHeight);
  input->setPlaceholder("Automatic");
  const auto commit = [this, input] {
    const auto value = input->value();
    if (value != m_current.terminalCommand) {
      send(P::TerminalCommand, std::optional(value));
    }
  };
  input->setOnSubmit([commit](const std::string&) { commit(); });
  input->setOnFocusLoss(commit);
  addRow<std::string>(
      P::TerminalCommand, "Terminal Command",
      [input](const std::string& value) {
        if (!input->inputArea()->focused()) {
          input->setValue(value);
        }
      },
      std::move(control), "For example: foot or kitty. Leave empty to use $TERMINAL, then automatic discovery."
  );
}

void SettingsDialog::addKeybinds() {
  Flex* page = m_pages.back();
  std::vector<ui::SegmentedOption> scopes;
  scopes.reserve(luciole::kKeybindScopes.size());
  for (const auto scope : luciole::kKeybindScopes) {
    scopes.push_back({.label = std::string(keybindScopeTitle(scope)), .glyph = {}, .tooltip = {}});
  }
  page->addChild(
      ui::row(
          {.justify = FlexJustify::Center},
          ui::segmented({.options = std::move(scopes), .selectedIndex = 0, .onChange = [this](std::size_t index) {
                           selectKeybindScope(index);
                         }})
      )
  );

  m_keybindBlocks.reserve(luciole::kKeybindCommandCount);
  for (const auto scope : luciole::kKeybindScopes) {
    auto scopePage = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceMd});
    Flex* scopePointer = scopePage.get();
    page->addChild(std::move(scopePage));
    m_keybindScopePages.push_back(scopePointer);
    m_section = nullptr;
    scopePointer->addChild(luciole::card(std::string(keybindScopeTitle(scope)), &m_section));
    m_section->setGap(Style::spaceMd);

    for (const auto command : luciole::keybindCommands(scope)) {
      const std::size_t blockIndex = m_keybindBlocks.size();
      auto slots = ui::column(
          {.align = FlexAlign::Start, .gap = 2.0F, .minWidth = 220.0F + Style::spaceXs + Style::controlHeightSm}
      );
      KeybindBlock entry{.command = command, .slots = {}};
      entry.slots.reserve(luciole::kMaxKeybindChords);
      for (std::size_t slot = 0; slot < luciole::kMaxKeybindChords; ++slot) {
        KeybindRecorder* recorder = nullptr;
        Button* remove = nullptr;
        auto row = ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceXs},
            ui::keybindRecorder(
                {.out = &recorder,
                 .scale = 1.0F,
                 .unsetPlaceholder = "Add shortcut",
                 .recordingPlaceholder = "Press keys…",
                 .modifierPolicy = keybindModifierPolicy(command),
                 .onCommit =
                     [this, blockIndex, slot, command](KeyChord chord) {
                       auto& target = m_keybindBlocks[blockIndex].slots[slot];
                       const auto keybindChord = toKeybindChord(chord);
                       std::optional<luciole::KeybindChord> original;
                       if (slot < m_current.keybinds.chords(command).size()) {
                         original = m_current.keybinds.chords(command)[slot];
                       }
                       if (const auto conflict =
                               luciole::keybindConflict(m_current.keybinds, command, slot, keybindChord)) {
                         target.recorder->setChord(fromKeybindChord(original));
                         m_pendingStatus = std::format(
                             "{} is already used by {}.", luciole::keybindChordLabel(keybindChord),
                             luciole::keybindCommandLabel(*conflict)
                         );
                         m_pendingStatusError = true;
                         setStatus(m_pendingStatus, true);
                         return;
                       }
                       m_pendingStatus.clear();
                       m_pendingStatusError = false;
                       auto next = m_current.keybinds;
                       luciole::setKeybindChord(next, command, slot, keybindChord);
                       if (!luciole::keybindConfigValid(next)) {
                         target.recorder->setChord(fromKeybindChord(original));
                         m_pendingStatus = keybindRejectionMessage(KeybindRejection::Printable);
                         m_pendingStatusError = true;
                         setStatus(m_pendingStatus, true);
                         return;
                       }
                       if (const auto other = crossScopeConflict(m_current.keybinds, command, keybindChord)) {
                         m_pendingStatus = std::format(
                             "{} is also used by {} in another scope.", luciole::keybindChordLabel(keybindChord),
                             luciole::keybindCommandLabel(*other)
                         );
                         setStatus(m_pendingStatus, false);
                       }
                       send(P::Keybinds, std::optional(next));
                     },
                 .onReject =
                     [this](KeybindRejection reason) {
                       m_pendingStatus = keybindRejectionMessage(reason);
                       m_pendingStatusError = true;
                       setStatus(m_pendingStatus, true);
                     }}
            ),
            ui::button(
                {.out = &remove,
                 .glyph = "close",
                 .glyphSize = Style::fontSizeCaption,
                 .controlHeight = Style::controlHeightSm,
                 .variant = ButtonVariant::Ghost,
                 .tooltip = "Clear shortcut",
                 .minWidth = Style::controlHeightSm,
                 .onClick = [this, command, slot] {
                   m_pendingStatus.clear();
                   m_pendingStatusError = false;
                   auto next = m_current.keybinds;
                   luciole::setKeybindChord(next, command, slot, std::nullopt);
                   send(P::Keybinds, std::optional(next));
                 }}
            )
        );
        auto* rowPointer = row.get();
        slots->addChild(std::move(row));
        entry.slots.push_back({.recorder = recorder, .remove = remove, .row = rowPointer});
      }
      auto block = ui::row(
          {.align = FlexAlign::Start,
           .justify = FlexJustify::SpaceBetween,
           .gap = Style::spaceMd,
           .paddingV = kRowPaddingV},
          ui::label(
              {.text = std::string(luciole::keybindCommandLabel(command)),
               .fontSize = Style::fontSizeBody,
               .fontWeight = FontWeight::SemiBold,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .minWidth = 200.0F,
               .maxLines = 1,
               .ellipsize = TextEllipsize::End}
          ),
          std::move(slots)
      );
      m_section->addChild(std::move(block));
      m_keybindBlocks.push_back(std::move(entry));
    }
  }
  m_rows[P::Keybinds.id()] = [this] { syncKeybinds(); };
  selectKeybindScope(0);
  syncKeybinds();
}

void SettingsDialog::syncKeybinds() {
  for (auto& block : m_keybindBlocks) {
    const auto& chords = m_current.keybinds.chords(block.command);
    std::size_t count = 0;
    while (count < chords.size() && chords[count]) {
      ++count;
    }
    for (std::size_t slot = 0; slot < block.slots.size(); ++slot) {
      auto& widgets = block.slots[slot];
      const bool used = slot < count;
      const bool shown = used || slot == count;
      widgets.row->setVisible(shown);
      widgets.row->setParticipatesInLayout(shown);
      widgets.remove->setVisible(used);
      std::optional<KeyChord> chord;
      if (used) {
        chord = fromKeybindChord(chords[slot]);
      }
      widgets.recorder->setChord(chord);
      widgets.recorder->setEnabled(shown);
    }
  }
}

void SettingsDialog::selectKeybindScope(std::size_t selected) {
  for (std::size_t index = 0; index < m_keybindScopePages.size(); ++index) {
    const bool active = index == selected;
    m_keybindScopePages[index]->setVisible(active);
    m_keybindScopePages[index]->setParticipatesInLayout(active);
  }
  m_body->requestScrollToOffset(0);
}

void SettingsDialog::addBool(luciole::PreferenceKey<bool> key, const std::string& title, std::string subtitle) {
  Toggle* control = nullptr;
  auto row = ui::row(
      {.justify = FlexJustify::End, .width = 180.0F},
      ui::toggle({.out = &control, .onChange = [this, key](bool value) { send(key, std::optional(value)); }})
  );
  addRow<bool>(
      key, title, [control](bool value) { control->setCheckedImmediate(value); }, std::move(row), std::move(subtitle)
  );
}

void SettingsDialog::addNumber(
    luciole::PreferenceKey<int> key, const std::string& title, const std::vector<int>& values, std::string suffix
) {
  auto control = std::make_unique<Stepper>();
  auto* stepper = control.get();
  stepper->setMinWidth(180.0F);
  stepper->setMaxWidth(180.0F);
  stepper->setRange(values.front(), values.back());
  stepper->setStep(16);
  stepper->setValueSuffix(std::move(suffix));
  stepper->setOnValueChanged([this, key, stepper, values](int value) {
    const int previous = luciole::preferenceValue(m_current, key);
    if (key == P::GridIconSize && !std::ranges::contains(values, value)) {
      const auto next =
          value > previous ? std::ranges::upper_bound(values, value) : std::ranges::lower_bound(values, value);
      stepper->setValue(
          value > previous ? (next == values.end() ? values.back() : *next)
                           : (next == values.begin() ? values.front() : *std::prev(next))
      );
    } else if (key == P::NameColumnWidth && value > 0 && value < 160) {
      stepper->setValue(value > previous ? 160 : 0);
    }
  });
  stepper->setOnValueCommitted([this, key](int value) { send(key, std::optional(value)); });
  const auto show = [stepper](int value) {
    if (stepper->value() != value || !stepper->valueField()->inputArea()->focused()) {
      stepper->setValue(value);
    }
  };
  addRow<int>(key, title, show, std::move(control));
}

void SettingsDialog::addFraction(
    luciole::PreferenceKey<double> key, const std::string& title, double maximum, double step, std::string subtitle,
    std::function<void(double)> onPreview
) {
  Label* percent = nullptr;
  auto slider = std::make_unique<Slider>();
  auto* control = slider.get();
  control->setRange(0.0, maximum);
  control->setStep(step);
  control->setFlexGrow(1.0F);
  const auto showPercent = [](Label* label, double value) {
    label->setText(std::format("{}%", std::lround(value * 100)));
  };
  auto row = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm, .width = 180.0F}, std::move(slider),
      ui::label({.out = &percent, .fontSize = Style::fontSizeCaption, .minWidth = 36.0F})
  );
  control->setOnValueChanged([percent, showPercent, onPreview = std::move(onPreview)](double value) {
    showPercent(percent, value);
    if (onPreview) {
      onPreview(value);
    }
  });
  // Whole percents, so a snapped 0.6000000000000001 matches an inherited 0.6.
  control->setOnDragEnd([this, key, control] {
    send(key, std::optional(std::round(control->value() * 100.0) / 100.0));
  });
  const auto show = [control, percent, showPercent](double value) {
    if (!control->dragging()) {
      control->setValue(value);
      showPercent(percent, value);
    }
  };
  addRow<double>(key, title, show, std::move(row), std::move(subtitle));
}

template <class T> void SettingsDialog::send(luciole::PreferenceKey<T> key, std::optional<T> value) {
  // Choosing the inherited value stores no override.
  if (value && *value == luciole::preferenceValue(m_inherited, key)) {
    value.reset();
  }
  luciole::preferenceValue(m_current, key) = value.value_or(luciole::preferenceValue(m_inherited, key));
  m_rows.at(key.id())();
  setStatus("Saving…", false);
  m_change(value ? luciole::OverrideEdit(key, *value) : luciole::OverrideEdit(key, std::nullopt));
}
void SettingsDialog::sync(
    const luciole::BrowserSettings& settings, const luciole::BrowserSettings& inherited, bool busy,
    const std::string& error, bool clearingThumbnails
) {
  m_clearThumbnails->setEnabled(!clearingThumbnails);
  m_clearThumbnails->setText(clearingThumbnails ? "Clearing…" : "Clear Cache");
  m_current = settings;
  m_inherited = inherited;
  for (const auto& [id, show] : m_rows) {
    show();
  }
  const bool transient = !error.empty() || clearingThumbnails || busy;
  const std::string text = !error.empty() ? error
      : clearingThumbnails                ? "Clearing cache…"
      : busy                              ? "Saving…"
                                          : m_pendingStatus;
  setStatus(text, !error.empty() || (!transient && m_pendingStatusError));
}
void SettingsDialog::setStatus(const std::string& text, bool error) {
  // One line in the header: the tooltip shows an ellipsized error in full.
  m_status->setText(text);
  m_status->setTooltip(text);
  m_status->setColor(colorSpecFromRole(error ? ColorRole::Error : ColorRole::OnSurfaceVariant));
}
void SettingsDialog::onResetClicked() {
  if (!m_resetArmed) {
    m_resetArmed = true;
    m_reset->setText("Confirm Reset");
    m_reset->setVariant(ButtonVariant::Destructive);
    return;
  }
  disarmReset();
  m_resetAll();
}
void SettingsDialog::disarmReset() {
  if (!std::exchange(m_resetArmed, false)) {
    return;
  }
  m_reset->setText("Reset All Settings");
  m_reset->setVariant(ButtonVariant::Ghost);
}
void SettingsDialog::reveal(const Node* node) {
  m_navigationScroll->requestScrollIntoView(node);
  m_body->requestScrollIntoView(node);
}
