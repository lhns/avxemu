/*
 * sem_hw_call(sem_state *st, void *code): run the instruction at `code` natively with every
 * ymm/gpr/flags/mxcsr taken from *st and written back to it. Only the real-CPU side of the
 * semantics test calls this, never in check mode. `code` must end with a 6-byte
 * `jmp *0(%rip)` followed by the 8-byte address sem_hw_after. rsp is st->gpr[4] while the
 * instruction runs (a scratch buffer), so memory operands may use it.
 */
        .text
        .globl  sem_hw_call
        .type   sem_hw_call, @function
sem_hw_call:
        push    %rbx
        push    %rbp
        push    %r12
        push    %r13
        push    %r14
        push    %r15
        mov     %rsp, sem_saved_rsp(%rip)
        mov     %rdi, sem_cur(%rip)
        mov     %rsi, sem_code(%rip)
        .irp    n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        vmovups \n*32(%rdi), %ymm\n
        .endr
        ldmxcsr 648(%rdi)
        pushq   640(%rdi)
        popfq
        mov     512+8(%rdi), %rcx
        mov     512+16(%rdi), %rdx
        mov     512+24(%rdi), %rbx
        mov     512+40(%rdi), %rbp
        mov     512+48(%rdi), %rsi
        mov     512+64(%rdi), %r8
        mov     512+72(%rdi), %r9
        mov     512+80(%rdi), %r10
        mov     512+88(%rdi), %r11
        mov     512+96(%rdi), %r12
        mov     512+104(%rdi), %r13
        mov     512+112(%rdi), %r14
        mov     512+120(%rdi), %r15
        mov     512+0(%rdi), %rax
        mov     512+32(%rdi), %rsp
        mov     512+56(%rdi), %rdi
        jmp     *sem_code(%rip)

        .globl  sem_hw_after
sem_hw_after:
        mov     %rax, sem_rax(%rip)
        mov     sem_cur(%rip), %rax
        mov     %rcx, 512+8(%rax)
        mov     %rdx, 512+16(%rax)
        mov     %rbx, 512+24(%rax)
        mov     %rsp, 512+32(%rax)
        mov     %rbp, 512+40(%rax)
        mov     %rsi, 512+48(%rax)
        mov     %rdi, 512+56(%rax)
        mov     %r8,  512+64(%rax)
        mov     %r9,  512+72(%rax)
        mov     %r10, 512+80(%rax)
        mov     %r11, 512+88(%rax)
        mov     %r12, 512+96(%rax)
        mov     %r13, 512+104(%rax)
        mov     %r14, 512+112(%rax)
        mov     %r15, 512+120(%rax)
        mov     sem_rax(%rip), %rcx
        mov     %rcx, 512(%rax)
        mov     sem_saved_rsp(%rip), %rsp
        pushfq
        pop     %rcx
        mov     %rcx, 640(%rax)
        stmxcsr 648(%rax)
        .irp    n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        vmovups %ymm\n, \n*32(%rax)
        .endr
        vzeroupper
        pop     %r15
        pop     %r14
        pop     %r13
        pop     %r12
        pop     %rbp
        pop     %rbx
        ret
        .size   sem_hw_call, .-sem_hw_call

        .bss
        .align 8
sem_saved_rsp: .quad 0
sem_cur:       .quad 0
sem_code:      .quad 0
sem_rax:       .quad 0
        .section .note.GNU-stack,"",@progbits
