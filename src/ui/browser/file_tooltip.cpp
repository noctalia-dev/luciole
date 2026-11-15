#include "ui/browser/file_tooltip.h"

#include "fs/file_query.h"
#include "fs/gio_ptr.h"

#include <format>
#include <utility>

namespace {
  std::string modified(std::optional<std::uint64_t> seconds) {
    if (!seconds || *seconds > static_cast<std::uint64_t>(G_MAXINT64)) {
      return "Unknown";
    }
    const std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> date(
        g_date_time_new_from_unix_local(static_cast<gint64>(*seconds)), &g_date_time_unref
    );
    const luciole::GString text(date ? g_date_time_format(date.get(), "%Y-%m-%d %H:%M:%S") : nullptr);
    return text ? text.get() : "Unknown";
  }
} // namespace

bool luciole::sameTooltipFile(const FileTooltipTarget& a, const FileTooltipTarget& b) {
  return a.context == b.context
      && a.entry.id == b.entry.id
      && a.entry.location() == b.entry.location()
      && a.entry.identity() == b.entry.identity();
}

struct luciole::FileTooltipData::State {
  std::function<void()> changed;
  std::optional<FileTooltipTarget> target;
  FileDetails details;
  CancelToken token;
  std::uint64_t serial = 0;
  bool pending = false;
  std::string error;
};

luciole::FileTooltipData::FileTooltipData(std::function<void()> changed) : m_state(std::make_shared<State>()) {
  m_state->changed = std::move(changed);
}
luciole::FileTooltipData::~FileTooltipData() { clear(); }

void luciole::FileTooltipData::clear() {
  m_state->token.cancel();
  ++m_state->serial;
  m_state->target.reset();
  m_state->pending = false;
}

void luciole::FileTooltipData::show(const FileTooltipTarget& target) {
  if (m_state->target && sameTooltipFile(*m_state->target, target)) {
    return;
  }
  clear();
  m_state->target = target;
  m_state->error.clear();
  m_state->details = {
      .type = target.entry.type,
      .displayName = target.entry.displayName(),
      .typeDescription = target.typeLabel,
      .size = target.entry.size,
      .modifiedSeconds = target.entry.modifiedSeconds,
      .identity = target.entry.identity(),
      .linkTarget = std::nullopt,
  };
  refresh();
}

void luciole::FileTooltipData::refresh() {
  auto& state = *m_state;
  if (!state.target || state.pending) {
    return;
  }
  state.pending = true;
  state.token = CancelToken{};
  const auto serial = state.serial;
  queryFileDetails(state.target->entry.location(), state.token, [weak = std::weak_ptr(m_state), serial](auto reply) {
    const auto state = weak.lock();
    if (!state || !state->target || state->serial != serial) {
      return;
    }
    state->pending = false;
    const auto identity = state->target->entry.identity();
    if (!reply || (identity && reply->identity != identity)) {
      state->details = {};
      state->error = reply ? "This item changed" : "Metadata unavailable";
    } else {
      state->details = std::move(*reply);
      state->error.clear();
    }
    state->changed();
  });
}

std::vector<TooltipRow> luciole::FileTooltipData::rows() const {
  const auto& state = *m_state;
  if (!state.target) {
    return {};
  }
  std::vector<TooltipRow> rows{{"Name", state.details.displayName.value_or(state.target->entry.displayName())}};
  if (!state.error.empty()) {
    rows.push_back({"Status", state.error});
    return rows;
  }
  rows.push_back({"Type", state.details.typeDescription.value_or("Unknown")});
  if (state.details.type != FileType::Directory) {
    rows.push_back({"Size", state.details.size ? std::format("{} bytes", *state.details.size) : "Unknown"});
  }
  rows.push_back({"Modified", modified(state.details.modifiedSeconds)});
  if (state.details.linkTarget) {
    rows.push_back({"Target", *state.details.linkTarget, TextEllipsize::Middle});
  }
  return rows;
}
