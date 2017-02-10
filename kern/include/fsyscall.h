#ifndef _FSYSCALL_H_
#define _FSYSCALL_H_


#include <filetable.h>
#include <kern/seek.h>


int sys_open(const char *, int);
int sys_close(int);
ssize_t sys_write(int fd, const void *buf, size_t nbytes);
ssize_t sys_read(int fd, void *buf, size_t buflen);
off_t sys_lseek(int fd, off_t pos, int whence);
int sys_dup2(int oldfd, int newfd);

#endif
