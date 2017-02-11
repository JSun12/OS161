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

    for (int i = 0; i < OPEN_MAX; i++) {
        ft->entries[i] = NULL;
    }

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

    if (ft->entries[0] == NULL){
        struct vnode *stdin_v;
        int ret;

        ret = vfs_open(kstrdup(cons), O_RDONLY, 0, &stdin_v);  //XXX: Check the return value
        ft->entries[0] = entry_create(stdin_v);
        ft->entries[0]->rwflags = O_RDONLY;
        entry_incref(ft->entries[0]);
        (void) ret;
        // kprintf("STDIN opened\n");
    }

    if (ft->entries[1] == NULL){
        struct vnode *stdout_v;
        int ret;

        ret = vfs_open(kstrdup(cons), O_WRONLY, 0, &stdout_v);  //XXX: Check the return value
        ft->entries[1] = entry_create(stdout_v);
        ft->entries[1]->rwflags = O_WRONLY;
        entry_incref(ft->entries[1]);
        (void) ret;
        // kprintf("STDOUT opened\n");
    }

    if (ft->entries[2] == NULL){
        struct vnode *stderr_v;
        int ret;

        ret = vfs_open(kstrdup(cons), O_WRONLY, 0, &stderr_v);  //XXX: Check the return value
        ft->entries[2] = entry_create(stderr_v);
        ft->entries[2]->rwflags = O_WRONLY;
        entry_incref(ft->entries[2]);
        (void) ret;
        // kprintf("STDERR opened\n");
    }

}

/*
Returns a -1 if OPEN_MAX files are
already open. Otherwise, returns new
file descriptor.
*/
int
add_entry(struct ft *ft, struct ft_entry *entry)
{
    lock_acquire(ft->ft_lock);

    for(int i = 3; i < OPEN_MAX; i++){
        if(ft->entries[i] == NULL){
            assign_fd(ft, entry, i);
            lock_release(ft->ft_lock);
            return i;
        }
    }

    lock_release(ft->ft_lock);

    return -1;
}

void
assign_fd(struct ft *ft, struct ft_entry *entry, int fd)
{   
    ft->entries[fd] = entry;
    entry_incref(entry);
}


/*
Removes an entry at the specified location in the filetable.
Will return 0 for success, -1 for failure
*/
int
free_fd(struct ft *ft, int fd)
{
    if(!fd_valid_and_used(ft, fd)){
        return -1;
    }

    lock_acquire(ft->ft_lock);    

    entry_decref(ft->entries[fd]);
    ft->entries[fd] = NULL;

    lock_release(ft->ft_lock);

    return 0;
}

bool
fd_valid_and_used(struct ft *ft, int fd)
{
    if (fd < 0 || fd >= OPEN_MAX) {
        return false;
    }
    return ft->entries[fd] != NULL;
}


struct ft_entry *
entry_create(struct vnode *vnode)
{
    struct ft_entry *entry;

    entry = kmalloc(sizeof(struct ft_entry));
    entry->entry_lock = lock_create("entry_lock");
    entry->file = vnode;
    entry->offset = 0;
    entry->count = 0; 

    return entry;
}


// This will also VFS close the file.
void
entry_destroy(struct ft_entry *entry)
{
    vfs_close(entry->file);    
    lock_destroy(entry->entry_lock);
    kfree(entry);
}

void
entry_incref(struct ft_entry *entry){
    entry->count += 1; 
}

void
entry_decref(struct ft_entry *entry){    
    entry->count -= 1; 
    if (entry->count == 0){
        entry_destroy(entry);
    }
}