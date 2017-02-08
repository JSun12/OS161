#ifndef _FILETABLE_H_
#define _FILETABLE_H_


#include <synch.h>
#include <limits.h>
#include <vnode.h>
#include <lib.h>



// File descriptor table
struct ft {
    struct lock *ft_lock;
    struct ft_entry *entries[OPEN_MAX];
    int used[OPEN_MAX];
};

// Entry of a file table
struct ft_entry {
    struct lock *entry_lock;
    struct vnode *file;
    off_t offset;
};

// Functions
struct ft *ft_create(void);
void ft_destroy(struct ft *);
int add_entry(struct ft*, struct ft_entry *);
int remove_entry(struct ft *ft, int position);

struct ft_entry *entry_create(struct vnode *);
void entry_destroy(struct ft_entry *);

// These syscalls belong in syscall.h
int sys_open(const char *filename, int flags);
int sys_close(int fd);


struct addrspace;
struct vnode;


#endif