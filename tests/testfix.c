/* TESTFIX - provokes the MS-DOS 4.00 bugs fixed in 4.0z that a program can
 * see through INT 21h, and says whether each now behaves.
 *
 * test-boot.ps1 compiles it with libdos's C compiler (demu --cc) and runs it in
 * demu --boot, with C: a 2 GB volume. It prints a line per check and ends
 * with "TESTFIX: <n> passed, <m> failed". It is a test, not part of DOS.
 */

int __int21(int ax, int bx, int cx, int dx);
int __int21si(int ax, int bx, int cx, int dx, int si);
unsigned __seg(void);
extern int __cf;

int passed;
int failed;
char buffer[80];
unsigned parameters[11]; /* INT 21h AX=5D00h's register list: AX to ES, then three more words */

void put(const char *text)
{
	while (*text)
	{
		if (*text == '\n') __int21(0x0200, 0, 0, '\r');
		__int21(0x0200, 0, 0, *text);
		++text;
	}
}

void put_number(int value)
{
	char digits[6];
	int count = 0;
	do
	{
		digits[count] = (char)('0' + value % 10);
		++count;
		value = value / 10;
	} while (value != 0);
	while (count > 0)
	{
		--count;
		__int21(0x0200, 0, 0, digits[count]);
	}
}

void result(int ok, const char *what)
{
	put(ok ? "PASS  " : "FAIL  ");
	put(what);
	put("\n");
	if (ok)
		++passed;
	else
		++failed;
}

int main(void)
{
	int ax;
	int cf;
	int handle;

	put("TESTFIX - the 4.0z fixes a program can see\n\n");

	ax = __int21si(0x4700, 0, 0, 0, (int)buffer);
	cf = __cf;
	result(cf == 0 && ax == 0x0100, "INT 21h AH=47h returns AX=0100h");

	ax = __int21(0x4e00, 0, 0x0008, (int)"CON");
	cf = __cf;
	result(cf != 0 && ax == 0x0012, "a search for a volume label named CON finds no device");

	/* Function 7Fh is beyond the table; it used to be jumped through anyway. */
	parameters[0] = 0x7f00;
	parameters[6] = __seg();
	parameters[7] = __seg();
	ax = __int21(0x5d00, 0, 0, (int)parameters);
	result(ax == 0x7f00, "INT 21h AX=5D00h refuses function 7Fh");

	/* Minor code 63h is not one the disk driver knows; it used to crash. */
	__int21(0x440d, 0, 0x0863, (int)buffer);
	cf = __cf;
	result(cf != 0, "generic IOCTL 0863h on a disk fails with the carry set");

	/* 2 GB and 4 KB into a file on a volume of 32 KB clusters is past cluster
	 * 65535; the write used to land on cluster 0 instead. */
	handle = __int21(0x3c00, 0, 0, (int)"C:\\BIG.TMP");
	if (__cf)
	{
		result(0, "C:\\BIG.TMP can be created");
	}
	else
	{
		__int21(0x4200, handle, 0x8000, 0x1000);
		ax = __int21(0x4000, handle, 1, (int)buffer);
		cf = __cf;
		result(cf == 0 && ax == 0, "a write 2 GB into a file reports the disk full");
		__int21(0x3e00, handle, 0, 0);
		__int21(0x4100, 0, 0, (int)"C:\\BIG.TMP");
	}

	put("\nTESTFIX: ");
	put_number(passed);
	put(" passed, ");
	put_number(failed);
	put(" failed\n");
	return failed;
}
