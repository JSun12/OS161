// Contains the structures for the file descriptor table.
// TODO: make sure to keep vnode reference count.


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
struct ft *ft_create();
void ft_destroy(struct ft *);
int add_entry(struct ft*, struct ft_entry *);

struct ft_entry *entry_create(struct vnode *);
void entry_destroy(struct ft_entry *);

#endif

