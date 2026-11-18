#pragma once

#include "config/settings.h"
#include "ui/controls/flex.h"

#include <functional>
#include <map>

class Button;
class Label;
class ScrollView;

class SettingsDialog final : public Flex {
public:
  SettingsDialog(
      std::function<void(luciole::OverrideEdit)> change, std::function<void()> reset, std::function<void()> close,
      std::function<void()> clearThumbnails
  );
  void sync(
      const luciole::BrowserSettings& settings, const luciole::BrowserSettings& inherited, bool busy,
      const std::string& error, bool clearingThumbnails = false
  );
  void reveal(const Node* node);
  void setDialogWidth(float dialogWidth);
  // The page list; initial keyboard focus goes here rather than to the header actions.
  Node* navigation() const;

private:
  template <class T> using Choices = std::vector<std::pair<std::string, T>>;
  void selectPage(std::size_t selected);
  void section(const std::string& title, const std::string& glyph);
  // Starts a titled card on the current page; following rows go into it.
  void card(const std::string& title);
  template <class T>
  void addChoice(
      luciole::PreferenceKey<T> key, const std::string& title, const Choices<T>& choices, std::string subtitle = {}
  );
  void addTerminal();
  void addBool(luciole::PreferenceKey<bool> key, const std::string& title);
  void addNumber(
      luciole::PreferenceKey<int> key, const std::string& title, const std::vector<int>& values,
      std::string suffix = "px"
  );
  // A `minimum` to `maximum` slider shown as a percentage. `onPreview` runs for every
  // value while the user drags, before the override is persisted on release.
  void addFraction(
      luciole::PreferenceKey<double> key, const std::string& title, double minimum, double maximum, double step,
      std::string subtitle = {}, std::function<void(double)> onPreview = nullptr
  );
  template <class T>
  void addRow(
      luciole::PreferenceKey<T> key, const std::string& title, std::function<void(const T&)> show,
      std::unique_ptr<Node> control, std::string subtitle = {}
  );
  template <class T> void send(luciole::PreferenceKey<T> key, std::optional<T> value);
  void setStatus(const std::string& text, bool error);
  // Reset asks for a second click; leaving the button or the page disarms it.
  void onResetClicked();
  void disarmReset();
  std::function<void(luciole::OverrideEdit)> m_change;
  std::function<void()> m_resetAll;
  luciole::BrowserSettings m_current;
  luciole::BrowserSettings m_inherited;
  Label* m_title = nullptr;
  Button* m_reset = nullptr;
  bool m_resetArmed = false;
  ScrollView* m_body = nullptr;
  Label* m_status = nullptr;
  Button* m_clearThumbnails = nullptr;
  Flex* m_navigation = nullptr;
  ScrollView* m_navigationScroll = nullptr;
  Flex* m_section = nullptr;
  std::vector<Flex*> m_pages;
  std::vector<std::string> m_pageTitles;
  std::vector<Button*> m_navButtons;
  // Shows m_current's value in a row's control.
  std::map<luciole::BrowserPreferenceId, std::function<void()>> m_rows;
};
