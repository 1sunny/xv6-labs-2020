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

struct bucket {
    struct spinlock lock;
    struct buf head;
} table[NBUCKET];

struct {
    struct spinlock lock;
    struct buf buf[NBUF];

    // Linked list of all buffers, through prev/next.
    // Sorted by how recently the buffer was used.
    // head.next is most recent, head.prev is least.
//  struct buf head;
    uint earliest;
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

// must hold lock_i
struct buf *find_in_bucket_i(int i) {
  struct buf *b = table[i].head.next;
  while (b) {
    if (b->refcnt == 0 && b->timestamp == bcache.earliest) {
      break;
    }
    b = b->next;
  }
  return b;
}

// hold lock_i
uint find_min(int i) {
  uint mn = __UINT32_MAX__;
  struct buf *b = table[i].head.next;
  while (b) {
    if (b->refcnt == 0 && b->timestamp < mn) {
      mn = b->timestamp;
    }
    b = b->next;
  }
  return mn;
}

void set_attr(struct buf *b, uint dev, uint blockno) {
  b->dev = dev;
  b->blockno = blockno;
  b->valid = 0;
  b->refcnt = 1;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf *
bget(uint dev, uint blockno) {
  int p = blockno % NBUCKET;
  acquire(&table[p].lock);
  struct buf *b = table[p].head.next;
  while (b) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&table[p].lock);
      acquiresleep(&b->lock);
      return b;
    }
    b = b->next;
  }

  acquire(&bcache.lock);
  uint mn = __UINT32_MAX__;
  for (int i = 0; i < NBUCKET; ++i) {
    if (i != p) {
      acquire(&table[i].lock);
    }
    uint cur = find_min(i);
    if (cur < mn) {
      mn = cur;
    }
    if (i != p) {
      release(&table[i].lock);
    }
  }
  bcache.earliest = mn;

  for (int i = 0; i < NBUCKET; ++i) {
    if (i != p) {
      acquire(&table[i].lock);
    }
    b = find_in_bucket_i(i);
    if (b) {
      // --- remove from bucket i start ---
      struct buf *pre = &table[i].head;
      struct buf *now = table[i].head.next;
      while (now != b) {
        pre = now;
        now = now->next;
      }
      pre->next = now->next;
      // --- remove from bucket i end ---
      if (i != p){
        release(&table[i].lock);
      }

      // set attr and insert into bucket p
      set_attr(b, dev, blockno);
      // insert
      b->next = table[p].head.next;
      table[p].head.next = b;
      break;
    }
    if (i != p) {
      release(&table[i].lock);
    }
  }
  printf("-------\n");
  release(&table[p].lock);
  release(&bcache.lock);
//  for (int i = 0; i < NBUCKET; ++i) {
//    struct buf* b = table[i].head.next;
//    while (b){
//      printf("blockno: %d, refcnt: %d,  timestamp: %d\n", b->blockno, b->refcnt, b->timestamp);
//      b = b->next;
//    }
//    printf("\n");
//  }
//  printf("------------------\n");
  if (b) {
    acquiresleep(&b->lock);
    return b;
  }
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


//#include "types.h"
//#include "param.h"
//#include "spinlock.h"
//#include "sleeplock.h"
//#include "riscv.h"
//#include "defs.h"
//#include "fs.h"
//#include "buf.h"
//
//#define NBUCKET 13
//#undef NBUF
//#define NBUF (NBUCKET * 3)
//
//struct {
//    struct spinlock lock;
//    struct buf buf[NBUF];
//} bcache;
//
//struct bucket {
//    struct spinlock lock;
//    struct buf head;
//}hashtable[NBUCKET];
//
//int
//hash(uint dev, uint blockno)
//{
//  return blockno % NBUCKET;
//}
//
//void
//binit(void)
//{
//  struct buf *b;
//
//  initlock(&bcache.lock, "bcache");
//
//  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
//    initsleeplock(&b->lock, "buffer");
//  }
//
//  b = bcache.buf;
//  for (int i = 0; i < NBUCKET; i++) {
//    initlock(&hashtable[i].lock, "bcache_bucket");
//    for (int j = 0; j < NBUF / NBUCKET; j++) {
//      b->blockno = i;
//      b->next = hashtable[i].head.next;
//      hashtable[i].head.next = b;
//      b++;
//    }
//  }
//}
//
//// Look through buffer cache for block on device dev.
//// If not found, allocate a buffer.
//// In either case, return locked buffer.
//static struct buf*
//bget(uint dev, uint blockno)
//{
//  // printf("dev: %d blockno: %d Status: ", dev, blockno);
//  struct buf *b;
//
//  int idx = hash(dev, blockno);
//  struct bucket* bucket = hashtable + idx;
//  acquire(&bucket->lock);
//
//  // Is the block already cached?
//  for(b = bucket->head.next; b != 0; b = b->next){
//    if(b->dev == dev && b->blockno == blockno){
//      b->refcnt++;
//      release(&bucket->lock);
//      acquiresleep(&b->lock);
//      // printf("Cached %p\n", b);
//      return b;
//    }
//  }
//
//  // Not cached.
//  // First try to find in current bucket.
//  int min_time = 0x8fffffff;
//  struct buf* replace_buf = 0;
//
//  for(b = bucket->head.next; b != 0; b = b->next){
//    if(b->refcnt == 0 && b->timestamp < min_time) {
//      replace_buf = b;
//      min_time = b->timestamp;
//    }
//  }
//  if(replace_buf) {
//    // printf("Local %d %p\n", idx, replace_buf);
//    goto find;
//  }
//
//  // Try to find in other bucket.
//  acquire(&bcache.lock);
//  refind:
//  for(b = bcache.buf; b < bcache.buf + NBUF; b++) {
//    if(b->refcnt == 0 && b->timestamp < min_time) {
//      replace_buf = b;
//      min_time = b->timestamp;
//    }
//  }
//  if (replace_buf) {
//    // remove from old bucket
//    int ridx = hash(replace_buf->dev, replace_buf->blockno);
//    acquire(&hashtable[ridx].lock);
//    if(replace_buf->refcnt != 0)  // be used in another bucket's local find between finded and acquire
//    {
//      release(&hashtable[ridx].lock);
//      goto refind;
//    }
//    struct buf *pre = &hashtable[ridx].head;
//    struct buf *p = hashtable[ridx].head.next;
//    while (p != replace_buf) {
//      pre = pre->next;
//      p = p->next;
//    }
//    pre->next = p->next;
//    release(&hashtable[ridx].lock);
//    // add to current bucket
//    replace_buf->next = hashtable[idx].head.next;
//    hashtable[idx].head.next = replace_buf;
//    release(&bcache.lock);
//    // printf("Global %d -> %d %p\n", ridx, idx, replace_buf);
//    goto find;
//  }
//  else {
//    panic("bget: no buffers");
//  }
//
//  find:
//  replace_buf->dev = dev;
//  replace_buf->blockno = blockno;
//  replace_buf->valid = 0;
//  replace_buf->refcnt = 1;
//  release(&bucket->lock);
//  acquiresleep(&replace_buf->lock);
//  return replace_buf;
//}
//
//// Return a locked buf with the contents of the indicated block.
//struct buf*
//bread(uint dev, uint blockno)
//{
//  struct buf *b;
//
//  b = bget(dev, blockno);
//  if(!b->valid) {
//    virtio_disk_rw(b, 0);
//    b->valid = 1;
//  }
//  return b;
//}
//
//// Write b's contents to disk.  Must be locked.
//void
//bwrite(struct buf *b)
//{
//  if(!holdingsleep(&b->lock))
//    panic("bwrite");
//  virtio_disk_rw(b, 1);
//}
//
//// Release a locked buffer.
//// Move to the head of the most-recently-used list.
//void
//brelse(struct buf *b)
//{
//  if(!holdingsleep(&b->lock))
//    panic("brelse");
//
//  releasesleep(&b->lock);
//
//  int idx = hash(b->dev, b->blockno);
//
//  acquire(&hashtable[idx].lock);
//  b->refcnt--;
//  if (b->refcnt == 0) {
//    // no one is waiting for it.
//    b->timestamp = ticks;
//  }
//
//  release(&hashtable[idx].lock);
//}
//
//void
//bpin(struct buf *b) {
//  int idx = hash(b->dev, b->blockno);
//  acquire(&hashtable[idx].lock);
//  b->refcnt++;
//  release(&hashtable[idx].lock);
//}
//
//void
//bunpin(struct buf *b) {
//  int idx = hash(b->dev, b->blockno);
//  acquire(&hashtable[idx].lock);
//  b->refcnt--;
//  release(&hashtable[idx].lock);
//}

