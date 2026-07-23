/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <mutex>

#include <oaknut/code_block.hpp>
#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/common/spin_lock.h"

namespace Dynarmic {

using Backend::Arm64::Wscratch0;
using Backend::Arm64::Wscratch1;
using namespace oaknut::util;

void EmitSpinLockLock(oaknut::CodeGenerator& code, oaknut::XReg ptr) {
    oaknut::Label start, loop;

    code.MOV(Wscratch1, 1);
    code.SEVL();
    code.l(start);
    code.WFE();
    code.l(loop);
    code.LDAXR(Wscratch0, ptr);
    code.CBNZ(Wscratch0, start);
    code.STXR(Wscratch0, Wscratch1, ptr);
    code.CBNZ(Wscratch0, loop);
}

void EmitSpinLockUnlock(oaknut::CodeGenerator& code, oaknut::XReg ptr) {
    code.STLR(WZR, ptr);
}

namespace {

#if !defined(__SWITCH__)
struct SpinLockImpl {
    SpinLockImpl();

    void Initialize();

    oaknut::CodeBlock mem;
    oaknut::CodeGenerator code;

    void (*lock)(volatile int*);
    void (*unlock)(volatile int*);
};

std::once_flag flag;
SpinLockImpl impl;

SpinLockImpl::SpinLockImpl()
        : mem{4096}
        , code{mem.wptr(), mem.xptr()} {}

void SpinLockImpl::Initialize() {
    mem.unprotect();

    lock = code.xptr<void (*)(volatile int*)>();
    EmitSpinLockLock(code, X0);
    code.RET();

    unlock = code.xptr<void (*)(volatile int*)>();
    EmitSpinLockUnlock(code, X0);
    code.RET();

    mem.invalidate_all();
    mem.protect();
}
#endif

}  // namespace

void SpinLock::Lock() {
#if defined(__SWITCH__)
    for (;;) {
        if (__atomic_exchange_n(&storage, 1, __ATOMIC_ACQUIRE) == 0) {
            return;
        }
        while (__atomic_load_n(&storage, __ATOMIC_RELAXED) != 0) {
            asm volatile("yield");
        }
    }
#else
    std::call_once(flag, &SpinLockImpl::Initialize, impl);
    impl.lock(&storage);
#endif
}

void SpinLock::Unlock() {
#if defined(__SWITCH__)
    __atomic_store_n(&storage, 0, __ATOMIC_RELEASE);
#else
    std::call_once(flag, &SpinLockImpl::Initialize, impl);
    impl.unlock(&storage);
#endif
}

}  // namespace Dynarmic
