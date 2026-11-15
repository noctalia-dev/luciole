#include "ui/controls/recursive_permission_editor.h"

#include "ui/builders.h"
#include "ui/controls/card_style.h"
#include "ui/controls/input.h"
#include "ui/controls/segmented.h"

#include <unistd.h>

namespace {
  constexpr std::array<std::uint32_t, 12> bits{0400, 0200, 0100, 0040,  0020,  0010,
                                               0004, 0002, 0001, 04000, 02000, 01000};
  std::pair<std::optional<std::uint32_t>, std::uint32_t> patch(const std::array<Segmented*, 12>& choices) {
    std::uint32_t mode = 0, mask = 0;
    for (std::size_t i = 0; i < choices.size(); ++i) {
      const auto choice = choices[i]->selectedIndex();
      if (choice != 0) {
        mask |= bits[i];
        if (choice == 1) {
          mode |= bits[i];
        }
      }
    }
    return {mask ? std::make_optional(mode) : std::nullopt, mask};
  }
} // namespace
RecursivePermissionEditor::RecursivePermissionEditor(const std::function<void()>& changed) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceMd);
  addChild(
      ui::label(
          {.text = "Keep preserves each item's value. Symbolic links and mounted subtrees are skipped. Collection is "
                   "limited to 16384 items and 128 folder levels; resource-limit failures apply no changes.",
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxLines = 5}
      )
  );
  constexpr std::array names{"Owner read",     "Owner write",   "Owner execute", "Group read",
                             "Group write",    "Group execute", "Others read",   "Others write",
                             "Others execute", "Set user ID",   "Set group ID",  "Sticky bit"};
  for (auto* choices : {&m_files, &m_folders}) {
    Flex* body = nullptr;
    addChild(luciole::card(choices == &m_files ? "Files" : "Folders", &body));
    for (std::size_t i = 0; i < bits.size(); ++i) {
      body->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingV = 2.0F},
              ui::label({.text = names[i], .minWidth = 112.0F, .maxWidth = 112.0F}),
              ui::segmented(
                  {.out = &(*choices)[i],
                   .options =
                       std::vector<ui::SegmentedOption>{
                           {.label = "Keep", .glyph = {}, .tooltip = {}},
                           {.label = "Allow", .glyph = {}, .tooltip = {}},
                           {.label = "Deny", .glyph = {}, .tooltip = {}}
                       },
                   .equalSegmentWidths = true,
                   .flexGrow = 1.0F,
                   .onChange = [changed](std::size_t) { changed(); }}
              )
          )
      );
    }
  }
  Flex* ownership = nullptr;
  addChild(luciole::card("Ownership", &ownership));
  const auto onChange = [changed](const std::string&) { changed(); };
  for (const bool owner : {true, false}) {
    ownership->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingV = 2.0F},
            ui::label(
                {.text = owner ? "Owner" : "Group",
                 .fontWeight = FontWeight::SemiBold,
                 .minWidth = 88.0F,
                 .maxWidth = 88.0F}
            ),
            ui::input(
                {.out = owner ? &m_owner : &m_group,
                 .placeholder = owner ? "Keep owner (name or ID to change)" : "Keep group (name or ID to change)",
                 .flexGrow = 1.0F,
                 .onChange = onChange}
            )
        )
    );
  }
}
luciole::RecursivePermissionEdit RecursivePermissionEditor::edit() const {
  const auto [fileMode, fileMask] = patch(m_files);
  const auto [folderMode, folderMask] = patch(m_folders);
  return {
      .files =
          {.mode = fileMode,
           .owner = m_owner->value().empty() ? std::nullopt : std::make_optional(m_owner->value().substr(0, 1025)),
           .group = m_group->value().empty() ? std::nullopt : std::make_optional(m_group->value().substr(0, 1025)),
           .mask = fileMask},
      .directoryMode = folderMode,
      .directoryMask = folderMask
  };
}
bool RecursivePermissionEditor::canApply() const {
  const auto value = edit();
  return m_ready && (value.files.mode || value.directoryMode || value.files.owner || value.files.group);
}
void RecursivePermissionEditor::sync(const luciole::PermissionSnapshot& snapshot) {
  // Descendants are unknown until Apply. Kernel/provider checks remain authoritative.
  m_ready = snapshot.phase == luciole::PermissionPhase::Ready && !(snapshot.recursive && snapshot.finished);
  for (const auto& choices : {m_files, m_folders}) {
    for (auto* choice : choices) {
      choice->setEnabled(m_ready);
    }
  }
  m_owner->setEnabled(m_ready && ::geteuid() == 0);
  m_group->setEnabled(m_ready);
}
const Node* RecursivePermissionEditor::messageNode() const { return m_group; }
