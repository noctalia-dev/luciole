#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace luciole {
  enum class FileErrorKind : std::uint8_t {
    Missing,
    Denied,
    NotDirectory,
    IsDirectory,
    Exists,
    NotEmpty,
    NameTooLong,
    NoSpace,
    ReadOnly,
    InvalidArgument,
    WouldRecurse,
    WouldMerge,
    Unavailable,
    NotMounted,
    Unsupported,
    Changed, // The item or its folder no longer matches what the operation captured.
    Cancelled,
    Other,
  };

  // message is the provider's text (strerror, GError) or an operation's own sentence;
  // presentation code may replace it with phrasing chosen by kind.
  struct FileError {
    FileErrorKind kind = FileErrorKind::Other;
    std::string message;
    bool operator==(const FileError&) const = default;
  };

  FileError errnoError(int code);

  // Thrown by worker code; background jobs deliver error() to the main context.
  class FileErrorException : public std::runtime_error {
  public:
    explicit FileErrorException(FileError error);
    const FileError& error() const { return m_error; }

  private:
    FileError m_error;
  };

  [[noreturn]] void throwFileError(FileErrorKind kind, std::string message);
  [[noreturn]] void throwErrno(int code);

  // Inside a catch handler: the in-flight exception as a FileError.
  FileError currentFileError();
} // namespace luciole
