/**
 * @file        rex/core/fiber_posix.cpp
 * @brief       POSIX backend for rex::thread::Fiber (makecontext/swapcontext)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
// Darwin hides the deprecated ucontext API unless a POSIX/XSI feature level
// is requested before <ucontext.h> is included. fiber.h includes ucontext.h,
// so the feature macro must be set in this translation unit first.
#define _XOPEN_SOURCE 700
#endif

#include <rex/platform.h>
#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC

#include <rex/thread/fiber.h>

#include <cassert>

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

#if REX_FIBER_STACK_SWITCH

// rex_fiber_switch(save, load) pushes the callee-saved registers and the
// floating-point control state on the current stack, stores the stack pointer
// to *save, then loads `load` and pops the same frame from it. A new fiber's
// stack starts with such a frame whose return address is rex_fiber_start,
// which calls the entry function held in a callee-saved register.
extern "C" void rex_fiber_switch(void** save, void* load);
extern "C" void rex_fiber_start();

#if REX_ARCH_AMD64
// Frame, from the saved stack pointer up: MXCSR (4 bytes) and the x87 control
// word (2 bytes, padded to 8), r15, r14, r13, r12, rbx, rbp, return address.
asm(R"(
  .text
  .p2align 4
  .globl rex_fiber_switch
  .hidden rex_fiber_switch
  .type rex_fiber_switch, @function
rex_fiber_switch:
  .cfi_startproc
  pushq %rbp
  pushq %rbx
  pushq %r12
  pushq %r13
  pushq %r14
  pushq %r15
  subq $8, %rsp
  stmxcsr (%rsp)
  fnstcw 4(%rsp)
  movq %rsp, (%rdi)
  movq %rsi, %rsp
  ldmxcsr (%rsp)
  fldcw 4(%rsp)
  addq $8, %rsp
  popq %r15
  popq %r14
  popq %r13
  popq %r12
  popq %rbx
  popq %rbp
  ret
  .cfi_endproc
  .size rex_fiber_switch, .-rex_fiber_switch

  .p2align 4
  .globl rex_fiber_start
  .hidden rex_fiber_start
  .type rex_fiber_start, @function
rex_fiber_start:
  .cfi_startproc
  .cfi_undefined rip
  movq %r13, %rdi
  callq *%r12
  ud2
  .cfi_endproc
  .size rex_fiber_start, .-rex_fiber_start
)");
constexpr size_t kFiberFrameWords = 8;
constexpr size_t kFiberFrameEntry = 4;  // r12
constexpr size_t kFiberFrameArg = 3;    // r13
constexpr size_t kFiberFrameReturn = 7;
#elif REX_ARCH_ARM64
// Frame, from the saved stack pointer up: x19 to x28, x29, x30 (the return
// address), d8 to d15, FPCR, FPSR.
asm(R"(
  .text
  .p2align 4
  .globl rex_fiber_switch
  .hidden rex_fiber_switch
  .type rex_fiber_switch, %function
rex_fiber_switch:
  .cfi_startproc
  hint #34
  sub sp, sp, #176
  stp x19, x20, [sp, #0]
  stp x21, x22, [sp, #16]
  stp x23, x24, [sp, #32]
  stp x25, x26, [sp, #48]
  stp x27, x28, [sp, #64]
  stp x29, x30, [sp, #80]
  stp d8, d9, [sp, #96]
  stp d10, d11, [sp, #112]
  stp d12, d13, [sp, #128]
  stp d14, d15, [sp, #144]
  mrs x9, fpcr
  mrs x10, fpsr
  stp x9, x10, [sp, #160]
  mov x9, sp
  str x9, [x0]
  mov sp, x1
  ldp x9, x10, [sp, #160]
  msr fpcr, x9
  msr fpsr, x10
  ldp x19, x20, [sp, #0]
  ldp x21, x22, [sp, #16]
  ldp x23, x24, [sp, #32]
  ldp x25, x26, [sp, #48]
  ldp x27, x28, [sp, #64]
  ldp x29, x30, [sp, #80]
  ldp d8, d9, [sp, #96]
  ldp d10, d11, [sp, #112]
  ldp d12, d13, [sp, #128]
  ldp d14, d15, [sp, #144]
  add sp, sp, #176
  ret
  .cfi_endproc
  .size rex_fiber_switch, .-rex_fiber_switch

  .p2align 4
  .globl rex_fiber_start
  .hidden rex_fiber_start
  .type rex_fiber_start, %function
rex_fiber_start:
  .cfi_startproc
  .cfi_undefined x30
  hint #34
  mov x0, x20
  blr x19
  brk #0
  .cfi_endproc
  .size rex_fiber_start, .-rex_fiber_start
)");
constexpr size_t kFiberFrameWords = 22;
constexpr size_t kFiberFrameEntry = 0;  // x19
constexpr size_t kFiberFrameArg = 1;    // x20
constexpr size_t kFiberFrameReturn = 11;  // x30
#endif

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);
  // The first switch to the fiber pops this frame: callee-saved registers
  // zeroed but for the entry and its argument, the floating-point control
  // state of the creating thread, and a return into rex_fiber_start with the
  // stack 16-byte aligned.
  auto top = reinterpret_cast<uintptr_t>(f->stack_.data() + f->stack_.size()) & ~uintptr_t(15);
  auto* frame = reinterpret_cast<uint64_t*>(top) - kFiberFrameWords;
  for (size_t i = 0; i < kFiberFrameWords; ++i) {
    frame[i] = 0;
  }
  frame[kFiberFrameEntry] = reinterpret_cast<uint64_t>(entry);
  frame[kFiberFrameArg] = reinterpret_cast<uint64_t>(arg);
  frame[kFiberFrameReturn] = reinterpret_cast<uint64_t>(&rex_fiber_start);
#if REX_ARCH_AMD64
  uint32_t mxcsr;
  uint16_t fpcw;
  asm volatile("stmxcsr %0" : "=m"(mxcsr));
  asm volatile("fnstcw %0" : "=m"(fpcw));
  frame[0] = uint64_t(mxcsr) | (uint64_t(fpcw) << 32);
#elif REX_ARCH_ARM64
  uint64_t fpcr;
  asm volatile("mrs %0, fpcr" : "=r"(fpcr));
  frame[20] = fpcr;
#endif
  f->stack_pointer_ = frame;
  return f;
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  rex_fiber_switch(&from->stack_pointer_, target->stack_pointer_);
}

#else  // !REX_FIBER_STACK_SWITCH

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  if (getcontext(&f->context_) == -1) {
    delete f;
    return nullptr;
  }
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);

  if (getcontext(&f->context_) == -1) {
    delete f;
    return nullptr;
  }
  f->context_.uc_stack.ss_sp = f->stack_.data();
  f->context_.uc_stack.ss_size = f->stack_.size();
  f->context_.uc_link = nullptr;
  // Trampoline reads entry_/arg_ from tls_current_ — no pointer splitting needed.
  makecontext(&f->context_, &Fiber::Trampoline, 0);
  return f;
}

/*static*/ void Fiber::Trampoline() {
  // tls_current_ was updated by SwitchTo before swapcontext returned here.
  Fiber* f = tls_current_;
  f->entry_(f->arg_);
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  swapcontext(&from->context_, &target->context_);
}

#endif  // REX_FIBER_STACK_SWITCH

void Fiber::Destroy() {
  // Thread fibers are destroyed from the owning thread itself.
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
  }
  // No POSIX equivalent of ConvertFiberToThread; stack_ is freed by the vector destructor.
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_LINUX || REX_PLATFORM_MAC
