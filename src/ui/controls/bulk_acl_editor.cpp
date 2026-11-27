#include "ui/controls/bulk_acl_editor.h"

#include "i18n/i18n.h"
#include "ui/builders.h"
#include "ui/controls/card_style.h"
#include "ui/controls/input.h"
#include "ui/controls/permission_editor.h"
#include "ui/controls/segmented.h"

#include <algorithm>
#include <format>

namespace {
  constexpr std::array tags{
      luciole::AclTag::Owner, luciole::AclTag::OwningGroup, luciole::AclTag::Other, luciole::AclTag::Mask
  };
  std::pair<std::uint32_t, std::uint32_t> rights(const std::array<std::size_t, 3>& choices) {
    std::uint32_t value = 0, mask = 0;
    for (std::size_t i = 0; i < choices.size(); ++i) {
      if (choices[i] != 0) {
        mask |= 4U >> i;
        if (choices[i] == 1) {
          value |= 4U >> i;
        }
      }
    }
    return {value, mask};
  }
  std::vector<ui::SegmentedOption> options(std::initializer_list<const char*> labels) {
    std::vector<ui::SegmentedOption> result;
    for (const auto* label : labels) {
      result.push_back({.label = label, .glyph = {}, .tooltip = {}});
    }
    return result;
  }
} // namespace
BulkAclEditor::BulkAclEditor(std::function<void()> changed) : m_changed(std::move(changed)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceMd);
  addChild(
      ui::label(
          {.text = "Keep preserves each item's rules. Changes apply only to the selected items, without visiting "
                   "folder contents.",
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxLines = 3}
      )
  );
  addChild(ui::label({.out = &m_summary, .fontSize = Style::fontSizeCaption, .maxLines = 3}));
  addChild(ui::column({.out = &m_rows, .align = FlexAlign::Stretch, .gap = Style::spaceMd}));
  addChild(
      ui::label({.out = &m_status, .fontSize = Style::fontSizeCaption, .maxLines = 4, .configure = [](Label& label) {
                   label.setUseMarkup(false);
                 }})
  );
}
void BulkAclEditor::changed(bool rebuild) {
  m_rebuild |= rebuild;
  m_status->setText(canApply() ? i18n::text("Unsaved ACL changes. Unrelated rules will be preserved.") : "");
  m_status->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_changed();
}
luciole::AclPatchEdit BulkAclEditor::edit() const {
  luciole::AclPatchEdit edit;
  edit.defaultPolicy = m_defaults;
  for (std::size_t i = 0; i < m_lists.size(); ++i) {
    if (i == 1 && (m_defaults == luciole::AclDefaultPolicy::Keep || m_defaults == luciole::AclDefaultPolicy::Remove)) {
      continue;
    }
    const auto& list = m_lists[i];
    auto& patch = i == 0 ? edit.access : edit.defaults;
    patch.maskPolicy = list.mask;
    for (std::size_t j = 0; j < tags.size(); ++j) {
      if (tags[j] == luciole::AclTag::Mask && list.mask != luciole::AclMaskPolicy::Keep) {
        continue;
      }
      const auto [value, mask] = rights(list.bits[j]);
      if (mask != 0) {
        patch.entries.push_back({tags[j], {}, value, mask});
      }
    }
    for (const auto& rule : list.named) {
      const auto [value, mask] = rights(rule.bits);
      if (rule.remove || mask != 0) {
        patch.entries.push_back({rule.tag, rule.account, rule.remove ? std::nullopt : std::make_optional(value), mask});
      }
    }
  }
  return edit;
}
bool BulkAclEditor::canApply() const {
  const auto value = edit();
  return m_ready
      && (!value.access.entries.empty()
          || value.access.maskPolicy != luciole::AclMaskPolicy::Keep
          || !value.defaults.entries.empty()
          || value.defaults.maskPolicy != luciole::AclMaskPolicy::Keep
          || value.defaultPolicy == luciole::AclDefaultPolicy::Create
          || value.defaultPolicy == luciole::AclDefaultPolicy::Remove);
}
void BulkAclEditor::sync(const luciole::PermissionSnapshot& snapshot) {
  std::size_t editable = 0, inspected = 0;
  if (snapshot.results) {
    for (const auto& row : *snapshot.results) {
      if (row.info && row.info->acl && row.info->acl->supported) {
        ++inspected;
        editable += row.info->acl->editable ? 1 : 0;
      }
    }
  }
  const bool ready =
      snapshot.phase == luciole::PermissionPhase::Ready && editable > 0 && !(snapshot.recursive && snapshot.finished);
  if (snapshot.results != m_results && snapshot.finished && snapshot.aclOperation) {
    m_lists = {};
    m_defaults = luciole::AclDefaultPolicy::Keep;
    m_rebuild = true;
  }
  m_results = snapshot.results;
  if (ready != m_ready) {
    m_ready = ready;
    m_rebuild = true;
  }
  m_summary->setText(
      std::format(
          "{} readable ACLs · {} editable · {} unavailable or unknown", inspected, editable,
          snapshot.results ? snapshot.results->size() - inspected : 0
      )
  );
  m_summary->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_status->setText(snapshot.aclOperation ? permissionStatusText(snapshot) : "");
  m_status->setColor(colorSpecFromRole(snapshot.failed() ? ColorRole::Error : ColorRole::OnSurfaceVariant));
}
const Node* BulkAclEditor::messageNode() const { return m_status; }
void BulkAclEditor::rebuild() {
  while (!m_rows->children().empty()) {
    m_rows->removeChild(m_rows->children().back().get());
  }
  const auto bitRows = [this](
                           Flex& parent, const std::array<std::size_t, 3>& values, bool enabled,
                           const std::function<void(std::size_t, std::size_t)>& change
                       ) {
    constexpr std::array names{"Read", "Write", "Execute"};
    for (std::size_t bit = 0; bit < names.size(); ++bit) {
      parent.addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingV = 2.0F},
              ui::label(
                  {.text = names[bit],
                   .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                   .minWidth = 104,
                   .maxWidth = 104}
              ),
              ui::segmented(
                  {.options = options({"Keep", "Allow", "Deny"}),
                   .selectedIndex = values[bit],
                   .enabled = m_ready && enabled,
                   .equalSegmentWidths = true,
                   .flexGrow = 1,
                   .onChange = [this, change, bit](std::size_t value) {
                     if (m_ready && !m_rebuild) {
                       change(bit, value);
                     }
                   }}
              )
          )
      );
    }
  };
  const auto caption = [](const char* text) {
    return ui::label(
        {.text = text,
         .fontSize = Style::fontSizeCaption,
         .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
         .maxLines = 4}
    );
  };
  // A role's title over its Read/Write/Execute rows; a separator splits it from the previous role.
  const auto section = [](Flex& parent, std::unique_ptr<Node> title) {
    if (!parent.children().empty()) {
      parent.addChild(ui::separator({}));
    }
    parent.addChild(std::move(title));
  };
  for (std::size_t index = 0; index < m_lists.size(); ++index) {
    auto& list = m_lists[index];
    Flex* body = nullptr;
    m_rows->addChild(luciole::card(index == 0 ? "Rules" : "Defaults for New Items in Folders", &body));
    if (index == 1) {
      body->addChild(
          ui::segmented(
              {.options = options({"Keep", "Edit Existing", "Create", "Remove"}),
               .selectedIndex = static_cast<std::size_t>(m_defaults),
               .enabled = m_ready,
               .equalSegmentWidths = true,
               .onChange = [this](std::size_t value) {
                 m_defaults = static_cast<luciole::AclDefaultPolicy>(value);
                 changed(true);
               }}
          )
      );
      body->addChild(caption(
          "Edit existing preserves absent defaults. Create starts from each folder's updated rules when defaults are "
          "absent. Files and existing children are unaffected."
      ));
      if (m_defaults == luciole::AclDefaultPolicy::Keep || m_defaults == luciole::AclDefaultPolicy::Remove) {
        continue;
      }
    }
    Flex* roles = nullptr;
    body->addChild(ui::column({.out = &roles, .align = FlexAlign::Stretch, .gap = Style::spaceXs}));
    section(
        *roles,
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingV = 2.0F},
            ui::label({.text = "Mask", .fontWeight = FontWeight::SemiBold, .minWidth = 104, .maxWidth = 104}),
            ui::segmented(
                {.options = options({"Keep", "Recalculate", "Remove Unused"}),
                 .selectedIndex = static_cast<std::size_t>(list.mask),
                 .enabled = m_ready,
                 .equalSegmentWidths = true,
                 .flexGrow = 1,
                 .onChange = [this, index](std::size_t value) {
                   m_lists[index].mask = static_cast<luciole::AclMaskPolicy>(value);
                   changed(true);
                 }}
            )
        )
    );
    roles->addChild(caption(
        "Keep preserves masks; a newly required mask starts with owning-group rights. Recalculate may widen effective "
        "access. Remove unused requires no named rules and restores owning-group rights."
    ));
    constexpr std::array names{"Owner", "Owning Group", "Others", "Mask Rights"};
    for (std::size_t role = 0; role < tags.size(); ++role) {
      if (tags[role] == luciole::AclTag::Mask && list.mask != luciole::AclMaskPolicy::Keep) {
        continue;
      }
      section(*roles, ui::label({.text = names[role], .fontWeight = FontWeight::SemiBold}));
      bitRows(*roles, list.bits[role], true, [this, index, role](std::size_t bit, std::size_t value) {
        m_lists[index].bits[role][bit] = value;
        changed(false);
      });
    }
    for (std::size_t rule = 0; rule < list.named.size(); ++rule) {
      const auto& value = list.named[rule];
      roles->addChild(ui::separator({}));
      roles->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm},
              ui::label(
                  {.text = value.tag == luciole::AclTag::User ? "User" : "Group",
                   .fontWeight = FontWeight::SemiBold,
                   .minWidth = 104,
                   .maxWidth = 104}
              ),
              ui::input(
                  {.value = value.account,
                   .placeholder = "Name or numeric ID",
                   .enabled = m_ready,
                   .flexGrow = 1,
                   .onChange =
                       [this, index, rule](const std::string& text) {
                         if (!m_ready || m_rebuild) {
                           return; // Old rows retire at the next layout, after a result or structural edit.
                         }
                         m_lists[index].named[rule].account = text.substr(0, 1025);
                         changed(false);
                       }}
              ),
              ui::button(
                  {.glyph = "x",
                   .enabled = m_ready,
                   .variant = ButtonVariant::Ghost,
                   .tooltip = "Discard this rule change",
                   .onClick = [this, index, rule] {
                     if (!m_ready || m_rebuild) {
                       return;
                     }
                     auto& rules = m_lists[index].named;
                     rules.erase(rules.begin() + static_cast<std::ptrdiff_t>(rule));
                     changed(true);
                   }}
              )
          )
      );
      roles->addChild(
          ui::segmented(
              {.options = options({"Set Bits", "Remove Rule"}),
               .selectedIndex = value.remove ? 1U : 0U,
               .enabled = m_ready,
               .equalSegmentWidths = true,
               .onChange = [this, index, rule](std::size_t choice) {
                 if (!m_ready || m_rebuild) {
                   return;
                 }
                 m_lists[index].named[rule].remove = choice == 1;
                 changed(true);
               }}
          )
      );
      bitRows(*roles, value.bits, !value.remove, [this, index, rule](std::size_t bit, std::size_t choice) {
        m_lists[index].named[rule].bits[bit] = choice;
        changed(false);
      });
    }
    auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingV = Style::spaceXs});
    for (const bool user : {true, false}) {
      actions->addChild(
          ui::button(
              {.text = user ? "Add User Rule" : "Add Group Rule",
               .glyph = user ? "user-plus" : "users-plus",
               .controlHeight = Style::controlHeightSm,
               .enabled = m_ready && list.named.size() < luciole::maxAclEntries - tags.size(),
               .variant = ButtonVariant::Default,
               .onClick = [this, index, user] {
                 m_lists[index].named.push_back({user ? luciole::AclTag::User : luciole::AclTag::Group, {}, {}, false});
                 changed(true);
               }}
          )
      );
    }
    body->addChild(std::move(actions));
  }
}
void BulkAclEditor::prepareRows() {
  if (m_rebuild) {
    m_rebuild = false;
    rebuild();
  }
}
LayoutSize BulkAclEditor::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  prepareRows();
  return Flex::doMeasure(renderer, constraints);
}
void BulkAclEditor::doArrange(Renderer& renderer, const LayoutRect& rect) {
  prepareRows();
  Flex::doArrange(renderer, rect);
}
void BulkAclEditor::doLayout(Renderer& renderer) {
  prepareRows();
  Flex::doLayout(renderer);
}
