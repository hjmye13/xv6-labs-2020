// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUFMAP_BUCKET 13
#define BUFMAP_HASH(dev, blockno) ((((dev)<<27)|(blockno))%NBUFMAP_BUCKET)

struct {
  //struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  //struct buf head;
  struct buf bufmap[NBUFMAP_BUCKET]; // 该数组中的buf为dummy head
  struct spinlock bufmap_locks[NBUFMAP_BUCKET];
  struct spinlock bufeviction_locks[NBUFMAP_BUCKET];
} bcache;

// buffer cache是双向链接的列表
void
binit(void)
{
  struct buf *b;

  for(int i = 0; i < NBUFMAP_BUCKET; i++) {
    initlock(&bcache.bufmap_locks[i], "bcache_bufmap");
    initlock(&bcache.bufeviction_locks[i], "bcache_bufeviction");
    bcache.bufmap[i].next = 0;
  }

  for (int i = 0;i < NBUF; i++) {
    struct buf *temp = &bcache.buf[i];
    acquire(&bcache.bufmap[0]);
    temp->lastuse = 0;
    temp->refcnt = 0;
    temp->next = bcache.bufmap[0].next;
    bcache.bufmap[0].next = temp;
    release(&bcache.bufmap[0]);
  }
  //initlock(&bcache.lock, "bcache");

  // Create linked list of buffers
  //bcache.head.prev = &bcache.head;
  //bcache.head.next = &bcache.head;
  /*
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  */
  // 将数组buf中的块链接起来，每次都接到head的后面
  // 其余所有对buf的访问都是通过head链表进行的，而不是直接访问buf数组
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
// 查找buffer cache中是否有目标buffer
// 如果没有找到就分配一个buffer
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  uint key = BUFMAP_HASH(dev, blockno);

  acquire(&bcache.bufmap_locks[key]);

  for (b = bcache.bufmap[key].next; b; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.bufmap[key]);
      acquiresleep(&b->lock); // 确保同一时间只有一个进程使用该缓存块
    }
  }

  release(&bcache.bufmap_locks[key]);
  acquire(&bcache.bufeviction_locks);

  for (b = bcache.bufmap[key].next; b; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      acquire(&bcache.bufmap_locks);
      b->refcnt++;
      release(&bcache.bufmap_locks[key]);
      release(&bcache.bufeviction_locks[key]);
      acquiresleep(&b->lock); // 确保同一时间只有一个进程使用该缓存块
    }
  }

  struct buf *before_last = 0;
  uint holding_bucket = -1;

  for (int i = 0; i < NBUFMAP_BUCKET; i++) {
    acquire(&bcache.bufmap_locks[i]);

    int new_buf = 0;
    for (b = &bcache.bufmap[i]; b->next; b = b->next) {
      if (b->next->refcnt == 0 && ((!before_last) || b->lastuse < before_last->next->lastuse)) {
        before_last = b;
        new_buf = 1;
      }
    }
    if (!new_buf) {
      release(&bcache.bufmap_locks[i]);
    } else {
      if (holding_bucket != -1) {
        release(&bcache.bufmap_locks[holding_bucket]);
      }
      holding_bucket = i;
    }
  }

  if (!before_last) {
    panic("bget: no buffers");
  }

  b = before_last->next;
  if (holding_bucket != key) {
    before_last->next = b->next; // 从原来的bucket中移除
    release(&bcache.bufmap_locks[holding_bucket]);
    acquire(&bcache.bufmap_locks[key]);
    b->next = bcache.bufmap[key].next;
    bcache.bufmap[key].next = b;
  }

  b->dev = dev;
  b->blockno = blockno;
  b->refcnt = 1;
  b->valid = 0;
  release(&bcache.bufmap_locks[key]);
  release(&bcache.bufeviction_locks[key]);
  acquiresleep(&b->lock);
  return b;
  
  //acquire(&bcache.lock);

  // Is the block already cached?
  /*
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++; // 递增引用次数
      release(&bcache.lock);
      acquiresleep(&b->lock); // 确保每个每个buffer cache只有一个进程使用
      // 这种情况下，当进程获得buffer的使用权之后，buffer的valid位为1
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;// 表示是新分配的块
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
  */
  // 没有可分配的buffer
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0); // 从磁盘中读取块，b中保存了dev和blockno的值
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock)) // 检查该buffer是否为blocked并且是当前进程所持有的
    panic("bwrite");
  virtio_disk_rw(b, 1); // 将其写回磁盘
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock); // 将blocked和pid置零，并且唤醒等待该锁的进程

  uint key = BUFMAP_HASH(b->dev, b->blockno);

  acquire(&bcache.bufmap_locks[key]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // 没有进程使用当前buf，将其移到buf list的头部
    // no one is waiting for it.
    b->lastuse = ticks;
  }
  
  release(&bcache.bufmap_locks[key]);
}

void
bpin(struct buf *b) {
  uint key = BUFMAP_HASH(b->dev, b->blockno);
  acquire(&bcache.bufmap_locks[key]);
  b->refcnt++;
  release(&bcache.bufmap_locks[key]);
}

void
bunpin(struct buf *b) {
  uint key = BUFMAP_HASH(b->dev, b->blockno);
  acquire(&bcache.bufmap_locks[key]);
  b->refcnt--;
  release(&bcache.bufmap_locks[key]);
}


