#include "ui/browser/file_error_text.h"

std::string luciole::fileErrorText(const FileError& error) {
  if (!error.message.empty()) {
    return error.message;
  }
  using Kind = FileErrorKind;
  switch (error.kind) {
  case Kind::Missing:
    return "The item is no longer available.";
  case Kind::Denied:
    return "Permission denied.";
  case Kind::NotDirectory:
    return "Not a folder.";
  case Kind::IsDirectory:
    return "The item is a folder.";
  case Kind::Exists:
    return "An item with that name already exists.";
  case Kind::NotEmpty:
    return "The folder is not empty.";
  case Kind::NameTooLong:
    return "The name is too long.";
  case Kind::NoSpace:
    return "There is not enough space.";
  case Kind::ReadOnly:
    return "The location is read-only.";
  case Kind::InvalidArgument:
    return "The request is not valid.";
  case Kind::WouldRecurse:
    return "A folder cannot be placed inside itself.";
  case Kind::WouldMerge:
    return "The folders would have to be merged.";
  case Kind::NotMounted:
    return "The location is not mounted.";
  case Kind::Unavailable:
    return "The location is unavailable.";
  case Kind::Unsupported:
    return "This operation is not supported here.";
  case Kind::Changed:
    return "The item changed. Try again.";
  case Kind::Cancelled:
    return "Cancelled.";
  case Kind::Other:
    break;
  }
  return "The operation failed.";
}

std::string luciole::fileErrorText(const std::optional<FileError>& error) {
  return error ? fileErrorText(*error) : std::string{};
}

std::string luciole::nameErrorText(const FileError& error, std::string_view missing) {
  using Kind = FileErrorKind;
  switch (error.kind) {
  case Kind::Exists:
  case Kind::NotEmpty:
    return "An item with that name already exists.";
  case Kind::NameTooLong:
    return "The name is too long for this folder.";
  case Kind::Denied:
    return "Permission denied.";
  case Kind::Missing:
    return std::string(missing);
  default:
    return fileErrorText(error);
  }
}
