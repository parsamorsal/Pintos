#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include <stdbool.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define INDEXES_PER_SECTOR 128
#define DIRECT_BLOCKS_CNT 123

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    block_sector_t direct_blocks[DIRECT_BLOCKS_CNT];
    block_sector_t indirect_block;
    block_sector_t doubly_indirect_block;
    bool is_dir;
};

struct indirect_block_sec {
    block_sector_t indirect_blocks[INDEXES_PER_SECTOR];
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors(off_t size) {
    return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
};

static block_sector_t get_indirect_sector(block_sector_t sector, off_t pos) {
    struct indirect_block_sec *indirect_block_sec1;
    indirect_block_sec1 = malloc(sizeof *indirect_block_sec1);
    block_read(fs_device, sector, indirect_block_sec1);
    block_sector_t sec1 = indirect_block_sec1->indirect_blocks[pos];
    free(indirect_block_sec1);
    return sec1;
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector(const struct inode_disk *inode_disk, off_t pos) {
    ASSERT(inode_disk != NULL);
    pos = pos / BLOCK_SECTOR_SIZE;
    if (pos < DIRECT_BLOCKS_CNT) {
        return inode_disk->direct_blocks[pos];
    }
    if (pos < (DIRECT_BLOCKS_CNT + INDEXES_PER_SECTOR)) {
        return get_indirect_sector(inode_disk->indirect_block, pos - DIRECT_BLOCKS_CNT);
    }
    if (pos < (DIRECT_BLOCKS_CNT + INDEXES_PER_SECTOR + INDEXES_PER_SECTOR * INDEXES_PER_SECTOR)) {
        off_t indirect_offset = (pos - DIRECT_BLOCKS_CNT - INDEXES_PER_SECTOR) / INDEXES_PER_SECTOR;
        off_t indirect_double_offset = (pos - DIRECT_BLOCKS_CNT - INDEXES_PER_SECTOR) % INDEXES_PER_SECTOR;
        return get_indirect_sector(get_indirect_sector(inode_disk->doubly_indirect_block, indirect_offset),
                                   indirect_double_offset);
    } else
        return -1;
}

static bool
inode_extend_indirect(block_sector_t *p_entry, size_t num_sectors, int level) {
    static char zeros[BLOCK_SECTOR_SIZE];
    if (level == 0) {
        if (*p_entry == 0) {
            if (!free_map_allocate(1, p_entry))
                return false;
            block_write(fs_device, *p_entry, zeros);
        }
        return true;
    }
    struct indirect_block_sec indirect_block;
    if (*p_entry == 0) {
        free_map_allocate(1, p_entry);
        block_write(fs_device, *p_entry, zeros);
    }
    block_read(fs_device, *p_entry, &indirect_block);

    size_t unit = (level == 1 ? 1 : INDEXES_PER_SECTOR);
    size_t i, l = DIV_ROUND_UP(num_sectors, unit);

    for (i = 0; i < l; ++i) {
        size_t subsize = num_sectors < unit ? num_sectors : unit;
        if (!inode_extend_indirect(&indirect_block.indirect_blocks[i], subsize, level - 1))
            return false;
        num_sectors -= subsize;
    }

    ASSERT(num_sectors == 0);
    block_write(fs_device, *p_entry, &indirect_block);
    return true;
}

static bool
inode_extend(struct inode_disk *disk_inode, off_t length) {
    static char zeros[BLOCK_SECTOR_SIZE];
    if (length < 0) return false;

    size_t remaining_num_sectors = bytes_to_sectors(length);
    size_t i;

    size_t min_size = remaining_num_sectors;
    if (min_size > DIRECT_BLOCKS_CNT)
        min_size = DIRECT_BLOCKS_CNT;

    for (i = 0; i < min_size; ++i) {
        if (disk_inode->direct_blocks[i] == 0) {
            if (!free_map_allocate(1, &disk_inode->direct_blocks[i]))
                return false;
            block_write(fs_device, disk_inode->direct_blocks[i], zeros);
        }
    }
    remaining_num_sectors -= min_size;
    if (remaining_num_sectors == 0) return true;

    min_size = remaining_num_sectors;
    if (min_size > INDEXES_PER_SECTOR)
        min_size = INDEXES_PER_SECTOR;

    if (!inode_extend_indirect(&disk_inode->indirect_block, min_size, 1))
        return false;
    remaining_num_sectors -= min_size;
    if (remaining_num_sectors == 0) return true;

    min_size = remaining_num_sectors;
    if (min_size > (INDEXES_PER_SECTOR ^ 2))
        min_size = (INDEXES_PER_SECTOR ^ 2);
    if (!inode_extend_indirect(&disk_inode->doubly_indirect_block, min_size, 2))
        return false;
    remaining_num_sectors -= min_size;
    if (remaining_num_sectors == 0) return true;
    ASSERT(remaining_num_sectors == 0);
    return false;
}

static void
inode_deallocate_indirect(block_sector_t entry, size_t num_sectors, int level) {

    if (level == 0) {
        free_map_release(entry, 1);
        return;
    }
    struct indirect_block_sec indirect_block;
    block_read(fs_device, entry, &indirect_block);

    size_t unit = (level == 1 ? 1 : INDEXES_PER_SECTOR);
    size_t i, l = DIV_ROUND_UP(num_sectors, unit);

    for (i = 0; i < l; ++i) {
        size_t subsize = num_sectors < unit ? num_sectors : unit;
        inode_deallocate_indirect(indirect_block.indirect_blocks[i], subsize, level - 1);
        num_sectors -= subsize;
    }

    ASSERT(num_sectors == 0);
    free_map_release(entry, 1);
}

static
bool inode_deallocate(struct inode *inode) {
    off_t file_length = inode->data.length;
    if (file_length < 0) return false;

    size_t num_sectors = bytes_to_sectors(file_length);
    size_t i, minsize;

    minsize = num_sectors < DIRECT_BLOCKS_CNT ? num_sectors : DIRECT_BLOCKS_CNT;
    for (i = 0; i < minsize; ++i) {
        free_map_release(inode->data.direct_blocks[i], 1);
    }
    num_sectors -= minsize;

    minsize = num_sectors < INDEXES_PER_SECTOR ? num_sectors : INDEXES_PER_SECTOR;
    if (minsize > 0) {
        inode_deallocate_indirect(inode->data.indirect_block, minsize, 1);
        num_sectors -= minsize;
    }

    minsize = num_sectors < (INDEXES_PER_SECTOR ^ 2) ? num_sectors : (INDEXES_PER_SECTOR ^ 2);
    if (minsize > 0) {
        inode_deallocate_indirect(inode->data.doubly_indirect_block, minsize, 2);
        num_sectors -= minsize;
    }

    ASSERT(num_sectors == 0);
    return true;
}


/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init(void) {
    list_init(&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create(block_sector_t sector, off_t length) {
    struct inode_disk *disk_inode = NULL;
    bool success = false;

    ASSERT(length >= 0);

    /* If this assertion fails, the inode structure is not exactly
       one sector in size, and you should fix that. */
    ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

    disk_inode = calloc(1, sizeof *disk_inode);
    if (disk_inode != NULL) {
        disk_inode->length = length;
        disk_inode->magic = INODE_MAGIC;
//        disk_inode->is_dir = is_dir;
        if (inode_extend(disk_inode, disk_inode->length)) {
            block_write(fs_device, sector, disk_inode);
            success = true;
        }
        free(disk_inode);
    }
    return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open(block_sector_t sector) {
    struct list_elem *e;
    struct inode *inode;

    /* Check whether this inode is already open. */
    for (e = list_begin(&open_inodes); e != list_end(&open_inodes);
         e = list_next(e)) {
        inode = list_entry(e,
        struct inode, elem);
        if (inode->sector == sector) {
            inode_reopen(inode);
            return inode;
        }
    }

    /* Allocate memory. */
    inode = malloc(sizeof *inode);
    if (inode == NULL)
        return NULL;

    /* Initialize. */
    list_push_front(&open_inodes, &inode->elem);
    inode->sector = sector;
    inode->open_cnt = 1;
    inode->deny_write_cnt = 0;
    inode->removed = false;
    block_read(fs_device, inode->sector, &inode->data);
    return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen(struct inode *inode) {
    if (inode != NULL)
        inode->open_cnt++;
    return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber(const struct inode *inode) {
    return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close(struct inode *inode) {
    /* Ignore null pointer. */
    if (inode == NULL)
        return;

    /* Release resources if this was the last opener. */
    if (--inode->open_cnt == 0) {
        /* Remove from inode list and release lock. */
        list_remove(&inode->elem);

        /* Deallocate blocks if removed. */
        if (inode->removed) {
            free_map_release(inode->sector, 1);
            inode_deallocate(inode);
        }

        free(inode);
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove(struct inode *inode) {
    ASSERT(inode != NULL);
    inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at(struct inode *inode, void *buffer_, off_t size, off_t offset) {
    uint8_t *buffer = buffer_;
    off_t bytes_read = 0;
    uint8_t *bounce = NULL;

    while (size > 0) {
        /* Disk sector to read, starting byte offset within sector. */
        block_sector_t sector_idx = byte_to_sector(&inode->data, offset);
        int sector_ofs = offset % BLOCK_SECTOR_SIZE;

        /* Bytes left in inode, bytes left in sector, lesser of the two. */
        off_t inode_left = inode_length(inode) - offset;
        int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
        int min_left = inode_left < sector_left ? inode_left : sector_left;

        /* Number of bytes to actually copy out of this sector. */
        int chunk_size = size < min_left ? size : min_left;
        if (chunk_size <= 0)
            break;

        if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
            /* Read full sector directly into caller's buffer. */
            block_read(fs_device, sector_idx, buffer + bytes_read);
        } else {
            /* Read sector into bounce buffer, then partially copy
               into caller's buffer. */
            if (bounce == NULL) {
                bounce = malloc(BLOCK_SECTOR_SIZE);
                if (bounce == NULL)
                    break;
            }
            block_read(fs_device, sector_idx, bounce);
            memcpy(buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }

        /* Advance. */
        size -= chunk_size;
        offset += chunk_size;
        bytes_read += chunk_size;
    }
    free(bounce);

    return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at(struct inode *inode, const void *buffer_, off_t size, off_t offset) {
    const uint8_t *buffer = buffer_;
    off_t bytes_written = 0;
    uint8_t *bounce = NULL;

    if (inode->deny_write_cnt)
        return 0;

    if (byte_to_sector(&inode->data, offset + size - 1) == -1u) {
        bool success;
        success = inode_extend(&inode->data, offset + size);
        if (!success) return 0;

        inode->data.length = offset + size;
        block_write(fs_device, inode->sector, &inode->data);
    }

    while (size > 0) {
        /* Sector to write, starting byte offset within sector. */
        block_sector_t sector_idx = byte_to_sector(&inode->data, offset);
        int sector_ofs = offset % BLOCK_SECTOR_SIZE;

        /* Bytes left in inode, bytes left in sector, lesser of the two. */
        off_t inode_left = inode_length(inode) - offset;
        int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
        int min_left = inode_left < sector_left ? inode_left : sector_left;

        /* Number of bytes to actually write into this sector. */
        int chunk_size = size < min_left ? size : min_left;
        if (chunk_size <= 0)
            break;

        if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
            /* Write full sector directly to disk. */
            block_write(fs_device, sector_idx, buffer + bytes_written);
        } else {
            /* We need a bounce buffer. */
            if (bounce == NULL) {
                bounce = malloc(BLOCK_SECTOR_SIZE);
                if (bounce == NULL)
                    break;
            }

            /* If the sector contains data before or after the chunk
               we're writing, then we need to read in the sector
               first.  Otherwise we start with a sector of all zeros. */
            if (sector_ofs > 0 || chunk_size < sector_left)
                block_read(fs_device, sector_idx, bounce);
            else
                memset(bounce, 0, BLOCK_SECTOR_SIZE);
            memcpy(bounce + sector_ofs, buffer + bytes_written, chunk_size);
            block_write(fs_device, sector_idx, bounce);
        }

        /* Advance. */
        size -= chunk_size;
        offset += chunk_size;
        bytes_written += chunk_size;
    }
    free(bounce);

    return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write(struct inode *inode) {
    inode->deny_write_cnt++;
    ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write(struct inode *inode) {
    ASSERT(inode->deny_write_cnt > 0);
    ASSERT(inode->deny_write_cnt <= inode->open_cnt);
    inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length(const struct inode *inode) {
    return inode->data.length;
}

