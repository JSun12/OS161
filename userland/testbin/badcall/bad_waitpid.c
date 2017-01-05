/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * bad calls to waitpid()
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>

#include "config.h"
#include "test.h"

static
void
wait_badpid(pid_t pid, const char *desc)
{
	pid_t rv;
	int x;

	rv = waitpid(pid, &x, 0);
	/* Allow ENOSYS for 0 or negative values of pid only */
	if (pid <= 0 && rv == -1 && errno == ENOSYS) {
		errno = ESRCH;
	}
	report_test2(rv, errno, ESRCH, ECHILD, desc);
}

static
void
wait_nullstatus(void)
{
	pid_t pid, rv;
	int x;

	pid = fork();
	if (pid<0) {
		warn("UH-OH: fork failed");
		return;
	}
	if (pid==0) {
		exit(0);
	}

	/* POSIX explicitly says passing NULL for status is allowed */
	rv = waitpid(pid, NULL, 0);
	report_test(rv, errno, 0, "wait with NULL status");
	waitpid(pid, &x, 0);
}

static
void
wait_badstatus(void *ptr, const char *desc)
{
	pid_t pid, rv;
	int x;

	pid = fork();
	if (pid<0) {
		warn("UH-OH: fork failed");
		return;
	}
	if (pid==0) {
		exit(0);
	}

	rv = waitpid(pid, ptr, 0);
	report_test(rv, errno, EFAULT, desc);
	waitpid(pid, &x, 0);
}

static
void
wait_unaligned(void)
{
	pid_t pid, rv;
	int x;
	int status[2];	/* will have integer alignment */
	char *ptr;

	pid = fork();
	if (pid<0) {
		warn("UH-OH: fork failed");
		return;
	}
	if (pid==0) {
		exit(0);
	}

	/* start with proper integer alignment */
	ptr = (char *)(&status[0]);

	/* generate improper alignment on platforms with restrictions */
	ptr++;

	rv = waitpid(pid, (int *)ptr, 0);
	report_survival(rv, errno, "wait with unaligned status");
	if (rv<0) {
		waitpid(pid, &x, 0);
	}
}

static
void
wait_badflags(void)
{
	pid_t pid, rv;
	int x;

	pid = fork();
	if (pid<0) {
		warn("UH-OH: fork failed");
		return;
	}
	if (pid==0) {
		exit(0);
	}

	rv = waitpid(pid, &x, 309429);
	report_test(rv, errno, EINVAL, "wait with bad flags");
	waitpid(pid, &x, 0);
}

static
void
wait_self(void)
{
	pid_t rv;
	int x;

	rv = waitpid(getpid(), &x, 0);
	report_survival(rv, errno, "wait for self");
}

static
void
wait_parent(void)
{
	pid_t mypid, childpid, rv;
	int x;

	mypid = getpid();
	childpid = fork();
	if (childpid<0) {
		warn("UH-OH: can't fork");
		return;
	}
	if (childpid==0) {
		/* Child. Wait for parent. */
		rv = waitpid(mypid, &x, 0);
		report_survival(rv, errno, "wait for parent (from child)");
		_exit(0);
	}
	rv = waitpid(childpid, &x, 0);
	report_survival(rv, errno, "wait for parent test (from parent)");
}

////////////////////////////////////////////////////////////

static
void
wait_siblings_child(const char *semname)
{
	pid_t pids[2], mypid, otherpid;
	int rv, fd, semfd, x;

	mypid = getpid();

	/*
	 * Get our own handle for the semaphore, in case naive
	 * file-level synchronization causes concurrent use to
	 * deadlock.
	 */
	semfd = open(semname, O_RDONLY);
	if (semfd < 0) {
		warn("UH-OH: child process (pid %d) can't open %s",
		     mypid, semname);
	}
	else {
		if (read(semfd, NULL, 1) < 0) {
			warn("UH-OH: in pid %d: %s: read", mypid, semname);
		}
		close(semfd);
	}

	fd = open(TESTFILE, O_RDONLY);
	if (fd<0) {
		warn("UH-OH: child process (pid %d) can't open %s",
		     mypid, TESTFILE);
		return;
	}

	/*
	 * In case the semaphore above didn't work, as a backup
	 * busy-wait until the parent writes the pids into the
	 * file. If the semaphore did work, this shouldn't loop.
	 */
	do {
		rv = lseek(fd, 0, SEEK_SET);
		if (rv<0) {
			warn("UH-OH: child process (pid %d) lseek error",
			     mypid);
			return;
		}
		rv = read(fd, pids, sizeof(pids));
		if (rv<0) {
			warn("UH-OH: child process (pid %d) read error",
			     mypid);
			return;
		}
	} while (rv < (int)sizeof(pids));

	if (mypid==pids[0]) {
		otherpid = pids[1];
	}
	else if (mypid==pids[1]) {
		otherpid = pids[0];
	}
	else {
		warn("UH-OH: child process (pid %d) got garbage in comm file",
		     mypid);
		return;
	}
	close(fd);

	rv = waitpid(otherpid, &x, 0);
	report_survival(rv, errno, "sibling wait");
}

static
void
wait_siblings(void)
{
	pid_t pids[2];
	int rv, fd, semfd, x;
	char semname[32];

	/* This test may also blow up if FS synchronization is substandard */

	snprintf(semname, sizeof(semname), "sem:badcall.%d", (int)getpid());
	semfd = open(semname, O_WRONLY|O_CREAT|O_TRUNC, 0664);
	if (semfd < 0) {
		warn("UH-OH: can't make semaphore");
		return;
	}

	fd = open_testfile(NULL);
	if (fd<0) {
		close(semfd);
		remove(semname);
		return;
	}

	pids[0] = fork();
	if (pids[0]<0) {
		warn("UH-OH: can't fork");
		close(fd);
		close(semfd);
		remove(semname);
		return;
	}
	if (pids[0]==0) {
		close(fd);
		close(semfd);
		wait_siblings_child(semname);
		_exit(0);
	}

	pids[1] = fork();
	if (pids[1]<0) {
		warn("UH-OH: can't fork");
		/* abandon the other child process :( */
		close(fd);
		close(semfd);
		remove(semname);
		return;
	}
	if (pids[1]==0) {
		close(fd);
		close(semfd);
		wait_siblings_child(semname);
		_exit(0);
	}

	rv = write(fd, pids, sizeof(pids));
	if (rv < 0) {
		warn("UH-OH: write error on %s", TESTFILE);
		/* abandon child procs :( */
		close(fd);
		close(semfd);
		remove(semname);
		return;
	}
	if (rv != (int)sizeof(pids)) {
		warnx("UH-OH: write error on %s: short count", TESTFILE);
		/* abandon child procs :( */
		close(fd);
		close(semfd);
		remove(semname);
		return;
	}

	/* gate the child procs */
	rv = write(semfd, NULL, 2);
	if (rv < 0) {
		warn("UH-OH: %s: write", semname);
	}

	rv = waitpid(pids[0], &x, 0);
	if (rv<0) {
		warn("UH-OH: error waiting for child 0 (pid %d)", pids[0]);
	}
	rv = waitpid(pids[1], &x, 0);
	if (rv<0) {
		warn("UH-OH: error waiting for child 1 (pid %d)", pids[1]);
	}
	warnx("passed: siblings wait for each other");
	close(fd);
	close(semfd);
	remove(semname);
	remove(TESTFILE);
}

////////////////////////////////////////////////////////////

void
test_waitpid(void)
{
	wait_badpid(-8, "wait for pid -8");
	wait_badpid(-1, "wait for pid -1");
	wait_badpid(0, "pid zero");
	wait_badpid(NONEXIST_PID, "nonexistent pid");

	wait_nullstatus();
	wait_badstatus(INVAL_PTR, "wait with invalid pointer status");
	wait_badstatus(KERN_PTR, "wait with kernel pointer status");

	wait_unaligned();

	wait_badflags();

	wait_self();
	wait_parent();
	wait_siblings();
}
