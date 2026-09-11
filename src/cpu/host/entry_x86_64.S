/*
 * Host CPU assembly: process entry, signal trampoline, context switch.
 *
 * Context switch frame (as pointed to by thread_t.t_sp when suspended):
 *
 *   [6] return address (into cpu_switch's caller, or cpu_thread_start)
 *   [5] rbp             (cpu_thread_start: thread_exit)
 *   [4] rbx             (cpu_thread_start: entry)
 *   [3] r12             (cpu_thread_start: arg0)
 *   [2] r13             (cpu_thread_start: arg1)
 *   [1] r14             (cpu_thread_start: arg2)
 *   [0] r15             (cpu_thread_start: arg3)  <- t_sp
 *
 * Everything else is caller-saved in the SysV ABI. When a thread is
 * suspended from a signal handler (preemption), the full interrupted
 * register state lives in the kernel sigframe further up its stack.
 */

        .text

        .global _start
_start:
        xor     %ebp, %ebp
        mov     %rsp, %rdi              // -> argc, argv..., NULL, envp...
        and     $-16, %rsp
        call    host_start
        ud2

        .global __restore_rt
__restore_rt:
        mov     $15, %eax               // rt_sigreturn
        syscall
        ud2

// void cpu_switch(void)
//   Save callee-saved registers, let task_switch() pick the next thread,
//   resume it. Called from irq_dispatch() at IRQ_LEVEL_SWITCH.
        .global cpu_switch
cpu_switch:
        push    %rbp
        push    %rbx
        push    %r12
        push    %r13
        push    %r14
        push    %r15
        mov     %rsp, %rdi
        sub     $8, %rsp                // 16-byte align for the call
        call    task_switch
        mov     %rax, %rsp
        pop     %r15
        pop     %r14
        pop     %r13
        pop     %r12
        pop     %rbx
        pop     %rbp
        ret

// First instruction of a new thread, reached via the frame built by
// cpu_stack_init(). Drop to IRQ level 0 (exception return), then call
// entry(arg0..arg3) and hand the result to thread_exit().
        .global cpu_thread_start
cpu_thread_start:
        and     $-16, %rsp
        call    cpu_thread_start_hook
        mov     %r12, %rdi
        mov     %r13, %rsi
        mov     %r14, %rdx
        mov     %r15, %rcx
        call    *%rbx
        mov     %rax, %rdi
        call    *%rbp
        ud2

// void cpu_jump_stack(void *sp, void (*fn)(void))  -- noreturn
        .global cpu_jump_stack
cpu_jump_stack:
        mov     %rdi, %rsp
        and     $-16, %rsp
        xor     %ebp, %ebp
        call    *%rsi
        ud2

// long linux_clone_thread(unsigned long flags, void *child_sp,
//                         void (*fn)(void *arg), void *arg)
//   Returns the tid in the parent. The child runs fn(arg) on child_sp
//   and exits when it returns. Used for simulation peers (sim.c).
        .global linux_clone_thread
linux_clone_thread:
        sub     $16, %rsi
        mov     %rdx, 0(%rsi)           // fn
        mov     %rcx, 8(%rsi)           // arg
        mov     $56, %eax               // clone
        xor     %edx, %edx              // parent_tid
        xor     %r10d, %r10d            // child_tid
        xor     %r8d, %r8d              // tls
        syscall
        test    %rax, %rax
        jnz     1f
        // Child
        xor     %ebp, %ebp
        pop     %rax
        pop     %rdi
        call    *%rax
        mov     $60, %eax               // exit
        xor     %edi, %edi
        syscall
        ud2
1:      ret

// void cpu_coswitch(void **save_sp, void *resume_sp)
//   Symmetric coroutine swap: save callee-saved regs + return address,
//   store rsp to *save_sp, load rsp from resume_sp, restore, return into
//   whoever last saved there. Used to hand the single CPU thread back and
//   forth between the host harness and the mios kernel in library mode.
        .global cpu_coswitch
cpu_coswitch:
        push    %rbp
        push    %rbx
        push    %r12
        push    %r13
        push    %r14
        push    %r15
        mov     %rsp, (%rdi)            // *save_sp = rsp
        mov     %rsi, %rsp             // rsp = resume_sp
        pop     %r15
        pop     %r14
        pop     %r13
        pop     %r12
        pop     %rbx
        pop     %rbp
        ret

        .section .note.GNU-stack,"",@progbits
