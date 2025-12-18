#include "cache.h"
#include "string.h"
#include "error.h"
#include "thread.h"
#include "intr.h"
#include "assert.h"
#include <stddef.h>
#include <limits.h>
#include "heap.h"
#include "io.h"

#define CACHE_BLOCK_AMMOUNT 64
// CHECK EVICTION POLICY METHOD AND IMPLEMENTATION
// struct cache
// {
//   struct io *bkgio;
//   struct
//   {
//     int block_id;

//     // uint8_t data[CACHE_BLKSZ];
//     void *data;

//     struct lock cache_lock;

//     // int status; // Dirty (1) or Clean (0) (DO WE NEED STATUS?)

//     int assign_num;
//   } cache_blocks[CACHE_BLOCK_AMMOUNT];
// };
struct cache
{
  struct io *bkgio;
  struct lock cache_lock;
  int lock_owner;
  struct
  {
    int block_id;

    // uint8_t data[CACHE_BLKSZ];
    uint8_t data[CACHE_BLKSZ];
    //int status; // Dirty (1) or Clean (0) Neither (-1)

    int assign_num;
  } cache_blocks[CACHE_BLOCK_AMMOUNT];
};

struct cache cache;

int create_cache(struct io *bkgio, struct cache **cptr)
{
  trace("%s()", __func__);
  // What am I supposed to do with bkgio, check if it has read and write?s
  if (bkgio == NULL)
  {
    return -EINVAL;
  }

  // Initialize Members
  for (int i = 0; i < CACHE_BLOCK_AMMOUNT; i++)
  {
    cache.cache_blocks[i].block_id = -1;
    // cache.cache_blocks[i].status = -1;
    cache.cache_blocks[i].assign_num = 0;
  }
  cache.bkgio = bkgio;
  lock_init(&cache.cache_lock);
  cache.lock_owner = -1;
  // kprintf("hello %p\n", cache->bkgio);
  *cptr = &cache;
  return 0;
}

int cache_get_block(struct cache *cache, unsigned long long pos, void **pptr)
{
  trace("%s()", __func__);
  if (cache == NULL || pos % CACHE_BLKSZ != 0)
  {
    return -EINVAL;
  }
  // If the block already exists in cache we set pptr to the data

  for (int i = 0; i < CACHE_BLOCK_AMMOUNT; i++) // when you are switching threads, you should flush or not
  {
    if (cache->cache_blocks[i].block_id == (pos / CACHE_BLKSZ))
    {
      *pptr = cache->cache_blocks[i].data;
      lock_acquire(&cache->cache_lock);
      cache->lock_owner = i;
      return 0;
    }
  }
  // Otherwise we have to read from block device
  // ioreadat(cache->bkgio, pos, cache->cache_blocks[i].data, CACHE_BLKSZ);
  for (int i = 0; i < CACHE_BLOCK_AMMOUNT; i++)
  {
    if (cache->cache_blocks[i].block_id == -1)
    {
      // kprintf("\n !!! BACKING DEVICE CALLED !!!\n");
      ioreadat(cache->bkgio, pos, cache->cache_blocks[i].data, CACHE_BLKSZ);
      *pptr = cache->cache_blocks[i].data;
      cache->cache_blocks[i].block_id = (pos / CACHE_BLKSZ);
      // kprintf("%d\n",cache->cache_blocks[i].block_id);
      lock_acquire(&cache->cache_lock);
      cache->lock_owner = i;
      return 0;
      // cache->cache_blocks[i].data = *pptr;
    }
  }
  // Evict if cache is full
  // QUESTION: are we supposed to evict a block that hasnt even gotten called in cache_release_block yet?
  int min = cache->cache_blocks[0].assign_num;
  for (int i = 0; i < CACHE_BLOCK_AMMOUNT; i++)
  {
    if (cache->cache_blocks[i].assign_num < min)
    {
      min = cache->cache_blocks[i].assign_num;
    }
  }
  for (int i = 0; i < CACHE_BLOCK_AMMOUNT; i++)
  {
    if (cache->cache_blocks[i].assign_num == min)
    {
      // kprintf("\n !!! BACKING DEVICE CALLED !!!\n");
      ioreadat(cache->bkgio, pos, cache->cache_blocks[i].data, CACHE_BLKSZ);
      *pptr = cache->cache_blocks[i].data;
      // cache->cache_blocks[i].data = *pptr;
      cache->cache_blocks[i].block_id = (pos / CACHE_BLKSZ);
      lock_acquire(&cache->cache_lock);
      cache->lock_owner = i;
      return 0;
    }
  }
  return -EIO;
}

void cache_release_block(struct cache *cache, void *pblk, int dirty)
{
  trace("%s()", __func__);
  int block_just_released = -1;
  int num_released_blocks = 0;
  for (int i = 0; i < CACHE_BLOCK_AMMOUNT; i++)
  {

    if (cache->cache_blocks[i].data == pblk)
    {
      if (dirty == CACHE_DIRTY)
      {
        iowriteat(cache->bkgio, cache->cache_blocks[i].block_id * CACHE_BLKSZ, pblk, CACHE_BLKSZ);
      }
      // kprintf("\n%d",i);
      block_just_released = i;
      break;
    }
  }
  // Tracking the Least Recently Used cache
  num_released_blocks = (cache->cache_blocks[block_just_released].assign_num == 0) ? 1 : 0;
  for (int i = 0; i < CACHE_BLOCK_AMMOUNT; i++)
  {
    if (cache->cache_blocks[i].assign_num != 0)
    {
      num_released_blocks++;
    }
  }
  if (cache->cache_blocks[block_just_released].assign_num != num_released_blocks)
  {
    if (cache->cache_blocks[block_just_released].assign_num == 0)
    {
      cache->cache_blocks[block_just_released].assign_num = num_released_blocks;
    }
    else
    {
      for (int i = 0; i < CACHE_BLOCK_AMMOUNT; i++)
      {
        if ((cache->cache_blocks[i].assign_num > 1))
        {
          cache->cache_blocks[i].assign_num -= 1;
        }
      }
      cache->cache_blocks[block_just_released].assign_num = num_released_blocks;
    }
  }

  if (block_just_released != -1)
  {
    lock_release(&cache->cache_lock);
    cache->lock_owner = -1;
  }
}

int cache_flush(struct cache *cache)
{
  trace("%s()", __func__);
  if (cache == NULL)
  {
    return -EINVAL;
  }
  if (cache->lock_owner != -1)
  {
    cache_release_block(cache, cache->cache_blocks[cache->lock_owner].data, CACHE_DIRTY);
  }
  // for (int i = 0; i < CACHE_BLOCK_AMMOUNT; i++)
  // {
  //   cache_release_block(cache, cache->cache_blocks[i].data, CACHE_DIRTY); // cache->cache_blocks[i].status or 1 to keep writing to the blocks
  // }
  return 0;
}

// void print_cache(struct cache *cache)
// {
//   for (int i = 0; i < CACHE_BLOCK_AMMOUNT; i++)
//   {
//     kprintf("Block ID: %d Assign Num:%d\n", cache->cache_blocks[i].block_id, cache->cache_blocks[i].assign_num);
//   }
// }