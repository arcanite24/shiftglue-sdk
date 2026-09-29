/**
 * @file        kernel/xam/ui_provider.h
 * @brief       Host implementations of the XAM system dialogs
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rex::kernel::xam {

// Draws the XAM system dialogs a title opens (XamShowMessageBoxUI,
// XamShowKeyboardUI) in the host's own UI instead of the built-in ImGui ones.
// The dispatcher keeps the XN_SYS_UI notifications, the XamIsUIActive count
// and the overlapped completion; a provider only shows the dialog and reports
// how it was closed. Both methods are called on the UI thread and must call
// `done` exactly once, on the UI thread.
class XamUiProvider {
 public:
  virtual ~XamUiProvider() = default;

  // `done(button)` with the chosen button index, or kCancelled for B/Escape.
  static constexpr uint32_t kCancelled = UINT32_MAX;
  virtual void ShowMessageBox(const std::string& title, const std::string& text,
                              const std::vector<std::string>& buttons, uint32_t default_button,
                              std::function<void(uint32_t button)> done) = 0;

  // `done(true, text)` when accepted, `done(false, {})` when cancelled.
  virtual void ShowKeyboard(const std::string& title, const std::string& description,
                            const std::string& default_text, size_t max_length,
                            std::function<void(bool accepted, std::string text)> done) = 0;

  // The achievements list (XamShowAchievementsUI); `done()` when closed.
  virtual void ShowAchievements(std::function<void()> done) { done(); }
};

// Installs the provider (nullptr restores the built-in dialogs). The caller
// keeps it alive until it is replaced.
void SetXamUiProvider(XamUiProvider* provider);
XamUiProvider* GetXamUiProvider();

}  // namespace rex::kernel::xam
