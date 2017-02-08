#ifndef _FSYSCALL_H_
#define _FSYSCALL_H_


#include <filetable.h>


int sys_open(const char *, int);
int sys_close(int);

#endif