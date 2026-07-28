// Copyright 2026 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "GBAStation/overlay/overlay_ui.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <utility>

namespace SwitchFrontend::OverlayUI {
namespace {

std::mutex state_mutex;
std::string game_title;
std::string toast_message;
using ToastExpiryRep = std::chrono::steady_clock::duration::rep;
std::atomic<ToastExpiryRep> toast_expiry{};
SlotOccupiedFn slot_occupied;
CheatListFn cheat_list;
CheatToggleFn cheat_toggle;

bool IsActionInRange(Action action, Action first, Action last) {
    return static_cast<int>(action) >= static_cast<int>(first) &&
           static_cast<int>(action) <= static_cast<int>(last);
}

} // namespace

bool IsSaveStateAction(Action action) {
    return action == Action::QuickSaveState ||
           IsActionInRange(action, Action::SaveStateSlot1, Action::SaveStateSlot10);
}

bool IsLoadStateAction(Action action) {
    return action == Action::QuickLoadState ||
           IsActionInRange(action, Action::LoadStateSlot1, Action::LoadStateSlot10);
}

int GetStateSlotForAction(Action action) {
    if (action == Action::QuickSaveState || action == Action::QuickLoadState) {
        return 0;
    }
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
    const auto expiry = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    toast_expiry.store(toast_message.empty() ? ToastExpiryRep{} : expiry.time_since_epoch().count(),
                       std::memory_order_release);
}

bool HasTransientContent() {
    ToastExpiryRep expiry = toast_expiry.load(std::memory_order_acquire);
    if (expiry == ToastExpiryRep{}) {
        return false;
    }
    if (std::chrono::steady_clock::now().time_since_epoch().count() < expiry) {
        return true;
    }
    toast_expiry.compare_exchange_strong(expiry, ToastExpiryRep{}, std::memory_order_acq_rel,
                                         std::memory_order_acquire);
    return false;
}

std::string GetToast() {
    std::lock_guard lock{state_mutex};
    const ToastExpiryRep expiry = toast_expiry.load(std::memory_order_acquire);
    if (toast_message.empty() || expiry == ToastExpiryRep{} ||
        std::chrono::steady_clock::now().time_since_epoch().count() >= expiry) {
        toast_message.clear();
        toast_expiry.store(ToastExpiryRep{}, std::memory_order_release);
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

void SetCheatCallbacks(CheatListFn list_callback, CheatToggleFn toggle_callback) {
    std::lock_guard lock{state_mutex};
    cheat_list = std::move(list_callback);
    cheat_toggle = std::move(toggle_callback);
}

std::vector<CheatEntry> GetCheats() {
    CheatListFn callback;
    {
        std::lock_guard lock{state_mutex};
        callback = cheat_list;
    }
    return callback ? callback() : std::vector<CheatEntry>{};
}

bool ToggleCheat(int index) {
    CheatToggleFn callback;
    {
        std::lock_guard lock{state_mutex};
        callback = cheat_toggle;
    }
    return callback && callback(index);
}

void FeedNav(const NavInput&) {}

} // namespace SwitchFrontend::OverlayUI
