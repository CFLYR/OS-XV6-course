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

// 每个CPU核心都有自己的空闲链表和锁
struct kmem {
  struct spinlock lock;
  struct run *freelist;
};

// 为每个CPU核心创建一个kmem结构
struct kmem kmems[NCPU];

void
kinit()
{
  for (int i = 0; i < NCPU; i++) {
    char lock_name[16];
    snprintf(lock_name, sizeof(lock_name), "kmem_%d", i);
    initlock(&kmems[i].lock, lock_name);
  }
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

void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();
  int cid = cpuid();
  acquire(&kmems[cid].lock);
  r->next = kmems[cid].freelist;
  kmems[cid].freelist = r;
  release(&kmems[cid].lock);
  pop_off();
}

void *
kalloc(void)
{
  struct run *r;

  push_off();
  int cid = cpuid();
  
  acquire(&kmems[cid].lock);
  r = kmems[cid].freelist;
  if(r) {
    kmems[cid].freelist = r->next;
  }
  release(&kmems[cid].lock);

  if(r) {
    pop_off();
    memset((char*)r, 5, PGSIZE);
    return (void*)r;
  }

  // 本地分配失败，开始窃取
  for(int i = 0; i < NCPU; i++) {
    if(i == cid) continue;

    acquire(&kmems[i].lock);
    // 从对方的链表头开始窃取
    struct run *steal_head = kmems[i].freelist;
    if(steal_head) {
      // 如果对方链表非空，窃取一半
      struct run *slow = steal_head;
      struct run *fast = steal_head;
      while(fast->next && fast->next->next){
        slow = slow->next;
        fast = fast->next->next;
      }
      // slow现在指向前半部分的尾部
      // 我们更新被窃取cpu的链表为后半部分
      kmems[i].freelist = slow->next;
      // 将前半部分(steal_head到slow)截断
      slow->next = 0;

      release(&kmems[i].lock);
      
      // 将窃取来的前半部分链表（头是steal_head）加入到自己的空闲链表
      acquire(&kmems[cid].lock);
      kmems[cid].freelist = steal_head;
      
      // 从现在非空的本地链表中分配一个
      r = kmems[cid].freelist;
      kmems[cid].freelist = r->next;
      
      release(&kmems[cid].lock);
      
      pop_off();
      memset((char*)r, 5, PGSIZE);
      return (void*)r;
    }
    release(&kmems[i].lock);
  }

  pop_off();
  return 0; // 窃取失败
}