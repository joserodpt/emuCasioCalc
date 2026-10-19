/* gint file system test: Bfile_* syscalls, called from the OS world like gint
   add-ins do. Each step is checked; if all pass, \\fls0\EMUFILES.txt is left
   with "ok" in it (the CTest test looks for it) and the screen says so. */
#include <gint/display.h>
#include <gint/keyboard.h>
#include <gint/gint.h>
#include <gint/bfile.h>
#include <string.h>

static const uint16_t TEST[] = u"\\\\fls0\\EMUTEST.dat";
static const uint16_t OK[] = u"\\\\fls0\\EMUFILES.txt";

static int step, failed_step;
static int got; /* value seen at the failing step */

/* Monochrome builds (fx-9860G) have no colours */
#if GINT_RENDER_RGB
#define OK_COLOR C_GREEN
#define FAIL_COLOR C_RED
#else
#define OK_COLOR C_BLACK
#define FAIL_COLOR C_BLACK
#endif

#define CHECK(cond, value) do { step++; if(!(cond)) { failed_step = step; got = (value); return 1; } } while(0)

static int run(void)
{
	char data[16] = "emuCasioFxCG ok", back[20];
	int size = 16, fd, rc;

	BFile_Remove(TEST);
	rc = BFile_Create(TEST, BFile_File, &size);
	CHECK(rc == 0, rc);                                     /* 1 create */
	rc = BFile_Create(TEST, BFile_File, &size);
	CHECK(rc == BFile_AlreadyExists, rc);                   /* 2 exists */

	fd = BFile_Open(TEST, BFile_WriteOnly);
	CHECK(fd >= 0, fd);                                     /* 3 open w */
	rc = BFile_Write(fd, data, 16);
	CHECK(rc >= 0, rc);                                     /* 4 write */
	BFile_Close(fd);

	fd = BFile_Open(TEST, BFile_ReadOnly);
	CHECK(fd >= 0, fd);                                     /* 5 open r */
	memset(back, 0x55, sizeof back);
	rc = BFile_Read(fd, back, 16, 0);
	CHECK(rc == 16 && !memcmp(back, data, 16), rc);         /* 6 read */
	rc = BFile_Read(fd, back, 4, 14);                       /* 2 bytes past the end */
	CHECK(rc == 4 && back[1] == 0 && back[2] == 0, rc);     /* 7 read past end */
	BFile_Close(fd);

	rc = BFile_Remove(TEST);
	CHECK(rc == 0, rc);                                     /* 8 remove */
	fd = BFile_Open(TEST, BFile_ReadOnly);
	CHECK(fd == BFile_EntryNotFound, fd);                   /* 9 gone */

	/* Leave the marker */
	int two = 2;
	BFile_Remove(OK);
	BFile_Create(OK, BFile_File, &two);
	fd = BFile_Open(OK, BFile_WriteOnly);
	CHECK(fd >= 0, fd);                                     /* 10 marker */
	BFile_Write(fd, "ok", 2);
	BFile_Close(fd);
	return 0;
}

int main(void)
{
	int rc = gint_world_switch(GINT_CALL(run));

	dclear(C_WHITE);
	if(rc == 0)
		dtext(10, 10, OK_COLOR, "files: ok (10 steps)");
	else
		dprint(10, 10, FAIL_COLOR, "files: step %d failed (got %d)", failed_step, got);
	dupdate();
	getkey();
	return 0;
}
