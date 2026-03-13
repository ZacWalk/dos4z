/* exec-dos.h — DOS EXEC (INT 21h/4Bh) handler and shell command emulation */
#ifndef EXEC_DOS_H
#define EXEC_DOS_H

#include "8086.h"
#include "exe.h"

/* ── Shared state accessors (owned by syscall-dos.c) ─────────────────── */

/* DOS MCB arena */
void     mcbWrite(Word seg, Byte type, Word owner, Word size);
Byte     mcbType(Word seg);
Word     mcbOwner(Word seg);
Word     mcbSize(Word seg);
Word     dosAllocMem(Word paragraphs);
int      dosFreeMem(Word seg);
int      dosResizeMem(Word seg, Word newSize);
Word     getMcbHead(void);
Word     getPspSegment(void);
void     setMcbHead(Word seg);
void     setPspSegment(Word seg);

/* DTA */
Word     getDtaSeg(void);
Word     getDtaOff(void);
void     setDtaSeg(Word seg);
void     setDtaOff(Word off);

/* File descriptors */
int     *getFds(void);
int      getFdCount(void);
unsigned char *getFdCR(void);
void     setFds(int *fds, unsigned char *cr, int count);
int      getFreeFd(void);

/* Exit */
int      sysExit(struct exe *e, int rc);

/* Last child exit code */
int      getLastChildExitCode(void);
void     setLastChildExitCode(int rc);

/* Native command dispatch (for LIB.EXE etc.) */
int      runNativeCommand(const char *cmdName, const char *args);

/* ── EXEC handler (called from INT 21h/4Bh) ─────────────────────────── */

/* Handle INT 21h/4Bh.  Sets CF/AX as appropriate.  Returns 1. */
int handleExec(struct exe *e);

#endif /* EXEC_DOS_H */
