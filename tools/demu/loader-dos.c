/* DOS executable loader for 8086 emulator - Windows/MSVC port */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "8086.h"
#include "exe.h"

extern int f_verbose;
extern int f_shell;
Word loadSegment;

static void loadError(const char *msg, ...)
{
    va_list args;
    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
    exit(1);
}

int envSegment;  /* set by write_environ, used by initDOSMemory */

static void write_environ(int argc, char **argv)
{
    /* Environment needs enough room for PATH=<long path> etc.
       Allocate 0x100 paragraphs (4KB) before the PSP. */
    envSegment = loadSegment - 0x10 - 0x100;
    char *filename = argv[0];
    int i, j;

    /* prepare environment segment — write DOS-style environment strings.
       Format: "VAR=VALUE\0VAR=VALUE\0\0" followed by Word(1) and program name. */
    setES(envSegment);
    setShadowFlags(0, ES, 0x1000, fRead);

    /* Write environment strings from host */
    i = 0;
    /* Always include COMSPEC */
    {
        const char *comspec = "COMSPEC=COMMAND.COM";
        for (j = 0; comspec[j]; j++)
            writeByte(comspec[j], i++, ES);
        writeByte(0, i++, ES);
    }
    /* Copy selected host environment variables that DOS tools need */
    {
        static const char *passVars[] = { "PATH", "INCLUDE", "LIB", "INIT",
                                          "TMP", "TEMP", "COUNTRY", NULL };
        for (int v = 0; passVars[v]; v++) {
            const char *val = getenv(passVars[v]);
            if (val) {
                const char *name = passVars[v];
                for (j = 0; name[j]; j++)
                    writeByte(name[j], i++, ES);
                writeByte('=', i++, ES);
                for (j = 0; val[j] && i < 0xF00; j++)
                    writeByte(val[j], i++, ES);
                writeByte(0, i++, ES);
            }
        }
    }
    writeByte(0, i++, ES);     /* double NUL = end of environment */
    writeWord(0x0001, i, ES);
    i += 2;
    for (j = 0; filename[j] != 0; ++j) {
        writeByte(filename[j], i + j, ES);
        if (i + j + 1 >= 0x1000)
            loadError("Program name too long\n");
    }
    writeWord(0x0000, i + j, ES);

    /* prepare PSP */
    setES(loadSegment - 0x10);
    setShadowFlags(0, ES, 0x0100, fRead|fWrite);
    writeByte(0xCD, 0x00, ES);         /* INT 20h at PSP:0000 */
    writeByte(0x20, 0x01, ES);
    writeWord(0x9fff, 2, ES);
    writeWord(envSegment, 0x2c, ES);
    i = 0x81;
    for (int a = 1; a < argc; ++a) {
        writeByte(' ', i++, ES);

        char* arg = argv[a];
        int quote = strchr(arg, ' ') != 0;
        if (quote)
            writeByte('\"', i++, ES);

        for (; *arg != 0; ++arg) {
            if (*arg == '\"')
                writeByte('\\', i++, ES);
            writeByte(*arg, i++, ES);
        }
        if (quote)
            writeByte('\"', i++, ES);
        if (i > 0xff)
            loadError("Arguments too long\n");
    }
    writeByte('\r', i, ES);
    writeByte(i - 0x81, 0x80, ES);
}

static void load_bios_values(void)
{
    /* In real DOS, the first 0x500 bytes are the IVT (0-3FF) and BIOS data
       area (400-4FF), and are freely readable/writable by programs. */
    setES(0x0000);
    setShadowFlags(0x0000, ES, 0x0500, fRead|fWrite);
    writeWord(0x0000, 0x0080, ES);
    writeWord(0xFFFF, 0x0082, ES);
    writeWord(0x0058, 0x046C, ES);
    writeWord(0x000C, 0x046E, ES);
    writeByte(0x00, 0x0470, ES);
    setES(0xF000);
    setShadowFlags(0xFF00, ES, 0x0100, fRead);
    for (int i = 0; i < 0x100; i += 2)
        writeWord(0xF4F4, 0xFF00 + i, ES);
    writeByte(0xEA, 0xFFF0, ES);
    writeWord(0xFFF0, 0xFFF1, ES);
    writeWord(0xF000, 0xFFF3, ES);
}

void loadExecutableDOS(struct exe *e, const char *path, int argc, char **argv)
{
    struct _stat sbuf;
    int fd;
    int bytesRead;

    fd = _open(path, _O_RDONLY | _O_BINARY);
    if (fd < 0)
        loadError("Can't open %s\n", path);
    if (_fstat(fd, &sbuf) < 0)
        loadError("Can't stat %s\n", path);
    size_t filesize = (size_t)sbuf.st_size;

    loadSegment = 0x1000;
    Word pspSegment = loadSegment - 0x10;

    /* Read entire file into a temporary area at end of RAM for parsing */
    DWord tempOffset = (DWord)(RAMSIZE - filesize);
    tempOffset &= ~0xF;  /* align to paragraph */
    if (tempOffset < ((DWord)loadSegment << 4) + filesize)
        loadError("Not enough memory to load %s\n", path);
    bytesRead = _read(fd, &ram[tempOffset], (unsigned int)filesize);
    if (bytesRead != (int)filesize)
        loadError("Error reading executable: %s\n", path);
    _close(fd);

    write_environ(argc, argv);
    struct image_dos_header *hdr = (struct image_dos_header *)&ram[tempOffset];
    if (filesize >= 2 && hdr->e_magic == DOSMAGIC) {  /* .exe file */
        if (filesize < 0x21)
            loadError("%s is too short to be an .exe file\n", path);
        Word bytesInLastBlock = hdr->e_cblp;
        int exeLength = ((hdr->e_cp - (bytesInLastBlock == 0 ? 0 : 1)) << 9)
            + bytesInLastBlock;
        Word headerParagraphs = hdr->e_cparhdr;
        Word headerLength = headerParagraphs << 4;
        if (exeLength > (int)filesize || headerLength > (int)filesize || headerLength > exeLength)
            loadError("%s is corrupt\n", path);

        /* In real DOS, image loads right after PSP at PSP+0x10 */
        Word imageSegment = pspSegment + 0x10;
        int imageBytes = exeLength - headerLength;

        /* Copy just the image (no header) to the right location */
        DWord imagePhys = (DWord)imageSegment << 4;
        memmove(&ram[imagePhys], &ram[tempOffset + headerLength], imageBytes);
        /* Zero out BSS area */
        int extraParas = hdr->e_minalloc;
        memset(&ram[imagePhys + imageBytes], 0, extraParas << 4);

        /* Mark all program memory read/write up to conventional memory limit.
           Programs (especially EXEPACK'd) may need more than the header says. */
        int totalImage = (0x9000 - imageSegment) << 4;
        DWord physEnd = imagePhys + totalImage;
        if (physEnd > RAMSIZE)
            totalImage = RAMSIZE - (int)imagePhys;
        DWord stackEnd = ((DWord)hdr->e_ss << 4) + hdr->e_sp;
        if ((int)stackEnd > totalImage)
            totalImage = (int)stackEnd;
        setES(imageSegment);
        setShadowFlags(0, ES, totalImage, fRead|fWrite);

        /* Apply relocations using the temp copy of the relocation table */
        struct dos_reloc *r = (struct dos_reloc *)&ram[tempOffset + hdr->e_lfarlc];
        for (int i = 0; i < hdr->e_crlc; ++i) {
            Word offset = r->r_offset;
            setCS(imageSegment + r->r_seg);
            writeWord(readWordSeg(offset, CS) + imageSegment, offset, CS);
            r++;
        }

        setES(pspSegment);
        setDS(pspSegment);
        setIP(hdr->e_ip);
        setCS(hdr->e_cs + imageSegment);
        Word ssval = hdr->e_ss + imageSegment;
        setSS(ssval);
        setSP(hdr->e_sp);
        e->t_stackLow = (((imageBytes + 15) >> 4) + imageSegment) << 4;
        if (e->t_stackLow < ((DWord)ssval << 4) + 0x10)
            e->t_stackLow = ((DWord)ssval << 4) + 0x10;
        if (e->t_stackLow > ((DWord)ssval << 4) + sp())
            e->t_stackLow = 0;

        initDOSMemory(pspSegment, 0x9FFF);
    } else {
        /* .COM file - gets full 64K segment.
           In DOS, CS=DS=ES=SS=PSP segment, file loads at PSP:0100 */
        /* Copy from temp area to PSP:0100 */
        DWord comDest = ((DWord)pspSegment << 4) + 0x100;
        memmove(&ram[comDest], &ram[tempOffset], filesize);

        if (filesize > 0xff00)
            loadError("%s is too long to be a .com file\n", path);
        setES(pspSegment);
        setShadowFlags(0, ES, 0xFFF0, fRead|fWrite);
        setES(pspSegment);
        setDS(pspSegment);
        setSS(pspSegment);
        setSP(0xFFFE);
        setCS(pspSegment);
        setIP(0x0100);
        /* Push 0x0000 on stack so RET goes to PSP:0000 (INT 20h) */
        writeWord(0x0000, 0xFFFE, SS);
        e->t_stackLow = ((DWord)pspSegment << 4) + (DWord)filesize + 0x100;
        initDOSMemory(pspSegment, 0x9FFF);
    }
    setShadowFlags(0, SS, sp(), fRead|fWrite);
    load_bios_values();

    if (f_verbose) printf("CS:IP %04x:%04x DS %04x SS:SP %04x:%04x\n",
        cs(), getIP(), ds(), ss(), sp());
    setES(pspSegment);
    setAX(0x0000);
    setBX(0x0000);
    setCX(0x0000);
    setDX(0x0000);
    setBP(0x091C);
    setSI(0x0100);
    setDI(0xFFFE);
    setFlags(0xF202);   /* Interrupts enabled and 8086 reserved bits on */

    e->handleSyscall = handleSyscallDOS;
    e->checkStack = checkStackDOS;

    /* In shell mode, COMMAND.COM sets up its own stack within its code
       area and manages its own memory layout.  Disable the stack overflow
       guard so demu doesn't trip over the deliberate stack relocation. */
    if (f_shell)
        e->t_stackLow = 0;
}
