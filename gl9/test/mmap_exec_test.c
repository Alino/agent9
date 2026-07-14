#include <sys/mman.h>
#include <stdio.h>
typedef int (*fn)(void);
int main(void){
  unsigned char code[]={0xb8,0x2a,0,0,0,0xc3}; /* MOVL $42,AX; RET */
  void *p=mmap(0,0x1000,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON,-1,0);
  if(p==(void*)-1){ printf("mmap FAILED\n"); return 1; }
  for(int i=0;i<6;i++) ((unsigned char*)p)[i]=code[i];
  printf("mmap=%p exec-result=%d (expect 42)\n", p, ((fn)p)());
  return 0;
}
