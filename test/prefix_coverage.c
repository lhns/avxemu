#include "regfile.h"
#include <assert.h>
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static sigjmp_buf jump;
static void caught(int sig) { siglongjmp(jump,sig); }
int main(void) {
 /* Volatile keeps generated instruction data out of executable immediates. */
 static const volatile unsigned char cases[][16]={
  {0xf3,0xf3,0x0f,0xbc,0xc0},
  {0xf3,0xf3,0x48,0x0f,0xbc,0xc0},
  {0xf3,0xf3,0x66,0x0f,0xbc,0xc0},
  {0xf3,0xf3,0x66,0x66,0x66,0x66,0x66,0x0f,0xbc,0x84,0x25,0,0,0,0}
 };
 int sizes[]={5,6,6,15},widths[]={32,64,16,16};
 unsigned char *code=mmap(0,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON,-1,0);
 assert(code!=MAP_FAILED);
 struct sigaction a; memset(&a,0,sizeof a); a.sa_handler=caught; sigemptyset(&a.sa_mask); assert(!sigaction(SIGILL,&a,0));
 for(int operation=0;operation<2;operation++) for(int i=0;i<4;i++) {
  uint64_t value=8; uintptr_t address=(uintptr_t)&value;
  unsigned char patched[32]={0}; for(int j=0;j<sizes[i];j++) patched[j]=cases[i][j]; patched[1]=0xf0;
  if(operation) for(int j=0;j<sizes[i]-1;j++) if(patched[j]==0x0f) patched[j+1]=0xbd;
  unsigned expected=operation?(unsigned)widths[i]-4:3;
  decoded d; assert(decode(patched,&d)==sizes[i] && d.op==(operation?BMI_LZCNT:BMI_TZCNT) && d.opsize==widths[i]);
  avxemu_regfile rf; memset(&rf,0,sizeof rf); rf.gpr[0]=8; rf.gpr[5]=address;
  assert(avxemu_emulate(&d,&rf) && rf.gpr[0]==expected);
  size_t n=0; code[n++]=0x55; code[n++]=0x48;code[n++]=0xbd;memcpy(code+n,&address,8);n+=8;
  code[n++]=0xb8;code[n++]=8;memset(code+n,0,3);n+=3;
  memcpy(code+n,patched,sizes[i]);n+=sizes[i];code[n++]=0x5d;code[n++]=0xc3;
  int sig=sigsetjmp(jump,1); if(!sig) { ((void(*)(void))code)(); assert(0); } assert(sig==SIGILL);
 }
 puts("prefix-coverage:8 decoder/emulator/native-SIGILL variants PASS");
 return 0;
}
