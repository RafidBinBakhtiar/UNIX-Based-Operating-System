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

int find_free_inode(uint8_t *bitmap, uint64_t inode_count) {
    for (uint64_t initial = 0; initial < inode_count; initial++) {
        if (!(bitmap[initial / 8] & (1 << (initial % 8)))) {
            return initial + 1; 
        }
    }
    return -1;
}

int find_free_data_block(uint8_t *bitmap, uint64_t data_blocks) {
    for (uint64_t initial = 0; initial < data_blocks; initial++) {
        if (!(bitmap[initial / 8] & (1 << (initial % 8)))) {
            return initial;
        }
    }
    return -1;
}

void usage() {
    fprintf(stderr, "Usage: mkfs_adder --input <input.img> --output <output.img> --file <filename>\n");
}

int main(int argc, char *argv[]) {
    crc32_init();
    
    char *input_name = NULL;
    char *output_name = NULL;
    char *file_name = NULL;
    
    static struct option long_options[] = {
        {"input", required_argument, 0, 'i'},
        {"output", required_argument, 0, 'o'},
        {"file", required_argument, 0, 'f'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "i:o:f:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i': input_name = optarg; break;
            case 'o': output_name = optarg; break;
            case 'f': file_name = optarg; break;
            default: 
                usage();
                exit(EXIT_FAILURE);
        }
    }
    
    if (!input_name || !output_name || !file_name) {
        usage();
        exit(EXIT_FAILURE);
    }
    
    // Open input file
    FILE *fp_in = fopen(input_name, "rb");
    if (!fp_in) {
        perror("Failed to open input image");
        exit(EXIT_FAILURE);
    }
    
    
    superblock_t sb;
    if (fread(&sb, sizeof(sb), 1, fp_in) != 1) {
        perror("Failed to read superblock");
        fclose(fp_in);
        exit(EXIT_FAILURE);
    }
    
    
    if (sb.magic != 0x4D565346) {
        fprintf(stderr, "Invalid file system magic number\n");
        fclose(fp_in);
        exit(EXIT_FAILURE);
    }
    
    
    uint8_t inode_bitmap[BS];
    fseek(fp_in, sb.inode_bitmap_start * BS, SEEK_SET);
    if (fread(inode_bitmap, BS, 1, fp_in) != 1) {
        perror("Failed to read inode bitmap");
        fclose(fp_in);
        exit(EXIT_FAILURE);
    }
    
    
    uint8_t data_bitmap[BS];
    fseek(fp_in, sb.data_bitmap_start * BS, SEEK_SET);
    if (fread(data_bitmap, BS, 1, fp_in) != 1) {
        perror("Failed to read data bitmap");
        fclose(fp_in);
        exit(EXIT_FAILURE);
    }
    
    
    uint8_t *inode_table = malloc(sb.inode_table_blocks * BS);
    fseek(fp_in, sb.inode_table_start * BS, SEEK_SET);
    if (fread(inode_table, sb.inode_table_blocks * BS, 1, fp_in) != 1) {
        perror("Failed to read inode table");
        free(inode_table);
        fclose(fp_in);
        exit(EXIT_FAILURE);
    }
    
printf("Data region start: %" PRIu64 " blocks\n", sb.data_region_start);
printf("Data region blocks: %" PRIu64 " blocks\n", sb.data_region_blocks);
printf("File position before seeking: %ld\n", ftell(fp_in));

fseek(fp_in, sb.data_region_start * BS, SEEK_SET);
printf("File position after seeking: %ld\n", ftell(fp_in));


uint8_t *data_region = malloc(sb.data_region_blocks * BS);
if (fread(data_region, sb.data_region_blocks * BS, 1, fp_in) != 1) {
    perror("Failed to read data region");
    printf("Error code: %d\n", errno);
    printf("Tried to read %" PRIu64 " bytes\n", sb.data_region_blocks * BS);
    printf("File size: %ld bytes\n", ftell(fp_in));
    free(inode_table);
    free(data_region);
    fclose(fp_in);
    exit(EXIT_FAILURE);
}
    
    fclose(fp_in);
    
    
    int free_inode = find_free_inode(inode_bitmap, sb.inode_count);
    if (free_inode == -1) {
        fprintf(stderr, "Sorry.No free inodes available\n");
        free(inode_table);
        free(data_region);
        exit(EXIT_FAILURE);
    }
    
    
    FILE *file_fp = fopen(file_name, "rb");
    if (!file_fp) {
        perror("Failed to open file to add");
        free(inode_table);
        free(data_region);
        exit(EXIT_FAILURE);
    }
    
    
    fseek(file_fp, 0, SEEK_END);
    uint64_t file_size = ftell(file_fp);
    fseek(file_fp, 0, SEEK_SET);
    
    
    uint64_t needed_blocks = (file_size + BS - 1) / BS;
    if (needed_blocks > 12) {
        fprintf(stderr, "File too large - exceeds 12 direct blocks\n");
        fclose(file_fp);
        free(inode_table);
        free(data_region);
        exit(EXIT_FAILURE);
    }
    
    // Find free data blocks
    uint32_t data_blocks[12] = {0};
    for (uint64_t initial = 0; initial < needed_blocks; initial++) {
        int free_block = find_free_data_block(data_bitmap, sb.data_region_blocks);
        if (free_block == -1) {
            fprintf(stderr, "Not enough free data blocks\n");
            fclose(file_fp);
            free(inode_table);
            free(data_region);
            exit(EXIT_FAILURE);
        }
        data_blocks[initial] = sb.data_region_start + free_block;
        data_bitmap[free_block / 8] |= (1 << (free_block % 8));
    }
    
    // Read file content
    uint8_t *file_content = malloc(file_size);
    if (fread(file_content, file_size, 1, file_fp) != 1) {
        perror("Failed to read file content");
        fclose(file_fp);
        free(inode_table);
        free(data_region);
        free(file_content);
        exit(EXIT_FAILURE);
    }
    fclose(file_fp);
    
    // Write file content to data blocks
    for (uint64_t initial = 0; initial < needed_blocks; initial++) {
        uint64_t offset = (data_blocks[initial] - sb.data_region_start) * BS;
        uint64_t copy_size = (initial == needed_blocks - 1) ? 
            file_size - (initial * BS) : BS;
        memcpy(data_region + offset, file_content + (initial * BS), copy_size);
    }
    free(file_content);
    
    // Create new inode
    inode_t *new_inode = (inode_t *)(inode_table + (free_inode - 1) * INODE_SIZE);
    time_t now = time(NULL);
    
    new_inode->mode = 0x8000; // Regular file
    new_inode->links = 1;
    new_inode->uid = 0;
    new_inode->gid = 0;
    new_inode->size_bytes = file_size;
    new_inode->atime = now;
    new_inode->mtime = now;
    new_inode->ctime = now;
    for (uint64_t initial = 0; initial < 12; initial++) {
        new_inode->direct[initial] = (initial < needed_blocks) ? data_blocks[initial] : 0;
    }
    new_inode->proj_id = 1234;
    inode_crc_finalize(new_inode);
    

    inode_bitmap[(free_inode - 1) / 8] |= (1 << ((free_inode - 1) % 8));
    
    
    inode_t *root_inode = (inode_t *)inode_table;
    uint32_t root_data_block = root_inode->direct[0] - sb.data_region_start;
    dirent64_t *root_entries = (dirent64_t *)(data_region + root_data_block * BS);
    
    
    int free_entry = -1;
    uint64_t entry_count = root_inode->size_bytes / sizeof(dirent64_t);
    for (uint64_t i = 0; i < entry_count; i++) {
        if (root_entries[i].inode_no == 0) {
            free_entry = i;
            break;
        }
    }
    
    if (free_entry == -1) {
    
        if (entry_count >= (BS / sizeof(dirent64_t))) {
            fprintf(stderr, "Root directory is full\n");
            free(inode_table);
            free(data_region);
            exit(EXIT_FAILURE);
        }
        
        free_entry = entry_count;
        root_inode->size_bytes += sizeof(dirent64_t);
        inode_crc_finalize(root_inode);
    }
    
    // Create directory entry
    root_entries[free_entry].inode_no = free_inode;
    root_entries[free_entry].type = 1; // File
    
    
    size_t name_len = strlen(file_name);
    if (name_len > 57) name_len = 57; 
    memset(root_entries[free_entry].name, 0, 58);
    strncpy(root_entries[free_entry].name, file_name, name_len);
    root_entries[free_entry].name[name_len] = '\0';
    
    dirent_checksum_finalize(&root_entries[free_entry]);
    
    root_inode->links++;
    inode_crc_finalize(root_inode);
    
    // Write output file
    FILE *fp_out = fopen(output_name, "wb");
    if (!fp_out) {
        perror("Failed to create output image");
        free(inode_table);
        free(data_region);
        exit(EXIT_FAILURE);
    }
    

    fwrite(&sb, sizeof(sb), 1, fp_out);
    

    fseek(fp_out, sb.inode_bitmap_start * BS, SEEK_SET);
    fwrite(inode_bitmap, BS, 1, fp_out);
    
    
    fseek(fp_out, sb.data_bitmap_start * BS, SEEK_SET);
    fwrite(data_bitmap, BS, 1, fp_out);
    

    fseek(fp_out, sb.inode_table_start * BS, SEEK_SET);
    fwrite(inode_table, sb.inode_table_blocks * BS, 1, fp_out);
    
    
    fseek(fp_out, sb.data_region_start * BS, SEEK_SET);
    fwrite(data_region, sb.data_region_blocks * BS, 1, fp_out);
    
    fclose(fp_out);
    free(inode_table);
    free(data_region);
    
    printf("File '%s' added successfully to inode %d\n", file_name, free_inode);
    printf("Output image: %s\n", output_name);
    
    return 0;
}