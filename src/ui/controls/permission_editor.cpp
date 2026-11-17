#include "ui/controls/permission_editor.h"

#include "ui/browser/file_error_text.h"
#include "ui/builders.h"
#include "ui/controls/card_style.h"
#include "ui/controls/input.h"
#include "ui/controls/recursive_permission_editor.h"
#include "ui/controls/segmented.h"
#include "ui/controls/toggle.h"

#include <algorithm>
#include <format>
#include <string_view>

namespace {
  constexpr std::array<std::uint32_t, 12> bits{0400, 0200, 0100, 0040,  0020,  0010,
                                               0004, 0002, 0001, 04000, 02000, 01000};
  constexpr float labelWidth = 88.0F;
  // Row title on the left: SemiBold, like Settings rows; an optional caption label goes under it.
  std::unique_ptr<Flex> rowTitle(const char* text, Label** caption = nullptr, float width = 0.0F) {
    auto copy = ui::column(
        {.align = FlexAlign::Stretch,
         .gap = 2.0F,
         .minWidth = width > 0.0F ? std::optional(width) : std::nullopt,
         .maxWidth = width > 0.0F ? std::optional(width) : std::nullopt,
         .flexGrow = width > 0.0F ? 0.0F : 1.0F},
        ui::label({.text = text, .fontWeight = FontWeight::SemiBold})
    );
    if (caption != nullptr) {
      copy->addChild(
          ui::label(
              {.out = caption,
               .fontSize = Style::fontSizeCaption,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .maxLines = 2}
          )
      );
    }
    return copy;
  }
  std::unique_ptr<Segmented> choice(Segmented** out, std::function<void()> changed) {
    return ui::segmented(
        {.out = out,
         .options =
             std::vector<ui::SegmentedOption>{
                 {.label = "Keep", .glyph = {}, .tooltip = {}},
                 {.label = "Allow", .glyph = {}, .tooltip = {}},
                 {.label = "Deny", .glyph = {}, .tooltip = {}}
             },
         .equalSegmentWidths = true,
         .flexGrow = 1.0F,
         .onChange = [changed = std::move(changed)](std::size_t) { changed(); }}
    );
  }
} // namespace
std::string permissionStatusText(const luciole::PermissionSnapshot& snapshot) {
  return snapshot.error ? luciole::fileErrorText(*snapshot.error) : snapshot.message;
}
std::string permissionModeText(std::uint32_t mode) {
  std::string text = "---------";
  constexpr std::string_view letters = "rwxrwxrwx";
  for (std::size_t i = 0; i < 9; ++i) {
    if ((mode & bits[i]) != 0) {
      text[i] = letters[i];
    }
  }
  const auto special = [&](std::size_t index, std::uint32_t bit, char set, char unset) {
    if ((mode & bit) != 0) {
      text[index] = text[index] == 'x' ? set : unset;
    }
  };
  special(2, 04000, 's', 'S');
  special(5, 02000, 's', 'S');
  special(8, 01000, 't', 'T');
  return std::format("{}  {:04o}", text, mode & 07777U);
}
PermissionEditor::PermissionEditor(std::function<void()> edited, bool multiple, bool folders)
    : m_multiple(multiple), m_edited(std::move(edited)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceMd);
  const auto onChanged = [this] { changed(); };
  if (folders) {
    Flex* scope = nullptr;
    addChild(luciole::card({}, &scope));
    scope->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingV = 2.0F},
            ui::column(
                {.align = FlexAlign::Stretch, .gap = 2.0F, .flexGrow = 1.0F},
                ui::label({.text = "Include folder contents", .fontWeight = FontWeight::SemiBold}),
                ui::label(
                    {.text = "Change every file and folder inside, per kind.",
                     .fontSize = Style::fontSizeCaption,
                     .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
                )
            ),
            ui::toggle({.out = &m_recursive, .onChange = [this](bool active) {
                          m_direct->setVisible(!active);
                          m_direct->setParticipatesInLayout(!active);
                          m_tree->setVisible(active);
                          m_tree->setParticipatesInLayout(active);
                          changed();
                        }})
        )
    );
    auto tree = std::make_unique<RecursivePermissionEditor>([this] { changed(); });
    m_tree = tree.get();
    tree->setVisible(false);
    tree->setParticipatesInLayout(false);
    addChild(std::move(tree));
  }
  auto direct = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceMd});
  m_direct = direct.get();
  // The current mode sits in the Access title as a monospace badge, e.g. "rwx------  0700".
  auto badge = ui::row(
      {.align = FlexAlign::Center,
       .paddingV = 2.0F,
       .paddingH = Style::spaceSm,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainer),
       .styleRadius = Style::radiusMd},
      ui::label(
          {.out = &m_mode,
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .configure = [multiple](Label& label) {
             if (!multiple) {
               label.setFontFamily("monospace");
             }
           }}
      )
  );
  Flex* access = nullptr;
  direct->addChild(luciole::card("Access", &access, std::move(badge)));
  Flex* special = nullptr;
  auto specialCard = luciole::card("Special permissions", &special);
  if (multiple) {
    constexpr std::array names{"Owner read",     "Owner write",   "Owner execute", "Group read",
                               "Group write",    "Group execute", "Others read",   "Others write",
                               "Others execute", "Set user ID",   "Set group ID",  "Sticky bit"};
    for (std::size_t i = 0; i < bits.size(); ++i) {
      (i < 9 ? access : special)
          ->addChild(
              ui::row(
                  {.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingV = 2.0F},
                  ui::label({.text = names[i], .minWidth = 112.0F, .maxWidth = 112.0F}),
                  choice(&m_choices[i], onChanged),
                  ui::label(
                      {.out = &m_current[i], .fontSize = Style::fontSizeCaption, .minWidth = 64.0F, .maxWidth = 64.0F}
                  )
              )
          );
    }
  } else {
    auto header = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm});
    header->addChild(ui::label({.text = "", .minWidth = labelWidth, .maxWidth = labelWidth}));
    for (const auto* text : {"Read", "Write", "Execute"}) {
      header->addChild(
          ui::column(
              {.align = FlexAlign::Center, .flexGrow = 1.0F},
              ui::label(
                  {.text = text,
                   .fontSize = Style::fontSizeCaption,
                   .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
              )
          )
      );
    }
    access->addChild(std::move(header));
    constexpr std::array names{"Owner", "Group", "Others"};
    for (std::size_t row = 0; row < names.size(); ++row) {
      auto controls = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingV = 2.0F});
      controls->addChild(
          ui::label(
              {.text = names[row], .fontWeight = FontWeight::SemiBold, .minWidth = labelWidth, .maxWidth = labelWidth}
          )
      );
      for (std::size_t column = 0; column < 3; ++column) {
        controls->addChild(
            ui::column(
                {.align = FlexAlign::Center, .flexGrow = 1.0F},
                ui::toggle({.out = &m_bits[row * 3 + column], .onChange = [this](bool) { changed(); }})
            )
        );
      }
      access->addChild(std::move(controls));
    }
    constexpr std::array specials{"Set user ID", "Set group ID", "Sticky bit"};
    constexpr std::array hints{
        "Executables run as the file's owner.", "Run as the group; new items inherit the folder's group.",
        "Only owners can delete or rename items inside."
    };
    for (std::size_t i = 0; i < specials.size(); ++i) {
      Label* hint = nullptr;
      special->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingV = 2.0F}, rowTitle(specials[i], &hint),
              ui::toggle({.out = &m_bits[i + 9], .onChange = [this](bool) { changed(); }})
          )
      );
      hint->setText(hints[i]);
    }
  }
  direct->addChild(std::move(specialCard));
  Flex* ownership = nullptr;
  direct->addChild(luciole::card("Ownership", &ownership));
  const auto change = [this](const std::string&) { changed(); };
  ownership->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingV = 2.0F},
          rowTitle("Owner", &m_ownerId, labelWidth),
          ui::input({.out = &m_owner, .placeholder = "Name or numeric ID", .flexGrow = 1.0F, .onChange = change})
      )
  );
  ownership->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingV = 2.0F},
          rowTitle("Group", &m_groupId, labelWidth),
          ui::input({.out = &m_group, .placeholder = "Name or numeric ID", .flexGrow = 1.0F, .onChange = change})
      )
  );
  for (auto** out : {&m_reason, &m_message}) {
    direct->addChild(
        ui::label({.out = out, .fontSize = Style::fontSizeCaption, .maxLines = 4, .configure = [](Label& label) {
                     label.setUseMarkup(false);
                   }})
    );
  }
  addChild(std::move(direct));
  sync({});
}
std::uint32_t PermissionEditor::mode() const {
  std::uint32_t value = 0;
  if (m_multiple) {
    return value;
  }
  for (std::size_t i = 0; i < bits.size(); ++i) {
    if (m_bits[i]->checked()) {
      value |= bits[i];
    }
  }
  return value;
}
luciole::PermissionEdit PermissionEditor::edit() const {
  luciole::PermissionEdit edit;
  if (!m_snapshot.info) {
    return edit;
  }
  const auto& info = *m_snapshot.info;
  if (m_multiple && info.canMode) {
    edit.mask = 0;
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < bits.size(); ++i) {
      const auto choice = m_choices[i]->selectedIndex();
      if (choice != 0) {
        edit.mask |= bits[i];
        if (choice == 1) {
          value |= bits[i];
        }
      }
    }
    if (edit.mask != 0) {
      edit.mode = value;
    }
  } else if (!m_multiple && info.canMode && mode() != info.mode) {
    edit.mode = mode();
  }
  if (info.canOwner && m_owner->value() != m_initialOwner) {
    edit.owner = m_owner->value().substr(0, 1025);
  }
  if (info.canGroup && m_group->value() != m_initialGroup) {
    edit.group = m_group->value().substr(0, 1025);
  }
  return edit;
}
bool PermissionEditor::canApply() const {
  if (recursive()) {
    return m_tree->canApply();
  }
  const auto value = edit();
  return !(m_snapshot.recursive && m_snapshot.finished)
      && m_snapshot.phase == luciole::PermissionPhase::Ready
      && (value.mode || value.owner || value.group);
}
void PermissionEditor::changed() {
  if (!m_syncing) {
    if (!m_multiple) {
      m_mode->setText(permissionModeText(mode()));
    }
    m_edited();
  }
}
void PermissionEditor::sync(const luciole::PermissionSnapshot& snapshot) {
  m_syncing = true;
  const bool sameValues = snapshot.info
      && m_snapshot.info
      && snapshot.info->mode == m_snapshot.info->mode
      && snapshot.info->owner == m_snapshot.info->owner
      && snapshot.info->group == m_snapshot.info->group;
  const bool keepDraft = sameValues
      && (snapshot.phase == luciole::PermissionPhase::Saving
          || (snapshot.phase == luciole::PermissionPhase::Ready && snapshot.failed() && !snapshot.applied));
  m_snapshot = snapshot;
  const bool ready = snapshot.phase == luciole::PermissionPhase::Ready
      && snapshot.info.has_value()
      && !(snapshot.recursive && snapshot.finished);
  if (m_tree) {
    m_tree->sync(snapshot);
    m_recursive->setEnabled(ready);
  }
  for (std::size_t i = 0; i < bits.size(); ++i) {
    if (m_multiple) {
      if (!keepDraft) {
        m_choices[i]->setSelectedIndex(0);
      }
      m_choices[i]->setEnabled(ready && snapshot.info->canMode);
      m_current[i]->setText(
          !snapshot.info                             ? "Unknown"
              : snapshot.unknownItems > 0            ? "Unknown"
              : (snapshot.mixedMode & bits[i]) != 0  ? "Mixed"
              : (snapshot.info->mode & bits[i]) != 0 ? "Allowed"
                                                     : "Denied"
      );
      m_current[i]->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    } else {
      if (!keepDraft) {
        m_bits[i]->setCheckedImmediate(snapshot.info && (snapshot.info->mode & bits[i]) != 0);
      }
      m_bits[i]->setEnabled(ready && snapshot.info->canMode);
    }
  }
  m_owner->setEnabled(ready && snapshot.info->canOwner);
  m_group->setEnabled(ready && snapshot.info->canGroup);
  m_initialOwner = !m_multiple && snapshot.info
      ? (snapshot.info->ownerName.empty() ? std::to_string(snapshot.info->owner) : snapshot.info->ownerName)
      : "";
  m_initialGroup = !m_multiple && snapshot.info
      ? (snapshot.info->groupName.empty() ? std::to_string(snapshot.info->group) : snapshot.info->groupName)
      : "";
  if (!keepDraft) {
    m_owner->setValue(m_initialOwner);
    m_group->setValue(m_initialGroup);
  }
  const std::size_t items = snapshot.results ? snapshot.results->size() : 0;
  m_mode->setText(
      m_multiple ? (snapshot.unknownItems > 0 ? std::format("{} items · {} unknown", items, snapshot.unknownItems)
                                              : std::format("{} items", items))
          : snapshot.info ? permissionModeText(mode())
                          : "Unknown"
  );
  if (m_multiple) {
    const auto id = [&](bool mixed, std::uint32_t value, const char* kind) {
      return !snapshot.info                    ? std::string("Unknown")
          : mixed || snapshot.unknownItems > 0 ? std::string("Mixed or unknown")
                                               : std::format("{} {}", kind, value);
    };
    m_ownerId->setText(id(snapshot.mixedOwner, snapshot.info ? snapshot.info->owner : 0, "UID"));
    m_groupId->setText(id(snapshot.mixedGroup, snapshot.info ? snapshot.info->group : 0, "GID"));
    m_owner->setPlaceholder("Keep current owner (name or ID to change)");
    m_group->setPlaceholder("Keep current group (name or ID to change)");
  } else {
    m_ownerId->setText(snapshot.info ? std::format("UID {}", snapshot.info->owner) : "");
    m_groupId->setText(snapshot.info ? std::format("GID {}", snapshot.info->group) : "");
  }
  std::string reason = snapshot.info ? snapshot.info->reason : "";
  if (snapshot.info && snapshot.info->multipleLinks) {
    reason += " Changes affect all hard links to this file.";
  }
  if (snapshot.info && snapshot.info->canGroup && !snapshot.info->canOwner) {
    reason += " Changing the owner requires administrator permissions.";
  }
  if (snapshot.info && (snapshot.info->mode & 06000) != 0) {
    reason += " Ownership changes can clear set-user/group-ID bits.";
  }
  if (snapshot.info
      && snapshot.info->acl
      && snapshot.info->acl->supported
      && std::ranges::any_of(snapshot.info->acl->access, [](const auto& entry) {
           return entry.tag == luciole::AclTag::Mask;
         })) {
    reason += " Group mode bits control the ACL mask. Use ACL to see effective rights.";
  }
  m_reason->setText(reason);
  m_reason->setVisible(!reason.empty());
  m_reason->setParticipatesInLayout(!reason.empty());
  const std::string message = snapshot.phase == luciole::PermissionPhase::Loading ? "Reading permissions…"
      : snapshot.phase == luciole::PermissionPhase::Saving                        ? "Saving changes…"
                                                                                  : permissionStatusText(snapshot);
  m_message->setText(message);
  m_message->setVisible(!m_multiple && !message.empty());
  m_message->setParticipatesInLayout(!m_multiple && !message.empty());
  m_message->setColor(colorSpecFromRole(snapshot.failed() ? ColorRole::Error : ColorRole::OnSurfaceVariant));
  m_reason->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_syncing = false;
}
bool PermissionEditor::recursive() const { return m_recursive && m_recursive->checked(); }
luciole::RecursivePermissionEdit PermissionEditor::recursiveEdit() const { return m_tree->edit(); }
const Node* PermissionEditor::messageNode() const {
  return recursive() ? m_tree->messageNode() : m_multiple ? static_cast<const Node*>(m_group) : m_message;
}
