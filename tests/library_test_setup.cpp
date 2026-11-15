#include "core/application_identity.h"

#ifdef LUCIOLE_TEST_LOCAL_GIO
#include <gio/gio.h>
#endif
namespace {
  const bool initialized = [] {
    setApplicationIdentity({.name = "luciole", .envPrefix = "LUCIOLE"});
#ifdef LUCIOLE_TEST_LOCAL_GIO
    // Initialize GIO's process-global mount cache before concurrent filesystem queries.
    GFile* root = g_file_new_for_path("/");
    GFileInfo* info = g_file_query_filesystem_info(root, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, nullptr, nullptr);
    g_clear_object(&info);
    g_object_unref(root);
#endif
    return true;
  }();
} // namespace
