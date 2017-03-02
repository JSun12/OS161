#include <types.h>
#include <lib.h>
#include <vfs.h>
#include <filetable.h>
#include <kern/fcntl.h>
#include <fsyscall.h>
#include <kern/errno.h>


static int init_std_io(struct ft *, int, int);


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
ft_init_std(struct ft *ft)
{
    KASSERT(ft != NULL);

    int result;

    result = init_std_io(ft, 0, O_RDONLY);
    if (result) {
        return result;
    }
    result = init_std_io(ft, 1, O_WRONLY);
    if (result) {
        return result;
    }
    result = init_std_io(ft, 2, O_WRONLY);
    if (result) {
        return result;
    }

    return 0;
}

static 
int 
init_std_io(struct ft *ft, int fd, int rwflags)
{
    struct vnode *std_io;
    int ret;
    const char *cons = "con:";    

    ret = vfs_open(kstrdup(cons), rwflags, 0, &std_io);
    if (ret) {
        return ret;
    }

    ft->entries[fd] = entry_create(std_io);
    if (ft->entries[fd] == NULL) {
        return ENOMEM;
    }

    ft->entries[fd]->rwflags = rwflags;
    entry_incref(ft->entries[fd]);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////

/*
Returns a EMFILE if OPEN_MAX files are already open.
Otherwise, returns new file descriptor.
*/
int
add_entry(struct ft *ft, struct ft_entry *entry, int32_t *fd)
{
    KASSERT(ft != NULL);
    KASSERT(entry != NULL);

    for(int i = 3; i < OPEN_MAX; i++){
        if(ft->entries[i] == NULL){
            assign_fd(ft, entry, i);
            *fd = i;
            return 0;
        }
    }

    return EMFILE;
}

void
assign_fd(struct ft *ft, struct ft_entry *entry, int fd)
{
    KASSERT(ft != NULL);
    KASSERT(entry != NULL);
    KASSERT(fd_valid(fd));

    ft->entries[fd] = entry;

    lock_acquire(entry->entry_lock);
    entry_incref(entry);
    lock_release(entry->entry_lock);
}

void
free_fd(struct ft *ft, int fd)
{
    KASSERT(ft != NULL);
    KASSERT(fd_valid(fd));

    if (ft->entries[fd] == NULL) {
        return;
    }

    struct ft_entry *entry = ft->entries[fd];    

    lock_acquire(entry->entry_lock);
    entry_decref(entry, true);
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

/*
The new_ft is still being created, so we don't need to acquire it's lock.
*/
void
ft_copy(struct ft *old_ft, struct ft *new_ft)
{
    for (int i = 0; i < OPEN_MAX; i++) {
        struct ft_entry *entry; 
        entry = old_ft->entries[i]; 

        if (entry == NULL) {
            continue;
        }
        
        lock_acquire(entry->entry_lock);
        entry->count += 1;
        lock_release(entry->entry_lock);

        new_ft->entries[i] = entry;
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////

struct ft_entry *
entry_create(struct vnode *vnode)
{
    KASSERT(vnode != NULL);

    struct ft_entry *entry;

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

/*
Must acquire both ft_lock and entry_lock to delete an entry.
*/
void
entry_destroy(struct ft_entry *entry)
{
    KASSERT(entry != NULL);

    vfs_close(entry->file);
    lock_destroy(entry->entry_lock);
    kfree(entry);
}

void
entry_incref(struct ft_entry *entry)
{
    KASSERT(entry != NULL);
    entry->count += 1;
}

void
entry_decref(struct ft_entry *entry, bool lock_held)
{
    KASSERT(entry != NULL);
    KASSERT(lock_held);

    entry->count -= 1;
    if (entry->count == 0){
        entry_destroy(entry);
        return;
    }
    lock_release(entry->entry_lock);
}

