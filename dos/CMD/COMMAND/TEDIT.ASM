 page 80,132
TITLE	COMMAND Transient Line Editor

;	4.0z: the line editor of the interactive prompt, with a history of
;	the command lines typed at it.  It reads the line that REGCOM in
;	TCODE.ASM used to read with INT 21h AH=0Ah.  Its code is transient;
;	only the history, HistBuf in RDATA.ASM, stays resident.

.xlist
.xcref
	INCLUDE comsw.asm
	INCLUDE DOSSYM.INC
	INCLUDE comseg.asm
	INCLUDE comequ.asm
.list
.cref

DATARES 	SEGMENT PUBLIC BYTE
	EXTRN	HistBuf:BYTE
	EXTRN	HistEnd:WORD
	EXTRN	HistTop:BYTE
DATARES 	ENDS

TRANSPACE	SEGMENT PUBLIC BYTE
	EXTRN	EdCol:BYTE
	EXTRN	EdCols:BYTE
	EXTRN	EdHist:WORD
	EXTRN	EdIns:BYTE
	EXTRN	EdLen:BYTE
	EXTRN	EdPage:BYTE
	EXTRN	EdPos:BYTE
	EXTRN	EdRow:BYTE
	EXTRN	EdShown:BYTE
	EXTRN	RESSEG:WORD
	EXTRN	UCOMBUF:BYTE
TRANSPACE	ENDS

TRANCODE	SEGMENT PUBLIC BYTE

ASSUME	CS:TRANGROUP,DS:TRANGROUP,ES:NOTHING,SS:TRANGROUP

;
;   EditLine - read a command line into UCOMBUF, as INT 21h AH=0Ah does.
;
;   When standard input and output are both the console, the line is
;   edited here:
;
;	Left, Right		move the cursor; Right at the end copies a
;				character from the last line, as F1 does
;	Home, End		go to the start or the end of the line
;	Backspace, Del		delete the character before or at the cursor
;	Ins			insert rather than overwrite, for this line
;	Esc			clear the line
;	Up, Down		an earlier or later line from the history
;	F1, F3			copy one or all of the characters of the last
;				line from the cursor's place on
;
;   A Tab stays a Tab in the line, as with INT 21h AH=0Ah, but is shown as
;   one space.  With input redirected, or after CTTY, INT 21h AH=0Ah reads
;   the line instead.
;
;   The history keeps the lines that are not empty, without repeating
;   the last one, and loses the oldest when it is full.  The editor
;   keeps ES = TRANGROUP; a routine that changes it puts it back.
;
;   Inputs:	DS = TRANGROUP, UCOMBUF+0 = COMBUFLEN
;   Outputs:	UCOMBUF+1 = length, and the line and a CR from UCOMBUF+2
;   Registers modified: AX, BX, CX, DX, SI, DI
;

Procedure EditLine,NEAR
	PUSH	ES
	PUSH	BP
	MOV	AX,IOCTL SHL 8			; is standard input the console?
	XOR	BX,BX
	INT	int_command
	JC	EdDos
	AND	DL,devid_ISDEV + devid_ISCIN
	CMP	DL,devid_ISDEV + devid_ISCIN
	JNZ	EdDos
	MOV	AX,IOCTL SHL 8			; and standard output?
	MOV	BX,1
	INT	int_command
	JC	EdDos
	AND	DL,devid_ISDEV + devid_ISCOT
	CMP	DL,devid_ISDEV + devid_ISCOT
	JZ	EdStart
EdDos:
	MOV	DX,OFFSET TRANGROUP:UCOMBUF
	MOV	AH,STD_CON_STRING_INPUT
	INT	int_command
EdExit:
	POP	BP
	POP	ES
	return

EdStart:
	PUSH	CS
	POP	ES
	XOR	AX,AX
	MOV	[EdLen],AL
	MOV	[EdPos],AL
	MOV	[EdShown],AL
	MOV	[EdIns],AL
	CALL	EdNewest			; Up starts from the newest line
	MOV	AH,0Fh				; AH = columns, BH = video page
	INT	10h
	OR	AH,AH
	JNZ	EdHaveCols
	MOV	AH,80
EdHaveCols:
	MOV	[EdCols],AH
	MOV	[EdPage],BH
	MOV	AH,3				; the line starts at the cursor
	INT	10h
	MOV	[EdRow],DH
	MOV	[EdCol],DL

EdKey:
	MOV	AH,STD_CON_INPUT_NO_ECHO	; DOS sees to ^C, ^S and ^P
	INT	int_command
	OR	AL,AL
	JZ	EdExtKey
	CMP	AL,0Dh
	JZ	EdEnter
	MOV	BX,OFFSET TRANGROUP:EdBack
	CMP	AL,08h
	JZ	EdDo
	CMP	AL,7Fh				; Ctrl-Backspace
	JZ	EdDo
	MOV	BX,OFFSET TRANGROUP:EdEsc
	CMP	AL,1Bh
	JZ	EdDo
	CMP	AL,09h				; Tab is kept, and shown as a space
	JZ	EdTypeKey
	CMP	AL,' '
	JB	EdKey				; other control keys do nothing
EdTypeKey:
	CALL	EdType
	JMP	EdKey

EdExtKey:
	MOV	AH,STD_CON_INPUT_NO_ECHO	; the key's scan code
	INT	int_command
	MOV	SI,OFFSET TRANGROUP:EdKeys
EdFind:
	CMP	BYTE PTR [SI],0
	JZ	EdKey				; a key the editor does not use
	CMP	AL,[SI]
	JZ	EdFound
	ADD	SI,3
	JMP	EdFind
EdFound:
	MOV	BX,[SI+1]
EdDo:
	CALL	BX
	JMP	EdKey

EdEnter:
	MOV	AL,[EdLen]
	CALL	EdGoto				; leave the cursor after the line
	MOV	BL,[EdLen]
	XOR	BH,BH
	MOV	[UCOMBUF+1],BL
	MOV	BYTE PTR UCOMBUF[BX+2],0Dh
	MOV	DL,0Dh				; and go back, as INT 21h AH=0Ah
	CALL	EdPut				;  does
	CALL	EdSave
	JMP	EdExit

EndProc EditLine

;
; The extended keys, by scan code
;
EdKeys	LABEL	BYTE
	DB	4Bh
	DW	OFFSET TRANGROUP:EdLeft
	DB	4Dh
	DW	OFFSET TRANGROUP:EdRight
	DB	47h
	DW	OFFSET TRANGROUP:EdHome
	DB	4Fh
	DW	OFFSET TRANGROUP:EdEnd
	DB	53h
	DW	OFFSET TRANGROUP:EdDelete
	DB	52h
	DW	OFFSET TRANGROUP:EdInsert
	DB	48h
	DW	OFFSET TRANGROUP:EdUp
	DB	50h
	DW	OFFSET TRANGROUP:EdDown
	DB	3Bh
	DW	OFFSET TRANGROUP:EdCopy1	; F1
	DB	3Dh
	DW	OFFSET TRANGROUP:EdCopyAll	; F3
	DB	0

;
; EdType - put the character in AL at the cursor, over the one there or, in
; insert mode, in front of it.  Carry set, and a beep, if the line is full.
;
EdType:
	MOV	BL,[EdPos]
	XOR	BH,BH
	MOV	CL,[EdLen]
	XOR	CH,CH
	CMP	BX,CX
	JB	EdTypeIn			; within the line
	CMP	CX,COMBUFLEN-1
	JAE	EdFull				; no room after it
	INC	[EdLen]
	JMP	SHORT EdTypePut
EdTypeIn:
	CMP	[EdIns],0
	JZ	EdTypePut			; overwrite the one there
	CMP	CX,COMBUFLEN-1
	JAE	EdFull
	MOV	DI,CX				; move the rest of the line up one
	ADD	DI,OFFSET TRANGROUP:UCOMBUF+2
	MOV	SI,DI
	DEC	SI
	SUB	CX,BX
	STD
	REP	MOVSB
	CLD
	INC	[EdLen]
EdTypePut:
	MOV	UCOMBUF[BX+2],AL
	INC	[EdPos]
	MOV	AL,BL
	CALL	EdShow
	CLC
	RET
EdFull:
	MOV	DL,7				; beep
	CALL	EdPut
	STC
	RET

;
; Backspace - delete the character before the cursor
;
EdBack:
	CMP	[EdPos],0
	JZ	EdNone
	DEC	[EdPos]
;
; Del - delete the character at the cursor
;
EdDelete:
	MOV	BL,[EdPos]
	XOR	BH,BH
	MOV	CL,[EdLen]
	XOR	CH,CH
	SUB	CX,BX
	JBE	EdNone				; there is none
	DEC	CX				; the characters after it
	LEA	DI,UCOMBUF[BX+2]
	LEA	SI,UCOMBUF[BX+3]
	REP	MOVSB
	DEC	[EdLen]
	MOV	AL,BL
	JMP	EdShow
EdNone:
	RET

;
; Esc - clear the line
;
EdEsc:
	CALL	EdNewest
	XOR	AL,AL
	MOV	[EdLen],AL
	MOV	[EdPos],AL
	JMP	EdShow

;
; Left, Right, Home, End - move the cursor
;
EdLeft:
	MOV	AL,[EdPos]
	OR	AL,AL
	JZ	EdNone
	DEC	AL
EdMove:
	MOV	[EdPos],AL
	JMP	EdGoto
EdRight:
	MOV	AL,[EdPos]
	CMP	AL,[EdLen]
	JAE	EdCopy1 			; at the end: as F1
	INC	AL
	JMP	EdMove
EdHome:
	XOR	AL,AL
	JMP	EdMove
EdEnd:
	MOV	AL,[EdLen]
	JMP	EdMove

;
; Ins - insert or overwrite
;
EdInsert:
	XOR	[EdIns],1
	RET

;
; F1 - copy the character of the last line at the cursor's place
;
EdCopy1:
	CALL	EdTmplChar
	JC	EdNone
	JMP	EdType

;
; F3 - copy the rest of the last line from the cursor's place
;
EdCopyAll:
	CALL	EdTmplChar
	JC	EdNone
	CALL	EdType
	JNC	EdCopyAll
	RET

;
; EdTmplChar - AL = the character of the newest history line at the
; cursor's place; carry set if it has none there.  Uses BX, CX, DI.
;
EdTmplChar:
	PUSH	ES
	MOV	ES,[RESSEG]
	MOV	DI,OFFSET RESGROUP:HistEnd
	MOV	BX,ES:[DI]
	CMP	BX,OFFSET RESGROUP:HistBuf
	JBE	EdTmplNone			; the history is empty
	CALL	EdPrev				; BX = the newest line
	MOV	DI,BX
	MOV	CL,[EdPos]
	XOR	CH,CH
	MOV	AL,0Dh
	JCXZ	EdTmplAt
	REPNE	SCASB				; past the characters before
	JZ	EdTmplNone			;  the place, unless it ends
EdTmplAt:
	MOV	AL,ES:[DI]
	CMP	AL,0Dh
	JZ	EdTmplNone
	POP	ES
	CLC
	RET
EdTmplNone:
	POP	ES
	STC
	RET

;
; Up - the history line before the one shown
;
EdUp:
	PUSH	ES
	MOV	ES,[RESSEG]
	MOV	BX,[EdHist]
	CMP	BX,OFFSET RESGROUP:HistBuf
	JBE	EdHistDone			; the oldest, or no history
	CALL	EdPrev
	JMP	SHORT EdLoad

;
; Down - the history line after the one shown, then an empty line
;
EdDown:
	PUSH	ES
	MOV	ES,[RESSEG]
	MOV	BX,[EdHist]
	MOV	DI,OFFSET RESGROUP:HistEnd
	CMP	BX,ES:[DI]
	JAE	EdHistDone			; none is shown
	CALL	EdNext
;
; EdLoad - make the history line at BX the line; none if BX = HistEnd
;
EdLoad:
	MOV	[EdHist],BX
	MOV	SI,BX
	MOV	DI,OFFSET RESGROUP:HistEnd
	MOV	DX,ES:[DI]
	MOV	DI,OFFSET TRANGROUP:UCOMBUF+2
	XOR	CX,CX
	PUSH	DS
	PUSH	ES
	POP	DS
	PUSH	CS
	POP	ES
EdLoadCh:
	CMP	SI,DX
	JAE	EdLoaded			; after the newest line
	CMP	CX,COMBUFLEN-1
	JAE	EdLoaded
	LODSB
	CMP	AL,0Dh
	JZ	EdLoaded
	STOSB
	INC	CX
	JMP	EdLoadCh
EdLoaded:
	POP	DS
	MOV	[EdLen],CL
	MOV	[EdPos],CL
	XOR	AL,AL
	CALL	EdShow
EdHistDone:
	POP	ES
	RET

;
; EdNewest - show no history line: EdHist = HistEnd.  A HistEnd outside
; HistBuf, from the resident data being overwritten, empties the history.
; Uses AX, DI.
;
EdNewest:
	PUSH	ES
	MOV	ES,[RESSEG]
	MOV	DI,OFFSET RESGROUP:HistEnd
	MOV	AX,ES:[DI]
	CMP	AX,OFFSET RESGROUP:HistBuf
	JB	EdNewReset
	CMP	AX,OFFSET RESGROUP:HistTop
	JBE	EdNewSet
EdNewReset:
	MOV	AX,OFFSET RESGROUP:HistBuf
	MOV	ES:[DI],AX
EdNewSet:
	MOV	[EdHist],AX
	POP	ES
	RET

;
; EdPrev - BX = where the history line before the one at BX starts.
; ES = the resident segment, BX after HistBuf.  Uses AX, CX, DI.
;
EdPrev:
	MOV	DI,BX
	DEC	DI
	DEC	DI				; the last character of that line
	MOV	CX,DI
	SUB	CX,OFFSET RESGROUP:HistBuf - 1	; characters up to it
	MOV	AL,0Dh
	STD
	REPNE	SCASB
	CLD
	MOV	BX,OFFSET RESGROUP:HistBuf	; no CR before it: the oldest
	JNZ	EdPrevDone
	LEA	BX,[DI+2]			; just after the CR before it
EdPrevDone:
	RET

;
; EdNext - BX = where the history line after the one at BX starts.
; ES = the resident segment.  Uses AX, CX, DI.
;
EdNext:
	MOV	DI,BX
	MOV	CX,COMBUFLEN			; no line is longer
	MOV	AL,0Dh
	REPNE	SCASB
	MOV	BX,DI
	RET

;
; EdSave - add the line in UCOMBUF, which ends in a CR, to the history,
; unless it is empty or the same as the newest line.  The oldest lines go
; to make room for it.
;
EdSave:
	MOV	CL,[EdLen]
	XOR	CH,CH
	JCXZ	EdSaveRet
	PUSH	ES
	MOV	ES,[RESSEG]
	MOV	DI,OFFSET RESGROUP:HistEnd
	MOV	DX,ES:[DI]			; DX = HistEnd
	MOV	BX,DX
	CMP	BX,OFFSET RESGROUP:HistBuf
	JBE	EdSaveFit			; the history is empty
	PUSH	CX
	CALL	EdPrev				; BX = the newest line
	POP	CX
	MOV	DI,BX
	MOV	SI,OFFSET TRANGROUP:UCOMBUF+2
	PUSH	CX
	INC	CX				; with the CRs
	REPE	CMPSB
	POP	CX
	JZ	EdSaveDone			; the same line again
EdSaveFit:
	MOV	AX,DX
	ADD	AX,CX
	CMP	AX,OFFSET RESGROUP:HistTop	; room for it and its CR?
	JB	EdSaveAdd
	PUSH	CX
	MOV	BX,OFFSET RESGROUP:HistBuf
	CALL	EdNext				; BX = the second oldest line
	CMP	BX,DX
	JBE	EdSaveDrop
	MOV	BX,DX
EdSaveDrop:
	MOV	CX,DX				; move the lines after the
	SUB	CX,BX				;  oldest down over it
	PUSH	DS
	PUSH	ES
	POP	DS
	MOV	SI,BX
	MOV	DI,OFFSET RESGROUP:HistBuf
	REP	MOVSB
	POP	DS
	MOV	DX,DI
	POP	CX
	JMP	EdSaveFit
EdSaveAdd:
	MOV	DI,DX
	MOV	SI,OFFSET TRANGROUP:UCOMBUF+2
	INC	CX				; with its CR
	REP	MOVSB
	MOV	BX,OFFSET RESGROUP:HistEnd
	MOV	ES:[BX],DI
EdSaveDone:
	POP	ES
EdSaveRet:
	RET

;
; EdShow - write the line from index AL to its end, blank what is left of
; a longer line before, and put the cursor back at EdPos.  Writing may
; scroll the screen; EdRow follows it.  Uses AX, BX, CX, DX.
;
EdShow:
	CALL	EdGoto
	MOV	BL,AL
	XOR	BH,BH
EdShowCh:
	CMP	BL,[EdLen]
	JAE	EdShowBlank
	MOV	DL,UCOMBUF[BX+2]
	CMP	DL,09h				; a Tab takes one column here
	JNZ	EdShowPut
	MOV	DL,' '
EdShowPut:
	CALL	EdPut
	INC	BX
	JMP	EdShowCh
EdShowBlank:
	CMP	BL,[EdShown]
	JAE	EdShowEnd
	MOV	DL,' '
	CALL	EdPut
	INC	BX
	JMP	EdShowBlank
EdShowEnd:
	MOV	AL,BL				; the cursor is at index BL
	CALL	EdWhere 			; DH = the row it should be on
	PUSH	DX
	MOV	BH,[EdPage]
	MOV	AH,3
	INT	10h				; DH = the row it is on
	POP	AX
	SUB	AH,DH				; the rows the screen scrolled
	JBE	EdShowAt
	SUB	[EdRow],AH
EdShowAt:
	MOV	AL,[EdLen]
	MOV	[EdShown],AL
	MOV	AL,[EdPos]
;
; EdGoto - put the cursor at index AL of the line
;
EdGoto:
	PUSH	AX
	PUSH	BX
	PUSH	DX
	CALL	EdWhere
	MOV	BH,[EdPage]
	MOV	AH,2
	INT	10h
	POP	DX
	POP	BX
	POP	AX
	RET

;
; EdWhere - DH = the row and DL = the column of index AL of the line.
; Uses AX.
;
EdWhere:
	XOR	AH,AH
	ADD	AL,[EdCol]
	ADC	AH,0
	DIV	[EdCols]
	MOV	DL,AH
	ADD	AL,[EdRow]
	MOV	DH,AL
	RET

;
; EdPut - write the character in DL to the console
;
EdPut:
	MOV	AH,STD_CON_OUTPUT
	INT	int_command
	RET

TRANCODE	ENDS
	END
