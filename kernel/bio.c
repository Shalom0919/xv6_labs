// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13

struct bucket {
  struct spinlock lock;
  struct buf *head;
};

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct bucket bucket[NBUCKET];
  uint clock;
} bcache;

static uint
bhash(uint dev, uint blockno)
{
  return (dev + blockno) % NBUCKET;
}

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");
  bcache.clock = 0;
  for(int i = 0; i < NBUCKET; i++){
    initlock(&bcache.bucket[i].lock, "bcache.bucket");
    bcache.bucket[i].head = 0;
  }

  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    uint index;

    initsleeplock(&b->lock, "buffer");
    b->dev = (uint)-1;
    b->blockno = b - bcache.buf;
    b->refcnt = 0;
    b->timestamp = 0;
    index = bhash(b->dev, b->blockno);
    b->next = bcache.bucket[index].head;
    bcache.bucket[index].head = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b, *victim;
  uint index = bhash(dev, blockno);

  acquire(&bcache.bucket[index].lock);
  for(b = bcache.bucket[index].head; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bucket[index].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.bucket[index].lock);

  // Serialize misses so that two CPUs cannot create duplicate entries.
  acquire(&bcache.lock);

  // A different CPU may have inserted the block before we got the miss lock.
  acquire(&bcache.bucket[index].lock);
  for(b = bcache.bucket[index].head; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bucket[index].lock);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.bucket[index].lock);

  for(;;){
    uint oldest = (uint)-1;
    victim = 0;

    // Find the least-recently-used buffer that appeared unused.
    for(b = bcache.buf; b < bcache.buf + NBUF; b++){
      uint oldindex = bhash(b->dev, b->blockno);
      acquire(&bcache.bucket[oldindex].lock);
      if(b->refcnt == 0 && b->timestamp < oldest){
        oldest = b->timestamp;
        victim = b;
      }
      release(&bcache.bucket[oldindex].lock);
    }

    if(victim == 0)
      panic("bget: no buffers");

    uint oldindex = bhash(victim->dev, victim->blockno);
    acquire(&bcache.bucket[oldindex].lock);
    if(victim->refcnt != 0){
      release(&bcache.bucket[oldindex].lock);
      continue;
    }
    if(oldindex != index)
      acquire(&bcache.bucket[index].lock);

    struct buf **link = &bcache.bucket[oldindex].head;
    while(*link && *link != victim)
      link = &(*link)->next;
    if(*link == 0)
      panic("bget: lost buffer");
    *link = victim->next;

    victim->dev = dev;
    victim->blockno = blockno;
    victim->valid = 0;
    victim->refcnt = 1;
    victim->timestamp = 0;
    victim->next = bcache.bucket[index].head;
    bcache.bucket[index].head = victim;

    if(oldindex != index)
      release(&bcache.bucket[index].lock);
    release(&bcache.bucket[oldindex].lock);
    release(&bcache.lock);

    acquiresleep(&victim->lock);
    return victim;
  }
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
  uint index;

  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  index = bhash(b->dev, b->blockno);
  acquire(&bcache.bucket[index].lock);
  b->refcnt--;
  if(b->refcnt == 0)
    b->timestamp = __sync_add_and_fetch(&bcache.clock, 1);
  release(&bcache.bucket[index].lock);
}

void
bpin(struct buf *b) {
  uint index = bhash(b->dev, b->blockno);
  acquire(&bcache.bucket[index].lock);
  b->refcnt++;
  release(&bcache.bucket[index].lock);
}

void
bunpin(struct buf *b) {
  uint index = bhash(b->dev, b->blockno);
  acquire(&bcache.bucket[index].lock);
  b->refcnt--;
  if(b->refcnt == 0)
    b->timestamp = __sync_add_and_fetch(&bcache.clock, 1);
  release(&bcache.bucket[index].lock);
}

