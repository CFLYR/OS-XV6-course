// Buffer cache.
//
// The buffer cache is a hash table of buf structures holding
// cached copies of disk block contents. Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// This implementation uses a hash table with per-bucket locks to reduce
// contention on lookups. A global lock is used for eviction to simplify
// finding a victim buffer.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKETS 13 // 使用一个素数作为哈希桶的数量

struct {
  struct spinlock evict_lock;   // 全局锁，仅用于回收缓冲区
  struct buf buf[NBUF];         // 所有的缓冲区
  struct buf buckets[NBUCKETS]; // 哈希表的桶，每个桶是一个链表的头节点
  struct spinlock locks[NBUCKETS]; // 每个桶对应一个锁
} bcache;

// 哈希函数
static uint
hash(uint blockno)
{
  return blockno % NBUCKETS;
}

void
binit(void)
{
  struct buf *b;
  char lock_name[32];

  initlock(&bcache.evict_lock, "bcache_evict");
  
  for (int i = 0; i < NBUCKETS; i++) {
    snprintf(lock_name, sizeof(lock_name), "bcache_bucket_%d", i);
    initlock(&bcache.locks[i], lock_name);
    bcache.buckets[i].next = 0;
  }

  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
    // 初始时，所有缓冲区都属于 bucket 0
    b->next = bcache.buckets[0].next;
    bcache.buckets[0].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int i = hash(blockno);

  // 阶段一：在特定桶中查找（高并发）
  acquire(&bcache.locks[i]);
  for(b = bcache.buckets[i].next; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.locks[i]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.locks[i]);

  // 阶段二：未命中，需要回收一个缓冲区（低并发，使用全局锁）
  acquire(&bcache.evict_lock);
  
  // 再次检查缓存，因为在我们获取evict_lock期间，可能别的进程已经把这个块缓存了
  acquire(&bcache.locks[i]);
  for(b = bcache.buckets[i].next; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.locks[i]);
      release(&bcache.evict_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.locks[i]);

  // 确定未命中，现在寻找一个victim
  struct buf *victim = 0;
  int old_bucket_idx = -1;

  // 遍历所有桶寻找victim
  for (int j = 0; j < NBUCKETS; j++) {
    acquire(&bcache.locks[j]);
    for (b = bcache.buckets[j].next; b; b = b->next) {
        if (b->refcnt == 0) {
            victim = b;
            old_bucket_idx = j;
            goto found_victim;
        }
    }
    release(&bcache.locks[j]);
  }
  
  // 理论上不应该发生，因为总有空闲的buffer
  panic("bget: no buffers");

found_victim:
  // 此刻，我们持有victim所在桶(old_bucket_idx)的锁和全局evict_lock

  // 从旧桶链表中移除victim
  struct buf **p;
  for (p = &bcache.buckets[old_bucket_idx].next; *p; p = &(*p)->next) {
      if (*p == victim) {
          *p = victim->next;
          break;
      }
  }
  release(&bcache.locks[old_bucket_idx]);

  // 更新victim的元数据
  victim->dev = dev;
  victim->blockno = blockno;
  victim->valid = 0;
  victim->refcnt = 1;

  // 将victim加入到新桶中
  acquire(&bcache.locks[i]);
  victim->next = bcache.buckets[i].next;
  bcache.buckets[i].next = victim;
  release(&bcache.locks[i]);

  // 所有操作完成，释放全局回收锁
  release(&bcache.evict_lock);

  acquiresleep(&victim->lock);
  return victim;
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
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int i = hash(b->blockno);
  acquire(&bcache.locks[i]);
  b->refcnt--;
  release(&bcache.locks[i]);
}

void
bpin(struct buf *b) {
  int i = hash(b->blockno);
  acquire(&bcache.locks[i]);
  b->refcnt++;
  release(&bcache.locks[i]);
}

void
bunpin(struct buf *b) {
  int i = hash(b->blockno);
  acquire(&bcache.locks[i]);
  b->refcnt--;
  release(&bcache.locks[i]);
}