#pragma once

#include <ship/window/gui/GuiWindow.h>

// Small always-on-top counter of the enemies sent through HiveShock (spawned / alive / waiting / defeated).
// Shown while HiveShock is enabled and "gWindows.HiveShockCounter" is on (Network > HiveShock).
class HiveShockCounterWindow final : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

    void InitElement() override{};
    void DrawElement() override{};
    void Draw() override;
    void UpdateElement() override{};
};
