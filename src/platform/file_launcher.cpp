#include "platform/file_launcher.h"

#include "core/log.h"
#include "fs/gio_interop.h"
#include "fs/name_matching.h"
#include "fs/network_info.h"
#include "fs/network_job.h"
#include "platform/launch_context.h"
#include "platform/terminal_command.h"

#include <algorithm>
#include <cstdint>
#include <gio/gdesktopappinfo.h>
#include <set>
#include <utility>

namespace {
  constexpr Logger kLog("file-launch");

  void queryHandler(
      const luciole::Location& location, const luciole::CancelToken& token,
      std::move_only_function<void(luciole::JobResult<luciole::GioPtr<GAppInfo>>)> done
  ) {
    luciole::runIoJob<luciole::GioPtr<GAppInfo>>(
        luciole::ioLane(location), token,
        [location, token] {
          // Remote URI handlers can work without a VFS backend. Native files use their MIME association.
          const auto file = luciole::gioFile(location);
          const luciole::GString scheme(g_file_get_uri_scheme(file.get()));
          if (!location.isNative() && scheme) {
            if (luciole::GioPtr<GAppInfo> handler(g_app_info_get_default_for_uri_scheme(scheme.get())); handler) {
              return handler;
            }
          }
          const auto info = luciole::isolatedInfo(
              location, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE "," G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE,
              G_FILE_QUERY_INFO_NONE, token
          );
          const auto mime = luciole::readContentType(info.get());
          const luciole::GString path(g_file_get_path(file.get()));
          luciole::GioPtr<GAppInfo> app(mime ? g_app_info_get_default_for_type(mime->c_str(), !path) : nullptr);
          if (!app) {
            luciole::throwFileError(
                luciole::FileErrorKind::Unsupported, "No application is associated with this file."
            );
          }
          return app;
        },
        std::move(done)
    );
  }
  luciole::ApplicationChoice applicationChoice(GAppInfo* app) {
    const char* id = g_app_info_get_id(app);
    luciole::ApplicationChoice choice;
    choice.id = id ? id : "";
    choice.app = std::shared_ptr<GAppInfo>(G_APP_INFO(g_object_ref(app)), g_object_unref);
    return choice;
  }

} // namespace

struct luciole::FileLauncher::State {
  GioPtr<GCancellable> cancellable{g_cancellable_new()};
  std::function<void(const std::string&)> changed;
  std::size_t active = 0;
  std::uint64_t nextSerial = 0;
  std::uint64_t querySerial = 0;
  GioPtr<GCancellable> queryCancel;
  ActivationProvider activation;
  std::string terminalCommand;
  static void dispatch(
      const std::shared_ptr<State>& state, const std::vector<Location>& locations, ApplicationChoice application,
      GioPtr<GAppLaunchContext> supplied = {}, std::uint64_t serial = 0, gint64 started = 0
  );
  static void context(
      const std::shared_ptr<State>& state, const std::string& appId,
      std::function<void(GioPtr<GAppLaunchContext>)> ready
  ) {
    auto replied = std::make_shared<bool>(false);
    auto reply = [state, replied, ready = std::move(ready)](const std::string& token) {
      if (std::exchange(*replied, true)) {
        return;
      }
      if (g_cancellable_is_cancelled(state->cancellable.get())) {
        --state->active;
        return;
      }
      ready(launchContext(token));
    };
    if (const auto provider = state->activation) {
      provider(appId, std::move(reply));
    } else {
      reply({});
    }
  }
};

luciole::FileLauncher::FileLauncher() : m_state(std::make_shared<State>()) {}
luciole::FileLauncher::~FileLauncher() {
  cancelApplications();
  m_state->changed = {};
  m_state->activation = {};
  g_cancellable_cancel(m_state->cancellable.get());
}
void luciole::FileLauncher::setActivationProvider(ActivationProvider provider) {
  m_state->activation = std::move(provider);
}
void luciole::FileLauncher::cancelApplications() {
  ++m_state->querySerial;
  if (m_state->queryCancel) {
    g_cancellable_cancel(m_state->queryCancel.get());
  }
}
void luciole::FileLauncher::applications(
    std::string mime, std::function<void(ApplicationList)> callback, bool includeOther, std::string search
) {
  applications(std::vector{std::move(mime)}, std::move(callback), includeOther, std::move(search));
}
void luciole::FileLauncher::applications(
    std::vector<std::string> types, std::function<void(ApplicationList)> callback, bool includeOther, std::string search
) {
  if (types.empty()) {
    types.emplace_back("application/octet-stream");
  }
  std::ranges::sort(types);
  const auto duplicates = std::ranges::unique(types);
  types.erase(duplicates.begin(), duplicates.end());
  cancelApplications();
  m_state->queryCancel.reset(g_cancellable_new());
  struct Query {
    std::shared_ptr<State> state;
    std::uint64_t serial;
    std::vector<std::string> types;
    std::function<void(ApplicationList)> callback;
    bool includeOther;
    std::string search;
  };
  ++m_state->active;
  const GioPtr<GTask> task(g_task_new(
      nullptr, m_state->queryCancel.get(),
      [](GObject*, GAsyncResult* result, gpointer) {
        auto& query = *static_cast<Query*>(g_task_get_task_data(G_TASK(result)));
        --query.state->active;
        const std::unique_ptr<ApplicationList> choices(
            static_cast<ApplicationList*>(g_task_propagate_pointer(G_TASK(result), nullptr))
        );
        if (choices && query.serial == query.state->querySerial) {
          query.callback(std::move(*choices));
        }
      },
      nullptr
  ));
  g_task_set_task_data(
      task.get(),
      new Query{m_state, m_state->querySerial, std::move(types), std::move(callback), includeOther, std::move(search)},
      [](gpointer data) { delete static_cast<Query*>(data); }
  );
  g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable* cancel) {
    const auto& query = *static_cast<Query*>(data);
    auto result = std::make_unique<ApplicationList>();
    const auto search = foldedDisplayText(query.search);
    const GioPtr<GAppInfo> defaultApp(
        query.types.size() == 1 ? g_app_info_get_default_for_type(query.types.front().c_str(), false) : nullptr
    );
    std::vector<std::set<std::string>> supporting;
    for (std::size_t index = 1; index < query.types.size() && !g_cancellable_is_cancelled(cancel); ++index) {
      auto* apps = g_app_info_get_all_for_type(query.types[index].c_str());
      std::set<std::string> ids;
      for (auto* item = apps; item; item = item->next) {
        if (const auto* id = g_app_info_get_id(G_APP_INFO(item->data))) {
          ids.emplace(id);
        }
      }
      g_list_free_full(apps, g_object_unref);
      supporting.push_back(std::move(ids));
    }
    std::set<std::string> seen;
    const auto append = [&](GList* applications, bool suggested) {
      for (auto* item = applications; item && !g_cancellable_is_cancelled(cancel); item = item->next) {
        auto* app = G_APP_INFO(item->data);
        const char* id = g_app_info_get_id(app);
        const char* name = g_app_info_get_display_name(app);
        if (!id
            || (suggested && std::ranges::any_of(supporting, [id](const auto& ids) { return !ids.contains(id); }))
            || !name
            || !g_app_info_should_show(app)
            || (!g_app_info_supports_files(app) && !g_app_info_supports_uris(app))
            || !seen.emplace(id).second) {
          continue;
        }
        if (!search.empty() && !foldedDisplayText(std::string(name) + " " + id).contains(search)) {
          continue;
        }
        result->choices.push_back(
            {id, name, readIconSource(g_app_info_get_icon(app)),
             std::shared_ptr<GAppInfo>(G_APP_INFO(g_object_ref(app)), ObjectUnref{}), suggested,
             defaultApp && g_app_info_equal(app, defaultApp.get())}
        );
      }
      g_list_free_full(applications, g_object_unref);
    };
    append(g_app_info_get_all_for_type(query.types.front().c_str()), true);
    if (query.includeOther && !g_cancellable_is_cancelled(cancel)) {
      append(g_app_info_get_all(), false);
    }
    std::ranges::sort(result->choices, [](const auto& left, const auto& right) {
      if (left.isDefault != right.isDefault) {
        return left.isDefault;
      }
      if (left.suggested != right.suggested) {
        return left.suggested;
      }
      const auto compared = g_utf8_collate(left.name.c_str(), right.name.c_str());
      return compared == 0 ? left.id < right.id : compared < 0;
    });
    // Search is applied before this UI bound, so every installed application remains discoverable.
    if (result->choices.size() > 128) {
      result->truncated = true;
      result->choices.resize(128);
    }
    g_task_return_pointer(task, result.release(), [](gpointer p) { delete static_cast<ApplicationList*>(p); });
  });
}

void luciole::FileLauncher::setDefault(
    std::string mime, ApplicationChoice application, std::function<void(std::string)> callback
) {
  struct Change {
    std::shared_ptr<State> state;
    ApplicationChoice application;
    std::string mime;
    std::function<void(std::string)> callback;
  };
  ++m_state->active;
  const GioPtr<GTask> task(g_task_new(
      nullptr, m_state->cancellable.get(),
      [](GObject*, GAsyncResult* result, gpointer) {
        auto& change = *static_cast<Change*>(g_task_get_task_data(G_TASK(result)));
        --change.state->active;
        GError* error = nullptr;
        const bool success = g_task_propagate_boolean(G_TASK(result), &error);
        std::string message = error ? error->message : success ? "" : "Could not change the default application.";
        g_clear_error(&error);
        if (!g_cancellable_is_cancelled(change.state->cancellable.get())) {
          change.callback(std::move(message));
        }
      },
      nullptr
  ));
  g_task_set_task_data(
      task.get(), new Change{m_state, std::move(application), std::move(mime), std::move(callback)},
      [](gpointer data) { delete static_cast<Change*>(data); }
  );
  g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable*) {
    const auto& change = *static_cast<Change*>(data);
    if (g_task_return_error_if_cancelled(task)) {
      return;
    }
    if (!change.application.app
        || change.mime.empty()
        || change.mime.contains('\0')
        || !g_utf8_validate(change.mime.c_str(), -1, nullptr)
        || g_content_type_is_unknown(change.mime.c_str())) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "This file type has no editable default.");
      return;
    }
    GError* error = nullptr;
    if (!g_app_info_set_as_default_for_type(change.application.app.get(), change.mime.c_str(), &error)) {
      if (error) {
        g_task_return_error(task, error);
      } else {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not change the default application.");
      }
      return;
    }
    g_task_return_boolean(task, true);
  });
}

void luciole::FileLauncher::openWith(const Location& location, const ApplicationChoice& application) {
  openWith(std::vector{location}, application);
}
void luciole::FileLauncher::openWith(const std::vector<Location>& locations, const ApplicationChoice& application) {
  State::dispatch(m_state, locations, application);
}
void luciole::FileLauncher::State::dispatch(
    const std::shared_ptr<State>& state, const std::vector<Location>& locations, ApplicationChoice application,
    GioPtr<GAppLaunchContext> supplied, std::uint64_t serial, gint64 started
) {
  if (!application.app || locations.empty()) {
    return;
  }
  try {
    std::string directory;
    if (G_IS_DESKTOP_APP_INFO(application.app.get())
        && g_desktop_app_info_get_boolean(G_DESKTOP_APP_INFO(application.app.get()), "Terminal")) {
      const auto parent = locations.front().parent();
      if (parent && parent->isNative() && std::ranges::all_of(locations, [&](const Location& item) {
            return item.parent() == parent;
          })) {
        directory = parent->nativePath().value_or("");
      }
    }
    auto prepared = terminalApplication(application.app.get(), state->terminalCommand, directory);
    application.app = std::shared_ptr<GAppInfo>(prepared.release(), g_object_unref);
  } catch (const FileErrorException& failure) {
    if (const auto changed = state->changed; changed && !g_cancellable_is_cancelled(state->cancellable.get())) {
      changed(failure.what());
    }
    return;
  }
  if (serial == 0) {
    serial = ++state->nextSerial;
  }
  if (started == 0) {
    started = g_get_monotonic_time();
  }
  struct LaunchWith {
    std::shared_ptr<State> state;
    std::shared_ptr<GAppInfo> app;
    GioPtr<GAppLaunchContext> context{g_app_launch_context_new()};
    std::vector<std::string> uris;
    GList* list = nullptr;
    std::uint64_t serial;
    gint64 started;
    ~LaunchWith() {
      g_signal_handlers_disconnect_by_data(context.get(), this);
      g_list_free(list);
    }
  };
  ++state->active;
  auto launch = [state, locations, application, serial, started](GioPtr<GAppLaunchContext> context) {
    auto* request = new LaunchWith{state, application.app, std::move(context), {}, nullptr, serial, started};
    g_signal_connect(
        request->context.get(), "launched",
        G_CALLBACK(+[](GAppLaunchContext*, GAppInfo*, GVariant* platform, gpointer data) {
          const auto& request = *static_cast<LaunchWith*>(data);
          gint32 pid = 0;
          if (platform) {
            (void)g_variant_lookup(platform, "pid", "i", &pid);
          }
          kLog.debug(
              "request {}: desktop handoff at {:.2f} ms (pid={}, not viewer readiness)", request.serial,
              static_cast<double>(g_get_monotonic_time() - request.started) / 1000.0, pid
          );
        }),
        request
    );
    request->uris.reserve(locations.size());
    for (const auto& location : locations) {
      request->uris.push_back(location.uri());
    }
    for (auto& uri : request->uris) {
      request->list = g_list_prepend(request->list, uri.data());
    }
    request->list = g_list_reverse(request->list);
    g_app_info_launch_uris_async(
        request->app.get(), request->list, request->context.get(), state->cancellable.get(),
        [](GObject* app, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<LaunchWith> request(static_cast<LaunchWith*>(data));
          GError* error = nullptr;
          const bool success = g_app_info_launch_uris_finish(G_APP_INFO(app), result, &error);
          kLog.debug(
              "request {}: GIO completion at {:.2f} ms ({})", request->serial,
              static_cast<double>(g_get_monotonic_time() - request->started) / 1000.0, success ? "success" : "failed"
          );
          --request->state->active;
          const std::string message = error ? error->message : success ? "" : "Could not open this file.";
          g_clear_error(&error);
          if (const auto changed = request->state->changed;
              changed && !g_cancellable_is_cancelled(request->state->cancellable.get())) {
            changed(message);
          }
        },
        request
    );
  };
  if (supplied) {
    launch(std::move(supplied));
  } else {
    State::context(state, application.id, std::move(launch));
  }
}
void luciole::FileLauncher::setChangedCallback(std::function<void(const std::string&)> callback) {
  m_state->changed = std::move(callback);
}
bool luciole::FileLauncher::busy() const { return m_state->active != 0; }
void luciole::FileLauncher::setTerminalCommand(std::string command) { m_state->terminalCommand = std::move(command); }
void luciole::FileLauncher::open(const Location& location) {
  const auto serial = ++m_state->nextSerial;
  const auto started = g_get_monotonic_time();
  kLog.debug("request {}: begin", serial);
  ++m_state->active;
  State::context(m_state, {}, [state = m_state, location, serial, started](GioPtr<GAppLaunchContext> context) {
    kLog.debug(
        "request {}: context ready at {:.2f} ms", serial, static_cast<double>(g_get_monotonic_time() - started) / 1000.0
    );
    queryHandler(
        location, cancellationToken(state->cancellable.get()),
        [state, location, context = std::move(context), serial, started](JobResult<GioPtr<GAppInfo>> result) mutable {
          --state->active;
          const auto application = result ? std::move(*result) : GioPtr<GAppInfo>{};
          if (g_cancellable_is_cancelled(state->cancellable.get())) {
            kLog.debug("request {}: GIO completion (cancelled handler lookup)", serial);
            return;
          }
          if (!application) {
            kLog.debug("request {}: GIO completion (handler lookup failed)", serial);
            if (const auto changed = state->changed) {
              changed(result ? "No application is associated with this file." : result.error().message);
            }
            return;
          }
          const char* id = g_app_info_get_id(application.get());
          kLog.debug(
              "request {}: handler={} at {:.2f} ms", serial, id ? id : "unknown",
              static_cast<double>(g_get_monotonic_time() - started) / 1000.0
          );
          auto choice = applicationChoice(application.get());
          State::dispatch(state, {location}, std::move(choice), std::move(context), serial, started);
        }
    );
  });
}

void luciole::FileLauncher::open(const std::vector<Location>& locations) {
  if (locations.empty()) {
    return;
  }
  if (locations.size() == 1) {
    open(locations.front());
    return;
  }
  struct Group {
    ApplicationChoice application;
    std::vector<Location> locations;
  };
  struct Batch {
    std::shared_ptr<State> state;
    std::vector<Location> locations;
    std::vector<std::optional<ApplicationChoice>> applications;
    std::size_t next = 0;
    std::size_t remaining;

    void dispatch() {
      std::vector<Group> groups;
      for (std::size_t index = 0; index < applications.size(); ++index) {
        if (!applications[index]) {
          continue;
        }
        const auto& application = *applications[index];
        auto found = std::ranges::find_if(groups, [&application](const Group& group) {
          return g_app_info_equal(group.application.app.get(), application.app.get());
        });
        if (found == groups.end()) {
          groups.push_back({application, {locations[index]}});
        } else {
          found->locations.push_back(locations[index]);
        }
      }
      for (auto& group : groups) {
        State::dispatch(state, group.locations, std::move(group.application));
      }
    }

    // Keep large selections out of the bounded I/O queue until a lookup slot is free.
    static void query(const std::shared_ptr<Batch>& batch) {
      const auto index = batch->next++;
      const auto& state = batch->state;
      ++state->active;
      queryHandler(
          batch->locations[index], cancellationToken(state->cancellable.get()),
          [batch, index](JobResult<GioPtr<GAppInfo>> result) {
            const auto& state = batch->state;
            --state->active;
            if (g_cancellable_is_cancelled(state->cancellable.get())) {
              return;
            }
            if (result) {
              batch->applications[index] = applicationChoice(result->get());
            } else if (const auto changed = state->changed) {
              changed(result.error().message);
            }
            if (g_cancellable_is_cancelled(state->cancellable.get())) {
              return;
            }
            if (--batch->remaining == 0) {
              batch->dispatch();
            } else if (batch->next < batch->locations.size()) {
              query(batch);
            }
          }
      );
    }
  };
  auto batch = std::make_shared<Batch>(
      m_state, locations, std::vector<std::optional<ApplicationChoice>>(locations.size()), 0, locations.size()
  );
  constexpr std::size_t parallelLookups = 4;
  for (std::size_t i = 0; i < std::min(locations.size(), parallelLookups); ++i) {
    Batch::query(batch);
  }
}
