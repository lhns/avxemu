#define _GNU_SOURCE
#include "platform/context.h"
#include "xstate.h"
#include <string.h>
#include <signal.h>
#include <ucontext.h>

static const int registers[16] = {
    REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
    REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15
};

int avxemu_context_signal_blocked(const void *context, int signal) {
    const ucontext_t *uc=context;
    return uc ? sigismember(&uc->uc_sigmask,signal) : -1;
}
uintptr_t avxemu_context_ip(const void *context) {
    const ucontext_t *uc=context;
    return uc ? (uintptr_t)uc->uc_mcontext.gregs[REG_RIP] : 0;
}
void avxemu_context_set_ip(void *context, uintptr_t ip) {
    ucontext_t *uc=context;
    if (uc) uc->uc_mcontext.gregs[REG_RIP]=(greg_t)ip;
}
int avxemu_context_read(const void *context, avxemu_regfile *rf, int vectors) {
    const ucontext_t *uc=context;
    avxemu_xstate state;
    if (!uc || !rf || (vectors && !avxemu_xstate_live(uc->uc_mcontext.fpregs,&state))) return 0;
    for (int i=0; i<16; i++) rf->gpr[i]=uc->uc_mcontext.gregs[registers[i]];
    rf->rip=uc->uc_mcontext.gregs[REG_RIP];
    rf->rflags=uc->uc_mcontext.gregs[REG_EFL];
    if (vectors) for (int i=0; i<16; i++) {
        if (state.active & 2) memcpy(rf->ymm[i],state.xmm+16*i,16);
        else memset(rf->ymm[i],0,16);
        if (state.active & 4) memcpy(rf->ymm[i]+16,state.ymmh+16*i,16);
        else memset(rf->ymm[i]+16,0,16);
    }
    return 1;
}
int avxemu_context_write(void *context, const avxemu_regfile *rf, int vectors) {
    ucontext_t *uc=context;
    avxemu_xstate state;
    if (!uc || !rf || (vectors && !avxemu_xstate_live(uc->uc_mcontext.fpregs,&state))) return 0;
    for (int i=0; i<16; i++) uc->uc_mcontext.gregs[registers[i]]=(greg_t)rf->gpr[i];
    uc->uc_mcontext.gregs[REG_RIP]=(greg_t)rf->rip;
    uc->uc_mcontext.gregs[REG_EFL]=(greg_t)rf->rflags;
    if (vectors) {
        for (int i=0; i<16; i++) {
            memcpy(state.xmm+16*i,rf->ymm[i],16);
            memcpy(state.ymmh+16*i,rf->ymm[i]+16,16);
        }
        /* Even initially inactive components must be restored by sigreturn.
         * Keep every unrelated active component, including x87, intact. */
        state.active |= 6;
        memcpy(state.header,&state.active,sizeof state.active);
    }
    return 1;
}
int avxemu_context_mxcsr(const void *context, uint32_t *mxcsr) {
    const ucontext_t *uc=context;
    if (!uc || !mxcsr || !uc->uc_mcontext.fpregs) return 0;
    *mxcsr=uc->uc_mcontext.fpregs->mxcsr;
    return 1;
}
int avxemu_context_set_mxcsr(void *context, uint32_t mxcsr) {
    ucontext_t *uc=context;
    if (!uc || !uc->uc_mcontext.fpregs) return 0;
    uc->uc_mcontext.fpregs->mxcsr=mxcsr;
    return 1;
}
