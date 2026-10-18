/* DISKINFO - what the BIOS tells DOS about the hard disks.
 *
 * For each hard disk: the geometry INT 13h AH=08h reports, whether the INT 13h
 * extensions are there (AH=41h), and the size AH=48h gives. That is what
 * decides how much of a disk - a CompactFlash card on an XT-IDE adapter, say -
 * DOS 4.0z can use; see docs/LARGE-DISKS.md, "Checking the card's BIOS".
 *
 * Unlike the rest of DOS this is not built by the 1988 toolchain: make-dos.ps1
 * compiles it with libdos's C compiler (demu --cc) into a .COM. It talks to
 * nothing but INT 21h, for its output, and INT 13h, and runs on an 8088.
 */

int __int21(int ax, int bx, int cx, int dx);
int __int13(int ax, int bx, int cx, int dx);
int __int13si(int ax, int bx, int cx, int dx, int si);
extern int __cf;
extern int __bx;
extern int __cx;
extern int __dx;

/* What AH=48h fills in, as words: the size, flags, cylinders, heads and
 * sectors per track, then the total number of sectors and the sector size. */
unsigned parameters[13];

void put(const char *text)
{
	while (*text)
	{
		if (*text == '\n') __int21(0x0200, 0, 0, '\r');
		__int21(0x0200, 0, 0, *text);
		++text;
	}
}

void put_number(unsigned long value)
{
	char digits[11];
	int count = 0;
	do
	{
		digits[count] = (char)('0' + (int)(value % 10));
		++count;
		value = value / 10;
	} while (value != 0);
	while (count > 0)
	{
		--count;
		__int21(0x0200, 0, 0, digits[count]);
	}
}

void put_hex(unsigned value)
{
	const char *hex = "0123456789ABCDEF";
	__int21(0x0200, 0, 0, hex[(value >> 4) & 15]);
	__int21(0x0200, 0, 0, hex[value & 15]);
	put("h");
}

void show(int drive)
{
	unsigned cx;
	unsigned dx;
	unsigned cylinders;
	unsigned heads;
	unsigned sectors;
	unsigned long reachable;
	unsigned long total;
	int ax;

	put("Drive ");
	put_hex(drive);
	put("\n");

	__int13(0x0800, 0, 0, drive);
	cx = __cx;
	dx = __dx;
	if (__cf)
	{
		put("  CHS:  the BIOS does not say (INT 13h AH=08h failed)\n");
		return;
	}
	cylinders = ((cx >> 8) | ((cx & 0xc0) << 2)) + 1;
	sectors = cx & 0x3f;
	heads = (dx >> 8) + 1;
	reachable = (unsigned long)cylinders * heads * sectors;
	put("  CHS:  ");
	put_number(cylinders);
	put(" cylinders, ");
	put_number(heads);
	put(" heads, ");
	put_number(sectors);
	put(" sectors per track: ");
	put_number(reachable / 2048);
	put(" MB\n");
	if (sectors != 63)
		put("        not the 63 sectors per track dimg uses, so prepare the disk here, with FDISK\n");

	ax = __int13(0x4100, 0x55aa, 0, drive);
	if (__cf || __bx != (int)0xaa55 || (__cx & 1) == 0)
	{
		put("  LBA:  no - DOS 4.0z reaches the first ");
		put_number(reachable / 2048);
		put(" MB, by CHS\n");
		return;
	}
	put("  LBA:  yes, extensions version ");
	put_hex((ax >> 8) & 0xff);
	put(" - DOS 4.0z uses them for the whole disk\n");

	parameters[0] = 0x1a;
	__int13si(0x4800, 0, 0, drive, (int)parameters);
	if (__cf)
	{
		put("  Size: the BIOS does not say (INT 13h AH=48h failed)\n");
		return;
	}
	total = parameters[8] | ((unsigned long)parameters[9] << 16);
	put("  Size: ");
	put_number(total / 2048);
	put(" MB (");
	put_number(total);
	put(" sectors)\n");
}

int main(void)
{
	int drives;
	int drive;

	put("DISKINFO - what the BIOS says about the hard disks\n\n");
	__int13(0x0800, 0, 0, 0x80);
	drives = __dx & 0xff;
	if (__cf || drives == 0)
	{
		put("There are no hard disks.\n");
		return 1;
	}
	for (drive = 0x80; drive < 0x80 + drives && drive < 0x88; ++drive) show(drive);
	return 0;
}
