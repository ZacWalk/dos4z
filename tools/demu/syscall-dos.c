/* DOS system call handler for 8086 emulator - Windows/MSVC port */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <conio.h>
#include <sys/stat.h>
#include <share.h>
#include <windows.h>
#include "8086.h"
#include "exe.h"
#include "exec-dos.h"

extern int f_verbose;
extern int f_shell;
extern Word loadSegment;
extern Byte shadowRam[];

char demuExePath[MAX_PATH];  /* path to demu.exe itself */
static int lastChildExitCode;       /* exit code from last EXEC child */

static char* pathBuffers[2];
static int* fileDescriptors;
static int fileDescriptorCount = 6;
static unsigned char* fdLastWasCR;  /* per-fd: last byte read was \r */
static Word dtaSeg;    /* DTA segment set by INT 21h/1Ah */
static Word dtaOff;    /* DTA offset set by INT 21h/1Ah */

/* DOS Memory Control Block (MCB) arena allocator.
   MCB is 1 paragraph (16 bytes) at the start of each block:
     Byte 0: 'M' (0x4D) = chain continues, 'Z' (0x5A) = last block
     Word 1: owner PSP segment (0 = free)
     Word 3: size in paragraphs (not counting the MCB itself)
*/
static Word mcbHead;      /* segment of first MCB */
static Word pspSegment;   /* cached PSP segment */

void mcbWrite(Word seg, Byte type, Word owner, Word size)
{
    DWord a = (DWord)seg << 4;
    ram[a+0] = type;
    ram[a+1] = (Byte)owner;
    ram[a+2] = (Byte)(owner >> 8);
    ram[a+3] = (Byte)size;
    ram[a+4] = (Byte)(size >> 8);
}

Byte  mcbType(Word seg)  { DWord a=(DWord)seg<<4; return ram[a]; }
Word  mcbOwner(Word seg) { DWord a=(DWord)seg<<4; return ram[a+1]|(ram[a+2]<<8); }
Word  mcbSize(Word seg)  { DWord a=(DWord)seg<<4; return ram[a+3]|(ram[a+4]<<8); }

/* Initialize the MCB chain. Called after loading the program.
   progSeg  = PSP segment (program owns from PSP to freeEnd).
   freeEnd  = last usable paragraph (e.g. 0x9FFF).
   In real DOS, the MCB for the program is at PSP-1. */
void initDOSMemory(Word progSeg, Word freeEnd)
{
    extern Byte shadowRam[];
    extern int envSegment;  /* set by write_environ in loader-dos.c */
    pspSegment = progSeg;
    dtaSeg = progSeg;
    dtaOff = 0x80;  /* Default DTA is at PSP:0x80 */

    /* Build a proper MCB chain like real DOS:
       MCB at envSeg-1: env block (owner=PSP, size=0x100, type='M')
       MCB at PSP-1:    program block (owner=PSP, size=rest, type='Z') */
    Word envMcb = (Word)envSegment - 1;
    Word progMcb = progSeg - 1;
    mcbHead = envMcb;

    /* Environment MCB: size = distance from envSeg to progMcb - 1 */
    Word envSize = progMcb - (Word)envSegment;
    mcbWrite(envMcb, 'M', progSeg, envSize);

    /* Program MCB at PSP-1 */
    Word totalSize = freeEnd - progSeg;
    mcbWrite(progMcb, 'Z', progSeg, totalSize);

    /* Mark everything from envMcb to freeEnd as accessible */
    DWord startPhys = (DWord)envMcb << 4;
    DWord physEnd = (DWord)freeEnd << 4;
    if (physEnd > RAMSIZE) physEnd = RAMSIZE;
    for (DWord i = startPhys; i < physEnd && i < RAMSIZE; i++)
        shadowRam[i] |= fRead | fWrite;
}

Word dosAllocMem(Word paragraphs)
{
    /* First-fit search through MCB chain */
    Word seg = mcbHead;
    Word bestSeg = 0;
    Word largest = 0;
    (void)bestSeg;
    while (1) {
        Byte type = mcbType(seg);
        Word owner = mcbOwner(seg);
        Word size = mcbSize(seg);
        if (owner == 0 && size >= paragraphs) {
            /* Found a free block big enough */
            if (size > paragraphs + 1) {
                /* Split: create new free MCB after allocated portion */
                Word newMcb = seg + 1 + paragraphs;
                mcbWrite(newMcb, type, 0, size - paragraphs - 1);
                mcbWrite(seg, 'M', pspSegment, paragraphs);
            } else {
                /* Use entire block */
                mcbWrite(seg, type, pspSegment, size);
            }
            /* Mark allocated block accessible in shadow RAM */
            {
                DWord bStart = ((DWord)(seg + 1)) << 4;
                DWord bEnd   = ((DWord)(seg + 1 + paragraphs)) << 4;
                if (bEnd > RAMSIZE) bEnd = RAMSIZE;
                for (DWord a = bStart; a < bEnd; a++)
                    shadowRam[a] |= fRead | fWrite;
            }
            return seg + 1;  /* return segment after MCB */
        }
        if (owner == 0 && size > largest)
            largest = size;
        if (type == 'Z')
            break;
        seg = seg + 1 + size;
    }
    /* Allocation failed - return largest available in BX */
    setBX(largest);
    return 0;
}

int dosFreeMem(Word seg)
{
    /* seg points to the block (MCB is at seg-1) */
    Word mcb = seg - 1;
    if (mcb < mcbHead) return 9;  /* invalid block */
    mcbWrite(mcb, mcbType(mcb), 0, mcbSize(mcb));
    /* Coalesce forward with next block(s) if also free */
    while (mcbType(mcb) == 'M') {
        Word next = mcb + 1 + mcbSize(mcb);
        if (mcbOwner(next) == 0) {
            Word combined = mcbSize(mcb) + 1 + mcbSize(next);
            mcbWrite(mcb, mcbType(next), 0, combined);
        } else {
            break;
        }
    }
    return 0;
}

int dosResizeMem(Word seg, Word newSize)
{
    Word mcb = seg - 1;
    if (mcb < mcbHead) return 9;
    Word curSize = mcbSize(mcb);
    if (newSize <= curSize) {
        /* Shrink: free the tail */
        if (curSize > newSize + 1) {
            Word newMcb = mcb + 1 + newSize;
            mcbWrite(newMcb, mcbType(mcb), 0, curSize - newSize - 1);
            mcbWrite(mcb, 'M', mcbOwner(mcb), newSize);
        }
        return 0;
    }
    /* Try to grow into next block */
    if (mcbType(mcb) == 'M') {
        Word next = mcb + 1 + curSize;
        if (mcbOwner(next) == 0) {
            Word avail = curSize + 1 + mcbSize(next);
            if (avail >= newSize) {
                Word grownTo = (avail > newSize + 1) ? newSize : avail;
                if (avail > newSize + 1) {
                    Word newMcb = mcb + 1 + newSize;
                    mcbWrite(newMcb, mcbType(next), 0, avail - newSize - 1);
                    mcbWrite(mcb, 'M', mcbOwner(mcb), newSize);
                } else {
                    mcbWrite(mcb, mcbType(next), mcbOwner(mcb), avail);
                }
                /* Mark newly grown area accessible in shadow RAM */
                {
                    DWord gStart = ((DWord)(mcb + 1 + curSize)) << 4;
                    DWord gEnd   = ((DWord)(mcb + 1 + grownTo)) << 4;
                    if (gEnd > RAMSIZE) gEnd = RAMSIZE;
                    for (DWord a = gStart; a < gEnd; a++)
                        shadowRam[a] |= fRead | fWrite;
                }
                return 0;
            }
        }
    }
    /* Can't grow - report largest available */
    setBX(curSize);
    return 8;  /* insufficient memory */
}

static void* sysalloc(size_t bytes)
{
    void* r = malloc(bytes);
    if (r == 0)
        runtimeError("Out of memory\n");
    return r;
}

static void init(void)
{
    pathBuffers[0] = (char*)sysalloc(0x10000);
    pathBuffers[1] = (char*)sysalloc(0x10000);

    fileDescriptors = (int*)sysalloc(6*sizeof(int));
    fdLastWasCR = (unsigned char*)sysalloc(6*sizeof(unsigned char));
    memset(fdLastWasCR, 0, 6*sizeof(unsigned char));
    fileDescriptors[0] = _fileno(stdin);
    fileDescriptors[1] = _fileno(stdout);
    fileDescriptors[2] = _fileno(stderr);
    fileDescriptors[3] = _fileno(stdout);
    fileDescriptors[4] = _fileno(stdout);
    fileDescriptors[5] = -1;

    /* Set stdout/stderr to binary mode to avoid CR/LF translation */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
}

static char* initString(Word offset, int seg, int dowrite, int buffer, int bytes)
{
    for (int i = 0; i < bytes; ++i) {
        char p;
        if (dowrite) {
            p = pathBuffers[buffer][i];
            ram[physicalAddress(offset + i, seg, true)] = p;
        }
        else {
            p = ram[physicalAddress(offset + i, seg, false)];
            pathBuffers[buffer][i] = p;
        }
        if (p == 0 && bytes == 0x10000)
            break;
    }
    if (!dowrite)
        pathBuffers[buffer][0xffff] = 0;
    return pathBuffers[buffer];
}

static char* dsdxparms(int dowrite, int bytes)
{
    return initString(dx(), DS, dowrite, 0, bytes);
}

static char *dsdx(void)
{
    return dsdxparms(false, 0x10000);
}

static int dosError(int e)
{
    switch (e) {
        case ENOENT:  return 2;   /* File not found */
        case EACCES:  return 5;   /* Access denied */
        case EEXIST:  return 80;  /* File exists */
        case EBADF:   return 6;   /* Invalid handle */
        case ENOMEM:  return 8;   /* Insufficient memory */
        case EINVAL:  return 87;  /* Invalid parameter */
        default:
            if (f_verbose)
                fprintf(stderr, "DOS error mapping: errno=%d (%s)\n", e, strerror(e));
            return 2;  /* Default: file not found */
    }
}

static int getDescriptor(void)
{
    for (int i = 0; i < fileDescriptorCount; ++i)
        if (fileDescriptors[i] == -1)
            return i;
    int newCount = fileDescriptorCount << 1;
    int* newDescriptors = (int*)sysalloc(newCount*sizeof(int));
    unsigned char* newLastWasCR = (unsigned char*)sysalloc(newCount*sizeof(unsigned char));
    for (int i = 0; i < fileDescriptorCount; ++i) {
        newDescriptors[i] = fileDescriptors[i];
        newLastWasCR[i] = fdLastWasCR[i];
    }
    for (int i = fileDescriptorCount; i < newCount; ++i) {
        newDescriptors[i] = -1;
        newLastWasCR[i] = 0;
    }
    free(fileDescriptors);
    free(fdLastWasCR);
    int oldCount = fileDescriptorCount;
    fileDescriptorCount = newCount;
    fileDescriptors = newDescriptors;
    fdLastWasCR = newLastWasCR;
    return oldCount;
}

int checkStackDOS(struct exe *e)
{
    return (e->t_stackLow && ((DWord)ss() << 4) + sp() <= e->t_stackLow);
}

static int SysWrite(struct exe *e, int fd, char *buf, size_t n)
{
    return _write(fd, buf, (unsigned int)n);
}

/* ── Accessor functions for exec-dos.c ───────────────────────────────── */

Word getMcbHead(void)      { return mcbHead; }
Word getPspSegment(void)   { return pspSegment; }
void setMcbHead(Word seg)  { mcbHead = seg; }
void setPspSegment(Word s) { pspSegment = s; }

Word getDtaSeg(void)       { return dtaSeg; }
Word getDtaOff(void)       { return dtaOff; }
void setDtaSeg(Word seg)   { dtaSeg = seg; }
void setDtaOff(Word off)   { dtaOff = off; }

int *getFds(void)              { return fileDescriptors; }
int  getFdCount(void)          { return fileDescriptorCount; }
unsigned char *getFdCR(void)   { return fdLastWasCR; }
void setFds(int *fds, unsigned char *cr, int count) {
    free(fileDescriptors); free(fdLastWasCR);
    fileDescriptors = fds; fdLastWasCR = cr; fileDescriptorCount = count;
}
int getFreeFd(void)            { return getDescriptor(); }

int  getLastChildExitCode(void) { return lastChildExitCode; }
void setLastChildExitCode(int rc) { lastChildExitCode = rc; }

/* ═════════════════════════════════════════════════════════════════════ */

int handleSyscallDOS(struct exe *e, int intno)
{
    int fileDescriptor;
    char *p, *addr;
    DWord sysdata;
    static int once = 0;

    if (!once) {
        init();
        once = 1;
    }

    if (f_verbose && (intno == 0x21 || intno == 0x2f))
        fprintf(stderr, "INT %02Xh AH=%02X AX=%04X BX=%04X CX=%04X DX=%04X DS=%04X CS:IP=%04X:%04X\n",
            intno, ah(), ax(), bx(), cx(), dx(), ds(), cs(), getIP());

    /* INT 20h = terminate (used by COM files and PSP:0000) */
    if (intno == 0x20) {
        sysExit(e, 0);
        return 1;
    }

    /* INT 2Fh = DOS multiplex interrupt */
    if (intno == 0x2f) {
        switch (ah()) {
        case 0x12:  /* DOS internal services */
            switch (al()) {
            case 0x2e:  /* MSG_RETRIEVAL - get/set message block address (func 46 decimal) */
                /* DL even = get (return ES:DI), DL odd = set (store ES:DI).
                   We don't have a real message database, so for "get" return
                   a pointer to a small zero-filled area; for "set" just ignore. */
                if ((dl() & 1) == 0) {
                    /* "get" — point ES:DI at PSP:00F0 (unused area), zeroed */
                    Word seg = loadSegment - 0x10;
                    DWord a = ((DWord)seg << 4) + 0x00F0;
                    memset(&ram[a], 0, 12);
                    setES(seg);
                    setDI(0x00F0);
                }
                /* "set" — silently ignore */
                break;
            default:
                if (f_verbose)
                    fprintf(stderr, "  INT 2Fh/12h unhandled AL=%02Xh\n", al());
                setCF(true);
                break;
            }
            break;
        default:
            /* Unknown multiplex — return AL=0 (not installed) */
            if (f_verbose)
                fprintf(stderr, "  INT 2Fh unhandled AH=%02Xh AL=%02Xh\n", ah(), al());
            setAL(0);
            break;
        }
        return 1;
    }

    switch (intno << 8 | ah()) {
        case 0x1a00:    /* Get system time (BIOS) */
            sysdata = es();
            setES(0);
            setDX(readWordSeg(0x046c, ES));
            setCX(readWordSeg(0x046e, ES));
            setAL(readByte(0x0470, ES));
            setES((Word)sysdata);
            break;

        case 0x2100:    /* Terminate program */
            sysExit(e, 0);
            break;

        case 0x2101:    /* Read character with echo */
            {
                int ch;
                if (_isatty(fileDescriptors[0])) {
                    ch = _getch();
                } else {
                    unsigned char b;
                    if (_read(fileDescriptors[0], &b, 1) == 1) ch = b;
                    else ch = 0x1A; /* EOF = Ctrl-Z */
                }
                setAL((Byte)ch);
            }
            break;

        case 0x2102:    /* Display character */
            {
                char c = (char)dl();
                _write(fileDescriptors[1], &c, 1);
            }
            break;

        case 0x2106:    /* Direct console I/O */
            if (dl() == 0xFF) {
                /* Input */
                if (_isatty(fileDescriptors[0])) {
                    if (_kbhit()) {
                        setAL((Byte)_getch());
                        setCF(false);
                        setFlags(getFlags() & ~0x40);
                    } else {
                        setAL(0);
                        setFlags(getFlags() | 0x40);
                    }
                } else {
                    unsigned char b;
                    if (_read(fileDescriptors[0], &b, 1) == 1) {
                        setAL(b);
                        setCF(false);
                        setFlags(getFlags() & ~0x40);
                    } else {
                        setAL(0);
                        setFlags(getFlags() | 0x40);
                    }
                }
            } else {
                char c = (char)dl();
                _write(fileDescriptors[1], &c, 1);
            }
            break;

        case 0x2108:    /* Read character without echo */
            {
                int ch;
                if (_isatty(fileDescriptors[0])) {
                    ch = _getch();
                } else {
                    unsigned char b;
                    if (_read(fileDescriptors[0], &b, 1) == 1) ch = b;
                    else ch = 0x1A;
                }
                setAL((Byte)ch);
            }
            break;

        case 0x2109:    /* Display string (terminated by '$') */
            addr = dsdx();
            p = strchr(addr, '$');
            if (p) SysWrite(e, fileDescriptors[1], addr, p-addr);
            break;

        case 0x210a:    /* Buffered keyboard input */
            {
                Word bufAddr = dx();
                Byte maxLen = readByte(bufAddr, DS);
                Byte count = 0;
                int ch;
                int isTty = _isatty(fileDescriptors[0]);
                while (count < maxLen - 1) {
                    if (isTty) {
                        ch = _getch();
                    } else {
                        unsigned char b;
                        if (_read(fileDescriptors[0], &b, 1) == 1) ch = b;
                        else break;
                    }
                    if (ch == '\r' || ch == '\n') break;
                    writeByte((Byte)ch, bufAddr + 2 + count, DS);
                    count++;
                }
                writeByte('\r', bufAddr + 2 + count, DS);
                writeByte(count, bufAddr + 1, DS);
            }
            break;

        case 0x210b:    /* Check standard input status */
            if (_isatty(fileDescriptors[0])) {
                setAL(_kbhit() ? 0xFF : 0x00);
            } else {
                /* Redirected stdin — always report data available */
                setAL(0xFF);
            }
            break;

        case 0x2119:    /* Get current default drive */
        {
            char cwdBuf[MAX_PATH];
            if (_getcwd(cwdBuf, sizeof(cwdBuf)) && cwdBuf[1] == ':') {
                char ch = cwdBuf[0];
                if (ch >= 'a' && ch <= 'z') ch -= 32;
                setAL(ch - 'A');
            } else {
                setAL(2);  /* fallback: C: */
            }
            break;
        }

        case 0x211a:    /* Set disk transfer address */
            /* Store DTA address */
            dtaSeg = ds();
            dtaOff = dx();
            if (f_verbose) fprintf(stderr, "  SetDTA: %04X:%04X\n", dtaSeg, dtaOff);
            break;

        case 0x2125:    /* Set interrupt vector */
        {
            /* Write vector to IVT at 0000:(AL*4) */
            DWord ivtAddr = (DWord)al() * 4;
            ram[ivtAddr + 0] = (Byte)dx();
            ram[ivtAddr + 1] = (Byte)(dx() >> 8);
            ram[ivtAddr + 2] = (Byte)ds();
            ram[ivtAddr + 3] = (Byte)(ds() >> 8);
            break;
        }

        case 0x2130:    /* Get DOS version */
            setAX(f_shell ? 0x0004 : 0x1403);  /* 4.00 in shell mode, 3.20 otherwise */
            setBX(0xff00);
            setCX(0);
            break;

        case 0x2133:    /* Get/Set Ctrl-Break checking */
            if (al() == 0) {
                setDL(0);  /* Ctrl-Break checking is OFF */
            }
            break;

        case 0x2135:    /* Get interrupt vector */
        {
            /* Read vector from IVT at 0000:(AL*4) */
            DWord ivtAddr = (DWord)al() * 4;
            Word off = ram[ivtAddr + 0] | (ram[ivtAddr + 1] << 8);
            Word seg = ram[ivtAddr + 2] | (ram[ivtAddr + 3] << 8);
            setES(seg);
            setBX(off);
            break;
        }

        case 0x2137:    /* Get Disk Free Space - undocumented Switchar */
            if (al() == 0) {
                setDL('/');  /* Switch character */
            }
            break;

        case 0x2138:    /* Get Country Info - return US defaults */
            setCF(false);
            break;

        case 0x2129:    /* Parse Filename into FCB */
        {
            /* AL = parsing flags:
               bit 0: skip leading separators
               bit 1: set drive byte only if specified in string
               bit 2: set filename only if specified
               bit 3: set extension only if specified */
            Byte flags = al();
            Word srcOff = si();
            Word dstOff = di();

            DWord srcPhys = ((DWord)ds() << 4) + srcOff;
            DWord dstPhys = ((DWord)es() << 4) + dstOff;

            /* Skip leading separators if flag bit 0 set */
            if (flags & 1) {
                while (1) {
                    Byte c = ram[srcPhys];
                    if (c == ' ' || c == '\t' || c == ';' || c == ',' || c == '=')
                        { srcPhys++; srcOff++; }
                    else
                        break;
                }
            }

            /* Initialize FCB: drive=0, filename=spaces, extension=spaces */
            if (!(flags & 2)) ram[dstPhys + 0] = 0;
            for (int i = 1; i <= 8; i++)
                if (!(flags & 4)) ram[dstPhys + i] = ' ';
            for (int i = 9; i <= 11; i++)
                if (!(flags & 8)) ram[dstPhys + i] = ' ';

            Byte result = 0;  /* 0=no wildcards, 1=wildcards, ff=bad drive */

            /* Parse drive letter */
            if (ram[srcPhys + 1] == ':') {
                Byte drv = ram[srcPhys];
                if (drv >= 'a' && drv <= 'z') drv -= 0x20;
                if (drv >= 'A' && drv <= 'Z') {
                    ram[dstPhys + 0] = drv - 'A' + 1;
                    srcPhys += 2; srcOff += 2;
                } else {
                    result = 0xFF;
                }
            }

            /* Parse filename (up to 8 chars) */
            {
                int pos = 0;
                while (pos < 8) {
                    Byte c = ram[srcPhys];
                    if (c == 0 || c == '.' || c == ' ' || c == '/' || c == '\\' ||
                        c == ':' || c == ';' || c == ',' || c == '=' || c == '\t' ||
                        c == '+' || c == '<' || c == '>' || c == '|' || c == '[' ||
                        c == ']' || c == '"' || c == '\r' || c == '\n') break;
                    if (c == '*') {
                        while (pos < 8) { ram[dstPhys + 1 + pos] = '?'; pos++; }
                        result = 1;
                        srcPhys++; srcOff++;
                        break;
                    }
                    if (c == '?') result = 1;
                    if (c >= 'a' && c <= 'z') c -= 0x20;
                    ram[dstPhys + 1 + pos] = c;
                    pos++;
                    srcPhys++; srcOff++;
                }
                /* Skip extra chars beyond 8 */
                while (1) {
                    Byte c = ram[srcPhys];
                    if (c == 0 || c == '.' || c == ' ' || c == '/' || c == '\\' ||
                        c == ':' || c == ';' || c == ',' || c == '=' || c == '\t' ||
                        c == '\r' || c == '\n') break;
                    srcPhys++; srcOff++;
                }
            }

            /* Parse extension */
            if (ram[srcPhys] == '.') {
                srcPhys++; srcOff++;
                int pos = 0;
                while (pos < 3) {
                    Byte c = ram[srcPhys];
                    if (c == 0 || c == '.' || c == ' ' || c == '/' || c == '\\' ||
                        c == ':' || c == ';' || c == ',' || c == '=' || c == '\t' ||
                        c == '+' || c == '<' || c == '>' || c == '|' || c == '[' ||
                        c == ']' || c == '"' || c == '\r' || c == '\n') break;
                    if (c == '*') {
                        while (pos < 3) { ram[dstPhys + 9 + pos] = '?'; pos++; }
                        result = 1;
                        srcPhys++; srcOff++;
                        break;
                    }
                    if (c == '?') result = 1;
                    if (c >= 'a' && c <= 'z') c -= 0x20;
                    ram[dstPhys + 9 + pos] = c;
                    pos++;
                    srcPhys++; srcOff++;
                }
            }

            /* Mark FCB area as written in shadow RAM */
            for (int i = 0; i < 12; i++)
                shadowRam[dstPhys + i] |= fRead | fWrite;

            setAL(result);
            setSI(srcOff);
            setCF(false);
            if (f_verbose) {
                char fn[13];
                for (int i = 0; i < 12; i++) fn[i] = (char)ram[dstPhys + i];
                fn[12] = 0;
                fprintf(stderr, "  ParseFCB: drv=%d fn='%.8s' ext='%.3s' result=%d\n",
                        fn[0], fn+1, fn+9, result);
            }
            break;
        }

        case 0x212a:    /* Get Date — return fixed date for deterministic builds */
        {
            setCX(1988);      /* year */
            setDH(6);         /* month */
            setDL(15);        /* day */
            setAL(3);         /* Wednesday */
            break;
        }

        case 0x212c:    /* Get Time — use real Windows time so that CL.EXE
                           generates unique temp-file prefixes across runs. */
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            setCH((Byte)st.wHour);
            setCL((Byte)st.wMinute);
            setDH((Byte)st.wSecond);
            setDL((Byte)(st.wMilliseconds / 10));
            break;
        }

        case 0x2139:    /* Create directory */
            if (_mkdir(dsdx()) == 0)
                setCF(false);
            else {
                setCF(true);
                setAX(dosError(errno));
            }
            break;

        case 0x213a:    /* Remove directory */
            if (_rmdir(dsdx()) == 0)
                setCF(false);
            else {
                setCF(true);
                setAX(dosError(errno));
            }
            break;

        case 0x213b:    /* Change directory */
            if (_chdir(dsdx()) == 0)
                setCF(false);
            else {
                setCF(true);
                setAX(dosError(errno));
            }
            break;

        case 0x213c:    /* Create file */
            if (_sopen_s(&fileDescriptor, dsdx(), _O_CREAT | _O_TRUNC | _O_RDWR | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0)
                fileDescriptor = -1;
            if (fileDescriptor != -1) {
                setCF(false);
                int guestDescriptor = getDescriptor();
                setAX(guestDescriptor);
                fileDescriptors[guestDescriptor] = fileDescriptor;
                fdLastWasCR[guestDescriptor] = 0;
            }
            else {
                setCF(true);
                setAX(dosError(errno));
            }
            break;

        case 0x213d:    /* Open file */
        {
            char *openPath = dsdx();
            if (f_verbose) fprintf(stderr, "  Open: \"%s\" mode=%d\n", openPath, al() & 3);
            int openFlags = (al() & 3) == 0 ? _O_RDONLY | _O_BINARY :
                            (al() & 3) == 1 ? _O_WRONLY | _O_BINARY :
                                              _O_RDWR | _O_BINARY;
            if (_sopen_s(&fileDescriptor, openPath, openFlags, _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0)
                fileDescriptor = -1;
            if (fileDescriptor != -1) {
                setCF(false);
                int gd = getDescriptor();
                setAX(gd);
                fileDescriptors[gd] = fileDescriptor;
                fdLastWasCR[gd] = 0;
                if (f_verbose) fprintf(stderr, "  Open: fd=%d (host %d)\n", gd, fileDescriptor);
            }
            else {
                setCF(true);
                setAX(dosError(errno));
                if (f_verbose) fprintf(stderr, "  Open: FAILED errno=%d\n", errno);
            }
        }
            break;

        case 0x213e:    /* Close file */
            if (bx() >= (Word)fileDescriptorCount || fileDescriptors[bx()] == -1) {
                setCF(true);
                setAX(6);  /* Invalid handle */
                break;
            }
            fileDescriptor = fileDescriptors[bx()];
            if (fileDescriptor > 2 && _close(fileDescriptor) != 0) {
                setCF(true);
                setAX(dosError(errno));
            }
            else {
                fileDescriptors[bx()] = -1;
                fdLastWasCR[bx()] = 0;
                setCF(false);
            }
            break;

        case 0x213f:    /* Read file */
            if (bx() >= (Word)fileDescriptorCount || fileDescriptors[bx()] == -1) {
                setCF(true);
                setAX(6);  /* Invalid handle */
                break;
            }
            fileDescriptor = fileDescriptors[bx()];
            if (fileDescriptor == _fileno(stdin) && _isatty(fileDescriptor)) {
                /* Console input — read one line */
                int count = 0;
                int maxread = cx();
                int ch;
                while (count < maxread) {
                    ch = _getch();
                    if (ch == '\r') {
                        pathBuffers[0][count++] = '\r';
                        if (count < maxread)
                            pathBuffers[0][count++] = '\n';
                        _putch('\r');
                        _putch('\n');
                        break;
                    }
                    pathBuffers[0][count++] = (char)ch;
                    _putch(ch);
                }
                dsdxparms(true, count);
                setCF(false);
                setAX(count);
            } else {
                /* Direct file read — no translation.
                   Source files must have CRLF line endings (convert before build). */
                long prePos = _tell(fileDescriptor);
                sysdata = _read(fileDescriptor, pathBuffers[0], cx());
                if (f_verbose && cx() >= 0x100) {
                    long postPos = _tell(fileDescriptor);
                    fprintf(stderr, "    Read: fd=%d(host %d) req=%d got=%d pos=%ld->%ld first=0x%02X\n",
                            bx(), fileDescriptor, cx(), (int)sysdata, prePos, postPos,
                            sysdata > 0 ? (unsigned char)pathBuffers[0][0] : 0);
                }
                if (sysdata == (DWord)-1) {
                    setCF(true);
                    setAX(dosError(errno));
                }
                else {
                    dsdxparms(true, (int)sysdata);
                    setCF(false);
                    setAX((Word)sysdata);
                }
            }
            break;

        case 0x2140:    /* Write file */
            if (bx() >= (Word)fileDescriptorCount || fileDescriptors[bx()] == -1) {
                setCF(true);
                setAX(6);  /* Invalid handle */
                if (f_verbose) fprintf(stderr, "  Write: invalid handle %d\n", bx());
                break;
            }
            fileDescriptor = fileDescriptors[bx()];
            if (f_verbose) {
                char *wdata = dsdxparms(false, cx());
                fprintf(stderr, "  Write: fd=%d(host %d) len=%d \"%.40s\"\n",
                    bx(), fileDescriptor, cx(), wdata);
                sysdata = SysWrite(e, fileDescriptor, wdata, cx());
            } else {
                sysdata = SysWrite(e, fileDescriptor, dsdxparms(false, cx()), cx());
            }
            if (sysdata == (DWord)-1) {
                setCF(true);
                setAX(dosError(errno));
            }
            else {
                setCF(false);
                setAX((Word)sysdata);
            }
            break;

        case 0x2141:    /* Delete file */
        {
            char *delPath = dsdx();
            if (f_verbose) fprintf(stderr, "  Delete: \"%s\"\n", delPath);
            if (DeleteFileA(delPath))
                setCF(false);
            else {
                DWORD winErr = GetLastError();
                setCF(true);
                setAX(dosError(errno));
                if (f_verbose) fprintf(stderr, "  Delete: FAILED win32err=%lu errno=%d\n", winErr, errno);
            }
        }
            break;

        case 0x2142:    /* Seek file */
            if (bx() >= (Word)fileDescriptorCount || fileDescriptors[bx()] == -1) {
                setCF(true);
                setAX(6);
                break;
            }
            fileDescriptor = fileDescriptors[bx()];
            sysdata = _lseek(fileDescriptor, (long)((cx() << 16) + dx()), al());
            if (f_verbose)
                fprintf(stderr, "    Seek: fd=%d(host %d) origin=%d offset=%ld result=%ld\n",
                        bx(), fileDescriptor, al(), (long)((cx() << 16) + dx()), (long)sysdata);
            if (sysdata != (DWord)-1) {
                setCF(false);
                setDX((Word)(sysdata >> 16));
                setAX((Word)sysdata);
            }
            else {
                setCF(true);
                setAX(dosError(errno));
            }
            break;

        case 0x2143:    /* Get/Set file attributes */
            if (al() == 0) {
                /* Get attributes */
                DWORD attrs = GetFileAttributesA(dsdx());
                if (attrs != INVALID_FILE_ATTRIBUTES) {
                    Word dosAttr = 0;
                    if (attrs & FILE_ATTRIBUTE_READONLY)  dosAttr |= 0x01;
                    if (attrs & FILE_ATTRIBUTE_HIDDEN)    dosAttr |= 0x02;
                    if (attrs & FILE_ATTRIBUTE_SYSTEM)    dosAttr |= 0x04;
                    if (attrs & FILE_ATTRIBUTE_DIRECTORY) dosAttr |= 0x10;
                    if (attrs & FILE_ATTRIBUTE_ARCHIVE)   dosAttr |= 0x20;
                    setCX(dosAttr);
                    setCF(false);
                } else {
                    setCF(true);
                    setAX(2); /* File not found */
                }
            } else if (al() == 1) {
                /* Set attributes */
                DWORD attrs = 0;
                if (cx() & 0x01) attrs |= FILE_ATTRIBUTE_READONLY;
                if (cx() & 0x02) attrs |= FILE_ATTRIBUTE_HIDDEN;
                if (cx() & 0x04) attrs |= FILE_ATTRIBUTE_SYSTEM;
                if (cx() & 0x20) attrs |= FILE_ATTRIBUTE_ARCHIVE;
                if (attrs == 0) attrs = FILE_ATTRIBUTE_NORMAL;
                if (SetFileAttributesA(dsdx(), attrs)) {
                    setCF(false);
                } else {
                    setCF(true);
                    setAX(5); /* Access denied */
                }
            }
            break;

        case 0x2144:    /* IOCTL */
            if (al() == 0) {
                /* Get device information */
                if (bx() >= (Word)fileDescriptorCount || fileDescriptors[bx()] == -1) {
                    setCF(true);
                    setAX(6);
                    break;
                }
                fileDescriptor = fileDescriptors[bx()];
                if (_isatty(fileDescriptor)) {
                    setDX(0x80);    /* Character device */
                    setCF(false);
                } else {
                    setDX(0);       /* Block device (file) */
                    setCF(false);
                }
            } else if (al() == 1) {
                /* Set device information - ignore */
                setCF(false);
            } else if (al() == 7) {
                /* Get output status */
                setCF(false);
                setAL(0xFF);    /* Ready */
            } else if (al() == 8) {
                /* Is device removable? */
                setCF(false);
                setAX(1);       /* Not removable */
            } else {
                if (f_verbose)
                    fprintf(stderr, "Unimplemented IOCTL 0x%02x\n", al());
                setCF(true);
                setAX(1);       /* Invalid function */
            }
            break;

        case 0x2145:    /* Duplicate file handle */
            if (bx() >= (Word)fileDescriptorCount || fileDescriptors[bx()] == -1) {
                setCF(true);
                setAX(6);
                break;
            }
            {
                int newFd = _dup(fileDescriptors[bx()]);
                if (newFd != -1) {
                    int gd = getDescriptor();
                    fileDescriptors[gd] = newFd;
                    setAX(gd);
                    setCF(false);
                } else {
                    setCF(true);
                    setAX(dosError(errno));
                }
            }
            break;

        case 0x2146:    /* Force duplicate file handle */
            if (bx() >= (Word)fileDescriptorCount || fileDescriptors[bx()] == -1) {
                setCF(true);
                setAX(6);
                break;
            }
            {
                /* Ensure cx() slot exists */
                while (cx() >= (Word)fileDescriptorCount) {
                    int newCount = fileDescriptorCount << 1;
                    int *nd = (int*)sysalloc(newCount * sizeof(int));
                    for (int i = 0; i < fileDescriptorCount; i++) nd[i] = fileDescriptors[i];
                    for (int i = fileDescriptorCount; i < newCount; i++) nd[i] = -1;
                    free(fileDescriptors);
                    fileDescriptorCount = newCount;
                    fileDescriptors = nd;
                }
                if (fileDescriptors[cx()] != -1 && fileDescriptors[cx()] >= 5)
                    _close(fileDescriptors[cx()]);
                int newFd = _dup(fileDescriptors[bx()]);
                if (newFd != -1) {
                    fileDescriptors[cx()] = newFd;
                    setCF(false);
                } else {
                    setCF(true);
                    setAX(dosError(errno));
                }
            }
            break;

        case 0x2147:    /* Get current directory */
            if (_getcwd(pathBuffers[0], 64) != 0) {
                setCF(false);
                /* Skip drive letter and colon and backslash */
                p = pathBuffers[0];
                if (p[0] && p[1] == ':') {
                    p += 2;
                    if (*p == '\\' || *p == '/') p++;
                }
                /* Copy to DS:SI */
                for (int i = 0; ; i++) {
                    ram[physicalAddress(si() + i, DS, true)] = p[i];
                    if (p[i] == 0) break;
                }
                setCF(false);
            }
            else {
                setCF(true);
                setAX(dosError(errno));
            }
            break;

        case 0x2148:    /* Allocate memory */
        {
            Word req = bx();
            Word seg = dosAllocMem(req);
            if (seg) {
                if (f_verbose)
                    fprintf(stderr, "  Alloc req=%04X -> seg=%04X\n", req, seg);
                setCF(false);
                setAX(seg);
            } else {
                if (f_verbose)
                    fprintf(stderr, "  Alloc req=%04X FAIL maxAvail=%04X\n", req, bx());
                setCF(true);
                setAX(8);  /* Insufficient memory */
            }
            break;
        }

        case 0x2149:    /* Free memory */
        {
            if (f_verbose)
                fprintf(stderr, "  Free seg=%04X\n", es());
            int err = dosFreeMem(es());
            if (err) {
                setCF(true);
                setAX(err);
            } else {
                setCF(false);
            }
            break;
        }

        case 0x214a:    /* Resize memory block */
        {
            if (f_verbose)
                fprintf(stderr, "  Resize seg=%04X cur=%04X new=%04X\n",
                        es(), mcbSize(es() - 1), bx());
            int err = dosResizeMem(es(), bx());
            if (err) {
                if (f_verbose)
                    fprintf(stderr, "  Resize FAIL err=%d maxAvail=%04X\n", err, bx());
                setCF(true);
                setAX(err);
            } else {
                setCF(false);
            }
            break;
        }

        case 0x214b:    /* EXEC - Load and execute program */
            handleExec(e);
            break;

        case 0x214c:    /* Terminate with return code */
            sysExit(e, al());
            break;

        case 0x214d:    /* Get return code */
            setAX(lastChildExitCode & 0xFF);
            break;

        case 0x214e:    /* Find first matching file */
        case 0x214f:    /* Find next matching file */
            {
                /* Use Windows FindFirstFile / FindNextFile */
                static HANDLE hFind = INVALID_HANDLE_VALUE;
                static WIN32_FIND_DATAA findData;
                int found = 0;

                if ((intno << 8 | ah()) == 0x214e) {
                    /* Find first */
                    if (hFind != INVALID_HANDLE_VALUE) {
                        FindClose(hFind);
                        hFind = INVALID_HANDLE_VALUE;
                    }
                    hFind = FindFirstFileA(dsdx(), &findData);
                    found = (hFind != INVALID_HANDLE_VALUE);
                } else {
                    /* Find next */
                    if (hFind != INVALID_HANDLE_VALUE)
                        found = FindNextFileA(hFind, &findData);
                }

                if (found) {
                    /* DTA is at address set by INT 21h/1Ah */
                    DWord dtaPhys = ((DWord)dtaSeg << 4) + dtaOff;
                    /* Attribute byte at offset 0x15 */
                    Byte dosAttr = 0;
                    if (findData.dwFileAttributes & FILE_ATTRIBUTE_READONLY)  dosAttr |= 0x01;
                    if (findData.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)    dosAttr |= 0x02;
                    if (findData.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)    dosAttr |= 0x04;
                    if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dosAttr |= 0x10;
                    if (findData.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE)   dosAttr |= 0x20;
                    ram[dtaPhys + 0x15] = dosAttr;
                    /* File size at offset 0x1A (4 bytes, little-endian) */
                    DWord fsize = findData.nFileSizeLow;
                    ram[dtaPhys + 0x1A] = (Byte)fsize;
                    ram[dtaPhys + 0x1B] = (Byte)(fsize >> 8);
                    ram[dtaPhys + 0x1C] = (Byte)(fsize >> 16);
                    ram[dtaPhys + 0x1D] = (Byte)(fsize >> 24);
                    /* Time at offset 0x16 (2 bytes), Date at offset 0x18 (2 bytes) */
                    FILETIME localFt;
                    WORD dosDate, dosTime;
                    FileTimeToLocalFileTime(&findData.ftLastWriteTime, &localFt);
                    FileTimeToDosDateTime(&localFt, &dosDate, &dosTime);
                    ram[dtaPhys + 0x16] = (Byte)dosTime;
                    ram[dtaPhys + 0x17] = (Byte)(dosTime >> 8);
                    ram[dtaPhys + 0x18] = (Byte)dosDate;
                    ram[dtaPhys + 0x19] = (Byte)(dosDate >> 8);
                    /* Filename at offset 0x1E (13 bytes, NUL terminated) */
                    {
                        int fnLen = (int)strlen(findData.cFileName);
                        if (fnLen > 12) fnLen = 12;
                        for (int i = 0; i < fnLen; i++)
                            ram[dtaPhys + 0x1E + i] = (Byte)findData.cFileName[i];
                        ram[dtaPhys + 0x1E + fnLen] = 0;
                    }
                    /* Mark shadow bits as written */
                    for (int i = 0; i < 43; i++)
                        shadowRam[dtaPhys + i] |= fRead | fWrite;
                    if (f_verbose) fprintf(stderr, "  FindFirst: \"%s\" -> \"%s\" at DTA %04X:%04X\n",
                        dsdx(), findData.cFileName, dtaSeg, dtaOff);
                    setCF(false);
                } else {
                    setCF(true);
                    setAX(18);  /* No more files */
                    if (f_verbose) fprintf(stderr, "  FindFirst: \"%s\" -> NOT FOUND\n", dsdx());
                    if (hFind != INVALID_HANDLE_VALUE) {
                        FindClose(hFind);
                        hFind = INVALID_HANDLE_VALUE;
                    }
                }
            }
            break;

        case 0x2156:    /* Rename file */
        {
            char *oldName = dsdx();
            char *newName = initString(di(), ES, false, 1, 0x10000);
            if (f_verbose) fprintf(stderr, "  Rename: \"%s\" -> \"%s\"\n", oldName, newName);
            if (rename(oldName, newName) == 0)
                setCF(false);
            else {
                setCF(true);
                setAX(dosError(errno));
                if (f_verbose) fprintf(stderr, "  Rename: FAILED errno=%d\n", errno);
            }
        }
            break;

        case 0x2157:    /* Get/Set file date and time */
            switch (al()) {
                case 0x00:
                    if (bx() >= (Word)fileDescriptorCount || fileDescriptors[bx()] == -1) {
                        setCF(true);
                        setAX(6);
                        break;
                    }
                    setCX(0x0000);  /* Return reasonable time */
                    setDX(0x0021);  /* and date */
                    setCF(false);
                    break;
                case 0x01:
                    /* Set file date/time - ignore */
                    setCF(false);
                    break;
                default:
                    if (f_verbose)
                        fprintf(stderr, "Unknown INT 21h/57h subfn: AL=0x%02x\n", al());
                    setCF(true);
                    setAX(1);
            }
            break;

        case 0x2158:    /* Get/Set allocation strategy */
            if (al() == 0) {
                setAX(0);   /* First fit */
                setCF(false);
            } else if (al() == 1) {
                setCF(false);  /* Set - ignore */
            } else {
                setCF(true);
                setAX(1);
            }
            break;

        case 0x215a:    /* Create temporary file */
            {
                char *dir = dsdx();
                char tmpname[260];
                if (GetTempFileNameA(dir[0] ? dir : ".", "dos", 0, tmpname)) {
                    fileDescriptor = _open(tmpname, _O_CREAT | _O_TRUNC | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
                    if (fileDescriptor != -1) {
                        int gd = getDescriptor();
                        setAX(gd);
                        fileDescriptors[gd] = fileDescriptor;
                        /* Write back the filename to DS:DX */
                        for (int i = 0; tmpname[i]; i++)
                            ram[physicalAddress(dx() + i, DS, true)] = tmpname[i];
                        ram[physicalAddress(dx() + (Word)strlen(tmpname), DS, true)] = 0;
                        setCF(false);
                    } else {
                        setCF(true);
                        setAX(dosError(errno));
                    }
                } else {
                    setCF(true);
                    setAX(5);
                }
            }
            break;

        case 0x215b:    /* Create new file (fail if exists) */
            fileDescriptor = _open(dsdx(), _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
            if (fileDescriptor != -1) {
                setCF(false);
                int gd = getDescriptor();
                setAX(gd);
                fileDescriptors[gd] = fileDescriptor;
            }
            else {
                setCF(true);
                setAX(dosError(errno));
            }
            break;

        case 0x2162:    /* Get PSP address */
            setBX(loadSegment - 0x10);
            setCF(false);
            break;

        case 0x2163:    /* Get Lead Byte Table (DBCS) */
        {
            /* Return pointer to empty double-byte lead byte table.
               Store a terminator (0x0000) at a fixed location in PSP reserved area. */
            Word tblSeg = loadSegment - 0x10;  /* PSP segment */
            Word tblOff = 0x00FC;              /* unused area near end of PSP */
            DWord addr = ((DWord)tblSeg << 4) + tblOff;
            ram[addr] = 0;
            ram[addr+1] = 0;
            setDS(tblSeg);
            setSI(tblOff);
            setCF(false);
            break;
        }

        case 0x2165:    /* Get Extended Country Information */
        {
            /* AL=02: uppercase table, AL=04: filename uppercase table,
               AL=06: collating table.  Return a simple ASCII uppercase table
               stored in scratch RAM so COMMAND.COM can uppercase commands.
               INT 21h/65h buffer is at ES:DI (not DS:DX).
               BX=code page (FFFFh=current), DX=country ID (FFFFh=current). */
            int subfn = al();
            if (subfn == 0x02 || subfn == 0x04 || subfn == 0x06) {
                /* Build an uppercase table at a fixed location.
                   Format: info_id (byte), dword pointer to table header.
                   Table header: word(128) = length, then 128 bytes mapping 80h-FFh.
                   For plain ASCII, map 80h-FFh to themselves (no accented chars). */
                Word tblSeg = loadSegment - 0x10;   /* PSP segment */
                Word tblOff = 0x00A0;               /* scratch area in PSP */
                DWord base = ((DWord)tblSeg << 4) + tblOff;

                /* ES:DI points to caller's buffer; CX = buffer size. */
                DWord buf = ((DWord)es() << 4) + di();
                if (cx() >= 5) {
                    ram[buf] = (Byte)subfn;          /* info ID */
                    /* Pointer to the 130-byte table (word length + 128 bytes) */
                    ram[buf+1] = (Byte)(tblOff);
                    ram[buf+2] = (Byte)(tblOff >> 8);
                    ram[buf+3] = (Byte)(tblSeg);
                    ram[buf+4] = (Byte)(tblSeg >> 8);
                }
                /* Write the uppercase table header */
                ram[base+0] = 128;  /* length low */
                ram[base+1] = 0;    /* length high */
                for (int i = 0; i < 128; i++)
                    ram[base+2+i] = (Byte)(0x80 + i);  /* identity map */

                setCX(5);
                setCF(false);
            } else if (subfn == 0x01) {
                /* AL=01: Extended country info — return US defaults in ES:DI buffer */
                DWord buf = ((DWord)es() << 4) + di();
                if (cx() >= 41) {
                    memset(&ram[buf], 0, 41);
                    ram[buf] = 1;       /* info ID */
                    /* country code = 1 (US), codepage = 437 */
                    ram[buf+3] = 1;     /* country code low */
                    ram[buf+5] = 0xB5;  /* codepage 437 low */
                    ram[buf+6] = 0x01;  /* codepage 437 high */
                    setCX(41);
                }
                setCF(false);
            } else {
                setCF(true);
                setAX(1);
            }
            break;
        }

        default:
            if (f_verbose)
                fprintf(stderr, "Unimplemented DOS/BIOS call: INT 0x%02x, AH=0x%02x, AX=0x%04x\n",
                    intno, (unsigned)ah(), (unsigned)ax());
            /* For unknown calls, try to not crash - set carry and return error */
            setCF(true);
            setAX(1);  /* Invalid function */
            return 1;   /* Don't abort */
    }
    return 1;
}
