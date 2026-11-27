#include "ui/dialogs/archive_dialog.h"

#include "i18n/i18n.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/dialogs/dialog_chrome.h"

#include <utility>

ArchiveDialog::ArchiveDialog(
    bool compress, std::string name, std::string destination,
    std::function<std::string(
        const std::string&, const std::string&, luciole::ArchiveFormat, luciole::ArchiveCompressionLevel,
        const std::string&
    )>
        submit,
    std::function<void()> cancel
)
    : m_submit(std::move(submit)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  luciole::applyDialogStyle(*this);
  addChild(luciole::dialogHeader(compress ? "Compress" : "Extract Archive", compress ? "file-zip" : "archive", cancel));
  auto scroll =
      ui::scrollView({.out = &m_scroll, .viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F, .flexGrow = 1.0F});
  scroll->content()->setDirection(FlexDirection::Vertical);
  scroll->content()->setAlign(FlexAlign::Stretch);
  auto body = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm});
  if (compress) {
    body->addChild(ui::label({.text = "Archive Name"}));
    body->addChild(ui::input({.out = &m_name, .value = std::move(name), .onSubmit = [this](const std::string&) {
                                this->submit();
                              }}));
    std::vector<ui::SegmentedOption> options;
    for (const auto& format : luciole::archiveFormats()) {
      options.push_back({.label = std::string(format.label), .glyph = {}, .tooltip = std::string(format.suffix)});
    }
    body->addChild(
        ui::segmented(
            {.options = std::move(options),
             .compact = true,
             .equalSegmentWidths = true,
             .onChange = [this](std::size_t index) {
               const auto formats = luciole::archiveFormats();
               std::string value = m_name->value();
               const auto old = formats[m_format].suffix;
               if (value.ends_with(old)) {
                 value.resize(value.size() - old.size());
               }
               m_format = index;
               value += formats[index].suffix;
               m_name->setValue(value);
               showCompression(luciole::archiveCompressionLevelAvailable(formats[index].format));
               if (m_passwordSection) {
                 m_password->setValue("");
                 m_confirmation->setValue("");
                 showPassword(formats[index].format == luciole::ArchiveFormat::Zip);
               }
             }}
        )
    );
    auto compressionSection =
        ui::column({.out = &m_compressionSection, .align = FlexAlign::Stretch, .gap = Style::spaceSm});
    compressionSection->addChild(ui::label({.text = "Compression"}));
    std::vector<ui::SegmentedOption> levelOptions{
        {.label = "Fast", .glyph = {}, .tooltip = {}},
        {.label = "Balanced", .glyph = {}, .tooltip = {}},
        {.label = "Maximum", .glyph = {}, .tooltip = {}}
    };
    compressionSection->addChild(
        ui::segmented(
            {.options = std::move(levelOptions),
             .selectedIndex = static_cast<std::size_t>(luciole::ArchiveCompressionLevel::Balanced),
             .compact = true,
             .equalSegmentWidths = true,
             .onChange = [this](std::size_t index) { m_level = static_cast<luciole::ArchiveCompressionLevel>(index); }}
        )
    );
    body->addChild(std::move(compressionSection));
    const auto formats = luciole::archiveFormats();
    showCompression(m_format < formats.size() && luciole::archiveCompressionLevelAvailable(formats[m_format].format));
  }
  body->addChild(ui::label({.text = compress ? "Destination Folder" : "New Output Folder"}));
  body->addChild(
      ui::input({.out = &m_destination, .value = std::move(destination), .onSubmit = [this](const std::string&) {
                   this->submit();
                 }})
  );
  if (!compress) {
    body->addChild(
        ui::label(
            {.text = "Extract into a new folder. Existing folders require a conflict decision.",
             .fontSize = Style::fontSizeCaption,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
             .maxLines = 2}
        )
    );
  }
  if (compress && luciole::archiveEncryptionAvailable()) {
    auto section = ui::column({.out = &m_passwordSection, .align = FlexAlign::Stretch, .gap = Style::spaceSm});
    section->addChild(ui::label({.text = "Password (optional)"}));
    section->addChild(ui::input({.out = &m_password, .passwordMode = true, .onSubmit = [this](const std::string&) {
                                   this->submit();
                                 }}));
    section->addChild(ui::label({.text = "Confirm Password"}));
    section->addChild(ui::input({.out = &m_confirmation, .passwordMode = true, .onSubmit = [this](const std::string&) {
                                   this->submit();
                                 }}));
    section->addChild(
        ui::label(
            {.text = "ZIP AES-256 protects file contents. File names remain visible.",
             .fontSize = Style::fontSizeCaption,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
             .maxLines = 2}
        )
    );
    body->addChild(std::move(section));
    const auto formats = luciole::archiveFormats();
    showPassword(m_format < formats.size() && formats[m_format].format == luciole::ArchiveFormat::Zip);
  }
  scroll->content()->addChild(std::move(body));
  addChild(std::move(scroll));
  addChild(ui::label({.out = &m_error, .color = colorSpecFromRole(ColorRole::Error), .height = 40.0F, .maxLines = 2}));
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm},
          ui::button({.text = "Cancel", .variant = ButtonVariant::Ghost, .onClick = std::move(cancel)}),
          ui::button({.text = compress ? "Compress" : "Extract", .variant = ButtonVariant::Primary, .onClick = [this] {
                        this->submit();
                      }})
      )
  );
}
void ArchiveDialog::submit() {
  if (m_passwordSection && m_passwordSection->visible()) {
    const auto& password = m_password->value();
    if (const auto problem = luciole::archivePasswordProblem(password)) {
      m_error->setText(*problem);
      return;
    }
    if (password != m_confirmation->value()) {
      m_error->setText(i18n::text("Passwords do not match."));
      return;
    }
  }
  const auto error = m_submit(
      m_name ? m_name->value() : "", m_destination->value(),
      luciole::archiveFormats().empty() ? luciole::ArchiveFormat::Zip : luciole::archiveFormats()[m_format].format,
      m_level, m_passwordSection && m_passwordSection->visible() ? m_password->value() : ""
  );
  m_error->setText(error);
  if (error.empty() && m_passwordSection) {
    m_password->setValue("");
    m_confirmation->setValue("");
  }
  markLayoutDirty();
}
void ArchiveDialog::showPassword(bool visible) {
  m_passwordSection->setVisible(visible);
  m_passwordSection->setParticipatesInLayout(visible);
  markLayoutDirty();
}
void ArchiveDialog::showCompression(bool visible) {
  if (m_compressionSection) {
    m_compressionSection->setVisible(visible);
    m_compressionSection->setParticipatesInLayout(visible);
    markLayoutDirty();
  }
}
InputArea* ArchiveDialog::focusArea() const { return (m_name ? m_name : m_destination)->inputArea(); }
void ArchiveDialog::reveal(const Node* node) { m_scroll->requestScrollIntoView(node); }

ArchivePasswordDialog::ArchivePasswordDialog(
    std::string name, bool retry, std::function<bool(std::string)> submit, std::function<void()> cancel,
    luciole::ArchiveFormat format
)
    : m_submit(std::move(submit)), m_format(format) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  luciole::applyDialogStyle(*this);
  addChild(luciole::dialogHeader("Archive Password", "lock", cancel));
  addChild(ui::label({.text = std::move(name), .maxLines = 2, .configure = [](Label& label) {
                        label.setUseMarkup(false);
                      }}));
  addChild(
      ui::label(
          {.text = retry ? (format == luciole::ArchiveFormat::SevenZip
                                ? "Could not unlock the data. Check the password or archive integrity."
                                : "That password did not unlock all selected files. Try another password.")
                         : "Enter the password to unlock the selected files.",
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(retry ? ColorRole::Error : ColorRole::OnSurfaceVariant),
           .maxLines = 2}
      )
  );
  addChild(ui::input({.out = &m_password, .passwordMode = true, .onSubmit = [this](const std::string&) {
                        this->submit();
                      }}));
  addChild(ui::label({.out = &m_error, .color = colorSpecFromRole(ColorRole::Error), .maxLines = 2}));
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm},
          ui::button({.text = "Cancel", .variant = ButtonVariant::Ghost, .onClick = std::move(cancel)}),
          ui::button({.text = "Unlock", .variant = ButtonVariant::Primary, .onClick = [this] { this->submit(); }})
      )
  );
}
void ArchivePasswordDialog::submit() {
  const auto& password = m_password->value();
  if (password.empty()) {
    m_error->setText(i18n::text("Enter a password."));
  } else if (const auto problem = luciole::archivePasswordProblem(password, m_format)) {
    m_error->setText(*problem);
  } else if (m_submit(password)) {
    m_password->setValue("");
  } else {
    m_error->setText(i18n::text("This password request is no longer active."));
  }
  markLayoutDirty();
}
InputArea* ArchivePasswordDialog::focusArea() const { return m_password->inputArea(); }
