/* DOS MZ executable header and loader structures */
#ifndef EXE_H
#define EXE_H

#include <stdint.h>

struct image_dos_header {       /* DOS .EXE header */
    uint16_t e_magic;           /* Magic number                  0x00 */
    uint16_t e_cblp;            /* Bytes on last page of file    0x02 */
    uint16_t e_cp;              /* Pages in file                 0x04 */
    uint16_t e_crlc;            /* Relocations                   0x06 */
    uint16_t e_cparhdr;         /* Size of header in paragraphs  0x08 */
    uint16_t e_minalloc;        /* Minimum extra paragraphs needed     */
    uint16_t e_maxalloc;        /* Maximum extra paragraphs needed     */
    uint16_t e_ss;              /* Initial (relative) SS value   0x0e */
    uint16_t e_sp;              /* Initial SP value              0x10 */
    uint16_t e_csum;            /* Checksum                            */
    uint16_t e_ip;              /* Initial IP value              0x14 */
    uint16_t e_cs;              /* Initial (relative) CS value   0x16 */
    uint16_t e_lfarlc;          /* File address of relocation table 0x18 */
    uint16_t e_ovno;            /* Overlay number                      */
    uint16_t e_res[4];          /* Reserved words                      */
    uint16_t e_oemid;           /* OEM identifier                      */
    uint16_t e_oeminfo;         /* OEM information                     */
    uint16_t e_res2[10];        /* Reserved words                      */
    uint32_t e_lfanew;          /* File address of new exe header      */
};

struct dos_reloc {              /* DOS relocation table entry */
    uint16_t r_offset;          /* Offset of segment to reloc from r_seg */
    uint16_t r_seg;             /* Segment relative to load segment */
};

#define DOSMAGIC    0x5a4d      /* 'MZ' magic number for DOS MZ executables */

struct exe {
    struct image_dos_header dos;
    int (*checkStack)(struct exe *e);
    int (*handleSyscall)(struct exe *e, int intno);
    /* break management */
    uint16_t t_endseg;          /* end of data segment */
    uint16_t t_begstack;        /* start SP */
    uint16_t t_minstack;        /* min stack size */
    uint16_t t_enddata;         /* start heap = end of data+bss */
    uint16_t t_endbrk;          /* current break (end of heap) */
    /* stack overflow check */
    uint32_t t_stackLow;        /* lowest SS:SP allowed */
};

/* loader entry point */
void loadExecutableDOS(struct exe *e, const char *filename, int argc, char **argv);

#endif /* EXE_H */
