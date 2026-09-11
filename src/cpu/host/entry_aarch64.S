/*
 * Host CPU assembly for AArch64: process entry, signal trampoline,
 * context switch.
 *
 * Context switch frame (as pointed to by thread_t.t_sp when suspended),
 * 160 bytes, offsets from t_sp:
 *
 *     0  x19             (cpu_thread_start: entry)
 *     8  x20             (cpu_thread_start: thread_exit)
 *    16  x21             (cpu_thread_start: arg0)
 *    24  x22             (cpu_thread_start: arg1)
 *    32  x23             (cpu_thread_start: arg2)
 *    40  x24             (cpu_thread_start: arg3)
 *    48  x25
 *    56  x26
 *    64  x27
 *    72  x28
 *    80  x29 (fp)
 *    88  x30 (return address, or cpu_thread_start)
 *    96  d8 - d15
 *
 * That is exactly the AAPCS64 callee-saved set: the general registers
 * x19-x28 plus fp/lr, and the low 64 bits of v8-v15. Everything else is
 * caller-saved. When a thread is suspended from a signal handler
 * (preemption), the full interrupted register state lives in the kernel
 * sigframe further up its stack.
 */

#define FRAME_SIZE 160

        .macro  save_callee_saved
        sub     sp, sp, #FRAME_SIZE
        stp     x19, x20, [sp, #0]
        stp     x21, x22, [sp, #16]
        stp     x23, x24, [sp, #32]
        stp     x25, x26, [sp, #48]
        stp     x27, x28, [sp, #64]
        stp     x29, x30, [sp, #80]
        stp     d8,  d9,  [sp, #96]
        stp     d10, d11, [sp, #112]
        stp     d12, d13, [sp, #128]
        stp     d14, d15, [sp, #144]
        .endm

        .macro  load_callee_saved
        ldp     x19, x20, [sp, #0]
        ldp     x21, x22, [sp, #16]
        ldp     x23, x24, [sp, #32]
        ldp     x25, x26, [sp, #48]
        ldp     x27, x28, [sp, #64]
        ldp     x29, x30, [sp, #80]
        ldp     d8,  d9,  [sp, #96]
        ldp     d10, d11, [sp, #112]
        ldp     d12, d13, [sp, #128]
        ldp     d14, d15, [sp, #144]
        add     sp, sp, #FRAME_SIZE
        .endm

        .text

        .global _start
_start:
        mov     x0, sp                  // -> argc, argv..., NULL, envp...
        mov     x29, #0
        mov     x30, #0
        bl      host_start
        brk     #0

// The kernel enters this with sp pointing at the signal frame, which is
// all rt_sigreturn needs (see SA_RESTORER in linux.h).
        .global __restore_rt
__restore_rt:
        mov     x8, #139                // rt_sigreturn
        svc     #0
        brk     #0

// void cpu_switch(void)
//   Save callee-saved registers, let task_switch() pick the next thread,
//   resume it. Called from irq_dispatch() at IRQ_LEVEL_SWITCH.
        .global cpu_switch
cpu_switch:
        save_callee_saved
        mov     x0, sp
        bl      task_switch
        mov     sp, x0
        load_callee_saved
        ret

// First instruction of a new thread, reached via the frame built by
// cpu_stack_init(). Drop to IRQ level 0 (exception return), then call
// entry(arg0..arg3) and hand the result to thread_exit().
        .global cpu_thread_start
cpu_thread_start:
        mov     x29, #0
        bl      cpu_thread_start_hook
        mov     x0, x21
        mov     x1, x22
        mov     x2, x23
        mov     x3, x24
        blr     x19
        blr     x20
        brk     #0

// void cpu_jump_stack(void *sp, void (*fn)(void))  -- noreturn
        .global cpu_jump_stack
cpu_jump_stack:
        and     x0, x0, #-16
        mov     sp, x0
        mov     x29, #0
        blr     x1
        brk     #0

// long linux_clone_thread(unsigned long flags, void *child_sp,
//                         void (*fn)(void *arg), void *arg)
//   Returns the tid in the parent. The child runs fn(arg) on child_sp
//   and exits when it returns. Used for simulation peers (sim.c).
        .global linux_clone_thread
linux_clone_thread:
        sub     x1, x1, #16
        stp     x2, x3, [x1]            // fn, arg, for the child to pick up
        mov     x2, #0                  // parent_tid
        mov     x3, #0                  // tls
        mov     x4, #0                  // child_tid
        mov     x8, #220                // clone
        svc     #0
        cbnz    x0, 1f
        // Child, running on child_sp - 16
        mov     x29, #0
        ldp     x2, x0, [sp], #16       // fn, arg
        blr     x2
        mov     x0, #0
        mov     x8, #93                 // exit
        svc     #0
        brk     #0
1:      ret

// void cpu_coswitch(void **save_sp, void *resume_sp)
//   Symmetric coroutine swap: save callee-saved regs + return address,
//   store sp to *save_sp, load sp from resume_sp, restore, return into
//   whoever last saved there. Used to hand the single CPU thread back and
//   forth between the host harness and the mios kernel in library mode.
        .global cpu_coswitch
cpu_coswitch:
        save_callee_saved
        mov     x2, sp
        str     x2, [x0]                // *save_sp = sp
        mov     sp, x1                  // sp = resume_sp
        load_callee_saved
        ret

        .section .note.GNU-stack,"",%progbits
