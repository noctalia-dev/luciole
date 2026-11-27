#include "ui/controls/acl_editor.h"

#include "i18n/i18n.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/card_style.h"
#include "ui/controls/input.h"
#include "ui/controls/permission_editor.h"
#include "ui/controls/toggle.h"

#include <algorithm>
#include <array>
#include <format>

namespace {
  using Tag = luciole::AclTag;
  bool named(Tag tag) { return tag == Tag::User || tag == Tag::Group; }
  std::string permissions(std::uint32_t value) {
    return std::string{(value & 4) != 0 ? 'r' : '-', (value & 2) != 0 ? 'w' : '-', (value & 1) != 0 ? 'x' : '-'};
  }
} // namespace
AclEditor::AclEditor(std::function<void()> changed) : m_changed(std::move(changed)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceMd);
  addChild(
      ui::label(
          {.text = "Rules grant rights to more users and groups. The mask caps the owning group and every named rule; "
                   "rights above it show their effective value.",
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxLines = 3}
      )
  );
  addChild(
      ui::label({.out = &m_reason, .fontSize = Style::fontSizeCaption, .maxLines = 3, .configure = [](Label& label) {
                   label.setUseMarkup(false);
                 }})
  );
  addChild(ui::column({.out = &m_rows, .align = FlexAlign::Stretch, .gap = Style::spaceMd}));
  addChild(
      ui::label({.out = &m_status, .fontSize = Style::fontSizeCaption, .maxLines = 4, .configure = [](Label& label) {
                   label.setUseMarkup(false);
                 }})
  );
}
void AclEditor::changed(bool layout) {
  m_draft = m_actual && (m_access != m_actual->access || m_defaults != m_actual->defaults);
  m_status->setText(m_draft ? i18n::text("Unsaved access control changes.") : "");
  m_status->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_rebuild |= layout;
  if (!layout) {
    updateEffective();
  }
  m_changed();
}
void AclEditor::sync(const luciole::PermissionSnapshot& snapshot) {
  const auto actual = snapshot.info ? snapshot.info->acl : nullptr;
  const bool preserve = m_draft
      && snapshot.aclOperation
      && (snapshot.phase == luciole::PermissionPhase::Saving || (snapshot.error.has_value() && !snapshot.applied));
  const bool editable = snapshot.phase == luciole::PermissionPhase::Ready && actual && actual->editable;
  if (actual != m_actual || editable != m_editable) {
    if (!preserve) {
      m_access = actual ? actual->access : std::vector<luciole::AclEntry>{};
      m_defaults = actual ? actual->defaults : std::vector<luciole::AclEntry>{};
      m_draft = false;
    }
    m_actual = actual;
    m_editable = editable;
    m_rebuild = true;
  }
  m_reason->setText(
      actual ? i18n::text(actual->message)
             : i18n::text("ACL inspection is available for one native regular file or folder.")
  );
  const auto message = snapshot.aclOperation ? permissionStatusText(snapshot) : "";
  m_status->setText(
      snapshot.aclOperation && snapshot.phase == luciole::PermissionPhase::Saving ? "Saving access control…" : message
  );
  m_status->setColor(colorSpecFromRole(snapshot.failed() ? ColorRole::Error : ColorRole::OnSurfaceVariant));
  m_reason->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
}
luciole::AclEdit AclEditor::edit() const {
  luciole::AclEdit value;
  if (m_actual && m_access != m_actual->access) {
    value.access = m_access;
  }
  if (m_actual && m_actual->directory && m_defaults != m_actual->defaults) {
    value.defaults = m_defaults;
  }
  return value;
}
bool AclEditor::canApply() const {
  const auto value = edit();
  return m_editable && (value.access || value.defaults);
}
const Node* AclEditor::messageNode() const { return m_status; }
void AclEditor::updateEffective() {
  for (const auto& [label, key] : m_effective) {
    const auto& rows = key.first ? m_defaults : m_access;
    if (key.second >= rows.size()) {
      continue;
    }
    const auto& row = rows[key.second];
    const auto mask = std::ranges::find(rows, Tag::Mask, &luciole::AclEntry::tag);
    const auto effective = (named(row.tag) || row.tag == Tag::OwningGroup) && mask != rows.end()
        ? row.permissions & mask->permissions
        : row.permissions;
    // Only rights the mask actually cuts are worth a note.
    label->setText(i18n::text("Effective") + " " + permissions(effective));
    label->setVisible(effective != row.permissions);
    label->setParticipatesInLayout(effective != row.permissions);
  }
}
void AclEditor::recalculate(bool defaults) {
  auto& rows = defaults ? m_defaults : m_access;
  std::uint32_t value = 0;
  for (const auto& row : rows) {
    if (named(row.tag) || row.tag == Tag::OwningGroup) {
      value |= row.permissions;
    }
  }
  auto mask = std::ranges::find(rows, Tag::Mask, &luciole::AclEntry::tag);
  if (mask != rows.end()) {
    mask->permissions = value;
  } else if (rows.size() < luciole::maxAclEntries) {
    rows.push_back({Tag::Mask, {}, value});
  }
  changed(true);
}
void AclEditor::rebuild() {
  m_effective.clear();
  while (!m_rows->children().empty()) {
    m_rows->removeChild(m_rows->children().back().get());
  }
  if (!m_actual || !m_actual->supported) {
    return;
  }
  constexpr std::array names{"Owner", "User", "Owning Group", "Group", "Mask", "Others"};
  constexpr float nameWidth = 168.0F, removeWidth = Style::controlHeightSm;
  for (const bool defaults : {false, true}) {
    if (defaults && !m_actual->directory) {
      continue;
    }
    auto& rows = defaults ? m_defaults : m_access;
    const bool mask = std::ranges::any_of(rows, [](const auto& row) { return row.tag == Tag::Mask; });
    std::unique_ptr<Node> removeDefaults;
    if (defaults && !rows.empty()) {
      removeDefaults = ui::button(
          {.text = "Remove Defaults",
           .glyph = "trash",
           .controlHeight = Style::controlHeightSm,
           .enabled = m_editable,
           .variant = ButtonVariant::Ghost,
           .onClick = [this] {
             m_defaults.clear();
             changed(true);
           }}
      );
    }
    Flex* body = nullptr;
    m_rows->addChild(luciole::card(defaults ? "Defaults for New Items" : "Rules", &body, std::move(removeDefaults)));
    if (defaults) {
      body->addChild(
          ui::label(
              {.text = "New files and folders created inside inherit these rules. Existing items keep theirs.",
               .fontSize = Style::fontSizeCaption,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .maxLines = 2}
          )
      );
      if (rows.empty()) {
        body->addChild(
            ui::row(
                {.paddingV = Style::spaceXs},
                ui::button(
                    {.text = "Add Defaults",
                     .glyph = "plus",
                     .controlHeight = Style::controlHeightSm,
                     .enabled = m_editable,
                     .variant = ButtonVariant::Default,
                     .tooltip = "Start from this folder's rules",
                     .onClick = [this] {
                       m_defaults = m_access;
                       changed(true);
                     }}
                )
            )
        );
        continue;
      }
    }
    // Same grid as the Access card: name column, Read/Write/Execute, then the remove column.
    auto header = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm});
    header->addChild(ui::label({.text = "", .minWidth = nameWidth, .maxWidth = nameWidth}));
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
    header->addChild(ui::label({.text = "", .minWidth = removeWidth, .maxWidth = removeWidth}));
    body->addChild(std::move(header));
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto& rule = rows[i];
      auto entry = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingV = 2.0F});
      auto who = ui::column({.align = FlexAlign::Stretch, .gap = 2.0F, .minWidth = nameWidth, .maxWidth = nameWidth});
      if (named(rule.tag)) {
        // A person or people glyph tells user rules from group rules.
        who->addChild(
            ui::row(
                {.align = FlexAlign::Center, .gap = Style::spaceSm},
                ui::glyph(
                    {.glyph = rule.tag == Tag::User ? "user" : "users",
                     .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
                ),
                ui::input(
                    {.value = rule.qualifier,
                     .placeholder = rule.tag == Tag::User ? "User name or ID" : "Group name or ID",
                     .enabled = m_editable,
                     .flexGrow = 1.0F,
                     .onChange = [this, defaults, i](const std::string& value) {
                       (defaults ? m_defaults : m_access)[i].qualifier = value.substr(0, 1025);
                       changed(false);
                     }}
                )
            )
        );
      } else {
        who->addChild(
            ui::label({.text = names[static_cast<std::size_t>(rule.tag)], .fontWeight = FontWeight::SemiBold})
        );
      }
      Label* effective = nullptr;
      who->addChild(
          ui::label(
              {.out = &effective,
               .fontSize = Style::fontSizeCaption,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
          )
      );
      m_effective.push_back({effective, {defaults, i}});
      entry->addChild(std::move(who));
      for (std::size_t bit = 0; bit < 3; ++bit) {
        const auto value = 4U >> bit;
        entry->addChild(
            ui::column(
                {.align = FlexAlign::Center, .flexGrow = 1.0F},
                ui::toggle(
                    {.checkedImmediate = (rule.permissions & value) != 0,
                     .enabled = m_editable,
                     .onChange = [this, defaults, i, value](bool checked) {
                       auto& permissions = (defaults ? m_defaults : m_access)[i].permissions;
                       permissions = checked ? permissions | value : permissions & ~value;
                       changed(false);
                     }}
                )
            )
        );
      }
      if (named(rule.tag) || rule.tag == Tag::Mask) {
        // The mask can go only once no named rule needs it.
        const bool removable =
            named(rule.tag) || std::ranges::none_of(rows, [](const auto& row) { return named(row.tag); });
        entry->addChild(
            ui::button(
                {.glyph = "x",
                 .controlHeight = Style::controlHeightSm,
                 .enabled = m_editable && removable,
                 .variant = ButtonVariant::Ghost,
                 .tooltip = "Remove rule",
                 .minWidth = removeWidth,
                 .maxWidth = removeWidth,
                 .onClick = [this, defaults, i] {
                   auto& values = defaults ? m_defaults : m_access;
                   values.erase(values.begin() + static_cast<std::ptrdiff_t>(i));
                   changed(true);
                 }}
            )
        );
      } else {
        entry->addChild(ui::label({.text = "", .minWidth = removeWidth, .maxWidth = removeWidth}));
      }
      body->addChild(std::move(entry));
    }
    auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingV = Style::spaceXs});
    for (const bool user : {true, false}) {
      actions->addChild(
          ui::button(
              {.text = user ? "Add User" : "Add Group",
               .glyph = user ? "user-plus" : "users-plus",
               .controlHeight = Style::controlHeightSm,
               .enabled = m_editable && rows.size() + (mask ? 1 : 2) <= luciole::maxAclEntries,
               .variant = ButtonVariant::Default,
               .onClick = [this, defaults, user] {
                 auto& values = defaults ? m_defaults : m_access;
                 if (std::ranges::none_of(values, [](const auto& row) { return row.tag == Tag::Mask; })) {
                   const auto group = std::ranges::find(values, Tag::OwningGroup, &luciole::AclEntry::tag);
                   values.push_back({Tag::Mask, {}, group == values.end() ? 0 : group->permissions});
                 }
                 values.push_back({user ? Tag::User : Tag::Group, {}, 0});
                 changed(true);
               }}
          )
      );
    }
    actions->addChild(ui::spacer());
    actions->addChild(
        ui::button(
            {.text = "Recalculate Mask",
             .glyph = "calculator",
             .controlHeight = Style::controlHeightSm,
             .enabled = m_editable && (mask || rows.size() < luciole::maxAclEntries),
             .variant = ButtonVariant::Ghost,
             .tooltip = "Set the mask to the union of the owning group and named rules",
             .onClick = [this, defaults] { recalculate(defaults); }}
        )
    );
    body->addChild(std::move(actions));
  }
  updateEffective();
}
void AclEditor::doLayout(Renderer& renderer) {
  prepareRows();
  Flex::doLayout(renderer);
}
void AclEditor::prepareRows() {
  if (m_rebuild) {
    m_rebuild = false;
    rebuild();
  }
}
LayoutSize AclEditor::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  prepareRows();
  return Flex::doMeasure(renderer, constraints);
}
void AclEditor::doArrange(Renderer& renderer, const LayoutRect& rect) {
  prepareRows();
  Flex::doArrange(renderer, rect);
}
