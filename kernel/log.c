#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// Simple logging that allows concurrent FS system calls.
//
// A log transaction contains the updates of multiple FS system
// calls. The logging system only commits when there are
// no FS system calls active. Thus there is never
// any reasoning required about whether a commit might
// write an uncommitted system call's updates to disk.
//
// A system call should call begin_op()/end_op() to mark
// its start and end. Usually begin_op() just increments
// the count of in-progress FS system calls and returns.
// But if it thinks the log is close to running out, it
// sleeps until the last outstanding end_op() commits.
//
// The log is a physical re-do log containing disk blocks.
// The on-disk log format:
//   header block, containing block #s for block A, B, C, ...
//   block A
//   block B
//   block C
//   ...
// Log appends are synchronous.

// Contents of the header block, used for both the on-disk header block
// and to keep track in memory of logged block# before commit.
struct logheader {
  int n; // 记录有多少log是committed
  int block[LOGSIZE]; // 30 记录了磁盘上的哪些块被log
};

struct log {
  struct spinlock lock;
  int start;  // 磁盘上[block2, block32) 由start和size决定
  int size;
  int outstanding; // how many FS sys calls are executing. 有多少文件系统的系统调用在执行
  int committing;  // in commit(), please wait. log是否在committing
  int dev;
  struct logheader lh; 
};
struct log log;

static void recover_from_log(void);
static void commit();

/* 系统调用中log的基本格式
 * begin_op(); // 确保log没有在committing，并且log中足够的空间，outstanding计数+1，当前进程继续运行
 * ...
 * bp = bread(...);
 * bp->data[...] = ...;
 * log_write(bp);
 * ...
 * end_op();
*/

void
initlog(int dev, struct superblock *sb)
{
  if (sizeof(struct logheader) >= BSIZE) // 检查logheader的大小是否大于一个block
    panic("initlog: too big logheader");

  // 根据superblock中的信息进行初始化
  initlock(&log.lock, "log");
  log.start = sb->logstart;
  log.size = sb->nlog;
  log.dev = dev;
  recover_from_log();
}

// Copy committed blocks from log to their home location
static void
install_trans(int recovering)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    // 为什么又拷贝了一次
    struct buf *lbuf = bread(log.dev, log.start+tail+1); // read log block
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]); // read dst
    memmove(dbuf->data, lbuf->data, BSIZE);  // copy block to dst
    bwrite(dbuf);  // write dst to disk // 将buffer写入磁盘
    if(recovering == 0) // commit中recovering为0，recovery_from_log中为1
      bunpin(dbuf); // recovery的过程，buffer的引用计数已经被减过了
    brelse(lbuf);
    brelse(dbuf);
  }
}

// Read the log header from disk into the in-memory log header
// 从磁盘中将log header读入内存中的log header
static void
read_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  // 如果当前磁盘块的buffer存在的话，直接引用该buffer
  // 如果该磁盘块的buffer不存在，则分配一块新的buffer，并将磁盘中的内容拷贝到buffer中
  struct logheader *lh = (struct logheader *) (buf->data);
  int i;
  log.lh.n = lh->n;
  for (i = 0; i < log.lh.n; i++) {
    log.lh.block[i] = lh->block[i];
  }
  brelse(buf);
}

// Write in-memory log header to disk.
// This is the true point at which the
// current transaction commits.
static void
write_head(void)
{
  struct buf *buf = bread(log.dev, log.start); // 读取log起始磁盘块对应的buffer
  // buf 是log header所在磁盘块的buffer cache
  // 这里的log是内存中的数据结构，不是磁盘中的log和log header
  struct logheader *hb = (struct logheader *) (buf->data);
  int i;
  hb->n = log.lh.n;
  for (i = 0; i < log.lh.n; i++) {
    hb->block[i] = log.lh.block[i];
  }
  bwrite(buf);
  brelse(buf);
}

static void
recover_from_log(void)
{
  read_head(); // 读取磁盘中的head
  install_trans(1); // if committed, copy from log to disk
  // 将提交的log更新到磁盘
  log.lh.n = 0;
  write_head(); // clear the log
}

// called at the start of each FS system call.
// 文件系统的系统调用会以该函数开始
// 检查log是否在committing
// 检查log是否有足够的剩余空间
// 增加正在执行系统调用的进程数
void
begin_op(void)
{
  acquire(&log.lock);
  while(1){
    if(log.committing){
      sleep(&log, &log.lock); // 等到logging系统不进行提交时
      // 等待log，sleep时释放log的锁
    } else if(log.lh.n + (log.outstanding+1)*MAXOPBLOCKS > LOGSIZE){
      // 正在运行的系统调用每个按照MAXOPBLOCKS进行计算
      // this op might exhaust log space; wait for commit.
      sleep(&log, &log.lock); // 等待有足够的剩余空间可以使用
    } else {
      log.outstanding += 1; // 该进程开始执行
      release(&log.lock); // 释放掉log的锁
      break;
    }
  }
}

// called at the end of each FS system call.
// commits if this was the last outstanding operation.
// 所有文件系统系统调用的结束都会调用该函数
// 运行的进程数-1
// 如果没有运行文件系统系统调用的进程，则进行commit；否则唤醒等待log的进程
// 
void
end_op(void)
{
  int do_commit = 0;

  acquire(&log.lock);
  log.outstanding -= 1; // 运行的进程数减一
  if(log.committing)
    panic("log.committing");
  if(log.outstanding == 0){ // 没有运行的进程时，对当前transaction进行提交
    do_commit = 1;          // 设置log的状态
    log.committing = 1;
  } else { // 如果仍不能提交，则唤醒等待log的进程
    // begin_op() may be waiting for log space,
    // and decrementing log.outstanding has decreased
    // the amount of reserved space.
    wakeup(&log); 
  }
  release(&log.lock);

  // 不是每个文件系统系统调用的结束都会调用commit
  // 只有当该系统调用结束，并且没有其他进行使用文件系统的系统调用时，才会进行commit
  if(do_commit){
    // call commit w/o holding locks, since not allowed
    // to sleep with locks.
    commit();
    // 获取log的锁，对transaction提交后唤醒等待log的进程
    acquire(&log.lock);
    log.committing = 0;
    wakeup(&log);
    release(&log.lock);
  }
}

// Copy modified blocks from cache to log.
static void
write_log(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    // bread获取到的是磁盘块对应的buffer
    // 因此log也进行buffer cache
    struct buf *to = bread(log.dev, log.start+tail+1); // 不写log start（log header所在位置）对应的buffer cache
    struct buf *from = bread(log.dev, log.lh.block[tail]); // cache block 磁盘上哪些块的内容被记录在log中
    memmove(to->data, from->data, BSIZE); // 将磁盘块对应的buffer cache的内容拷贝到log的buffer cache中
    bwrite(to);  // write the log 将buffer cache的内容写入磁盘
    brelse(from); // 引用计数-- 如果当前buffer引用变为0则防止循环列表起始处
    brelse(to); 
  }
}

static void
commit()
{
  if (log.lh.n > 0) { //log中有block
    write_log();     // Write modified blocks from cache to log 将磁盘块对应的buffer cache写入log，然后从log中写入磁盘
    write_head();    // Write header to disk -- the real commit
    // 将内存中数据结构log header的内容写入磁盘的log header
    install_trans(0); // Now install writes to home locations
    // 将log中的内容提交到磁盘
    log.lh.n = 0;
    write_head();    // Erase the transaction from the log
  }
}

// Caller has modified b->data and is done with the buffer.
// Record the block number and pin in the cache by increasing refcnt.
// commit()/write_log() will do the disk write.
//
// log_write() replaces bwrite(); a typical use is:
//   bp = bread(...)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
void
log_write(struct buf *b)
{
  int i;

  if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1) // 检查当前transaction的大小
    panic("too big a transaction");
  if (log.outstanding < 1)
    panic("log_write outside of trans");

  acquire(&log.lock);
  for (i = 0; i < log.lh.n; i++) {
    if (log.lh.block[i] == b->blockno)   // log absorbtion 查看该磁盘块是否在log中已经存在
      break;
  }
  log.lh.block[i] = b->blockno; // 如果for没有找到blockno对应的log，则会分配一个新的，需要更新磁盘号
  if (i == log.lh.n) {  // Add new block to log?
    bpin(b); // 对buf的引用++
    log.lh.n++; // log中出现了一个新的块
  }
  release(&log.lock);
}

