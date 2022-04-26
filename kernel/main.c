#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

volatile static int started = 0;

// start() jumps here in supervisor mode on all CPUs.
void
main()
{
  if(cpuid() == 0){
    /***/
    // 设置UART，使其可以产生中断
    // 但是还未对PLIC编程，所以中断不能被CPU感知
    consoleinit(); 
    
    printfinit();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("\n");
    kinit();         // physical page allocator
    kvminit();       // create kernel page table
    kvminithart();   // turn on paging
    procinit();      // process table
    trapinit();      // trap vectors
    trapinithart();  // install kernel trap vector
    /***/
    // 设置PLIC，使其可以传递中断到单个CPU
    // CPU还未设置好接收中断，还未设置好sstatus寄存器
    plicinit();      // set up interrupt controller
    plicinithart();  // ask PLIC for device interrupts

    binit();         // buffer cache
    iinit();         // inode cache
    fileinit();      // file table
    virtio_disk_init(); // emulated hard disk
    userinit();      // first user process
    __sync_synchronize();
    started = 1;
  } else {
    while(started == 0)
      ;
    __sync_synchronize();
    printf("hart %d starting\n", cpuid());
    kvminithart();    // turn on paging
    trapinithart();   // install kernel trap vector
    plicinithart();   // ask PLIC for device interrupts
  }

  /***/
  // 设置CPU的status寄存器，使其能够接收中断
  // 至此，中断被完全打开
  scheduler();        
}
