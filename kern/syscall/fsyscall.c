#include <types.h>
#include <vfs.h>
#include <current.h>
#include <fsyscall.h>
#include <filetable.h>
#include <proc.h>
#include <uio.h>
#include <vnode.h>
#include <kern/stat.h>


// must make extern (use as errno for now)
int errno;


int
sys_open(const char *filename, int flags)
{
    struct vnode *new;
    int ret;

	ret = vfs_open((char *)filename, flags, 0, &new);
    if(ret){
		errno = ret; 
        return -1;
    }

    struct ft_entry *entry = entry_create(new);
    ret = add_entry(curproc->proc_ft, entry);
    if (ret == -1){
        return ret;
    }

	kprintf("Open: %d\n", ret);


    return ret;

}

/*
  Atomic removal of an entry from the filetable.
  Vfs file is also properly closed.
*/
int
sys_close(int fd)
{
	int ret = free_fd(curproc->proc_ft, fd);
	kprintf("Close: %d\n", fd);
	return ret;
}

/*
write writes up to buflen bytes to the file specified by fd, at the 
location in the file specified by the current seek position of the file, 
taking the data from the space pointed to by buf. The file must be open for writing.
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
	u.uio_offset = entry->offset;
	u.uio_segflg = UIO_USERSPACE;
	u.uio_rw = UIO_WRITE;
	u.uio_space = curproc->p_addrspace;

	result = VOP_WRITE(entry->file, &u);
	if (result) {
		return result;
	}

	entry->offset += nbytes - u.uio_resid; //hopefully correct implementation
	// kprintf("Current offset: %d\n", (int) entry->offset);

	// struct stat *stat; 
	// off_t eof; 
	// stat = kmalloc(sizeof(struct stat));
	// VOP_STAT(entry->file, stat);
	// eof = stat->st_size;
	// kprintf("eof: %d\n", (int) eof);

    return 0;
}
/*
read reads up to buflen bytes from the file specified by fd,
at the location in the file specified by the current seek position
of the file, and stores them in the space pointed to by buf.
The file must be open for reading.

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

	entry->offset += buflen - u.uio_resid; //hopefully correct implementation
	// kprintf("Current offset: %d\n", (int) entry->offset);
	
	
	// struct stat *stat; 
	// off_t eof; 
	// stat = kmalloc(sizeof(struct stat));
	// VOP_STAT(entry->file, stat);
	// eof = stat->st_size;
	// kprintf("eof: %d\n", (int) eof);

    return 0;
}

off_t 
sys_lseek(int fd, off_t pos, int whence)
{

	// kprintf("%d\n", fd);
	// kprintf("%ld\n", (long) pos);
	// kprintf("%d\n", whence);
	//XXX: Lock this

	struct ft *ft = curproc->proc_ft;
	struct ft_entry *entry; 
	struct stat *stat; 
	off_t eof; 

	stat = kmalloc(sizeof(struct stat));

	entry = ft->entries[fd];

	VOP_STAT(entry->file, stat);
	eof = stat->st_size;

	switch (whence) {
		case SEEK_SET: 
		entry->offset = pos; 
		break;
		case SEEK_CUR:
		entry->offset += pos; 
		break;
		case SEEK_END:
		entry->offset = eof + pos;
		break;	
	}

	return entry->offset;
}

int
sys_dup2(int oldfd, int newfd)
{
	struct ft *ft = curproc->proc_ft;
	struct ft_entry *entry = ft->entries[oldfd];

	if(used(ft, newfd)){
		free_fd(ft, newfd);
	}

	assign_fd(ft, entry, newfd);
	return newfd;
}

