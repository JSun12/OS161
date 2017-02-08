#include <types.h>
#include <vfs.h>
#include <filetable.h>


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
			kprintf("Open %d\n", i);
            return i;
        }
    }

    lock_release(ft->ft_lock);

    return -1;
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
    kprintf("Close %d\n", pos);

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



void
entry_destroy(struct ft_entry *entry)
{
    lock_destroy(entry->entry_lock);

    kfree(entry);
}