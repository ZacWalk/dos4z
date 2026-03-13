/* exec-dos.c — DOS EXEC (INT 21h/4Bh) and shell command emulation.
   All process-launch logic lives here so that demu never calls CreateProcess. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <sys/stat.h>
#include <errno.h>
#include <setjmp.h>
#include <windows.h>
#include "8086.h"
#include "exe.h"
#include "exec-dos.h"

extern int f_verbose;
extern Word loadSegment;
extern Byte shadowRam[];

/* lib-omf.c */
extern int nativeLib(const char *args);

/* Forward declarations */
static void cmdBaseName(const char *prog, char *out, int outSz);
int runNativeCommand(const char *cmdName, const char *args);

/* ── In-process EXEC state ───────────────────────────────────────────── */
#define EXEC_MAX_DEPTH 8

struct execState {
    struct cpuState cpu;
    Word   loadSeg;
    Word   mcbHeadSaved;
    Word   pspSeg;
    Word   dtaSeg, dtaOff;
    char   cwd[MAX_PATH];
    int   *savedFds;
    unsigned char *savedFdCR;
    int    savedFdCount;
    jmp_buf jmpEnv;
};

static struct execState execStack[EXEC_MAX_DEPTH];
static int execDepth = 0;

/* ── sysExit: terminate current program (or child if nested) ─────────── */

int sysExit(struct exe *e, int rc)
{
    if (f_verbose)
        fprintf(stderr, "EXIT %d (depth %d)\n", rc, execDepth);
    if (execDepth > 0) {
        setLastChildExitCode(rc);
        longjmp(execStack[execDepth - 1].jmpEnv, rc + 1);
    }
    exit(rc);
    return -1;
}

/* ── PATH search ─────────────────────────────────────────────────────── */

static int searchPathForDosExe(const char *progName, char *outPath, int outSize)
{
    const char *exts[4];
    int nExts = 0;
    exts[nExts++] = "";
    const char *dot = strrchr(progName, '.');
    int hasExe = (dot && _stricmp(dot, ".EXE") == 0);
    int hasCom = (dot && _stricmp(dot, ".COM") == 0);
    if (!dot) {
        exts[nExts++] = ".EXE";
        exts[nExts++] = ".COM";
    }

    char baseName[MAX_PATH];
    if (hasExe || hasCom) {
        size_t baseLen = dot - progName;
        if (baseLen >= sizeof(baseName)) baseLen = sizeof(baseName) - 1;
        memcpy(baseName, progName, baseLen);
        baseName[baseLen] = '\0';
    }

    const char *pathEnv = getenv("PATH");
    char *pathCopy = pathEnv ? _strdup(pathEnv) : NULL;

    /* Phase 1: current directory */
    for (int i = 0; i < nExts; i++) {
        _snprintf(outPath, outSize, "%s%s", progName, exts[i]);
        outPath[outSize - 1] = '\0';
        if (GetFileAttributesA(outPath) != INVALID_FILE_ATTRIBUTES)
            { free(pathCopy); return 1; }
    }
    if (hasExe || hasCom) {
        const char *altExt = hasExe ? ".COM" : ".EXE";
        _snprintf(outPath, outSize, "%s%s", baseName, altExt);
        outPath[outSize - 1] = '\0';
        if (GetFileAttributesA(outPath) != INVALID_FILE_ATTRIBUTES)
            { free(pathCopy); return 1; }
    }

    /* Phase 2: search PATH */
    if (!pathCopy) return 0;
    char *ctx = NULL;
    for (char *dir = strtok_s(pathCopy, ";", &ctx); dir; dir = strtok_s(NULL, ";", &ctx)) {
        if (!*dir) continue;
        int dlen = (int)strlen(dir);
        const char *sfx = (dir[dlen-1] == '\\' || dir[dlen-1] == '/') ? "" : "\\";
        for (int i = 0; i < nExts; i++) {
            _snprintf(outPath, outSize, "%s%s%s%s", dir, sfx, progName, exts[i]);
            outPath[outSize - 1] = '\0';
            if (GetFileAttributesA(outPath) != INVALID_FILE_ATTRIBUTES)
                { free(pathCopy); return 1; }
        }
        if (hasExe || hasCom) {
            const char *altExt = hasExe ? ".COM" : ".EXE";
            _snprintf(outPath, outSize, "%s%s%s%s", dir, sfx, baseName, altExt);
            outPath[outSize - 1] = '\0';
            if (GetFileAttributesA(outPath) != INVALID_FILE_ATTRIBUTES)
                { free(pathCopy); return 1; }
        }
    }
    free(pathCopy);
    return 0;
}

/* ── Redirection parser ──────────────────────────────────────────────── */

static void parseRedirects(char *cmdTail, char *inFile, int inSz,
                           char *outFile, int outSz)
{
    inFile[0] = '\0';
    outFile[0] = '\0';
    char cleaned[512];
    int ci = 0;
    char *p = cmdTail;
    while (*p) {
        if (*p == '<') {
            p++;
            while (*p == ' ') p++;
            int j = 0;
            while (*p && *p != ' ' && *p != '>' && j < inSz - 1)
                inFile[j++] = *p++;
            inFile[j] = '\0';
        } else if (*p == '>') {
            p++;
            while (*p == ' ') p++;
            int j = 0;
            while (*p && *p != ' ' && *p != '<' && j < outSz - 1)
                outFile[j++] = *p++;
            outFile[j] = '\0';
        } else {
            if (ci < (int)sizeof(cleaned) - 1)
                cleaned[ci++] = *p;
            p++;
        }
    }
    cleaned[ci] = '\0';
    strcpy(cmdTail, cleaned);
}

/* ── Child environment block ─────────────────────────────────────────── */

static Word createChildEnv(Word parentEnvSeg, const char *childPath)
{
    DWord parentPhys = (DWord)parentEnvSeg << 4;
    int envLen = 0;
    while (envLen < 0x7FFF) {
        if (ram[parentPhys + envLen] == 0) {
            envLen++;
            if (ram[parentPhys + envLen] == 0) { envLen++; break; }
        } else {
            envLen++;
        }
    }
    int pathLen = (int)strlen(childPath);
    int totalBytes = envLen + 2 + pathLen + 1;
    Word needParas = (Word)((totalBytes + 0x1F) >> 4);

    Word envBlock = dosAllocMem(needParas);
    if (!envBlock) return 0;

    DWord envPhys = (DWord)envBlock << 4;
    memcpy(&ram[envPhys], &ram[parentPhys], envLen);
    for (DWord a = envPhys; a < envPhys + (DWord)totalBytes && a < RAMSIZE; a++)
        shadowRam[a] |= fRead | fWrite;
    ram[envPhys + envLen]     = 0x01;
    ram[envPhys + envLen + 1] = 0x00;
    for (int i = 0; i < pathLen; i++)
        ram[envPhys + envLen + 2 + i] = (Byte)childPath[i];
    ram[envPhys + envLen + 2 + pathLen] = 0;
    if (f_verbose)
        fprintf(stderr, "  createChildEnv: envLen=%d pathLen=%d path='%s' seg=%04X\n",
                envLen, pathLen, childPath, envBlock);
    return envBlock;
}

/* ── Load a DOS executable in-process ────────────────────────────────── */

static Word execLoadChild(struct exe *e, const char *path, const char *cmdTail,
                           Word envSeg, const char *resolvedPath)
{
    int fd = _open(path, _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        if (f_verbose) fprintf(stderr, "  execLoadChild: can't open '%s'\n", path);
        return 0;
    }
    struct _stat sbuf;
    if (_fstat(fd, &sbuf) < 0) { _close(fd); return 0; }
    size_t filesize = (size_t)sbuf.st_size;

    unsigned char *fileBuf = (unsigned char *)malloc(filesize);
    if (!fileBuf) { _close(fd); return 0; }
    int n = _read(fd, fileBuf, (unsigned int)filesize);
    _close(fd);
    if (n != (int)filesize) { free(fileBuf); return 0; }

    struct image_dos_header *hdr = (struct image_dos_header *)fileBuf;
    int isMZ = (filesize >= 2 && hdr->e_magic == DOSMAGIC);

    Word needParas, wantParas;
    if (isMZ) {
        Word bytesInLastBlock = hdr->e_cblp;
        int exeLength = ((hdr->e_cp - (bytesInLastBlock == 0 ? 0 : 1)) << 9)
            + bytesInLastBlock;
        Word headerLength = hdr->e_cparhdr << 4;
        int imageBytes = exeLength - headerLength;
        int totalBytes = imageBytes + (hdr->e_minalloc << 4);
        int stackEnd = (hdr->e_ss << 4) + hdr->e_sp;
        if (stackEnd > totalBytes) totalBytes = stackEnd;
        needParas = (Word)((totalBytes + 0x10F) >> 4);
        int maxBytes = totalBytes + ((int)hdr->e_maxalloc << 4);
        if (hdr->e_maxalloc == 0xFFFF || maxBytes < 0)
            wantParas = 0xFFFF;
        else
            wantParas = (Word)((maxBytes + 0x10F) >> 4);
    } else {
        needParas = 0x1000;
        wantParas = 0xFFFF;
    }

    /* Allocate env block FIRST (before main program memory), matching real DOS EXEC behavior */
    Word preAllocEnvSeg = 0;
    {
        Word srcEnvSeg;
        if (envSeg != 0) {
            srcEnvSeg = envSeg;
        } else {
            Word curPsp = getPspSegment();
            srcEnvSeg = ram[((DWord)curPsp << 4) + 0x2C] |
                        (ram[((DWord)curPsp << 4) + 0x2D] << 8);
        }
        preAllocEnvSeg = createChildEnv(srcEnvSeg, resolvedPath);
    }

    Word childBlock = dosAllocMem(wantParas);
    if (!childBlock) {
        Word largest = bx();
        if (largest >= needParas)
            childBlock = dosAllocMem(largest);
    }
    if (!childBlock)
        childBlock = dosAllocMem(needParas);
    if (!childBlock) {
        if (f_verbose)
            fprintf(stderr, "  execLoadChild: alloc failed need=%04X for '%s'\n",
                    needParas, path);
        if (preAllocEnvSeg) dosFreeMem(preAllocEnvSeg);
        free(fileBuf);
        return 0;
    }

    Word pspSeg = getPspSegment();
    Word childPSP = childBlock;
    DWord pspPhys = (DWord)childPSP << 4;

    Word childMCB = childPSP - 1;
    mcbWrite(childMCB, mcbType(childMCB), childPSP, mcbSize(childMCB));

    for (DWord a = pspPhys; a < pspPhys + 0x100 && a < RAMSIZE; a++)
        shadowRam[a] |= fRead | fWrite;
    memset(&ram[pspPhys], 0, 0x100);

    ram[pspPhys + 0x00] = 0xCD;
    ram[pspPhys + 0x01] = 0x20;
    Word mcb = childPSP - 1;
    Word blockSize = mcbSize(mcb);
    Word memTop = childPSP + blockSize;
    ram[pspPhys + 0x02] = (Byte)memTop;
    ram[pspPhys + 0x03] = (Byte)(memTop >> 8);
    ram[pspPhys + 0x16] = (Byte)pspSeg;
    ram[pspPhys + 0x17] = (Byte)(pspSeg >> 8);

    Word childEnvSeg;
    if (preAllocEnvSeg) {
        childEnvSeg = preAllocEnvSeg;
        Word envMcb = childEnvSeg - 1;
        if (envMcb >= getMcbHead())
            mcbWrite(envMcb, mcbType(envMcb), childPSP, mcbSize(envMcb));
    } else {
        /* Pre-allocation failed; use parent env directly */
        if (envSeg != 0) {
            childEnvSeg = envSeg;
        } else {
            childEnvSeg = ram[((DWord)pspSeg << 4) + 0x2C] |
                          (ram[((DWord)pspSeg << 4) + 0x2D] << 8);
        }
    }
    if (f_verbose)
        fprintf(stderr, "  execLoadChild: envSeg=%04X childEnv=%04X path='%s'\n",
                envSeg, childEnvSeg, resolvedPath);
    ram[pspPhys + 0x2C] = (Byte)childEnvSeg;
    ram[pspPhys + 0x2D] = (Byte)(childEnvSeg >> 8);

    int tailLen = (int)strlen(cmdTail);
    if (tailLen > 126) tailLen = 126;
    ram[pspPhys + 0x80] = (Byte)tailLen;
    for (int i = 0; i < tailLen; i++)
        ram[pspPhys + 0x81 + i] = (Byte)cmdTail[i];
    ram[pspPhys + 0x81 + tailLen] = '\r';

    if (f_verbose) {
        fprintf(stderr, "  PSP %04X: tail[%d]='", childPSP, tailLen);
        for (int i = 0; i < tailLen; i++) {
            unsigned char c = ram[pspPhys + 0x81 + i];
            if (c >= 0x20 && c < 0x7F) fputc(c, stderr);
            else fprintf(stderr, "\\x%02X", c);
        }
        fprintf(stderr, "'\n");
        DWord ep = (DWord)childEnvSeg << 4;
        int off = 0;
        /* Dump all environment strings */
        fprintf(stderr, "  Env:\n");
        while (off < 0x7FFF && ram[ep + off] != 0) {
            fprintf(stderr, "    ");
            while (ram[ep + off] != 0 && off < 0x7FFF) {
                fputc(ram[ep + off], stderr);
                off++;
            }
            fputc('\n', stderr);
            off++;  /* skip NUL */
        }
        off++;  /* skip final NUL */
        Word cnt = ram[ep + off] | (ram[ep + off + 1] << 8);
        off += 2;
        fprintf(stderr, "  Env prog name (count=%d): '", cnt);
        while (ram[ep + off]) { fputc(ram[ep + off], stderr); off++; }
        fprintf(stderr, "'\n");
    }

    Word childLoadSeg = childPSP + 0x10;

    if (isMZ) {
        Word bytesInLastBlock = hdr->e_cblp;
        int exeLength = ((hdr->e_cp - (bytesInLastBlock == 0 ? 0 : 1)) << 9)
            + bytesInLastBlock;
        Word headerLength = hdr->e_cparhdr << 4;
        if (exeLength > (int)filesize || headerLength > (int)filesize) {
            dosFreeMem(childBlock);
            free(fileBuf);
            return 0;
        }
        int imageBytes = exeLength - headerLength;
        DWord imagePhys = (DWord)childLoadSeg << 4;
        memcpy(&ram[imagePhys], fileBuf + headerLength, imageBytes);
        int bssBytes = hdr->e_minalloc << 4;
        if (imagePhys + imageBytes + bssBytes <= RAMSIZE)
            memset(&ram[imagePhys + imageBytes], 0, bssBytes);

        int totalMem = (blockSize - 0x10) << 4;
        if (imagePhys + totalMem > RAMSIZE)
            totalMem = RAMSIZE - (int)imagePhys;
        for (DWord a = imagePhys; a < imagePhys + (DWord)totalMem && a < RAMSIZE; a++)
            shadowRam[a] |= fRead | fWrite;

        struct dos_reloc *r = (struct dos_reloc *)(fileBuf + hdr->e_lfarlc);
        for (int i = 0; i < hdr->e_crlc; ++i) {
            DWord relocPhys = imagePhys + ((DWord)r->r_seg << 4) + r->r_offset;
            if (relocPhys + 1 < RAMSIZE) {
                Word val = ram[relocPhys] | (ram[relocPhys + 1] << 8);
                val += childLoadSeg;
                ram[relocPhys] = (Byte)val;
                ram[relocPhys + 1] = (Byte)(val >> 8);
            }
            r++;
        }

        setCS(hdr->e_cs + childLoadSeg);
        setIP(hdr->e_ip);
        setSS(hdr->e_ss + childLoadSeg);
        setSP(hdr->e_sp);
        setDS(childPSP);
        setES(childPSP);
        e->t_stackLow = 0;
    } else {
        DWord comDest = (DWord)childPSP << 4;
        for (DWord a = comDest; a < comDest + 0xFFF0 && a < RAMSIZE; a++)
            shadowRam[a] |= fRead | fWrite;
        memcpy(&ram[comDest + 0x100], fileBuf, filesize);

        setCS(childPSP);
        setIP(0x0100);
        setSS(childPSP);
        setSP(0xFFFE);
        setDS(childPSP);
        setES(childPSP);
        ram[comDest + 0xFFFE] = 0;
        ram[comDest + 0xFFFF] = 0;
        e->t_stackLow = 0;
    }

    setAX(0); setBX(0); setCX(0); setDX(0);
    setBP(0x091C); setSI(0x0100); setDI(0xFFFE);
    setFlags(0xF202);

    loadSegment = childLoadSeg;
    setPspSegment(childPSP);
    setDtaSeg(childPSP);
    setDtaOff(0x80);

    free(fileBuf);
    return childPSP;
}

/* ── Run a DOS .EXE/.COM in-process ──────────────────────────────────── */

static int execInProcess(struct exe *e, const char *progPath, char *cmdTail, Word envSeg)
{
    if (execDepth >= EXEC_MAX_DEPTH) {
        fprintf(stderr, "EXEC: nesting too deep (%d)\n", execDepth);
        return -1;
    }

    char inFile[MAX_PATH], outFile[MAX_PATH];
    parseRedirects(cmdTail, inFile, sizeof(inFile), outFile, sizeof(outFile));

    /* Intercept programs that have native replacements (e.g. LIB.EXE is OS/2 NE) */
    {
        char baseName[64];
        cmdBaseName(progPath, baseName, sizeof(baseName));
        int nrc = runNativeCommand(baseName, cmdTail);
        if (nrc >= 0) {
            if (f_verbose)
                fprintf(stderr, "EXEC[%d]: native '%s' tail='%s' rc=%d\n",
                        execDepth, baseName, cmdTail, nrc);
            return nrc;
        }
    }

    char foundPath[MAX_PATH];
    if (!searchPathForDosExe(progPath, foundPath, sizeof(foundPath))) {
        if (f_verbose) fprintf(stderr, "EXEC: not found: %s\n", progPath);
        return -1;
    }

    if (f_verbose)
        fprintf(stderr, "EXEC[%d]: in-process '%s' tail='%s' in='%s' out='%s'\n",
                execDepth, foundPath, cmdTail, inFile, outFile);

    struct execState *st = &execStack[execDepth];
    saveCPU(&st->cpu);
    st->loadSeg      = loadSegment;
    st->mcbHeadSaved = getMcbHead();
    st->pspSeg       = getPspSegment();
    st->dtaSeg       = getDtaSeg();
    st->dtaOff       = getDtaOff();
    _getcwd(st->cwd, sizeof(st->cwd));

    int fdCount = getFdCount();
    int *fds    = getFds();
    unsigned char *fdCR = getFdCR();
    st->savedFdCount = fdCount;
    st->savedFds  = (int *)malloc(fdCount * sizeof(int));
    st->savedFdCR = (unsigned char *)malloc(fdCount);
    memcpy(st->savedFds,  fds,  fdCount * sizeof(int));
    memcpy(st->savedFdCR, fdCR, fdCount);

    /* I/O redirects */
    int savedStdin = -1, savedStdout = -1;
    int redirIn = -1, redirOut = -1;
    if (inFile[0]) {
        redirIn = _open(inFile, _O_RDONLY | _O_BINARY);
        if (redirIn >= 0) { savedStdin = fds[0]; fds[0] = redirIn; }
        else fprintf(stderr, "EXEC: can't open redirect input '%s'\n", inFile);
    }
    if (outFile[0]) {
        redirOut = _open(outFile, _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY,
                         _S_IREAD | _S_IWRITE);
        if (redirOut >= 0) { savedStdout = fds[1]; fds[1] = redirOut; }
    }

    Word childPSP = execLoadChild(e, foundPath, cmdTail, envSeg, foundPath);
    if (!childPSP) {
        if (redirIn  >= 0) { _close(redirIn);  fds[0] = savedStdin;  }
        if (redirOut >= 0) { _close(redirOut); fds[1] = savedStdout; }
        memcpy(fds,  st->savedFds,  st->savedFdCount * sizeof(int));
        memcpy(fdCR, st->savedFdCR, st->savedFdCount);
        free(st->savedFds); free(st->savedFdCR);
        restoreCPU(&st->cpu);
        loadSegment = st->loadSeg;
        setPspSegment(st->pspSeg);
        setDtaSeg(st->dtaSeg);
        setDtaOff(st->dtaOff);
        return -1;
    }

    execDepth++;
    int jmpVal = setjmp(st->jmpEnv);
    int childExitCode;

    if (jmpVal == 0) {
        fflush(stdout);
        fflush(stderr);
        while (1)
            executeInstruction();
    }

    childExitCode = jmpVal - 1;
    execDepth--;

    if (f_verbose)
        fprintf(stderr, "EXEC[%d]: child exited with code %d\n", execDepth, childExitCode);

    if (redirIn  >= 0) _close(redirIn);
    if (redirOut >= 0) _close(redirOut);

    /* Restore file descriptors */
    fds   = getFds();
    fdCR  = getFdCR();
    fdCount = getFdCount();
    for (int i = 0; i < fdCount; i++) {
        if (i < st->savedFdCount) {
            if (fds[i] != st->savedFds[i] && fds[i] != -1 && fds[i] > 2)
                _close(fds[i]);
        } else {
            if (fds[i] != -1 && fds[i] > 2)
                _close(fds[i]);
        }
    }
    if (fdCount != st->savedFdCount) {
        int *newFds = (int *)malloc(st->savedFdCount * sizeof(int));
        unsigned char *newCR = (unsigned char *)malloc(st->savedFdCount);
        setFds(newFds, newCR, st->savedFdCount);
        fds = newFds; fdCR = newCR;
    }
    memcpy(fds,  st->savedFds,  st->savedFdCount * sizeof(int));
    memcpy(fdCR, st->savedFdCR, st->savedFdCount);
    free(st->savedFds); free(st->savedFdCR);

    /* Free child memory — zero both ram[] and shadowRam[] for freed blocks
       so the next child loaded at the same address won't see stale data or
       stale fRead/fWrite shadow flags from the previous occupant. */
    {
        Word seg = getMcbHead();
        while (1) {
            Byte type = mcbType(seg);
            Word owner = mcbOwner(seg);
            Word size = mcbSize(seg);
            if (owner == childPSP) {
                /* Zero the block contents (after MCB header) */
                DWord blockStart = ((DWord)seg << 4) + 16; /* skip MCB paragraph */
                DWord blockEnd   = ((DWord)(seg + 1 + size)) << 4;
                if (blockEnd > RAMSIZE) blockEnd = RAMSIZE;
                if (blockStart < blockEnd) {
                    memset(&ram[blockStart], 0, blockEnd - blockStart);
                    memset(&shadowRam[blockStart], 0, blockEnd - blockStart);
                }
                mcbWrite(seg, type, 0, size);
            }
            if (type == 'Z') break;
            seg = seg + 1 + size;
        }
        seg = getMcbHead();
        while (mcbType(seg) != 'Z') {
            Word next = seg + 1 + mcbSize(seg);
            if (mcbOwner(seg) == 0 && mcbOwner(next) == 0) {
                Word combined = mcbSize(seg) + 1 + mcbSize(next);
                mcbWrite(seg, mcbType(next), 0, combined);
            } else {
                seg = seg + 1 + mcbSize(seg);
            }
        }
    }

    restoreCPU(&st->cpu);
    loadSegment = st->loadSeg;
    setPspSegment(st->pspSeg);
    setDtaSeg(st->dtaSeg);
    setDtaOff(st->dtaOff);
    _chdir(st->cwd);

    return childExitCode;
}

/* ── Load overlay (AL=3) ─────────────────────────────────────────────── */

static int execLoadOverlay(const char *progPath)
{
    Word paramOff = bx();
    Word loadSeg  = readWordSeg(paramOff + 0, ES);
    Word relocFactor = readWordSeg(paramOff + 2, ES);

    char foundPath[MAX_PATH];
    if (!searchPathForDosExe(progPath, foundPath, sizeof(foundPath))) {
        if (f_verbose)
            fprintf(stderr, "EXEC overlay: file not found '%s'\n", progPath);
        setCF(true); setAX(2);
        return 1;
    }
    if (f_verbose)
        fprintf(stderr, "EXEC overlay: '%s' seg=%04X reloc=%04X\n",
                foundPath, loadSeg, relocFactor);

    int fd = _open(foundPath, _O_RDONLY | _O_BINARY);
    if (fd < 0) { setCF(true); setAX(2); return 1; }
    struct _stat sbuf;
    if (_fstat(fd, &sbuf) < 0) { _close(fd); setCF(true); setAX(2); return 1; }
    size_t filesize = (size_t)sbuf.st_size;
    unsigned char *fileBuf = (unsigned char *)malloc(filesize);
    if (!fileBuf) { _close(fd); setCF(true); setAX(8); return 1; }
    int n = _read(fd, fileBuf, (unsigned int)filesize);
    _close(fd);
    if (n != (int)filesize) { free(fileBuf); setCF(true); setAX(2); return 1; }

    struct image_dos_header *hdr = (struct image_dos_header *)fileBuf;
    if (filesize < 2 || hdr->e_magic != DOSMAGIC) {
        free(fileBuf); setCF(true); setAX(11); return 1;
    }

    Word headerLength = hdr->e_cparhdr << 4;
    Word bytesInLastBlock = hdr->e_cblp;
    int exeLength = ((hdr->e_cp - (bytesInLastBlock == 0 ? 0 : 1)) << 9)
        + bytesInLastBlock;
    int imageBytes = exeLength - headerLength;
    if (headerLength > (int)filesize || imageBytes < 0) {
        free(fileBuf); setCF(true); setAX(11); return 1;
    }

    DWord loadPhys = (DWord)loadSeg << 4;
    if (loadPhys + imageBytes <= RAMSIZE) {
        memcpy(&ram[loadPhys], fileBuf + headerLength, imageBytes);
        for (DWord a = loadPhys; a < loadPhys + (DWord)imageBytes && a < RAMSIZE; a++)
            shadowRam[a] |= fRead | fWrite;
    }

    struct dos_reloc *r = (struct dos_reloc *)(fileBuf + hdr->e_lfarlc);
    for (int i = 0; i < hdr->e_crlc; ++i) {
        DWord relocPhys = loadPhys + ((DWord)r->r_seg << 4) + r->r_offset;
        if (relocPhys + 1 < RAMSIZE) {
            Word val = ram[relocPhys] | (ram[relocPhys + 1] << 8);
            val += relocFactor;
            ram[relocPhys] = (Byte)val;
            ram[relocPhys + 1] = (Byte)(val >> 8);
        }
        r++;
    }

    free(fileBuf);
    setCF(false);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════
   Native command emulation — replaces CreateProcessA("cmd.exe /c ...")
   ═══════════════════════════════════════════════════════════════════════ */

/* Helper: extract base command name without path/extension */
static void cmdBaseName(const char *prog, char *out, int outSz)
{
    const char *p = strrchr(prog, '\\');
    if (p) p++; else p = prog;
    const char *f = strrchr(p, '/');
    if (f) f++; else f = p;
    int i;
    for (i = 0; f[i] && f[i] != '.' && i < outSz - 1; i++)
        out[i] = f[i];
    out[i] = '\0';
}

/* ── COPY ────────────────────────────────────────────────────────────── */

static int cmdCopy(const char *args)
{
    char buf[512];
    _snprintf(buf, sizeof(buf), "%s", args);
    buf[sizeof(buf) - 1] = '\0';

    /* Trim leading spaces */
    char *p = buf;
    while (*p == ' ') p++;

    /* Check for /b flag (binary mode — we always copy binary) */
    int binaryFlag = 0;
    while (_strnicmp(p, "/b", 2) == 0 || _strnicmp(p, "/a", 2) == 0 ||
           _strnicmp(p, "/v", 2) == 0 || _strnicmp(p, "/y", 2) == 0) {
        if (_strnicmp(p, "/b", 2) == 0) binaryFlag = 1;
        p += 2;
        while (*p == ' ') p++;
    }

    /* Split into source(s) and destination */
    /* Format: "source dest" or "src1+src2+... dest" */
    char *lastSpace = NULL;
    {
        char *q = p;
        int inQuote = 0;
        while (*q) {
            if (*q == '"') inQuote = !inQuote;
            if (*q == ' ' && !inQuote) lastSpace = q;
            q++;
        }
    }

    char dest[MAX_PATH] = "";
    char srcPart[512];
    if (lastSpace && lastSpace > p) {
        /* Trim trailing whitespace from dest */
        char *dstart = lastSpace + 1;
        while (*dstart == ' ') dstart++;
        _snprintf(dest, sizeof(dest), "%s", dstart);
        dest[sizeof(dest) - 1] = '\0';
        /* Trim trailing whitespace */
        int dlen = (int)strlen(dest);
        while (dlen > 0 && (unsigned char)dest[dlen-1] <= ' ') dest[--dlen] = '\0';

        int slen = (int)(lastSpace - p);
        if (slen >= (int)sizeof(srcPart)) slen = (int)sizeof(srcPart) - 1;
        memcpy(srcPart, p, slen);
        srcPart[slen] = '\0';
    } else {
        /* No dest — just source (copy to current dir) */
        _snprintf(srcPart, sizeof(srcPart), "%s", p);
        srcPart[sizeof(srcPart) - 1] = '\0';
        /* Trim trailing whitespace */
        int slen = (int)strlen(srcPart);
        while (slen > 0 && (unsigned char)srcPart[slen-1] <= ' ') srcPart[--slen] = '\0';
        dest[0] = '.';
        dest[1] = '\0';
    }

    /* Check for concatenation (+ in source list) */
    if (strchr(srcPart, '+')) {
        /* Binary concatenation: open dest, append each source */
        int outFd = _open(dest, _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY,
                          _S_IREAD | _S_IWRITE);
        if (outFd < 0) {
            fprintf(stderr, "copy: cannot create '%s'\n", dest);
            return 1;
        }

        char *ctx = NULL;
        char *src = strtok_s(srcPart, "+", &ctx);
        while (src) {
            while (*src == ' ') src++;
            int slen = (int)strlen(src);
            while (slen > 0 && (unsigned char)src[slen-1] <= ' ') src[--slen] = '\0';

            int inFd = _open(src, _O_RDONLY | _O_BINARY);
            if (inFd < 0) {
                fprintf(stderr, "copy: cannot open '%s'\n", src);
                _close(outFd);
                return 1;
            }
            char iobuf[4096];
            int rd;
            while ((rd = _read(inFd, iobuf, sizeof(iobuf))) > 0)
                _write(outFd, iobuf, rd);
            _close(inFd);

            src = strtok_s(NULL, "+", &ctx);
        }
        _close(outFd);
        if (f_verbose) fprintf(stderr, "  copy: concatenated -> '%s'\n", dest);
        return 0;
    }

    /* Simple copy: single source -> dest */
    if (!CopyFileA(srcPart, dest, FALSE)) {
        /* If dest is a directory, copy into it */
        DWORD attr = GetFileAttributesA(dest);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            const char *fname = strrchr(srcPart, '\\');
            if (!fname) fname = strrchr(srcPart, '/');
            if (fname) fname++; else fname = srcPart;
            char fullDest[MAX_PATH];
            _snprintf(fullDest, sizeof(fullDest), "%s\\%s", dest, fname);
            fullDest[sizeof(fullDest) - 1] = '\0';
            if (!CopyFileA(srcPart, fullDest, FALSE)) {
                fprintf(stderr, "copy: failed '%s' -> '%s'\n", srcPart, fullDest);
                return 1;
            }
        } else {
            fprintf(stderr, "copy: failed '%s' -> '%s'\n", srcPart, dest);
            return 1;
        }
    }
    if (f_verbose) fprintf(stderr, "  copy: '%s' -> '%s'\n", srcPart, dest);
    return 0;
}

/* ── DEL / ERASE ─────────────────────────────────────────────────────── */

static int cmdDel(const char *args)
{
    char path[MAX_PATH];
    const char *p = args;
    while (*p == ' ' || *p == '\t') p++;
    _snprintf(path, sizeof(path), "%s", p);
    path[sizeof(path) - 1] = '\0';
    int len = (int)strlen(path);
    while (len > 0 && (unsigned char)path[len-1] <= ' ') path[--len] = '\0';

    /* Handle wildcards */
    if (strchr(path, '*') || strchr(path, '?')) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(path, &fd);
        if (h == INVALID_HANDLE_VALUE) return 0; /* no files to delete is OK */

        /* Get directory part */
        char dir[MAX_PATH] = "";
        const char *lastSep = strrchr(path, '\\');
        if (!lastSep) lastSep = strrchr(path, '/');
        if (lastSep) {
            int dlen = (int)(lastSep - path + 1);
            memcpy(dir, path, dlen);
            dir[dlen] = '\0';
        }

        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                char full[MAX_PATH];
                _snprintf(full, sizeof(full), "%s%s", dir, fd.cFileName);
                full[sizeof(full) - 1] = '\0';
                DeleteFileA(full);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        return 0;
    }

    if (DeleteFileA(path) || GetLastError() == ERROR_FILE_NOT_FOUND)
        return 0;
    fprintf(stderr, "del: cannot delete '%s'\n", path);
    return 1;
}

/* ── REN / RENAME ────────────────────────────────────────────────────── */

static int cmdRen(const char *args)
{
    char buf[512];
    _snprintf(buf, sizeof(buf), "%s", args);
    buf[sizeof(buf) - 1] = '\0';
    char *p = buf;
    while (*p == ' ') p++;
    char *oldName = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = '\0';
    while (*p == ' ') p++;
    char *newName = p;
    int len = (int)strlen(newName);
    while (len > 0 && (unsigned char)newName[len-1] <= ' ') newName[--len] = '\0';

    if (!*oldName || !*newName) {
        fprintf(stderr, "ren: syntax error\n");
        return 1;
    }
    if (rename(oldName, newName) != 0) {
        fprintf(stderr, "ren: '%s' -> '%s' failed\n", oldName, newName);
        return 1;
    }
    return 0;
}

/* ── TYPE ────────────────────────────────────────────────────────────── */

static int cmdType(const char *args)
{
    char path[MAX_PATH];
    const char *p = args;
    while (*p == ' ') p++;
    _snprintf(path, sizeof(path), "%s", p);
    path[sizeof(path) - 1] = '\0';
    int len = (int)strlen(path);
    while (len > 0 && (unsigned char)path[len-1] <= ' ') path[--len] = '\0';

    int fd = _open(path, _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        fprintf(stderr, "type: cannot open '%s'\n", path);
        return 1;
    }
    char buf[4096];
    int rd;
    int outFd = getFds()[1];
    while ((rd = _read(fd, buf, sizeof(buf))) > 0)
        _write(outFd, buf, rd);
    _close(fd);
    return 0;
}

/* ── ECHO ────────────────────────────────────────────────────────────── */

static int cmdEcho(const char *args)
{
    const char *p = args;
    while (*p == ' ') p++;
    /* "echo." = blank line, "echo off" / "echo on" = no-op for batch compat */
    if (_stricmp(p, "off") == 0 || _stricmp(p, "on") == 0)
        return 0;
    int outFd = getFds()[1];
    int len = (int)strlen(p);
    /* Trim trailing whitespace */
    while (len > 0 && (unsigned char)p[len-1] <= ' ') len--;
    if (len > 0)
        _write(outFd, p, len);
    _write(outFd, "\r\n", 2);
    return 0;
}

/* ── CD / CHDIR ──────────────────────────────────────────────────────── */

static int cmdCd(const char *args)
{
    char dir[MAX_PATH];
    const char *p = args;
    while (*p == ' ') p++;
    _snprintf(dir, sizeof(dir), "%s", p);
    dir[sizeof(dir) - 1] = '\0';
    int len = (int)strlen(dir);
    while (len > 0 && (unsigned char)dir[len-1] <= ' ') dir[--len] = '\0';

    if (len == 0) {
        /* Just print CWD */
        char cwd[MAX_PATH];
        if (_getcwd(cwd, sizeof(cwd)))
            fprintf(stdout, "%s\r\n", cwd);
        return 0;
    }
    if (_chdir(dir) != 0) {
        fprintf(stderr, "cd: '%s' - %s\n", dir, strerror(errno));
        return 1;
    }
    return 0;
}

/* ── SET ─────────────────────────────────────────────────────────────── */

static int cmdSet(const char *args)
{
    const char *p = args;
    while (*p == ' ') p++;
    if (!*p) return 0; /* bare SET — ignore */

    /* SET VAR=VALUE — apply to host process environment */
    char buf[512];
    _snprintf(buf, sizeof(buf), "%s", p);
    buf[sizeof(buf) - 1] = '\0';
    int len = (int)strlen(buf);
    while (len > 0 && (unsigned char)buf[len-1] <= ' ') buf[--len] = '\0';

    char *eq = strchr(buf, '=');
    if (!eq) return 0;
    *eq = '\0';
    const char *val = eq + 1;
    if (*val)
        SetEnvironmentVariableA(buf, val);
    else
        SetEnvironmentVariableA(buf, NULL); /* unset */
    return 0;
}

/* ── MKDIR / MD ──────────────────────────────────────────────────────── */

static int cmdMkdir(const char *args)
{
    char dir[MAX_PATH];
    const char *p = args;
    while (*p == ' ') p++;
    _snprintf(dir, sizeof(dir), "%s", p);
    dir[sizeof(dir) - 1] = '\0';
    int len = (int)strlen(dir);
    while (len > 0 && (unsigned char)dir[len-1] <= ' ') dir[--len] = '\0';
    if (_mkdir(dir) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir: '%s' failed\n", dir);
        return 1;
    }
    return 0;
}

/* ── RMDIR / RD ──────────────────────────────────────────────────────── */

static int cmdRmdir(const char *args)
{
    char dir[MAX_PATH];
    const char *p = args;
    while (*p == ' ') p++;
    _snprintf(dir, sizeof(dir), "%s", p);
    dir[sizeof(dir) - 1] = '\0';
    int len = (int)strlen(dir);
    while (len > 0 && (unsigned char)dir[len-1] <= ' ') dir[--len] = '\0';
    if (_rmdir(dir) != 0) {
        fprintf(stderr, "rmdir: '%s' failed\n", dir);
        return 1;
    }
    return 0;
}

/* ── ATTRIB ──────────────────────────────────────────────────────────── */

static int cmdAttrib(const char *args)
{
    const char *p = args;
    while (*p == ' ') p++;

    /* Parse +R/-R +H/-H +S/-S +A/-A flags then filename */
    DWORD setMask = 0, clearMask = 0;
    while (*p == '+' || *p == '-') {
        int set = (*p == '+');
        p++;
        char flag = *p;
        if (flag) p++;
        while (*p == ' ') p++;
        DWORD bit = 0;
        switch (flag | 0x20) {
            case 'r': bit = FILE_ATTRIBUTE_READONLY; break;
            case 'h': bit = FILE_ATTRIBUTE_HIDDEN;   break;
            case 's': bit = FILE_ATTRIBUTE_SYSTEM;    break;
            case 'a': bit = FILE_ATTRIBUTE_ARCHIVE;   break;
        }
        if (set) setMask |= bit; else clearMask |= bit;
    }

    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%s", p);
    path[sizeof(path) - 1] = '\0';
    int len = (int)strlen(path);
    while (len > 0 && (unsigned char)path[len-1] <= ' ') path[--len] = '\0';

    if (!*path) return 0;

    DWORD cur = GetFileAttributesA(path);
    if (cur == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "attrib: '%s' not found\n", path);
        return 1;
    }
    DWORD newAttr = (cur & ~clearMask) | setMask;
    if (newAttr == 0) newAttr = FILE_ATTRIBUTE_NORMAL;
    if (!SetFileAttributesA(path, newAttr)) {
        fprintf(stderr, "attrib: failed on '%s'\n", path);
        return 1;
    }
    return 0;
}

/* ── CLS — no-op in non-interactive builds ───────────────────────────── */
static int cmdCls(const char *args) { (void)args; return 0; }

/* ── VER — print DOS version ─────────────────────────────────────────── */
static int cmdVer(const char *args) {
    (void)args;
    int outFd = getFds()[1];
    const char *msg = "MS-DOS Version 4.00\r\n";
    _write(outFd, msg, (unsigned int)strlen(msg));
    return 0;
}

/* ── No-op stubs for batch flow commands that don't apply ────────────── */
static int cmdNoop(const char *args) { (void)args; return 0; }

/* ── Dispatch table ──────────────────────────────────────────────────── */

typedef int (*nativeCmdFn)(const char *args);

static struct { const char *name; nativeCmdFn fn; } nativeCommands[] = {
    { "copy",    cmdCopy   },
    { "del",     cmdDel    },
    { "erase",   cmdDel    },
    { "ren",     cmdRen    },
    { "rename",  cmdRen    },
    { "type",    cmdType   },
    { "echo",    cmdEcho   },
    { "cd",      cmdCd     },
    { "chdir",   cmdCd     },
    { "set",     cmdSet    },
    { "md",      cmdMkdir  },
    { "mkdir",   cmdMkdir  },
    { "rd",      cmdRmdir  },
    { "rmdir",   cmdRmdir  },
    { "attrib",  cmdAttrib },
    { "cls",     cmdCls    },
    { "ver",     cmdVer    },
    { "lib",     (nativeCmdFn)nativeLib },
    { "path",    cmdSet    },   /* PATH x  ≈  SET PATH=x */
    { "break",   cmdNoop   },
    { "verify",  cmdNoop   },
    { "ctty",    cmdNoop   },
    { "pause",   cmdNoop   },
    { "rem",     cmdNoop   },
    { "if",      cmdNoop   },   /* build system doesn't rely on IF in COMMAND /c */
    { "for",     cmdNoop   },
    { "goto",    cmdNoop   },
    { "shift",   cmdNoop   },
    { "call",    cmdNoop   },
    { "vol",     cmdNoop   },
    { "date",    cmdNoop   },
    { "time",    cmdNoop   },
    { "dir",     cmdNoop   },
    { NULL, NULL }
};

/* Look up and run a native command.  Returns -1 if not a native command. */
int runNativeCommand(const char *cmdName, const char *args)
{
    char name[64];
    cmdBaseName(cmdName, name, sizeof(name));

    for (int i = 0; nativeCommands[i].name; i++) {
        if (_stricmp(name, nativeCommands[i].name) == 0)
            return nativeCommands[i].fn(args);
    }
    return -1;  /* not a native command */
}

static int isNativeCommand(const char *prog)
{
    char name[64];
    cmdBaseName(prog, name, sizeof(name));
    for (int i = 0; nativeCommands[i].name; i++)
        if (_stricmp(name, nativeCommands[i].name) == 0) return 1;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   Top-level EXEC handler — called from INT 21h/4Bh in syscall-dos.c
   ═══════════════════════════════════════════════════════════════════════ */

int handleExec(struct exe *e)
{
    /* Program path from DS:DX (caller already set up) */
    char progPath[MAX_PATH];
    {
        Word off = dx();
        int i;
        for (i = 0; i < MAX_PATH - 1; i++) {
            Byte b = ram[physicalAddress(off + i, DS, false)];
            if (b == 0) break;
            progPath[i] = (char)b;
        }
        progPath[i] = '\0';
    }

    /* AL=3: overlay load */
    if (al() == 3)
        return execLoadOverlay(progPath);

    if (al() != 0) {
        if (f_verbose)
            fprintf(stderr, "EXEC: unsupported subfunction AL=%02x\n", al());
        setCF(true); setAX(1);
        return 1;
    }

    /* Read parameter block at ES:BX */
    Word paramOff = bx();
    Word envSeg   = readWordSeg(paramOff + 0, ES);
    Word cmdOff   = readWordSeg(paramOff + 2, ES);
    Word cmdSeg   = readWordSeg(paramOff + 4, ES);

    Byte cmdLen = ram[((DWord)cmdSeg << 4) + cmdOff];
    char cmdTail[256];
    for (int i = 0; i < cmdLen && i < 255; i++)
        cmdTail[i] = (char)ram[((DWord)cmdSeg << 4) + cmdOff + 1 + i];
    cmdTail[cmdLen] = '\0';

    if (f_verbose)
        fprintf(stderr, "EXEC: prog='%s' tail='%s'\n", progPath, cmdTail);

    /* ── Detect COMMAND.COM /c <command> ─────────────────────────────── */
    const char *base = progPath;
    const char *slash = strrchr(progPath, '\\');
    if (slash) base = slash + 1;
    const char *fslash = strrchr(base, '/');
    if (fslash) base = fslash + 1;

    if (_stricmp(base, "COMMAND.COM") == 0 || _stricmp(base, "CMD.EXE") == 0) {
        char *t = cmdTail;
        while (*t == ' ' || *t == '\t') t++;
        if ((_strnicmp(t, "/c", 2) == 0 || _strnicmp(t, "-c", 2) == 0)
            && (t[2] == ' ' || t[2] == '\t' || t[2] == '\0')) {
            t += 2;
            while (*t == ' ' || *t == '\t') t++;

            /* Parse first token as program, rest as args */
            char actualProg[260];
            int pi2 = 0;
            while (*t && *t != ' ' && *t != '\t' && pi2 < 259)
                actualProg[pi2++] = *t++;
            actualProg[pi2] = '\0';
            char *restArgs = t;

            /* Parse redirections from args before running commands */
            char nativeArgs[512];
            char inFile[MAX_PATH], outFile[MAX_PATH];
            _snprintf(nativeArgs, sizeof(nativeArgs), "%s", restArgs);
            nativeArgs[sizeof(nativeArgs) - 1] = '\0';
            parseRedirects(nativeArgs, inFile, sizeof(inFile), outFile, sizeof(outFile));

            /* Set up I/O redirections */
            int *fds = getFds();
            int savedStdin = -1, savedStdout = -1;
            int redirIn = -1, redirOut = -1;
            if (inFile[0]) {
                redirIn = _open(inFile, _O_RDONLY | _O_BINARY);
                if (redirIn >= 0) { savedStdin = fds[0]; fds[0] = redirIn; }
                else if (f_verbose) fprintf(stderr, "EXEC: can't open redirect input '%s'\n", inFile);
            }
            if (outFile[0]) {
                redirOut = _open(outFile, _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY,
                                 _S_IREAD | _S_IWRITE);
                if (redirOut >= 0) { savedStdout = fds[1]; fds[1] = redirOut; }
            }

            /* Try as native command first (use args with redirects stripped) */
            int nrc = runNativeCommand(actualProg, nativeArgs);
            if (nrc >= 0) {
                if (redirIn  >= 0) { _close(redirIn);  fds[0] = savedStdin;  }
                if (redirOut >= 0) { _close(redirOut); fds[1] = savedStdout; }
                setLastChildExitCode(nrc);
                setCF(false);
                return 1;
            }
            /* Restore redirections before execInProcess (it handles its own) */
            if (redirIn  >= 0) { _close(redirIn);  fds[0] = savedStdin;  }
            if (redirOut >= 0) { _close(redirOut); fds[1] = savedStdout; }

            /* Try as DOS executable */
            char foundPath[MAX_PATH];
            if (searchPathForDosExe(actualProg, foundPath, sizeof(foundPath))) {
                char childTail[512];
                _snprintf(childTail, sizeof(childTail), "%s", restArgs);
                childTail[sizeof(childTail) - 1] = '\0';
                int rc = execInProcess(e, actualProg, childTail, 0);
                setLastChildExitCode((rc >= 0) ? rc : 1);
                setCF(false);
            } else {
                /* Unknown command — not a DOS exe, not a native command */
                if (f_verbose)
                    fprintf(stderr, "EXEC: unknown command '%s'\n", actualProg);
                setLastChildExitCode(1);
                setCF(false);
            }
        } else {
            /* COMMAND.COM without /c — no-op */
            setCF(false);
        }
        return 1;
    }

    /* ── Direct DOS executable ───────────────────────────────────────── */
    int rc = execInProcess(e, progPath, cmdTail, envSeg);
    if (rc >= 0) {
        setLastChildExitCode(rc);
        setCF(false);
    } else {
        setCF(true);
        setAX(2);
    }
    return 1;
}
