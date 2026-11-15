#include "event_loop/glib_poll_source.h"
#include "fs/file_query.h"
#include "fs/gio_interop.h"
#include "test_check.h"
#include "ui/browser/file_tooltip.h"
#include "ui/controls/label.h"
#include "ui/controls/tooltip_presenter.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <sys/stat.h>
#include <thread>

namespace {
  namespace fs = std::filesystem;
  class MetricsRenderer final : public Renderer {
  public:
    TextMetrics measureText(
        std::string_view text, float size, const TextFont&, float width, int, TextAlign, TextEllipsize, bool
    ) override {
      const float natural = static_cast<float>(text.size()) * size * 0.5F;
      return {
          .width = width > 0 ? std::min(width, natural) : natural, .bottom = size, .capHeight = size, .lineCount = 1
      };
    }
    TextMetrics measureFont(float size, const TextFont&) override { return {.bottom = size, .capHeight = size}; }
    TextMetrics measureGlyph(char32_t, float size) override { return {.width = size, .bottom = size}; }
    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, const TextFont&
    ) override {}
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, const TextFont&
    ) override {}
    TextureManager& textureManager() override { std::abort(); }
    float renderScale() const noexcept override { return 1; }
  };
  void wait(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  void drain() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    wait([&] { return std::chrono::steady_clock::now() >= deadline; });
  }
  bool hasText(const Node& node, const std::string& text) {
    if (!node.visible()) {
      return false;
    }
    if (const auto* label = dynamic_cast<const Label*>(&node); label && label->text() == text) {
      return true;
    }
    return std::ranges::any_of(node.children(), [&](const auto& child) { return hasText(*child, text); });
  }
  void presenterChecks() {
    MetricsRenderer renderer;
    Node scene;
    scene.setSize(480, 240);
    auto area = std::make_unique<InputArea>();
    area->setSize(100, 32);
    std::string text = "<first>";
    bool empty = false;
    area->setTooltipProvider(
        [&] -> TooltipContent {
          if (empty) {
            return {};
          }
          std::vector<TooltipRow> rows;
          rows.push_back({"<key>", text});
          return rows;
        },
        std::chrono::milliseconds(100)
    );
    scene.addChild(std::move(area));
    int updates = 0;
    TooltipPresenter presenter([&] { ++updates; });
    const auto sync = [&] { presenter.sync(renderer, scene, 10, 10, {480, 240}); };
    const auto wake = [&] {
      const int before = updates;
      wait([&] {
        TimerManager::instance().tick();
        return updates > before;
      });
    };
    presenter.resume();
    sync();
    TEST_CHECK(!presenter.visible());
    wake();
    sync();
    TEST_CHECK(presenter.visible() && hasText(presenter, "<first>") && hasText(presenter, "<key>"));
    TEST_CHECK(presenter.width() <= 480 && presenter.height() <= 240);
    text = "second";
    wake();
    sync();
    TEST_CHECK(presenter.visible() && hasText(presenter, "second") && !hasText(presenter, "<first>"));
    empty = true;
    wake();
    sync();
    TEST_CHECK(!presenter.visible());
    empty = false;
    wake();
    sync();
    TEST_CHECK(presenter.visible());
    presenter.dismiss();
    const int before = updates;
    scene.removeChild(scene.children().front().get()).reset();
    drain();
    TimerManager::instance().tick();
    TEST_CHECK(updates == before && !presenter.visible());
    presenter.resume();
    sync();
    TEST_CHECK(!presenter.visible());
  }
  std::string value(const luciole::FileTooltipData& data, const std::string& key) {
    for (const auto& row : data.rows()) {
      if (row.key == key) {
        return row.value;
      }
    }
    return {};
  }
  luciole::FileTooltipTarget target(const fs::path& path) {
    luciole::FileTooltipTarget result{
        .context = "page:column:context",
        .entry =
            {.id = 1,
             .place = luciole::Location::fromPath(path.native()),
             .name = path.filename().native(),
             .type = luciole::FileType::Regular,
             .size = std::nullopt,
             .modifiedSeconds = std::nullopt},
        .typeLabel = "Unknown",
    };
    result.entry.setDisplayName("<literal> café");
    return result;
  }
  void cursorChecks(const fs::path& root) {
    MetricsRenderer renderer;
    Node scene;
    scene.setSize(600, 400);
    TooltipPresenter presenter([] {});
    const auto file = target(root / "literal <name> café.txt");
    float x = 80, y = 60;
    const auto sync = [&] { presenter.sync(renderer, scene, x, y, {600, 400}, file); };
    const auto show = [&] {
      presenter.resume();
      sync();
      wait([&] {
        TimerManager::instance().tick();
        sync();
        return presenter.visible();
      });
    };
    show();
    TEST_NEAR(presenter.x(), 92.0F, 0.5F);
    TEST_NEAR(presenter.y(), 76.0F, 0.5F);
    x = 580;
    y = 380;
    sync();
    TEST_CHECK(!presenter.visible());
    show();
    TEST_CHECK(presenter.x() >= 8 && presenter.x() + presenter.width() <= 592);
    TEST_CHECK(presenter.y() >= 8 && presenter.y() + presenter.height() < y);
    presenter.dismiss();
  }
  void checks(const fs::path& root) {
    const auto a = root / "literal <name> café.txt", b = root / "another.txt";
    std::ofstream(a) << "abc";
    std::ofstream(b) << "123456789";
    int replies = 0;
    luciole::FileTooltipData data([&] { ++replies; });
    auto first = target(a);
    data.show(first);
    TEST_CHECK(value(data, "Name") == "<literal> café" && value(data, "Size") == "Unknown");
    wait([&] { return replies == 1; });
    TEST_CHECK(value(data, "Name") == a.filename().native());
    TEST_CHECK(value(data, "Size") == "3 bytes" && value(data, "Modified") != "Unknown");
    std::ofstream(a, std::ios::app) << "def";
    data.refresh();
    wait([&] { return replies == 2; });
    TEST_CHECK(value(data, "Size") == "6 bytes");
    auto other = target(b);
    TEST_CHECK(!luciole::sameTooltipFile(first, other));
    auto recycled = first;
    recycled.entry.id = 2;
    TEST_CHECK(!luciole::sameTooltipFile(first, recycled));
    recycled = first;
    recycled.context = "another page";
    TEST_CHECK(!luciole::sameTooltipFile(first, recycled));
    data.show(first);
    data.refresh();
    data.show(other);
    wait([&] { return value(data, "Size") == "9 bytes"; });
    drain();
    TEST_CHECK(value(data, "Name") == "another.txt");
    const auto before = replies;
    data.refresh();
    data.clear();
    drain();
    TEST_CHECK(replies == before && data.rows().empty());
    {
      luciole::FileTooltipData retired([&] { TEST_CHECK(false); });
      retired.show(first);
    }
    drain();
    const auto folder = root / "folder";
    fs::create_directory(folder);
    data.show(target(folder));
    wait([&] { return value(data, "Type") != "Unknown"; });
    TEST_CHECK(value(data, "Size").empty());
    const auto link = root / "link";
    fs::create_symlink("missing <literal>", link);
    data.show(target(link));
    wait([&] { return !value(data, "Target").empty(); });
    TEST_CHECK(value(data, "Target") == "missing <literal>");
    const auto fifo = root / "fifo";
    TEST_CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
    data.show(target(fifo));
    const int fifoBefore = replies;
    wait([&] { return replies > fifoBefore; });
    fs::remove(b);
    data.show(other);
    wait([&] { return value(data, "Status") == "Metadata unavailable"; });
    TEST_CHECK(value(data, "Size").empty() && value(data, "Modified").empty());
    auto stale = first;
    stale.entry.setIdentity(luciole::FileIdentity{"filesystem", "obsolete"});
    data.show(stale);
    wait([&] { return value(data, "Status") == "This item changed"; });
    TEST_CHECK(value(data, "Size").empty());
  }
} // namespace

int main() {
  const luciole::GString directory(g_dir_make_tmp("luciole-tooltip-test-XXXXXX", nullptr));
  TEST_CHECK(directory != nullptr);
  checks(directory.get());
  presenterChecks();
  cursorChecks(directory.get());
  fs::remove_all(directory.get());
  std::println("file tooltip checks passed");
}
