#include "HiveShockWindow.h"

#include "HiveShock.h"
#include "soh/cvar_prefixes.h"

#include <imgui.h>
#include <libultraship/bridge/consolevariablebridge.h>

void HiveShockCounterWindow::Draw() {
    if (!CVarGetInteger(CVAR_WINDOW("HiveShockCounter"), 0)) {
        return;
    }
    if (HiveShock::Instance == nullptr || !HiveShock::Instance->IsEnabled()) {
        return;
    }

    const HiveShock::Stats stats = HiveShock::Instance->GetStats();

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);

    ImGui::Begin("HiveShockCounter", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
    ImGui::TextColored(ImVec4(0.92f, 0.63f, 0.04f, 1.0f), "HiveShock");
    ImGui::Text("Spawned:  %d / %d", stats.spawned, stats.total);
    ImGui::Text("Alive:    %d", stats.alive);
    ImGui::Text("Waiting:  %d", stats.waiting);
    ImGui::Text("Defeated: %d", stats.defeated);
    if (stats.eliteBankMax > 0) {
        ImGui::Text("Elite:    %d / %d  (%d waiting)", stats.eliteBank, stats.eliteBankMax, stats.eliteWaiting);
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(1);
}
