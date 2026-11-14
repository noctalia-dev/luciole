#include "ui/dialogs/settings_dialog.h"

#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/card_style.h"
#include "ui/controls/input.h"
#include "ui/controls/select.h"
#include "ui/controls/stepper.h"
#include "ui/dialogs/dialog_chrome.h"

#include <algorithm>
#include <format>

namespace {
  using P = luciole::BrowserPreference;
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
} // namespace

SettingsDialog::SettingsDialog(
    std::function<void(luciole::OverrideEdit)> change, std::function<void()> reset, std::function<void()> close,
    std::function<void()> clearThumbnails
)
    : m_change(std::move(change)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  luciole::applyDialogStyle(*this);
  addChild(luciole::dialogHeader("Settings", "settings", std::move(close)));
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
       .flexGrow = 1.0F,
       .configure = [](ScrollView& view) { view.setReserveScrollbarGutter(true); }}
  );
  m_body->content()->setAlign(FlexAlign::Stretch);
  addChild(
      ui::row(
          {.align = FlexAlign::Stretch, .gap = Style::spaceMd, .flexGrow = 1.0F}, std::move(navigation),
          ui::box({.fill = colorSpecFromRole(ColorRole::OutlineVariant), .width = Style::borderWidth}), std::move(body)
      )
  );
  section("General", "adjustments-horizontal");
  addChoice(
      P::ActivationMode, "Open items",
      {{"Single click", luciole::ActivationMode::SingleClick}, {"Double click", luciole::ActivationMode::DoubleClick}}
  );
  addChoice(
      P::TypingMode, "When typing",
      {{"Search subfolders", luciole::TypingMode::Search},
       {"Filter current folder", luciole::TypingMode::Filter},
       {"Select names", luciole::TypingMode::Typeahead}}
  );
  addBool(P::SwitchToNewTab, "Switch to new folder tabs");
  addBool(P::RestoreLastWindow, "Restore tabs from last closed window");
  addBool(P::ShowHidden, "Show hidden files");
  addBool(P::ShowBackups, "Show backup files (*~)");
  addBool(P::AlwaysShowTabBar, "Always show tab bar");
  group("Terminal");
  addTerminal();
  group("Sorting");
  addChoice(
      P::SortBy, "Sort by",
      {{"Name", luciole::SortField::Name},
       {"Type", luciole::SortField::Type},
       {"Size", luciole::SortField::Size},
       {"Modified", luciole::SortField::Modified},
       {"Accessed", luciole::SortField::Accessed},
       {"Created", luciole::SortField::Created},
       {"Owner", luciole::SortField::Owner},
       {"Group", luciole::SortField::Group},
       {"Permissions", luciole::SortField::Permissions},
       {"Location", luciole::SortField::Location}}
  );
  addBool(P::SortDescending, "Reverse order");
  addBool(P::DirectoriesFirst, "Folders first");
  section("Views", "layout-grid");
  addChoice(
      P::ViewMode, "View",
      {{"Grid", luciole::ViewMode::Grid}, {"List", luciole::ViewMode::List}, {"Columns", luciole::ViewMode::Miller}}
  );
  addChoice(
      P::Density, "Density",
      {{"Compact", luciole::ViewDensity::Compact}, {"Comfortable", luciole::ViewDensity::Comfortable}}
  );
  addBool(P::ShowEmblems, "Link and read-only emblems");
  group("Grid");
  addNumber(P::GridIconSize, "Icon size", {luciole::kGridIconSizes.begin(), luciole::kGridIconSizes.end()});
  addBool(P::ShowSizeCaption, "File size caption");
  addBool(P::ShowTypeCaption, "File type caption");
  addBool(P::ShowModifiedCaption, "Modified date caption");
  group("Columns");
  addNumber(P::MillerColumnWidth, "Column width", {180, 208, 240, 260, 300, 360, 480, 640, 800});
  addBool(P::MillerShowParents, "Show parent folders");
  section("List columns", "list-details");
  addBool(P::ExpandableFolders, "Expandable folders");
  auto orderLabel = ui::label({.fontSize = Style::fontSizeCaption, .maxWidth = 180.0F});
  auto* orderText = orderLabel.get();
  addRow<luciole::DetailsColumnOrder>(
      P::ColumnOrder, "Column order",
      [orderText](const auto& order) {
        orderText->setText(order == luciole::kDefaultDetailsColumnOrder ? "Default" : "Custom");
      },
      std::move(orderLabel)
  );
  group("Visible columns");
  addBool(P::ShowTypeColumn, "Type");
  addBool(P::ShowSizeColumn, "Size");
  addBool(P::ShowModifiedColumn, "Modified");
  addBool(P::ShowAccessedColumn, "Accessed");
  addBool(P::ShowCreatedColumn, "Created");
  addBool(P::ShowOwnerColumn, "Owner");
  addBool(P::ShowGroupColumn, "Group");
  addBool(P::ShowPermissionsColumn, "Permissions");
  addBool(P::ShowLocationColumn, "Location");
  group("Column widths");
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
  section("Sidebar & preview", "layout-sidebar");
  group("Sidebar");
  addBool(P::ShowSidebar, "Show sidebar");
  addBool(P::SidebarTree, "Show folder tree");
  addBool(P::ShowDrives, "Show drives");
  addNumber(P::SidebarWidth, "Sidebar width", widths(luciole::kMinSidebarWidth, luciole::kMaxSidebarWidth));
  group("Preview");
  addBool(P::ShowPreview, "Show preview");
  addNumber(P::PreviewWidth, "Preview width", widths(208, 800));
  section("Thumbnails", "photo");
  addChoice(
      P::ThumbnailPolicy, "Show thumbnails",
      {{"Never", luciole::ThumbnailPolicy::Never},
       {"Local files", luciole::ThumbnailPolicy::Local},
       {"All locations", luciole::ThumbnailPolicy::All}}
  );
  m_section->addChild(
      ui::label(
          {.text =
               "All locations can generate thumbnails on mounted shares. Unmounted locations use cached thumbnails.",
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxLines = 2}
      )
  );
  addNumber(P::ThumbnailMaxFileMib, "Image/PDF size limit", {1, 1024}, "MiB");
  addNumber(P::ThumbnailMaxMegapixels, "Image pixel limit", {1, 64}, "MP");
  addNumber(P::ThumbnailCacheMib, "Disk cache limit", {16, 1024}, "MiB");
  m_section->addChild(
      ui::button(
          {.out = &m_clearThumbnails,
           .text = "Clear thumbnail cache",
           .variant = ButtonVariant::Default,
           .onClick = std::move(clearThumbnails)}
      )
  );
  selectPage(0);
  // Fixed footer: status text never changes the dialog's layout.
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceMd},
          ui::label(
              {.out = &m_status,
               .fontSize = Style::fontSizeCaption,
               .maxLines = 2,
               .flexGrow = 1.0F,
               .configure = [](Label& label) { label.setUseMarkup(false); }}
          ),
          ui::button(
              {.text = "Reset all settings",
               .glyph = "restore",
               .controlHeight = Style::controlHeightSm,
               .variant = ButtonVariant::Ghost,
               .onClick = std::move(reset)}
          )
      )
  );
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
  auto page = ui::column(
      {.align = FlexAlign::Stretch, .gap = Style::spaceMd},
      ui::row(
          {.align = FlexAlign::Center, .gap = 8.0F},
          ui::glyph({.glyph = glyph, .color = colorSpecFromRole(ColorRole::Primary)}),
          ui::label(
              {.text = title,
               .fontSize = Style::fontSizeHeader,
               .fontWeight = FontWeight::SemiBold,
               .color = colorSpecFromRole(ColorRole::Primary)}
          )
      )
  );
  page->addChild(ui::column({.out = &m_section, .align = FlexAlign::Stretch, .gap = Style::spaceXs}));
  m_pages.push_back(page.get());
  m_body->content()->addChild(std::move(page));
}
void SettingsDialog::group(const std::string& title) {
  // Same caption heading as the places sidebar; top space separates it from the previous group.
  const float top = m_section->children().empty() ? 0.0F : Style::spaceMd;
  m_section->addChild(
      ui::column(
          {.align = FlexAlign::Start,
           .configure = [top](Flex& heading) { heading.setPadding(top, 0.0F, Style::spaceXs, 0.0F); }},
          ui::label(
              {.text = title,
               .fontSize = Style::fontSizeCaption,
               .fontWeight = FontWeight::SemiBold,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
          )
      )
  );
}
void SettingsDialog::selectPage(std::size_t selected) {
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
template <class T>
void SettingsDialog::addRow(
    luciole::PreferenceKey<T> key, const std::string& title, std::function<void(const T&)> show,
    std::unique_ptr<Node> control
) {
  m_section->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = 8.0F, .paddingV = 6.0F},
          ui::label({.text = title, .fontWeight = FontWeight::SemiBold, .maxLines = 2, .flexGrow = 1.0F}),
          std::move(control)
      )
  );
  m_rows[key.id()] = [this, key, show = std::move(show)] { show(luciole::preferenceValue(m_current, key)); };
}

template <class T>
void SettingsDialog::addChoice(luciole::PreferenceKey<T> key, const std::string& title, const Choices<T>& choices) {
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
  addRow<T>(key, title, show, std::move(control));
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
      P::TerminalCommand, "Terminal command",
      [input](const std::string& value) {
        if (!input->inputArea()->focused()) {
          input->setValue(value);
        }
      },
      std::move(control)
  );
  m_section->addChild(
      ui::label(
          {.text = "For example: foot or kitty. Leave empty to use $TERMINAL, then automatic discovery.",
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxLines = 3}
      )
  );
}

void SettingsDialog::addBool(luciole::PreferenceKey<bool> key, const std::string& title) {
  Toggle* control = nullptr;
  auto row = ui::row(
      {.justify = FlexJustify::End, .width = 180.0F},
      ui::toggle({.out = &control, .onChange = [this, key](bool value) { send(key, std::optional(value)); }})
  );
  addRow<bool>(key, title, [control](bool value) { control->setCheckedImmediate(value); }, std::move(row));
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

template <class T> void SettingsDialog::send(luciole::PreferenceKey<T> key, std::optional<T> value) {
  // Choosing the inherited value stores no override.
  if (value && *value == luciole::preferenceValue(m_inherited, key)) {
    value.reset();
  }
  luciole::preferenceValue(m_current, key) = value.value_or(luciole::preferenceValue(m_inherited, key));
  m_rows.at(key.id())();
  m_status->setText("Saving…");
  m_status->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_change(value ? luciole::OverrideEdit(key, *value) : luciole::OverrideEdit(key, std::nullopt));
}
void SettingsDialog::sync(
    const luciole::BrowserSettings& settings, const luciole::BrowserSettings& inherited, bool busy,
    const std::string& error, bool clearingThumbnails
) {
  m_clearThumbnails->setEnabled(!clearingThumbnails);
  m_clearThumbnails->setText(clearingThumbnails ? "Clearing…" : "Clear thumbnail cache");
  m_current = settings;
  m_inherited = inherited;
  for (const auto& [id, show] : m_rows) {
    show();
  }
  m_status->setText(!error.empty() ? error : clearingThumbnails ? "Clearing cache…" : busy ? "Saving…" : "");
  m_status->setColor(colorSpecFromRole(error.empty() ? ColorRole::OnSurfaceVariant : ColorRole::Error));
}
void SettingsDialog::reveal(const Node* node) {
  m_navigationScroll->requestScrollIntoView(node);
  m_body->requestScrollIntoView(node);
}
