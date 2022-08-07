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
} kmem;

// -- my code for lab2 --
int free_mem = 0;

uint
free_amount()
{
//  return free_mem;
  uint count = 0;
  struct run* now = kmem.freelist;
  while (now){
    now = now->next;
    count++;
  }
  return count * PGSIZE;
}
// -- my code for lab2 --

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
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
  // 操作需要加锁
  acquire(&kmem.lock);
  // -- my code for lab2 --
  // free_mem += PGSIZE; //这样写测试结果会差一个4096,不知道为什么
  // -- my code for lab2 --

  // -- my comment --
  // freelist 是一个空闲内存块(一个内存块大小为4k)的链表
  // 采用头插法将要 free 的内存块加入链表
  // -- my comment --
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  // 操作需要加锁
  acquire(&kmem.lock);
  r = kmem.freelist;
  // -- my comment --
  // 使用 链表的头
  // -- my comment --
  if(r)
    kmem.freelist = r->next;

    // -- my code for lab2 --
    // free_mem -= PGSIZE; //这样写测试结果会差一个4096,不知道为什么
    // -- my code for lab2 --

  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk:垃圾
  return (void*)r;
}
