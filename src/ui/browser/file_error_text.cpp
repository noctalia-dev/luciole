#include "ui/browser/file_error_text.h"

#include "i18n/i18n.h"

std::string luciole::fileErrorText(const FileError& error) {
  if (!error.message.empty()) {
    return i18n::text(error.message);
  }
  using Kind = FileErrorKind;
  switch (error.kind) {
  case Kind::Missing:
    return i18n::text("The item is no longer available.");
  case Kind::Denied:
    return i18n::text("Permission denied.");
  case Kind::NotDirectory:
    return i18n::text("Not a folder.");
  case Kind::IsDirectory:
    return i18n::text("The item is a folder.");
  case Kind::Exists:
    return i18n::text("An item with that name already exists.");
  case Kind::NotEmpty:
    return i18n::text("The folder is not empty.");
  case Kind::NameTooLong:
    return i18n::text("The name is too long.");
  case Kind::NoSpace:
    return i18n::text("There is not enough space.");
  case Kind::ReadOnly:
    return i18n::text("The location is read-only.");
  case Kind::InvalidArgument:
    return i18n::text("The request is not valid.");
  case Kind::WouldRecurse:
    return i18n::text("A folder cannot be placed inside itself.");
  case Kind::WouldMerge:
    return i18n::text("The folders would have to be merged.");
  case Kind::NotMounted:
    return i18n::text("The location is not mounted.");
  case Kind::Unavailable:
    return i18n::text("The location is unavailable.");
  case Kind::Unsupported:
    return i18n::text("This operation is not supported here.");
  case Kind::Changed:
    return i18n::text("The item changed. Try again.");
  case Kind::Cancelled:
    return i18n::text("Cancelled.");
  case Kind::Other:
    break;
  }
  return i18n::text("The operation failed.");
}

std::string luciole::fileErrorText(const std::optional<FileError>& error) {
  return error ? fileErrorText(*error) : std::string{};
}

std::string luciole::nameErrorText(const FileError& error, std::string_view missing) {
  using Kind = FileErrorKind;
  switch (error.kind) {
  case Kind::Exists:
  case Kind::NotEmpty:
    return i18n::text("An item with that name already exists.");
  case Kind::NameTooLong:
    return i18n::text("The name is too long for this folder.");
  case Kind::Denied:
    return i18n::text("Permission denied.");
  case Kind::Missing:
    return i18n::text(missing);
  default:
    return fileErrorText(error);
  }
}
