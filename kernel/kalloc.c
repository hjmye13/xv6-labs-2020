// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
  struct spinlock stealing_lock;
} kmem[NCPU];

char* kmem_lock_name [NCPU] = {
  "kmem_lock_cpu0",
  "kmem_lock_cpu1",
  "kmem_lock_cpu2",
  "kmem_lock_cpu3",
  "kmem_lock_cpu4",
  "kmem_lock_cpu5",
  "kmem_lock_cpu6",
  "kmem_lock_cpu7",
};

char* kmem_stealing_lock_name [NCPU] = {
  "kmem_stealing_lock_cpu0",
  "kmem_stealing_lock_cpu1",
  "kmem_stealing_lock_cpu2",
  "kmem_stealing_lock_cpu3",
  "kmem_stealing_lock_cpu4",
  "kmem_stealing_lock_cpu5",
  "kmem_stealing_lock_cpu6",
  "kmem_stealing_lock_cpu7",
};

void
kinit()
{
  for (int i = 0; i < NCPU; i++) {
    initlock(&kmem[i].lock, kmem_lock_name[i]);
    initlock(&kmem[i].stealing_lock, kmem_stealing_lock_name[i]);
  }
  freerange(end, (void*)PHYSTOP); // freerange会将内存分配给每个CPU内核的freelist
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off(); // 关闭中断确保cpuid能够返回正确的cpu编号
  int cpu = cpuid();

  acquire(&kmem[cpu].lock);
  r->next = kmem[cpu].freelist;
  kmem[cpu].freelist = r;
  release(&kmem[cpu].lock);

  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int cpu = cpuid();

  acquire(&kmem[cpu].lock);
  if (!kmem[cpu].freelist) {
    acquire(&kmem[cpu].stealing_lock); // 防止出现cpu互相偷内存而死锁的情况          
    release(&kmem[cpu].lock);
    int steal_left = 64;
    for (int i = 0; i < NCPU; i++) {
      if (i == cpu) {
        continue;
      }
      acquire(&kmem[i].lock);
      struct run *rr = kmem[i].freelist;
      while (r && steal_left) {
        kmem[i].freelist = rr->next;
        rr->next = kmem[cpu].freelist;
        kmem[cpu].freelist = rr;
        rr = kmem[i].freelist;
        steal_left--;
      }
      release(&kmem[i].lock);
      if (!steal_left) {
        break;
      }
    }
    acquire(&kmem[cpu].lock);
    release(&kmem[cpu].stealing_lock);
  }

  r = kmem[cpu].freelist;
  if(r)
    kmem[cpu].freelist = r->next;
  release(&kmem[cpu].lock);

  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
