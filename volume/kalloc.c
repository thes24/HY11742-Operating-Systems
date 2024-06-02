// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"
#include "proc.h"

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
} kmem;

int page_ref_count[PHYSTOP / PGSIZE];

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  freerange(vstart, vend);
}

void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;
}

void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uint)vstart);
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE)
    kfree(p);
}
//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(char *v)
{
  struct run *r;

  if((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(v, 1, PGSIZE);

  if(kmem.use_lock)
    acquire(&kmem.lock);

  if (--page_ref_count[V2P(v) / PGSIZE] > 0) {
    release(&kmem.lock);
    return;
  }

  r = (struct run*)v;
  r->next = kmem.freelist;
  kmem.freelist = r;
  if(kmem.use_lock)
    release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
char*
kalloc(void)
{
  struct run *r;

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = kmem.freelist;
  if(r) {
    kmem.freelist = r->next;
    uint pa = (uint)r;
    page_ref_count[pa / PGSIZE] = 1;
  }
  if(kmem.use_lock)
    release(&kmem.lock);
  return (char*)r;
}

// Ref Count
void incr_refc(uint pa) {
  acquire(&kmem.lock);
  page_ref_count[pa / PGSIZE]++;
  release(&kmem.lock);
}

void decr_refc(uint pa) {
  acquire(&kmem.lock);
  if (--page_ref_count[pa / PGSIZE] == 0) {
    kfree((void*)P2V(pa));
  }
  release(&kmem.lock);
}

int get_refc(uint pa) {
  int count;

  acquire(&kmem.lock);
  count = page_ref_count[pa / PGSIZE];
  release(&kmem.lock);

  return count;
}

// Count
int countfp(void) {
  struct run *r;
  int count = 0;

  acquire(&kmem.lock);
  for (r = kmem.freelist; r; r = r -> next) {
    count++;
  }
  release(&kmem.lock);

  return count;
}

int countvp(void) {
  struct proc *curproc = myproc();
  uint sz = curproc -> sz;
  return sz / PGSIZE;
}

int countpp(void) {
  struct proc *curproc = myproc();
  uint sz = curproc -> sz;
  uint count = 0;

  for (uint addr = 0; addr < sz; addr += PGSIZE) {
    pde_t *pde = &curproc -> pgdir[PDX(addr)];

    if (*pde & PTE_P && (addr < KERNBASE || addr >= KERNBASE + PHYSTOP)) {
      count++;
    }
  }

  return count;
}

int countptp(void) {
  struct proc *curproc = myproc();
  int count = 0;

  count++;

  pde_t *pgdir = curproc -> pgdir;
  for(int i = 0; i < NPDENTRIES; i++) {
    if(pgdir[i] & PTE_P) {
      count++;
    }
  }

  pde_t *kpgdir = (pde_t*)P2V(PGROUNDUP(V2P(pgdir)));
  for(int j = KERNBASE / PGSIZE; j < NPDENTRIES; j++) {
    if(kpgdir[j] & PTE_P) {
      count++;
    }
  }

  return count;
}
