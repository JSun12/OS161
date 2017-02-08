// File system calls defined here


#ifndef _FILE_SYSCALLS_H_
#define _FILE_SYSCALLS_H_


#include <current.h>
#include <filetable.h>
#include <types.h>


int open(const char *, int, mode_t);

#endif