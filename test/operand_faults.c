#include "decode.h"
#include "regfile.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static void operand(int mode) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *p = mmap(0, page * 2, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(p != MAP_FAILED);
    memset(p, 0x31, page * 2);
    if (mode == 1) assert(!munmap(p + page, page));
    if (mode == 2) assert(!mprotect(p + page, page, PROT_NONE));
    if (mode == 3) assert(!mprotect(p, page * 2, PROT_NONE));
    unsigned char insn[] = {0xc5, 0xf5, 0xfe, 0x00};
    decoded d; avxemu_regfile rf; memset(&rf, 0, sizeof rf);
    assert(decode(insn, &d) == 4);
    rf.gpr[0] = (uintptr_t)(p + page - 16);
    assert(avxemu_emulate(&d, &rf));
    for (int i = 0; i < 32; i++)
        assert(rf.ymm[0][i] == (mode == 1 && i >= 16 ? 0 : 0x31));
    _exit(0);
}
int main(void) {
    for (int step = 0; step < 4; step++) {
        int mode = (step + 2) % 4;
        pid_t child = fork(); assert(child >= 0);
        if (!child) operand(mode);
        int status; assert(waitpid(child, &status, 0) == child);
        if (mode < 2) assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        else assert(WIFSIGNALED(status) &&
                    (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS));
    }
    puts("operand-faults: readable, gap, guard and bad operand OK");
    return 0;
}
