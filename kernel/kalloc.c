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
// --- my code for lab8 start ---
struct {
    struct spinlock lock;
    struct run *freelist;
} kmem[NCPU];

void
kinit() {
  char lock_name[10];
  for (int i = 0; i < NCPU; ++i) {
    snprintf(lock_name, sizeof lock_name, "kmem_%d", i);
    initlock(&kmem[i].lock, lock_name);
  }
  freerange(end, (void *) PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end) {
  char *p;
  p = (char *) PGROUNDUP((uint64) pa_start);
  for (; p + PGSIZE <= (char *) pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa) {
  struct run *r;

  if (((uint64) pa % PGSIZE) != 0 || (char *) pa < end || (uint64) pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *) pa;

  push_off();
  int cid = cpuid();
  acquire(&kmem[cid].lock);
  r->next = kmem[cid].freelist;
  kmem[cid].freelist = r;
  release(&kmem[cid].lock);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void) {
  struct run *r;
  // 函数 cpuid 返回当前的核心编号,但只有在中断关闭时调用它并使用其结果才是安全的
  push_off();
  int cid = cpuid();

  acquire(&kmem[cid].lock);
  r = kmem[cid].freelist;
  if (r) {
    kmem[cid].freelist = r->next;
  } else {
    for (int i = 0; i < NCPU; ++i) {
      if (i == cid) {
        continue;
      }
      acquire(&kmem[i].lock);
      int ok = 0;
      if (kmem[i].freelist) {
        // 一定是成功的,因为至少有一个空闲块
        ok = 1;
        // 快慢指针找到链表中点
        struct run *fast = kmem[i].freelist, *slow = kmem[i].freelist;
        while (fast) {
          fast = fast->next;
          // 防止只有一个空闲块的情况,这时slow不移动
          if (fast) {
            fast = fast->next;
            slow = slow->next;
          }
        }
        kmem[cid].freelist = kmem[i].freelist;
        kmem[i].freelist = slow->next;
        slow->next = 0;
        r = kmem[cid].freelist;
        kmem[cid].freelist = r->next;
      }
      release(&kmem[i].lock);
      if(ok){
        break;
      }
    }
  }
  release(&kmem[cid].lock);
  pop_off();

  if (r)
    memset((char *) r, 5, PGSIZE); // fill with junk
  return (void *) r;
}
// --- my code for lab8 end ---