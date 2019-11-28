#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"


struct cache_entry_t {
    
    block_sector_t disk_sector;
    uint8_t buffer[BLOCK_SECTOR_SIZE];
    
    bool is_dirty;
    bool is_accessed;
    bool is_free;
};

void cache_init (void);

void cache_close (void);

void cache_read (block_sector_t sector, void *dst);

static void cache_flush (struct cache_entry_t *entry);

void cache_write (block_sector_t sector, const void *src);

static struct cache_entry_t* cache_lookup (block_sector_t sector);

void cache_close (void);

#endif