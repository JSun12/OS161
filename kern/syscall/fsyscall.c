#include <types.h>
#include <vfs.h>
#include <current.h>
#include <fsyscall.h>
#include <filetable.h>
#include <proc.h>
#include <uio.h>
#include <vnode.h>
#include <kern/stat.h>
#include <kern/fcntl.h>
#include <kern/errno.h>
#include <vm.h>
#include <copyinout.h>
#include <limits.h>


// must make extern (use as errno for now)
//int errno;

int
sys_open(const char *filename, int flags, int32_t *output)
{

    struct vnode *new;
    int result;
	char *path;
    size_t *path_len;
    int err;

    path = kmalloc(PATH_MAX);
    path_len = kmalloc(sizeof(int));

    /* Copy the string from userspace to kernel space and check for valid address */
    err = copyinstr((const_userptr_t) filename, path, PATH_MAX, path_len);

    if (err){
        kfree(path);
        kfree(path_len);
        return err;
    }

    /* Open the address and discard the kernel space address */
	result = vfs_open(path, flags, 0, &new);

    kfree(path);
    kfree(path_len);

	if (result) {
		return result;
    }

    struct ft_entry *entry = entry_create(new);
    result = add_entry(curproc->proc_ft, entry);

    if (result == -1){
        return EMFILE;
    }

	if (flags & O_APPEND) {

        struct stat *stat;
		off_t eof;
		stat = kmalloc(sizeof(struct stat));

        if(stat == NULL){
            return ENOMEM;
        }

		VOP_STAT(entry->file, stat);
		eof = stat->st_size;
		entry->offset = eof;
        kfree(stat);
	}

    *output = result;

    return 0;
}

/*
  Atomic removal of an entry from the filetable.
  Vfs file is also properly closed.
*/
int
sys_close(int fd)
{
	int result = free_fd(curproc->proc_ft, fd);
	if (result == -1) {
		return EBADF;
	}
	return 0;
}

/*
write writes up to buflen bytes to the file specified by fd, at the
location in the file specified by the current seek position of the file,
taking the data from the space pointed to by buf. The file must be open for writing.
*/
int
sys_write(int fd, const void *buf, size_t nbytes, int32_t *retval0)
{
    //XXX: Lock this

	if (!fd_valid_and_used(curproc->proc_ft, fd)) {
		return EBADF;
	}

    struct iovec iov;
	struct uio u;
	int result;
    struct ft_entry *entry = curproc->proc_ft->entries[fd];

	iov.iov_ubase = (userptr_t)buf;
	iov.iov_len = nbytes;		 // length of the memory space
	u.uio_iov = &iov;
	u.uio_iovcnt = 1;
	u.uio_resid = nbytes;          // amount to write from the file -> Amount left to transfer
	u.uio_offset = entry->offset;
	u.uio_segflg = UIO_USERSPACE;
	u.uio_rw = UIO_WRITE;
	u.uio_space = curproc->p_addrspace;

	result = VOP_WRITE(entry->file, &u);
	if (result) {
		return result;
	}

	ssize_t len = nbytes - u.uio_resid;
	entry->offset += (off_t) len; //hopefully correct implementation
	// kprintf("Current offset: %d\n", (int) entry->offset);

	// struct stat *stat;
	// off_t eof;
	// stat = kmalloc(sizeof(struct stat));
	// VOP_STAT(entry->file, stat);
	// eof = stat->st_size;
	// kprintf("eof: %d\n", (int) eof);

    *retval0 = len;
	return 0;
}
/*
read reads up to buflen bytes from the file specified by fd,
at the location in the file specified by the current seek position
of the file, and stores them in the space pointed to by buf.
The file must be open for reading.

The current seek position of the file is advanced by the number of bytes read.
*/
int
sys_read(int fd, void *buf, size_t buflen, ssize_t *output)
{
    //XXX: Lock this

	if (!fd_valid_and_used(curproc->proc_ft, fd)) {
		return EBADF;
	}


    struct iovec iov;
	struct uio u;
	int result;
    struct ft_entry *entry = curproc->proc_ft->entries[fd];

	iov.iov_ubase = (userptr_t)buf;
	iov.iov_len = buflen;		 // length of the memory space
	u.uio_iov = &iov;
	u.uio_iovcnt = 1;
	u.uio_resid = buflen;          // amount to read from the file -> Amount left to transfer
	u.uio_offset = entry->offset;
	u.uio_segflg = UIO_USERSPACE;
	u.uio_rw = UIO_READ;
	u.uio_space = curproc->p_addrspace;

	result = VOP_READ(entry->file, &u);
	if (result) {
		return result;
	}

	ssize_t len = buflen - u.uio_resid;
	entry->offset += (off_t) len; //hopefully correct implementation
	// kprintf("Current offset: %d\n", (int) entry->offset);

	// struct stat *stat;
	// off_t eof;
	// stat = kmalloc(sizeof(struct stat));
	// VOP_STAT(entry->file, stat);
	// eof = stat->st_size;
	// kprintf("eof: %d\n", (int) eof);

    *output = len;
    return 0;
}

int
sys_lseek(int fd, off_t pos, int whence, int32_t *retval0, int32_t *retval1)
{

	// kprintf("%d\n", fd);
	// kprintf("%ld\n", (long) pos);
	// kprintf("%d\n", whence);
	//XXX: Lock this

	if (whence < 0 || whence > 2) {
		return EINVAL;
	}

	struct ft *ft = curproc->proc_ft;
	struct ft_entry *entry;
	struct stat *stat;
	off_t eof;
	off_t seek;

	if (!fd_valid_and_used(ft, fd)) {
		return EBADF;
	}

	entry = ft->entries[fd];

	if (!VOP_ISSEEKABLE(entry->file)) {
		return ESPIPE;
	}

	stat = kmalloc(sizeof(struct stat));
	VOP_STAT(entry->file, stat);
	eof = stat->st_size;

	seek = entry->offset;

	switch (whence) {
		case SEEK_SET:
		seek = pos;
		break;
		case SEEK_CUR:
		seek += pos;
		break;
		case SEEK_END:
		seek = eof + pos;
		break;
	}

	if (seek < 0) {
		return EINVAL;
	}

	entry->offset = seek;

	*retval0 = seek >> 32;
	*retval1 = seek & 0xFFFFFFFF;

	return 0;
}

int
sys_dup2(int oldfd, int newfd, int *output)
{
	struct ft *ft = curproc->proc_ft;
	struct ft_entry *entry = ft->entries[oldfd];

	if (!fd_valid_and_used(ft, oldfd)) {
		return EBADF;
	}

	if (newfd < 0 || newfd > OPEN_MAX) {
		return EBADF;
	}

	if (fd_valid_and_used(ft, newfd)){
		free_fd(ft, newfd);
	}

	assign_fd(ft, entry, newfd);
    *output = newfd;
	return 0;
}

int
sys_chdir(const char *pathname)
{
    char *path;
    size_t *path_len;
    int err;

    path = kmalloc(PATH_MAX);
    path_len = kmalloc(sizeof(int));

    /* Copy the string from userspace to kernel space and check for valid address */
    err = copyinstr((const_userptr_t) pathname, path, PATH_MAX, path_len);

    if (err){
        kfree(path);
        kfree(path_len);
        return err;
    }

	int result = vfs_chdir((char *) pathname);

    kfree(path);
    kfree(path_len);

	if (result) {
		return result;
	}
    
	return 0;
}

int
sys___getcwd(char *buf, size_t buflen, int32_t *output)
{
	struct iovec iov;
	struct uio u;
	int result;

	iov.iov_ubase = (userptr_t)buf;
	iov.iov_len = buflen;		 // length of the memory space
	u.uio_iov = &iov;
	u.uio_iovcnt = 1;
	u.uio_resid = buflen;          // amount to read from the file -> Amount left to transfer
	u.uio_offset = 0;//entry->offset;
	u.uio_segflg = UIO_USERSPACE;
	u.uio_rw = UIO_READ;
	u.uio_space = curproc->p_addrspace;

	result = vfs_getcwd(&u);
	if (result) {
		return result;
	}

	*output = buflen - u.uio_resid;
	return 0;
}
