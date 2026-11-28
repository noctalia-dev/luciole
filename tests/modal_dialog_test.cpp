#include "modal-dialog-server-protocol.h"
#include "platform/modal_dialog.h"
#include "test_check.h"
#include "xdg-dialog-v1-client-protocol.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <poll.h>
#include <print>
#include <string_view>
#include <sys/socket.h>
#include <wayland-client.h>

extern const wl_interface xdg_toplevel_interface;

namespace {
  using namespace std::chrono_literals;

  // A real protocol connection with a minimal role resource, independent of any compositor or renderer.
  class Peer {
    wl_display* m_server = wl_display_create();
    wl_display* m_client = nullptr;
    wl_registry* m_registry = nullptr;
    xdg_wm_dialog_v1* m_manager = nullptr;
    wl_proxy* m_role = nullptr;

  public:
    unsigned modal = 0, cleared = 0, destroyed = 0;
    Peer() {
      TEST_CHECK(m_server);
      wl_global_create(
          m_server, &xdg_toplevel_interface, 1, nullptr, [](wl_client* client, void*, std::uint32_t, std::uint32_t id) {
            auto* role = wl_resource_create(client, &xdg_toplevel_interface, 1, id);
            wl_resource_set_implementation(role, nullptr, nullptr, nullptr);
          }
      );
      wl_global_create(
          m_server, &xdg_wm_dialog_v1_interface, 1, this,
          [](wl_client* client, void* data, std::uint32_t, std::uint32_t id) {
            auto* manager = wl_resource_create(client, &xdg_wm_dialog_v1_interface, 1, id);
            static const struct xdg_wm_dialog_v1_interface implementation{
                .destroy = [](wl_client*, wl_resource* resource) { wl_resource_destroy(resource); },
                .get_xdg_dialog =
                    [](wl_client* client, wl_resource* resource, std::uint32_t id, wl_resource*) {
                      auto* dialog = wl_resource_create(client, &xdg_dialog_v1_interface, 1, id);
                      static const struct xdg_dialog_v1_interface implementation{
                          .destroy = [](wl_client*, wl_resource* resource) { wl_resource_destroy(resource); },
                          .set_modal = [](
                                           wl_client*, wl_resource* resource
                                       ) { ++static_cast<Peer*>(wl_resource_get_user_data(resource))->modal; },
                          .unset_modal = [](
                                             wl_client*, wl_resource* resource
                                         ) { ++static_cast<Peer*>(wl_resource_get_user_data(resource))->cleared; }
                      };
                      wl_resource_set_implementation(
                          dialog, &implementation, wl_resource_get_user_data(resource), [](wl_resource* resource) {
                            ++static_cast<Peer*>(wl_resource_get_user_data(resource))->destroyed;
                          }
                      );
                    }
            };
            wl_resource_set_implementation(manager, &implementation, data, nullptr);
          }
      );
      int sockets[2];
      TEST_CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
      TEST_CHECK(wl_client_create(m_server, sockets[0]));
      m_client = wl_display_connect_to_fd(sockets[1]);
      TEST_CHECK(m_client);
      m_registry = wl_display_get_registry(m_client);
      static const wl_registry_listener listener{
          .global =
              [](void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t) {
                auto& peer = *static_cast<Peer*>(data);
                if (std::string_view(interface) == "xdg_wm_dialog_v1") {
                  peer.m_manager =
                      static_cast<xdg_wm_dialog_v1*>(wl_registry_bind(registry, name, &xdg_wm_dialog_v1_interface, 1));
                } else if (std::string_view(interface) == "xdg_toplevel") {
                  peer.m_role = static_cast<wl_proxy*>(wl_registry_bind(registry, name, &xdg_toplevel_interface, 1));
                }
              },
          .global_remove = [](void*, wl_registry*, std::uint32_t) {}
      };
      wl_registry_add_listener(m_registry, &listener, this);
      until([&] { return m_manager && m_role; });
    }
    ~Peer() {
      xdg_wm_dialog_v1_destroy(m_manager);
      wl_proxy_destroy(m_role);
      wl_registry_destroy(m_registry);
      wl_display_disconnect(m_client);
      wl_display_destroy_clients(m_server);
      wl_display_destroy(m_server);
    }
    auto dialog() {
      return std::make_unique<luciole::ModalDialog>(
          m_client, xdg_wm_dialog_v1_get_xdg_dialog(m_manager, reinterpret_cast<xdg_toplevel*>(m_role))
      );
    }
    void receiveRequests() {
      TEST_CHECK(wl_display_flush(m_client) >= 0);
      TEST_CHECK(wl_event_loop_dispatch(wl_display_get_event_loop(m_server), 0) >= 0);
    }
    void disconnectServer() {
      wl_display_destroy_clients(m_server);
      TEST_CHECK(wl_display_dispatch(m_client) == -1);
    }
    void pump() {
      receiveRequests();
      wl_display_flush_clients(m_server);
      pollfd fd{wl_display_get_fd(m_client), POLLIN, 0};
      if (::poll(&fd, 1, 0) > 0) {
        TEST_CHECK(wl_display_dispatch(m_client) >= 0);
      }
    }
    template <typename Predicate> void until(Predicate done) {
      const auto deadline = std::chrono::steady_clock::now() + 2s;
      while (!done() && std::chrono::steady_clock::now() < deadline) {
        pump();
        ::poll(nullptr, 0, 1);
      }
      TEST_CHECK(done());
    }
  };

  void protocol() {
    Peer peer;
    auto dialog = peer.dialog();
    peer.until([&] { return peer.modal == 1; });
    unsigned replies = 0;
    const auto ready = [&](bool acknowledged) {
      TEST_CHECK(acknowledged && peer.cleared == 1 && !dialog->pending());
      ++replies;
    };
    dialog->complete(ready);
    dialog->complete(ready);
    TEST_CHECK(dialog->pending() && replies == 0);
    // Processing unset_modal alone is insufficient: the reply waits for delivery of the sync acknowledgment.
    peer.receiveRequests();
    TEST_CHECK(peer.cleared == 1 && replies == 0);
    peer.until([&] { return replies == 2; });
    dialog->complete(ready);
    TEST_CHECK(replies == 3 && !dialog->pending());
    dialog.reset();
    peer.until([&] { return peer.destroyed == 1; });
  }

  void teardown() {
    Peer peer;
    auto dialog = peer.dialog();
    unsigned replies = 0;
    dialog->complete([&](bool acknowledged) {
      TEST_CHECK(!acknowledged);
      ++replies;
    });
    peer.receiveRequests();
    dialog.reset();
    TEST_CHECK(replies == 1);
    peer.until([&] { return peer.destroyed == 1; });
    TEST_CHECK(replies == 1); // A queued acknowledgment cannot call into the destroyed dialog.
    dialog = peer.dialog();
    dialog->complete([&](bool acknowledged) {
      TEST_CHECK(acknowledged);
      dialog.reset();
      ++replies;
    });
    peer.until([&] { return replies == 2; });
    peer.until([&] { return peer.destroyed == 2; });
  }

  void disconnected() {
    Peer peer;
    auto dialog = peer.dialog();
    peer.until([&] { return peer.modal == 1; });
    peer.disconnectServer();
    bool replied = false;
    dialog->complete([&](bool acknowledged) {
      TEST_CHECK(!acknowledged);
      replied = true;
    });
    TEST_CHECK(replied && !dialog->pending());
  }
} // namespace

int main() {
  try {
    protocol();
    teardown();
    disconnected();
    std::println("modal dialog checks passed");
  } catch (const std::exception& error) {
    std::println(stderr, "modal dialog: {}", error.what());
    return 1;
  }
}
