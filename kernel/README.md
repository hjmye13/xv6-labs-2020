## 记录一下lab8 Buffer cache的几个要点

### 思路

- xv6对buffer cache原始的实现
  - 将所有的buffer块链接为双向链表，使用一个大锁bcache.lock进行保护
  - 问题：多进程同时使用文件系统的时候，在bcache.lock上会产生严重的竞争，多进程不能同时操作磁盘缓存
  - 解决办法
    - 细化锁的粒度，使用hashtable索引（dev，blockno）至bucket，对每个bucket单独进行加锁保护
    - 初始时，所有buf块都链接在bucket 0中
    - 每次miss cache发生，都遍历所有bucket寻找没有被引用且最近最久未使用buf，链接到当前bucket下

### 注意问题

- 出现gap情况

  - 遍历hashtable，寻找未被引用且最近最少使用的块，该过程中需要获取某个bucket的锁，查看buf属性，然后释放该bucket的锁

  - 获取目标buf对应bucket的锁，对bucket中的buf进行eviction，释放该bucket的锁

    **中间存在一个gap，遍历完成之后获得的最近最久未使用的块可能不是正确的，因此在释放了相应bucket的锁之后，其他进程可能对该块进行引用，因此需要保持该bucket的锁直到eviction结束**

  - 解决办法：保持当前最近最久未使用块对应bucket的锁，每次最近最久未使用块出现更新之后释放之前保持的锁

- 死锁情况1

  - 检查当前块不在相应的bucket a中

  - 持有bucket a的锁，不释放

  - 遍历hashtable，寻找未被引用且最近最少使用的块，该过程中需要获取某个bucket的锁，查看buf属性，然后释放该bucket的锁

    **a持有自己的锁，获取b的锁，同时，b持有自己的锁，获取a的锁**

  - 解决办法：第二步释放bucket a的锁，但是要避免出现多个进程同时为一个块进行驱逐分配，因此增加一个给每个bucket增加一个eviction_lock，获取该锁之后，重新检查bucket a中是否存在目标block（只有第一个获取该锁的进程对块进行驱逐分配，其余进程增加引用即可），并且这里应先释放bucket a的锁，然后获取bucket a对应的eviction lock以避免死锁；两个获取了不同bucket对应的eviction的锁后，由于会hashtable的遍历是顺序进行的，也就是获取锁是有序的，因此不会产生死锁的情况
