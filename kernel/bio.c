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

#define NBUCKET 13
#define NBUF (NBUCKET * 3)

/*
 这个实验是要对XV6的磁盘缓冲区进行优化.在初始的XV6磁盘缓冲区中是使用一个LRU链表来维护的,
 而这就导致了每次获取,释放缓冲区时就要对整个链表加锁,也就是说缓冲区的操作是完全串行进行的.

 为了提高并行性能,我们可以用哈希表来代替链表,这样每次获取和释放的时候,都只需要对哈希表的一个桶进行加锁,
 桶之间的操作就可以并行进行.只有当需要对缓冲区进行驱逐替换时,才需要对整个哈希表加锁来查找要替换的块

 使用哈希表就不能使用链表来维护LRU信息,因此需要在buf结构体中添加timestamp域来记录释放的事件,同时prev域也不再需要

 from: https://www.cnblogs.com/weijunji/p/xv6-study-12.html
 */

// 参考自 https://zhuanlan.zhihu.com/p/449726796
struct bucket {
    struct spinlock lock;
    struct buf head;
} table[NBUCKET];

struct {
    struct spinlock lock;
    struct buf buf[NBUF];
} bcache;

void
binit(void) {
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
    initsleeplock(&b->lock, "buffer");
    b->timestamp = 0;
  }
  b = bcache.buf;
  for (int i = 0; i < NBUCKET; ++i) {
    initlock(&table[i].lock, "buffer");
    for (int j = 0; j < NBUF / NBUCKET; j++) {
      b->blockno = i;
      b->next = table[i].head.next;
      table[i].head.next = b;
      b++;
    }
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf *
bget(uint dev, uint blockno) {
  int p = blockno % NBUCKET;
  acquire(&table[p].lock);
  struct buf *b;
  for (b = table[p].head.next; b; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&table[p].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&table[p].lock);

  // 对锁排序
  acquire(&bcache.lock);
  // 必须要在获取 bcache.lock 前释放 table[p].lock 然后重新获取, 否则会出现下面这种情况:
  // p1 hold: table[p1].lock, bcache.lock, 试图获取 table[p2].lock
  // p2 hold: table[p2].lock, 试图获取 bcache.lock
  acquire(&table[p].lock);
  // 由于之前的释放和再获取之间有一点间隙(可能其它线程(也bget相同的blockno)已经从其它地方找到空闲块了(放入了bucket[p]))
  // 所以需要重新遍历一次
  for (b = table[p].head.next; b; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&table[p].lock);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  uint mn = __UINT32_MAX__;
  struct buf* cur;
  b = 0;
  // find LRU buf in current bucket
  for (cur = table[p].head.next; cur; cur = cur->next) {
    if (cur->refcnt == 0 && cur->timestamp < mn) {
      b = cur;
    }
  }
  if (b){
    b->dev = dev;
    b->blockno = blockno;
    b->refcnt++;
    b->valid = 0;
    release(&table[p].lock);
    release(&bcache.lock);
    acquiresleep(&b->lock);
    return b;
  }
  b = 0;
  // find LRU buf in other buckets
  for (int i = (p+1)%NBUCKET; i != p; i = (i+1)%NBUCKET) {
    mn = __UINT32_MAX__;
    acquire(&table[i].lock);
    for (cur = table[i].head.next; cur; cur = cur->next){
      if (cur->refcnt == 0 && cur->timestamp < mn){
        b = cur;
        mn = cur->timestamp;
      }
    }
    if (b){
      b->dev = dev;
      b->blockno = blockno;
      b->refcnt++;
      b->valid = 0;
      // remove from bucket[i]
      for (cur = &table[i].head; cur->next != b; cur = cur->next){}
      cur->next = cur->next->next;
      release(&table[i].lock);
      // add to bucket[p]
      b->next = table[p].head.next;
      table[p].head.next = b;

      release(&table[p].lock);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
    release(&table[i].lock);
  }
  release(&table[p].lock);
  release(&bcache.lock);
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf *
bread(uint dev, uint blockno) {
  struct buf *b;

  b = bget(dev, blockno);
  if (!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b) {
  if (!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b) {
  if (!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int p = b->blockno % NBUCKET;
  acquire(&table[p].lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->timestamp = ticks;
  }

  release(&table[p].lock);
}

void
bpin(struct buf *b) {
  int p = b->blockno % NBUCKET;
  acquire(&table[p].lock);
  b->refcnt++;
  release(&table[p].lock);
}

void
bunpin(struct buf *b) {
  int p = b->blockno % NBUCKET;
  acquire(&table[p].lock);
  b->refcnt--;
  release(&table[p].lock);
}