#include <ship/Context.h>

#include "SohMenu.h"
#include <soh/Notification/Notification.h>
#include "SohGui.hpp"
#include "soh/OTRGlobals.h"
#include "soh/util.h"
#include <soh/Network/Sail/Sail.h>
#include <soh/Network/CrowdControl/CrowdControl.h>
#include <soh/Network/HiveShock/HiveShock.h>
#include "soh/SohGui/UIWidgets.hpp"

namespace SohGui {

extern std::shared_ptr<SohMenu> mSohMenu;
using namespace UIWidgets;

void SohMenu::AddMenuNetwork() {
    // Add Network Menu
    AddMenuEntry("Network", CVAR_SETTING("Menu.NetworkSidebarSection"));
    WidgetPath path;

    // Sail
    path = { "Network", "Sail", SECTION_COLUMN_1 };
    AddSidebarEntry("Network", path.sidebarName, 3);

    AddWidget(path,
              "Sail is a networking protocol designed to facilitate remote "
              "control of the Ship of Harkinian client. It is intended to "
              "be utilized alongside a Sail server, for which we provide a "
              "few straightforward implementations on our GitHub. The current "
              "implementations available allow integration with Twitch chat "
              "and SAMMI Bot, feel free to contribute your own!\n"
              "\n"
              "Click this button to copy the link to the Sail Github "
              "page to your clipboard.",
              WIDGET_TEXT);
    AddWidget(path, ICON_FA_CLIPBOARD "##Sail", WIDGET_BUTTON)
        .Callback([](WidgetInfo& info) {
            ImGui::SetClipboardText("https://github.com/HarbourMasters/sail");
            Notification::Emit({
                .message = "Copied to clipboard",
            });
        })
        .Options(ButtonOptions().Tooltip("https://github.com/HarbourMasters/sail"));
    AddWidget(path, "Host & Port", WIDGET_CUSTOM).CustomFunction([](WidgetInfo& info) {
        ImGui::BeginDisabled(Sail::Instance->isEnabled || CVarGetInteger(CVAR_SETTING("DisableChanges"), 0));
        ImGui::Text("%s", info.name.c_str());
        CVarInputString("##HostSail", CVAR_REMOTE_SAIL("Host"),
                        InputOptions()
                            .Color(THEME_COLOR)
                            .PlaceholderText("127.0.0.1")
                            .DefaultValue("127.0.0.1")
                            .Size(ImVec2(ImGui::GetFontSize() * 15, 0))
                            .LabelPosition(LabelPositions::None));
        ImGui::SameLine();
        ImGui::Text(":");
        ImGui::SameLine();
        CVarInputInt("##PortSail", CVAR_REMOTE_SAIL("Port"),
                     InputOptions()
                         .Color(THEME_COLOR)
                         .PlaceholderText("43384")
                         .DefaultValue("43384")
                         .Size(ImVec2(ImGui::GetFontSize() * 5, 0))
                         .LabelPosition(LabelPositions::None));
        ImGui::EndDisabled();
    });
    AddWidget(path, "Enable##Sail", WIDGET_BUTTON)
        .PreFunc([](WidgetInfo& info) {
            std::string host = CVarGetString(CVAR_REMOTE_SAIL("Host"), "127.0.0.1");
            uint16_t port = CVarGetInteger(CVAR_REMOTE_SAIL("Port"), 43384);
            info.options->disabled = !(!SohUtils::IsStringEmpty(host) && port > 1024 && port < 65535);
            if (Sail::Instance->isEnabled) {
                info.name = "Disable##Sail";
            } else {
                info.name = "Enable##Sail";
            }
        })
        .Callback([](WidgetInfo& info) {
            if (Sail::Instance->isEnabled) {
                CVarClear(CVAR_REMOTE_SAIL("Enabled"));
                Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                Sail::Instance->Disable();
            } else {
                CVarSetInteger(CVAR_REMOTE_SAIL("Enabled"), 1);
                Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                Sail::Instance->Enable();
            }
        });
    AddWidget(path, "Connecting...##Sail", WIDGET_TEXT).PreFunc([](WidgetInfo& info) {
        info.isHidden = !Sail::Instance->isEnabled;
        if (Sail::Instance->isConnected) {
            info.name = "Connected##Sail";
        } else {
            info.name = "Connecting...##Sail";
        }
    });

    path.sidebarName = "Crowd Control";
    AddSidebarEntry("Network", path.sidebarName, 3);
    path.column = SECTION_COLUMN_1;

    AddWidget(path, "About Crowd Control", WIDGET_SEPARATOR_TEXT);
    AddWidget(path,
              "Crowd Control is a platform that allows viewers to interact "
              "with a streamer's game in real time.\n"
              "\n"
              "Please head over to www.crowdcontrol.live for more information!",
              WIDGET_TEXT);

    AddWidget(path, "Connect to Crowd Control", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Host & Port", WIDGET_CUSTOM).CustomFunction([](WidgetInfo& info) {
        ImGui::BeginDisabled(CrowdControl::Instance->isEnabled || CVarGetInteger(CVAR_SETTING("DisableChanges"), 0));
        ImGui::Text("%s", info.name.c_str());
        CVarInputString("##HostCrowdControl", CVAR_REMOTE_CROWD_CONTROL("Host"),
                        InputOptions()
                            .Color(THEME_COLOR)
                            .PlaceholderText("127.0.0.1")
                            .DefaultValue("127.0.0.1")
                            .Size(ImVec2(ImGui::GetFontSize() * 15, 0))
                            .LabelPosition(LabelPositions::None));
        ImGui::SameLine();
        ImGui::Text(":");
        ImGui::SameLine();
        CVarInputInt("##PortCrowdControl", CVAR_REMOTE_CROWD_CONTROL("Port"),
                     InputOptions()
                         .Color(THEME_COLOR)
                         .PlaceholderText("43384")
                         .DefaultValue("43384")
                         .Size(ImVec2(ImGui::GetFontSize() * 5, 0))
                         .LabelPosition(LabelPositions::None));
        ImGui::EndDisabled();
    });
    AddWidget(path, "Enable##CrowdControl", WIDGET_BUTTON)
        .PreFunc([](WidgetInfo& info) {
            std::string host = CVarGetString(CVAR_REMOTE_CROWD_CONTROL("Host"), "127.0.0.1");
            uint16_t port = CVarGetInteger(CVAR_REMOTE_CROWD_CONTROL("Port"), 43384);
            info.options->disabled = !(!SohUtils::IsStringEmpty(host) && port > 1024 && port < 65535);
            if (CrowdControl::Instance->isEnabled) {
                info.name = "Disable##CrowdControl";
            } else {
                info.name = "Enable##CrowdControl";
            }
        })
        .Callback([](WidgetInfo& info) {
            if (CrowdControl::Instance->isEnabled) {
                CVarClear(CVAR_REMOTE_CROWD_CONTROL("Enabled"));
                Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                CrowdControl::Instance->Disable();
            } else {
                CVarSetInteger(CVAR_REMOTE_CROWD_CONTROL("Enabled"), 1);
                Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                CrowdControl::Instance->Enable();
            }
        });
    AddWidget(path, "Connecting...", WIDGET_TEXT).PreFunc([](WidgetInfo& info) {
        info.isHidden = !CrowdControl::Instance->isEnabled;
        if (CrowdControl::Instance->isConnected) {
            info.name = "Connected";
        } else {
            info.name = "Connecting...";
        }
    });
    AddWidget(path, "Additional Settings", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Enemy Name Tags", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_REMOTE_CROWD_CONTROL("EnemyNameTags"))
        .RaceDisable(true)
        .Options(CheckboxOptions().Tooltip(
            "When viewers spawn enemies, the enemy will have a name tag above them with the viewer's name."));
    AddWidget(path, "Spawned Enemies Ignored Ingame", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_REMOTE_CROWD_CONTROL("SpawnedEnemiesIgnoredIngame"))
        .RaceDisable(true)
        .Options(CheckboxOptions().Tooltip("Enemies spawned by CrowdControl won't be considered for \"clear enemy "
                                           "rooms\", so they don't need to be killed to complete these rooms."));
    path.sidebarName = "HiveShock";
    AddSidebarEntry("Network", path.sidebarName, 3);
    path.column = SECTION_COLUMN_1;

    AddWidget(path, "About HiveShock", WIDGET_SEPARATOR_TEXT);
    AddWidget(path,
              "HiveShock is a lightweight TCP server built into this client: an external "
              "app/controller connects to it and sends JSON commands to spawn enemies or apply "
              "effects (heal, damage, buffs, teleports, etc.) on your live game.\n"
              "\n"
              "Unlike Sail/Crowd Control, HiveShock listens for an incoming connection instead "
              "of dialing out. It only accepts connections from this computer (127.0.0.1), so run "
              "the controller app on the same machine and point it at the address shown below "
              "once enabled.",
              WIDGET_TEXT);

    AddWidget(path, "Port", WIDGET_CUSTOM).CustomFunction([](WidgetInfo& info) {
        ImGui::BeginDisabled(HiveShock::Instance->IsEnabled() || CVarGetInteger(CVAR_SETTING("DisableChanges"), 0));
        ImGui::Text("%s", info.name.c_str());
        CVarInputInt("##PortHiveShock", CVAR_REMOTE_HIVESHOCK("Port"),
                     InputOptions()
                         .Color(THEME_COLOR)
                         .PlaceholderText("43000")
                         .DefaultValue("43000")
                         .Size(ImVec2(ImGui::GetFontSize() * 5, 0))
                         .LabelPosition(LabelPositions::None));
        ImGui::EndDisabled();
    });
    AddWidget(path, "Enable##HiveShock", WIDGET_BUTTON)
        .PreFunc([](WidgetInfo& info) {
            uint16_t port = CVarGetInteger(CVAR_REMOTE_HIVESHOCK("Port"), 43000);
            info.options->disabled = !(port > 1024 && port < 65535);
            if (HiveShock::Instance->IsEnabled()) {
                info.name = "Disable##HiveShock";
            } else {
                info.name = "Enable##HiveShock";
            }
        })
        .Callback([](WidgetInfo& info) {
            if (HiveShock::Instance->IsEnabled()) {
                CVarClear(CVAR_REMOTE_HIVESHOCK("Enabled"));
                Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                HiveShock::Instance->Disable();
            } else {
                CVarSetInteger(CVAR_REMOTE_HIVESHOCK("Enabled"), 1);
                Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                HiveShock::Instance->Enable();
            }
        });
    AddWidget(path, "Enemy Load Limit: %d", WIDGET_CVAR_SLIDER_INT)
        .CVar(CVAR_REMOTE_HIVESHOCK("MaxLoad"))
        .Options(IntSliderOptions()
                     .Min(4)
                     .Max(30)
                     .DefaultValue(14)
                     .Format("%d")
                     .Tooltip("How many enemies spawned through HiveShock can be alive at once. Each enemy uses "
                              "1 to 4 points (bats and cuccos 1, Dark Link and Iron Knuckles 4). Extra spawns are "
                              "never discarded: they wait in a queue and enter as enemies are defeated."));
    AddWidget(path, "HiveShock Status", WIDGET_TEXT).PreFunc([](WidgetInfo& info) {
        info.isHidden = !HiveShock::Instance->IsEnabled();
        if (HiveShock::Instance->HasClient()) {
            info.name = "Client connected";
        } else if (HiveShock::Instance->IsListening()) {
            info.name = "Listening on " + HiveShock::Instance->GetListenEndpoint();
        } else {
            info.name = "Starting...";
        }
    });
    AddWidget(path, "Defeat Alive Enemies When Link Dies", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_REMOTE_HIVESHOCK("DefeatOnDeath"))
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip(
            "When Link dies, the HiveShock enemies that are still alive are removed and counted as defeated, so you "
            "never get stuck with enemies you cannot kill. The spawns still waiting in the queue carry on as usual. "
            "When off, the living enemies follow Link back into the scene after he dies."));
    AddWidget(path, "Enemy Counter", WIDGET_TEXT).PreFunc([](WidgetInfo& info) {
        info.isHidden = !HiveShock::Instance->IsEnabled();
        const HiveShock::Stats stats = HiveShock::Instance->GetStats();
        info.name = "Spawned " + std::to_string(stats.spawned) + " / " + std::to_string(stats.total) +
                    "  |  Alive " + std::to_string(stats.alive) + "  |  Waiting " + std::to_string(stats.waiting) +
                    "  |  Defeated " + std::to_string(stats.defeated);
    });
    AddWidget(path, "Reset Counter", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip("Starts counting from now. Enemies that are alive or waiting stay counted."))
        .PreFunc([](WidgetInfo& info) { info.isHidden = !HiveShock::Instance->IsEnabled(); })
        .Callback([](WidgetInfo& info) { HiveShock::Instance->ResetStats(); });
    AddWidget(path, "Toggle Enemy Counter Window", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("HiveShockCounter"))
        .RaceDisable(false)
        .WindowName("HiveShock Counter")
        .Options(WindowButtonOptions().Tooltip(
            "Shows a small on-screen counter of the enemies sent through HiveShock: spawned out of the total "
            "received, alive, waiting in the queue and defeated."));

    path.sidebarName = "Anchor";
    AddSidebarEntry("Network", path.sidebarName, 2);
}

} // namespace SohGui
