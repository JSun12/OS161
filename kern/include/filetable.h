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
};

// Entry of a file table
struct ft_entry {
    int count; 
    struct lock *entry_lock;
    struct vnode *file;
    off_t offset;
    const char* path;
    int rwflags; 
};

// Functions
struct ft *ft_create(void);
void ft_destroy(struct ft *);

void ft_init_std(struct ft *);
int add_entry(struct ft*, struct ft_entry *);
void assign_fd(struct ft *, struct ft_entry *, int);
int free_fd(struct ft *, int);
bool fd_valid_and_used(struct ft *, int);

struct ft_entry *entry_create(struct vnode *);
void entry_destroy(struct ft_entry *);

/*
At any state of the file table, the entry->count 
is the number of file descriptors it has. Once the 
count reaches zero, the entry must be destroyed.
*/

void entry_incref(struct ft_entry *);
void entry_decref(struct ft_entry *);


struct addrspace;
struct vnode;


#endif
