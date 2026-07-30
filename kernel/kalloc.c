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

#define NPHYPAGES ((PHYSTOP - KERNBASE) / PGSIZE)
#define PAINDEX(pa) (((uint64)(pa) - KERNBASE) / PGSIZE)

struct {
  struct spinlock lock;
  int count[NPHYPAGES];
} krefs;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&krefs.lock, "krefs");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    // kfree() decrements the count, so give each boot-time free page
    // one temporary reference before placing it on the free list.
    krefinc((uint64)p);
    kfree(p);
  }
}

// Add a reference to an allocated physical page.
void
krefinc(uint64 pa)
{
  if((pa % PGSIZE) != 0 || pa < (uint64)end || pa >= PHYSTOP)
    panic("krefinc");

  acquire(&krefs.lock);
  krefs.count[PAINDEX(pa)]++;
  release(&krefs.lock);
}

// Return the current number of references to a physical page.
int
krefcnt(uint64 pa)
{
  int count;

  if((pa % PGSIZE) != 0 || pa < (uint64)end || pa >= PHYSTOP)
    panic("krefcnt");

  acquire(&krefs.lock);
  count = krefs.count[PAINDEX(pa)];
  release(&krefs.lock);
  return count;
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int count;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&krefs.lock);
  if(krefs.count[PAINDEX(pa)] < 1){
    release(&krefs.lock);
    panic("kfree ref");
  }
  count = --krefs.count[PAINDEX(pa)];
  release(&krefs.lock);

  // Other page tables still refer to this page.
  if(count > 0)
    return;

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

  if(r){
    acquire(&krefs.lock);
    if(krefs.count[PAINDEX(r)] != 0){
      release(&krefs.lock);
      panic("kalloc ref");
    }
    krefs.count[PAINDEX(r)] = 1;
    release(&krefs.lock);
    memset((char*)r, 5, PGSIZE); // fill with junk
  }
  return (void*)r;
}
