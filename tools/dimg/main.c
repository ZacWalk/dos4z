/*
 * dimg — Create a bootable MS-DOS 4.0 floppy disk image (.img)
 *
 * Reads the compiled DOS binaries from output/bin/ and assembles a FAT12
 * disk image. Supports 360KB (5.25" DD) and 1.44MB (3.5" HD) formats.
 *
 * Usage:
 *   dimg -o <output.img> -b <bindir> [-s 360|1440] [-f <file> ...] [-l <label>]
 *
 * The boot sector is extracted from MSBOOT.BIN (offset 0x7C00, 512 bytes)
 * and patched with the appropriate BPB. IO.SYS and MSDOS.SYS are placed as
 * the first two directory entries with contiguous clusters, as required by
 * the boot loader. COMMAND.COM and any additional files are added after.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

/* ── Floppy geometry ───────────────────────────────────────────────── */

#define SECTOR_SIZE       512
#define NUM_FATS          2
#define RESERVED_SECTORS  1

/* Boot sector location inside MSBOOT.BIN (exe2bin output, ORG 7C00h) */
#define BOOT_SECTOR_OFFSET  0x7C00

/* Maximum disk size (1.44MB) for static buffer */
#define MAX_DISK_SIZE     1474560

typedef struct {
    const char *name;            /* e.g. "360KB" */
    unsigned int sectors_per_track;
    unsigned int num_heads;
    unsigned int num_tracks;
    unsigned int sectors_per_cluster;
    unsigned int root_dir_entries;
    unsigned int sectors_per_fat;
    unsigned char media_descriptor;
    /* Derived (filled by init_geometry) */
    unsigned int total_sectors;
    unsigned long disk_size;
    unsigned int fat_start;
    unsigned int fat2_start;
    unsigned int root_dir_start;
    unsigned int root_dir_sectors;
    unsigned int data_start;
    unsigned int data_clusters;
    unsigned int cluster_size;
} DiskGeometry;

static DiskGeometry geom_360 = {
    "360KB", 9, 2, 40, 2, 112, 2, 0xFD,
    0, 0, 0, 0, 0, 0, 0, 0, 0
};

static DiskGeometry geom_1440 = {
    "1.44MB", 18, 2, 80, 1, 224, 9, 0xF0,
    0, 0, 0, 0, 0, 0, 0, 0, 0
};

static void init_geometry(DiskGeometry *g) {
    g->total_sectors   = g->sectors_per_track * g->num_heads * g->num_tracks;
    g->disk_size       = (unsigned long)g->total_sectors * SECTOR_SIZE;
    g->fat_start       = RESERVED_SECTORS;
    g->fat2_start      = g->fat_start + g->sectors_per_fat;
    g->root_dir_start  = g->fat2_start + g->sectors_per_fat;
    g->root_dir_sectors = (g->root_dir_entries * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;
    g->data_start      = g->root_dir_start + g->root_dir_sectors;
    g->data_clusters   = (g->total_sectors - g->data_start) / g->sectors_per_cluster;
    g->cluster_size    = g->sectors_per_cluster * SECTOR_SIZE;
}

/* Active geometry (set in main) */
static DiskGeometry *geo;

/* FAT12 end-of-chain marker */
#define FAT12_EOC  0xFFF

/* Maximum extra files on command line */
#define MAX_EXTRA_FILES 64

/* ── Helpers ───────────────────────────────────────────────────────── */

static unsigned char disk[MAX_DISK_SIZE];

static void write_le16(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void write_le32(unsigned char *p, unsigned long v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* Get a FAT12 entry */
static unsigned int fat12_get(unsigned int cluster) {
    unsigned char *fat = disk + geo->fat_start * SECTOR_SIZE;
    unsigned int offset = cluster + (cluster / 2);
    unsigned int val = fat[offset] | ((unsigned int)fat[offset + 1] << 8);
    if (cluster & 1)
        val >>= 4;
    else
        val &= 0xFFF;
    return val;
}

/* Set a FAT12 entry (writes to both FAT copies) */
static void fat12_set(unsigned int cluster, unsigned int value) {
    int f;
    for (f = 0; f < NUM_FATS; f++) {
        unsigned char *fat = disk + (geo->fat_start + f * geo->sectors_per_fat) * SECTOR_SIZE;
        unsigned int offset = cluster + (cluster / 2);
        if (cluster & 1) {
            fat[offset]     = (fat[offset] & 0x0F) | ((value & 0x0F) << 4);
            fat[offset + 1] = (unsigned char)((value >> 4) & 0xFF);
        } else {
            fat[offset]     = (unsigned char)(value & 0xFF);
            fat[offset + 1] = (fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F);
        }
    }
}

/* Find the next free cluster (starting from hint), returns 0 on failure */
static unsigned int find_free_cluster(unsigned int hint) {
    unsigned int c;
    for (c = hint; c < geo->data_clusters + 2; c++) {
        if (fat12_get(c) == 0)
            return c;
    }
    return 0;
}

/* Sector number for a given cluster */
static unsigned int cluster_to_sector(unsigned int cluster) {
    return geo->data_start + (cluster - 2) * geo->sectors_per_cluster;
}

/* Format an 8.3 filename from a normal name. Returns 0 on success. */
static int format_83_name(const char *name, unsigned char out[11]) {
    const char *dot;
    int i, len;

    memset(out, ' ', 11);

    /* Find the last path separator */
    const char *base = name;
    const char *p;
    for (p = name; *p; p++) {
        if (*p == '\\' || *p == '/')
            base = p + 1;
    }

    dot = NULL;
    for (p = base; *p; p++) {
        if (*p == '.')
            dot = p;
    }

    if (dot) {
        len = (int)(dot - base);
        if (len > 8) len = 8;
        for (i = 0; i < len; i++)
            out[i] = (unsigned char)toupper((unsigned char)base[i]);
        len = (int)strlen(dot + 1);
        if (len > 3) len = 3;
        for (i = 0; i < len; i++)
            out[8 + i] = (unsigned char)toupper((unsigned char)dot[1 + i]);
    } else {
        len = (int)strlen(base);
        if (len > 8) len = 8;
        for (i = 0; i < len; i++)
            out[i] = (unsigned char)toupper((unsigned char)base[i]);
    }

    return 0;
}

/* Current directory entry index */
static int dir_entry_count = 0;

/* Add a file to the disk image. Returns 0 on success.
 * If system_file is set, the SYS+HIDDEN+READONLY attributes are used.
 * If contiguous is set, clusters must be sequential (required for IO.SYS). */
static int add_file(const char *filepath, int system_file, int contiguous) {
    FILE *fp;
    unsigned char name83[11];
    unsigned char *dir;
    unsigned long file_size, remaining;
    unsigned int clusters_needed, first_cluster, prev_cluster, cur_cluster, i;
    unsigned char attr;
    time_t now;
    struct tm *t;
    unsigned int fat_date, fat_time;

    if (dir_entry_count >= (int)geo->root_dir_entries) {
        fprintf(stderr, "Error: root directory full\n");
        return 1;
    }

    fp = fopen(filepath, "rb");
    if (!fp) {
        fprintf(stderr, "Error: cannot open '%s'\n", filepath);
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    file_size = (unsigned long)ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size == 0) {
        fprintf(stderr, "Warning: '%s' is empty, skipping\n", filepath);
        fclose(fp);
        return 0;
    }

    clusters_needed = (unsigned int)((file_size + geo->cluster_size - 1) / geo->cluster_size);

    /* Allocate clusters */
    first_cluster = find_free_cluster(2);
    if (first_cluster == 0) {
        fprintf(stderr, "Error: disk full, cannot add '%s'\n", filepath);
        fclose(fp);
        return 1;
    }

    prev_cluster = 0;
    cur_cluster = first_cluster;
    for (i = 0; i < clusters_needed; i++) {
        if (cur_cluster == 0 || cur_cluster >= geo->data_clusters + 2) {
            fprintf(stderr, "Error: disk full, cannot add '%s'\n", filepath);
            fclose(fp);
            return 1;
        }

        if (contiguous && i > 0 && cur_cluster != first_cluster + i) {
            fprintf(stderr, "Error: cannot allocate contiguous clusters for '%s'\n", filepath);
            fclose(fp);
            return 1;
        }

        /* Link previous cluster to this one */
        if (prev_cluster != 0)
            fat12_set(prev_cluster, cur_cluster);

        prev_cluster = cur_cluster;
        cur_cluster = find_free_cluster(cur_cluster + 1);
    }
    /* Mark end of chain */
    fat12_set(prev_cluster, FAT12_EOC);

    /* Write file data to clusters */
    cur_cluster = first_cluster;
    remaining = file_size;
    for (i = 0; i < clusters_needed; i++) {
        unsigned int sector = cluster_to_sector(cur_cluster);
        unsigned long to_read = remaining > geo->cluster_size ? geo->cluster_size : remaining;
        if (fread(disk + sector * SECTOR_SIZE, 1, (size_t)to_read, fp) != (size_t)to_read) {
            fprintf(stderr, "Error: read error on '%s'\n", filepath);
            fclose(fp);
            return 1;
        }
        remaining -= to_read;

        /* Follow chain to next cluster */
        if (i + 1 < clusters_needed) {
            unsigned int next = fat12_get(cur_cluster);
            if (next >= 0xFF8) break;
            cur_cluster = next;
        }
    }
    fclose(fp);

    /* Create directory entry */
    format_83_name(filepath, name83);
    dir = disk + geo->root_dir_start * SECTOR_SIZE + dir_entry_count * 32;

    memcpy(dir, name83, 11);

    attr = 0x20; /* Archive */
    if (system_file)
        attr = 0x07; /* Read-only | Hidden | System */
    dir[11] = attr;

    memset(dir + 12, 0, 10); /* Reserved bytes */

    /* Timestamp */
    now = time(NULL);
    t = localtime(&now);
    fat_time = ((t->tm_hour & 0x1F) << 11) | ((t->tm_min & 0x3F) << 5) | ((t->tm_sec / 2) & 0x1F);
    fat_date = (((t->tm_year - 80) & 0x7F) << 9) | (((t->tm_mon + 1) & 0x0F) << 5) | (t->tm_mday & 0x1F);

    write_le16(dir + 22, fat_time);
    write_le16(dir + 24, fat_date);
    write_le16(dir + 26, (unsigned int)(first_cluster & 0xFFFF));
    write_le32(dir + 28, file_size);

    dir_entry_count++;
    return 0;
}

/* ── Boot sector + BPB ─────────────────────────────────────────────── */

/* Load the boot sector from MSBOOT.BIN and patch BPB for 360KB */
static int load_boot_sector(const char *msboot_path) {
    FILE *fp;
    long file_size;
    unsigned char *raw;

    fp = fopen(msboot_path, "rb");
    if (!fp) {
        fprintf(stderr, "Error: cannot open boot sector '%s'\n", msboot_path);
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size < BOOT_SECTOR_OFFSET + SECTOR_SIZE) {
        fprintf(stderr, "Error: MSBOOT.BIN too small (%ld bytes, need >= %d)\n",
                file_size, BOOT_SECTOR_OFFSET + SECTOR_SIZE);
        fclose(fp);
        return 1;
    }

    raw = (unsigned char *)malloc((size_t)file_size);
    if (!raw) {
        fprintf(stderr, "Error: out of memory\n");
        fclose(fp);
        return 1;
    }

    if (fread(raw, 1, (size_t)file_size, fp) != (size_t)file_size) {
        fprintf(stderr, "Error: read error on '%s'\n", msboot_path);
        free(raw);
        fclose(fp);
        return 1;
    }
    fclose(fp);

    /* Verify it looks like a boot sector */
    if (raw[BOOT_SECTOR_OFFSET + 510] != 0x55 || raw[BOOT_SECTOR_OFFSET + 511] != 0xAA) {
        fprintf(stderr, "Error: no boot signature (55 AA) at offset 0x%X+0x1FE in '%s'\n",
                BOOT_SECTOR_OFFSET, msboot_path);
        free(raw);
        return 1;
    }

    /* Copy 512-byte boot sector to start of disk image */
    memcpy(disk, raw + BOOT_SECTOR_OFFSET, SECTOR_SIZE);
    free(raw);

    /* Patch the BPB (bytes 11-61) for target floppy geometry */
    write_le16(disk + 11, SECTOR_SIZE);           /* Bytes per sector */
    disk[13] = (unsigned char)geo->sectors_per_cluster; /* Sectors per cluster */
    write_le16(disk + 14, RESERVED_SECTORS);      /* Reserved sectors */
    disk[16] = NUM_FATS;                          /* Number of FATs */
    write_le16(disk + 17, geo->root_dir_entries); /* Root directory entries */
    write_le16(disk + 19, geo->total_sectors);    /* Total sectors (16-bit) */
    disk[21] = geo->media_descriptor;             /* Media descriptor */
    write_le16(disk + 22, geo->sectors_per_fat);  /* Sectors per FAT */
    write_le16(disk + 24, geo->sectors_per_track);/* Sectors per track */
    write_le16(disk + 26, geo->num_heads);        /* Number of heads */
    write_le32(disk + 28, 0);                     /* Hidden sectors (0 for floppy) */
    write_le32(disk + 32, 0);                     /* Total sectors (32-bit, 0 = use 16-bit) */
    disk[36] = 0x00;                              /* Physical drive number (0 = floppy A:) */
    disk[37] = 0x00;                              /* Current head */
    disk[38] = 0x29;                              /* Extended boot signature */
    /* Bytes 39-42: volume serial number (random) */
    {
        unsigned long serial = (unsigned long)time(NULL);
        write_le32(disk + 39, serial);
    }

    return 0;
}

/* Set the volume label in the BPB and add a volume label directory entry */
static void set_volume_label(const char *label) {
    unsigned char lbl[11];
    unsigned char *dir;
    int i, len;

    memset(lbl, ' ', 11);
    len = (int)strlen(label);
    if (len > 11) len = 11;
    for (i = 0; i < len; i++)
        lbl[i] = (unsigned char)toupper((unsigned char)label[i]);

    /* Patch BPB volume label field (offset 43) */
    memcpy(disk + 43, lbl, 11);

    /* Add volume label directory entry */
    if (dir_entry_count < (int)geo->root_dir_entries) {
        dir = disk + geo->root_dir_start * SECTOR_SIZE + dir_entry_count * 32;
        memset(dir, 0, 32);
        memcpy(dir, lbl, 11);
        dir[11] = 0x08;  /* Volume label attribute */
        dir_entry_count++;
    }
}

/* Initialize both FAT copies with media descriptor */
static void init_fat(void) {
    int f;
    for (f = 0; f < NUM_FATS; f++) {
        unsigned char *fat = disk + (geo->fat_start + f * geo->sectors_per_fat) * SECTOR_SIZE;
        fat[0] = geo->media_descriptor;
        fat[1] = 0xFF;
        fat[2] = 0xFF;
    }
}

/* ── Usage / main ──────────────────────────────────────────────────── */

static void usage(const char *progname) {
    fprintf(stderr,
        "dimg — Create a bootable MS-DOS 4.0 floppy disk image\n"
        "\n"
        "Usage: %s -o <output.img> -b <bindir> [options]\n"
        "\n"
        "Required:\n"
        "  -o <file>    Output disk image file\n"
        "  -b <dir>     Directory containing DOS binaries (output/bin)\n"
        "\n"
        "Options:\n"
        "  -s <size>    Disk size: 360 or 1440 (default: 360)\n"
        "  -f <file>    Add an extra file to the disk (repeatable)\n"
        "  -l <label>   Volume label (default: MSDOS4)\n"
        "  -v           Verbose output\n"
        "  -h           Show this help\n"
        "\n"
        "The tool expects these files in <bindir>:\n"
        "  MSBOOT.BIN   Boot sector (exe2bin output, boot code at offset 0x7C00)\n"
        "  io.sys       BIOS (IO.SYS)\n"
        "  msdos.sys    DOS kernel (MSDOS.SYS)\n"
        "  command.com  Command interpreter (COMMAND.COM)\n"
        "\n"
        "IO.SYS and MSDOS.SYS are placed first with system/hidden attributes.\n"
        "COMMAND.COM is added as a normal file. Additional files via -f are\n"
        "added after the system files.\n",
        progname);
}

/* Build a path from directory + filename. Caller must free result. */
static char *build_path(const char *dir, const char *file) {
    size_t dlen = strlen(dir);
    size_t flen = strlen(file);
    char *path = (char *)malloc(dlen + 1 + flen + 1);
    if (!path) return NULL;
    memcpy(path, dir, dlen);
    if (dlen > 0 && dir[dlen - 1] != '\\' && dir[dlen - 1] != '/') {
        path[dlen] = '\\';
        dlen++;
    }
    memcpy(path + dlen, file, flen + 1);
    return path;
}

int main(int argc, char *argv[]) {
    const char *output_path = NULL;
    const char *bin_dir = NULL;
    const char *volume_label = "MSDOS4";
    const char *extra_files[MAX_EXTRA_FILES];
    int extra_count = 0;
    int verbose = 0;
    int disk_size_kb = 360;
    int i;
    char *path;
    FILE *fp;

    /* Parse arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            bin_dir = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            disk_size_kb = atoi(argv[++i]);
            if (disk_size_kb != 360 && disk_size_kb != 1440) {
                fprintf(stderr, "Error: -s must be 360 or 1440\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            if (extra_count >= MAX_EXTRA_FILES) {
                fprintf(stderr, "Error: too many extra files (max %d)\n", MAX_EXTRA_FILES);
                return 1;
            }
            extra_files[extra_count++] = argv[++i];
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            volume_label = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!output_path || !bin_dir) {
        fprintf(stderr, "Error: -o and -b are required\n\n");
        usage(argv[0]);
        return 1;
    }

    /* Select and initialize geometry */
    geo = (disk_size_kb == 1440) ? &geom_1440 : &geom_360;
    init_geometry(geo);

    /* Initialize disk image to zeros */
    memset(disk, 0, (size_t)geo->disk_size);

    /* Step 1: Load and patch boot sector */
    if (verbose) printf("Loading boot sector...\n");
    path = build_path(bin_dir, "MSBOOT.BIN");
    if (load_boot_sector(path) != 0) { free(path); return 1; }
    free(path);

    /* Step 2: Initialize FAT */
    if (verbose) printf("Initializing FAT12...\n");
    init_fat();

    /* Step 3: Add IO.SYS — must be first directory entry, contiguous clusters */
    if (verbose) printf("Adding IO.SYS...\n");
    path = build_path(bin_dir, "io.sys");
    if (add_file(path, 1, 1) != 0) { free(path); return 1; }
    free(path);

    /* Step 4: Add MSDOS.SYS — must be second directory entry, system attributes */
    if (verbose) printf("Adding MSDOS.SYS...\n");
    path = build_path(bin_dir, "msdos.sys");
    if (add_file(path, 1, 0) != 0) { free(path); return 1; }
    free(path);

    /* Step 5: Set volume label (after system files so IO.SYS is entry #0) */
    set_volume_label(volume_label);

    /* Step 6: Add COMMAND.COM */
    if (verbose) printf("Adding COMMAND.COM...\n");
    path = build_path(bin_dir, "command.com");
    if (add_file(path, 0, 0) != 0) { free(path); return 1; }
    free(path);

    /* Step 7: Add extra files */
    for (i = 0; i < extra_count; i++) {
        if (verbose) printf("Adding %s...\n", extra_files[i]);
        if (add_file(extra_files[i], 0, 0) != 0)
            return 1;
    }

    /* Step 8: Write disk image */
    fp = fopen(output_path, "wb");
    if (!fp) {
        fprintf(stderr, "Error: cannot create '%s'\n", output_path);
        return 1;
    }
    if (fwrite(disk, 1, (size_t)geo->disk_size, fp) != (size_t)geo->disk_size) {
        fprintf(stderr, "Error: write failed on '%s'\n", output_path);
        fclose(fp);
        return 1;
    }
    fclose(fp);

    /* Summary */
    {
        unsigned int used_clusters = 0;
        unsigned int c;
        for (c = 2; c < geo->data_clusters + 2; c++) {
            if (fat12_get(c) != 0)
                used_clusters++;
        }
        printf("Created %s boot disk: %s\n", geo->name, output_path);
        printf("  Files: %d (including volume label)\n", dir_entry_count);
        printf("  Clusters used: %u / %u (%u KB free)\n",
               used_clusters, geo->data_clusters,
               (geo->data_clusters - used_clusters) * geo->sectors_per_cluster * SECTOR_SIZE / 1024);
    }

    return 0;
}