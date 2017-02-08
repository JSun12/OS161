#include <types.h>
#include <vfs.h>
#include <current.h>
#include <fsyscall.h>
#include <filetable.h>
#include <proc.h>


int
sys_open(const char *filename, int flags)
{
    struct vnode *new;
    int ret;

	ret = vfs_open((char *)filename, flags, 0, &new);
    if(ret){
        return -1;
    }

    struct ft_entry *entry = entry_create(new);
    ret = add_entry(curproc->proc_ft, entry);
    if (ret == -1){
        return ret;
    }

    return ret;

}

/*
  Atomic removal of an entry from the filetable.
  Vfs file is also properly closed.
*/
int
sys_close(int fd)
{
    return remove_entry(curproc->proc_ft, fd);
}