// ktfs.c - KTFS implementation
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef KTFS_TRACE
#define TRACE
#endif

#define KTFS_BLKSZ 512
#define FILENAME_SIZE 14
#define DIR_SIZE (KTFS_BLKSZ / KTFS_DENSZ)
#define INODES_PER_BLK (KTFS_BLKSZ / KTFS_INOSZ) // inodes
#define MAX_FILES 95
#define NUM_DINDIRECT_BLOCKS_NEEDED 3
#define DIR_INODE_OFFSET 1
#define BYTE_SIZE 8
#define ROUND_UP(n, k) (((n) + (k) - 1) / (k) * (k))

#ifdef KTFS_DEBUG
#define DEBUG
#endif

#include "heap.h"
#include "fs.h"
#include "ioimpl.h"
#include "ktfs.h"
#include "error.h"
#include "thread.h"
#include "string.h"
#include "console.h"
#include "cache.h"
#include <assert.h>

// INTERNAL TYPE DEFINITIONS
//

// struct ktfs_file_info
// { // represents open file extra info (Not file on disk)

// };

struct ktfs_file
{ // file io struct - represents open file
    // Fill to fulfill spec
    struct ktfs_dir_entry *dentry;
    struct ktfs_inode *file_inode;
    // unsigned long long pos;
    // unsigned long long end;
    struct io fileio;
    int name_pos;
    int open;
};
struct file_name
{
    struct io *fio;
    char name[KTFS_MAX_FILENAME_LEN + sizeof(uint8_t)];
};
// Make larger file system struct
// Held globally for mount
struct bitmap_block
{
    uint8_t block[512];
};
struct file_setup
{
    struct ktfs_superblock super_blk;
    struct ktfs_inode root_dir_inode;
    struct io *diskio;
    struct cache *cptr;
    struct file_name open_file_names[MAX_FILES];
    int cur_file_count;
    // struct bitmap_block *bitmap_blocks;
    uint8_t *inode_bitmap;
    int inodecount;
    // struct lock filesetup_lock;
};

struct indirBlock
{
    uint32_t data[KTFS_BLKS_PER_INDIRECT];
};

// static struct io *diskio;
struct file_setup filesetup;
// static

// INTERNAL FUNCTION DECLARATIONS
//

int ktfs_mount(struct io *io);

int ktfs_open(const char *name, struct io **ioptr);
void ktfs_close(struct io *io);
long ktfs_readat(struct io *io, unsigned long long pos, void *buf, long len);
long ktfs_writeat(struct io *io, unsigned long long pos, const void *buf, long len);
int ktfs_create(const char *name);
int ktfs_delete(const char *name);
int ktfs_cntl(struct io *io, int cmd, void *arg);
int blocknum(struct ktfs_file *fio, unsigned long long idx);
int allocateblk(struct ktfs_file *fio, unsigned long long idx);
unsigned long long allocate_open_block(void);
int add_new_inode_datablk(struct ktfs_file *fio, unsigned long long idx);
void write_inode_to_disk(struct ktfs_file *fio);
int inodeidxblocknum(struct ktfs_inode *inode, unsigned long long idx, unsigned long long global_datablock_0);
void free_block(int block_num);
// int ktfs_getblksz(struct ktfs_file *fd);
// int ktfs_getend(struct ktfs_file *fd, void *arg);

int ktfs_flush(void);
int blkidx;
int dentryidx;
uint8_t general_block[CACHE_BLKSZ];
char *data;
struct bitmap_block bitmap_data;
// FUNCTION ALIASES
//

int fsmount(struct io *io)
    __attribute__((alias("ktfs_mount")));

int fsopen(const char *name, struct io **ioptr)
    __attribute__((alias("ktfs_open")));

int fsflush(void)
    __attribute__((alias("ktfs_flush")));

int fsdelete(const char *name)
    __attribute__((alias("ktfs_delete")));

int fscreate(const char *name)
    __attribute__((alias("ktfs_create")));

// EXPORTED FUNCTION DEFINITIONS
//

static const struct iointf ktfs_file_iointf = {
    .close = &ktfs_close,
    .writeat = &ktfs_writeat,
    .readat = &ktfs_readat,
    .cntl = &ktfs_cntl};

// What are we supposed to be doing in mount?
// int ktfs_mount(struct io *io) // done??
// {
//     trace("%s()", __func__);
//     // Init cache
//     create_cache(io, &filesetup.cptr);
//     // lock_init(&filesetup.filesetup_lock);

//     // Save reference to disk I/O endpoint
//     filesetup.diskio = ioaddref(io);
//     filesetup.cur_file_count = 0;

//     // populates superblock
//     cache_get_block(filesetup.cptr, 0, (void **)&data);
//     memcpy(&filesetup.super_blk, data, sizeof(struct ktfs_superblock));
//     cache_release_block(filesetup.cptr, data, CACHE_CLEAN);

//     unsigned long long root_idx = filesetup.super_blk.root_directory_inode;
//     unsigned long long block_idx = root_idx / INODES_PER_BLK;
//     unsigned long long inode_idx = root_idx % INODES_PER_BLK;

//     unsigned long long global_block_idx = 1 + filesetup.super_blk.bitmap_block_count + block_idx;

//     // Populate root directory inode
//     // filesetup.root_dir_inode = kcalloc(1, sizeof(struct ktfs_inode));

//     cache_get_block(filesetup.cptr, global_block_idx * KTFS_BLKSZ, (void **)&data);
//     memcpy(&filesetup.root_dir_inode, (data + (inode_idx * KTFS_INOSZ)), sizeof(struct ktfs_inode));
//     cache_release_block(filesetup.cptr, data, CACHE_CLEAN);

//     filesetup.inode_bitmap = kmalloc(BYTE_SIZE * INODES_PER_BLK * filesetup.super_blk.inode_block_count);
//     memset(filesetup.inode_bitmap, 0, BYTE_SIZE * INODES_PER_BLK * filesetup.super_blk.inode_block_count);
//     filesetup.inode_bitmap[filesetup.super_blk.root_directory_inode] = 1;

//     unsigned long long global_datablock_0 = 1 + filesetup.super_blk.bitmap_block_count + filesetup.super_blk.inode_block_count;
//     //struct ktfs_dir_entry dir[DIR_SIZE];

//     for (int i = 0; i < (filesetup.root_dir_inode.size)/sizeof(struct ktfs_inode); i++)
//     {
//         filesetup.inode_bitmap[i] = 1;
//     }
//     // kprintf("Line  181\n");
//     // for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++)
//     // {
//     //     kprintf("Line  184\n");
//     //     cache_get_block(filesetup.cptr, (filesetup.root_dir_inode.block[i] + global_datablock_0) * KTFS_BLKSZ, (void **)&data);
//     //     memcpy(dir, data, sizeof(dir));
//     //     cache_release_block(filesetup.cptr, data, CACHE_CLEAN);

//     //     for (int j = 0; j < DIR_SIZE; j++)
//     //     {
//     //         kprintf("Line  191\n");
//     //         if (i * DIR_SIZE + j >= (filesetup.root_dir_inode.size / KTFS_DENSZ))
//     //         {
//     //             kprintf("Line  194\n");
//     //             filesetup.inodecount = i * DIR_SIZE + j;
//     //             return 0;
//     //         }
//     //         filesetup.inode_bitmap[dir[j].inode] = 1; // mark inode as used
//     //     }

//     // }
//     // for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++)
//     // {
//     //     cache_get_block(filesetup.cptr, (filesetup.root_dir_inode.block[i] + global_datablock_0) * KTFS_BLKSZ, (void **)&data);
//     //     memcpy(dir, data, sizeof(dir));
//     //     cache_release_block(filesetup.cptr, data, CACHE_CLEAN);
//     //     for (int j = 0; j < DIR_SIZE; j++)
//     //     {
//     //         if (dir[j].name != NULL)
//     //         {
//     //             filesetup.inode_bitmap[dir[j].inode] = 1;
//     //         }
//     //     }
//     // }
//     return 0;
// }

// What are we supposed to be doing in mount?
int ktfs_mount(struct io *io) // done??
{
    // Init cache
    create_cache(io, &filesetup.cptr);
    // lock_init(&filesetup.filesetup_lock);

    // Save reference to disk I/O endpoint
    filesetup.diskio = ioaddref(io);
    filesetup.cur_file_count = 0;

    // populates superblock
    // filesetup.super_blk = kcalloc(1, sizeof(struct ktfs_superblock));
    cache_get_block(filesetup.cptr, 0, (void **)&data);
    memcpy(&filesetup.super_blk, data, sizeof(struct ktfs_superblock));
    cache_release_block(filesetup.cptr, data, CACHE_CLEAN);

    unsigned long long root_idx = filesetup.super_blk.root_directory_inode;
    unsigned long long block_idx = root_idx / INODES_PER_BLK;
    unsigned long long inode_idx = root_idx % INODES_PER_BLK;

    unsigned long long global_block_idx = 1 + filesetup.super_blk.bitmap_block_count + block_idx;

    // Populate root directory inode
    // filesetup.root_dir_inode = kcalloc(1, sizeof(struct ktfs_inode));

    // Weird Why does this work? We get block so it will give the whole block but I give an unalligned pos if inode_idx is not zero
    // crazy
    cache_get_block(filesetup.cptr, global_block_idx * KTFS_BLKSZ, (void **)&data);
    memcpy(&filesetup.root_dir_inode, (data + (inode_idx * KTFS_INOSZ)), sizeof(struct ktfs_inode));
    cache_release_block(filesetup.cptr, data, CACHE_CLEAN);

    // filesetup.bitmap_blocks = kmalloc(sizeof(struct bitmap_block) * filesetup.super_blk.bitmap_block_count);
    // for (uint32_t i = 0; i < filesetup.super_blk.bitmap_block_count; i++)
    // {
    //     cache_get_block(filesetup.cptr, (1 + i), (void **)&data);
    //     memcpy(&filesetup.bitmap_blocks[i], data, sizeof(struct bitmap_block));
    //     cache_release_block(filesetup.cptr, data, CACHE_CLEAN);
    // }
    filesetup.inode_bitmap = kmalloc(BYTE_SIZE * INODES_PER_BLK * filesetup.super_blk.inode_block_count);
    memset(filesetup.inode_bitmap, 0, BYTE_SIZE * INODES_PER_BLK * filesetup.super_blk.inode_block_count);
    filesetup.inode_bitmap[filesetup.super_blk.root_directory_inode] = 1;

    struct ktfs_dir_entry dir[DIR_SIZE];
    unsigned long long global_datablock_0 = 1 + filesetup.super_blk.bitmap_block_count + filesetup.super_blk.inode_block_count;

    // for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++) {
    //     if (filesetup.root_dir_inode.block[i] == 0) continue; // Skip unused blocks

    //     cache_get_block(filesetup.cptr, (filesetup.root_dir_inode.block[i] + global_datablock_0) * KTFS_BLKSZ, (void **)&data);
    //     memcpy(dir, data, sizeof(dir));
    //     cache_release_block(filesetup.cptr, data, CACHE_CLEAN);

    //     for (int j = 0; j < DIR_SIZE; j++) {
    //         if (dir[j].name[0] != '\0' && dir[j].name != NULL) {
    //             filesetup.inode_bitmap[dir[j].inode] = 1; // mark inode as used
    //         }
    //     }
    // }

    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++)
    {
        cache_get_block(filesetup.cptr, (filesetup.root_dir_inode.block[i] + global_datablock_0) * KTFS_BLKSZ, (void **)&data);
        memcpy(dir, data, sizeof(dir));
        cache_release_block(filesetup.cptr, data, CACHE_CLEAN);

        for (int j = 0; j < DIR_SIZE; j++)
        {
            if (i * DIR_SIZE + j >= (filesetup.root_dir_inode.size / KTFS_DENSZ))
            {
                filesetup.inodecount = i * DIR_SIZE + j;
                return 0;
            }
            filesetup.inode_bitmap[dir[j].inode] = 1; // mark inode as used
        }
    }

    // for (int i = 1; i < (filesetup.root_dir_inode.size / sizeof(struct ktfs_dir_entry)) + 1; i++)
    // {
    //     filesetup.inode_bitmap[i] = 1;
    // }

    // struct ktfs_dir_entry dir[DIR_SIZE];
    // unsigned long long global_datablock_0 = 1 + filesetup.super_blk.bitmap_block_count + filesetup.super_blk.inode_block_count;

    // for (int i = 1; i < (filesetup.root_dir_inode.size / sizeof(struct ktfs_dir_entry)) + 1; i++)
    // {
    //     filesetup.inode_bitmap[i] = 1;
    // }
    return 0;
}

// Open a file by name.
// Modify the provided pointer to contain the io function pointer to interface with the requested file (you must do this!).
// This I/O should be associated with a specific ktfs_file and marked as in-use. The user program will ioptr to interact with the file construct.
// If a file marked as in-use is opened again, you should return an error that indicates that the file is already in-use.
// Returns 0 on success, negative values on error.
// Parameters
// name	The name of the file to be opened.
// ioptr	This is a double pointer to the io object that will be modified to point to the file io object that you create.
// Returns int that indicates success or failure.

int ktfs_open(const char *name, struct io **ioptr)
{
    trace("%s()", __func__);
    struct ktfs_file *fio; // for open file
    struct ktfs_dir_entry dir[DIR_SIZE];

    if (name == NULL || *name == '\0')
        return -ENOENT; // file not found

    //  TODO: We need to make sure to loop through all of the possible entries
    //  That could be in use and check if it is the file we want to open and also get the related inode

    // find the num of inodes per block, inode_index / num_inodes = block of inodes we want
    // inode_index % num_inodes is the inode we want within the block.
    // size of inode = 32 byte
    // size of blk = 512 byte
    // 16 inode per block
    // to find specific block that we're looking for is inode_idx / 16 (inodes per block) = block index
    // to find specific inode index that we're looking for is inode_idx % 16 (inodes per block) = index in block

    // dentry size * inodes in use / dentry size = inodes in use / inodes per blk = num of blks that are in use
    unsigned long long global_datablock_0 = 1 + filesetup.super_blk.bitmap_block_count + filesetup.super_blk.inode_block_count;

    // lock_acquire(&filesetup.filesetup_lock);
    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++)
    {
        // cache_get_block();
        cache_get_block(filesetup.cptr, (filesetup.root_dir_inode.block[i] + global_datablock_0) * KTFS_BLKSZ, (void **)&data);
        memcpy(dir, data, sizeof(dir));
        cache_release_block(filesetup.cptr, data, CACHE_CLEAN);

        for (int num_dir = 0; num_dir < DIR_SIZE; num_dir++)
        {
            if (strncmp(name, dir[num_dir].name, FILENAME_SIZE) == 0)
            {
                // kprintf("\nFile Name:%s\n", dir[num_dir].name);
                for (int name_cnt = 0; name_cnt < filesetup.cur_file_count; name_cnt++)
                {
                    if (strncmp(filesetup.open_file_names[name_cnt].name, dir[num_dir].name, FILENAME_SIZE) == 0)
                    {
                        // lock_release(&filesetup.filesetup_lock);
                        return -EBUSY;
                    }
                }

                fio = kmalloc(sizeof(struct ktfs_file));
                fio->dentry = kmalloc(sizeof(struct ktfs_dir_entry));
                fio->file_inode = kmalloc(sizeof(struct ktfs_inode));

                // fio->fileio = kmalloc(sizeof(struct io));
                *(fio->dentry) = dir[num_dir];
                strncpy(filesetup.open_file_names[filesetup.cur_file_count].name, dir[num_dir].name, sizeof(struct file_name));

                // does it give index of inode or index of block or something?
                unsigned long long block_idx = dir[num_dir].inode / INODES_PER_BLK;
                unsigned long long inode_idx = dir[num_dir].inode % INODES_PER_BLK;
                unsigned long long global_block_idx = 1 + filesetup.super_blk.bitmap_block_count + block_idx;

                cache_get_block(filesetup.cptr, global_block_idx * KTFS_BLKSZ,
                                (void **)(&data));
                memcpy(fio->file_inode, data + (inode_idx * KTFS_INOSZ), sizeof(struct ktfs_inode));
                cache_release_block(filesetup.cptr, data, CACHE_CLEAN);

                // fio->pos = 0;
                // fio->end = fio->file_inode->size;
                fio->open = 1;
                filesetup.open_file_names[filesetup.cur_file_count].fio = &fio->fileio;
                fio->name_pos = filesetup.cur_file_count;
                filesetup.cur_file_count++;
                struct io *io = ioinit1(&fio->fileio, &ktfs_file_iointf);
                *ioptr = create_seekable_io(io); // how do we use
                // lock_release(&filesetup.filesetup_lock);
                return 0;
            }
        }
    }
    // lock_release(&filesetup.filesetup_lock);
    return -ENOENT;
}

void ktfs_close(struct io *io)
{
    trace("%s()", __func__);
    struct ktfs_file *fio = (struct ktfs_file *)((void *)io - offsetof(struct ktfs_file, fileio));
    // for (int i = 0; i < filesetup.cur_file_count; i++)
    // {
    //     // kprintf("Before Close Files: %s\n", filesetup.open_file_names[i].name);
    // }
    char tmp_name[KTFS_MAX_FILENAME_LEN + sizeof(uint8_t)];
    strncpy(tmp_name, filesetup.open_file_names[filesetup.cur_file_count - 1].name, sizeof(struct file_name));

    strncpy(filesetup.open_file_names[filesetup.cur_file_count - 1].name,
            filesetup.open_file_names[fio->name_pos].name, sizeof(struct file_name));

    strncpy(filesetup.open_file_names[fio->name_pos].name, tmp_name, sizeof(struct file_name));
    filesetup.cur_file_count--; // problem rn

    fio->open = 0;

    return;
}

// Read len bytes starting at pos from the file associated with io.
// You should understand the filesystem well before writing this function, even though it seems simple.
// Since this task is (1) not simple and (2) somewhat similar to _writeat (which you will write in the future), we recommend introducing hierarchy (writing helper functions as appropriate).
// Parameters
// io	This is the io object of the file that you want to read from.
// pos	This is the position in the file that you want to start reading from.
// buf	This is the buffer that will be filled with the data read from the file.
// len	This is the number of bytes to read from the file.
// Returns
// long that indicates the number of bytes read or a negative value if there's an error.

long ktfs_readat(struct io *io, unsigned long long pos, void *buf, long len)
{
    trace("%s()", __func__);
    if (buf == NULL || io == NULL)
    {
        return -EINVAL;
    }

    // struct ktfs_file *fio = (struct ktfs_file *)(void *)io - offsetof(struct ktfs_file, fileio); // gives us the fio for the open file
    struct ktfs_file *fio = (struct ktfs_file *)((void *)io - offsetof(struct ktfs_file, fileio));
    // kprintf("\nFile Size: %d", fio->file_inode->size);
    if (fio->open == 0)
    {
        return -EIO;
    }
    long bytesread = 0;

    // boundary checking
    if (len < 0)
        return -EINVAL;
    if (len == 0)
        return 0;

    if (fio->file_inode->size == 0)
        return 0;

    // initiating curr and end position
    unsigned long long curr = pos;

    // struct ktfs_inode *inode = fio->file_inode;
    unsigned long long file_size = fio->file_inode->size;

    if (pos >= file_size)
        return -EINVAL;

    if ((pos + len) > file_size)
    { // do a short read
        len = file_size - pos;
    }

    unsigned long long end = pos + len;
    // kprintf("\nEND:%d", end);
    // lock_acquire(&filesetup.filesetup_lock);
    while (curr < end)
    {
        unsigned long long block_idx = curr / KTFS_BLKSZ; // finds the current block index (disregards direct, indirect) we need to access
        unsigned long long block_offset = curr % KTFS_BLKSZ;
        int blknum = blocknum(fio, block_idx); // gets the physical blk num
        if (blknum == -1)
        {
            // lock_release(&filesetup.filesetup_lock);
            return -ENODATABLKS;
        }

        int bytesleft = KTFS_BLKSZ - block_offset; // number of bytes remaining in the current block

        if (end <= (curr + bytesleft))
        { // last block
            bytesleft = end - curr;
        }
        // kprintf("\nBytes Left:%d", bytesleft);
        // char *data;

        cache_get_block(filesetup.cptr, blknum * KTFS_BLKSZ, (void **)&data); // loads the data from the block into data address
        memcpy(buf + bytesread, data + block_offset, bytesleft);
        cache_release_block(filesetup.cptr, data, CACHE_CLEAN);

        bytesread += bytesleft; // finished reading the current block
        // kprintf("\nbytesread:%d", bytesread);
        curr += bytesleft; // signals that we finished this block so we update curr position to start of new block
        // kprintf("\nCurr:%d", curr);
    }

    // fio->pos = pos + bytesread; // updates file position after read to pos + len
    //  lock_release(&filesetup.filesetup_lock);
    return bytesread;
}

// returns the physical block num (using the logical index from dividing currpos by blksz)
int blocknum(struct ktfs_file *fio, unsigned long long idx)
{
    // use the blkindex we calculated in the readat to find the actual block (direct, indirect, dindirect)
    struct ktfs_inode *inode = fio->file_inode; // gets the inode of the file
    struct indirBlock *indirectdata;
    // void *dindirectdata;
    int blknum;

    // Global offset calculation
    unsigned long long global_datablock_0 = 1 + filesetup.super_blk.bitmap_block_count + filesetup.super_blk.inode_block_count;

    if (idx < KTFS_NUM_DIRECT_DATA_BLOCKS)
    { // checks if the index is in the direct block
        return inode->block[idx] + global_datablock_0;
    }
    else if (idx < (KTFS_BLKS_PER_INDIRECT + KTFS_NUM_DIRECT_DATA_BLOCKS))
    {                                                                                         // indirect block
        unsigned long long indirectpos = (inode->indirect + global_datablock_0) * KTFS_BLKSZ; // gbl offset
        // unsigned long long indirect_idx = idx - KTFS_NUM_DIRECT_DATA_BLOCKS;

        cache_get_block(filesetup.cptr, indirectpos, (void **)&indirectdata);
        uint32_t *indirect_blocks = (uint32_t *)indirectdata;
        blknum = indirect_blocks[idx - KTFS_NUM_DIRECT_DATA_BLOCKS];
        // kprintf("\nblocknum\n%d\n",blknum);
        //  Release the block
        cache_release_block(filesetup.cptr, indirectdata, CACHE_CLEAN);

        return blknum + global_datablock_0;
    }
    else if (idx >= KTFS_NUM_DIRECT_DATA_BLOCKS + KTFS_BLKS_PER_INDIRECT)
    {
        // Adjust for both direct and indirect blocks
        unsigned long long newidx = idx - KTFS_NUM_DIRECT_DATA_BLOCKS - KTFS_BLKS_PER_INDIRECT;
        int dindirect_block_idx;

        // Calculate which dindirect block to use (0 or 1)
        if (newidx < KTFS_BLKS_PER_DINDIRECT)
        {
            dindirect_block_idx = 0;
        }

        if (newidx >= KTFS_BLKS_PER_DINDIRECT)
        {
            dindirect_block_idx = 1;
            newidx -= KTFS_BLKS_PER_DINDIRECT;
        }

        // Calculate indirect block index and offset within that indirect block
        int indirectidx = newidx / KTFS_BLKS_PER_INDIRECT;
        int indirectoffset = newidx % KTFS_BLKS_PER_INDIRECT;

        // Get the doubly indirect block
        unsigned long long dindirectblkpos = (inode->dindirect[dindirect_block_idx] + global_datablock_0) * KTFS_BLKSZ;

        void *dindirectdata;
        cache_get_block(filesetup.cptr, dindirectblkpos, (void **)&dindirectdata);

        uint32_t *dindirect_blocks = (uint32_t *)dindirectdata;
        uint32_t indirectnum = dindirect_blocks[indirectidx];

        // Release the doubly indirect block
        cache_release_block(filesetup.cptr, dindirectdata, CACHE_CLEAN);

        // Get the indirect block
        unsigned long long indirectpos = (indirectnum + global_datablock_0) * KTFS_BLKSZ;

        char *indirect_data;
        cache_get_block(filesetup.cptr, indirectpos, (void **)&indirect_data);

        uint32_t *indirect_blocks = (uint32_t *)indirect_data;
        blknum = indirect_blocks[indirectoffset];

        cache_release_block(filesetup.cptr, indirect_data, CACHE_CLEAN);

        return blknum + global_datablock_0;
    }

    return -1; // error
}

// Do an special / ioctl function on the filesystem.
// The action to take is determined by the value of cmd.
// Note that different commands have different ways to output values. For example, getting the block size will return the block size directly if successful, but IOCTL_GETEND will return the size of the file in the arg pointer.
// Unsupported cmd values should return -ENOTSUP.
// The function should return 0 on success.

// Parameters
// io	This is the io object of the file that you want to perform the control functifioon.
// cmd	This is the command that you want to execute.
// arg	This is the argument that you want to pass in, maybe different for different control functions.
// Returns
// int that indicates success or failure, or something else.

int ktfs_cntl(struct io *io, int cmd, void *arg)
{
    trace("%s()", __func__);
    struct ktfs_file *fio = (struct ktfs_file *)((void *)io - offsetof(struct ktfs_file, fileio));

    unsigned long long *ullarg = arg;

    switch (cmd)
    {
    case IOCTL_GETBLKSZ:
        return 1;

    case IOCTL_GETEND:
        // kprintf("\n%p\n",fio->file_inode);
        if (ullarg == NULL)
        {
            return -EINVAL;
        }
        *ullarg = fio->file_inode->size;
        return 0;

    case IOCTL_SETEND:
        // kprintf()
        //  need to allocate more inode if reach past end
        if (ullarg == NULL)
        {
            return -EINVAL;
        }
        if (*ullarg == fio->file_inode->size)
        {
            return 0;
        }
        else if (*ullarg > fio->file_inode->size)
        {
            size_t end = *ullarg;
            // if ((end / KTFS_BLKSZ) == (fio->file_inode->size / KTFS_BLKSZ))
            // {
            //     fio->file_inode->size = end;
            //     unsigned long long block_idx = fio->dentry->inode / INODES_PER_BLK;
            //     unsigned long long inode_idx = fio->dentry->inode % INODES_PER_BLK;
            //     unsigned long long global_block_idx = 1 + filesetup.super_blk.bitmap_block_count + block_idx;
            //     cache_get_block(filesetup.cptr, global_block_idx * KTFS_BLKSZ,
            //                     (void **)(&data));
            //     memcpy(data + (inode_idx * KTFS_INOSZ), fio->file_inode, sizeof(struct ktfs_inode));
            //     cache_release_block(filesetup.cptr, data, CACHE_DIRTY);
            //     return 0;
            // }
            // fio->file_inode->size = (((fio->file_inode->size) + (KTFS_BLKSZ)-1) / (KTFS_BLKSZ) * (KTFS_BLKSZ));
            while (fio->file_inode->size < end)
            {
                // handle case when end is still larger than size but on the same block so no need to allocate
                fio->file_inode->size = (((fio->file_inode->size) + (KTFS_BLKSZ)-1) / (KTFS_BLKSZ) * (KTFS_BLKSZ));
                if (end < fio->file_inode->size) //(end / KTFS_BLKSZ) == (fio->file_inode->size / KTFS_BLKSZ)
                {
                    fio->file_inode->size = end;
                    unsigned long long block_idx = fio->dentry->inode / INODES_PER_BLK;
                    unsigned long long inode_idx = fio->dentry->inode % INODES_PER_BLK;
                    unsigned long long global_block_idx = 1 + filesetup.super_blk.bitmap_block_count + block_idx;
                    cache_get_block(filesetup.cptr, global_block_idx * KTFS_BLKSZ,
                                    (void **)(&data));
                    memcpy(data + (inode_idx * KTFS_INOSZ), fio->file_inode, sizeof(struct ktfs_inode));
                    cache_release_block(filesetup.cptr, data, CACHE_DIRTY);
                    return 0;
                }

                // call helper to set correct block in inode to newly allocated block.
                int ret;
                if (fio->file_inode->size == 0)
                {
                    unsigned long long alloc_block = allocate_open_block();
                    if (alloc_block == -ENODATABLKS)
                    {
                        return -ENODATABLKS;
                    }
                    fio->file_inode->block[0] = alloc_block;
                    write_inode_to_disk(fio);
                }
                else
                {
                    ret = add_new_inode_datablk(fio, ((fio->file_inode->size - 1) / KTFS_BLKSZ));
                }
                if (ret == -ENODATABLKS)
                {
                    return -ENODATABLKS;
                }
                fio->file_inode->size = ((fio->file_inode->size / KTFS_BLKSZ) + 1) * KTFS_BLKSZ;
            }
            if (fio->file_inode->size >= end)
            {
                fio->file_inode->size = end;
                unsigned long long block_idx = fio->dentry->inode / INODES_PER_BLK;
                unsigned long long inode_idx = fio->dentry->inode % INODES_PER_BLK;
                unsigned long long global_block_idx = 1 + filesetup.super_blk.bitmap_block_count + block_idx;
                cache_get_block(filesetup.cptr, global_block_idx * KTFS_BLKSZ,
                                (void **)(&data));
                memcpy(data + (inode_idx * KTFS_INOSZ), fio->file_inode, sizeof(struct ktfs_inode));
                cache_release_block(filesetup.cptr, data, CACHE_DIRTY);
                return 0;
            }
            return -EINVAL;
        }
        else
        {
            return -EINVAL;
        }

    default:
        return -ENOTSUP;
    }
}

// Helper function to find which block you need to set based on the size and set that to newly allocated block_num
int add_new_inode_datablk(struct ktfs_file *fio, unsigned long long old_idx)
{
    // use the blkindex we calculated in the readat to find the actual block (direct, indirect, dindirect)
    struct ktfs_inode *inode = fio->file_inode; // gets the inode of the file
    // struct indirBlock *indirectdata;
    //  void *dindirectdata;
    // int blknum;

    // Handle the case where you need to free alrdy allocated data blocks if error occurs!! ASK OH?
    unsigned long long new_idx = old_idx + 1;

    // Global offset calculation
    unsigned long long global_datablock_0 = 1 + filesetup.super_blk.bitmap_block_count + filesetup.super_blk.inode_block_count;

    if (new_idx < KTFS_NUM_DIRECT_DATA_BLOCKS)
    { // checks if the index is in the direct block

        unsigned long long alloc_block = allocate_open_block();
        if (alloc_block == -ENODATABLKS)
        {
            return -ENODATABLKS;
        }
        inode->block[new_idx] = alloc_block;
        write_inode_to_disk(fio);

        // cache_get_block(filesetup.cptr, (inode->block[new_idx] + global_datablock_0) * KTFS_BLKSZ, (void **)&data);
        // uint32_t *direct_blocks = (uint32_t *)data;
        // direct_blocks[new_idx] = alloc_block;
        // inode->block[new_idx] = alloc_block;
        // cache_release_block(filesetup.cptr, data, CACHE_DIRTY);
        return 1;
    }
    else if (new_idx < (KTFS_BLKS_PER_INDIRECT + KTFS_NUM_DIRECT_DATA_BLOCKS))
    { // indirect block
        if (old_idx < KTFS_NUM_DIRECT_DATA_BLOCKS)
        {
            unsigned long long alloc_block = allocate_open_block();
            if (alloc_block == -ENODATABLKS)
            {
                return -ENODATABLKS;
            }
            inode->indirect = alloc_block;
            // Save back to disk
            write_inode_to_disk(fio);
        }

        unsigned long long indirectpos = (inode->indirect + global_datablock_0) * KTFS_BLKSZ; // gbl offset
        unsigned long long alloc_block = allocate_open_block();
        if (alloc_block == -ENODATABLKS)
        {
            return -ENODATABLKS;
        }
        cache_get_block(filesetup.cptr, indirectpos, (void **)&data);
        uint32_t *indirect_blocks = (uint32_t *)data;
        indirect_blocks[new_idx - KTFS_NUM_DIRECT_DATA_BLOCKS] = alloc_block;
        cache_release_block(filesetup.cptr, data, CACHE_DIRTY);

        return 1;
    }
    else if (new_idx >= KTFS_NUM_DIRECT_DATA_BLOCKS + KTFS_BLKS_PER_INDIRECT)
    {
        // Adjust for both direct and indirect blocks
        // unsigned long long old_zero_offset = old_idx - KTFS_NUM_DIRECT_DATA_BLOCKS - KTFS_BLKS_PER_INDIRECT;
        unsigned long long new_zero_offset = new_idx - KTFS_NUM_DIRECT_DATA_BLOCKS - KTFS_BLKS_PER_INDIRECT;
        if (new_zero_offset == 0)
        {
            unsigned long long alloc_block = allocate_open_block();
            if (alloc_block == -ENODATABLKS)
            {
                return -ENODATABLKS;
            }
            inode->dindirect[0] = alloc_block;
            // Save back to disk
            write_inode_to_disk(fio);
        }
        else if (new_zero_offset == KTFS_BLKS_PER_DINDIRECT)
        {
            unsigned long long alloc_block = allocate_open_block();
            if (alloc_block == -ENODATABLKS)
            {
                return -ENODATABLKS;
            }
            inode->dindirect[1] = alloc_block;
            // Save back to disk
            write_inode_to_disk(fio);
        }
        int dind_blk_idx;
        if (new_zero_offset < KTFS_BLKS_PER_DINDIRECT)
        {
            dind_blk_idx = 0;
        }
        else
        {
            dind_blk_idx = 1;
            new_zero_offset -= KTFS_BLKS_PER_DINDIRECT;
        }
        int indirectidx = new_zero_offset / KTFS_BLKS_PER_INDIRECT;
        int indirectoffset = new_zero_offset % KTFS_BLKS_PER_INDIRECT;

        if (indirectoffset == 0)
        {
            unsigned long long alloc_block = allocate_open_block();
            if (alloc_block == -ENODATABLKS)
            {
                return -ENODATABLKS;
            }
            cache_get_block(filesetup.cptr, (inode->dindirect[dind_blk_idx] + global_datablock_0) * KTFS_BLKSZ, (void **)&data);
            uint32_t *dindirect_block = (uint32_t *)data;
            dindirect_block[indirectidx] = alloc_block;
            cache_release_block(filesetup.cptr, data, CACHE_DIRTY);
        }
        // Get index to inder block in dinder block
        cache_get_block(filesetup.cptr, (inode->dindirect[dind_blk_idx] + global_datablock_0) * KTFS_BLKSZ, (void **)&data);
        uint32_t *dindirect_blocks = (uint32_t *)data;
        uint32_t indirectnum = dindirect_blocks[indirectidx];
        cache_release_block(filesetup.cptr, data, CACHE_CLEAN);

        // Find exact inder pos
        unsigned long long indirectpos = (indirectnum + global_datablock_0) * KTFS_BLKSZ;
        unsigned long long alloc_block = allocate_open_block();
        if (alloc_block == -ENODATABLKS)
        {
            return -ENODATABLKS;
        }

        // set entry to be new block
        cache_get_block(filesetup.cptr, indirectpos, (void **)&data);
        uint32_t *indirect_blocks = (uint32_t *)data;
        indirect_blocks[indirectoffset] = alloc_block;
        cache_release_block(filesetup.cptr, data, CACHE_DIRTY);
        return 1;
    }

    return -1; // error
}

void write_inode_to_disk(struct ktfs_file *fio)
{
    // write inode back to disk
    unsigned long long block_idx = fio->dentry->inode / INODES_PER_BLK;
    unsigned long long inode_idx = fio->dentry->inode % INODES_PER_BLK;
    unsigned long long global_block_idx = 1 + filesetup.super_blk.bitmap_block_count + block_idx;
    cache_get_block(filesetup.cptr, global_block_idx * KTFS_BLKSZ,
                    (void **)(&data));
    memcpy(data + (inode_idx * KTFS_INOSZ), fio->file_inode, sizeof(struct ktfs_inode));
    cache_release_block(filesetup.cptr, data, CACHE_DIRTY);
}

// Flush the cache to the backing device.
// Returns 0 if flush successful, negative values if there's an error.

int ktfs_flush(void)
{
    // struct ktfs_file *fio;
    if (filesetup.cptr == NULL)
    {
        return -EINVAL;
    }
    cache_flush((filesetup.cptr));
    return 0;
}

long ktfs_writeat(struct io *io, unsigned long long pos, const void *buf, long len)
{
    trace("%s()", __func__);

    if (buf == NULL || io == NULL)
    {
        return -EINVAL;
    }
    // struct ktfs_file *fio = (struct ktfs_file *)(void *)io - offsetof(struct ktfs_file, fileio); // gives us the fio for the open file
    struct ktfs_file *fio = (struct ktfs_file *)((void *)io - offsetof(struct ktfs_file, fileio));
    if (fio->open == 0)
    {
        return -EIO;
    }
    long byteswritten = 0;

    // boundary checking
    if (len < 0)
        return -EINVAL;
    if (len == 0)
        return 0;

    // initiating curr and end position
    unsigned long long curr = pos;

    // struct ktfs_inode *inode = fio->file_inode;
    unsigned long long file_size = fio->file_inode->size;

    if (pos >= file_size)
        return -EINVAL;

    if ((pos + len) > file_size)
    {
        len = file_size - pos;
    }

    unsigned long long end = pos + len;

    while (curr < end)
    {
        unsigned long long block_idx = curr / KTFS_BLKSZ;    // Logical block index
        unsigned long long block_offset = curr % KTFS_BLKSZ; // Offset within block

        // get physical blk
        int blknum = blocknum(fio, block_idx);
        if (blknum == -1)
        {
            return -ENODATABLKS;
        }

        int bytesleft = KTFS_BLKSZ - block_offset; // Max bytes we can write to this block

        if (end <= (curr + bytesleft))
        { // Last block
            bytesleft = end - curr;
        }

        // char *data;

        cache_get_block(filesetup.cptr, blknum * KTFS_BLKSZ, (void **)&data);
        // kprintf("\nDATA PTR:%p", data);
        // kprintf("\nStart Write: %p End Write: %p", (data + block_offset), (data + byteswritten + bytesleft));
        memcpy(data + block_offset, (char *)buf + byteswritten, bytesleft);
        cache_release_block(filesetup.cptr, data, CACHE_DIRTY);

        byteswritten += bytesleft;
        curr += bytesleft;
    }

    // fio->pos = pos + byteswritten; // Update file position
    return byteswritten;
}

// modify create to work with swaps: Done
int ktfs_create(const char *name)
{
    trace("%s()", __func__);
    if (name == NULL || strlen(name) > KTFS_MAX_FILENAME_LEN || strlen(name) == 0)
    {
        return -EINVAL;
    }

    struct ktfs_dir_entry dir[DIR_SIZE];
    unsigned long long global_datablock_0 = 1 + filesetup.super_blk.bitmap_block_count + filesetup.super_blk.inode_block_count;
    for (int blk_idx = 0; blk_idx < KTFS_NUM_DIRECT_DATA_BLOCKS; blk_idx++)
    {
        cache_get_block(filesetup.cptr, (filesetup.root_dir_inode.block[blk_idx] + global_datablock_0) * KTFS_BLKSZ, (void **)&data);
        memcpy(dir, data, sizeof(dir));
        cache_release_block(filesetup.cptr, data, CACHE_CLEAN);

        for (int dir_idx = 0; dir_idx < DIR_SIZE; dir_idx++)
        {
            if (strncmp(name, dir[dir_idx].name, FILENAME_SIZE) == 0)
            {
                return -EBUSY;
            }
        }
    }

    uint64_t num_files = (filesetup.root_dir_inode.size) / sizeof(struct ktfs_dir_entry);
    // kprintf("\nNum Files:%d\n", num_files);
    if ((num_files < MAX_FILES) && (filesetup.inodecount < INODES_PER_BLK * filesetup.super_blk.inode_block_count))
    {
        uint64_t blk_idx = num_files / DIR_SIZE;
        uint64_t dir_idx = num_files % DIR_SIZE;

        if ((num_files % DIR_SIZE == 0))
        {
            unsigned long long new_block = allocate_open_block();
            if (new_block == -ENODATABLKS)
            {
                return -ENODATABLKS;
            }
            // kprintf("\nNew Block Indesx: %d\n", new_block);
            filesetup.root_dir_inode.block[blk_idx] = new_block;
            unsigned long long block_idx = filesetup.super_blk.root_directory_inode / INODES_PER_BLK;
            unsigned long long inode_idx = filesetup.super_blk.root_directory_inode % INODES_PER_BLK;
            unsigned long long global_block_idx = 1 + filesetup.super_blk.bitmap_block_count + block_idx;

            cache_get_block(filesetup.cptr, global_block_idx * KTFS_BLKSZ, (void **)&data);
            memcpy(data + (inode_idx * KTFS_INOSZ), &filesetup.root_dir_inode, sizeof(struct ktfs_inode));
            cache_release_block(filesetup.cptr, data, CACHE_DIRTY);
        }

        unsigned long long dir_block_addr = (filesetup.root_dir_inode.block[blk_idx] + global_datablock_0) * KTFS_BLKSZ;
        // kprintf("Getting directory block at address: 0x%llx\n", dir_block_addr); // THE ISSUE

        cache_get_block(filesetup.cptr, dir_block_addr, (void **)&data);
        struct ktfs_dir_entry dentry;
        unsigned long long inodeidx = 0;
        // If we are not able to assume the inodes are consecutive do we have to make a bitmap
        // kprintf("Searching for free inode\n");
        for (int i = 0; i < (filesetup.super_blk.inode_block_count * INODES_PER_BLK); i++)
        {
            // kprintf("Inode %d is %s\n", i, filesetup.inode_bitmap[i] ? "used" : "free");
            if (filesetup.inode_bitmap[i] == 0)
            {
                filesetup.inode_bitmap[i] = 1;
                inodeidx = i;
                // kprintf("Found free inode at index %d\n", inodeidx);
                break;
            }
        }
        // kprintf("\nInode Index:%d\n", inodeidx);
        dentry.inode = inodeidx; // should we keep this here or make some variable for inode count
        strncpy(dentry.name, name, sizeof(struct file_name));
        // memcpy((data + (dir_idx * sizeof(struct ktfs_dir_entry))), &dentry, sizeof(struct ktfs_dir_entry));
        // cache_release_block(filesetup.cptr, data, CACHE_DIRTY); // the line that fucks it - but its just releasing it
        // unsigned long long dentry_addr = (unsigned long long)(data + (dir_idx * sizeof(struct ktfs_dir_entry)));
        // kprintf("writing directory entry at addr: 0x%llx (dir_idx: %llu)\n", dentry_addr, dir_idx);
        // kprintf("directory entry will contain inode: %d, name: %s\n", dentry.inode, dentry.name);
        memcpy((data + (dir_idx * sizeof(struct ktfs_dir_entry))), &dentry, sizeof(struct ktfs_dir_entry));

        // struct ktfs_dir_entry *written_entry = (struct ktfs_dir_entry *)(data + (dir_idx * sizeof(struct ktfs_dir_entry)));
        // kprintf("verified written entry: inode=%d, name=%s\n", written_entry->inode, written_entry->name);

        cache_release_block(filesetup.cptr, data, CACHE_DIRTY);
        // Update the size (number of inodes in root dir inode)
        unsigned long long block_idx = filesetup.super_blk.root_directory_inode / INODES_PER_BLK;
        unsigned long long inode_idx = filesetup.super_blk.root_directory_inode % INODES_PER_BLK;
        unsigned long long global_block_idx = 1 + filesetup.super_blk.bitmap_block_count + block_idx;

        cache_get_block(filesetup.cptr, global_block_idx * KTFS_BLKSZ, (void **)&data);
        filesetup.root_dir_inode.size += sizeof(dentry);
        memcpy(data + inode_idx, &filesetup.root_dir_inode, sizeof(struct ktfs_inode)); // added * ktfs inode sz
        cache_release_block(filesetup.cptr, data, CACHE_DIRTY);
        filesetup.inodecount++;
        return 0;
    }
    return -EMFILE;
}

// Deletes a file named name from the filesystem.
// This function will be used to delete an existing file from the filesystem.
// It must free all data blocks and the inode associated with the file.
// It must remove the dentry associated with the file from the root directory. Note that dentries must be contiguous (no gaps).
// The changes must immediately persist on the disk. This function should close the file if it is open.
// You may need to update the bitmap.

// Parameters
// name a string that is the name of the file to delete
// Returns
// 0 on success, negative value on error

// free all the data block first
// do not need to mess with the inode bitmap!!!!
// free the inode - swap deletion
// free dentry - swap deletion
// change root directory inode to reflect this information
// update dentry corrosponding to last inode idx to new inode idx
int ktfs_delete(const char *name)
{

    if (name == NULL || sizeof(name) > KTFS_MAX_FILENAME_LEN)
    {
        return -EINVAL;
    }

    struct ktfs_dir_entry dir[DIR_SIZE];
    unsigned long long global_datablock_0 = 1 + filesetup.super_blk.bitmap_block_count + filesetup.super_blk.inode_block_count;
    int flag = 0;
    struct ktfs_dir_entry entry;
    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++)
    {
        cache_get_block(filesetup.cptr, (filesetup.root_dir_inode.block[i] + global_datablock_0) * KTFS_BLKSZ, (void **)&data);
        memcpy(dir, data, sizeof(dir));
        cache_release_block(filesetup.cptr, data, CACHE_CLEAN);

        for (int j = 0; j < DIR_SIZE; j++)
        {
            if (strncmp(name, dir[j].name, FILENAME_SIZE) == 0)
            { // found file
                flag = 1;
                blkidx = i;
                dentryidx = j;
                entry = dir[j]; // we have the directory entry now
                break;
            }
        }
    }
    if (flag == 0)
    {
        return -ENOENT;
    }

    // close the file
    for (int f = 0; f < filesetup.cur_file_count; f++)
    {
        if (strncmp(entry.name, filesetup.open_file_names[f].name, FILENAME_SIZE) == 0)
        {
            // call ktfs close but with what io?
            struct io *fio = filesetup.open_file_names[f].fio;
            ktfs_close(fio);
            // filesetup.open_file_names[f].name == NULL;
            break;
        }
    }

    // found the directory entry
    uint16_t inodeidx = entry.inode;
    unsigned long long inodeblockidx = inodeidx / INODES_PER_BLK;
    unsigned long long inodeoffset = inodeidx % INODES_PER_BLK;
    unsigned long long globalinodeblock = 1 + filesetup.super_blk.bitmap_block_count + inodeblockidx;
    int physical_block;

    struct ktfs_inode inode;
    cache_get_block(filesetup.cptr, globalinodeblock * KTFS_BLKSZ, (void **)&data);
    memcpy(&inode, data + (inodeoffset * KTFS_INOSZ), sizeof(struct ktfs_inode));
    cache_release_block(filesetup.cptr, data, CACHE_CLEAN);

    // now we have the actual inode - need to free data blocks
    int filesizeinblocks = (inode.size + KTFS_BLKSZ - 1) / KTFS_BLKSZ; // find the number of blocks the file uses
    for (unsigned long long i = 0; i < filesizeinblocks && i < filesizeinblocks; i++)
    {
        physical_block = inodeidxblocknum(&inode, i, global_datablock_0);
        free_block(physical_block - global_datablock_0);
    }

    if (filesizeinblocks > KTFS_NUM_DIRECT_DATA_BLOCKS)
    {
        if (inode.indirect != 0)
        {
            free_block(inode.indirect); // free indirect block
        }
    }

    // free all the doubly indirect + indirect blocks associated with dindirect
    if (filesizeinblocks > KTFS_NUM_DIRECT_DATA_BLOCKS + KTFS_BLKS_PER_INDIRECT)
    {
        for (int i = 0; i < KTFS_NUM_DINDIRECT_BLOCKS; i++)
        {
            if (inode.dindirect[i] != 0)
            {
                unsigned long long dindirectpos = (inode.dindirect[i] + global_datablock_0) * KTFS_BLKSZ;

                uint32_t *dindirect_blocks;
                cache_get_block(filesetup.cptr, dindirectpos, (void **)&dindirect_blocks);

                for (int j = 0; j < KTFS_BLKS_PER_INDIRECT; j++)
                {
                    if (dindirect_blocks[j] != 0)
                    {
                        free_block(dindirect_blocks[j]);
                    }
                }

                cache_release_block(filesetup.cptr, dindirect_blocks, CACHE_CLEAN);
                free_block(inode.dindirect[i]);
            }
        }
    }

    // swap delete the dentry
    // unsigned long long num_dentries = filesetup.root_dir_inode.size / sizeof(struct ktfs_dir_entry);
    uint64_t num_files = filesetup.root_dir_inode.size / sizeof(struct ktfs_dir_entry);
    uint64_t blk_idx = (num_files - 1) / DIR_SIZE;
    uint64_t last_blk_idx = (num_files - 1) / DIR_SIZE;
    uint64_t last_entry_idx = (num_files - 1) % DIR_SIZE;
    cache_get_block(filesetup.cptr, (filesetup.root_dir_inode.block[blk_idx] + global_datablock_0) * KTFS_BLKSZ, (void **)&data);
    struct ktfs_dir_entry dentryblk[DIR_SIZE];
    memcpy(dentryblk, data, sizeof(dentryblk));

    struct ktfs_dir_entry last = dentryblk[(num_files - 1) % DIR_SIZE];

    cache_release_block(filesetup.cptr, data, CACHE_CLEAN);

    // copy the last dentry into the spot we want to delete
    cache_get_block(filesetup.cptr, (filesetup.root_dir_inode.block[blkidx] + global_datablock_0) * KTFS_BLKSZ, (void **)&data);
    memcpy(dir, data, sizeof(dir));
    dir[dentryidx] = last;
    memcpy(data, dir, sizeof(dir));
    cache_release_block(filesetup.cptr, data, CACHE_DIRTY);

    if (blkidx != last_blk_idx)
    {
        cache_get_block(filesetup.cptr, (filesetup.root_dir_inode.block[last_blk_idx] + global_datablock_0) * KTFS_BLKSZ, (void **)&data);

        // Clear the last entry
        struct ktfs_dir_entry empty;
        memset(&empty, 0, sizeof(struct ktfs_dir_entry));

        memcpy(data + (last_entry_idx * sizeof(struct ktfs_dir_entry)), &empty, sizeof(struct ktfs_dir_entry));

        cache_release_block(filesetup.cptr, data, CACHE_DIRTY);
    }

    // does "freeing" inode mean when no directory entry points to it anymore?
    filesetup.inode_bitmap[inodeidx] = 0;

    // Memset inode to zero for saftey
    cache_get_block(filesetup.cptr, globalinodeblock * KTFS_BLKSZ,
                    (void **)(&data));
    memset(data + (inodeidx * KTFS_INOSZ), 0, sizeof(struct ktfs_inode));
    cache_release_block(filesetup.cptr, data, CACHE_DIRTY);

    filesetup.root_dir_inode.size -= sizeof(struct ktfs_dir_entry); // update root dir inode size

    // Write updated root directory inode back to disk
    unsigned long long rootidx = filesetup.super_blk.root_directory_inode;
    unsigned long long rootblockidx = rootidx / INODES_PER_BLK;
    unsigned long long rootinodeidx = rootidx % INODES_PER_BLK;

    cache_get_block(filesetup.cptr, (1 + filesetup.super_blk.bitmap_block_count + rootblockidx) * KTFS_BLKSZ, (void **)&data);
    memcpy(data + (rootinodeidx * KTFS_INOSZ), &filesetup.root_dir_inode, sizeof(struct ktfs_inode));
    cache_release_block(filesetup.cptr, data, CACHE_DIRTY);
    ktfs_flush();
    return 0;
}

// returns the physical block num (using the inode information for the file we want to delete)
int inodeidxblocknum(struct ktfs_inode *inode, unsigned long long idx, unsigned long long global_datablock_0)
{
    struct indirBlock *indirectdata;

    int blknum;

    if (idx < KTFS_NUM_DIRECT_DATA_BLOCKS)
    { // checks if the index is in the direct block
        return inode->block[idx] + global_datablock_0;
    }

    else if (idx < (KTFS_BLKS_PER_INDIRECT + KTFS_NUM_DIRECT_DATA_BLOCKS))
    {                                                                                         // indirect block
        unsigned long long indirectpos = (inode->indirect + global_datablock_0) * KTFS_BLKSZ; // gbl offset
        // unsigned long long indirect_idx = idx - KTFS_NUM_DIRECT_DATA_BLOCKS;

        cache_get_block(filesetup.cptr, indirectpos, (void **)&indirectdata);
        uint32_t *indirect_blocks = (uint32_t *)indirectdata;
        blknum = indirect_blocks[idx - KTFS_NUM_DIRECT_DATA_BLOCKS];
        // kprintf("\nblocknum\n%d\n",blknum);
        //  Release the block
        cache_release_block(filesetup.cptr, indirectdata, CACHE_CLEAN);

        return blknum + global_datablock_0;
    }
    else if (idx >= KTFS_NUM_DIRECT_DATA_BLOCKS + KTFS_BLKS_PER_INDIRECT)
    {
        // Adjust for both direct and indirect blocks
        unsigned long long newidx = idx - KTFS_NUM_DIRECT_DATA_BLOCKS - KTFS_BLKS_PER_INDIRECT;
        int dindirect_block_idx;

        // Calculate which dindirect block to use (0 or 1)
        if (newidx < KTFS_BLKS_PER_DINDIRECT)
        {
            dindirect_block_idx = 0;
        }

        if (newidx >= KTFS_BLKS_PER_DINDIRECT)
        {
            dindirect_block_idx = 1;
            newidx -= KTFS_BLKS_PER_DINDIRECT;
        }

        // Calculate indirect block index and offset within that indirect block
        int indirectidx = newidx / KTFS_BLKS_PER_INDIRECT;
        int indirectoffset = newidx % KTFS_BLKS_PER_INDIRECT;

        // Get the doubly indirect block
        unsigned long long dindirectblkpos = (inode->dindirect[dindirect_block_idx] + global_datablock_0) * KTFS_BLKSZ;

        void *dindirectdata;
        cache_get_block(filesetup.cptr, dindirectblkpos, (void **)&dindirectdata);

        uint32_t *dindirect_blocks = (uint32_t *)dindirectdata;
        uint32_t indirectnum = dindirect_blocks[indirectidx];

        // Release the doubly indirect block
        cache_release_block(filesetup.cptr, dindirectdata, CACHE_CLEAN);

        // Get the indirect block
        unsigned long long indirectpos = (indirectnum + global_datablock_0) * KTFS_BLKSZ;

        char *indirect_data;
        cache_get_block(filesetup.cptr, indirectpos, (void **)&indirect_data);

        uint32_t *indirect_blocks = (uint32_t *)indirect_data;
        blknum = indirect_blocks[indirectoffset];

        cache_release_block(filesetup.cptr, indirect_data, CACHE_CLEAN);

        return blknum + global_datablock_0;
    }

    return -1; // error
}

// free blk in bitmap - set to 0 - use to freedatablock
void free_block(int block_num)
{
    unsigned int bitmap_block_idx = 1 + block_num / (KTFS_BLKSZ * BYTE_SIZE);
    unsigned int bit_offset_in_bitmap = block_num % (KTFS_BLKSZ * BYTE_SIZE);

    // Get bitmap block from cache
    char *data;
    cache_get_block(filesetup.cptr, bitmap_block_idx * KTFS_BLKSZ, (void **)&data);

    data[bit_offset_in_bitmap / BYTE_SIZE] &= ~(1 << (bit_offset_in_bitmap % BYTE_SIZE));

    cache_release_block(filesetup.cptr, data, CACHE_DIRTY);
}

unsigned long long allocate_open_block(void)
{
    // kprintf("bitmap blk count: %d", filesetup.super_blk.bitmap_block_count);

    for (int block = 0; block < filesetup.super_blk.bitmap_block_count; block++)
    {
        cache_get_block(filesetup.cptr, (1 + block) * KTFS_BLKSZ, (void **)&data);
        // for (int i = 0; i < KTFS_BLKSZ; i++)
        // {
        //     kprintf("%02x ", (char)(data[i]));
        // }

        // memcpy(&filesetup.bitmap_blocks[i], data, sizeof(struct bitmap_block));
        for (int byte = 0; byte < KTFS_BLKSZ; byte++)
        {
            for (int bit = 7; bit >= 0; bit--)
            {
                if ((data[byte] & (1 << bit)) == 0)
                {
                    data[byte] |= (1 << bit);
                    cache_release_block(filesetup.cptr, data, CACHE_DIRTY);
                    return (block * KTFS_BLKSZ * BYTE_SIZE) + (byte * BYTE_SIZE + bit);
                }
            }
        }
        cache_release_block(filesetup.cptr, data, CACHE_CLEAN);
    }
    return -ENODATABLKS;
}
