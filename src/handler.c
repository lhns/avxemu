/*
 * handler.c — SIGILL trap-and-emulate.
 *
 * On #UD we decode the faulting instruction, pull its operands from the
 * signal's AVX mcontext (xmm + ymmh halves, GPRs, rflags), run the SSE-only
 * executor, write results back, and advance RIP past the instruction. Faults
 * we don't recognise (genuine ud2 asserts etc.) chain to the previous handler.
 *
 * The 256-bit register state lives in the mcontext on an AVX-capable CPU
 * (which the target is), so no software shadow register file is needed.
 */

#define _XOPEN_SOURCE 700
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <xmmintrin.h>
#include <fcntl.h>       /* open() in faulthist_dump */
#include <stdio.h>       /* rename() in faulthist_dump */

#include "decode.h"
#include "regs.h"
#include "lde.h"
#include "avxemu_cpuid.h"
#include "regfile.h"
#include "lcache.h"
#include "image.h"
#include "platform/host.h"
#include "platform/context.h"
#include "platform/loader.h"
#include "platform/memory.h"


/*
 * Fault-driven relocation trigger (Task C). A 4-byte isolated scalar BMI op that
 * the eager trampoliner couldn't patch (no room for a 5-byte jmp) keeps trapping
 * forever at ~57us/round-trip. We count #UDs per RIP and, once a RIP crosses a
 * threshold, relocate its block (avxemu_relocate_block) so it stops faulting. The
 * table is a fixed open-addressed RIP->site map (no allocation in the handler).
 *
 * Every thread faults into this handler, so the table is shared: slots are
 * claimed with a CAS and counted with atomic adds, and never freed. What a
 * site's relocation needs to prove, build or write happens under g_reloc_lock,
 * which also covers the state reloc.c and tramp.c keep in statics (the pool
 * cursor, the patch-safety scratch map). Holders are all inside this handler
 * doing bounded work, so a waiter spins briefly — and never on itself: signals
 * are blocked while it is held, so no other handler can interrupt the holder
 * and fault its way back in here.
 */
#define HOT_SLOTS 4096
#define HOT_THRESHOLD 50          /* faults at one RIP before we relocate it */
enum { SITE_NEW, SITE_RELOCATABLE, SITE_NEVER };
static struct {
    uint64_t rip;
    uint32_t n;
    volatile uint8_t state;       /* SITE_*; written under g_reloc_lock */
    volatile uint8_t patched;     /* set by reloc.c just before the jmp lands */
    uint8_t *resume;              /* RELOCATABLE: where an emulation resumes */
} g_hot[HOT_SLOTS];
static int g_reloc_enabled = 1;   /* AVXEMU_RELOC=0 disables (A/B) */
static volatile int g_reloc_lock;

static void reloc_lock(sigset_t *saved) {
    sigset_t all; sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, saved);
    while (__sync_lock_test_and_set(&g_reloc_lock, 1))
        while (g_reloc_lock) __asm__ volatile("pause");
}
static void reloc_unlock(const sigset_t *saved) {
    __sync_lock_release(&g_reloc_lock);
    pthread_sigmask(SIG_SETMASK, saved, 0);
}

extern int avxemu_reloc_prepare(uint8_t *site, uint8_t **resume);

extern int avxemu_reloc_commit(uint8_t *site, uint8_t *resume, volatile uint8_t *patched);

/* AVXEMU_FAULTHIST=1 diagnostic: periodically dump the g_hot RIP->count table
 * (i.e. every site still faulting into the handler) to /tmp/faulthist.out so a
 * live spin can be probed for WHICH sites fault repeatedly (eager-scan misses,
 * relocation declines, JIT-emitted code). Async-signal-safe: open/write/close
 * + static buffers only, atomic rename like ophist. */
static int g_faulthist_on = -1;
static uint64_t g_faulthist_total;
static int faulthist_enabled(void){
    if (g_faulthist_on < 0){ const char *e = getenv("AVXEMU_FAULTHIST"); g_faulthist_on = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return g_faulthist_on;
}
static void fh_u64(int fd, uint64_t v){
    char b[24]; int i = 24;
    if (v == 0){ (void)!write(fd, "0", 1); return; }
    while (v){ b[--i] = (char)('0' + (v % 10)); v /= 10; }
    (void)!write(fd, b + i, (size_t)(24 - i));
}
static void fh_hex(int fd, uint64_t v){
    char b[18]; int i = 18;
    if (v == 0){ (void)!write(fd, "0x0", 3); return; }
    while (v){ int d = (int)(v & 0xF); b[--i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
    b[--i] = 'x'; b[--i] = '0';
    (void)!write(fd, b + i, (size_t)(18 - i));
}
/* AVXEMU_FAULTSNAP: register+string snapshot from inside the handler. */
static int g_faultsnap_on = -1;
static uint64_t g_faultsnap_total;
static int faultsnap_enabled(void){
    if (g_faultsnap_on < 0){ const char *e = getenv("AVXEMU_FAULTSNAP"); g_faultsnap_on = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return g_faultsnap_on;
}
static void faultsnap_dump(uint64_t rip, const avxemu_regfile *rf){
    int fd = open("/tmp/faultsnap.out", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    /* seq + monotonic time in the header so recurrence ACROSS the run is measurable */
    (void)!write(fd, "--- snap ", 9); fh_u64(fd, g_faultsnap_total);
    (void)!write(fd, " t ", 3); fh_u64(fd, avxemu_host_ticks());
    (void)!write(fd, " rip ", 5); fh_hex(fd, rip); (void)!write(fd, "\n", 1);
    for (int i = 0; i < 16; i++){
        uint64_t v = rf->gpr[i];
        (void)!write(fd, "r", 1); fh_u64(fd, (uint64_t)i); (void)!write(fd, "=", 1); fh_hex(fd, v);
        /* plausible user pointer? try a guarded read and print printable runs */
        if (v >= 0x100000000ull && v < 0x800000000000ull){
            uint8_t buf[128];
            if (avxemu_memory_read(buf, (uintptr_t)(v & ~0xFull), sizeof buf) == sizeof buf){
                int run = 0;
                for (int k = 0; k <= (int)sizeof buf; k++){
                    int printable = k < (int)sizeof buf && buf[k] >= 0x20 && buf[k] < 0x7F;
                    if (printable) run++;
                    else {
                        if (run >= 6){ (void)!write(fd, " \"", 2); (void)!write(fd, buf + k - run, (size_t)run); (void)!write(fd, "\"", 1); }
                        run = 0;
                    }
                }
            }
        }
        (void)!write(fd, "\n", 1);
    }
    close(fd);
}

/* declined-relocation log: (rip, reason) recorded once per site, when its
 * relocation is first found impossible (at its first fault, or at the commit). Reasons (reloc.c RELOC_DECLINE):
 * 1=pool-init 2=decode 3=not-faulting 4=window 5=patch-safety 6=pool-alloc
 * 7=rel32-range 8=site-write 9=jmp-straddles-cache-line */
extern int avxemu_reloc_last_reason;
static struct { uint64_t rip; int reason; } g_declined[512];
static int g_ndeclined;
static void declined_log(uint64_t rip, int reason){
    if (g_ndeclined < (int)(sizeof g_declined / sizeof g_declined[0]))
        { g_declined[g_ndeclined].rip = rip; g_declined[g_ndeclined].reason = reason; g_ndeclined++; }
}
static void faulthist_dump(void){
    int fd = open("/tmp/faulthist.out.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    static const char hdr[] = "=== AVXEMU_FAULTHIST: still-faulting RIPs (count desc, top 48) ===\n";
    (void)!write(fd, hdr, sizeof hdr - 1);
    /* top-48 selection over the fixed table; O(48*4096) compares, diagnostic-only */
    uint64_t seen_max = ~0ull; uint32_t seen_n = ~0u;
    for (int rank = 0; rank < 48; rank++){
        uint32_t bn = 0; uint64_t brip = 0;
        for (int k = 0; k < HOT_SLOTS; k++){
            uint32_t n = g_hot[k].n; uint64_t r = g_hot[k].rip;
            if (!r || n > seen_n || (n == seen_n && r >= seen_max)) continue;
            if (n > bn || (n == bn && r > brip)){ bn = n; brip = r; }
        }
        if (!brip) break;
        fh_hex(fd, brip); (void)!write(fd, "\t", 1); fh_u64(fd, bn); (void)!write(fd, "\n", 1);
        seen_n = bn; seen_max = brip;
    }
    (void)!write(fd, "TOTAL_FAULTS\t", 13); fh_u64(fd, g_faulthist_total); (void)!write(fd, "\n", 1);
    static const char dh[] = "=== relocation DECLINES (rip, reason: 1=pool-init 2=decode 3=not-faulting 4=window 5=patch-safety 6=pool-alloc 7=rel32 8=site-write 9=line-straddle) ===\n";
    (void)!write(fd, dh, sizeof dh - 1);
    for (int i = 0; i < g_ndeclined; i++){
        fh_hex(fd, g_declined[i].rip); (void)!write(fd, "\tR", 2);
        fh_u64(fd, (uint64_t)g_declined[i].reason); (void)!write(fd, "\n", 1);
    }
    close(fd);
    (void)rename("/tmp/faulthist.out.tmp", "/tmp/faulthist.out");
}
/* The site's slot if it has one, without claiming. */
static int site_lookup(uint64_t rip) {
    uint32_t i = (uint32_t)((rip * 0x9e3779b97f4a7c15ull) >> 52) & (HOT_SLOTS - 1);
    for (int probe = 0; probe < 8; probe++) {
        uint32_t k = (i + probe) & (HOT_SLOTS - 1);
        uint64_t r = g_hot[k].rip;
        if (r == rip) return (int)k;
        if (r == 0) return -1;
    }
    return -1;
}

/* The site's slot, claimed if new; -1 if its probe run is full (then the site
 * is simply never relocated, which is always safe). */
static int site_slot(uint64_t rip) {
    uint32_t i = (uint32_t)((rip * 0x9e3779b97f4a7c15ull) >> 52) & (HOT_SLOTS - 1);
    for (int probe = 0; probe < 8; probe++) {
        uint32_t k = (i + probe) & (HOT_SLOTS - 1);
        uint64_t r = g_hot[k].rip;
        if (r == rip) return (int)k;
        if (r == 0) {
            r = __sync_val_compare_and_swap(&g_hot[k].rip, 0, rip);
            if (r == 0 || r == rip) return (int)k;
        }
    }
    return -1;
}

/*
 * Test hook: on an AVX2 host the target instructions don't fault, so the
 * fault-injection harness prefixes each one with `ud2`. When this is set, a
 * #UD whose bytes are 0F 0B is treated as "decode the instruction that follows
 * and skip past both." Zero in production (real #UD points straight at the
 * unsupported instruction).
 */
int avxemu_test_ud2 = 0;

static void emit(const char *s){ (void)!write(2, s, strlen(s)); }

extern void avxemu_selftest(void);   /* selftest.c */
extern long avxemu_install_trampolines(void); /* tramp.c */

/* ---- crash handler state (for the SIMD over-read fixup) ---- */

static int g_fixup_overread = 0;        /* map a zero page on a SIMD over-read fault */
static unsigned long g_overread_pages = 0;

/*
 * Optimized SIMD code (in Bun's native code and in ours) reads a full vector
 * past a buffer's logical end, relying on the trailing page being mapped — true
 * on the AVX2 machines this code was written for, but on the target that page
 * can be unmapped. If a fault lands on an UNMAPPED GAP whose immediately
 * preceding page IS mapped (an over-read just off the end of a mapped region,
 * not a wild pointer and not a PROT_NONE guard page), map a zero page there and
 * let the instruction retry — reproducing the AVX2 machine's behaviour. The
 * over-read bytes are not used by correct code. Returns 1 if patched.
 */
static int try_fixup_overread(uint64_t fa){
    if (!g_fixup_overread || !fa) return 0;
    size_t page = avxemu_page_size();
    uint64_t pg = fa - fa % page;
    if (fa % page > 256) return 0;              /* deep in the page: not a just-past-the-edge over-read */
    avxemu_mapping mapping;
    /* Only a verified gap after a known mapping is eligible. */
    if (avxemu_memory_query((uintptr_t)pg, &mapping) != 0) return 0;
    if (!pg || avxemu_memory_query((uintptr_t)pg - 1, &mapping) != 1) return 0;
    if (g_overread_pages > 200000) return 0;       /* runaway guard */
    if (!avxemu_memory_map_gap((uintptr_t)pg, page)) return 0;
    g_overread_pages++;
    return 1;
}

static void on_sigill(int sig, siginfo_t *info, void *uctx);
static void on_crash(int sig, siginfo_t *info, void *uctx);

/* The kernel supplies application masking and restart behavior even while our
 * front-line entry remains installed. One-shot reset belongs to chain_action:
 * an emulated instruction must not consume the application's disposition. */
static int install_owned(int sig, const struct sigaction *app) {
    struct sigaction sa;
    memset(&sa,0,sizeof sa);
    sa.sa_sigaction = sig==SIGILL ? on_sigill : on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    /* An inactive disposition can retain masks/flags across exec. They do not
     * describe a callback and must not block faults raised by our emulator. */
    if (app->sa_handler!=SIG_DFL && app->sa_handler!=SIG_IGN) {
        sa.sa_mask = app->sa_mask;
        sa.sa_flags |= app->sa_flags & (SA_RESTART|SA_NODEFER|SA_ONSTACK);
    }
    if (sig!=SIGILL) sa.sa_flags |= SA_ONSTACK;
    return avxemu_real_sigaction(sig,&sa,0);
}

/* Writers never modify the active record. Kernel registration and publication
 * are serialized with all catchable signals blocked on the acquiring thread,
 * so an asynchronous handler cannot interrupt its own lock holder. Only trusted
 * baseline-ISA copies and genuine signal operations run inside the section.
 * Callbacks always run after releasing it.
 *
 * Fork can copy a held lock or an unfinished write. The active record remains a
 * complete committed disposition in either case. PID change elects one child
 * recovery thread, which repairs kernel registrations from those records before
 * releasing other child callers. This needs no application atfork ordering. */
typedef struct { struct sigaction action; int owned; } policy_state;
/* Both records and their publication/recovery fields occupy one aligned 4 KiB
 * unit. x86-64 Darwin/Linux pages are multiples of this unit, so fork/COW cannot
 * capture an index on a different page snapshot from its records. Three slots
 * cost 12 KiB of zero-initialized storage. Active records are never modified. */
typedef struct __attribute__((aligned(4096))) {
    policy_state states[2];
    volatile unsigned active;
    volatile int installing;
} policy_slot;
_Static_assert(sizeof(policy_slot)==4096, "policy slot must fit one 4 KiB unit");
_Static_assert(_Alignof(policy_slot)==4096, "policy slot must be page-local");
static policy_slot g_policy[3];
static const int g_policy_signals[3]={SIGILL,SIGSEGV,SIGBUS};
static volatile int g_policy_lock;
static volatile pid_t g_policy_pid;

static policy_slot *policy_for(int sig) {
    for(int i=0;i<3;i++) if(g_policy_signals[i]==sig) return &g_policy[i];
    return 0;
}
static policy_state policy_current(const policy_slot *slot) {
    unsigned active=__sync_fetch_and_add((volatile unsigned *)&slot->active,0);
    return slot->states[active];
}
static void policy_publish(policy_slot *slot, const struct sigaction *action, int owned) {
    unsigned next=slot->active^1;
    slot->states[next].action=*action;
    slot->states[next].owned=owned;
    __sync_synchronize();
    __sync_lock_test_and_set(&slot->active,next);
}
static void policy_process(void) {
    pid_t pid=getpid();
    for(;;) {
        pid_t seen=__sync_val_compare_and_swap(&g_policy_pid,0,0);
        if(seen==pid) return;
        if(!seen) {
            if(__sync_bool_compare_and_swap(&g_policy_pid,0,pid)) return;
        } else if(seen==-pid) {
            __asm__ volatile("pause");
        } else if(__sync_bool_compare_and_swap(&g_policy_pid,seen,-pid)) {
            for(int i=0;i<3;i++) {
                policy_state committed=policy_current(&g_policy[i]);
                if(committed.owned) install_owned(g_policy_signals[i],&committed.action);
                else if(g_policy[i].installing)
                    avxemu_real_sigaction(g_policy_signals[i],&committed.action,0);
                g_policy[i].installing=0;
            }
            __sync_lock_release(&g_policy_lock);
            __sync_synchronize();
            __sync_lock_test_and_set(&g_policy_pid,pid);
            return;
        }
    }
}
static void policy_enter(sigset_t *saved) {
    sigset_t all; sigfillset(&all);
    pthread_sigmask(SIG_BLOCK,&all,saved);
    policy_process();
    while(__sync_lock_test_and_set(&g_policy_lock,1))
        while(g_policy_lock) __asm__ volatile("pause");
}
static void policy_leave(const sigset_t *saved) {
    int error=errno;
    __sync_lock_release(&g_policy_lock);
    pthread_sigmask(SIG_SETMASK,saved,0);
    errno=error;
}
static int policy_owned(int sig) {
    sigset_t saved; policy_enter(&saved);
    int owned=policy_current(policy_for(sig)).owned;
    policy_leave(&saved);
    return owned;
}
static int policy_take(int sig) {
    sigset_t saved; policy_enter(&saved);
    policy_slot *slot=policy_for(sig);
    if(!slot) { policy_leave(&saved); errno=EINVAL; return -1; }
    if(policy_current(slot).owned) { policy_leave(&saved); return 0; }
    struct sigaction app;
    int result=avxemu_real_sigaction(sig,0,&app);
    if(!result) {
        /* Child recovery can roll back an interrupted first installation. */
        policy_publish(slot,&app,0);
        slot->installing=1;
        result=install_owned(sig,&app);
        if(!result) policy_publish(slot,&app,1);
        slot->installing=0;
    }
    policy_leave(&saved);
    return result;
}
static void policy_release(int sig) {
    sigset_t saved; policy_enter(&saved);
    policy_slot *slot=policy_for(sig);
    policy_state app=policy_current(slot);
    if(!avxemu_real_sigaction(sig,&app.action,0)) policy_publish(slot,&app.action,0);
    policy_leave(&saved);
}

static void chain_action(int sig, siginfo_t *info, void *uctx, policy_slot *slot) {
    sigset_t saved; policy_enter(&saved);
    struct sigaction app=policy_current(slot).action;
    if(app.sa_handler==SIG_DFL) {
        /* Keep a concurrent registration from replacing the default between
         * restoring it and delivering this already-claimed fatal signal. */
        avxemu_real_sigaction(sig,&app,0);
        sigset_t unblock; sigemptyset(&unblock); sigaddset(&unblock,sig);
        pthread_sigmask(SIG_UNBLOCK,&unblock,0);
        raise(sig);
        policy_leave(&saved);
        return;
    }
    if(app.sa_handler!=SIG_IGN && (app.sa_flags&SA_RESETHAND)) {
        struct sigaction reset; memset(&reset,0,sizeof reset);
        reset.sa_handler=SIG_DFL; sigemptyset(&reset.sa_mask);
        if(install_owned(sig,&reset)) { policy_leave(&saved); return; }
        policy_publish(slot,&reset,1);
    }
    policy_leave(&saved);
    /* A signal already in flight can retain the preceding kernel mask/restart
     * settings; the application disposition is claimed atomically above. */
    if(app.sa_handler==SIG_IGN) return;
    if(app.sa_flags&SA_SIGINFO) app.sa_sigaction(sig,info,uctx);
    else app.sa_handler(sig);
}

/*
 * SIGSEGV/SIGBUS front-line handler. Its sole job is the SIMD over-read fixup
 * above; anything it can't fix it passes straight to the previous handler so a
 * genuine crash still reports exactly as it would have.
 */
static void on_crash(int sig, siginfo_t *info, void *uctx){
    uint64_t crip = avxemu_context_ip(uctx);
    uint64_t fa = info ? (uint64_t)info->si_addr : 0;

    /* Data over-read off the end of a mapped region -> map a zero page, retry.
     * Skip code-fetch faults (fa==rip): those are wild jumps, not over-reads. */
    if ((sig==SIGSEGV || sig==SIGBUS) && fa != crip && try_fixup_overread(fa)) return;

    chain_action(sig, info, uctx, policy_for(sig));
}

static void chain(int sig, siginfo_t *info, void *uctx) {
    chain_action(sig, info, uctx, policy_for(sig));
}

static void on_sigill(int sig, siginfo_t *info, void *uctx) {
    avxemu_regfile rf;
    /* User-generated signals belong to the application, regardless of which
     * instruction happens to be interrupted. */
    if (avxemu_signal_generated(info)) {
        chain(sig, info, uctx); return;
    }
    if (!avxemu_context_read(uctx, &rf, 0)) { chain(sig, info, uctx); return; }
    if (avxemu_cpuid_handle(&rf)) {
        if (!avxemu_context_write(uctx, &rf, 0)) chain(sig, info, uctx);
        return;
    }
    if (!avxemu_context_read(uctx, &rf, 1)) { chain(sig, info, uctx); return; }
    uint64_t rip = rf.rip;
    uint64_t base = rip;
    const uint8_t *ip = (const uint8_t *)rip;
    if (avxemu_test_ud2 && ip[0] == 0x0F && ip[1] == 0x0B) base = rip + 2;  /* skip injected ud2 */
    const uint8_t *bp = (const uint8_t *)base;
    decoded d;
    int dl = decode(bp, &d);
    /* Another thread may have patched this site between our fault and our read
     * of it: we then decoded its jmp (or, mid-store, a mix) rather than the op
     * that faulted. reloc.c raises `patched` before the store, so reading it
     * AFTER decoding tells us which — and the answer is simply to re-execute.
     * Checked before chain(): the jmp is ours, not a genuine #UD. */
    __asm__ volatile("" ::: "memory");             /* decode's reads stay before this one */
    if (g_reloc_enabled) {
        int k = site_lookup(base);
        if (k >= 0 && g_hot[k].patched) { avxemu_context_set_ip(uctx, base); return; }
    }
    if (!dl) { chain(sig, info, uctx); return; }   /* genuine #UD: let it surface */
    uint64_t rip_next = base + d.len;

    /* AVXEMU_FAULTHIST diagnostic: snapshot the still-faulting-RIP table periodically */
    if (faulthist_enabled() && ((++g_faulthist_total & 0x7FFF) == 0)) faulthist_dump();

    /* AVXEMU_FAULTSNAP diagnostic: every 1024th fault, dump rip + GPRs and any
     * printable string data the pointer-looking registers reach (guarded reads,
     * in-process — works where a debugger can't attach through the fault stream).
     * Identifies WHAT data the hot loop is chewing on. */
    if (faultsnap_enabled() && ((++g_faultsnap_total & 0x3FF) == 0))
        faultsnap_dump(base, &rf);

    /*
     * Fault-driven relocation — relocate BEFORE emulate. Once `base` has trapped
     * HOT_THRESHOLD times, overwrite [base, base+5) with a `jmp rel32` into a
     * relocated block that runs the op (natively/emulated) and jumps back.
     *
     * Why re-enter at `base` (not rip_next): the jmp we just wrote lives AT base
     * and MUST execute. Advancing RIP would skip it; worse, for a 4-byte faulting
     * op rip_next = base+4 lands on the LAST byte of the 5-byte E9 jmp, so the
     * kernel would resume one byte INSIDE the jmp and decode garbage (a
     * deterministic crash on the 50th hit of every such site). Re-entering at base
     * runs the op exactly once, via the patched jmp.
     *
     * Why relocate-before-emulate (not emulate -> write back -> rip=base): that
     * naive ordering DOUBLE-APPLIES dst==src ops. E.g. `tzcnt eax,eax`: emulate
     * sets eax=f(eax_orig), then re-entering the relocated block computes
     * f(f(eax_orig)). By relocating first and NOT emulating on success, the op
     * runs exactly once.
     *
     * avxemu_relocate_block reads only the instruction bytes at base (it
     * re-decodes and emits code); it never needs the emulated result, so calling
     * it before emulation is correct. If it declines (unsafe site, non-relocatable
     * op, or pool exhausted) we fall through and emulate this instance normally — a
     * declined site keeps faulting + emulating forever (the safe floor).
     *
     * Gating matches the old post-write-back trigger, just moved earlier: reached
     * only on the normal emulated path (the cpuid trap and every unrecognised-#UD
     * chain() return earlier), using the real faulting address `base`. One
     * thread commits; any others that crossed the threshold with it see
     * `patched` and re-enter too.
     */
    int slot = g_reloc_enabled ? site_slot(base) : -1;
    if (slot >= 0) {
        if (g_hot[slot].state == SITE_NEW) {           /* first fault here: can it ever relocate? */
            sigset_t saved; reloc_lock(&saved);
            if (g_hot[slot].state == SITE_NEW) {
                uint8_t *resume = 0;
                if (avxemu_reloc_prepare((uint8_t *)base, &resume)) {
                    g_hot[slot].resume = resume;
                    __sync_synchronize();
                    g_hot[slot].state = SITE_RELOCATABLE;
                } else {
                    g_hot[slot].state = SITE_NEVER;
                    if (faulthist_enabled()) declined_log(base, avxemu_reloc_last_reason);
                }
            }
            reloc_unlock(&saved);
        }
        uint32_t n = __sync_add_and_fetch(&g_hot[slot].n, 1);
        if (g_hot[slot].state == SITE_RELOCATABLE && n >= HOT_THRESHOLD) {
            sigset_t saved; reloc_lock(&saved);
            int won = 0;
            if (g_hot[slot].state == SITE_RELOCATABLE && !g_hot[slot].patched) {
                won = avxemu_reloc_commit((uint8_t *)base, g_hot[slot].resume, &g_hot[slot].patched);
                if (!won) {
                    g_hot[slot].state = SITE_NEVER;
                    if (faulthist_enabled()) declined_log(base, avxemu_reloc_last_reason);
                }
            }
            reloc_unlock(&saved);
            if (won || g_hot[slot].patched) {
                avxemu_context_set_ip(uctx, base);   /* re-enter: the patched jmp runs the op once */
                return;
            }
            /* declined -> fall through and emulate this instance normally */
        }
    }

    /* Snapshot the signal's machine state into a register-file, run the shared
     * core, and write the (possibly modified) state back. The same core runs
     * from the trampoline path, so there is exactly one emulation code path. */
    rf.rip = base;

    /* Application callback masks apply to chaining, not to an instruction we
     * recognize. Only remove BUS/SEGV blocking added by this SIGILL delivery;
     * signals already blocked by the interrupted caller stay blocked. */
    sigset_t operand_unblock, handler_mask;
    sigemptyset(&operand_unblock);
    int bus_blocked=avxemu_context_signal_blocked(uctx,SIGBUS);
    int segv_blocked=avxemu_context_signal_blocked(uctx,SIGSEGV);
    if (bus_blocked<0 || segv_blocked<0) { chain(sig,info,uctx); return; }
    if (!bus_blocked) sigaddset(&operand_unblock,SIGBUS);
    if (!segv_blocked) sigaddset(&operand_unblock,SIGSEGV);
    if (pthread_sigmask(SIG_UNBLOCK,&operand_unblock,&handler_mask)) {
        chain(sig,info,uctx); return;
    }
    /* Emulate under the application's rounding/FTZ/DAZ, not the handler's reset
     * MXCSR, with all exceptions masked; sticky flags go back to the application. */
    uint32_t app_csr = 0, host_csr = _mm_getcsr();
    int have_csr = avxemu_context_mxcsr(uctx, &app_csr);
    if (have_csr) _mm_setcsr(app_csr | 0x1F80u);
    int emulated=avxemu_emulate(&d,&rf);
    uint32_t raised = have_csr ? (_mm_getcsr() & 0x3Fu) : 0;
    _mm_setcsr(host_csr);
    pthread_sigmask(SIG_SETMASK,&handler_mask,0);
    if (!emulated) { chain(sig, info, uctx); return; }

    /* A prepared site resumes at its out-of-line tail: the following bytes
     * may become part of a later five-byte jump. */
    rf.rip = rip_next;
    if (!avxemu_context_write(uctx, &rf, 1)) { chain(sig, info, uctx); return; }
    if (raised) (void)avxemu_context_set_mxcsr(uctx, app_csr | raised);
    if (slot >= 0 && g_hot[slot].state == SITE_RELOCATABLE)
        avxemu_context_set_ip(uctx, (uintptr_t)g_hot[slot].resume);
}

/*
 * Stay the front-line SIGILL handler. Runtimes like Bun install their own crash
 * reporter on SIGILL during startup; if it wins, our emulated instructions #UD
 * straight into "illegal instruction" crash reports. We interpose sigaction()
 * (and signal()): a SIGILL registration is recorded as our chain target instead
 * of replacing us, so the runtime's reporter still fires for genuine #UDs while
 * we keep first crack at the ones we emulate. Everything else passes through.
 */
int avxemu_sigaction(int sig, const struct sigaction *act, struct sigaction *old){
    /* Resolution may enumerate loader metadata; it must precede the lock. */
    if (!avxemu_loader_resolve()) { errno=ENOSYS; return -1; }
    sigset_t saved; policy_enter(&saved);
    policy_slot *slot=policy_for(sig);
    policy_state previous;
    if(slot) previous=policy_current(slot);
    if(!slot || !previous.owned) {
        int result=avxemu_real_sigaction(sig,act,old);
        policy_leave(&saved);
        return result;
    }
    /* act and old may alias. Publish only after kernel success, under the same
     * lock as snapshots, competing replacements and one-shot claims. */
    if(act) {
        struct sigaction replacement=*act;
        if(install_owned(sig,&replacement)) { policy_leave(&saved); return -1; }
        policy_publish(slot,&replacement,1);
    }
    if(old) *old=previous.action;
    policy_leave(&saved);
    return 0;
}
void (*avxemu_signal(int sig, void (*h)(int)))(int){
    struct sigaction a, o; memset(&a,0,sizeof a);
    a.sa_handler = h; a.sa_flags = SA_RESTART; sigemptyset(&a.sa_mask);
    if (avxemu_sigaction(sig, &a, &o) != 0) return SIG_ERR;
    return (o.sa_flags & SA_SIGINFO) ? (void(*)(int))o.sa_sigaction : o.sa_handler;
}

/*
 * Does this CPU already have everything we emulate? Then there is nothing for us
 * to do, and doing it anyway is not free: the cpuid pass alone decodes the whole
 * of __text (~360 ms for Claude Code) because we advertise AVX2 regardless, every
 * cpuid then traps, and we take ownership of SIGILL. Returning early makes a
 * linked avxemu inert on such a CPU -- a loaded dylib and nothing else.
 * Asked of the raw CPU. The force/diagnostic knobs, which exist to exercise us on
 * a capable host, keep us installed. AVXEMU_ASSUME_CAPABLE is the test hook that
 * lets a CPU without these features check the early return.
 */
static int g_force_install;   /* avxemu_force_install: the test programs that drive the handler */
static int cpu_has_everything(void) {
    if (g_force_install) return 0;
    if (getenv("AVXEMU_FORCEPATCH") || getenv("AVXEMU_FORCETRAMP") || getenv("AVXEMU_SELFTEST") ||
        getenv("AVXEMU_FAKE_CPUID") || getenv("AVXEMU_CPUID_SET") || getenv("AVXEMU_CPUID_CLR"))
        return 0;
    if (getenv("AVXEMU_ASSUME_CAPABLE")) return 1;
    uint32_t r1[4], r7[4] = {0}, r8[4];
    if (!avxemu_get_cpuid(1, 0, r1)) return 0;
    (void)avxemu_get_cpuid(7, 0, r7);
    if (!avxemu_get_cpuid(0x80000001u, 0, r8)) return 0;
    const uint32_t l1  = (1u<<12) | (1u<<22) | (1u<<29);   /* FMA, MOVBE, F16C */
    const uint32_t l7  = (1u<<3)  | (1u<<5)  | (1u<<8);    /* BMI1, AVX2, BMI2 */
    const uint32_t l81 = (1u<<5);                           /* LZCNT */
    return (r1[2] & l1) == l1 && (r7[1] & l7) == l7 && (r8[2] & l81) == l81;
}

__attribute__((constructor))
static void avxemu_install(void) {
    /* Even an inert image can forward sigaction from an application handler.
     * Prepare genuine functions here, without activating loader rebinding. */
    (void)avxemu_loader_resolve();
    if (getenv("AVXEMU_DISABLE")) return;     /* opt-out escape hatch */
    if (cpu_has_everything()) return;         /* nothing to emulate: stay inert */
    if (!avxemu_cpu_baseline()) {
        emit("avxemu: SSE4.2, AVX and OS AVX register state are required\n"); _exit(127);
    }
    if (!avxemu_page_size() || !avxemu_loader_init()) {
        emit("avxemu: cannot initialize platform services\n"); _exit(127);
    }
    if (!avxemu_image_init()) {
        emit("avxemu: cannot initialize executable metadata\n"); _exit(127);
    }
    uint32_t r7[4] = {0}, r8[4] = {0};
    (void)avxemu_get_cpuid(7, 0, r7);
    (void)avxemu_get_cpuid(0x80000001u, 0, r8);
    unsigned required = avxemu_zcnt_required(r7[1], r8[2], getenv("AVXEMU_FORCEPATCH") != 0);
    /* Read-only cache validation precedes preparation; no code edits yet. */
    avxemu_lc_begin();
    avxemu_zcnt_plan *zcnt_plan=avxemu_zcnt_prepare(avxemu_image_main(), required);
    if (!zcnt_plan) {
        emit("avxemu: unsupported executable: cannot safely discover required LZCNT/TZCNT\n"); _exit(127);
    }
    if (policy_take(SIGILL) != 0) {
        emit("avxemu: failed to install SIGILL handler\n");
        _exit(127);
    }

    /* Fault-driven relocation is on by default; AVXEMU_RELOC=0 disables it so the
     * A/B harness can compare emulate-only vs relocate. */
    { const char *r = getenv("AVXEMU_RELOC"); if (r) g_reloc_enabled = (r[0] != '0'); }

    /* Advertise AVX2 + BMI1/BMI2 through a trapped cpuid so the target program
     * takes its AVX2 code path (whose faulting instructions we emulate) rather
     * than its broken no-AVX2 fallback. The SET mask only ORs bits in, so on a
     * CPU that genuinely has them this changes nothing. Env vars tune it for
     * diagnostics; AVXEMU_CPUID_CLR wins over the default (clr is applied last). */
    avxemu_cpuid_configure();
    /* The three passes below are analysis-heavy and give the same answer every
     * launch of the same binary: replay a recording of it when there is a good
     * one, else analyse and record (lcache.c). */
    avxemu_patch_cpuid();

    /* Rewrite lzcnt/tzcnt in the main binary to a faulting form so they get
     * emulated rather than silently running as bsr/bsf on a pre-Haswell CPU.
     * No-op on CPUs that actually have LZCNT. Runs after the handler is armed. */
    long n_lz = avxemu_zcnt_apply(zcnt_plan);
    if (n_lz < 0) { emit("avxemu: required LZCNT/TZCNT rewriting failed\n"); _exit(127); }

    /* Rewrite runs of faulting instructions to non-faulting trampolines so they
     * don't trap. The thunk runs its emulation on a dedicated side stack, so it
     * adds no program-stack pressure. Un-trampolined sites (4-byte isolated,
     * dirty functions) still fault into the SIGILL handler above. */
    long n_tr = avxemu_install_trampolines();
    avxemu_lc_end();

    /* Are we actually emulating on this CPU? If so, install the crash handler so
     * SIMD over-reads (off the end of a buffer into an unmapped page, which a
     * real AVX2 machine tolerates because that page is mapped) are fixed up
     * rather than crashing. FORCEPATCH/FORCETRAMP force this on an AVX2 host so
     * the test suite can exercise the whole path. */
    int emulating = (n_lz > 0 || n_tr > 0 ||
                     getenv("AVXEMU_FORCEPATCH") || getenv("AVXEMU_FORCETRAMP"));
    if (emulating) {
        g_fixup_overread = 1;
        /* Alternate stack so a stack-overflow SIGSEGV (guard page) can still be
         * handled — the faulting thread's own stack is exhausted at that point. */
        static char altstk[64*1024];
        stack_t ss2; ss2.ss_sp = altstk; ss2.ss_size = sizeof altstk; ss2.ss_flags = 0;
        sigaltstack(&ss2, 0);
        if (!policy_take(SIGSEGV) && policy_take(SIGBUS)) policy_release(SIGSEGV);
    }

    /* Preflight: validate the whole path on this exact CPU, then exit. */
    if (getenv("AVXEMU_SELFTEST")) avxemu_selftest();
}

/* Exposed for the test programs that drive the SIGILL handler themselves (fault injection, the
 * patched-lzcnt and BMI-memory oracles, the fuzzer). They need it installed on any CPU, including
 * one that has everything, where the constructor stays inert; where the constructor did install
 * it, once is enough. */
void avxemu_force_install(void) {
    if (policy_owned(SIGILL)) return;
    g_force_install = 1;
    avxemu_install();
}
