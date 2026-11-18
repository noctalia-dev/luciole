#include "app/page/properties_controller.h"

#include "app/operation_hub.h"
#include "app/page/active_popup.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "browser/location_presentation.h"
#include "clipboard/text_clipboard.h"
#include "config/settings.h"
#include "core/ui_phase.h"
#include "fs/file_query.h"
#include "fs/gio_ptr.h"
#include "operations/permission_service.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "ui/browser/file_icon.h"
#include "ui/builders.h"
#include "ui/controls/acl_editor.h"
#include "ui/controls/bulk_acl_editor.h"
#include "ui/controls/card_style.h"
#include "ui/controls/file_metadata_view.h"
#include "ui/controls/folder_totals_view.h"
#include "ui/controls/permission_editor.h"
#include "ui/controls/permission_results_view.h"
#include "ui/controls/scroll_view.h"
#include "ui/dialogs/dialog_chrome.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  // Equal tabs wide enough for "Permissions"; the strip is centered rather than spanning the dialog.
  constexpr float kTabWidth = 128.0F;
  // Text width inside a General card: dialog padding, scrollbar gutter, then the card's own padding.
  float generalCardWidth(float dialogWidth) {
    return std::max(1.0F, dialogWidth - Style::spaceMd * 4 - Style::scrollbarWidth - Style::scrollbarGap);
  }

  // Names the targets above the tabs, so every tab shows what it applies to; `out` receives the icon.
  std::unique_ptr<Flex> identityRow(
      const std::vector<luciole::DirectoryEntry>& targets, luciole::FileIconCache& icons, float width,
      luciole::FileIcon** out
  ) {
    const auto isFolder = [](const luciole::DirectoryEntry& entry) {
      return entry.type == luciole::FileType::Directory;
    };
    auto icon = std::make_unique<luciole::FileIcon>(icons, 48.0F);
    *out = icon.get();
    std::string title;
    std::string caption;
    if (targets.size() == 1) {
      const auto& entry = targets.front();
      icon->bind(luciole::iconSource(entry.icon), colorSpecFromRole(ColorRole::OnSurface));
      title = entry.displayName();
      caption = luciole::fileTypeDescription(entry.type, entry.symlink, entry.contentType);
    } else {
      const auto folders = static_cast<std::size_t>(std::ranges::count_if(targets, isFolder));
      const auto files = targets.size() - folders;
      const auto count = [](std::size_t n, std::string_view one, std::string_view many) {
        return std::format("{} {}", n, n == 1 ? one : many);
      };
      icon->bind(
          {.names = {files == 0 ? "folder" : "text-x-generic"}, .filePath = {}, .desktopFile = {}},
          colorSpecFromRole(ColorRole::OnSurface)
      );
      title = std::format("{} items", targets.size());
      caption = folders == 0 ? count(files, "file", "files")
          : files == 0       ? count(folders, "folder", "folders")
                             : count(files, "file", "files") + ", " + count(folders, "folder", "folders");
    }
    return ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceMd}, std::move(icon),
        ui::column(
            {.align = FlexAlign::Start, .gap = Style::spaceXs, .flexGrow = 1.0F},
            ui::label(
                {.text = std::move(title),
                 .fontWeight = FontWeight::SemiBold,
                 .maxWidth = width - 104.0F,
                 .maxLines = 2,
                 .configure = [](Label& label) { label.setUseMarkup(false); }}
            ),
            ui::label(
                {.text = std::move(caption),
                 .fontSize = Style::fontSizeCaption,
                 .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
            )
        )
    );
  }
} // namespace

void PropertiesController::showDetails(bool shown) {
  if (!m_permissionResults || shown == m_detailsShown) {
    return;
  }
  m_detailsShown = shown;
  // The per-item list takes the body's place; the footer keeps the summary, Apply and Close.
  m_permissionResults->setVisible(shown);
  m_permissionResults->setParticipatesInLayout(shown);
  m_permissionScroll->setVisible(!shown);
  m_permissionScroll->setParticipatesInLayout(!shown);
  m_permissionDetails->setText(shown ? "Back" : "Details…");
  m_permissionDetails->setGlyph(shown ? "chevron-left" : "");
  m_permissionReport->setVisible(shown);
  m_permissionReport->setParticipatesInLayout(shown);
  m_page.popupSurface()->requestLayout();
}

PropertiesController::PropertiesController(PageContext& page, luciole::FileIconCache& icons)
    : m_page(page), m_icons(icons) {
  m_selectionMetadata.setChangedCallback([this] {
    m_metadataDirty = true;
    m_page.requestSync();
  });
  m_metadata.setChangedCallback([this] {
    m_metadataDirty = true;
    m_page.requestSync();
  });
}

void PropertiesController::progressTick() { m_dirty = m_job && m_job->running(); }

void PropertiesController::changed(OperationJob& job) {
  if (&job == m_job.get()) {
    m_dirty = true;
    return;
  }
  // A job whose dialog closed: report its outcome once.
  const auto& permissions = job.service<luciole::PermissionService>();
  const auto& snapshot = permissions.snapshot();
  if (permissions.busy() || !job.takeResult()) {
    return;
  }
  if ((snapshot.phase == luciole::PermissionPhase::Ready || snapshot.phase == luciole::PermissionPhase::Cancelled)
      && snapshot.applied) {
    m_page.browser().refresh();
  }
  if (const auto status = permissionStatusText(snapshot); !status.empty()) {
    m_page.actionError(status);
  }
}

void PropertiesController::onFrame() {
  if (std::exchange(m_dirty, false)) {
    sync();
  }
  if (std::exchange(m_metadataDirty, false)) {
    syncMetadata();
  }
}

void PropertiesController::syncMetadata() {
  if (!m_metadataView || !m_page.popupSurface()) {
    return;
  }
  auto fields = m_multiple ? luciole::MetadataFields{} : m_metadata.fields();
  if (m_multiple) {
    for (const auto& [label, value] : m_selectionMetadata.fields()) {
      fields.push_back({label, value, false});
    }
  }
  if (m_job) {
    const auto& snapshot = m_job->service<luciole::PermissionService>().snapshot();
    if (snapshot.info && snapshot.info->current) {
      const auto& info = *snapshot.info;
      const auto replace = [&](std::string label, std::string value) {
        const auto found = std::ranges::find(fields, label, &luciole::MetadataField::label);
        if (found != fields.end()) {
          found->value = std::move(value);
        } else {
          fields.push_back({std::move(label), std::move(value), false});
        }
      };
      if (!snapshot.multiple || !m_selectionMetadata.busy()) {
        const char* const unknown = snapshot.unknownItems ? " (some unknown)" : "";
        replace(
            "Owner",
            (snapshot.mixedOwner          ? "Mixed"
                 : info.ownerName.empty() ? std::to_string(info.owner)
                                          : info.ownerName)
                + std::string(unknown)
        );
        replace(
            "Group",
            (snapshot.mixedGroup          ? "Mixed"
                 : info.groupName.empty() ? std::to_string(info.group)
                                          : info.groupName)
                + std::string(unknown)
        );
        replace(
            "Permissions",
            (snapshot.mixedMode ? "Mixed" : std::format("{:04o}", info.mode & 07777U)) + std::string(unknown)
        );
      }
    }
  }
  if (m_folderTotals) {
    std::erase_if(fields, [](const auto& field) { return field.label == "Folder contents"; });
    m_folderTotals->sync(m_metadataWidth);
  }
  // The entry's own facts lead, so both kinds read as one table with one row rhythm.
  fields.insert(fields.begin(), m_basicFields.begin(), m_basicFields.end());
  m_metadataView->sync(fields, m_metadataWidth);
  m_page.popupSurface()->requestLayout();
}

void PropertiesController::updateApply() {
  if (m_permissionEditor) {
    m_permissionSave->setEnabled(canApply() && !m_page.operationsBusy());
  }
}

bool PropertiesController::resultsKey(const KeyboardEvent& event) {
  if (m_permissionResults
      && m_permissionResults->visible()
      && event.pressed
      && m_permissionResults->navigate(event.sym, event.modifiers, event.preedit)) {
    m_page.popupSurface()->requestLayout();
    return true;
  }
  return false;
}

void PropertiesController::layoutDialog(float width) {
  const float metadataWidth = generalCardWidth(width);
  if (m_metadataView && metadataWidth != m_metadataWidth) {
    m_metadataWidth = metadataWidth;
    syncMetadata();
  }
}

void PropertiesController::showAdvanced(bool advanced, bool focus) {
  if (!m_permissionBasic || !m_permissionAdvanced) {
    return;
  }
  m_aclActive = advanced;
  m_permissionBasic->setVisible(!advanced);
  m_permissionBasic->setParticipatesInLayout(!advanced);
  m_permissionAdvanced->setVisible(advanced);
  m_permissionAdvanced->setParticipatesInLayout(advanced);
  m_permissionSave->setEnabled(canApply() && !m_page.operationsBusy());
  m_permissionScroll->requestScrollToOffset(0);
  if (focus) {
    m_page.popupDispatcher().setFocus((advanced ? m_permissionAdvancedBack : m_permissionAdvancedOpen)->inputArea());
  }
  m_page.popupSurface()->requestLayout();
}

bool PropertiesController::canApply() const {
  if (m_aclActive) {
    return m_aclEditor ? m_aclEditor->canApply() : m_bulkAclEditor && m_bulkAclEditor->canApply();
  }
  return m_permissionEditor && m_permissionEditor->canApply();
}

void PropertiesController::sync() {
  if (!m_job) {
    return;
  }
  const auto& permissions = m_job->service<luciole::PermissionService>();
  const auto& snapshot = permissions.snapshot();
  m_metadataDirty = true;
  if (m_permissionEditor) {
    m_permissionEditor->sync(snapshot);
    if (m_aclEditor) {
      m_aclEditor->sync(snapshot);
    }
    if (m_bulkAclEditor) {
      m_bulkAclEditor->sync(snapshot);
    }
    m_permissionSave->setEnabled(canApply() && !m_page.operationsBusy());
    m_permissionSave->setText(snapshot.phase == luciole::PermissionPhase::Saving ? "Saving…" : "Apply");
    if (m_permissionResults) {
      m_permissionResults->sync(snapshot.results);
      const bool saving = snapshot.phase == luciole::PermissionPhase::Saving;
      m_permissionStatus->setText(
          permissions.busy()
              ? (snapshot.cancelRequested ? "Cancelling…"
                                          : std::format(
                                                "{} · {} items completed",
                                                permissions.collecting() ? "Collecting targets (total unknown)"
                                                    : saving             ? "Applying changes"
                                                                         : "Reading selection",
                                                permissions.completed()
                                            ))
              : permissionStatusText(snapshot)
      );
      if (permissions.collecting() && !snapshot.cancelRequested) {
        m_permissionStatus->setText(
            std::format("Collecting targets… {} found · total unknown", permissions.completed())
        );
      }
      if (snapshot.recursive && permissions.busy() && !permissions.collecting() && !snapshot.cancelRequested) {
        m_permissionStatus->setText(
            std::format("Applying changes · {} of {} collected items", permissions.completed(), permissions.total())
        );
      }
      m_permissionStatus->setColor(
          colorSpecFromRole(snapshot.failed() ? ColorRole::Error : ColorRole::OnSurfaceVariant)
      );
      // Clipboard access needs a recent input serial, which the click itself provides; checking it here left the
      // button greyed out whenever the job finished between inputs.
      m_permissionReport->setEnabled(!permissions.busy() && !snapshot.report.empty());
      m_permissionCancel->setVisible(saving);
      m_permissionCancel->setParticipatesInLayout(saving);
      m_permissionCancel->setEnabled(!snapshot.cancelRequested);
      // Reading a multi-selection fills one Ready row per item; that is the selection, not a result. Details
      // become available once an item was changed, failed, or skipped.
      const bool outcomes = snapshot.results && std::ranges::any_of(*snapshot.results, [](const auto& row) {
                              return row.outcome != luciole::PermissionOutcome::Ready;
                            });
      m_permissionDetails->setVisible(outcomes);
      m_permissionDetails->setParticipatesInLayout(outcomes);
      if (!outcomes) {
        showDetails(false);
      }
    }
    if (snapshot.failed() && !snapshot.finished && snapshot.phase == luciole::PermissionPhase::Ready) {
      m_permissionTabs->setSelectedIndex(1);
      showAdvanced(snapshot.aclOperation);
      m_permissionScroll->requestScrollIntoView(
          snapshot.aclOperation ? (m_aclEditor ? m_aclEditor->messageNode() : m_bulkAclEditor->messageNode())
                                : m_permissionEditor->messageNode()
      );
    }
    m_page.popupSurface()->requestLayout();
  }
  if ((snapshot.phase == luciole::PermissionPhase::Ready || snapshot.phase == luciole::PermissionPhase::Cancelled)
      && snapshot.applied) {
    m_page.browser().refresh();
  }
  if (const auto status = permissionStatusText(snapshot);
      !permissions.busy() && !status.empty() && (!m_permissionEditor || snapshot.applied)) {
    m_page.actionError(status);
  }
}

struct PropertiesController::Body {
  std::unique_ptr<Node> node;
  ScrollView* scroll = nullptr;
  Flex* general = nullptr;
  FileMetadataView* metadata = nullptr;
  FolderTotalsView* folderTotals = nullptr;
  Flex* permissions = nullptr;
  Flex* basic = nullptr;
  Flex* advanced = nullptr;
  Button* advancedOpen = nullptr;
  Button* advancedBack = nullptr;
  PermissionEditor* editor = nullptr;
  AclEditor* acl = nullptr;
  BulkAclEditor* bulkAcl = nullptr;
  PermissionResultsView* results = nullptr;
  // The entry's own facts, shown ahead of the loaded metadata in the Details card.
  luciole::MetadataFields fields;
};

PropertiesController::Body
PropertiesController::buildGeneral(const std::vector<luciole::DirectoryEntry>& targets, bool editable) {
  auto general = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceMd});
  auto* generalGroup = general.get();
  luciole::MetadataFields fields;
  const auto field = [&](std::string name, std::string value) {
    fields.push_back({.label = std::move(name), .value = std::move(value), .preview = false});
  };
  if (targets.size() == 1) {
    const auto& entry = targets.front();
    const auto location = entry.location();
    field("Location", luciole::locationEditText(location));
    if (entry.contentType) {
      field("MIME", *entry.contentType);
    }
    if (location.isTrash()) {
      const auto& trash = entry.trashInfo();
      field("Original", trash.original ? luciole::locationEditText(*trash.original) : "Unknown");
      field("Deleted", trash.deletionDate.value_or("Unknown"));
    }
    if (entry.type != luciole::FileType::Directory) {
      const luciole::GString size(entry.size ? g_format_size_full(*entry.size, G_FORMAT_SIZE_IEC_UNITS) : nullptr);
      field("Size", size ? size.get() : "Unknown");
    }
    if (entry.modifiedSeconds && *entry.modifiedSeconds <= static_cast<std::uint64_t>(G_MAXINT64)) {
      const std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> date(
          g_date_time_new_from_unix_local(static_cast<gint64>(*entry.modifiedSeconds)), &g_date_time_unref
      );
      if (date) {
        const luciole::GString text(g_date_time_format(date.get(), "%Y-%m-%d %H:%M:%S"));
        field("Modified", text.get());
      }
    }
  } else if (!editable) {
    field("Permissions", "Select at most 4096 items for permission editing.");
  }
  Flex* details = nullptr;
  general->addChild(luciole::card("Details", &details));
  auto metadata = std::make_unique<FileMetadataView>();
  auto* metadataView = metadata.get();
  details->addChild(std::move(metadata));
  FolderTotalsView* totals = nullptr;
  if (std::ranges::any_of(targets, [](const auto& entry) {
        return entry.type == luciole::FileType::Directory && !entry.symlink;
      })) {
    auto view = std::make_unique<FolderTotalsView>(targets, [this] {
      m_metadataDirty = true;
      m_page.requestSync();
    });
    totals = view.get();
    // FolderTotalsView brings its own "Contents" heading beside the Stop button.
    Flex* contents = nullptr;
    general->addChild(luciole::card({}, &contents));
    contents->addChild(std::move(view));
  }
  Body result;
  result.general = generalGroup;
  result.metadata = metadataView;
  result.folderTotals = totals;
  result.fields = std::move(fields);
  result.node = std::move(general);
  return result;
}

PropertiesController::Body PropertiesController::buildBody(
    const std::vector<luciole::DirectoryEntry>& targets, bool editable, bool multiple, bool folders, bool withResults
) {
  ScrollView* scroll = nullptr;
  auto body = ui::scrollView(
      {.out = &scroll, .viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F, .fillWidth = true, .flexGrow = 1.0F}
  );
  body->content()->setAlign(FlexAlign::Stretch);
  body->content()->setGap(Style::spaceMd);
  auto general = buildGeneral(targets, editable);
  body->content()->addChild(std::move(general.node));
  PermissionEditor* editor = nullptr;
  AclEditor* acl = nullptr;
  BulkAclEditor* bulkAcl = nullptr;
  Flex *permissionsGroup = nullptr, *basicGroup = nullptr, *advancedGroup = nullptr;
  Button *advancedOpen = nullptr, *advancedBack = nullptr;
  PermissionResultsView* results = nullptr;
  std::unique_ptr<PermissionResultsView> resultView;
  if (editable) {
    auto permissions = std::make_unique<PermissionEditor>(
        [this] {
          if (m_permissionEditor && m_permissionSave) {
            m_permissionSave->setEnabled(canApply() && !m_page.operationsBusy());
          }
          m_page.popupSurface()->requestLayout();
        },
        multiple, folders
    );
    editor = permissions.get();
    Flex* advancedEntry = nullptr;
    auto advancedCard = luciole::card({}, &advancedEntry);
    advancedEntry->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingV = 2.0F},
            ui::column(
                {.align = FlexAlign::Stretch, .gap = Style::spaceXs, .flexGrow = 1.0F},
                ui::label({.text = "Access control list", .fontWeight = FontWeight::SemiBold}),
                ui::label(
                    {.text = "Grant rights to more users and groups.",
                     .fontSize = Style::fontSizeCaption,
                     .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
                )
            ),
            ui::button(
                {.out = &advancedOpen,
                 .text = "Advanced permissions…",
                 .controlHeight = Style::controlHeightSm,
                 .variant = ButtonVariant::Default,
                 .onClick = [this] { showAdvanced(true, true); }}
            )
        )
    );
    // The editor's cards come first; ACLs are the less common path, so they close the page.
    auto basic = ui::column(
        {.out = &basicGroup, .align = FlexAlign::Stretch, .gap = Style::spaceMd}, std::move(permissions),
        std::move(advancedCard)
    );
    const auto aclChanged = [this] {
      if (m_permissionSave) {
        m_permissionSave->setEnabled(canApply() && !m_page.operationsBusy());
      }
      m_page.popupSurface()->requestLayout();
    };
    std::unique_ptr<Flex> aclControl;
    if (multiple) {
      auto control = std::make_unique<BulkAclEditor>(aclChanged);
      bulkAcl = control.get();
      aclControl = std::move(control);
    } else {
      auto control = std::make_unique<AclEditor>(aclChanged);
      acl = control.get();
      aclControl = std::move(control);
    }
    auto advanced = ui::column(
        {.out = &advancedGroup,
         .align = FlexAlign::Stretch,
         .gap = Style::spaceMd,
         .visible = false,
         .participatesInLayout = false},
        // Page header: back arrow beside the page title, like a sub-page.
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm},
            ui::button(
                {.out = &advancedBack,
                 .glyph = "chevron-left",
                 .controlHeight = Style::controlHeightSm,
                 .variant = ButtonVariant::Ghost,
                 .tooltip = "Back to permissions",
                 .minWidth = Style::controlHeightSm,
                 .maxWidth = Style::controlHeightSm,
                 .onClick = [this] { showAdvanced(false, true); }}
            ),
            ui::label({.text = "Access control list", .fontSize = Style::fontSizeTitle, .fontWeight = FontWeight::Bold})
        ),
        std::move(aclControl)
    );
    body->content()->addChild(
        ui::column(
            {.out = &permissionsGroup, .align = FlexAlign::Stretch, .visible = false, .participatesInLayout = false},
            std::move(basic), std::move(advanced)
        )
    );
    if (withResults) {
      resultView = std::make_unique<PermissionResultsView>();
      results = resultView.get();
      resultView->setVisible(false);
      resultView->setParticipatesInLayout(false);
    }
  }
  std::unique_ptr<Node> propertiesBody;
  if (resultView) {
    propertiesBody =
        ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F}, std::move(body), std::move(resultView));
  } else {
    propertiesBody = std::move(body);
  }
  return {
      .node = std::move(propertiesBody),
      .scroll = scroll,
      .general = general.general,
      .metadata = general.metadata,
      .folderTotals = general.folderTotals,
      .permissions = permissionsGroup,
      .basic = basicGroup,
      .advanced = advancedGroup,
      .advancedOpen = advancedOpen,
      .advancedBack = advancedBack,
      .editor = editor,
      .acl = acl,
      .bulkAcl = bulkAcl,
      .results = results,
      .fields = std::move(general.fields),
  };
}

void PropertiesController::show(const std::vector<luciole::DirectoryEntry>& targets, bool captureCurrentLocation) {
  if (targets.empty()) {
    return;
  }
  if (m_page.jobs().newest([](const OperationJob& job) {
        return job.kind() == JobKind::Permission && job.running();
      })) {
    m_page.actionError("A Properties request is still finishing.");
    return;
  }
  const bool editable = !targets.empty() && targets.size() <= 4096;
  const bool multiple = targets.size() > 1;
  const bool folders = std::ranges::any_of(targets, [](const auto& entry) {
    if (entry.type != luciole::FileType::Directory || entry.symlink) {
      return false;
    }
    const auto location = entry.location();
    return location.nativePath() && !location.isTrash();
  });
  const bool withResults = multiple || folders;
  const float width = m_page.dialogWidth(DialogWidth::Medium);
  auto body = buildBody(targets, editable, multiple, folders, withResults);
  auto* scroll = body.scroll;
  auto* generalGroup = body.general;
  auto* permissionsGroup = body.permissions;
  auto* results = body.results;
  const auto literal = [](Label& label) { label.setUseMarkup(false); };
  Segmented* tabs = nullptr;
  Button *save = nullptr, *reportButton = nullptr, *cancelButton = nullptr, *detailsButton = nullptr;
  Label* progress = nullptr;
  const auto onTabChange = [this, generalGroup, permissionsGroup, scroll](std::size_t index) {
    showDetails(false);
    generalGroup->setVisible(index == 0);
    generalGroup->setParticipatesInLayout(index == 0);
    if (permissionsGroup) {
      permissionsGroup->setVisible(index == 1);
      permissionsGroup->setParticipatesInLayout(index == 1);
    }
    showAdvanced(false);
    if (m_permissionSave) {
      // Only the Permissions tab has anything to apply.
      m_permissionSave->setVisible(index == 1);
      m_permissionSave->setParticipatesInLayout(index == 1);
      m_permissionSave->setEnabled(canApply() && !m_page.operationsBusy());
    }
    scroll->requestScrollToOffset(0);
    m_page.popupSurface()->requestLayout();
  };
  luciole::FileIcon* identity = nullptr;
  auto root = ui::column(
      {.align = FlexAlign::Stretch},
      luciole::dialogHeader("Properties", "file-info", [this] { m_page.dismissPopup(); }),
      identityRow(targets, m_icons, width, &identity),
      ui::row(
          {.justify = FlexJustify::Center, .visible = editable, .participatesInLayout = editable},
          ui::segmented(
              {.out = &tabs,
               .options =
                   std::vector<ui::SegmentedOption>{
                       {.label = "General", .glyph = {}, .tooltip = {}},
                       {.label = "Permissions", .glyph = {}, .tooltip = {}}
                   },
               .equalSegmentWidths = true,
               .width = kTabWidth * 2.0F,
               .onChange = onTabChange}
          )
      ),
      std::move(body.node)
  );
  luciole::applyDialogStyle(*root);
  if (editable) {
    if (withResults) {
      root->addChild(
          ui::label({.out = &progress, .fontSize = Style::fontSizeCaption, .maxLines = 2, .configure = literal})
      );
    }
    root->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm},
            // Per-item outcomes of the last bulk or recursive job; shown only once there are some.
            ui::button(
                {.out = &detailsButton,
                 .text = "Details…",
                 .variant = ButtonVariant::Ghost,
                 .visible = false,
                 .participatesInLayout = false,
                 .onClick = [this] { showDetails(!m_detailsShown); }}
            ),
            ui::button(
                {.out = &reportButton,
                 .text = "Copy report",
                 .enabled = false,
                 .variant = ButtonVariant::Ghost,
                 .visible = false,
                 .participatesInLayout = false,
                 .onClick =
                     [this] {
                       if (!m_job) {
                         return;
                       }
                       if (!m_page.clipboard().available()) {
                         m_page.actionError("The clipboard is not available.");
                         return;
                       }
                       m_page.clipboard().setClipboardText(
                           m_job->service<luciole::PermissionService>().snapshot().report
                       );
                     }}
            ),
            ui::spacer(),
            ui::button(
                {.out = &cancelButton,
                 .text = "Cancel",
                 .variant = ButtonVariant::Ghost,
                 .visible = false,
                 .participatesInLayout = false,
                 .onClick =
                     [this] {
                       if (m_job) {
                         m_job->cancel();
                       }
                     }}
            ),
            ui::button(
                {.text = "Close", .variant = ButtonVariant::Ghost, .onClick = [this] { m_page.dismissPopup(); }}
            ),
            ui::button(
                {.out = &save,
                 .text = "Apply",
                 .enabled = false,
                 .variant = ButtonVariant::Primary,
                 .visible = false,
                 .participatesInLayout = false,
                 .onClick = [this] {
                   if (m_permissionEditor && m_job && !m_page.operationsBusy()) {
                     auto& permissions = m_job->service<luciole::PermissionService>();
                     const bool started = m_aclActive && m_bulkAclEditor
                         ? permissions.applyAclPatch(m_bulkAclEditor->edit())
                         : m_aclActive && m_aclEditor ? permissions.applyAcl(m_aclEditor->edit())
                         : m_permissionEditor->recursive()
                         ? permissions.applyRecursive(m_permissionEditor->recursiveEdit())
                         : permissions.apply(m_permissionEditor->edit());
                     if (started) {
                       m_page.jobs().follow(*m_job);
                     }
                   }
                 }}
            )
        )
    );
  }
  m_page.showDialog(std::move(root), DialogWidth::Medium);
  if (m_page.popupSurface()) {
    m_page.activePopup().bind(ActivePopup::Kind::Properties, [this] { close(); });
    m_identityIcon = identity;
    // Entries without an icon, such as Places rows, get it from GIO.
    if (!multiple && !targets.front().icon) {
      m_iconQuery = luciole::CancelToken{};
      luciole::queryFileIcon(targets.front().location(), m_iconQuery, [this, identity](auto reply) {
        if (reply && identity == m_identityIcon) {
          identity->bind(*reply, colorSpecFromRole(ColorRole::OnSurface));
          m_page.popupSurface()->requestLayout();
        }
      });
    }
    // Start on the tabs rather than the header's close button.
    if (editable) {
      m_page.popupDispatcher().setFocus(m_page.popupDispatcher().firstTabFocusUnder(tabs));
    }
    m_metadataView = body.metadata;
    m_folderTotals = body.folderTotals;
    m_basicFields = std::move(body.fields);
    m_metadataWidth = generalCardWidth(width);
    m_multiple = multiple;
    if (m_metadataView) {
      if (multiple) {
        m_selectionMetadata.show(targets);
      } else {
        m_metadata.show(targets.front(), m_page.settings().thumbnails);
      }
      m_metadataDirty = true;
    }
    if (!editable) {
      return;
    }
    m_permissionEditor = body.editor;
    m_permissionBasic = body.basic;
    m_permissionAdvanced = body.advanced;
    m_permissionAdvancedOpen = body.advancedOpen;
    m_permissionAdvancedBack = body.advancedBack;
    m_aclEditor = body.acl;
    m_bulkAclEditor = body.bulkAcl;
    m_aclActive = false;
    m_permissionResults = results;
    m_permissionReport = reportButton;
    m_permissionDetails = detailsButton;
    m_permissionCancel = cancelButton;
    m_permissionStatus = progress;
    m_permissionTabs = tabs;
    m_permissionSave = save;
    m_permissionScroll = scroll;
    m_page.popupDispatcher().setFocusChangeCallback([this](InputArea*, InputArea* focused) {
      if (m_permissionScroll) {
        m_permissionScroll->requestScrollIntoView(focused);
        m_page.popupSurface()->requestLayout();
      }
    });
    m_job = m_page.jobs().create<luciole::PermissionService>(JobScope::Page);
    auto& permissions = m_job->service<luciole::PermissionService>();
    if (captureCurrentLocation) {
      (void)permissions.prepare(targets.front().location());
    } else if (multiple) {
      (void)permissions.prepare(targets);
    } else {
      (void)permissions.prepare(targets.front());
    }
  }
}

void PropertiesController::close() {
  m_metadata.clear();
  m_selectionMetadata.clear();
  m_iconQuery.cancel();
  m_identityIcon = nullptr;
  m_multiple = false;
  m_metadataView = nullptr;
  m_folderTotals = nullptr;
  m_basicFields.clear();
  m_metadataDirty = false;
  // Saving goes on for the page that takes the job over once this page left its window.
  if (const auto job = std::exchange(m_job, nullptr); job && (m_page.jobs().attached() || !job->running())) {
    job->cancel();
  }
  m_permissionEditor = nullptr;
  m_permissionBasic = m_permissionAdvanced = nullptr;
  m_permissionAdvancedOpen = m_permissionAdvancedBack = nullptr;
  m_aclEditor = nullptr;
  m_bulkAclEditor = nullptr;
  m_aclActive = false;
  m_permissionResults = nullptr;
  m_permissionReport = m_permissionCancel = m_permissionDetails = nullptr;
  m_detailsShown = false;
  m_permissionStatus = nullptr;
  m_permissionTabs = nullptr;
  m_permissionSave = nullptr;
  m_permissionScroll = nullptr;
  m_page.focusFileList();
}
