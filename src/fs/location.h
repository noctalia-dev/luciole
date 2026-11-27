#pragma once

#include <optional>
#include <string>

namespace luciole {
  // URIs and native names are operation data, never presentation text.
  class Location {
  public:
    static Location fromUri(const std::string& uri);
    static Location fromPath(const std::string& absolutePath);
    // For a URI that already is g_file_get_uri() output of a native file; skips revalidation and re-canonicalization.
    static Location fromCanonicalNativeUri(std::string uri);
    static Location trash();
    static Location network();
    bool isNetworkDiscovery() const;
    // MTP or PTP device root; GVFS lists it empty while the device is locked or not in file transfer mode.
    bool isDeviceRoot() const;

    const std::string& uri() const { return m_uri; }
    std::optional<std::string> nativePath() const;
    // file:// URI of nativePath() built without GVfs, which would map a FUSE path back to its remote URI.
    std::optional<std::string> nativeFileUri() const;
    bool isTrash() const;
    bool isNative() const;
    // Local GVfs administrator namespace, with no authority, credentials, query or fragment.
    bool isAdmin() const;
    std::optional<Location> administratorLocation() const;
    std::optional<Location> unprivilegedLocation() const;

    // nullopt at a root.
    std::optional<Location> parent() const;
    // One filename component, as raw bytes.
    Location child(const std::string& name) const;
    // A relative path below this location; absolute paths replace it.
    Location resolve(const std::string& relativePath) const;
    // Raw last-component bytes; nullopt when the provider reports none.
    std::optional<std::string> basename() const;
    // basename() as valid UTF-8 presentation text.
    std::optional<std::string> displayName() const;
    // True for strict descendants only.
    bool isInside(const Location& ancestor) const;

    bool operator==(const Location&) const = default;

  private:
    explicit Location(std::string uri);
    std::string m_uri;
  };
} // namespace luciole
