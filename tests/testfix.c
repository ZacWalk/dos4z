/* TESTFIX - provokes the MS-DOS 4.00 bugs fixed in 4.0z that a program can
 * see through INT 21h, and says whether each now behaves.
 *
 * test-boot.ps1 compiles it with libdos's C compiler (demu --cc) and runs it in
 * demu --boot, with C: a 2 GB volume and SHARE not loaded. It prints a line
 * per check and ends with "TESTFIX: <n> passed, <m> failed". It is a test, not
 * part of DOS.
 */

int __int21(int ax, int bx, int cx, int dx);
int __int21si(int ax, int bx, int cx, int dx, int si);
unsigned __seg(void);
extern int __cf;

int passed;
int failed;
char buffer[80];
unsigned parameters[11]; /* INT 21h AX=5D00h's register list: AX to ES, then three more words */
char fcbs[6][37];	/* FCBs for F0.TMP to F5.TMP in C:'s current directory */
char record[256];	/* the DTA, and a buffer for reading a file back */
char path[] = "C:\\DEEP\\F0.TMP";

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

/* An unopened FCB for Fn.TMP on C:. */
void make_fcb(char *fcb, int n)
{
	int i;
	for (i = 0; i < 37; ++i) fcb[i] = 0;
	for (i = 1; i < 12; ++i) fcb[i] = ' ';
	fcb[0] = 3;
	fcb[1] = 'F';
	fcb[2] = (char)('0' + n);
	fcb[9] = 'T';
	fcb[10] = 'M';
	fcb[11] = 'P';
}

/* Whether count bytes of record from offset at all hold value. */
int holds(int at, int count, int value)
{
	int i;
	for (i = at; i < at + count; ++i)
		if (record[i] != (char)value) return 0;
	return 1;
}

void fill(int value)
{
	int i;
	for (i = 0; i < 128; ++i) record[i] = (char)value;
}

/* FCB I/O on a 2 GB volume without SHARE, which MS-DOS 4.00 needed on volumes
 * over 32 MB. A 34 MB file puts C:\DEEP past sector 65535, and six files there
 * are open at once by FCB, more than FCBS=4 keeps SFTs for, so DOS rebuilds a
 * file's SFT from its FCB at nearly every call. Record n of file i holds
 * 'A' + 6 * n + i. */
void fcb_checks(void)
{
	int handle;
	int count;
	int i;
	int n;
	int created = 1;
	int written = 1;
	int read = 1;
	int on_disk = 1;

	handle = __int21(0x3c00, 0, 0, (int)"C:\\FILL.TMP");
	if (__cf)
	{
		result(0, "C:\\FILL.TMP can be created");
		return;
	}
	__int21(0x4200, handle, 0x0206, (int)0xcc80); /* 34,000,000 bytes in */
	__int21(0x4000, handle, 1, (int)record);
	__int21(0x3e00, handle, 0, 0);
	__int21(0x3900, 0, 0, (int)"C:\\DEEP");
	__int21(0x3b00, 0, 0, (int)"C:\\DEEP"); /* FCBs use C:'s current directory */
	__int21(0x1a00, 0, 0, (int)record);

	for (i = 0; i < 6; ++i)
	{
		make_fcb(fcbs[i], i);
		if ((__int21(0x1600, 0, 0, (int)fcbs[i]) & 0xff) != 0) created = 0;
	}
	result(created, "FCB create in a directory past sector 65535 without SHARE");
	for (n = 0; n < 2; ++n)
		for (i = 0; i < 6; ++i)
		{
			fill('A' + 6 * n + i);
			if ((__int21(0x1500, 0, 0, (int)fcbs[i]) & 0xff) != 0) written = 0;
		}
	for (i = 0; i < 6; ++i)
		if ((__int21(0x1000, 0, 0, (int)fcbs[i]) & 0xff) != 0) written = 0;
	result(written, "FCB writes to six files at once, and closing them");

	for (i = 0; i < 6; ++i)
	{
		make_fcb(fcbs[i], i);
		if ((__int21(0x0f00, 0, 0, (int)fcbs[i]) & 0xff) != 0) read = 0;
	}
	for (n = 0; n < 2; ++n)
		for (i = 0; i < 6; ++i)
		{
			fill(0);
			if ((__int21(0x1400, 0, 0, (int)fcbs[i]) & 0xff) != 0 || !holds(0, 128, 'A' + 6 * n + i)) read = 0;
		}
	for (i = 0; i < 6; ++i)
		if ((__int21(0x1000, 0, 0, (int)fcbs[i]) & 0xff) != 0) read = 0;
	result(read, "FCB reads from six files at once get the records back");

	for (i = 0; i < 6; ++i)
	{
		path[9] = (char)('0' + i);
		handle = __int21(0x3d00, 0, 0, (int)path);
		if (__cf)
		{
			on_disk = 0;
			continue;
		}
		count = __int21(0x3f00, handle, 256, (int)record);
		if (__cf || count != 256 || !holds(0, 128, 'A' + i) || !holds(128, 128, 'A' + 6 + i)) on_disk = 0;
		__int21(0x3e00, handle, 0, 0);
		__int21(0x4100, 0, 0, (int)path);
	}
	result(on_disk, "the files written by FCB have their size and data on the disk");

	__int21(0x3b00, 0, 0, (int)"C:\\");
	__int21(0x3a00, 0, 0, (int)"C:\\DEEP");
	__int21(0x4100, 0, 0, (int)"C:\\FILL.TMP");
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

	fcb_checks();

	put("\nTESTFIX: ");
	put_number(passed);
	put(" passed, ");
	put_number(failed);
	put(" failed\n");
	return failed;
}
