#include <types.h>
#include <lib.h>
#include <vfs.h>
#include <filetable.h>
#include <kern/fcntl.h>
#include <fsyscall.h>
#include <kern/errno.h>


struct ft *
ft_create()
{
    struct ft *ft;

    ft = kmalloc(sizeof(struct ft));
    if(ft == NULL) {
        return NULL;
    }

    ft->ft_lock = lock_create("fs_lock");
    if (ft->ft_lock == NULL) {
        kfree(ft);
        return NULL;
    }

    for (int i = 0; i < OPEN_MAX; i++) {
        ft->entries[i] = NULL;
    }

    return ft;
}

void
ft_destroy(struct ft *ft)
{
    KASSERT(ft != NULL);

    lock_destroy(ft->ft_lock);
    kfree(ft);
}

/*
Initializes the console of the filetable.
*/
int
ft_init_std(struct ft *ft){

    KASSERT(ft != NULL);

    const char * cons = "con:";  // Filepath of console

    if (ft->entries[0] == NULL){
        struct vnode *stdin_v;
        int ret;

        ret = vfs_open(kstrdup(cons), O_RDONLY, 0, &stdin_v);
        if (ret) {
            return ret;
        }

        ft->entries[0] = entry_create(stdin_v);
        if (ft->entries[0] == NULL) {
            return ENOMEM;
        }

        ft->entries[0]->rwflags = O_RDONLY;
        entry_incref(ft->entries[0]);
        (void) ret;
    }

    if (ft->entries[1] == NULL){
        struct vnode *stdout_v;
        int ret;

        ret = vfs_open(kstrdup(cons), O_WRONLY, 0, &stdout_v);
        if (ret) {
            return ret;
        }

        ft->entries[1] = entry_create(stdout_v);
        if (ft->entries[1] == NULL) {
            return ENOMEM;
        }

        ft->entries[1]->rwflags = O_WRONLY;
        entry_incref(ft->entries[1]);
        (void) ret;
    }

    if (ft->entries[2] == NULL){
        struct vnode *stderr_v;
        int ret;

        ret = vfs_open(kstrdup(cons), O_WRONLY, 0, &stderr_v);
        if (ret) {
            return ret;
        }

        ft->entries[2] = entry_create(stderr_v);
        if (ft->entries[2] == NULL) {
            return ENOMEM;
        }

        ft->entries[2]->rwflags = O_WRONLY;
        entry_incref(ft->entries[2]);
        (void) ret;
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////

/*
Returns a -1 if OPEN_MAX files are already open.
Otherwise, returns new file descriptor.
*/
int
add_entry(struct ft *ft, struct ft_entry *entry)
{
    KASSERT(ft != NULL);
    KASSERT(entry != NULL);

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
    KASSERT(ft != NULL);
    KASSERT(entry != NULL);
    KASSERT(fd_valid(fd));

    ft->entries[fd] = entry;
    entry_incref(entry);
}


void
free_fd(struct ft *ft, int fd)
{
    KASSERT(ft != NULL);
    KASSERT(fd_valid(fd));

    if (ft->entries[fd] == NULL) {
        return;
    }

    entry_decref(ft->entries[fd]);
    ft->entries[fd] = NULL;
}

bool
fd_valid_and_used(struct ft *ft, int fd)
{
    KASSERT(ft != NULL);

    if (!fd_valid(fd)) {
        return false;
    }
    return ft->entries[fd] != NULL;
}

bool
fd_valid(int fd)
{
    if (fd < 0 || fd >= OPEN_MAX) {
        return false;
    }

    return true;
}

void
entry_incref(struct ft_entry *entry)
{
    KASSERT(entry != NULL);

    entry->count += 1;
}

void
entry_decref(struct ft_entry *entry)
{
    KASSERT(entry != NULL);

    entry->count -= 1;
    if (entry->count == 0){
        entry_destroy(entry);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////

struct ft_entry *
entry_create(struct vnode *vnode)
{
    struct ft_entry *entry;

    KASSERT(vnode != NULL);

    entry = kmalloc(sizeof(struct ft_entry));
    if (entry == NULL) {
        return NULL;
    }
    entry->entry_lock = lock_create("entry_lock");
    if (entry->entry_lock == NULL) {
        kfree(entry);
        return NULL;
    }

    entry->file = vnode;
    entry->offset = 0;
    entry->count = 0;

    return entry;
}

void
entry_destroy(struct ft_entry *entry)
{
    KASSERT(entry != NULL);

    vfs_close(entry->file);
    lock_destroy(entry->entry_lock);
    kfree(entry);
}
