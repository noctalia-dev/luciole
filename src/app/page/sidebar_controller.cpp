#include "app/page/sidebar_controller.h"

#include "app/drive_removal.h"
#include "app/operation_hub.h"
#include "app/page/drag_drop_controller.h"
#include "app/page/menu_controller.h"
#include "app/page/page_context.h"
#include "app/page/properties_controller.h"
#include "app/page/rename_controller.h"
#include "app/page/sidebar_controller.h"
#include "browser/browser_controller.h"
#include "browser/places.h"
#include "config/settings.h"
#include "config/view_preference_writer.h"
#include "core/input/key_symbols.h"
#include "cursor-shape-v1-client-protocol.h"
#include "fs/directory_entries.h"
#include "fs/drive_monitor.h"
#include "fs/file_metadata.h"
#include "platform/desktop_integration.h"
#include "render/animation/animation_manager.h"
#include "ui/browser/action_shortcuts.h"
#include "ui/browser/places_sidebar.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "wayland/toplevel_surface.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <unistd.h>

SidebarController::SidebarController(
    PageContext& page, luciole::PinService& pins, luciole::DriveMonitor& drives,
    luciole::ViewPreferenceWriter& preferences, AnimationManager& animations, MenuController& menus,
    RenameController& renamer, PropertiesController& properties, DragDropController& drags
)
    : m_page(page), m_pins(pins), m_drives(drives), m_preferences(preferences), m_animations(animations),
      m_menus(menus), m_renamer(renamer), m_properties(properties), m_drags(drags) {}

void SidebarController::load(const luciole::BrowserSettings& settings) {
  m_sidebarWidth = settings.sidebarWidth;
  m_state.setPreferred(settings.showSidebar);
  (void)m_state.updateWidth(static_cast<float>(m_page.surface().width()));
  m_reveal = visible() ? 1.0F : 0.0F;
  m_sidebarTree = settings.sidebarTree;
  m_showDrives = settings.showDrives;
  m_showNetwork = settings.showNetwork;
}

void SidebarController::applySettings(
    const luciole::BrowserSettings& before, const luciole::BrowserSettings& settings, bool force
) {
  if (force || before.showSidebar != settings.showSidebar) {
    setVisible(settings.showSidebar, false);
  }
  if (force || before.sidebarTree != settings.sidebarTree) {
    m_sidebarTree = settings.sidebarTree;
    if (m_places) {
      m_places->setTreeEnabled(m_sidebarTree);
    }
  }
  if (force || before.showDrives != settings.showDrives) {
    m_showDrives = settings.showDrives;
    if (m_places) {
      m_places->setShowDrives(m_showDrives);
    }
  }
  if (force || before.showNetwork != settings.showNetwork) {
    m_showNetwork = settings.showNetwork;
    if (m_places) {
      m_places->setShowNetwork(m_showNetwork);
    }
  }
  if (force || before.sidebarWidth != settings.sidebarWidth) {
    m_sidebarWidth = settings.sidebarWidth;
  }
}

void SidebarController::setVisible(bool visible, bool persist) {
  if (!m_state.compact() && visible == m_state.visible()) {
    return;
  }
  m_page.dispatcher().cancelPointerCapture();
  if (m_drags.dragging()) {
    m_drags.cancelDrag();
  }
  m_page.captureViewAnchor();
  m_state.setPreferred(visible);
  revealDrawer();
  if (!visible) {
    m_restorePlaceFocus.reset();
    for (const Node* node = m_page.dispatcher().focusedArea(); node != nullptr; node = node->parent()) {
      if (node == m_places || node == m_sidebarResize) {
        m_page.dispatcher().setFocus(m_page.fileList()->focusArea());
        break;
      }
    }
  }
  if (persist) {
    m_preferences.saveShowSidebar(visible);
  }
  m_page.surface().requestLayout();
}

float SidebarController::width() const {
  return !m_state.compact()
      ? (static_cast<float>(std::min(m_sidebarWidth, maximumWidth())) + kWorkspaceInset) * m_reveal
      : 0.0F;
}

float SidebarController::controlWidth() const {
  return visible() || m_reveal > 0
      ? static_cast<float>(std::min(m_sidebarWidth, maximumWidth())) + kWorkspaceInset - kGapInset
      : 0.0F;
}

int SidebarController::maximumWidth() const {
  return std::clamp(
      static_cast<int>(m_page.surface().width())
          - (m_state.compact() ? 48 : 320 + static_cast<int>(kWorkspaceInset * 3)),
      luciole::kMinSidebarWidth, luciole::kMaxSidebarWidth
  );
}

void SidebarController::resize(int width, bool persist) {
  const int bounded = std::clamp(width, luciole::kMinSidebarWidth, maximumWidth());
  if (bounded == m_sidebarWidth) {
    return;
  }
  m_page.captureViewAnchor();
  m_sidebarWidth = bounded;
  if (persist) {
    m_preferences.saveSidebarWidth(m_sidebarWidth);
  }
  m_page.surface().requestLayout();
}

void SidebarController::take(Node& host) {
  if (m_detachedSidebar) {
    host.addChild(std::move(m_detachedSidebar));
  }
}

void SidebarController::giveBack() {
  dismissDrawer();
  if (m_sidebarBundle && m_sidebarBundle->parent()) {
    m_detachedSidebar = m_sidebarBundle->parent()->removeChild(m_sidebarBundle);
  }
}

void SidebarController::reuseFrom(SidebarController& previous) {
  if (&previous == this || !m_sidebarBundle || !previous.m_sidebarBundle) {
    return;
  }
  auto ready = previous.m_sidebarPanel->removeChild(previous.m_places);
  auto placeholder = m_sidebarPanel->removeChild(m_places);
  std::swap(m_places, previous.m_places);
  m_sidebarPanel->insertChildAt(0, std::move(ready));
  previous.m_sidebarPanel->insertChildAt(0, std::move(placeholder));
  m_placesDirty = previous.m_placesDirty;
  previous.m_placesDirty = true;
  configureCallbacks();
  previous.configureCallbacks();
}

void SidebarController::configureCallbacks() {
  m_places->setChangedCallback([this] { m_page.requestSync(); });
  m_places->setTreeEnabled(m_sidebarTree);
  m_places->setShowDrives(m_showDrives);
  m_places->setShowNetwork(m_showNetwork);
  m_places->setOpenCallback([this](const luciole::Location& location) {
    m_page.setActionMessage({});
    m_page.browser().open(location);
    dismissDrawer();
    m_page.dispatcher().setFocus(m_page.fileList()->focusArea());
  });
  m_places->setContextCallback([this](const luciole::Bookmark& bookmark, float x, float y) {
    m_menus.openPlaceMenu(bookmark, x, y);
  });
  m_places->setDragCallback([this](const luciole::Bookmark& bookmark) { m_drags.startPlaceDrag(bookmark); });
  m_places->setRenameCallback([this](const luciole::Bookmark& bookmark) { m_renamer.openPlaceRename(bookmark); });
  m_places->setDriveOpenCallback([this](const std::string& key) { openDrive(key); });
  m_places->setDriveRemoveCallback([this](const std::string& key) { removeDrive(key); });
  m_places->setDriveContextCallback([this](const std::string& key, float x, float y) { openDriveMenu(key, x, y); });
}

void SidebarController::openDrive(const std::string& key) {
  m_page.setActionMessage({});
  m_page.browser().openDrive(m_drives, key, [this, lifetime = std::weak_ptr(m_lifetime)](std::string error) {
    if (lifetime.expired()) {
      return;
    }
    if (!error.empty()) {
      m_page.actionError(std::format("Could not mount the drive: {}", error));
      return;
    }
    m_page.dispatcher().setFocus(m_page.fileList()->focusArea());
  });
  dismissDrawer();
}

void SidebarController::removeDrive(const std::string& key) {
  const auto* drive = m_drives.find(key);
  if (drive == nullptr || !drive->root || m_removingDrive) {
    return;
  }
  const auto root = *drive->root;
  auto verb = std::string(luciole::driveRemovalLabel(drive->removal));
  std::ranges::transform(verb, verb.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  m_removingDrive = true;
  m_page.jobs().leaveDrive(root);
  removeDriveAfterPreparation(
      [this, root, lifetime = std::weak_ptr(m_lifetime)](auto done) {
        if (lifetime.expired()) {
          done(luciole::FileError{luciole::FileErrorKind::Cancelled, "Drive removal cancelled."});
          return;
        }
        m_page.jobs().prepareDriveRemoval(root, std::move(done));
      },
      [this, root, lifetime = std::weak_ptr(m_lifetime)] {
        return lifetime.expired() || m_page.jobs().removalReady(root);
      },
      [this, key, root, lifetime = std::weak_ptr(m_lifetime)](auto done) {
        if (lifetime.expired()) {
          done({.error = "Drive removal cancelled.", .busy = false, .blockers = {}});
          return;
        }
        const auto* current = m_drives.find(key);
        if (!current || current->root != root) {
          done({.error = "This drive can no longer be removed.", .busy = false, .blockers = {}});
          return;
        }
        m_drives.remove(key, std::move(done));
      },
      [this, verb = std::move(verb), lifetime = std::weak_ptr(m_lifetime)](luciole::DriveRemoveResult result) {
        if (lifetime.expired()) {
          return;
        }
        m_removingDrive = false;
        if (result.error.empty()) {
          return;
        }
        if (!result.busy) {
          m_page.actionError(std::format("Could not {} the drive: {}", verb, result.error));
          return;
        }
        // Name who keeps it busy; for this process, its running file jobs say more than its name.
        std::vector<std::string> users;
        for (const auto& blocker : result.blockers) {
          if (blocker.pid == getpid()) {
            const auto jobs = m_page.runningJobTitles();
            users.insert(users.end(), jobs.begin(), jobs.end());
            if (jobs.empty()) {
              users.emplace_back("Luciole");
            }
          } else {
            users.push_back(blocker.name);
          }
        }
        std::ranges::sort(users);
        users.erase(std::ranges::unique(users).begin(), users.end());
        std::string names;
        for (std::size_t i = 0; i < users.size(); ++i) {
          names += i == 0 ? "" : (i + 1 == users.size() ? " and " : ", ");
          names += users[i];
        }
        m_page.actionError(
            names.empty() ? std::format("Could not {} the drive: files on it are still open.", verb)
                          : std::format("Could not {} the drive: it is in use by {}.", verb, names)
        );
      }
  );
}

void SidebarController::openDriveMenu(const std::string& key, float x, float y) {
  const auto* drive = m_drives.find(key);
  if (drive == nullptr) {
    return;
  }
  std::vector<ContextMenuControlEntry> entries{
      {.id = 1, .label = drive->root || drive->activation ? "Open" : "Mount and Open"}
  };
  if (drive->removal != luciole::DriveRemoval::None) {
    entries.push_back({.id = 2, .label = std::string(luciole::driveRemovalLabel(drive->removal))});
  }
  const bool canShowProperties = drive->root.has_value();
  if (canShowProperties) {
    entries.push_back({.label = "", .separator = true});
    entries.push_back({.id = 3, .label = "Properties"});
  }
  m_menus.showContextMenu(
      std::move(entries),
      [this, key](int action) {
        if (action == 1) {
          openDrive(key);
        } else if (action == 2) {
          removeDrive(key);
        } else if (action == 3) {
          const auto* target = m_drives.find(key);
          if (target == nullptr || !target->root) {
            return;
          }
          luciole::DirectoryEntry entry{
              .place = *target->root,
              .name = target->label,
              .type = luciole::FileType::Directory,
              .size = std::nullopt,
              .modifiedSeconds = std::nullopt
          };
          entry.setDisplayName(target->label);
          m_properties.show({std::move(entry)}, true, true);
        }
      },
      {x, y, 1, 1}
  );
}

void SidebarController::pinFolders(std::vector<luciole::Bookmark> bookmarks, std::size_t before) {
  if (!m_pins.add(std::move(bookmarks), before)) {
    m_page.actionError(
        m_pins.busy() ? "Pins are being saved. Try again shortly." : "Reload valid pins before editing them."
    );
  }
}

std::unique_ptr<Node> SidebarController::buildPlaces() {
  auto places = std::make_unique<luciole::PlacesSidebar>(m_pins, m_drives);
  m_places = places.get();
  configureCallbacks();
  return places;
}

std::unique_ptr<Node> SidebarController::buildResizeHandle() {
  auto sidebarResize = ui::inputArea(
      {.out = &m_sidebarResize,
       .acceptedButtons = InputArea::buttonMask(BTN_LEFT),
       .cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE,
       .focusable = true,
       .tabStop = true,
       .tooltip = "Resize Places; Left/Right adjusts, Home resets",
       .width = kHandleWidth,
       .onEnter = [this](const auto&) { m_page.surface().requestUpdate(); },
       .onLeave = [this] { m_page.surface().requestUpdate(); },
       .onMotion =
           [this](const InputArea::PointerData& pointer) {
             if (!m_resizingSidebar) {
               return;
             }
             float x = 0, y = 0;
             Node::absolutePosition(m_sidebarResize, x, y);
             const float delta = (x + pointer.localX - m_sidebarPressX) * (Style::rtl() ? -1.0F : 1.0F);
             resize(m_sidebarStartVisibleWidth + static_cast<int>(std::lround(delta)), false);
           },
       .onPress =
           [this](const InputArea::PointerData& pointer) {
             if (pointer.pressed) {
               m_sidebarStartWidth = m_sidebarWidth;
               m_sidebarStartVisibleWidth = static_cast<int>(std::lround(m_places->width()));
               m_sidebarPressX = pointer.sceneX;
               m_resizingSidebar = true;
             } else if (std::exchange(m_resizingSidebar, false) && m_sidebarWidth != m_sidebarStartWidth) {
               m_preferences.saveSidebarWidth(m_sidebarWidth);
             }
             m_page.surface().requestUpdate();
           },
       .onKeyDown =
           [this](const InputArea::KeyData& key) {
             if (!key.pressed || key.preedit || (key.modifiers != 0 && key.modifiers != KeyMod::Shift)) {
               return;
             }
             const int step = key.modifiers == KeyMod::Shift ? 24 : 8;
             if (KeySymbol::isLeft(key.sym) || KeySymbol::isRight(key.sym)) {
               const int direction = KeySymbol::isRight(key.sym) != Style::rtl() ? 1 : -1;
               resize(static_cast<int>(std::lround(m_places->width())) + direction * step, true);
             } else if (KeySymbol::isHome(key.sym) && key.modifiers == 0) {
               resize(luciole::kDefaultSidebarWidth, true);
             }
           },
       .onFocusGain = [this] { m_page.surface().requestUpdate(); },
       .onFocusLoss = [this] { m_page.surface().requestUpdate(); },
       .configure =
           [this](InputArea& handle) {
             handle.setTabFocusKey("places-width");
             handle.setOnCancel([this] {
               if (std::exchange(m_resizingSidebar, false)) {
                 m_sidebarWidth = m_sidebarStartWidth;
                 if (m_page.dispatcher().focusedArea() == m_sidebarResize) {
                   m_page.dispatcher().setFocus(nullptr);
                 }
                 m_page.surface().requestLayout();
               }
             });
             handle.addChild(ui::box({.out = &m_sidebarResizeLine}));
           }}
  );
  return sidebarResize;
}

void SidebarController::detach() {
  auto panel = ui::row(
      {.out = &m_sidebarPanel, .align = FlexAlign::Stretch, .gap = kGapInset, .participatesInLayout = false},
      m_places->parent()->removeChild(m_places), m_sidebarResize->parent()->removeChild(m_sidebarResize)
  );
  m_detachedSidebar = std::make_unique<Node>();
  m_detachedSidebar->setClipChildren(true);
  m_sidebarBundle = m_detachedSidebar.get();
  auto scrim = ui::inputArea(
      {.out = &m_drawerScrim,
       .acceptedButtons = ~0U,
       .focusable = false,
       .tabStop = false,
       .participatesInLayout = false,
       .onPress =
           [this](const InputArea::PointerData& pointer) {
             if (pointer.pressed) {
               dismissDrawer();
             }
           },
       .onAxisHandler = [](const auto&) { return true; }}
  );
  scrim->addChild(ui::box({.out = &m_scrimFill, .fill = colorSpecFromRole(ColorRole::Scrim)}));
  m_scrimFill->setHitTestVisible(false);
  m_sidebarBundle->addChild(std::move(scrim));
  panel->setZIndex(1);
  m_sidebarBundle->addChild(std::move(panel));
  m_sidebarBundle->setAnimationManager(&m_animations);
}

std::unique_ptr<Box> SidebarController::buildDropLine() {
  auto line = ui::box(
      {.out = &m_placeDropLine,
       .fill = colorSpecFromRole(ColorRole::Primary),
       .height = 2.0F,
       .visible = false,
       .participatesInLayout = false}
  );
  m_placeDropLine->setZIndex(98);
  m_placeDropLine->setHitTestVisible(false);
  return line;
}

void SidebarController::hideDropLine() { m_placeDropLine->setVisible(false); }

void SidebarController::showDropLine(Renderer& renderer, std::size_t slot) {
  m_placeDropLine->setVisible(true);
  m_placeDropLine->arrange(renderer, m_places->dropLine(slot));
}

void SidebarController::sync(const std::optional<luciole::Location>& location) {
  m_places->setShowHidden(m_page.browser().model().filter().showHidden);
  m_places->sync(location);
  if (m_trashQueryLocation != location) {
    dismissDrawer();
    m_trashQueryLocation = location;
    refreshTrashAvailability();
  }
}

void SidebarController::refreshTrashAvailability() {
  const std::weak_ptr<int> lifetime = m_lifetime;
  m_trashAvailability.query([this, lifetime](auto count) {
    if (!lifetime.expired() && count) {
      m_places->setTrashFull(*count > 0);
    }
  });
}

void SidebarController::refreshTrashAvailabilityAfterJob(luciole::JobId jobId) {
  if (m_trashJobsRefreshed.insert(jobId).second) {
    refreshTrashAvailability();
  }
}

void SidebarController::onFrame() {
  m_sidebarResizeLine->setFill(colorSpecFromRole(
      m_resizingSidebar || m_sidebarResize->focused() ? ColorRole::Primary
          : m_sidebarResize->hovered()                ? ColorRole::OnSurfaceVariant
                                                      : ColorRole::OutlineVariant
  ));
  m_sidebarResizeLine->setVisible(m_resizingSidebar || m_sidebarResize->focused() || m_sidebarResize->hovered());
}

void SidebarController::updateDriveUsage() { m_places->updateDriveUsage(); }

std::optional<InputDispatcher::TabFocusSnapshot> SidebarController::invalidate() {
  std::optional<InputDispatcher::TabFocusSnapshot> placesFocus;
  if (m_placesDirty) {
    for (const Node* node = m_page.dispatcher().focusedArea(); node != nullptr; node = node->parent()) {
      if (node == m_places) {
        placesFocus = m_page.dispatcher().captureTabFocus();
        break;
      }
    }
  }
  if (m_placesDirty) {
    m_placesDirty = false;
    m_places->invalidate();
  }
  return placesFocus;
}

void SidebarController::layout() {
  const bool show = visible() || m_reveal > 0;
  m_places->setVisible(show);
  m_places->setParticipatesInLayout(show);
  m_sidebarResize->setVisible(visible() && !m_state.compact());
  m_sidebarResize->setParticipatesInLayout(visible() && !m_state.compact());
  m_sidebarPanel->setExcludeSubtreeFromTabOrder(!visible());
  m_toggle->setSelected(visible());
  m_toggle->setTooltip(
      luciole::shortcutTooltip(visible() ? "Hide sidebar" : "Show sidebar", luciole::KeybindCommand::ToggleSidebar)
  );
  const float sidebarWidth = static_cast<float>(std::min(m_sidebarWidth, maximumWidth()));
  m_places->setMinWidth(sidebarWidth);
  m_places->setMaxWidth(sidebarWidth);
}

void SidebarController::layoutResizeLine(Renderer& renderer) {
  m_sidebarResizeLine->arrange(renderer, {2.5F, 0, 1.0F, m_sidebarResize->height()});
}

void SidebarController::restoreFocus(std::optional<InputDispatcher::TabFocusSnapshot> focus, bool covered) {
  if (focus) {
    m_page.dispatcher().restoreTabFocus(std::move(*focus));
  }
  if (m_restorePlaceFocus && !covered) {
    const auto location = *std::exchange(m_restorePlaceFocus, std::nullopt);
    if (visible()) {
      if (auto* area = m_places->focusArea(location)) {
        m_page.dispatcher().setFocus(area);
      }
    }
  }
  if (!covered) {
    focusDrawer();
  }
}
