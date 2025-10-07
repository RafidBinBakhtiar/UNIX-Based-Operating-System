#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <getopt.h>
#include <unistd.h>
#include <fcntl.h>


#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u


uint64_t g_random_seed = 0;


#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;
    uint64_t root_inode;
    uint64_t mtime_epoch;
    uint32_t flags;
    uint32_t checksum;
} superblock_t;
#pragma pack(pop)


_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");


#pragma pack(push,1)
typedef struct {
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[12];
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;
    uint64_t inode_crc;
} inode_t;
#pragma pack(pop)


_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");


#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;
    uint8_t type;
    char name[58];
    uint8_t checksum;
} dirent64_t;
#pragma pack(pop)


_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");


uint32_t CRC32_TAB[256];


void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}


uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}


static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}


void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c;
}


void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];
    de->checksum = x;
}


void usage() {
    fprintf(stderr, "Usage: mkfs_builder --image <filename> --size-kib <size> --inodes <count>\n");
    fprintf(stderr, "  --size-kib: 180-4096, multiple of 4\n");
    fprintf(stderr, "  --inodes: 128-512\n");
}


int main(int argc, char *argv[]) {
    crc32_init();
   
    char *imageName = NULL;
    uint64_t size_kib = 0;
    uint64_t inodes = 0;
   
    static struct option long_options[] = {
        {"image", required_argument, 0, 'i'},
        {"size-kib", required_argument, 0, 's'},
        {"inodes", required_argument, 0, 'n'},
        {0, 0, 0, 0}
    };
   
    int opt;
    while ((opt = getopt_long(argc, argv, "i:s:n:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i': imageName = optarg; break;
            case 's': size_kib = atoll(optarg); break;
            case 'n': inodes = atoll(optarg); break;
            default:
                usage();
                exit(EXIT_FAILURE);
        }
    }
   
    if (!imageName || size_kib < 180 || size_kib > 4096 ||
        inodes < 128 || inodes > 512 || (size_kib % 4 != 0)) {
        usage();
        exit(EXIT_FAILURE);
    }
   
    uint64_t total_blocks = size_kib * 1024 / BS;
    uint64_t inode_table_blocks = (inodes * INODE_SIZE + BS - 1) / BS;
    uint64_t data_region_start = 3 + inode_table_blocks;
    uint64_t data_region_blocks = total_blocks - data_region_start;
   
    if (data_region_blocks < 1) {
        fprintf(stderr, "File system too small for layout\n");
        exit(EXIT_FAILURE);
    }
   
    time_t now = time(NULL);
   
    superblock_t sb = {
        .magic = 0x4D565346,
        .version = 1,
        .block_size = BS,
        .total_blocks = total_blocks,
        .inode_count = inodes,
        .inode_bitmap_start = 1,
        .inode_bitmap_blocks = 1,
        .data_bitmap_start = 2,
        .data_bitmap_blocks = 1,
        .inode_table_start = 3,
        .inode_table_blocks = inode_table_blocks,
        .data_region_start = data_region_start,
        .data_region_blocks = data_region_blocks,
        .root_inode = 1,
        .mtime_epoch = now,
        .flags = 0
    };
    superblock_crc_finalize(&sb);
   
    FILE *fp = fopen(imageName, "wb");
    if (!fp) {
        perror("Failed to create image file");
        exit(EXIT_FAILURE);
    }


   
    uint8_t superblock_buffer[BS] = {0};
    memcpy(superblock_buffer, &sb, sizeof(sb));
    if (fwrite(superblock_buffer, BS, 1, fp) != 1) {
        perror("Superblock writen failed");
        fclose(fp);
        exit(EXIT_FAILURE);
    }


    uint8_t inode_bitmap[BS] = {0};
    inode_bitmap[0] = 0x01;
    if (fwrite(inode_bitmap, BS, 1, fp) != 1) {
        perror("Failed to write inode bitmap");
        fclose(fp);
        exit(EXIT_FAILURE);
    }




    uint8_t data_bitmap[BS] = {0};
    data_bitmap[0] = 0x01;
    if (fwrite(data_bitmap, BS, 1, fp) != 1) {
        perror("Failed to write data bitmap");
        fclose(fp);
        exit(EXIT_FAILURE);
    }




    uint8_t *inode_table = calloc(inode_table_blocks, BS);
    if (!inode_table) {
        perror("Failed to allocate inode table");
        fclose(fp);
        exit(EXIT_FAILURE);
    }


    inode_t *root_inode = (inode_t *)(inode_table);
    root_inode->mode = 0x4000;
    root_inode->links = 2;    
    root_inode->uid = 0;
    root_inode->gid = 0;
    root_inode->size_bytes = 2 * sizeof(dirent64_t);
    root_inode->atime = now;
    root_inode->mtime = now;
    root_inode->ctime = now;
    root_inode->direct[0] = data_region_start;
    for (int i = 1; i < 12; i++) root_inode->direct[i] = 0;
    root_inode->proj_id = 1234;
    inode_crc_finalize(root_inode);


    if (fwrite(inode_table, BS, inode_table_blocks, fp) != inode_table_blocks) {
        perror("Failed to write inode table");
        free(inode_table);
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    free(inode_table);


   
    uint8_t *data_region = calloc(data_region_blocks, BS);
    if (!data_region) {
        perror("Failed to allocate data region");
        fclose(fp);
        exit(EXIT_FAILURE);
    }


   
    dirent64_t *entries = (dirent64_t *)data_region;




    entries[0].inode_no = 1;
    entries[0].type = 2;
    strncpy(entries[0].name, ".", sizeof(entries[0].name));
    dirent_checksum_finalize(&entries[0]);




    entries[1].inode_no = 1;
    entries[1].type = 2;
    strncpy(entries[1].name, "..", sizeof(entries[1].name));
    dirent_checksum_finalize(&entries[1]);


    if (fwrite(data_region, BS, data_region_blocks, fp) != data_region_blocks) {
        perror("Failed to write data region");
        free(data_region);
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    free(data_region);




    long current_pos = ftell(fp);
    long expected_size = total_blocks * BS;
    if (current_pos != expected_size) {
        fprintf(stderr, "File size incorrect: %ld bytes (expected: %ld bytes)\n",
                current_pos, expected_size);
        fclose(fp);
        exit(EXIT_FAILURE);
    }


    fclose(fp);
   
    printf("File system created successfully: %s\n", imageName);
    printf("  Size: %" PRIu64 " KiB, Inodes: %" PRIu64 ", Blocks: %" PRIu64 "\n",
           size_kib, inodes, total_blocks);
   
    return 0;
}
