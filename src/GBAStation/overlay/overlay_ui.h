// Copyright 2026 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>

namespace SwitchFrontend::OverlayUI {

constexpr int StateSlotCount = 10;

enum class Action {
    None,
    Resume,
    Reset,
    Exit,
    SaveStateSlot1,
    SaveStateSlot2,
    SaveStateSlot3,
    SaveStateSlot4,
    SaveStateSlot5,
    SaveStateSlot6,
    SaveStateSlot7,
    SaveStateSlot8,
    SaveStateSlot9,
    SaveStateSlot10,
    LoadStateSlot1,
    LoadStateSlot2,
    LoadStateSlot3,
    LoadStateSlot4,
    LoadStateSlot5,
    LoadStateSlot6,
    LoadStateSlot7,
    LoadStateSlot8,
    LoadStateSlot9,
    LoadStateSlot10,
    DisplaySettingsChanged,
    CustomLayoutChanged,
    CustomLayoutCommitted,
    FastForwardMultiplierChanged,
    OverlaySettingsChanged,
    OverlaySettingsCommitted,
    SyncOverlaySettings,
    SyncDisplaySettings,
};

enum class ToastCorner {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

bool IsSaveStateAction(Action action);
bool IsLoadStateAction(Action action);
int GetStateSlotForAction(Action action);

void SetVisible(bool visible);
Action Render(int display_w, int display_h);

void SetGameTitle(std::string title);
void ShowToast(std::string message, ToastCorner corner = ToastCorner::BottomRight);
bool HasTransientContent();
std::string GetToast();

using SlotOccupiedFn = std::function<bool(int slot)>;
void SetSlotOccupiedCallback(SlotOccupiedFn callback);
bool IsSlotOccupied(int slot);
std::string GetGameTitle();

struct NavInput {
    bool up;
    bool down;
    bool left;
    bool right;
    bool accept;
    bool cancel;
};
void FeedNav(const NavInput& nav);

} // namespace SwitchFrontend::OverlayUI
