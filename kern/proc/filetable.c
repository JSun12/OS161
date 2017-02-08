// File table implementation


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
    int i; 
    
    lock_acquire(ft->ft_lock);

    for(i = 3; i < OPEN_MAX; i++){
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





