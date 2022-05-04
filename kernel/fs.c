// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb; 

// Read the super block.
static void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Init fs
void
fsinit(int dev) {
  readsb(dev, &sb);
  if(sb.magic != FSMAGIC)
    panic("invalid file system");
  initlog(dev, &sb);
}

// Zero a block.
// 将某个磁盘块对应的buffer内容置为0
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks.

// Allocate a zeroed disk block.
// 该函数中只调用了log_write
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  for(b = 0; b < sb.size; b += BPB){ // 外层循环只会循环一次，xv6的block数小于1024*8
    bp = bread(dev, BBLOCK(b, sb)); // xv6中计算出的bitmap块应该只有一个
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      m = 1 << (bi % 8); // 计算该块对应的是byte中的哪个bit
      if((bp->data[bi/8] & m) == 0){  // Is block free? bp->data[bi/8]找到相应的byte
        bp->data[bi/8] |= m;  // Mark block in use.
        log_write(bp); // bitmap的块发生了修改
        brelse(bp);
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  panic("balloc: out of blocks");
}

// Free a disk block.
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  bp = bread(dev, BBLOCK(b, sb)); // bitmap上的有效位检查
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m; // 分配位置0
  log_write(bp);
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at
// sb.startinode. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a cache entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The icache.lock spin-lock protects the allocation of icache
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold icache.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct spinlock lock;
  struct inode inode[NINODE]; // 磁盘上dinode的副本 50
} icache;
// inode cache为write through
// 修改了cached inode必须立刻写磁盘

void
iinit()
{
  int i = 0;
  
  initlock(&icache.lock, "icache");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }
}

static struct inode* iget(uint dev, uint inum);

// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode.
// 在dev上分配一个dinode
// 通过修改其type位不为0，表示已分配
// 返回dinode对应的in-memeory inode
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  for(inum = 1; inum < sb.ninodes; inum++){
    bp = bread(dev, IBLOCK(inum, sb)); // 该函数中有acquiresleep，确保同一时间只有一个进程使用bp
    dip = (struct dinode*)bp->data + inum%IPB; // 找到buffer cache中的inode
    if(dip->type == 0){  // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      return iget(dev, inum); // 分配了一个dinode，获取其inode（in-memory copy)
    }
    brelse(bp);
  }
  panic("ialloc: no inodes");
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk, since i-node cache is write-through.
// Caller must hold ip->lock.
// 将修改的in-memory inode拷贝到磁盘
// 该函数的调用这必须持有ip的锁
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp); // 修改in-memory数据结构和buffer cache
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
// 找到dev上编号为inum的inode
// 返回in-memory copy
// 不会lock inode也不会从磁盘上读出
// 只返回对应inode的引用，不会对其进行加锁
static struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&icache.lock);
      return ip; // 当前inode已经被cache，返回inode
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot. 记录最先找到的可用cache的inode
      empty = ip;
  }

  // Recycle an inode cache entry.
  if(empty == 0)
    panic("iget: no inodes"); // 没有可用的cache inode

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
// 增加ip对应node的引用次数
struct inode*
idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
// iget返回的inode可能不包含有效的内容
// 该函数从磁盘相应的inode中读取内容
// acquiresleep（inode）对inode加锁，如果inode内容无效则从磁盘中拷贝
// 只有唯一的进程会获得inode的使用权
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock); // 其余进程不可以ilock该inode

  if(ip->valid == 0){
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
// 释放inode的锁
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
// in-memory inode的引用--，如果没有对inode cache的引用，并没有对dinode的引用
// 如果disk inode引用为0，释放dinode
/* 需要考虑的一个问题：当dinode的link count变为0时，不能立即释放该dinode
 * 因为某些进程可能仍然持有in-memory inode的引用，仍然可以对文件进行读写
 * 如果此时crash发生，恢复后dinode被标记为已分配，但是没有任何dictionary entry指向它
 * 方法1：恢复之后扫描所有的已分配的dinode，如果没有目录入口想，则释放它
 * 方法2：文件系统在磁盘中记录link为0，但reference不为0的dinode的编号；
 *       当reference变为0时，从该list中将inode移除，恢复时，释放该list中的inode
 * xv6两种方法都没有实现，随着xv6的运行，可能会用光磁盘空间
*/ 
void
iput(struct inode *ip)
{
  acquire(&icache.lock);

  if(ip->ref == 1 && ip->valid && ip->nlink == 0){
    // inode has no links and no other references: truncate and free.

    // ip->ref == 1 means no other process can have ip locked,
    // so this acquiresleep() won't block (or deadlock).
    acquiresleep(&ip->lock);

    release(&icache.lock);

    itrunc(ip); // 释放当前inode size置为0，释放掉链接的blocks，同步dinode
    ip->type = 0; // 回收inode
    iupdate(ip); // 同步dinode
    ip->valid = 0;

    releasesleep(&ip->lock);

    acquire(&icache.lock);
  }

  ip->ref--; // 如果当前inode不需要释放，只将引用--即可
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
// 返回inode ip的第n块磁盘块的地址(blockno)，如果没有，则分配一个新的磁盘块
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a, *a_2;
  struct buf *bp, *bp_2;

  if(bn < NDIRECT){ // 直接链接的block内
    if((addr = ip->addrs[bn]) == 0)
      ip->addrs[bn] = addr = balloc(ip->dev); // 分配一个磁盘块
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev); // 分配间接链接的block目录的磁盘块
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){ // 获取block目录的第n个
      a[bn] = addr = balloc(ip->dev); // balloc中已经将bitmap的变化进行了log_write
      log_write(bp); // 这里log_write的是block目录的变化
    }
    brelse(bp);
    return addr;
  }
  bn -= NINDIRECT;

  if(bn < NINDIRECT * NINDIRECT){
    // Load 2-level indirect block, allocating if necessary.
    // 判断一级目录是否存在
    if((addr = ip->addrs[NDIRECT + 1]) == 0)
      ip->addrs[NDIRECT + 1] = addr = balloc(ip->dev); // 分配间接链接的block目录的磁盘块
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;

    uint level_1 = bn / NINDIRECT;
    uint level_2 = bn % NINDIRECT;
    if((addr = a[level_1]) == 0){ // 获取block目录的第n个
      a[level_1] = addr = balloc(ip->dev); // balloc中已经将bitmap的变化进行了log_write
      log_write(bp); // 这里log_write的是block目录的变化
    }
    brelse(bp);
    bp_2 = bread(ip->dev, addr);
    a_2 = (uint*)bp_2->data;
    if ((addr = a_2[level_2]) == 0) {
      a_2[level_2] = addr = balloc(ip->dev);
      log_write(bp_2);
    }
    brelse(bp_2);
    
    return addr;
  }

  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// Caller must hold ip->lock.
void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp, *bp_2;
  uint *a, *a_2;

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){ // 释放直接链接的blocks
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  if(ip->addrs[NDIRECT]){ // 释放间接链接的blocks
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
        bfree(ip->dev, a[j]);
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]); // 第二层链接block
    ip->addrs[NDIRECT] = 0;
  }

  if(ip->addrs[NDIRECT + 1]){ // 释放间接链接的blocks
    bp = bread(ip->dev, ip->addrs[NDIRECT + 1]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j]) {
        bp_2 = bread(ip->dev, a[j]);
        a_2 = (uint*)bp_2->data;
        for (int k = 0; k < NINDIRECT; k++) {
          if (a_2[k])
            bfree(ip->dev, a_2[k]);
        }
        brelse(bp_2);
        bfree(ip->dev, a[j]); 
        //a[j] = 0;
      }
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT + 1]); 
    ip->addrs[NDIRECT + 1] = 0;
  }

  ip->size = 0; 
  iupdate(ip); // 同步dinode
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

// Read data from inode.
// Caller must hold ip->lock.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
// 从inode中读取数据，调用者必须获取ip的锁
int
readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off) // 检查读取的起始位置是否在文件的大小范围内，检查读取字节数是否大于0
    return 0;
  if(off + n > ip->size) // 读取结束位置是否超出了文件大小
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE)); // 获取off对应的磁盘号，没有则分配
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1) {
      brelse(bp);
      tot = -1;
      break;
    }
    brelse(bp);
  }
  return tot;
}

// Write data to inode.
// Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address;
// otherwise, src is a kernel address.
// Returns the number of bytes successfully written.
// If the return value is less than the requested n,
// there was an error of some kind.
// 向inode中写入数据，调用者必须持有inode的锁
// 返回成功写入的字节数
int
writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE) // 超出了最大文件的限制
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    // bmap获得对应的磁盘号
    // bread获得磁盘号对应的buffer
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1) {
      brelse(bp);
      break;
    }
    log_write(bp);
    brelse(bp);
  }

  if(off > ip->size) // 写完之后可能更改了文件的大小
    ip->size = off;

  // write the i-node back to disk even if the size didn't change
  // because the loop above might have called bmap() and added a new
  // block to ip->addrs[].
  iupdate(ip); // write through

  return tot;
}

// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
// 在目录中寻找目录入口项
// 找到入口项的话通过poff返回字节偏移
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    // 0表示目的地址为内核地址 读取off开始，长度为sizeof（de）的内容到de
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;// 该entry在此目录的数据部分的偏移
      inum = de.inum; 
      return iget(dp->dev, inum); // 返回磁盘块的in-memory copy
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
// 在目录dp中写入给定name和inum的目录
// 如果entry已经存在，返回-1
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){ // entry中inum对应的inode的inmemeory cache
    iput(ip); // dirlookup的iget中对该inode的引用次数++了
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");

  return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//

/*
 * 将开头的路径元素拷贝到name中，将后续的路径元素返回
 * 空路径返回0
 */
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  // 路径起始目录拷贝到name中
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
// 返回当前目录的inode或上一级目录的inode
static struct inode*
namex(char *path, int nameiparent, char *name) // path是调用者传入的参数，nameiparent为标志位，name为空
{
  struct inode *ip, *next;

  if(*path == '/') // 根路径
    ip = iget(ROOTDEV, ROOTINO); // 直接读取根路径的inode cache，将根目录的引用次数++
  else
    ip = idup(myproc()->cwd); // 当前路径的引用次数++

  while((path = skipelem(path, name)) != 0){ // 拆分一级目录
    ilock(ip); // 获取目录对应inode cache的读写权限
    // 原因：ilock之前不保证ip被从磁盘中读取
    if(ip->type != T_DIR){
      iunlockput(ip); // 释放inode cache的锁并且引用次数--
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip); // 释放inode的锁，这里没有对引用次数--
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0){ // 在当前目录中寻找下一级目录
      iunlockput(ip); // 失败则释放inode cache的锁，并且当前目录引用次数--
      return 0;
    }
    iunlockput(ip); // 释放inode cache的锁，并且当前目录引用次数--
    // while循环退出时不会存在某个目录的inode是加锁的
    ip = next; // 进行下一级目录的查找
  }
  if(nameiparent){ // 原本为空目录，没有进到while循环中，也不存在上一级目录，因此对开始的引用次数--
                    // nameiparent要返回的是目标目录的上一级目录，因此应该对上一级目录的引用++，这里应该恢复当前目录的引用值，并返回错误
                    // namei要返回的是目标目录，因此在该函数中对当前目录进行了引用++
    iput(ip);
    return 0;
  }
  return ip;
}



struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}
