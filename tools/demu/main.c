/*
 * demu - DOS Executable eMUlator
 *
 * Loads a DOS .EXE or .COM executable and runs it on modern Windows
 * by interpreting 8086 instructions and mapping DOS system calls
 * to Windows equivalents. Similar in concept to Wine on Linux.
 *
 * Usage: demu [options] <program.exe|program.com> [args...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <windows.h>
#include "8086.h"
#include "exe.h"
#include "exec-dos.h"

extern int f_verbose;
extern char demuExePath[MAX_PATH];
static int f_dump;
int f_shell;  /* running a command shell (COMMAND.COM) */

void runtimeError(const char *msg, ...)
{
    va_list args;
    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
    fprintf(stderr, "\nCS:IP = %04x:%04x\n", cs(), getIP());
    exit(1);
}

/* Try to dispatch an interrupt through the IVT (real-mode interrupt vector table).
   If the vector at 0000:(intno*4) is non-zero, simulate hardware INT dispatch:
   push FLAGS, push CS, push IP, and jump to the handler in RAM.
   Returns 1 if dispatched, 0 if vector is 0000:0000 (not installed). */
static int dispatchViaIVT(int intno)
{
    DWord ivtAddr = (DWord)intno * 4;
    Word off = ram[ivtAddr + 0] | (ram[ivtAddr + 1] << 8);
    Word seg = ram[ivtAddr + 2] | (ram[ivtAddr + 3] << 8);
    if (seg == 0 && off == 0)
        return 0;  /* No handler installed */
    /* Simulate hardware INT: push flags, cs, ip; jump to handler */
    Word newSP = sp() - 2;
    DWord stackPhys = ((DWord)ss() << 4) + newSP;
    ram[stackPhys] = (Byte)getFlags(); ram[stackPhys+1] = (Byte)(getFlags()>>8);
    setSP(newSP);
    newSP -= 2; stackPhys = ((DWord)ss() << 4) + newSP;
    ram[stackPhys] = (Byte)cs(); ram[stackPhys+1] = (Byte)(cs()>>8);
    setSP(newSP);
    newSP -= 2; stackPhys = ((DWord)ss() << 4) + newSP;
    ram[stackPhys] = (Byte)getIP(); ram[stackPhys+1] = (Byte)(getIP()>>8);
    setSP(newSP);
    /* Clear IF and TF (standard INT behavior) */
    setFlags(getFlags() & ~0x0300);
    setCS(seg);
    setIP(off);
    return 1;
}

void handleInterrupt(struct exe *e, int intno)
{
    if (intno == 0x21 || intno == 0x20) {
        if (intno == 0x20) {
            /* INT 20h = terminate — handle via syscall so nested EXEC works */
            if (!e->handleSyscall(e, intno))
                exit(0);
            return;
        }
        if (!e->handleSyscall(e, intno))
            runtimeError("Unimplemented system call INT 0x%02x AH=0x%02x", intno, ah());
        return;
    }

    /* BIOS / DOS interrupts */
    switch (intno) {
    case 0x1a:
        /* Timer - dispatch as syscall */
        if (!e->handleSyscall(e, intno))
            runtimeError("Unimplemented BIOS call INT 0x%02x", intno);
        return;
    case 0x2f:
        /* DOS multiplex - dispatch as syscall */
        if (e->handleSyscall && e->handleSyscall(e, intno))
            return;
        runtimeError("Unimplemented INT 2Fh AX=0x%04x", ax());
        return;
    case INT0_DIV_ERROR:
        runtimeError("Divide by zero");
        return;
    case INT3_BREAKPOINT:
        /* On real DOS, INT 3 with no debugger is a no-op (IRET).  Some
           build tools (e.g. COMPRESS.COM) contain INT 3 instructions that
           are harmless on real hardware but fatal in an emulator. */
        return;
    case INT4_OVERFLOW:
        runtimeError("Overflow trap");
        return;
    default:
        /* Check if there's a user-installed handler in the IVT */
        if (dispatchViaIVT(intno))
            return;
        /* Try to handle as a DOS/BIOS syscall */
        if (e->handleSyscall && e->handleSyscall(e, intno))
            return;
        runtimeError("Unknown INT 0x%02x", intno);
    }
}

static void dumpExecutable(const char *path)
{
    struct _stat sbuf;
    int fd = _open(path, _O_RDONLY | _O_BINARY);
    if (fd < 0) { fprintf(stderr, "Can't open %s\n", path); exit(1); }
    if (_fstat(fd, &sbuf) < 0) { fprintf(stderr, "Can't stat %s\n", path); _close(fd); exit(1); }
    size_t filesize = (size_t)sbuf.st_size;
    unsigned char *buf = (unsigned char *)malloc(filesize);
    if (!buf) { fprintf(stderr, "Out of memory\n"); exit(1); }
    int n = _read(fd, buf, (unsigned int)filesize);
    _close(fd);
    if (n != (int)filesize) { fprintf(stderr, "Read error\n"); exit(1); }

    printf("File: %s (%zu bytes)\n", path, filesize);

    if (filesize < 2 || buf[0] != 'M' || buf[1] != 'Z') {
        printf("Type: COM (no MZ signature)\n");
        printf("Load: PSP:0100, size 0x%04X (%zu)\n", (unsigned)filesize, filesize);
        free(buf);
        exit(0);
    }

    struct image_dos_header *h = (struct image_dos_header *)buf;
    printf("Type: MZ EXE\n\n");
    printf("--- MZ Header ---\n");
    printf("  e_magic:    0x%04X ('MZ')\n", h->e_magic);
    printf("  e_cblp:     %5u   (bytes on last page)\n", h->e_cblp);
    printf("  e_cp:       %5u   (pages in file)\n", h->e_cp);
    printf("  e_crlc:     %5u   (relocation entries)\n", h->e_crlc);
    printf("  e_cparhdr:  %5u   (header paragraphs = 0x%X bytes)\n",
        h->e_cparhdr, h->e_cparhdr * 16);
    printf("  e_minalloc: %5u   (min extra paragraphs = 0x%X bytes)\n",
        h->e_minalloc, h->e_minalloc * 16);
    printf("  e_maxalloc: %5u   (max extra paragraphs = 0x%X bytes)\n",
        h->e_maxalloc, h->e_maxalloc * 16);
    printf("  e_ss:       0x%04X (relative SS)\n", h->e_ss);
    printf("  e_sp:       0x%04X (initial SP)\n", h->e_sp);
    printf("  e_csum:     0x%04X\n", h->e_csum);
    printf("  e_ip:       0x%04X (initial IP)\n", h->e_ip);
    printf("  e_cs:       0x%04X (relative CS)\n", h->e_cs);
    printf("  e_lfarlc:   0x%04X (reloc table offset)\n", h->e_lfarlc);
    printf("  e_ovno:     %5u   (overlay number)\n", h->e_ovno);
    printf("  e_lfanew:   0x%08X (new header offset)\n", h->e_lfanew);

    int headerLen = h->e_cparhdr * 16;
    int exeLen = ((h->e_cp - (h->e_cblp ? 1 : 0)) << 9) + h->e_cblp;
    int imageBytes = exeLen - headerLen;
    printf("\n--- Computed ---\n");
    printf("  Header size: %d bytes (0x%X)\n", headerLen, headerLen);
    printf("  EXE length:  %d bytes (0x%X)\n", exeLen, exeLen);
    printf("  Image size:  %d bytes (0x%X)\n", imageBytes, imageBytes);
    printf("  Image paras: %d (0x%X)\n", (imageBytes+15)/16, (imageBytes+15)/16);
    int bssBytes = h->e_minalloc * 16;
    printf("  BSS size:    %d bytes (0x%X)\n", bssBytes, bssBytes);
    printf("  Total mem:   %d bytes (image+BSS)\n", imageBytes + bssBytes);

    /* With load segment = PSP+0x10 = 0x1000 */
    unsigned imgSeg = 0x1000;
    printf("\n--- Load Map (PSP=0x0FF0, image=0x%04X) ---\n", imgSeg);
    printf("  CS:IP = %04X:%04X  (physical 0x%05X)\n",
        h->e_cs + imgSeg, h->e_ip,
        (h->e_cs + imgSeg) * 16 + h->e_ip);
    printf("  SS:SP = %04X:%04X  (physical 0x%05X)\n",
        h->e_ss + imgSeg, h->e_sp,
        (h->e_ss + imgSeg) * 16 + h->e_sp);
    unsigned endSeg = imgSeg + (imageBytes+15)/16 + h->e_minalloc;
    printf("  Program end: seg 0x%04X (physical 0x%05X)\n",
        endSeg, endSeg * 16);

    /* Detect EXEPACK */
    int isExepack = 0;
    if (h->e_crlc == 0 && h->e_ovno == 0) {
        /* EXEPACK: CS:IP points to decompression stub with 'RB' signature */
        int stubOff = headerLen + (h->e_cs * 16);
        /* Check for 'RB' at offset 0x10 or 0x12 (two known EXEPACK versions) */
        int rbOff = -1;
        if (stubOff + 0x14 <= (int)filesize) {
            if (buf[stubOff + 0x10] == 'R' && buf[stubOff + 0x11] == 'B')
                rbOff = 0x10;
            else if (buf[stubOff + 0x12] == 'R' && buf[stubOff + 0x13] == 'B')
                rbOff = 0x12;
        }
        if (rbOff >= 0) {
            isExepack = 1;
            printf("\n--- EXEPACK Detected (signature at stub+0x%X) ---\n", rbOff);
            printf("  Stub at file offset: 0x%X (CS=0x%04X relative)\n",
                stubOff, h->e_cs);
            unsigned rip = buf[stubOff]|(buf[stubOff+1]<<8);
            unsigned rcs = buf[stubOff+2]|(buf[stubOff+3]<<8);
            unsigned exepackSize = buf[stubOff+6]|(buf[stubOff+7]<<8);
            printf("  Real CS:IP = %04X:%04X\n", rcs, rip);
            printf("  Stub+reloc size: %u (0x%X)\n", exepackSize, exepackSize);
            printf("  Stub first bytes: ");
            for (int i = 0; i < 20 && stubOff+i < (int)filesize; i++)
                printf("%02X ", buf[stubOff+i]);
            printf("\n");
        }
    }

    /* Relocation table summary */
    if (h->e_crlc > 0) {
        printf("\n--- Relocations (%u entries at file offset 0x%X) ---\n",
            h->e_crlc, h->e_lfarlc);
        struct dos_reloc *r = (struct dos_reloc *)(buf + h->e_lfarlc);
        int show = h->e_crlc > 16 ? 16 : h->e_crlc;
        for (int i = 0; i < show; i++)
            printf("  [%3d] %04X:%04X\n", i, r[i].r_seg, r[i].r_offset);
        if (h->e_crlc > 16)
            printf("  ... (%u more)\n", h->e_crlc - 16);
    } else if (!isExepack) {
        printf("\n  No relocations (may be EXEPACK'd or .COM-style)\n");
    }

    /* Check for new EXE header (NE/LE/PE) */
    if (h->e_lfanew >= 0x40 && h->e_lfanew + 4 <= (unsigned)filesize) {
        unsigned char *ne = buf + h->e_lfanew;
        printf("\n--- Extended Header at 0x%X ---\n", h->e_lfanew);
        printf("  Signature: '%c%c' (0x%02X 0x%02X)\n",
            (ne[0] >= 0x20 && ne[0] < 0x7F) ? ne[0] : '.',
            (ne[1] >= 0x20 && ne[1] < 0x7F) ? ne[1] : '.',
            ne[0], ne[1]);
        if (ne[0] == 'N' && ne[1] == 'E')
            printf("  Type: NE (16-bit New Executable / OS/2 / Windows 3.x)\n");
        else if (ne[0] == 'L' && ne[1] == 'E')
            printf("  Type: LE (Linear Executable / OS/2 2.x / VxD)\n");
        else if (ne[0] == 'P' && ne[1] == 'E')
            printf("  Type: PE (Portable Executable / Win32)\n");
    }

    /* Hex dump of first 32 bytes of image */
    printf("\n--- Image bytes (first 32 at file offset 0x%X) ---\n", headerLen);
    for (int i = 0; i < 32 && headerLen + i < (int)filesize; i++) {
        printf("%02X ", buf[headerLen + i]);
        if ((i & 15) == 15) printf("\n");
    }
    printf("\n");

    /* Hex dump of bytes at CS:IP */
    int csipOff = headerLen + (h->e_cs * 16) + h->e_ip;
    if (csipOff != headerLen || h->e_cs != 0 || h->e_ip != 0) {
        printf("--- Bytes at CS:IP (file offset 0x%X) ---\n", csipOff);
        for (int i = 0; i < 32 && csipOff + i < (int)filesize; i++) {
            printf("%02X ", buf[csipOff + i]);
            if ((i & 15) == 15) printf("\n");
        }
        printf("\n");
    }

    free(buf);
    exit(0);
}

static void usage(const char *prog)
{
    fprintf(stderr, "demu - DOS Executable Emulator\n");
    fprintf(stderr, "Usage: %s [-v] [-d] [-shell] <program.exe|program.com> [args...]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -v      Verbose output (show load info)\n");
    fprintf(stderr, "  -d      Dump EXE header and exit\n");
    fprintf(stderr, "  -shell  Run a command shell (COMMAND.COM)\n");
    exit(1);
}

int main(int argc, char *argv[])
{
    struct exe e;
    int argstart = 1;

    memset(&e, 0, sizeof(e));

    /* Parse options */
    while (argstart < argc && argv[argstart][0] == '-') {
        if (strcmp(argv[argstart], "-v") == 0) {
            f_verbose = 1;
            argstart++;
        } else if (strcmp(argv[argstart], "-d") == 0) {
            f_dump = 1;
            argstart++;
        } else if (strcmp(argv[argstart], "-shell") == 0) {
            f_shell = 1;
            argstart++;
        } else if (strcmp(argv[argstart], "--") == 0) {
            argstart++;
            break;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[argstart]);
            usage(argv[0]);
        }
    }

    if (argstart >= argc)
        usage(argv[0]);

    if (f_dump) {
        dumpExecutable(argv[argstart]);
        return 0;
    }

    /* Record our own exe path so EXEC can re-invoke demu */
    GetModuleFileNameA(NULL, demuExePath, MAX_PATH);

    /* Check if the program is a natively-handled command (e.g. LIB.EXE).
       This lets us run native replacements directly from the command line,
       not just when called as a child process from within a DOS program. */
    {
        char cmdTail[512] = "";
        int pos = 0;
        for (int a = argstart + 1; a < argc; a++) {
            if (pos > 0) cmdTail[pos++] = ' ';
            int n = _snprintf(cmdTail + pos, sizeof(cmdTail) - pos, "%s", argv[a]);
            if (n > 0) pos += n;
        }
        int rc = runNativeCommand(argv[argstart], cmdTail);
        if (rc >= 0)
            return rc;
    }

    initMachine(&e);
    loadExecutableDOS(&e, argv[argstart], argc - argstart, &argv[argstart]);

    initExecute();
    for (;;) {
        executeInstruction();
    }
}