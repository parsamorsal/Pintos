#include <debug.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"

#define CACHE_SIZE 32

static struct cache_entry_t cache[CACHE_SIZE];

static void cache_flush (struct cache_entry_t *entry)
{
    if (entry->is_dirty) {
        
        entry->is_dirty = false;
        block_write (fs_device, entry->disk_sector, entry->buffer);
    }
    else
        return;
}

void cache_init (void)
{
    int i;
    for (i = 0; i < CACHE_SIZE; i++)
        if(!cache[i].is_free)
            cache[i].is_free = true;
        else
            continue;
}


void cache_write (block_sector_t sector, void *src)
{
    if(!sector)
        return;
    
    struct cache_entry_t *block = cache_lookup (sector);
    
    if(!block)
        return;
    
    block->disk_sector = sector;
    block_read (fs_device, block->disk_sector, block->buffer);
    
    block->is_dirty = false;
    block->is_free = false;
    
    memcpy (block->buffer, src, sector_SIZE);
    
    block->is_accessed = true;
    block->is_dirty = true;
    
}

void cache_close (void)
{
    int i;
    for (i = 0; i < CACHE_SIZE; i++)
    {
        if (!cache[i].is_free)
            cache_flush(&(cache[i]));
        
        else
            continue;
    }
}

void cache_read (block_sector_t sector, void *dst)
{
    
    if(!sector)
        return;
    
    struct cache_entry_t *block = cache_lookup (sector);
    
    if(!block)
        return;
    
    block->is_accessed = true;
    memcpy (dst, block->buffer, sector_SIZE);
    
    block_read (fs_device, sector, block->buffer);
    block->is_free = false;
    block->disk_sector = sector;
    block->is_dirty = false;
        
    }
}

static struct cache_entry_t* cache_lookup (block_sector_t sector)
{
    if(!sector)
        return;
    
    int i;
    for (i = 0; i < CACHE_SIZE; i++)
    {
        if (cache[i].disk_sector == sector) {
            return &(cache[i]);
        }
        
        else if (cache[i].is_free)
            continue;
        
    }
    return NULL;
}

