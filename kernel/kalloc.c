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

// --- my code for lab6 start ---
struct {
    struct spinlock lock;
    uint page_ref[(PHYSTOP - KERNBASE) / PGSIZE];
} refcnt;

uint page_index(uint64 pa){
  return (pa - KERNBASE) / PGSIZE;
}

void get_lock(){
  acquire(&refcnt.lock);
}

void rea_lock(){
  release(&refcnt.lock);
}

uint get_ref(uint64 pa){
  return refcnt.page_ref[page_index(pa)];
}

void set_ref(uint64 pa, uint n){
  refcnt.page_ref[page_index(pa)] = n;
}

void inc_ref(uint64 pa){
  acquire(&refcnt.lock);
  refcnt.page_ref[page_index(pa)]++;
  release(&refcnt.lock);
}

void dec_ref(uint64 pa){
  acquire(&refcnt.lock);
  refcnt.page_ref[page_index(pa)]--;
  release(&refcnt.lock);
}
// --- my code for lab6 end ---

struct run {
    struct run *next;
};

struct {
    struct spinlock lock;
    struct run *freelist;
} kmem;

void
kinit() {
  initlock(&kmem.lock, "kmem");
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
  // --- my code for lab6 start ---
  get_lock();
  if (refcnt.page_ref[page_index((uint64)pa)] > 1) {
    refcnt.page_ref[page_index((uint64)pa)]--;
    rea_lock();
    return;
  }
  // --- my code for lab6 end ---
  struct run *r;

  if (((uint64) pa % PGSIZE) != 0 || (char *) pa < end || (uint64) pa >= PHYSTOP)
    panic("kfree");
  // --- code for lab6 start ---
  refcnt.page_ref[page_index((uint64)pa)] = 0;
  rea_lock();
  // --- code for lab6 end ---
  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *) pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void) {
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if (r){
    memset((char *) r, 5, PGSIZE); // fill with junk
    // --- my code for lab6 start ---
    inc_ref((uint64)r);
    // --- my code for lab6 end ---
  }
  return (void *) r;
}
// --- code for lab6 start ---
void *
kalloc_no_ref_lock(void) {
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if (r){
    memset((char *) r, 5, PGSIZE); // fill with junk
    // --- my code for lab6 start ---
    set_ref((uint64)r, 1);
    // --- my code for lab6 end ---
  }
  return (void *) r;
}
// --- code for lab6 end ---