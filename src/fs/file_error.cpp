#include "fs/file_error.h"

#include <cerrno>
#include <cstring>
#include <exception>
#include <utility>

luciole::FileErrorException::FileErrorException(FileError error)
    : std::runtime_error(error.message), m_error(std::move(error)) {}

luciole::FileError luciole::errnoError(int code) {
  using Kind = FileErrorKind;
  auto kind = Kind::Other;
  switch (code) {
  case ENOENT:
    kind = Kind::Missing;
    break;
  case EACCES:
  case EPERM:
    kind = Kind::Denied;
    break;
  case ENOTDIR:
    kind = Kind::NotDirectory;
    break;
  case EISDIR:
    kind = Kind::IsDirectory;
    break;
  case EEXIST:
    kind = Kind::Exists;
    break;
  case ENOTEMPTY:
    kind = Kind::NotEmpty;
    break;
  case ENAMETOOLONG:
    kind = Kind::NameTooLong;
    break;
  case ENOSPC:
  case EDQUOT:
    kind = Kind::NoSpace;
    break;
  case EROFS:
    kind = Kind::ReadOnly;
    break;
  case EINVAL:
    kind = Kind::InvalidArgument;
    break;
  case ELOOP:
    kind = Kind::WouldRecurse;
    break;
  case ENOTSUP:
  case ENOSYS:
  case EXDEV:
    kind = Kind::Unsupported;
    break;
  case ENODEV:
  case ENXIO:
  case ENOTCONN:
  case ESTALE:
  case EHOSTUNREACH:
  case EHOSTDOWN:
  case ENETUNREACH:
  case ENETDOWN:
  case ECONNRESET:
  case ECONNREFUSED:
  case ETIMEDOUT:
    kind = Kind::Unavailable;
    break;
  case ECANCELED:
    kind = Kind::Cancelled;
    break;
  default:
    break;
  }
  return {.kind = kind, .message = std::strerror(code)};
}

void luciole::throwFileError(FileErrorKind kind, std::string message) {
  throw FileErrorException({.kind = kind, .message = std::move(message)});
}

void luciole::throwErrno(int code) { throw FileErrorException(errnoError(code)); }

luciole::FileError luciole::currentFileError() {
  try {
    throw;
  } catch (const FileErrorException& error) {
    return error.error();
  } catch (const std::exception& error) {
    return {.kind = FileErrorKind::Other, .message = error.what()};
  } catch (...) {
    return {.kind = FileErrorKind::Other, .message = "Unknown error."};
  }
}
