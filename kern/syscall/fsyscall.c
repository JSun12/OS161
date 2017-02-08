// File system calls implemented here


#include <fsyscall.h>
#include <vfs.h>


/*
Returns a nonnegative file descriptor in success,
and -1 on a failure with the correct error number
set.
*/
int
open(const char *filename, int flags, mode_t mode)
{
    struct vnode **new; 
    int ret; 

    ret = vsf_open(filename, flags, mode, new);
    if(ret){
        return -1;
    }

    struct ft_entry *entry = entry_create(*new);
    ret = add_entry(curproc->proc_ft, entry);
    if(ret == -1){
        return ret;
    }

    return ret;
    
}



