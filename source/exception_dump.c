/* Minimal libnx user-exception handler for the IB3 runtime.
 *
 * Two jobs:
 *  1. Service permission faults on executable guest mappings (the runtime's
 *     code cache is mapped RWX; Horizon only allows RW or RX, so
 *     ib3_handle_exec_fault() flips the mapping and the access is retried).
 *  2. Log anything else and panic, using only the already-open debug log.
 *
 * Resuming: the first attempt is the kernel's own svcReturnFromException(0).
 * In the previous test log that call was answered with a bad-SVC exception
 * (esr = svc #0x28), so this build detects that case and switches to a manual
 * resume: it rebuilds every general register from the saved exception dump and
 * branches back itself. A branch needs a register that already holds the
 * target, so this works directly only for instruction fetches (a jump into a
 * chunk that has just been made executable: the jump register already holds
 * the address). A faulting store is instead re-executed from a small stub
 * (ib3_store_stub) placed within branch range of the store: the resume code
 * jumps to the stub through x17, and the stub restores x17, repeats the
 * store and branches (directly) to the next instruction.
 *
 * SIMD/FP state: the kernel's exception dump carries only GPRs, and the C code
 * that runs here (ib3_handle_exec_fault, ib3_deliver_signal, newlib memset,
 * compiler-generated struct copies) freely uses v0-v31. Without care the
 * interrupted thread resumes with clobbered vector registers. libib3's stub
 * emitter loads a 2-instruction template into d0, then stores it after a store
 * that faulted on an RX chunk, so every thunk built right after a chunk flip
 * came out with zeroed ldr/br words (SIGILL at "stub:+[NSArray array]").
 * __libnx_exception_handler is therefore an assembly entry that saves
 * v0-v31/fpcr/fpsr into a per-thread slot before any C runs, and every resume
 * path restores them just before returning to the guest. */

#include <stdint.h>

#include <switch.h>

#include "util.h"

/* Defining these symbols enables libnx's user exception handler. */
u8 __nx_exception_stack[0x8000] __attribute__((aligned(16)));
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

/* ib3_shim.c: services RW<->RX toggles and sparse VA commits.
 * Returns 0=unhandled, 1=exec flip, 2=sparse commit (retry same pc). */
int ib3_handle_exec_fault(int is_instruction_fetch, uint64_t address);
/* ib3_shim.c: stub that re-executes the store at pc and continues at pc+4. */
uint64_t ib3_store_stub(uint64_t pc);
/* ib3_shim.c: rewrite `mrs xN, cntvct_el0` in a code-cache pool to cntpct_el0. */
int ib3_patch_counter_read(uint64_t pc);
/* ib3_shim.c: synthesise POSIX signal to a registered handler.
 * regs = x0..x30,sp,pc,pstate (34); out_pc may receive a patched resume PC. */
int ib3_deliver_signal(int signum, uint64_t fault_addr, uint64_t pc,
                       const uint64_t *regs, uint64_t *out_pc);

/* svc #0x28 (ReturnFromException) with result 0, and one manual-resume entry
 * point per possible "register that equals the resume address" (x0..x30). */
void ib3_return_from_exception(void);
void ib3_return_from_exception_end(void);
void ib3_resume_0(const uint64_t *frame);
void ib3_resume_1(const uint64_t *frame);
void ib3_resume_2(const uint64_t *frame);
void ib3_resume_3(const uint64_t *frame);
void ib3_resume_4(const uint64_t *frame);
void ib3_resume_5(const uint64_t *frame);
void ib3_resume_6(const uint64_t *frame);
void ib3_resume_7(const uint64_t *frame);
void ib3_resume_8(const uint64_t *frame);
void ib3_resume_9(const uint64_t *frame);
void ib3_resume_10(const uint64_t *frame);
void ib3_resume_11(const uint64_t *frame);
void ib3_resume_12(const uint64_t *frame);
void ib3_resume_13(const uint64_t *frame);
void ib3_resume_14(const uint64_t *frame);
void ib3_resume_15(const uint64_t *frame);
void ib3_resume_16(const uint64_t *frame);
void ib3_resume_17(const uint64_t *frame);
void ib3_resume_18(const uint64_t *frame);
void ib3_resume_19(const uint64_t *frame);
void ib3_resume_20(const uint64_t *frame);
void ib3_resume_21(const uint64_t *frame);
void ib3_resume_22(const uint64_t *frame);
void ib3_resume_23(const uint64_t *frame);
void ib3_resume_24(const uint64_t *frame);
void ib3_resume_25(const uint64_t *frame);
void ib3_resume_26(const uint64_t *frame);
void ib3_resume_27(const uint64_t *frame);
void ib3_resume_28(const uint64_t *frame);
void ib3_resume_29(const uint64_t *frame);
void ib3_resume_30(const uint64_t *frame);

/* Per-thread SIMD save slots (16 x 1 KiB, picked from tpidrro_el0). Needs a
 * non-static symbol because the assembly below references it. */
uint8_t ib3_simd_save[16 * 1024] __attribute__((aligned(16)));

/* Leaves the address of this thread's slot in x9 (clobbers x10). */
#define IB3_SIMD_SLOT \
"  mrs x9, tpidrro_el0\n" \
"  ubfx x9, x9, #9, #4\n" \
"  adrp x10, ib3_simd_save\n" \
"  add x10, x10, :lo12:ib3_simd_save\n" \
"  add x9, x10, x9, lsl #10\n"

/* Clobbers only x9-x11. */
#define IB3_SIMD_SAVE \
IB3_SIMD_SLOT \
"  stp q0, q1, [x9, #0]\n" \
"  stp q2, q3, [x9, #32]\n" \
"  stp q4, q5, [x9, #64]\n" \
"  stp q6, q7, [x9, #96]\n" \
"  stp q8, q9, [x9, #128]\n" \
"  stp q10, q11, [x9, #160]\n" \
"  stp q12, q13, [x9, #192]\n" \
"  stp q14, q15, [x9, #224]\n" \
"  stp q16, q17, [x9, #256]\n" \
"  stp q18, q19, [x9, #288]\n" \
"  stp q20, q21, [x9, #320]\n" \
"  stp q22, q23, [x9, #352]\n" \
"  stp q24, q25, [x9, #384]\n" \
"  stp q26, q27, [x9, #416]\n" \
"  stp q28, q29, [x9, #448]\n" \
"  stp q30, q31, [x9, #480]\n" \
"  mrs x10, fpcr\n" \
"  mrs x11, fpsr\n" \
"  str x10, [x9, #512]\n" \
"  str x11, [x9, #520]\n"

/* Clobbers only x9-x11; x0 and everything else is left alone. */
#define IB3_SIMD_RESTORE \
IB3_SIMD_SLOT \
"  ldp q0, q1, [x9, #0]\n" \
"  ldp q2, q3, [x9, #32]\n" \
"  ldp q4, q5, [x9, #64]\n" \
"  ldp q6, q7, [x9, #96]\n" \
"  ldp q8, q9, [x9, #128]\n" \
"  ldp q10, q11, [x9, #160]\n" \
"  ldp q12, q13, [x9, #192]\n" \
"  ldp q14, q15, [x9, #224]\n" \
"  ldp q16, q17, [x9, #256]\n" \
"  ldp q18, q19, [x9, #288]\n" \
"  ldp q20, q21, [x9, #320]\n" \
"  ldp q22, q23, [x9, #352]\n" \
"  ldp q24, q25, [x9, #384]\n" \
"  ldp q26, q27, [x9, #416]\n" \
"  ldp q28, q29, [x9, #448]\n" \
"  ldp q30, q31, [x9, #480]\n" \
"  ldr x10, [x9, #512]\n" \
"  ldr x11, [x9, #520]\n" \
"  msr fpcr, x10\n" \
"  msr fpsr, x11\n"

/* The real handler (below). Entered only through __libnx_exception_handler. */
void ib3_exception_handler_c(ThreadExceptionDump *context);

__asm__(
".pushsection .text,\"ax\",%progbits\n"
".balign 4\n"
".global __libnx_exception_handler\n"
".type __libnx_exception_handler, %function\n"
"__libnx_exception_handler:\n"
IB3_SIMD_SAVE
"  b ib3_exception_handler_c\n"
".popsection\n"
);

__asm__(
".pushsection .text,\"ax\",%progbits\n"
".balign 4\n"
".global ib3_return_from_exception\n"
".type ib3_return_from_exception, %function\n"
"ib3_return_from_exception:\n"
IB3_SIMD_RESTORE
"  mov w0, #0\n"
"  svc #0x28\n"
"  ret\n"
".global ib3_return_from_exception_end\n"
"ib3_return_from_exception_end:\n"
".global ib3_resume_0\n"
".type ib3_resume_0, %function\n"
"ib3_resume_0:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"10:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 10b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x0, [sp, #-8]\n"
"  br x0\n"
".global ib3_resume_1\n"
".type ib3_resume_1, %function\n"
"ib3_resume_1:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"11:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 11b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x1, [sp, #-8]\n"
"  br x1\n"
".global ib3_resume_2\n"
".type ib3_resume_2, %function\n"
"ib3_resume_2:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"12:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 12b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x2, [sp, #-8]\n"
"  br x2\n"
".global ib3_resume_3\n"
".type ib3_resume_3, %function\n"
"ib3_resume_3:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"13:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 13b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x3, [sp, #-8]\n"
"  br x3\n"
".global ib3_resume_4\n"
".type ib3_resume_4, %function\n"
"ib3_resume_4:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"14:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 14b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x4, [sp, #-8]\n"
"  br x4\n"
".global ib3_resume_5\n"
".type ib3_resume_5, %function\n"
"ib3_resume_5:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"15:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 15b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x5, [sp, #-8]\n"
"  br x5\n"
".global ib3_resume_6\n"
".type ib3_resume_6, %function\n"
"ib3_resume_6:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"16:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 16b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x6, [sp, #-8]\n"
"  br x6\n"
".global ib3_resume_7\n"
".type ib3_resume_7, %function\n"
"ib3_resume_7:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"17:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 17b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x7, [sp, #-8]\n"
"  br x7\n"
".global ib3_resume_8\n"
".type ib3_resume_8, %function\n"
"ib3_resume_8:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"18:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 18b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x8, [sp, #-8]\n"
"  br x8\n"
".global ib3_resume_9\n"
".type ib3_resume_9, %function\n"
"ib3_resume_9:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"19:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 19b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x9, [sp, #-8]\n"
"  br x9\n"
".global ib3_resume_10\n"
".type ib3_resume_10, %function\n"
"ib3_resume_10:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"110:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 110b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x10, [sp, #-8]\n"
"  br x10\n"
".global ib3_resume_11\n"
".type ib3_resume_11, %function\n"
"ib3_resume_11:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"111:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 111b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x11, [sp, #-8]\n"
"  br x11\n"
".global ib3_resume_12\n"
".type ib3_resume_12, %function\n"
"ib3_resume_12:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"112:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 112b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x12, [sp, #-8]\n"
"  br x12\n"
".global ib3_resume_13\n"
".type ib3_resume_13, %function\n"
"ib3_resume_13:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"113:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 113b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x13, [sp, #-8]\n"
"  br x13\n"
".global ib3_resume_14\n"
".type ib3_resume_14, %function\n"
"ib3_resume_14:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"114:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 114b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x14, [sp, #-8]\n"
"  br x14\n"
".global ib3_resume_15\n"
".type ib3_resume_15, %function\n"
"ib3_resume_15:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"115:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 115b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x15, [sp, #-8]\n"
"  br x15\n"
".global ib3_resume_16\n"
".type ib3_resume_16, %function\n"
"ib3_resume_16:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"116:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 116b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x16, [sp, #-8]\n"
"  br x16\n"
".global ib3_resume_17\n"
".type ib3_resume_17, %function\n"
"ib3_resume_17:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"117:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 117b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x16, [sp, #0xf8]\n"
"  mov sp, x16\n"
"  ldur x16, [sp, #-16]\n"
"  ldur x17, [sp, #-8]\n"
"  br x17\n"
".global ib3_resume_18\n"
".type ib3_resume_18, %function\n"
"ib3_resume_18:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"118:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 118b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x18, [sp, #-8]\n"
"  br x18\n"
".global ib3_resume_19\n"
".type ib3_resume_19, %function\n"
"ib3_resume_19:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"119:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 119b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x19, [sp, #-8]\n"
"  br x19\n"
".global ib3_resume_20\n"
".type ib3_resume_20, %function\n"
"ib3_resume_20:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"120:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 120b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x20, [sp, #-8]\n"
"  br x20\n"
".global ib3_resume_21\n"
".type ib3_resume_21, %function\n"
"ib3_resume_21:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"121:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 121b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x21, [sp, #-8]\n"
"  br x21\n"
".global ib3_resume_22\n"
".type ib3_resume_22, %function\n"
"ib3_resume_22:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"122:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 122b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x22, [sp, #-8]\n"
"  br x22\n"
".global ib3_resume_23\n"
".type ib3_resume_23, %function\n"
"ib3_resume_23:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"123:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 123b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x23, [sp, #-8]\n"
"  br x23\n"
".global ib3_resume_24\n"
".type ib3_resume_24, %function\n"
"ib3_resume_24:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"124:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 124b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x24, [sp, #-8]\n"
"  br x24\n"
".global ib3_resume_25\n"
".type ib3_resume_25, %function\n"
"ib3_resume_25:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"125:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 125b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x25, [sp, #-8]\n"
"  br x25\n"
".global ib3_resume_26\n"
".type ib3_resume_26, %function\n"
"ib3_resume_26:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"126:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 126b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x26, [sp, #-8]\n"
"  br x26\n"
".global ib3_resume_27\n"
".type ib3_resume_27, %function\n"
"ib3_resume_27:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"127:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 127b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x27, [sp, #-8]\n"
"  br x27\n"
".global ib3_resume_28\n"
".type ib3_resume_28, %function\n"
"ib3_resume_28:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"128:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 128b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x28, [sp, #-8]\n"
"  br x28\n"
".global ib3_resume_29\n"
".type ib3_resume_29, %function\n"
"ib3_resume_29:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"129:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 129b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x29, [sp, #-8]\n"
"  br x29\n"
".global ib3_resume_30\n"
".type ib3_resume_30, %function\n"
"ib3_resume_30:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x120\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"130:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 130b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-16]\n"
"  ldur x30, [sp, #-8]\n"
"  br x30\n"
".popsection\n"
);

typedef void (*ResumeFn)(const uint64_t *frame);
static const ResumeFn g_resume_fns[31] = {
  ib3_resume_0,
  ib3_resume_1,
  ib3_resume_2,
  ib3_resume_3,
  ib3_resume_4,
  ib3_resume_5,
  ib3_resume_6,
  ib3_resume_7,
  ib3_resume_8,
  ib3_resume_9,
  ib3_resume_10,
  ib3_resume_11,
  ib3_resume_12,
  ib3_resume_13,
  ib3_resume_14,
  ib3_resume_15,
  ib3_resume_16,
  ib3_resume_17,
  ib3_resume_18,
  ib3_resume_19,
  ib3_resume_20,
  ib3_resume_21,
  ib3_resume_22,
  ib3_resume_23,
  ib3_resume_24,
  ib3_resume_25,
  ib3_resume_26,
  ib3_resume_27,
  ib3_resume_28,
  ib3_resume_29,
  ib3_resume_30
};

static ThreadExceptionDump g_saved;
static volatile int g_kernel_resume_pending;
static volatile int g_kernel_resume_rejected;

static char *append_text(char *output, const char *text) {
  while (*text)
    *output++ = *text++;
  return output;
}

static char *append_hex(char *output, uint64_t value, int digits) {
  static const char hex[] = "0123456789abcdef";
  for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
    *output++ = hex[(value >> shift) & 0xf];
  return output;
}

static void log_line(const char *a, uint64_t va, const char *b, uint64_t vb) {
  char line[160];
  char *c = append_text(line, a);
  c = append_hex(c, va, 16);
  if (b) {
    c = append_text(c, b);
    c = append_hex(c, vb, 16);
  }
  *c++ = '\n';
  debugEmergencyWrite(line, (size_t)(c - line));
}

static int context_is_readable(const ThreadExceptionDump *context) {
  if (!context)
    return 0;
  MemoryInfo info;
  u32 page_info;
  if (R_FAILED(svcQueryMemory(&info, &page_info, (uint64_t)context)))
    return 0;
  const uintptr_t start = (uintptr_t)context;
  const uintptr_t end = start + sizeof(*context);
  return info.type != MemType_Unmapped && (info.perm & Perm_R) &&
         end >= start && end <= (uintptr_t)info.addr + info.size;
}

static uint64_t gpr(const ThreadExceptionDump *d, int i) {
  if (i < 29)
    return d->cpu_gprs[i].x;
  return i == 29 ? d->fp.x : d->lr.x;
}

/* ---- soft-null (guest audio path) -------------------------------------
 * libib3.so's AUGraph setup loads through a 32-bit "-1" pointer on the audio
 * thread; the guest's SIGSEGV handler would otherwise kill that thread and the
 * game goes silent. The old handler zeroed x0 and advanced PC by 4 for ANY
 * instruction at such an address. That corrupted live state: it cleared x0
 * even when x0 was unrelated, zeroed the *source* register of stores and of
 * SIMD loads/stores (vector registers were treated as GPRs), and "skipped" LDPs,
 * atomics and writeback forms without applying their side effects.
 *
 * Now only instructions whose effect can be reproduced exactly are skipped:
 *   - LDR/LDUR (GPR, no writeback, incl. register offset): the destination is
 *     zeroed, i.e. the load "returned 0";
 *   - LDP (GPR, signed offset): both destinations zeroed;
 *   - STR/STUR/STP (GPR or SIMD, no writeback): simply dropped.
 * Anything else is left to the normal SIGSEGV path so a real bug is visible
 * instead of silently turning into corrupted state. Returns 1 if applied. */
static void soft_null_zero_gpr(ThreadExceptionDump *context, int reg) {
  if (reg < 29)
    context->cpu_gprs[reg].x = 0;
  else if (reg == 29)
    context->fp.x = 0;
  else if (reg == 30)
    context->lr.x = 0;
  /* reg 31 is xzr in these encodings: nothing to clear */
}

static int soft_null_apply(ThreadExceptionDump *context, uint64_t address) {
  if (!context || (context->pc.x & 3) != 0)
    return 0;
  const uint32_t instr = *(volatile uint32_t *)(uintptr_t)context->pc.x;
  const int rt = (int)(instr & 0x1fu);
  const int rt2 = (int)((instr >> 10) & 0x1fu);
  const int vec = (int)((instr >> 26) & 1u);
  int load = 0;
  int pair = 0;

  if ((instr & 0x3b000000u) == 0x39000000u) {
    /* LDR/STR (immediate, unsigned offset). opc = bits 23:22. */
    const uint32_t opc = (instr >> 22) & 3u;
    if (vec)
      load = (opc & 1u) != 0;
    else
      load = opc != 0;
    if (!vec && opc == 2u && ((instr >> 30) & 3u) == 3u)
      return 0; /* PRFM (never faults; not a load/store we can reproduce) */
  } else if ((instr & 0x3b200c00u) == 0x38000000u) {
    /* LDUR/STUR (unscaled, no writeback). */
    const uint32_t opc = (instr >> 22) & 3u;
    load = vec ? (opc & 1u) != 0 : opc != 0;
  } else if ((instr & 0x3b200c00u) == 0x38200800u) {
    /* LDR/STR (register offset). */
    const uint32_t opc = (instr >> 22) & 3u;
    load = vec ? (opc & 1u) != 0 : opc != 0;
    if (!vec && opc == 2u && ((instr >> 30) & 3u) == 3u)
      return 0; /* PRFM */
  } else if ((instr & 0x3b800000u) == 0x29000000u) {
    /* LDP/STP, signed offset (no writeback). */
    pair = 1;
    load = (instr >> 22) & 1u;
  } else {
    return 0;
  }

  if (load && vec)
    return 0; /* SIMD destination: no vector registers in the dump to clear */
  if (load) {
    soft_null_zero_gpr(context, rt);
    if (pair)
      soft_null_zero_gpr(context, rt2);
  }
  context->pc.x += 4;

  static int soft_null_logs;
  if (soft_null_logs < 24) {
    soft_null_logs++;
    log_line("soft-null: far=0x", address, " skip instr=0x", (uint64_t)instr);
    /* Where it came from: lets the real culprit (whoever returned -1 where a
     * pointer was expected) be found instead of papering over it. */
    log_line("soft-null: pc=0x", context->pc.x - 4, " lr=0x", context->lr.x);
  }
  return 1;
}

static char *append_dec(char *o, unsigned v) {
  if (v >= 10)
    *o++ = (char)('0' + v / 10);
  *o++ = (char)('0' + v % 10);
  return o;
}

static int range_is_readable(uint64_t addr, size_t len) {
  MemoryInfo info;
  u32 page_info;
  if (R_FAILED(svcQueryMemory(&info, &page_info, addr)))
    return 0;
  return info.type != MemType_Unmapped && (info.perm & Perm_R) &&
         addr + len >= addr && addr + len <= (uint64_t)info.addr + info.size;
}

/* 32-bit words around `addr`, `|` marks addr itself. Only reads mapped,
 * readable memory. */
static void dump_words(const char *tag, uint64_t addr, int before, int after) {
  const uint64_t start = addr - (uint64_t)before * 4;
  const size_t bytes = (size_t)(before + after) * 4;
  char line[256];
  char *c = append_text(line, tag);
  c = append_hex(c, addr, 16);
  c = append_text(c, ":");
  if (!range_is_readable(start, bytes)) {
    c = append_text(c, " unreadable");
  } else {
    for (int i = 0; i < before + after; i++) {
      *c++ = ' ';
      if (i == before)
        *c++ = '|';
      c = append_hex(c, *(const volatile uint32_t *)(uintptr_t)(start + 4u * (unsigned)i), 8);
    }
  }
  *c++ = '\n';
  debugEmergencyWrite(line, (size_t)(c - line));
}

static void dump_regs(const ThreadExceptionDump *d) {
  for (int base = 0; base < 31; base += 3) {
    char line[160];
    char *c = append_text(line, "regs");
    for (int i = base; i < base + 3 && i < 31; i++) {
      c = append_text(c, " x");
      c = append_dec(c, (unsigned)i);
      c = append_text(c, "=");
      c = append_hex(c, gpr(d, i), 16);
    }
    *c++ = '\n';
    debugEmergencyWrite(line, (size_t)(c - line));
  }
}

/* Copy without libc (a compiler-generated memcpy could use SIMD registers). */
static void copy_dump(ThreadExceptionDump *dst, const ThreadExceptionDump *src) {
  volatile uint64_t *d = (volatile uint64_t *)dst;
  const volatile uint64_t *s = (const volatile uint64_t *)src;
  for (size_t i = 0; i < sizeof(*dst) / 8; i++)
    d[i] = s[i];
}

/* Frame layout (dwords) shared with the assembly: 0..30 x0..x30, 31 sp,
 * 32 nzcv, 33 saved x17 (lands at [sp-24]), 34 saved xT (lands at [sp-16]),
 * 35 branch target (lands at [sp-8]). */
/* Retry the faulting instruction at the SAME pc (sparse VA commit: pages
 * were unmapped so the access never completed). Uses resume_17: restores
 * every GPR except x17, which is loaded with `pc` and used to branch.
 * x17 is IP1 (linker scratch) and is almost always free across a fault. */
static void manual_resume_retry_same_pc(const ThreadExceptionDump *d) {
  const uint64_t pc = d->pc.x;
  const uint64_t sp = d->sp.x;
  if (sp & 0xf) {
    log_line("manual-resume: guest sp misaligned: 0x", sp, 0, 0);
    return;
  }
  uint64_t frame[36];
  for (int i = 0; i < 31; i++)
    frame[i] = gpr(d, i);
  frame[31] = sp;
  frame[32] = (uint64_t)(d->pstate & 0xf0000000u);
  frame[33] = 0;
  frame[34] = gpr(d, 16); /* resume_17 restores x16 from here */
  frame[35] = pc;         /* resume_17 loads x17=pc and br x17 */
  log_line("manual-resume: retry-same-pc=0x", pc, 0, 0);
  g_resume_fns[17](frame);
  __builtin_unreachable();
}

static void manual_resume_store(const ThreadExceptionDump *d) {
  const uint64_t pc = d->pc.x;
  const uint64_t sp = d->sp.x;
  if (!(d->esr & 0x40)) { /* ISS.WnR: not a write */
    /* Data load into freshly committed page: retry same pc. */
    manual_resume_retry_same_pc(d);
    return;
  }
  if (sp & 0xf) {
    log_line("manual-resume: guest sp misaligned: 0x", sp, 0, 0);
    return;
  }
  const uint64_t stub = ib3_store_stub(pc);
  if (!stub) {
    /* Store outside libib3.so (e.g. memset into guest VA after sparse commit).
     * Pages are now mapped RW — retry the same instruction. x17 clobbered. */
    log_line("manual-resume: store-retry-same-pc at 0x", pc, " instr=0x",
             *(const volatile uint32_t *)(uintptr_t)pc);
    manual_resume_retry_same_pc(d);
    return;
  }
  uint64_t frame[36];
  for (int i = 0; i < 31; i++)
    frame[i] = gpr(d, i);
  frame[31] = sp;
  frame[32] = (uint64_t)(d->pstate & 0xf0000000u);
  frame[33] = gpr(d, 17);
  frame[34] = gpr(d, 16);
  frame[35] = stub;
  g_resume_fns[17](frame); /* K=17, T=16: br x17 lands on the stub */
  __builtin_unreachable();
}

/* fault_kind: 0=fetch/exec-flip, 1=store exec-flip, 2=sparse-commit retry */
static void manual_resume(const ThreadExceptionDump *d, int is_fetch, int fault_kind) {
  const uint64_t pc = d->pc.x;
  const uint64_t sp = d->sp.x;
  if (fault_kind == 2) {
    manual_resume_retry_same_pc(d);
    return;
  }
  if (!is_fetch) {
    manual_resume_store(d);
    return;
  }
  if (sp & 0xf) {
    log_line("manual-resume: guest sp misaligned: 0x", sp, 0, 0);
    return;
  }
  int k = -1;
  for (int i = 0; i < 31; i++) {
    if (gpr(d, i) == pc) {
      k = i;
      break;
    }
  }
  if (k < 0) {
    /* No register holds pc (straight-line fetch into newly-RX chunk).
     * Retry same pc via x17 clobber path. */
    log_line("manual-resume: fetch-retry-same-pc=0x", pc, 0, 0);
    manual_resume_retry_same_pc(d);
    return;
  }
  const int t = (k == 17) ? 16 : 17;

  /* frame layout (dwords): 0..30 x0..x30, 31 sp, 32 nzcv, 33 pad,
   * 34 original xT, 35 pc */
  uint64_t frame[36];
  for (int i = 0; i < 31; i++)
    frame[i] = gpr(d, i);
  frame[31] = sp;
  frame[32] = (uint64_t)(d->pstate & 0xf0000000u);
  frame[33] = 0;
  frame[34] = gpr(d, t);
  frame[35] = pc;
  log_line("manual-resume: pc=0x", pc, " via x", (uint64_t)k);
  g_resume_fns[k](frame);
  __builtin_unreachable();
}

void ib3_exception_handler_c(ThreadExceptionDump *context) {
  static volatile int handling_exception;
  static int g_saved_fault_kind; /* 0/1/2 from ib3_handle_exec_fault */

  if (context_is_readable(context)) {
    const int was_pending = g_kernel_resume_pending;
    g_kernel_resume_pending = 0;
    const uint32_t ec = (context->esr >> 26) & 0x3f;
    const int is_fetch = ec == 0x20 || ec == 0x21;
    const int is_data = ec == 0x24 || ec == 0x25;

    /* The kernel refused svc #0x28 issued from ib3_return_from_exception. */
    /* The svc sits after the SIMD-restore prologue, so test the whole function
     * body rather than a fixed distance from its first instruction. */
    const uint64_t fn_start = (uint64_t)(uintptr_t)ib3_return_from_exception;
    const uint64_t fn_end = (uint64_t)(uintptr_t)ib3_return_from_exception_end;
    if (was_pending && ec == 0x15 && (context->esr & 0xffff) == 0x28 &&
        context->pc.x >= fn_start && context->pc.x < fn_end) {
      g_kernel_resume_rejected = 1;
      log_line("kernel-resume: svc #0x28 rejected (desc=0x", context->error_desc,
               0, 0);
      {
        const uint32_t saved_ec = (g_saved.esr >> 26) & 0x3f;
        manual_resume(&g_saved, saved_ec == 0x20 || saved_ec == 0x21,
                      g_saved_fault_kind);
      }
      goto crash; /* manual_resume returned: could not resume */
    }

    if (is_fetch || is_data) {
      const uint64_t address = is_fetch ? context->pc.x : context->far.x;
      const int kind = ib3_handle_exec_fault(is_fetch, address);
      if (kind) {
        g_saved_fault_kind = kind;
        if (g_kernel_resume_rejected) {
          manual_resume(context, is_fetch, kind);
        } else {
          copy_dump(&g_saved, context);
          g_kernel_resume_pending = 1;
          ib3_return_from_exception();
          /* not reached if the kernel resumed the thread */
        }
      } else if (is_data) {
        /* Soft-null for the guest audio path: libib3.so's AUGraph / AAudio
         * setup does a load from 0xffffffff (error-as-minus-one). The guest
         * SIGSEGV handler logs CRASH and kills the audio thread → silence.
         * For any null-ish FAR always zero x0 (observed bad pointer) and skip
         * the instruction so the audio thread survives. */
        /* FAR is reported as 0x00000000ffffffff (32-bit -1), not the full
         * 64-bit -1. Match both forms and any high canonical null. */
        if ((address == 0 || address == 0xffffffffull ||
             address == (uint64_t)-1 ||
             (address >= 0xfffffffffffff000ull)) &&
            soft_null_apply(context, address)) {
          g_saved_fault_kind = 2;
          if (g_kernel_resume_rejected) {
            manual_resume(context, 0, 2);
          } else {
            copy_dump(&g_saved, context);
            g_kernel_resume_pending = 1;
            ib3_return_from_exception();
          }
          /* not reached on successful resume */
        }

        /* Unhandled data abort → synthesise SIGSEGV (11) if registered. */
        uint64_t regs[34];
        for (int i = 0; i < 31; i++)
          regs[i] = gpr(context, i);
        regs[31] = context->sp.x;
        regs[32] = context->pc.x;
        regs[33] = context->pstate;
        uint64_t resume_pc = context->pc.x;
        if (ib3_deliver_signal(11, address, context->pc.x, regs, &resume_pc)) {
          g_saved_fault_kind = 2;
          if (resume_pc != context->pc.x)
            context->pc.x = resume_pc;
          if (g_kernel_resume_rejected) {
            manual_resume(context, 0, 2);
          } else {
            copy_dump(&g_saved, context);
            g_kernel_resume_pending = 1;
            ib3_return_from_exception();
          }
        }
      }
    }

    /* EC=0x18: trapped MSR/MRS. Horizon lets EL0 read CNTPCT_EL0 and CNTFRQ_EL0
     * but traps CNTVCT_EL0, which Android code (mach_absolute_time in libib3's
     * generated code) reads. Patch the instruction in place when it lives in
     * one of our code pools; otherwise emulate the read and skip it. */
    if (ec == 0x18) {
      const uint32_t iss = context->esr & 0x1ffffffu;
      const int is_read = iss & 1;
      const uint32_t crm = (iss >> 1) & 0xf, rt = (iss >> 5) & 0x1f;
      const uint32_t crn = (iss >> 10) & 0xf, op1 = (iss >> 14) & 7;
      const uint32_t op2 = (iss >> 17) & 7, op0 = (iss >> 20) & 3;
      if (is_read && op0 == 3 && op1 == 3 && crn == 14 && crm == 0 && op2 <= 2) {
        int handled = 0;
        if (op2 == 2 && ib3_patch_counter_read(context->pc.x)) {
          handled = 1; /* instruction rewritten: retry the same pc */
        } else {
          const uint64_t value = op2 == 0 ? 19200000ull : armGetSystemTick();
          if (rt < 29)
            context->cpu_gprs[rt].x = value;
          else if (rt == 29)
            context->fp.x = value;
          else if (rt == 30)
            context->lr.x = value;
          context->pc.x += 4;
          handled = 1;
        }
        if (handled) {
          g_saved_fault_kind = 2; /* retry (possibly advanced) pc */
          if (g_kernel_resume_rejected) {
            manual_resume(context, 1, 2);
          } else {
            copy_dump(&g_saved, context);
            g_kernel_resume_pending = 1;
            ib3_return_from_exception();
          }
        }
      }
    }

    /* EC=0: undefined instruction. Guest JIT uses zero/UDF words as traps
     * that a SIGILL handler is expected to patch and resume. */
    if (ec == 0) {
      const uint64_t pc = context->pc.x;
      uint64_t regs[34];
      for (int i = 0; i < 31; i++)
        regs[i] = gpr(context, i);
      regs[31] = context->sp.x;
      regs[32] = pc;
      regs[33] = context->pstate;
      uint64_t resume_pc = pc;
      if (ib3_deliver_signal(4 /* SIGILL */, pc, pc, regs, &resume_pc)) {
        g_saved_fault_kind = 2; /* retry (possibly advanced) pc after handler */
        if (resume_pc != pc)
          context->pc.x = resume_pc;
        if (g_kernel_resume_rejected) {
          manual_resume(context, 1, 2);
        } else {
          copy_dump(&g_saved, context);
          g_kernel_resume_pending = 1;
          ib3_return_from_exception();
        }
      }
    }
  }

crash:
  if (__atomic_exchange_n(&handling_exception, 1, __ATOMIC_SEQ_CST)) {
    svcBreak(BreakReason_Panic, (uint64_t)context, 0);
    for (;;)
      svcSleepThread(1000000000ULL);
  }

  char line[512];
  char *cursor = append_text(line, "exception-raw: context=0x");
  cursor = append_hex(cursor, (uintptr_t)context, 16);
  if (context_is_readable(context)) {
    cursor = append_text(cursor, " desc=0x");
    cursor = append_hex(cursor, context->error_desc, 8);
    cursor = append_text(cursor, " esr=0x");
    cursor = append_hex(cursor, context->esr, 8);
    cursor = append_text(cursor, " pc=0x");
    cursor = append_hex(cursor, context->pc.x, 16);
    cursor = append_text(cursor, " lr=0x");
    cursor = append_hex(cursor, context->lr.x, 16);
    cursor = append_text(cursor, " far=0x");
    cursor = append_hex(cursor, context->far.x, 16);
    cursor = append_text(cursor, " sp=0x");
    cursor = append_hex(cursor, context->sp.x, 16);
  } else {
    cursor = append_text(cursor, " UNREADABLE");
  }
  *cursor++ = '\n';
  debugEmergencyWrite(line, (size_t)(cursor - line));

  if (context_is_readable(context)) {
    /* ESR EC=0 means the CPU could not decode the word at pc (undefined
     * instruction). Show what is really there, and around the caller. */
    log_line("crash: ec=0x", (context->esr >> 26) & 0x3f, " pstate=0x", context->pstate);
    dump_words("words pc=0x", context->pc.x, 4, 8);
    dump_words("words lr=0x", context->lr.x, 4, 4);
    dump_regs(context);
  }

  /* Preserve Atmosphere's normal crash reporting after our log is flushed. */
  const uint64_t pc = context_is_readable(context) ? context->pc.x : 0;
  const uint64_t far = context_is_readable(context) ? context->far.x : 0;
  svcBreak(BreakReason_Panic, pc, far);
  for (;;)
    svcSleepThread(1000000000ULL);
}
