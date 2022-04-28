struct buf {
  int valid;   // has data been read from disk? 是否保存了从磁盘中读取的内容
  int disk;    // does disk "own" buf? buffer中的内容是否已经提交到磁盘
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev; // LRU cache list
  struct buf *next;
  uchar data[BSIZE];
};

