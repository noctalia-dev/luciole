#include "app/page/properties_controller.h"

#include "app/operation_hub.h"
#include "app/page/active_dialog.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "browser/location_presentation.h"
#include "clipboard/text_clipboard.h"
#include "config/settings.h"
#include "core/ui_phase.h"
#include "fs/gio_ptr.h"
#include "operations/permission_service.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "ui/browser/file_icon.h"
#include "ui/builders.h"
#include "ui/controls/acl_editor.h"
#include "ui/controls/bulk_acl_editor.h"
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
#include <utility>
#include <vector>

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
  if (!m_metadataView || !m_page.menuSurface()) {
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
  m_metadataView->sync(fields, m_metadataWidth);
  m_page.menuSurface()->requestLayout();
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
    m_page.menuSurface()->requestLayout();
    return true;
  }
  return false;
}

void PropertiesController::layoutDialog(float width) {
  const float metadataWidth = std::max(1.0F, width - Style::spaceMd * 2 - Style::scrollbarWidth - Style::scrollbarGap);
  if (m_metadataView && metadataWidth != m_metadataWidth) {
    m_metadataWidth = metadataWidth;
    syncMetadata();
  }
  if (m_permissionEditor || m_metadataView) {
    m_page.activeDialog().root()->setSize(
        width, std::min(600.0F, static_cast<float>(m_page.surface().height()) - 64.0F)
    );
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
    m_page.menuDispatcher().setFocus((advanced ? m_permissionAdvancedBack : m_permissionAdvancedOpen)->inputArea());
  }
  m_page.menuSurface()->requestLayout();
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
      m_permissionReport->setEnabled(!permissions.busy() && !snapshot.report.empty() && m_page.clipboard().available());
      m_permissionCancel->setVisible(saving);
      m_permissionCancel->setParticipatesInLayout(saving);
      m_permissionCancel->setEnabled(!snapshot.cancelRequested);
      if (snapshot.finished) {
        m_permissionTabs->setSelectedIndex(m_permissionResultTab);
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
    m_page.menuSurface()->requestLayout();
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
};

PropertiesController::Body
PropertiesController::buildGeneral(const std::vector<luciole::DirectoryEntry>& targets, float width, bool editable) {
  auto general = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceMd});
  auto* generalGroup = general.get();
  const auto literal = [](Label& label) { label.setUseMarkup(false); };
  const auto field = [&](const std::string& name, const std::string& value) {
    general->addChild(
        ui::row(
            {.align = FlexAlign::Start, .gap = Style::spaceMd},
            ui::label(
                {.text = name,
                 .fontSize = Style::fontSizeCaption,
                 .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                 .minWidth = 76.0F,
                 .maxWidth = 76.0F}
            ),
            ui::label(
                {.text = value, .maxWidth = width - 132.0F, .maxLines = 3, .flexGrow = 1.0F, .configure = literal}
            )
        )
    );
  };
  FileMetadataView* metadataView = nullptr;
  if (targets.size() == 1) {
    const auto& entry = targets.front();
    auto icon = std::make_unique<luciole::FileIcon>(m_icons, 48.0F);
    icon->bind(entry.icon, colorSpecFromRole(ColorRole::OnSurface));
    general->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceMd}, std::move(icon),
            ui::column(
                {.align = FlexAlign::Start, .gap = Style::spaceXs, .flexGrow = 1.0F},
                ui::label(
                    {.text = entry.displayName,
                     .fontWeight = FontWeight::SemiBold,
                     .maxWidth = width - 104.0F,
                     .maxLines = 2,
                     .configure = literal}
                ),
                ui::label(
                    {.text = luciole::fileTypeDescription(entry.type, entry.symlink, entry.contentType),
                     .fontSize = Style::fontSizeCaption,
                     .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
                )
            )
        )
    );
    general->addChild(ui::separator({}));
    field("Location", luciole::locationEditText(entry.location));
    if (entry.contentType) {
      field("MIME", *entry.contentType);
    }
    if (entry.location.isTrash()) {
      field("Original", entry.trash.original ? luciole::locationEditText(*entry.trash.original) : "Unknown");
      field("Deleted", entry.trash.deletionDate.value_or("Unknown"));
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
  } else {
    field("Selection", std::format("{} items", targets.size()));
    if (!editable) {
      field("Permissions", "Select at most 4096 items for permission editing.");
    }
  }
  FolderTotalsView* totals = nullptr;
  if (std::ranges::any_of(targets, [](const auto& entry) {
        return entry.type == luciole::FileType::Directory && !entry.symlink;
      })) {
    auto view = std::make_unique<FolderTotalsView>(targets, [this] {
      m_metadataDirty = true;
      m_page.requestSync();
    });
    totals = view.get();
    general->addChild(std::move(view));
  }
  auto metadata = std::make_unique<FileMetadataView>();
  metadataView = metadata.get();
  general->addChild(std::move(metadata));
  Body result;
  result.general = generalGroup;
  result.metadata = metadataView;
  result.folderTotals = totals;
  result.node = std::move(general);
  return result;
}

PropertiesController::Body PropertiesController::buildBody(
    const std::vector<luciole::DirectoryEntry>& targets, float width, bool editable, bool multiple, bool folders,
    bool withResults
) {
  ScrollView* scroll = nullptr;
  auto body = ui::scrollView(
      {.out = &scroll, .viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F, .fillWidth = true, .flexGrow = 1.0F}
  );
  body->setReserveScrollbarGutter(true);
  body->content()->setAlign(FlexAlign::Stretch);
  body->content()->setGap(Style::spaceMd);
  auto general = buildGeneral(targets, width, editable);
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
          m_page.menuSurface()->requestLayout();
        },
        multiple, folders
    );
    editor = permissions.get();
    auto basic = ui::column(
        {.out = &basicGroup, .align = FlexAlign::Stretch, .gap = Style::spaceSm},
        ui::row(
            {.justify = FlexJustify::End},
            ui::button(
                {.out = &advancedOpen,
                 .text = "Advanced permissions…",
                 .variant = ButtonVariant::Ghost,
                 .onClick = [this] { showAdvanced(true, true); }}
            )
        ),
        std::move(permissions)
    );
    const auto aclChanged = [this] {
      if (m_permissionSave) {
        m_permissionSave->setEnabled(canApply() && !m_page.operationsBusy());
      }
      m_page.menuSurface()->requestLayout();
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
         .gap = Style::spaceSm,
         .visible = false,
         .participatesInLayout = false},
        ui::row(
            {.justify = FlexJustify::Start},
            ui::button(
                {.out = &advancedBack,
                 .text = "Back to permissions",
                 .glyph = "chevron-left",
                 .variant = ButtonVariant::Ghost,
                 .onClick = [this] { showAdvanced(false, true); }}
            )
        ),
        ui::label({.text = "Advanced permissions", .fontWeight = FontWeight::SemiBold}), std::move(aclControl)
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
    return entry.type == luciole::FileType::Directory
        && !entry.symlink
        && entry.location.nativePath()
        && !entry.location.isTrash();
  });
  const bool withResults = multiple || folders;
  auto& surface = m_page.surface();
  const float width = std::min(editable ? 600.0F : 440.0F, static_cast<float>(surface.width()) - 32.0F);
  auto body = buildBody(targets, width, editable, multiple, folders, withResults);
  auto* scroll = body.scroll;
  auto* generalGroup = body.general;
  auto* permissionsGroup = body.permissions;
  auto* results = body.results;
  const auto literal = [](Label& label) { label.setUseMarkup(false); };
  Segmented* tabs = nullptr;
  Button *save = nullptr, *reportButton = nullptr, *cancelButton = nullptr;
  Label* progress = nullptr;
  std::vector<ui::SegmentedOption> propertyTabs{
      {.label = "General", .glyph = {}, .tooltip = {}}, {.label = "Permissions", .glyph = {}, .tooltip = {}}
  };
  constexpr std::size_t resultTab = 2;
  if (withResults) {
    propertyTabs.push_back({.label = "Results", .glyph = {}, .tooltip = {}});
  }
  auto root = ui::column(
      {.align = FlexAlign::Stretch},
      luciole::dialogHeader("Properties", "list-details", [this] { m_page.dismissMenu(); }),
      ui::segmented(
          {.out = &tabs,
           .options = std::move(propertyTabs),
           .equalSegmentWidths = true,
           .visible = editable,
           .participatesInLayout = editable,
           .onChange =
               [this, generalGroup, permissionsGroup, scroll, results](std::size_t index) {
                 generalGroup->setVisible(index == 0);
                 generalGroup->setParticipatesInLayout(index == 0);
                 if (permissionsGroup) {
                   permissionsGroup->setVisible(index == 1);
                   permissionsGroup->setParticipatesInLayout(index == 1);
                 }
                 showAdvanced(false);
                 if (m_permissionSave) {
                   m_permissionSave->setEnabled(canApply() && !m_page.operationsBusy());
                 }
                 if (results) {
                   results->setVisible(index == resultTab);
                   results->setParticipatesInLayout(index == resultTab);
                   scroll->setVisible(index != resultTab);
                   scroll->setParticipatesInLayout(index != resultTab);
                 }
                 scroll->requestScrollToOffset(0);
                 m_page.menuSurface()->requestLayout();
               }}
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
            {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm},
            ui::button(
                {.out = &reportButton,
                 .text = "Copy report",
                 .enabled = false,
                 .variant = ButtonVariant::Ghost,
                 .visible = withResults,
                 .participatesInLayout = withResults,
                 .onClick =
                     [this] {
                       if (m_job) {
                         m_page.clipboard().setClipboardText(
                             m_job->service<luciole::PermissionService>().snapshot().report
                         );
                       }
                     }}
            ),
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
            ui::button({.text = "Close", .variant = ButtonVariant::Ghost, .onClick = [this] { m_page.dismissMenu(); }}),
            ui::button(
                {.out = &save, .text = "Apply", .enabled = false, .variant = ButtonVariant::Primary, .onClick = [this] {
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
  UiPhaseScope phase(UiPhase::Layout);
  LayoutConstraints constraints;
  constraints.setExactWidth(width);
  const float height = (editable || multiple)
      ? std::min(600.0F, static_cast<float>(surface.height()) - 64.0F)
      : std::min(
            root->measure(surface.renderTarget().renderer(), constraints).height,
            static_cast<float>(surface.height()) - 64.0F
        );
  if (editable || multiple) {
    root->setSize(width, height);
  }
  m_page.showPopup(
      std::move(root), nullptr, {(surface.width() - width) * 0.5F, (surface.height() - height) * 0.5F, 1, 1}, width,
      height, false, PopupMode::Dialog
  );
  if (m_page.menuSurface()) {
    m_page.activeDialog().bind(ActiveDialog::Kind::Properties, [this] { close(); });
    m_metadataView = body.metadata;
    m_folderTotals = body.folderTotals;
    m_metadataWidth = std::max(1.0F, width - Style::spaceMd * 2 - Style::scrollbarWidth - Style::scrollbarGap);
    m_multiple = multiple;
    if (m_metadataView) {
      if (multiple) {
        m_selectionMetadata.show(targets);
      } else {
        m_metadata.show(targets.front(), m_page.settings().thumbnails);
      }
      m_metadataDirty = true;
    }
    m_page.setDialogSize(width, height);
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
    m_permissionResultTab = resultTab;
    m_permissionResults = results;
    m_permissionReport = reportButton;
    m_permissionCancel = cancelButton;
    m_permissionStatus = progress;
    m_permissionTabs = tabs;
    m_permissionSave = save;
    m_permissionScroll = scroll;
    m_page.menuDispatcher().setFocusChangeCallback([this](InputArea*, InputArea* focused) {
      if (m_permissionScroll) {
        m_permissionScroll->requestScrollIntoView(focused);
        m_page.menuSurface()->requestLayout();
      }
    });
    m_job = m_page.jobs().create<luciole::PermissionService>(JobScope::Page);
    auto& permissions = m_job->service<luciole::PermissionService>();
    if (captureCurrentLocation) {
      (void)permissions.prepare(targets.front().location);
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
  m_multiple = false;
  m_metadataView = nullptr;
  m_folderTotals = nullptr;
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
  m_permissionResultTab = 2;
  m_permissionResults = nullptr;
  m_permissionReport = m_permissionCancel = nullptr;
  m_permissionStatus = nullptr;
  m_permissionTabs = nullptr;
  m_permissionSave = nullptr;
  m_permissionScroll = nullptr;
  m_page.focusFileList();
}
