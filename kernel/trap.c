#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[]; // 汇编代码 trampoline.s

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0) // 检查是否是从用户模式进入trap
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  // 将kernalvec.S中的kernalvec送入stvec中，这是内核空间trap处理代码的位置
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  // 保存用户的程序计数器
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on(); // 允许device interrupts
    // 在处理系统调用的时候使能中断，这样中断可以更快的服务，有些系统调用需要许多时间处理
    // 中断总是会被RISC-V的trap硬件关闭，因此需要显式打开中断
    syscall();
  } else if((which_dev = devintr()) != 0){
    // ok
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2) {
    if (p->interval != 0) {
      p->ticks--;
      if (p->ticks <= 0 && p->alarm_goingoff == 0) {
        p->ticks = p->interval;
        *p->alarm_trapframe = *p->trapframe;
        p->alarm_goingoff = 1;
        p->trapframe->epc = (uint64)p->handler;
      }
    }
    yield();
  }
  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  // 关闭中断，原因：将要更新STVEC寄存器来指向用户空间的trap处理代码
  // 现在仍在内核中执行代码，如果这时发生一个中断，那么程序执行会走向用户空间的trap处理代码，这将导致内核出错
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  // 设置STVEC寄存器指向trampoline代码，在那里最终执行的sret指令会开中断
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  // 填入trapframe内容
  p->trapframe->kernel_satp = r_satp();         // 存储kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // 存储与当前用户进程的kernal stack process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap; // 存储usertrap函数指针，这样trampoine代码才能跳转到这个函数
  p->trapframe->kernel_hartid = r_tp();         // 读取CPU核编号hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  // 设置控制寄存器SSTATUS，SPP位控制sret指令的行为，0表示下次执行sret要返回user mode
  // SPIE位控制了在执行完sret之后是否打开中断，1表示返回用户空间后希望开中断
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x); // 将更新后的值写入SSTATUS寄存器

  // set S Exception Program Counter to the saved user pc.
  // 将SEPC寄存器的值设置成之前保存的用户程序寄存器的值
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  // 根据user page table地址生成相应的SATP值，以便在返回到用户空间的时候完成page table的切换
  // 该参数存放在a1寄存器中
  // 在汇编代码trampoline中完成page table的切换
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  // 跳转到trampoline的userret处
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  // 将fn指针作为一个函数指针，执行相应的函数，并传入两个参数， 存储在a0和a1寄存器中
  // a0寄存器中存储的是trapframe，a1寄存器中存储的是user page table 即 SATP的值   
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  // yield 可能会破坏之前的spec和sstatus，因此在这里进行保存
  // 在该函数运行结束之前进行恢复
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0) //SPP=1 supervisor mode SPP=0 user mode
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0) // 检查supervior模式下中断是否使能
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){ //device interrupt
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  // 时钟中断
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  // 恢复trap寄存器
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

