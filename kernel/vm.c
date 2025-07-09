#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

#ifdef LAB_NET
  // PCI-E ECAM (configuration space), for pci.c
  kvmmap(kpgtbl, 0x30000000L, 0x30000000L, 0x10000000, PTE_R | PTE_W);

  // pci.c maps the e1000's registers here.
  kvmmap(kpgtbl, 0x40000000L, 0x40000000L, 0x20000, PTE_R | PTE_W);
#endif  

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
#ifdef LAB_PGTBL
      if(PTE_LEAF(*pte)) {
        return pte;
      }
#endif
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}


// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}



// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
//   pagetable：进程的页表
//   va：要解除映射的起始虚拟地址
//   npages：要解除映射的页数
//   do_free：是否释放物理页
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;
  int sz;
  // 检查虚拟地址必须页对齐
  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");
  // 遍历每一页，解除映射
  for(a = va; a < va + npages * PGSIZE; a += sz){
    sz = PGSIZE;
    // 获取页表项（不分配页表，所以 alloc=0）
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    // 检查页表项是否有效（是否映射）
    if((*pte & PTE_V) == 0) {
      printf("va=%ld pte=%ld\n", a, *pte);
      panic("uvmunmap: not mapped");
    }
    // 检查该页表项是否为叶子节点（是否真正映射了物理页）
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    // 取得对应物理地址
    uint64 pa = PTE2PA(*pte);
    // 如果是超级页（物理地址 >= SUPERBASE），则说明是 2MB 映射
    // 需要额外调整步长，使得下一次循环跳过整个超级页范围
    if (pa >= SUPERBASE){
      a += SUPERPGSIZE;
      a -= sz; // 因为循环里还会加 sz（4KB），这里先减去以抵消
    }
    // 如果需要释放物理页帧
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      if (pa >= SUPERBASE) 
        superfree((void*)pa); // 释放超级页
      else 
        kfree((void*)pa);     // 释放普通页
    }
    // 清空页表项
    *pte = 0;
  }
}


// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvmfirst(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("uvmfirst: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}


// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a; 
  int sz; 
    
  if(newsz < oldsz)
    return oldsz;
    
  oldsz = PGROUNDUP(oldsz);
  // 利用超级页对其所浪费的空间
  for(a = oldsz; a < SUPERPGROUNDUP(oldsz) && a < newsz; a += sz){
    sz = PGSIZE; 
    mem = kalloc(); 
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz); 
      return 0;
    }
#ifndef LAB_SYSCALL
    memset(mem, 0, sz);
#endif
    if(mappages(pagetable, a, sz, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  // 第二步：尽可能使用超级页批量分配，提升性能并减少页表开销
  // 之所以加上 a + SUPERPGSIZE < newsz 的条件，是为了尽可能少地浪费内存
  for(; a + SUPERPGSIZE < newsz; a += sz){
    sz = SUPERPGSIZE; 
    mem = superalloc(); 
    if(mem == 0){
      break;
    }
#ifndef LAB_SYSCALL
    memset(mem, 0, sz);
#endif
    if(mappages(pagetable, a, sz, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      superfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  // 第三步：对剩下不足一个超级页的部分用普通页补齐
  for(; a < newsz; a += sz){
    sz = PGSIZE;
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
#ifndef LAB_SYSCALL
    memset(mem, 0, sz);
#endif
    if(mappages(pagetable, a, sz, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  // 返回新分配完成后的地址空间大小
  return newsz;
}


// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// 超级页映射只需要走到第1级，不需要走到最底层，这里也是我看别人博客才了解到的。
// 所以这里就没有必要用循环了。
pte_t* superwalk(pagetable_t pagetable, uint64 va, int alloc)
{
  // 检查虚拟地址是否越界
  if(va >= MAXVA)
    panic("superwalk");
  // 获取第2级页表中的页表项
  pte_t *pte = &pagetable[PX(2, va)];
  if(*pte & PTE_V) {
    // 如果该页表项有效（页表存在），进入第1级页表
    pagetable = (pagetable_t)PTE2PA(*pte); // 获取下一层页表的物理地址
    return &pagetable[PX(1, va)];          // 返回第1级页表中对应的 PTE 指针
  } else {
    // 页表项无效，页表不存在，需要根据 alloc 决定是否分配新的页表页
    // 注意：即使是映射超级页，它的页表结构也是由普通页组成的
    if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
      return 0;  // 分配失败或不允许分配，返回 NULL
    memset(pagetable, 0, PGSIZE); // 清零新分配的页表页
    *pte = PA2PTE(pagetable) | PTE_V; // 设置上层页表项为新分配页的地址，并置有效位
    // 返回第0级页表中的项
    return &pagetable[PX(0, va)];
  }
}




// - pagetable：进程页表
// - va：要映射的起始虚拟地址，必须页对齐
// - size：映射的大小，必须是页大小的倍数
// - pa：起始物理地址，决定是否使用超级页
// - perm：页表项权限位（如 PTE_W、PTE_U 等）
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  uint64 pgsize;
  pte_t *pte;

  // 根据物理地址是否超过 SUPERBASE 判断是否使用超级页
  if (pa >= SUPERBASE)
    pgsize = SUPERPGSIZE;
  else
    pgsize = PGSIZE; 

  // 检查虚拟地址是否页对齐
  if((va % pgsize) != 0)
    panic("mappages: va not aligned");

  // 检查 size 是否是页大小的整数倍
  if((size % pgsize) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");

  a = va;
  last = va + size - pgsize;

  // 遍历每一页并映射
  for(;;){
    // 1. 根据页大小选择 walk 方式，获取/创建对应的页表项地址
    if(pgsize == PGSIZE && (pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(pgsize == SUPERPGSIZE && (pte = superwalk(pagetable, a, 1)) == 0)
      return -1;

    // 2. 如果已经存在映射，说明重复映射，报错
    if(*pte & PTE_V)
      panic("mappages: remap");

    // 3. 设置页表项，包含物理地址 + 权限 + 有效位
    *pte = PA2PTE(pa) | perm | PTE_V;

    // 4. 判断是否完成整个映射区间
    if(a == last)
      break;

    // 5. 前进到下一页
    a += pgsize;
    pa += pgsize;
  }

  return 0;
}




// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
// 复制用户页表的内容：
// 把 old 页表中从 0 到 sz 的映射复制到 new 页表中。
// 如果对应的是超级页则使用 superalloc，否则使用 kalloc。
// 复制时连同数据内容一起复制（即数据页克隆）。
// 返回 0 表示成功，-1 表示失败（并清理 new 页表中已分配的页）。
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;
  int szinc;

  // 遍历 old 页表中从 0 到 sz 的所有虚拟地址
  for(i = 0; i < sz; i += szinc){
    szinc = PGSIZE;
    // 找到 old 页表中当前虚拟地址 i 对应的页表项
    // 由于此时使用的页已经被正确映射
    // 所以普通的walk也可以找到超级页的pte
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    // 提取物理地址与标志位
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    // 判断是普通页还是超级页
    if(pa >= SUPERBASE) {
      // 超级页大小设置为 2MB
      szinc = SUPERPGSIZE;
      // 分配新的超级页
      if((mem = superalloc()) == 0)
        goto err;
    }
    // 普通页
    else if((mem = kalloc()) == 0)
      goto err;
    // 拷贝数据内容（从旧物理页拷贝到新分配的物理页）
    memmove(mem, (char*)pa, szinc);
    // 映射到新页表中
    if(mappages(new, i, szinc, (uint64)mem, flags) != 0){
      if(szinc == SUPERPGSIZE)
        superfree(mem);
      else
        kfree(mem);
      goto err;
    }
  }
  return 0;
 err:
  // 若出错，释放 new 页表中已分配的页
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}


// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;
    if((pte = walk(pagetable, va0, 0)) == 0) {
      // printf("copyout: pte should exist 0x%x %d\n", dstva, len);
      return -1;
    }


    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
    
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;
  
  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}


// pagetable: 当前层级的页表指针
// level: 当前层级（2是顶层，0是页表最低层）
// va: 当前页表项对应的虚拟地址起始值
void PRINT(pagetable_t pagetable, int level, uint64 va) {
  uint64 sz;
  // 根据当前层级计算该层级每个页表项的虚拟地址的跨度
  if(level == 2) {
    sz = 512 * 512 * PGSIZE;
  } else if(level == 1) {
    sz = 512 * PGSIZE; 
  } else {
    sz = PGSIZE; 
  }
  // 遍历该页表的512个PTE
  for(int i = 0; i < 512; i++, va += sz) {
    pte_t pte = pagetable[i];
    // 无效，不管他
    if((pte & PTE_V) == 0)
      continue;
	// 树形结构(假)
    for(int j = level; j < 3; j++) {
      printf(" ..");
    }
    // 打印当前页表项的信息：
    // - 虚拟地址 va（从 pagetable 起开始累加）
    // - 页表项内容（pte）
    // - 对应的物理地址（通过 PTE2PA 提取）
    printf("%p: pte %p pa %p\n", 
           (pagetable_t)va, 
           (pagetable_t)pte, 
           (pagetable_t)PTE2PA(pte));
    // 如果该页表项不是叶子节点（即没有 R/W/X 权限），说明它指向下一级页表
    // 递归进入下一级页表打印，否则就不管他，直接走就行。
    if((pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      PRINT((pagetable_t)PTE2PA(pte), level - 1, va);
    }
  }
}

#ifdef LAB_PGTBL
void
vmprint(pagetable_t pagetable) {
  printf("page table %p\n", pagetable);
  PRINT(pagetable, 2, 0);
}
#endif



#ifdef LAB_PGTBL
pte_t*
pgpte(pagetable_t pagetable, uint64 va) {
  return walk(pagetable, va, 0);
}
#endif
