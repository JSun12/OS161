#include <types.h>
#include <vfs.h>
#include <current.h>
#include <fsyscall.h>
#include <filetable.h>
#include <proc.h>
#include <uio.h>
#include <vnode.h>


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

/*
write writes up to buflen bytes to the file specified by fd, at the location in the file specified by the current seek position of the file, taking the data from the space pointed to by buf. The file must be open for writing.
*/
ssize_t
sys_write(int fd, const void *buf, size_t nbytes)
{
    //XXX: Lock this

    struct iovec iov;
	struct uio u;
	int result;
    struct ft_entry *entry = curproc->proc_ft->entries[fd];

	iov.iov_ubase = (userptr_t)buf;
	iov.iov_len = nbytes;		 // length of the memory space
	u.uio_iov = &iov;
	u.uio_iovcnt = 1;
	u.uio_resid = nbytes;          // amount to write from the file -> Amount left to transfer
	u.uio_offset = 0;//entry->offset;
	u.uio_segflg = UIO_USERSPACE;
	u.uio_rw = UIO_WRITE;
	u.uio_space = curproc->p_addrspace;

	result = VOP_WRITE(entry->file, &u);
	if (result) {
		return result;
	}
    return 0;
}
/*
read reads up to buflen bytes from the file specified by fd, at the location in the file specified by the current seek position of the file, and stores them in the space pointed to by buf. The file must be open for reading.

The current seek position of the file is advanced by the number of bytes read.
*/
ssize_t
sys_read(int fd, void *buf, size_t buflen)
{
    //XXX: Lock this

    struct iovec iov;
	struct uio u;
	int result;
    struct ft_entry *entry = curproc->proc_ft->entries[fd];

	iov.iov_ubase = (userptr_t)buf;
	iov.iov_len = buflen;		 // length of the memory space
	u.uio_iov = &iov;
	u.uio_iovcnt = 1;
	u.uio_resid = buflen;          // amount to read from the file -> Amount left to transfer
	u.uio_offset = 0;//entry->offset;
	u.uio_segflg = UIO_USERSPACE;
	u.uio_rw = UIO_READ;
	u.uio_space = curproc->p_addrspace;

	result = VOP_READ(entry->file, &u);
	if (result) {
		return result;
	}
    return 0;
}
