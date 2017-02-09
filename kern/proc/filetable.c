#include <types.h>
#include <lib.h>
#include <vfs.h>
#include <filetable.h>
#include <kern/fcntl.h>
#include <fsyscall.h>


struct ft *
ft_create()
{
    struct ft *ft;

    ft = kmalloc(sizeof(struct ft));
    ft->ft_lock = lock_create("fs_lock");

    return ft;
}

void
ft_destroy(struct ft *ft)
{
    lock_destroy(ft->ft_lock);
    kfree(ft);
}

/*
Initializes the console of the filetable
*/
void
ft_init_std(struct ft *ft){

    const char * cons = "con:";  // Filepath of console

    if (ft->used[0] == 0){
        struct vnode *stdin_v;
        int ret;

        ret = vfs_open(kstrdup(cons), O_RDONLY, 0, &stdin_v);  //XXX: Check the return value
        ft->entries[0] = entry_create(stdin_v);
        ft->used[0] = 1;
        (void) ret;
        kprintf("STDIN opened\n");
    }

    if (ft->used[1] == 0){
        struct vnode *stdout_v;
        int ret;

        ret = vfs_open(kstrdup(cons), O_WRONLY, 0, &stdout_v);  //XXX: Check the return value
        ft->entries[1] = entry_create(stdout_v);
        ft->used[1] = 1;
        (void) ret;
        kprintf("STDOUT opened\n");
    }

    if (ft->used[2] == 0){
        struct vnode *stderr_v;
        int ret;

        ret = vfs_open(kstrdup(cons), O_WRONLY, 0, &stderr_v);  //XXX: Check the return value
        ft->entries[2] = entry_create(stderr_v);
        ft->used[2] = 1;
        (void) ret;
        kprintf("STDERR opened\n");
    }

}

/*
Returns a negative value if OPEN_MAX files are
already open. Otherwise, returns the index which
a file has been saved in the file table, which
is the file descriptor.
*/
int
add_entry(struct ft *ft, struct ft_entry *entry)
{
    lock_acquire(ft->ft_lock);

    for(int i = 3; i < OPEN_MAX; i++){
        if(!ft->used[i]){
            ft->used[i] = 1;
            ft->entries[i] = entry;
            lock_release(ft->ft_lock);
            return i;
        }
    }

    lock_release(ft->ft_lock);

    return -1;
}

bool
fd_used(struct ft *ft, int fd)
{
    return (ft->used[fd] == 1);
}

/*
Removes an entry at the specified location in the filetable.
Will return 0 for success, -1 for failure
*/
int
remove_entry(struct ft *ft, int pos)
{
    /* Out of bounds */
    if (pos >= OPEN_MAX || pos < 0)
        return -1;

    /* Unused position */
    if (ft->used[pos] == 0)
        return -1;

    lock_acquire(ft->ft_lock);

    vfs_close(ft->entries[pos]->file);

    entry_destroy(ft->entries[pos]);
    ft->entries[pos] = NULL;
    ft->used[pos] = 0;

    lock_release(ft->ft_lock);

    return 0;
}


struct ft_entry *
entry_create(struct vnode *vnode)
{
    struct ft_entry *entry;

    entry = kmalloc(sizeof(struct ft_entry));
    entry->entry_lock = lock_create("entry_lock");
    entry->file = vnode;
    entry->offset = 0;

    return entry;
}


// This will also VFS close the file.``
void
entry_destroy(struct ft_entry *entry)
{
    lock_destroy(entry->entry_lock);

    kfree(entry);
}
