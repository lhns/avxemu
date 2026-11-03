#define _XOPEN_SOURCE 700
#include "platform/context.h"
#include <sys/ucontext.h>
#include <string.h>
#include <signal.h>

static uint64_t *gpr_ptr(_STRUCT_X86_THREAD_STATE64 *ss, int i) {
    switch (i) {
    case 0: return &ss->__rax; case 1: return &ss->__rcx; case 2: return &ss->__rdx;
    case 3: return &ss->__rbx; case 4: return &ss->__rsp; case 5: return &ss->__rbp;
    case 6: return &ss->__rsi; case 7: return &ss->__rdi; case 8: return &ss->__r8;
    case 9: return &ss->__r9; case 10:return &ss->__r10; case 11:return &ss->__r11;
    case 12:return &ss->__r12; case 13:return &ss->__r13; case 14:return &ss->__r14;
    default:return &ss->__r15;
    }
}

static int valid(const ucontext_t *uc, int vectors) {
    return uc && uc->uc_mcontext && uc->uc_mcsize >=
        (vectors ? sizeof(_STRUCT_MCONTEXT_AVX64) : sizeof(_STRUCT_MCONTEXT64));
}
int avxemu_context_signal_blocked(const void *context, int signal) {
    const ucontext_t *uc=context;
    return uc ? sigismember(&uc->uc_sigmask,signal) : -1;
}
uintptr_t avxemu_context_ip(const void *context) {
    const ucontext_t *uc=context;
    return valid(uc,0) ? uc->uc_mcontext->__ss.__rip : 0;
}
void avxemu_context_set_ip(void *context, uintptr_t ip) {
    ucontext_t *uc=context;
    if(valid(uc,0)) uc->uc_mcontext->__ss.__rip=ip;
}
int avxemu_context_read(const void *context, avxemu_regfile *rf, int vectors) {
    const ucontext_t *uc=context;
    if(!rf || !valid(uc,vectors)) return 0;
    _STRUCT_X86_THREAD_STATE64 *ss=&uc->uc_mcontext->__ss;
    for(int i=0;i<16;i++) rf->gpr[i]=*gpr_ptr(ss,i);
    rf->rflags=ss->__rflags; rf->rip=ss->__rip;
    if(vectors) {
        const _STRUCT_MCONTEXT_AVX64 *mc=(const _STRUCT_MCONTEXT_AVX64 *)uc->uc_mcontext;
        const _STRUCT_XMM_REG *xmm=&mc->__fs.__fpu_xmm0, *ymmh=&mc->__fs.__fpu_ymmh0;
        for(int i=0;i<16;i++) {
            memcpy(rf->ymm[i],&xmm[i],16); memcpy(rf->ymm[i]+16,&ymmh[i],16);
        }
    }
    return 1;
}
int avxemu_context_write(void *context, const avxemu_regfile *rf, int vectors) {
    ucontext_t *uc=context;
    if(!rf || !valid(uc,vectors)) return 0;
    _STRUCT_X86_THREAD_STATE64 *ss=&uc->uc_mcontext->__ss;
    for(int i=0;i<16;i++) *gpr_ptr(ss,i)=rf->gpr[i];
    ss->__rflags=rf->rflags; ss->__rip=rf->rip;
    if(vectors) {
        _STRUCT_MCONTEXT_AVX64 *mc=(_STRUCT_MCONTEXT_AVX64 *)uc->uc_mcontext;
        _STRUCT_XMM_REG *xmm=&mc->__fs.__fpu_xmm0, *ymmh=&mc->__fs.__fpu_ymmh0;
        for(int i=0;i<16;i++) {
            memcpy(&xmm[i],rf->ymm[i],16); memcpy(&ymmh[i],rf->ymm[i]+16,16);
        }
    }
    return 1;
}
int avxemu_context_mxcsr(const void *context, uint32_t *mxcsr) {
    const ucontext_t *uc=context;
    if(!mxcsr || !valid(uc,0)) return 0;
    *mxcsr=((const _STRUCT_MCONTEXT_AVX64 *)uc->uc_mcontext)->__fs.__fpu_mxcsr;
    return 1;
}
int avxemu_context_set_mxcsr(void *context, uint32_t mxcsr) {
    ucontext_t *uc=context;
    if(!valid(uc,0)) return 0;
    ((_STRUCT_MCONTEXT_AVX64 *)uc->uc_mcontext)->__fs.__fpu_mxcsr=mxcsr;
    return 1;
}
