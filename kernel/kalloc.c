// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

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

  acquire(&kmem.lock);
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

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
// --- my code for lab10 start ---
// mmap_alloc -> check mmap, mappage, prot, readi
uint64 mmap_alloc(uint64 va) {
  struct proc *p = myproc();
  int i;
  for (i = 0; i < NVMA; ++i) {
    if (p->vma[i].len && p->vma[i].start <= va && va < p->vma[i].end){
      break;
    }
  }
  // not a mmap page fault
  if (i == NVMA){
    return -1;
  }
  struct vma* v = &p->vma[i];
//  if (va_pg >= p->sz) {
//    return -1;
//  }
  uint64 va_pg = PGROUNDDOWN(va);
  if (va_pg < p->trapframe->sp) {
    return -2;
  }
  void *mem = kalloc();
  if (mem == 0) {
    return -3;
  }
  memset(mem, 0, PGSIZE);

  //int readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
  ilock(v->file->ip);
  readi(v->file->ip, 0, (uint64)mem, v->off + (va_pg - v->start), PGSIZE);
  iunlock(v->file->ip);

  // PTE_V 不用设置,因为mappage里会设置
  int flags = PTE_U;
  if (v->readable){
    flags |= PTE_R;
  }
  if (v->writable){
    flags |= PTE_W;
  }
  if (mappages(p->pagetable, va_pg, PGSIZE, (uint64)mem, flags) != 0) {
    kfree(mem);
    return -4;
  }
  return 0;
}
// --- my code for lab10 end ---