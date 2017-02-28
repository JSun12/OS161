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

/*
Opens the given file for reading with the given flags.
*/
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
	if (entry == NULL) {
		return ENOMEM;
	}

    result = add_entry(curproc->proc_ft, entry, output);
    if (result){
        return result;
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

	entry->rwflags = flags;

    return 0;
}

/*
Closes the file at fd. Returns EBADF if fd is not used.
*/
int
sys_close(int fd)
{
	if(!fd_valid_and_used(curproc->proc_ft,fd)) {
		return EBADF;
	}

	free_fd(curproc->proc_ft, fd);

	return 0;
}

/*
Writes the data from buf up to buflen bytes to the file at fd, at the
current seek position. The file must be open for writing.
*/
int
sys_write(int fd, const void *buf, size_t nbytes, int32_t *retval0)
{

	if (!fd_valid_and_used(curproc->proc_ft, fd)) {
		return EBADF;
	}

    struct iovec iov;
	struct uio u;
	int result;
    struct ft_entry *entry = curproc->proc_ft->entries[fd];

	if (!(entry->rwflags & (O_WRONLY | O_RDWR))) {
		return EBADF;
	}

	iov.iov_ubase = (userptr_t)buf;
	iov.iov_len = nbytes;
	u.uio_iov = &iov;
	u.uio_iovcnt = 1;
	u.uio_resid = nbytes;
	u.uio_offset = entry->offset;
	u.uio_segflg = UIO_USERSPACE;
	u.uio_rw = UIO_WRITE;
	u.uio_space = curproc->p_addrspace;

	result = VOP_WRITE(entry->file, &u);
	if (result) {
		return result;
	}

	ssize_t len = nbytes - u.uio_resid;
	entry->offset += (off_t) len;

    *retval0 = len;
	return 0;
}

/*
Reads to buf up to buflen bytes from the file at fd,
at current seek position. The file must be open for reading.
*/
int
sys_read(int fd, void *buf, size_t buflen, ssize_t *retval0)
{

	if (!fd_valid_and_used(curproc->proc_ft, fd)) {
		return EBADF;
	}

    struct iovec iov;
	struct uio u;
	int result;
    struct ft_entry *entry = curproc->proc_ft->entries[fd];

	if (entry->rwflags & O_WRONLY) {
		return EBADF;
	}

	iov.iov_ubase = (userptr_t)buf;
	iov.iov_len = buflen;
	u.uio_iov = &iov;
	u.uio_iovcnt = 1;
	u.uio_resid = buflen;
	u.uio_offset = entry->offset;
	u.uio_segflg = UIO_USERSPACE;
	u.uio_rw = UIO_READ;
	u.uio_space = curproc->p_addrspace;

	result = VOP_READ(entry->file, &u);
	if (result) {
		return result;
	}

	ssize_t len = buflen - u.uio_resid;
	entry->offset += (off_t) len;

    *retval0 = len;
    return 0;
}

/*
Sets the file's seek position according to pos and whence.
*/
int
sys_lseek(int fd, off_t pos, int whence, int32_t *retval0, int32_t *retval1)
{

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

/*
Clones the instance of the open file at oldfd to newfd.
If there is an open file at newfd, it is closed.
*/
int
sys_dup2(int oldfd, int newfd, int *output)
{
    if (newfd < 0 || oldfd < 0 || newfd >= OPEN_MAX || oldfd >= OPEN_MAX) {
		return EBADF;
	}

    struct ft *ft = curproc->proc_ft;
	struct ft_entry *entry = ft->entries[oldfd];

	if (!fd_valid_and_used(ft, oldfd)) {
		return EBADF;
	}

	if (fd_valid_and_used(ft, newfd)){
		free_fd(ft, newfd);
	}

	assign_fd(ft, entry, newfd);
    *output = newfd;
	return 0;
}

/*
Changes the current working directory.
*/
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

/*
Stores the current working directory at buf.
*/
int
sys___getcwd(char *buf, size_t buflen, int32_t *output)
{
	struct iovec iov;
	struct uio u;
	int result;

	iov.iov_ubase = (userptr_t)buf;
	iov.iov_len = buflen;
	u.uio_iov = &iov;
	u.uio_iovcnt = 1;
	u.uio_resid = buflen;
	u.uio_offset = 0;
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
