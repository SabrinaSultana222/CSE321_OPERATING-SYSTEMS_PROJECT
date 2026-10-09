
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FS_MAGIC 0x56534653U

#define BLOCK_SIZE         4096U
#define INODE_SIZE          128U
#define JOURNAL_BLOCK_IDX     1U
#define JOURNAL_BLOCKS       16U
#define INODE_BLOCKS          2U
#define DATA_BLOCKS          64U
#define INODE_BMAP_IDX      (JOURNAL_BLOCK_IDX + JOURNAL_BLOCKS)
#define DATA_BMAP_IDX       (INODE_BMAP_IDX + 1U)
#define INODE_START_IDX     (DATA_BMAP_IDX + 1U)
#define DATA_START_IDX      (INODE_START_IDX + INODE_BLOCKS)
#define TOTAL_BLOCKS        (DATA_START_IDX + DATA_BLOCKS)
#define DIRECT_POINTERS      8U
#define DEFAULT_IMAGE      "vsfs.img"

#define JOURNAL_MAGIC 0x4A524E4CU  

#define REC_DATA   1U
#define REC_COMMIT 2U

struct superblock {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_count;

    uint32_t journal_block;
    uint32_t inode_bitmap;
    uint32_t data_bitmap;
    uint32_t inode_start;
    uint32_t data_start;

    uint8_t  _pad[128 - 9 * 4];
};

struct inode {
    uint16_t type;   
    uint16_t links;
    uint32_t size;
    uint32_t direct[DIRECT_POINTERS];
    uint32_t ctime;
    uint32_t mtime;
    uint8_t  _pad[128 - (2 + 2 + 4 + DIRECT_POINTERS * 4 + 4 + 4)];
};

struct dirent {
    uint32_t inode;     
    char name[28];      
};

struct journal_header {
    uint32_t magic;
    uint32_t nbytes_used; 
};


struct rec_header {
    uint16_t type; 
    uint16_t size; 
};

_Static_assert(sizeof(struct superblock) == 128, "superblock must be 128 bytes");
_Static_assert(sizeof(struct inode) == 128, "inode must be 128 bytes");
_Static_assert(sizeof(struct dirent) == 32, "dirent must be 32 bytes");
_Static_assert(sizeof(struct journal_header) == 8, "journal_header must be 8 bytes");
_Static_assert(sizeof(struct rec_header) == 4, "rec_header must be 4 bytes");



static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void die_msg(const char *msg) {
    fprintf(stderr, "ERROR: %s\n", msg);
    exit(EXIT_FAILURE);
}

static off_t block_off(uint32_t block_index) {
    return (off_t)block_index * (off_t)BLOCK_SIZE;
}

static void read_block(int fd, uint32_t block_index, uint8_t out[BLOCK_SIZE]) {
    ssize_t n = pread(fd, out, BLOCK_SIZE, block_off(block_index));
    if (n != (ssize_t)BLOCK_SIZE) die("pread(block)");
}

static void write_block(int fd, uint32_t block_index, const uint8_t in[BLOCK_SIZE]) {
    ssize_t n = pwrite(fd, in, BLOCK_SIZE, block_off(block_index));
    if (n != (ssize_t)BLOCK_SIZE) die("pwrite(block)");
}

static int bitmap_test(const uint8_t *bitmap, uint32_t index) {
    return (bitmap[index / 8] >> (index % 8)) & 0x1;
}

static void bitmap_set(uint8_t *bitmap, uint32_t index) {
    bitmap[index / 8] |= (uint8_t)(1U << (index % 8));
}

static int find_free_inode(const uint8_t *inode_bitmap, uint32_t inode_count) {
    for (uint32_t i = 0; i < inode_count; ++i) {
        if (!bitmap_test(inode_bitmap, i)) return (int)i;
    }
    return -1;
}



static off_t journal_base_off(void) {
    return block_off(JOURNAL_BLOCK_IDX);
}

static uint32_t journal_capacity(void) {
    return JOURNAL_BLOCKS * BLOCK_SIZE;
}

static void journal_read_header(int fd, struct journal_header *jh) {
    ssize_t n = pread(fd, jh, sizeof(*jh), journal_base_off());
    if (n != (ssize_t)sizeof(*jh)) die("pread(journal_header)");
}

static void journal_write_header(int fd, const struct journal_header *jh) {
    ssize_t n = pwrite(fd, jh, sizeof(*jh), journal_base_off());
    if (n != (ssize_t)sizeof(*jh)) die("pwrite(journal_header)");
}

static void journal_init_if_needed(int fd, struct journal_header *jh) {
    journal_read_header(fd, jh);
    if (jh->magic != JOURNAL_MAGIC) {
        jh->magic = JOURNAL_MAGIC;
        jh->nbytes_used = (uint32_t)sizeof(struct journal_header);
        journal_write_header(fd, jh);
    }
    if (jh->nbytes_used < sizeof(struct journal_header) || jh->nbytes_used > journal_capacity()) {
        die_msg("Journal header corrupted (nbytes_used out of range).");
    }
}

static void journal_require_exists(int fd, struct journal_header *jh) {
    journal_read_header(fd, jh);
    if (jh->magic != JOURNAL_MAGIC) {
        die_msg("Journal does not exist (bad magic).");
    }
    if (jh->nbytes_used < sizeof(struct journal_header) || jh->nbytes_used > journal_capacity()) {
        die_msg("Journal header corrupted (nbytes_used out of range).");
    }
}

static void journal_append_bytes(int fd, struct journal_header *jh, const void *data, uint32_t nbytes) {
    if (jh->nbytes_used + nbytes > journal_capacity()) {
        fprintf(stderr, "ERROR: Journal full, run install.\n");
        exit(EXIT_FAILURE);
    }
    off_t off = journal_base_off() + (off_t)jh->nbytes_used;
    ssize_t n = pwrite(fd, data, nbytes, off);
    if (n != (ssize_t)nbytes) die("pwrite(journal_append)");
    jh->nbytes_used += nbytes;
    journal_write_header(fd, jh);
}

static void journal_append_data_record(int fd, struct journal_header *jh, uint32_t block_no, const uint8_t block_img[BLOCK_SIZE]) {
    struct rec_header rh;
    rh.type = (uint16_t)REC_DATA;
    rh.size = (uint16_t)(sizeof(struct rec_header) + sizeof(uint32_t) + BLOCK_SIZE); 

    journal_append_bytes(fd, jh, &rh, sizeof(rh));
    journal_append_bytes(fd, jh, &block_no, sizeof(block_no));
    journal_append_bytes(fd, jh, block_img, BLOCK_SIZE);
}

static void journal_append_commit_record(int fd, struct journal_header *jh) {
    struct rec_header rh;
    rh.type = (uint16_t)REC_COMMIT;
    rh.size = (uint16_t)sizeof(struct rec_header); 
    journal_append_bytes(fd, jh, &rh, sizeof(rh));
}

static void journal_clear(int fd) {
    struct journal_header jh;
    jh.magic = JOURNAL_MAGIC;
    jh.nbytes_used = (uint32_t)sizeof(struct journal_header);
    journal_write_header(fd, &jh);
}


static void replay_committed_to_memory(
    int fd,
    const struct journal_header *jh,
    uint8_t inode_bitmap[BLOCK_SIZE],
    uint8_t inode_tbl0[BLOCK_SIZE],
    uint8_t inode_tbl1[BLOCK_SIZE],
    uint32_t root_dir_blk,
    uint8_t root_dir[BLOCK_SIZE]
) {
    if (jh->magic != JOURNAL_MAGIC) return;
    if (jh->nbytes_used <= sizeof(struct journal_header)) return;


    int pend_ib = 0, pend_t0 = 0, pend_t1 = 0, pend_rd = 0;
    uint8_t buf_ib[BLOCK_SIZE], buf_t0[BLOCK_SIZE], buf_t1[BLOCK_SIZE], buf_rd[BLOCK_SIZE];

    uint32_t end = jh->nbytes_used;
    uint32_t off = (uint32_t)sizeof(struct journal_header);

    while (off + sizeof(struct rec_header) <= end) {
        struct rec_header rh;
        if (pread(fd, &rh, sizeof(rh), journal_base_off() + (off_t)off) != (ssize_t)sizeof(rh)) break;

        if (rh.size < sizeof(struct rec_header)) break;
        if (off + (uint32_t)rh.size > end) break;

        uint32_t rec_start = off;
        off += (uint32_t)sizeof(rh);

        if (rh.type == REC_DATA) {
            uint32_t expected = (uint32_t)(sizeof(struct rec_header) + sizeof(uint32_t) + BLOCK_SIZE);
            if ((uint32_t)rh.size != expected) break;

            uint32_t blkno;
            uint8_t imgblk[BLOCK_SIZE];

            if (pread(fd, &blkno, sizeof(blkno), journal_base_off() + (off_t)off) != (ssize_t)sizeof(blkno)) break;
            off += (uint32_t)sizeof(blkno);

            if (pread(fd, imgblk, BLOCK_SIZE, journal_base_off() + (off_t)off) != (ssize_t)BLOCK_SIZE) break;
            off += BLOCK_SIZE;

            if (blkno == INODE_BMAP_IDX) {
                memcpy(buf_ib, imgblk, BLOCK_SIZE); pend_ib = 1;
            } else if (blkno == INODE_START_IDX) {
                memcpy(buf_t0, imgblk, BLOCK_SIZE); pend_t0 = 1;
            } else if (blkno == INODE_START_IDX + 1U) {
                memcpy(buf_t1, imgblk, BLOCK_SIZE); pend_t1 = 1;
            } else if (blkno == root_dir_blk) {
                memcpy(buf_rd, imgblk, BLOCK_SIZE); pend_rd = 1;
            }

        } else if (rh.type == REC_COMMIT) {
            if ((uint32_t)rh.size != sizeof(struct rec_header)) break;

            
            if (pend_ib) memcpy(inode_bitmap, buf_ib, BLOCK_SIZE);
            if (pend_t0) memcpy(inode_tbl0,  buf_t0, BLOCK_SIZE);
            if (pend_t1) memcpy(inode_tbl1,  buf_t1, BLOCK_SIZE);
            if (pend_rd) memcpy(root_dir,    buf_rd, BLOCK_SIZE);

            
            pend_ib = pend_t0 = pend_t1 = pend_rd = 0;

        } else {
            
            break;
        }

        off = rec_start + (uint32_t)rh.size;
    }
}



static void cmd_create(const char *img, const char *name) {
    if (strlen(name) > 27) {
        die_msg("File name too long (max 27 chars).");
    }

    int fd = open(img, O_RDWR);
    if (fd < 0) die("open");

    uint8_t sb_block[BLOCK_SIZE];
    read_block(fd, 0, sb_block);

    struct superblock sb;
    memcpy(&sb, sb_block, sizeof(sb));

    if (sb.magic != FS_MAGIC ||
        sb.block_size != BLOCK_SIZE ||
        sb.total_blocks != TOTAL_BLOCKS ||
        sb.journal_block != JOURNAL_BLOCK_IDX ||
        sb.inode_bitmap != INODE_BMAP_IDX ||
        sb.data_bitmap != DATA_BMAP_IDX ||
        sb.inode_start != INODE_START_IDX ||
        sb.data_start != DATA_START_IDX) {
        die_msg("Unexpected filesystem layout (superblock mismatch).");
    }


    uint8_t inode_bitmap[BLOCK_SIZE];
    uint8_t inode_tbl0[BLOCK_SIZE];
    uint8_t inode_tbl1[BLOCK_SIZE];

    read_block(fd, INODE_BMAP_IDX, inode_bitmap);
    read_block(fd, INODE_START_IDX, inode_tbl0);
    read_block(fd, INODE_START_IDX + 1U, inode_tbl1);

   
    uint8_t inode_area0[INODE_BLOCKS * BLOCK_SIZE];
    memcpy(inode_area0, inode_tbl0, BLOCK_SIZE);
    memcpy(inode_area0 + BLOCK_SIZE, inode_tbl1, BLOCK_SIZE);
    struct inode *inodes0 = (struct inode *)inode_area0;

    struct inode *root0 = &inodes0[0];
    if (root0->type != 2 || root0->direct[0] == 0) die_msg("Root inode invalid.");
    uint32_t root_dir_blk = root0->direct[0];

    uint8_t root_dir[BLOCK_SIZE];
    read_block(fd, root_dir_blk, root_dir);

    
    struct journal_header jh;
    journal_init_if_needed(fd, &jh);

    replay_committed_to_memory(fd, &jh, inode_bitmap, inode_tbl0, inode_tbl1, root_dir_blk, root_dir);

    
    uint8_t inode_area[INODE_BLOCKS * BLOCK_SIZE];
    memcpy(inode_area, inode_tbl0, BLOCK_SIZE);
    memcpy(inode_area + BLOCK_SIZE, inode_tbl1, BLOCK_SIZE);
    struct inode *inodes = (struct inode *)inode_area;

    struct inode *root = &inodes[0];
    if (root->type != 2 || root->direct[0] == 0) die_msg("Root inode invalid after replay.");
    if (root->direct[0] != root_dir_blk) die_msg("Unexpected root dir block change.");

    struct dirent *dirents = (struct dirent *)root_dir;

    
    for (uint32_t i = 0; i < BLOCK_SIZE / sizeof(struct dirent); ++i) {
        if (dirents[i].inode == 0) continue;
        if (strcmp(dirents[i].name, name) == 0) die_msg("File already exists.");
    }

    int free_ino = find_free_inode(inode_bitmap, sb.inode_count);
    if (free_ino < 0) die_msg("No free inode.");

    int free_slot = -1;
    for (uint32_t i = 0; i < BLOCK_SIZE / sizeof(struct dirent); ++i) {
        if (dirents[i].inode == 0 && dirents[i].name[0] == '\0') { free_slot = (int)i; break; }
    }
    if (free_slot < 0) die_msg("Root directory full.");

   
    bitmap_set(inode_bitmap, (uint32_t)free_ino);

    uint32_t now = (uint32_t)time(NULL);

    struct inode *newino = &inodes[free_ino];
    memset(newino, 0, sizeof(*newino));
    newino->type  = 1;
    newino->links = 1;
    newino->size  = 0;
    newino->ctime = now;
    newino->mtime = now;

    dirents[free_slot].inode = (uint32_t)free_ino;
    memset(dirents[free_slot].name, 0, sizeof(dirents[free_slot].name));
    strncpy(dirents[free_slot].name, name, sizeof(dirents[free_slot].name) - 1);

    
    uint32_t needed = (uint32_t)(free_slot + 1U) * (uint32_t)sizeof(struct dirent);
    if (root->size < needed) root->size = needed;
    root->mtime = now;

    
    memcpy(inode_tbl0, inode_area, BLOCK_SIZE);
    memcpy(inode_tbl1, inode_area + BLOCK_SIZE, BLOCK_SIZE);

   
    uint32_t ino_byte = (uint32_t)free_ino * INODE_SIZE;
    uint32_t ino_blk_in_table = ino_byte / BLOCK_SIZE; 
    int need_tbl1 = (ino_blk_in_table == 1);

    uint32_t data_rec_sz = (uint32_t)(sizeof(struct rec_header) + sizeof(uint32_t) + BLOCK_SIZE); 
    uint32_t data_records = 1 + 1 + 1 + (need_tbl1 ? 1 : 0);
    uint32_t need_bytes  = data_records * data_rec_sz + (uint32_t)sizeof(struct rec_header);

    if (jh.nbytes_used + need_bytes > journal_capacity()) {
        fprintf(stderr, "Journal full, run install first.\n");
        close(fd);
        exit(EXIT_FAILURE);
    }

   
    journal_append_data_record(fd, &jh, INODE_BMAP_IDX, inode_bitmap);
    journal_append_data_record(fd, &jh, INODE_START_IDX, inode_tbl0);
    if (need_tbl1) {
        journal_append_data_record(fd, &jh, INODE_START_IDX + 1U, inode_tbl1);
    }
    journal_append_data_record(fd, &jh, root_dir_blk, root_dir);
    journal_append_commit_record(fd, &jh);

    printf("Transaction committed for file '%s' (inode %d).\n", name, free_ino);

    close(fd);
}



struct pending_data {
    uint32_t blkno;
    uint8_t  img[BLOCK_SIZE];
};

static void pending_push(struct pending_data **arr, size_t *len, size_t *cap, uint32_t blkno, const uint8_t img[BLOCK_SIZE]) {
    if (*len == *cap) {
        size_t newcap = (*cap == 0) ? 8 : (*cap * 2);
        struct pending_data *tmp = realloc(*arr, newcap * sizeof(*tmp));
        if (!tmp) die("realloc");
        *arr = tmp;
        *cap = newcap;
    }
    (*arr)[*len].blkno = blkno;
    memcpy((*arr)[*len].img, img, BLOCK_SIZE);
    (*len)++;
}

static void cmd_install(const char *img) {
    int fd = open(img, O_RDWR);
    if (fd < 0) die("open");

    struct journal_header jh;
    journal_require_exists(fd, &jh);

    if (jh.nbytes_used <= sizeof(struct journal_header)) {
        die_msg("Journal is empty (nothing to install).");
    }

    uint32_t end = jh.nbytes_used;
    uint32_t off = (uint32_t)sizeof(struct journal_header);

    struct pending_data *pending = NULL;
    size_t pend_len = 0, pend_cap = 0;

    int saw_commit = 0;

    while (off + sizeof(struct rec_header) <= end) {
        struct rec_header rh;
        ssize_t n = pread(fd, &rh, sizeof(rh), journal_base_off() + (off_t)off);
        if (n != (ssize_t)sizeof(rh)) break;

        if (rh.size < sizeof(struct rec_header)) break;
        if (off + (uint32_t)rh.size > end) break; 

        uint32_t rec_start = off;
        off += (uint32_t)sizeof(rh);

        if (rh.type == REC_DATA) {
            uint32_t expected = (uint32_t)(sizeof(struct rec_header) + sizeof(uint32_t) + BLOCK_SIZE);
            if ((uint32_t)rh.size != expected) break;

            uint32_t blkno;
            uint8_t  imgblk[BLOCK_SIZE];

            if (pread(fd, &blkno, sizeof(blkno), journal_base_off() + (off_t)off) != (ssize_t)sizeof(blkno)) break;
            off += (uint32_t)sizeof(blkno);

            if (pread(fd, imgblk, BLOCK_SIZE, journal_base_off() + (off_t)off) != (ssize_t)BLOCK_SIZE) break;
            off += BLOCK_SIZE;

            pending_push(&pending, &pend_len, &pend_cap, blkno, imgblk);

        } else if (rh.type == REC_COMMIT) {
            if ((uint32_t)rh.size != sizeof(struct rec_header)) break;

            for (size_t i = 0; i < pend_len; ++i) {
                write_block(fd, pending[i].blkno, pending[i].img);
            }
            pend_len = 0;
            saw_commit = 1;

        } else {
            fprintf(stderr, "Unknown record type, aborting.\n");
            break;
        }

        off = rec_start + (uint32_t)rh.size;
    }

    if (!saw_commit) {
        printf("No complete transaction found, nothing applied.\n");
    } else {
        printf("Replayed committed transactions.\n");
    }

    journal_clear(fd);
    printf("Install complete (journal cleared).\n");

    free(pending);
    close(fd);
}



int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage:\n  %s create <name>\n  %s install\n", argv[0], argv[0]);
        return 1;
    }

    const char *img = DEFAULT_IMAGE;

    if (strcmp(argv[1], "create") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Missing file name.\n");
            return 1;
        }
        cmd_create(img, argv[2]);
        return 0;
    } else if (strcmp(argv[1], "install") == 0) {
        cmd_install(img);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    return 1;
}