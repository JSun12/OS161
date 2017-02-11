#ifndef _FSYSCALL_H_
#define _FSYSCALL_H_


#include <filetable.h>
#include <kern/seek.h>


int sys_open(const char *, int, int32_t *);
int sys_close(int);
int sys_write(int fd, const void *buf, size_t nbytes, int32_t *);
ssize_t sys_read(int fd, void *buf, size_t buflen);
int sys_lseek(int fd, off_t pos, int whence, int32_t *, int32_t *);
int sys_dup2(int oldfd, int newfd);
int sys_chdir(const char *);
int sys___getcwd(char *, size_t);

#endif
