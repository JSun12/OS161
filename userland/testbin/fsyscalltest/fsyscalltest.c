/*
 * fsyscalltest.c
 *
 * Tests file-related system calls open, close, read and write.
 *
 * Should run on emufs. This test allows testing the file-related system calls
 * early on, before much of the functionality implemented. This test does not
 * rely on full process functionality (e.g., fork/exec).
 *
 * Much of the code is borrowed from filetest.c
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <err.h>
#include <limits.h>

/* 
 * This is essentially the same code as in filetest.c, except we don't
 * expect any arguments, so the test can be executed before processes are
 * fully implemented. Furthermore, we do not call remove, because emufs does not
 * support it, and we would like to be able to run on emufs.
 */
static void
simple_test()
{
  	static char writebuf[40] = "Twiddle dee dee, Twiddle dum dum.......\n";
	static char readbuf[41];

	const char *file;
	int fd, rv;

	file = "testfile";

	fd = open(file, O_WRONLY|O_CREAT|O_TRUNC, 0664);
	if (fd<0) {
		err(1, "%s: open for write", file);
	}

	rv = write(fd, writebuf, 40);
	if (rv<0) {
		err(1, "%s: write", file);
	}

	rv = close(fd);
	if (rv<0) {
		err(1, "%s: close (1st time)", file);
	}

	fd = open(file, O_RDONLY);
	if (fd<0) {
		err(1, "%s: open for read", file);
	}

	rv = read(fd, readbuf, 40);
	if (rv<0) {
		err(1, "%s: read", file);
	}
	rv = close(fd);
	if (rv<0) {
		err(1, "%s: close (2nd time)", file);
	}
	/* ensure null termination */
	readbuf[40] = 0;

	if (strcmp(readbuf, writebuf)) {
		errx(1, "Buffer data mismatch!");
	}
}

static int openFDs[OPEN_MAX-3 + 1];

/*
 * This test makes sure that the underlying filetable implementation
 * allows us to open as many files as is allowed by the limit on the system.
 */
static void
test_openfile_limits()
{
	const char *file;
	int fd, rv, i;

	file = "testfile1";

	/* We should be allowed to open this file OPEN_MAX - 3 times, 
	 * because the first 3 file descriptors are occupied by stdin, 
	 * stdout and stderr. 
	 */
	for(i = 0; i < (OPEN_MAX-3); i++)
	{
		fd = open(file, O_RDWR|O_CREAT|O_TRUNC, 0664);
		if (fd<0)
			err(1, "%s: open for %dth time", file, (i+1));

		if( (fd == 0) || (fd == 1) || (fd == 2))
			err(1, "open for %s returned a reserved file descriptor",
			    file);

		/* We do not assume that the underlying system will return
		 * file descriptors as consecutive numbers, so we just remember
		 * all that were returned, so we can close them. 
		 */
		openFDs[i] = fd;
	}

	/* This one should fail. */
	fd = open(file, O_RDWR|O_CREAT|O_TRUNC, 0664);
	if(fd > 0)
		err(1, "Opening file for %dth time should fail, as %d "
		    "is the maximum allowed number of open files and the "
		    "first three are reserved. \n",
		    (i+1), OPEN_MAX);

	/* Let's close one file and open another one, which should succeed. */
	rv = close(openFDs[0]);
	if (rv<0)
		err(1, "%s: close for the 1st time", file);
	
	fd = open(file, O_RDWR|O_CREAT|O_TRUNC, 0664);
	if (fd<0)
		err(1, "%s: re-open after closing", file);

	rv = close(fd);
	if (rv<0)
		err(1, "%s: close for the 2nd time", file);

	/* Begin closing with index "1", because we already closed the one
	 * at slot "0".
	 */
	for(i = 1; i < OPEN_MAX - 3; i++)
	{
		rv = close(openFDs[i]);
		if (rv<0)
			err(1, "%s: close file descriptor %d", file, i);
	}
}

/* Open two files, write to them, read from them, make sure the
 * content checks, then close them. 
 */
static void
simultaneous_write_test()
{
  	static char writebuf1[41] = "Cabooble-madooddle, bora-bora-bora.....\n\0";
	static char writebuf2[41] = "Yada, yada, yada, yada, yada, yada.....\n\0";
	static char readbuf[41];
	static int seekpos = 20; // must be less than the writebuf length

	const char *file1, *file2;
	int fd1, fd2, rv;
	off_t lseek_ret;

	file1 = "testfile1";
	file2 = "testfile2";

	fd1 = open(file1, O_RDWR|O_CREAT|O_TRUNC, 0664);
	if (fd1<0) {
		err(1, "%s: open for write", file1);
	}
	fd2 = open(file2, O_RDWR|O_CREAT|O_TRUNC, 0664);
	if (fd2<0) {
		err(1, "%s: open for write", file2);
	}

	rv = write(fd1, writebuf1, 40);
	if (rv<0) {
		err(1, "%s: write", file1);
	}

	rv = write(fd2, writebuf2, 40);
	if (rv<0) {
		err(1, "%s: write", file2);
	}

	/* Rewind both files */
	lseek_ret = lseek(fd1, -(40-seekpos), SEEK_CUR);
	if (lseek_ret != seekpos) {
		err(1, "%s: lseek", file1);
	}

	lseek_ret = lseek(fd2, seekpos, SEEK_SET);
	if (lseek_ret != seekpos) {
		err(1, "%s: lseek", file2);
	}

	/* Read and test the data from the first file */
	rv = read(fd1, readbuf, 40-seekpos);
	if (rv<0) {
		err(1, "%s: read", file1);
	}	
	readbuf[40] = 0;
	
	if (strcmp(readbuf, &writebuf1[seekpos]))
		errx(1, "Buffer data mismatch for %s!", file1);
	
	/* Read and test the data from the second file */
	rv = read(fd2, readbuf, 40-seekpos);
	if (rv<0) {
		err(1, "%s: read", file2);
	}
	readbuf[40] = 0;

	if (strcmp(readbuf, &writebuf2[seekpos])) {
		printf("Expected: \"%s\", actual: \"%s\"\n", writebuf2,
		       readbuf);
		errx(1, "Buffer data mismatch for %s!", file2);
	}

	rv = close(fd1);
	if (rv<0) {
		err(1, "%s: close", file1);
	}

	rv = close(fd2);
	if (rv<0) {
		err(1, "%s: close", file2);
	}

}


/* This test takes no arguments, so we can run it before argument passing
 * is fully implemented. 
 */
int
main()
{
	test_openfile_limits();
	printf("Passed Part 1 of filesyscalls test\n");

	simple_test();
	printf("Passed Part 2 of filesyscalls test\n");
	
	simultaneous_write_test();
	printf("Passed Part 3 of filesyscalls test\n");
	
	return 0;
}
