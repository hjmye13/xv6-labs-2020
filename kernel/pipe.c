#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

#define PIPESIZE 512

struct pipe {
  struct spinlock lock;
  char data[PIPESIZE];
  uint nread;     // number of bytes read
  uint nwrite;    // number of bytes written
  int readopen;   // read fd is still open
  int writeopen;  // write fd is still open
};

int
pipealloc(struct file **f0, struct file **f1)
{
  struct pipe *pi;

  pi = 0;
  *f0 = *f1 = 0;
  if((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
    goto bad;
  if((pi = (struct pipe*)kalloc()) == 0)
    goto bad;
  pi->readopen = 1;
  pi->writeopen = 1;
  pi->nwrite = 0;
  pi->nread = 0;
  initlock(&pi->lock, "pipe");
  (*f0)->type = FD_PIPE;
  (*f0)->readable = 1;
  (*f0)->writable = 0;
  (*f0)->pipe = pi;
  (*f1)->type = FD_PIPE;
  (*f1)->readable = 0;
  (*f1)->writable = 1;
  (*f1)->pipe = pi;
  return 0;

 bad:
  if(pi)
    kfree((char*)pi);
  if(*f0)
    fileclose(*f0);
  if(*f1)
    fileclose(*f1);
  return -1;
}

void
pipeclose(struct pipe *pi, int writable)
{
  acquire(&pi->lock);
  if(writable){
    pi->writeopen = 0;
    wakeup(&pi->nread);
  } else {
    pi->readopen = 0;
    wakeup(&pi->nwrite);
  }
  if(pi->readopen == 0 && pi->writeopen == 0){
    release(&pi->lock);
#ifdef LAB_LOCK
    freelock(&pi->lock);
#endif    
    kfree((char*)pi);
  } else
    release(&pi->lock);
}

int
pipewrite(struct pipe *pi, uint64 addr, int n)
{
  int i;
  char ch;
  struct proc *pr = myproc();

  acquire(&pi->lock);
  for(i = 0; i < n; i++){
    while(pi->nwrite == pi->nread + PIPESIZE){  //DOC: pipewrite-full
      // nwrite和nread都是大于缓冲区大小的数值
      // 检查两个数值之前的差值是否等于缓冲区大小
      // 如果等于，则说明缓冲区已满
      if(pi->readopen == 0 || pr->killed){ // 缓冲区满并且没有读进程，或者当前进程已经被killed
        release(&pi->lock);
        return -1;
      }
      wakeup(&pi->nread); // 缓冲区满，但是存在写进程
      sleep(&pi->nwrite, &pi->lock);
    }
    if(copyin(pr->pagetable, &ch, addr + i, 1) == -1) // 读取一个字符
      break;
    pi->data[pi->nwrite++ % PIPESIZE] = ch; // 将字符放入缓冲区
  }
  wakeup(&pi->nread); // 写入完毕，唤醒读进程
  release(&pi->lock);
  return i;
}

int
piperead(struct pipe *pi, uint64 addr, int n)
{
  int i;
  struct proc *pr = myproc();
  char ch;

  acquire(&pi->lock);
  while(pi->nread == pi->nwrite && pi->writeopen){  //DOC: pipe-empty
    // 缓冲区已空并且存在写进程
    if(pr->killed){ // 检查进程状态
      release(&pi->lock);
      return -1;
    }
    sleep(&pi->nread, &pi->lock); //DOC: piperead-sleep
  }
  for(i = 0; i < n; i++){  //DOC: piperead-copy
    if(pi->nread == pi->nwrite) // 没有可读的数据
      break; // 每个读进程获得pipe的读取权限之后只能进行一次读取，可能不能读取到n个字符
    ch = pi->data[pi->nread++ % PIPESIZE]; // 读取字符
    if(copyout(pr->pagetable, addr + i, &ch, 1) == -1) // 拷贝到用户空间
      break;
  }
  wakeup(&pi->nwrite);  //DOC: piperead-wakeup
  release(&pi->lock);
  return i;
}
