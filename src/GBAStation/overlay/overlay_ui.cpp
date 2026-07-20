// Copyright 2026 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "GBAStation/overlay/overlay_ui.h"

#include <mutex>
#include <chrono>
#include <utility>

namespace SwitchFrontend::OverlayUI {
namespace {

std::mutex state_mutex;
std::string game_title;
std::string toast_message;
std::chrono::steady_clock::time_point toast_expiry{};
SlotOccupiedFn slot_occupied;

bool IsActionInRange(Action action, Action first, Action last) {
    return static_cast<int>(action) >= static_cast<int>(first) &&
           static_cast<int>(action) <= static_cast<int>(last);
}

} // namespace

bool IsSaveStateAction(Action action) {
    return IsActionInRange(action, Action::SaveStateSlot1, Action::SaveStateSlot10);
}

bool IsLoadStateAction(Action action) {
    return IsActionInRange(action, Action::LoadStateSlot1, Action::LoadStateSlot10);
}

int GetStateSlotForAction(Action action) {
    if (IsSaveStateAction(action)) {
        return static_cast<int>(action) - static_cast<int>(Action::SaveStateSlot1) + 1;
    }
    if (IsLoadStateAction(action)) {
        return static_cast<int>(action) - static_cast<int>(Action::LoadStateSlot1) + 1;
    }
    return 0;
}

void SetVisible(bool) {}

Action Render(int, int) {
    return Action::None;
}

void SetGameTitle(std::string title) {
    std::lock_guard lock{state_mutex};
    game_title = std::move(title);
}

void ShowToast(std::string message, ToastCorner) {
    std::lock_guard lock{state_mutex};
    toast_message = std::move(message);
    toast_expiry = std::chrono::steady_clock::now() + std::chrono::seconds(3);
}

bool HasTransientContent() {
    return !GetToast().empty();
}

std::string GetToast() {
    std::lock_guard lock{state_mutex};
    if (toast_message.empty() || std::chrono::steady_clock::now() >= toast_expiry) {
        toast_message.clear();
        return {};
    }
    return toast_message;
}

void SetSlotOccupiedCallback(SlotOccupiedFn callback) {
    std::lock_guard lock{state_mutex};
    slot_occupied = std::move(callback);
}

bool IsSlotOccupied(int slot) {
    std::lock_guard lock{state_mutex};
    return slot_occupied && slot_occupied(slot);
}

std::string GetGameTitle() {
    std::lock_guard lock{state_mutex};
    return game_title;
}

void FeedNav(const NavInput&) {}

} // namespace SwitchFrontend::OverlayUI
