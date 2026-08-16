/* 8086 emulator header file - Windows/MSVC compatible */
#ifndef EMU8086_H
#define EMU8086_H

#include <stdint.h>

typedef uint8_t  Byte;
typedef uint16_t Word;
typedef uint32_t DWord;

#ifndef __cplusplus
#ifndef false
#define false 0
#define true  1
#endif
typedef int bool;
#endif

/* segment registers after 8 general registers */
enum { ES = 0, CS, SS, DS };

/* emulator globals */
#define RAMSIZE     0x100000    /* 1M RAM */
extern Word registers[12];
extern Byte* byteRegisters[8];
extern Byte ram[RAMSIZE];
extern Byte shadowRam[RAMSIZE];  /* per-byte fRead/fWrite access flags */
extern int f_verbose;

/* emulator operation */
struct exe;                     /* defined in exe.h */
int initMachine(struct exe *e);
void initExecute(void);
void executeInstruction(void);
int isRepeating(void);

/* emulator callouts */
void runtimeError(const char *msg, ...);
void handleInterrupt(struct exe *e, int intno);
int checkStackDOS(struct exe *e);
int handleSyscallDOS(struct exe *e, int intno);

/* memory access functions */
Byte readByte(Word offset, int seg);
Word readWordSeg(Word offset, int seg);
void writeByte(Byte value, Word offset, int seg);
void writeWord(Word value, Word offset, int seg);
DWord physicalAddress(Word offset, int seg, int write);
#define fRead   0x01
#define fWrite  0x02
void setShadowFlags(Word offset, int seg, int len, int flags);

#define INT0_DIV_ERROR  0
#define INT3_BREAKPOINT 3
#define INT4_OVERFLOW   4

/* register access functions */
static __inline Word ax(void) { return registers[0]; }
static __inline Word cx(void) { return registers[1]; }
static __inline Word dx(void) { return registers[2]; }
static __inline Word bx(void) { return registers[3]; }
static __inline Word sp(void) { return registers[4]; }
static __inline Word bp(void) { return registers[5]; }
static __inline Word si(void) { return registers[6]; }
static __inline Word di(void) { return registers[7]; }
static __inline Word es(void) { return registers[8]; }
static __inline Word cs(void) { return registers[9]; }
static __inline Word ss(void) { return registers[10]; }
static __inline Word ds(void) { return registers[11]; }
static __inline Byte al(void) { return *byteRegisters[0]; }
static __inline Byte cl(void) { return *byteRegisters[1]; }
static __inline Byte dl(void) { return *byteRegisters[2]; }
static __inline Byte bl(void) { return *byteRegisters[3]; }
static __inline Byte ah(void) { return *byteRegisters[4]; }
static __inline Byte ch(void) { return *byteRegisters[5]; }
static __inline Byte dh(void) { return *byteRegisters[6]; }
static __inline Byte bh(void) { return *byteRegisters[7]; }
static __inline void setAX(Word value) { registers[0] = value; }
static __inline void setCX(Word value) { registers[1] = value; }
static __inline void setDX(Word value) { registers[2] = value; }
static __inline void setBX(Word value) { registers[3] = value; }
static __inline void setSP(Word value) { registers[4] = value; }
static __inline void setBP(Word value) { registers[5] = value; }
static __inline void setSI(Word value) { registers[6] = value; }
static __inline void setDI(Word value) { registers[7] = value; }
static __inline void setES(Word value) { registers[8] = value; }
static __inline void setCS(Word value) { registers[9] = value; }
static __inline void setSS(Word value) { registers[10] = value; }
static __inline void setDS(Word value) { registers[11] = value; }
static __inline void setAL(Byte value) { *byteRegisters[0] = value; }
static __inline void setCL(Byte value) { *byteRegisters[1] = value; }
static __inline void setDL(Byte value) { *byteRegisters[2] = value; }
static __inline void setBL(Byte value) { *byteRegisters[3] = value; }
static __inline void setAH(Byte value) { *byteRegisters[4] = value; }
static __inline void setCH(Byte value) { *byteRegisters[5] = value; }
static __inline void setDH(Byte value) { *byteRegisters[6] = value; }
static __inline void setBH(Byte value) { *byteRegisters[7] = value; }
/* DOS memory allocator */
void initDOSMemory(Word progSeg, Word freeEnd);

Word getIP(void);
void setIP(Word w);
void setFlags(Word w);
Word getFlags(void);
void setCF(int cf);

/* CPU state snapshot for nested EXEC */
struct cpuState {
    Word regs[12];      /* all registers */
    Word ip;
    Word flags;
};

void saveCPU(struct cpuState *s);
void restoreCPU(const struct cpuState *s);

#endif /* EMU8086_H */
