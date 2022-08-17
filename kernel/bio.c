// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
// 减少了读写磁盘的次数,为多个进程使用的磁盘块提供同步点
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
//   调用 bread 得到一个磁盘块的缓冲区
// * After changing buffer data, call bwrite to write it to disk.
//   调用 bwrite 将改变后的 buffer 写入磁盘
// * When done with the buffer, call brelse.
//   buffer使用完后调用 brelse 释放
// * Do not use the buffer after calling brelse.
//   brelse 后不要再使用 buffer
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.
//   同一时间,只有一个进程可以使用buffer


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // *** head.next is most recent, head.prev is least. ***
  struct buf head;
} bcache;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  // Create linked list of buffers
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// *** In either case, return locked buffer. ***
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  acquire(&bcache.lock);

  // Is the block already cached?
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      // bget 在 bcache.lock 临界区域之外获取缓冲区的睡眠锁是安全的
      // 因为非零 b->refcnt 防止缓冲区被重新用于不同的磁盘块
      // 睡眠锁保护块缓冲内容的读写,而 bcache.lock 保护有关缓存哪些块的信息
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    // 查找未在使用中的缓冲区
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      // 确保了 bread 将从磁盘读取块数据,而不是错误地使用缓冲区以前的内容
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.lock);
  b->refcnt--;
  // 学生提问:我有个关于brelease函数的问题,
  // 看起来它先释放了block cache的锁,然后再对引用计数refcnt减一,
  // 为什么可以这样呢?
  //Frans教授:如果我们释放了sleep lock,这时另一个进程正在等待锁,
  // 那么refcnt必然大于1,
  // 而b->refcnt --只是表明当前执行brelease的进程不再关心block cache
  // 如果还有其他进程正在等待锁,那么refcnt必然不等于0,
  // 我们也必然不会执行if(b->refcnt == 0)中的代码
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  
  release(&bcache.lock);
}

void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}


