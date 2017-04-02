#include <types.h>
#include <kern/errno.h>
#include <spinlock.h>
#include <vm.h>
#include <current.h>
#include <signal.h>
#include <spl.h>
#include <proc.h>
#include <wchan.h>
#include <addrspace.h>
#include <lib.h>
#include <mips/tlb.h>
#include <vfs.h>
#include <kern/fcntl.h>
#include <uio.h>
#include <vnode.h>

struct spinlock global = SPINLOCK_INITIALIZER;

struct coremap *cm;
struct spinlock cm_spinlock = SPINLOCK_INITIALIZER;
volatile size_t cm_counter = 0;

// TODO: probs need to change for tlb shootdown
// struct spinlock tlb_spinlock = SPINLOCK_INITIALIZER;

static p_page_t first_alloc_page; /* First physical page that can be dynamically allocated */
static p_page_t last_page; /* One page past the last free physical page in RAM */
static p_page_t first_page_swap; /* First physical page number that is allocated to swap */
static p_page_t last_page_swap; /* One page past the last free physical page SWAP */

// TODO: maybe this couting policy is ugly. If it works, clean it up by using proper types, etc.
static struct vnode *swap_disk;
static const char swap_dir[] = "lhd0raw:";

// test
static char content[0x1000];
static cm_entry_t old_mem; 
// static l1_entry_t l1[4];
static bool check = false;

// static struct cv *io_cv; 
// static struct lock *io_lock; 

struct wchan *io_wc; 
volatile bool io_flag;

static struct spinlock counter_spinlock = SPINLOCK_INITIALIZER;
static volatile int swap_out_counter = 0;
static volatile p_page_t swapclock;

#define SWAP_OUT_COUNT    0x1
#define NUM_FREE_PPAGES   8

// PASSING MATMULT!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

/////////////////////////////////////////////////////////////////////////////////////////
bool
in_ram(p_page_t p_page)
{   
    bool in_ram = (first_alloc_page <= p_page && p_page < last_page);
    return in_ram;
}

bool
in_swap(p_page_t p_page)
{
    bool in_swap = (first_page_swap <= p_page && p_page < last_page_swap);
    return in_swap;
}

bool
in_all_memory(p_page_t p_page)
{
    bool in_all_memory = (first_alloc_page <= p_page && p_page < last_page_swap);
    return in_all_memory;
}


static
void
kalloc_ppage(p_page_t p_page)
{
    v_page_t v_page = PPAGE_TO_KVPAGE(p_page);
    cm->cm_entries[p_page] = 0 | PP_USED | v_page;
    cm_counter++;
}

static
bool
p_page_used(p_page_t p_page)
{
    cm_entry_t entry = cm->cm_entries[p_page];
    bool used = entry & PP_USED;
    return used;
}


// TODO: change this to a for loop
static
int
find_free(size_t npages, p_page_t *start)
{
    while (*start < last_page - npages) {
        if (!p_page_used(*start)) {
            size_t offset;
            for (offset = 0; offset < npages; offset++) {
                if (p_page_used(*start + offset)) {
                    break;
                }
            }

            if (offset == npages) {
                break;
            }
        }

        (*start)++;
    }

    if (*start == last_page - npages) {
        return ENOMEM;
    }

    return 0;
}

void
free_ppage(p_page_t p_page)
{
    KASSERT(in_ram(p_page));

    cm->cm_entries[p_page] = 0;
    cm->pids8_entries[p_page] = 0;
    cm_counter--;
}

void
free_ppage_swap(p_page_t p_page)
{
    KASSERT(in_swap(p_page));

    cm->cm_entries[p_page] = 0; 
    cm->pids8_entries[p_page] = 0;
}

size_t
cm_getref(p_page_t p_page)
{
    KASSERT(in_all_memory(p_page));

    return GET_REF(cm->cm_entries[p_page]);
}

void
cm_incref(p_page_t p_page)
{
    KASSERT(in_all_memory(p_page));

    size_t curref = GET_REF(cm->cm_entries[p_page]);
    curref++;
    SET_REF(cm->cm_entries[p_page], curref);
}

void
cm_decref(p_page_t p_page)
{
    KASSERT(in_all_memory(p_page));

    size_t curref = GET_REF(cm->cm_entries[p_page]);
    curref--;
    SET_REF(cm->cm_entries[p_page], curref);
}

// TODO: too many magic numbers?
void
set_pid8(p_page_t p_page, pid_t pid, uint32_t pos)
{
    KASSERT(pos < NUM_CM_PIDS);
    KASSERT(in_all_memory(p_page));

    pid_t pid_to_add = 0;

    if (pid < MAX_CM_PID) {
        pid_to_add = pid;
    }

    cm->pids8_entries[p_page] = cm->pids8_entries[p_page] & (~(0x000000ff << pos*8));
    cm->pids8_entries[p_page] = cm->pids8_entries[p_page] | (pid_to_add << pos*8);
}

pid_t
get_pid8(p_page_t p_page, uint32_t pos) 
{
    KASSERT(pos < NUM_CM_PIDS);
    KASSERT(in_all_memory(p_page));

    return (cm->pids8_entries[p_page] & (0x000000ff << pos*8)) >> pos*8;
}

void
add_pid8(p_page_t p_page, pid_t pid)
{
    KASSERT(in_all_memory(p_page)); 

    if (!(pid < MAX_CM_PID)) {
        return; 
    }    

    for (uint32_t pos = 0; pos < NUM_CM_PIDS; pos++) {
        if (get_pid8(p_page, pos) == 0) {
            set_pid8(p_page, pid, pos);
            return;
        }
    }

    return;
}

void
rem_pid8(p_page_t p_page, pid_t pid)
{
    KASSERT(in_all_memory(p_page));

    if (!(pid < MAX_CM_PID)) {
        return;
    }

    uint32_t pos = 0; 
    while (pos < NUM_CM_PIDS) {
        pid_t cur_pid = get_pid8(p_page, pos); 
        if (cur_pid == pid) {
            set_pid8(p_page, 0, pos);
            break;
        }
        pos++;
    }

    while (pos < NUM_CM_PIDS - 1) {
        set_pid8(p_page, get_pid8(p_page, pos + 1), pos);
        set_pid8(p_page, 0, pos + 1);
        pos++;
    }

    return;
}
////////////////////////////////////////////////////////////////////////////////////

void
vm_bootstrap()
{
    paddr_t cm_paddr = ram_stealmem(COREMAP_PAGES);
    KASSERT(cm_paddr != 0);

    cm = (struct coremap *) PADDR_TO_KVADDR(cm_paddr);

    KASSERT(ram_stealmem(0) % PAGE_SIZE == 0);
    first_alloc_page = ADDR_TO_PAGE(ram_stealmem(0));
    swapclock = first_alloc_page;
    last_page = ADDR_TO_PAGE(ram_getsize());

    size_t pages_used = first_alloc_page;
    for (p_page_t p_page = 0; p_page < pages_used; p_page++) {
        kalloc_ppage(p_page);
    }
}

void
swap_bootstrap()
{
    // TODO: this is probably not appropriate
    int ret; 
    char dir[sizeof(swap_dir)];
    strcpy(dir, swap_dir);

    ret = vfs_open(dir, O_RDWR, 0, &swap_disk);
    if (ret) {
        panic("swap disk wasn't able to open\n");
    }

    first_page_swap = last_page; 
    last_page_swap = 0x400;

    io_wc = wchan_create("io_wc");
    io_flag = false;
}

vaddr_t
alloc_kpages(size_t npages)
{
    int result;
    bool acquired = spinlock_do_i_hold(&cm_spinlock);

    if (!acquired) {
        spinlock_acquire(&cm_spinlock);
    }

    p_page_t start = first_alloc_page;
    result = find_free(npages, &start);
    if (result) {
        if (!acquired) {
            spinlock_release(&cm_spinlock);
        }
        return 0;
    }

    for (p_page_t p_page = start; p_page < start + npages; p_page++) {
        kalloc_ppage(p_page);
    }

    cm->cm_entries[start + npages - 1] = cm->cm_entries[start + npages - 1] | KMALLOC_END;

    if (!acquired) {
        spinlock_release(&cm_spinlock);
    }

    return PADDR_TO_KVADDR(PAGE_TO_ADDR(start));
}



void
free_kpages(vaddr_t addr)
{
    p_page_t curr = ADDR_TO_PAGE(KVADDR_TO_PADDR(addr));
    bool end;
    bool acquired = spinlock_do_i_hold(&cm_spinlock);

    if (!acquired) {
        spinlock_acquire(&cm_spinlock);
    }

    while (p_page_used(curr)) {
        end = cm->cm_entries[curr] & KMALLOC_END;
        free_ppage(curr);
        if (end) {
            if (!acquired) {
                spinlock_release(&cm_spinlock);
            }
            return;
        }
        curr++;
    }

    if (!acquired) {
        spinlock_release(&cm_spinlock);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

// TODO: Test this function
void
vm_tlbshootdown_all()
{
    for (int i = 0; i < NUM_TLB; i++){
        tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
    }
}



void
vm_tlbshootdown(const struct tlbshootdown *tlbsd)
{
    vaddr_t v_page = tlbsd->v_page_num;
    pid_t pid = tlbsd->pid;
    uint32_t entryhi = 0 | (v_page & PAGE_FRAME) | pid << 6;
    int32_t index = tlb_probe(entryhi, 0);

    if (index > -1) {
        tlb_write(TLBHI_INVALID(index), TLBLO_INVALID(), index);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////
static
bool
entry_swappable(p_page_t p_page)
{
    KASSERT(in_ram(p_page));

    if (!p_page_used(p_page)) {
        return false;
    }

    // For now, we don't want to evict l1 page tables
    if ((cm->cm_entries[p_page] & PAGE_MASK) >= 0x00080000) {
        return false;
    }

    size_t ref = cm_getref(p_page);
    if (ref > NUM_CM_PIDS) {
        return false;
    }

    for (uint32_t pos = 0; pos < ref; pos++) {
        pid_t pid = get_pid8(p_page, pos); 
        if (pid == 0) {
            return false;
        }
    }

    return true;
}

// TODO: magic number
static
bool
entry_recently_used(p_page_t p_page)
{
    KASSERT(in_ram(p_page));
    
    if (cm->cm_entries[p_page] & REF_BIT) {
        return true;
    }

    int spl = splhigh();

	uint32_t entryhi;
	uint32_t entrylo;
	uint32_t index;

	for (index = 0; index < NUM_TLB; index++) {
		tlb_read(&entryhi, &entrylo, index);
        if (p_page == ((entrylo & TLBLO_PPAGE) >> 12)) {
            splx(spl);
            return true;
        }
	}

    splx(spl);

    return false;
}

//TODO: Proper error code
static
int
find_free_swap(p_page_t *p_page)
{
    p_page_t page; 
    for (page = first_page_swap; page < last_page_swap; page++) {
        if (!p_page_used(page)) {
            *p_page = page;
            return 0; 
        }
    }

    return ENOMEM; 
}

static 
off_t
swap_offset(p_page_t p_page) 
{
    KASSERT(in_swap(p_page));

    return PAGE_TO_ADDR(p_page - first_page_swap);
}

static
struct uio *
swap_evict_uio(p_page_t p_page)
{
    KASSERT(in_swap(p_page));

    struct iovec *iov = kmalloc(sizeof(struct iovec)); 
    struct uio *u = kmalloc(sizeof(struct uio));

    // TODO: same dirty hack; using the kernel to take data from memory
    iov->iov_kbase = (void *) PAGE_TO_ADDR(PPAGE_TO_KVPAGE(swapclock));
    iov->iov_len = PAGE_SIZE;
    u->uio_iov = iov;
    u->uio_iovcnt = 1;
    u->uio_resid = PAGE_SIZE;
    u->uio_offset = swap_offset(p_page);
    u->uio_segflg = UIO_SYSSPACE;
    u->uio_rw = UIO_WRITE;
    u->uio_space = NULL;

    return u;
}

static
struct uio *
swap_load_uio(p_page_t p_page, p_page_t old_p_page)
{
    KASSERT(in_ram(p_page));

    struct iovec *iov = kmalloc(sizeof(struct iovec)); 
    struct uio *u = kmalloc(sizeof(struct uio));

    // TODO: same dirty hack; using the kernel to take data from memory
    iov->iov_kbase = (void *) PAGE_TO_ADDR(PPAGE_TO_KVPAGE(p_page));
    iov->iov_len = PAGE_SIZE;
    u->uio_iov = iov;
    u->uio_iovcnt = 1;
    u->uio_resid = PAGE_SIZE;
    u->uio_offset = swap_offset(old_p_page);
    u->uio_segflg = UIO_SYSSPACE;
    u->uio_rw = UIO_READ;
    u->uio_space = NULL;

    return u;
}

static 
void 
swap_uio_cleanup(struct uio *u)
{
    kfree(u->uio_iov); 
    kfree(u); 
}

static
void
update_pt_entries(p_page_t swap_to_page)
{
    size_t refs = cm_getref(swap_to_page);

    for (uint32_t pos = 0; pos < refs; pos++) {
        pid_t pid = get_pid8(swap_to_page, pos);
        KASSERT(pid != 0);

        io_flag = true;
        spinlock_release(&cm_spinlock);
        spinlock_release(&global);

        struct proc *proc = get_pid(pid);
        KASSERT(proc != NULL); 

        spinlock_acquire(&global);
        spinlock_acquire(&cm_spinlock);
        io_flag = false;
        wchan_wakeall(io_wc, &global);
        
        struct addrspace *as = proc->p_addrspace; 
        KASSERT(as != NULL); 

        struct l2_pt *l2_pt = as->l2_pt; 

        v_page_t v_page = cm->cm_entries[swap_to_page] & VP_MASK;
        v_page_l2_t v_l2 = L2_PNUM(PAGE_TO_ADDR(v_page)); 
        v_page_l1_t v_l1 = L1_PNUM(PAGE_TO_ADDR(v_page));

        l2_entry_t l2_entry = l2_pt->l2_entries[v_l2]; 

        KASSERT(l2_entry & ENTRY_VALID); 
        
        struct l1_pt *l1_pt = (struct l1_pt *) PAGE_TO_ADDR(l2_entry & PAGE_MASK);

        // if(!check) {
        //     l1[pos] = l1_pt->l1_entries[v_l1];
        //     if ((l1_pt->l1_entries[v_l1] & PAGE_MASK) != swapclock){
        //         // KASSERT((l1_pt->l1_entries[v_l1] & PAGE_MASK) == swapclock);
        //     }
        // }

        l1_pt->l1_entries[v_l1] = l1_pt->l1_entries[v_l1] & (~PAGE_MASK);   
        l1_pt->l1_entries[v_l1] = l1_pt->l1_entries[v_l1] | swap_to_page;
        
        // if (check) {
        //     if (!(l1[pos] == l1_pt->l1_entries[v_l1])) {
        //         // KASSERT(l1[pos] == l1_pt->l1_entries[v_l1]);
        //     }
        // }
    }
}

static
void
swapclock_tick()
{
    KASSERT(in_ram(swapclock));

    if (swapclock == last_page - 1) {
        swapclock = first_alloc_page;
    } else {
        swapclock++;
    }
}

// TODO: Proper error numbers
// clean up iovec in uio
int
swap_out()
{
    size_t free_pages = last_page - cm_counter;

    if (free_pages >= NUM_FREE_PPAGES) {
        return -1; 
    }

    p_page_t first_clock = swapclock;
    int cycles = 0; 
    int result;

    spinlock_acquire(&cm_spinlock);

    // We iterate for 2 cycles, since after the first cycle, the reference bits are cleared
    while (cycles < 2) {
        if (entry_swappable(swapclock) && !entry_recently_used(swapclock)) {
            p_page_t swap_to_page; 

            result = find_free_swap(&swap_to_page);
            if (result) {
                spinlock_release(&cm_spinlock);
                return result;
            }

            cm->cm_entries[swap_to_page] = cm->cm_entries[swapclock];
            cm->pids8_entries[swap_to_page] = cm->pids8_entries[swapclock];

            struct uio *u = swap_evict_uio(swap_to_page);

            io_flag = true;
            spinlock_release(&cm_spinlock);
            spinlock_release(&global);

            VOP_WRITE(swap_disk, u);
            swap_uio_cleanup(u);

            spinlock_acquire(&global);
            spinlock_acquire(&cm_spinlock);
            io_flag = false;
            wchan_wakeall(io_wc, &global);

            update_pt_entries(swap_to_page);

            free_ppage(swapclock);

            spinlock_release(&cm_spinlock);
            return 0;
        }

        cm->cm_entries[swapclock] = cm->cm_entries[swapclock] & (~REF_BIT);

        swapclock_tick(); 
        if (swapclock == first_clock) {
            cycles++;
        }
    }

    spinlock_release(&cm_spinlock);

    return -1;
}

int
swap_out_test(p_page_t *mem_page, p_page_t *swap_page)
{
    // size_t free_pages = last_page - cm_counter;

    // if (free_pages >= NUM_FREE_PPAGES) {
    //     return -1; 
    // }


    p_page_t first_clock = swapclock;
    int cycles = 0; 
    int result;

    spinlock_acquire(&cm_spinlock);

    // We iterate for 2 cycles, since after the first cycle, the reference bits are cleared
    while (cycles < 2) {
        if (entry_swappable(swapclock) && !entry_recently_used(swapclock)) {
            p_page_t swap_to_page; 

            result = find_free_swap(&swap_to_page);
            if (result) {
                spinlock_release(&cm_spinlock);
                return result;
            }

            old_mem = cm->cm_entries[swapclock];            

            cm->cm_entries[swap_to_page] = cm->cm_entries[swapclock];
            cm->pids8_entries[swap_to_page] = cm->pids8_entries[swapclock];

            const void *src = (const void *) PADDR_TO_KVADDR(PAGE_TO_ADDR(swapclock));
            void *dst = (void *) &content;
            memmove(dst, src, (size_t) PAGE_SIZE);

            struct uio *u = swap_evict_uio(swap_to_page);

            io_flag = true;
            spinlock_release(&cm_spinlock);
            spinlock_release(&global);

            VOP_WRITE(swap_disk, u);
            swap_uio_cleanup(u);

            spinlock_acquire(&global);
            spinlock_acquire(&cm_spinlock);
            io_flag = false;
            wchan_wakeall(io_wc, &global);

            KASSERT(cm->cm_entries[swap_to_page] == cm->cm_entries[swapclock]);
            KASSERT(cm->pids8_entries[swap_to_page] == cm->pids8_entries[swapclock]);

            update_pt_entries(swap_to_page);

            // free_ppage(swapclock);
            *mem_page = swapclock;
            *swap_page = swap_to_page;

            spinlock_release(&cm_spinlock);
            return 0;
        }

        cm->cm_entries[swapclock] = cm->cm_entries[swapclock] & (~REF_BIT);

        swapclock_tick(); 
        if (swapclock == first_clock) {
            cycles++;
        }
    }

    spinlock_release(&cm_spinlock);

    return -1;
}

// add any error codes
int
swap_in(p_page_t p_page, p_page_t old_p_page)
{
    KASSERT(cm->cm_entries[p_page] & PP_USED);
    KASSERT(cm->cm_entries[old_p_page] & PP_USED);
    KASSERT(in_ram(p_page));
    KASSERT(in_swap(old_p_page));

    struct uio *u = swap_load_uio(p_page, old_p_page);

    io_flag = true;
    spinlock_release(&cm_spinlock);
    spinlock_release(&global);

    VOP_READ(swap_disk, u);
    swap_uio_cleanup(u);

    spinlock_acquire(&global);
    spinlock_acquire(&cm_spinlock);
    io_flag = false;
    wchan_wakeall(io_wc, &global);

    // for (unsigned i = 0; i < PAGE_SIZE; i++) {
    //     char *ptr = (char *) (PAGE_TO_ADDR(PPAGE_TO_KVPAGE(p_page)) + i);
    //     if (*ptr != content[i]) {
    //         KASSERT(*ptr == content[i]);
    //     }
    // }

    return 0; 
}

////////////////////////////////////////////////////////////////////////////////////////////////////

/*
(Pasindu's thoughts; don't worry if this is gibberish):
currently, readable and writable valid PTEs, user faultaddress. Now, when
read only PTEs are there, if faulttype is read, then same as before, but if
faulttype is write, we copy physical page content to another physical page,
and possibly the l1 page table.

if not specified (that is, valid), assume readable and writable.
*/
int
vm_fault(int faulttype, vaddr_t faultaddress)
{

    spinlock_acquire(&global);
    while (io_flag) {
        wchan_sleep(io_wc, &global);
    }

    KASSERT(io_flag == false);

    spinlock_acquire(&counter_spinlock);

    if (swap_out_counter == SWAP_OUT_COUNT - 1) {      
        swap_out_counter = 0;
        spinlock_release(&counter_spinlock);
        swap_out();
    } else {
        swap_out_counter++;
        spinlock_release(&counter_spinlock);
    }

    /*
    we have to adjust amount of swap depending on amount of ram (hard coded for now);
    */

    if (0) {
    spinlock_acquire(&counter_spinlock);

    if (swap_out_counter == SWAP_OUT_COUNT - 1) {      
        swap_out_counter = 0;
        spinlock_release(&counter_spinlock);
        p_page_t mem_page; 
        p_page_t swap_page;
        check = false;
        int ret = swap_out_test(&mem_page, &swap_page);

        if (ret == 0) {
            spinlock_acquire(&cm_spinlock);
            KASSERT(cm->cm_entries[mem_page] == cm->cm_entries[swap_page]);
            KASSERT(cm->pids8_entries[mem_page] == cm->pids8_entries[swap_page]);

            check = true;
            swap_in(mem_page, swap_page);
            update_pt_entries(mem_page);
            free_ppage_swap(swap_page);

            KASSERT(cm->cm_entries[mem_page] == old_mem);
            spinlock_release(&cm_spinlock);
        }
    } else {
        swap_out_counter++;
        spinlock_release(&counter_spinlock);
    }
    }

    struct addrspace *as = curproc->p_addrspace;
    pid_t pid = curproc->pid;

    // TODO: do a proper address check (make sure kernel addresses aren't called)
    if (as->brk <= faultaddress && faultaddress < as->stack_top) {
        spinlock_release(&global);
        return SIGSEGV;
    }

    int result;

    vaddr_t fault_page = faultaddress & PAGE_FRAME;
    v_page_l2_t v_l2 = L2_PNUM(fault_page);
    v_page_l1_t v_l1 = L1_PNUM(fault_page);

    struct l2_pt *l2_pt = as->l2_pt;
    l2_entry_t l2_entry = l2_pt->l2_entries[v_l2];

    struct l1_pt *l1_pt;

    // Get the l1 page table.
    if (l2_entry & ENTRY_VALID) {
        if (faulttype == VM_FAULT_READONLY && !(l2_entry & ENTRY_WRITABLE)) {
            v_page_t v_page = l2_entry & PAGE_MASK;
            p_page_t p_page = KVPAGE_TO_PPAGE(v_page);
            KASSERT(in_ram(p_page));

            spinlock_acquire(&cm_spinlock);

            if (cm_getref(p_page) > 1) {
                result = l1_create(&l1_pt);
                if (result) {
                    spinlock_release(&cm_spinlock);
                    spinlock_release(&global);
                    return result;
                }

                p_page_t new_p_page = ADDR_TO_PAGE(KVADDR_TO_PADDR((vaddr_t) l1_pt));

                struct l1_pt *l1_pt_orig = (struct l1_pt *) PAGE_TO_ADDR(v_page);
                for (v_page_l1_t l1_val = 0; l1_val < NUM_L1PT_ENTRIES; l1_val++) {
                    l1_pt->l1_entries[l1_val] = l1_pt_orig->l1_entries[l1_val];
                }

                l2_pt->l2_entries[v_l2] = 0
                                        | ENTRY_VALID
                                        | ENTRY_READABLE
                                        | ENTRY_WRITABLE
                                        | ADDR_TO_PAGE((vaddr_t) l1_pt);
                cm_incref(new_p_page);
                add_pid8(new_p_page, pid);

                cm_decref(p_page);
                rem_pid8(p_page, pid);
            } else {
                l2_pt->l2_entries[v_l2] = l2_pt->l2_entries[v_l2] | ENTRY_WRITABLE;
                l1_pt = (struct l1_pt *) PAGE_TO_ADDR(v_page);
            }

            spinlock_release(&cm_spinlock);
        } else {
            v_page_t v_page = l2_entry & PAGE_MASK;
            KASSERT(in_ram(KVPAGE_TO_PPAGE(v_page)));
            l1_pt = (struct l1_pt *) PAGE_TO_ADDR(v_page);
        }
    } else {
        result = l1_create(&l1_pt);
        if (result) {
            spinlock_release(&global);
            return result;
        }

        spinlock_acquire(&cm_spinlock);

        v_page_t v_page = ADDR_TO_PAGE((vaddr_t) l1_pt);
        l2_pt->l2_entries[v_l2] = 0
                                | ENTRY_VALID
                                | ENTRY_READABLE
                                | ENTRY_WRITABLE
                                | v_page;

        p_page_t p_page = KVPAGE_TO_PPAGE(v_page);

        cm_incref(p_page);
        add_pid8(p_page, pid);

        spinlock_release(&cm_spinlock);
    }

    l1_entry_t l1_entry = l1_pt->l1_entries[v_l1];
    p_page_t p_page;

    // Get the faulting address' physical address
    if (l1_entry & ENTRY_VALID) {
        if (faulttype == VM_FAULT_READONLY && !(l1_entry & ENTRY_WRITABLE)) {
            p_page_t old_page = l1_entry & PAGE_MASK;
            KASSERT(in_all_memory(old_page));
            KASSERT((cm->cm_entries[old_page] & PAGE_MASK) == ADDR_TO_PAGE(fault_page));
            KASSERT(in_ram(old_page)); // used as a test (this shouldn't be here)

            spinlock_acquire(&cm_spinlock);

            if (cm_getref(old_page) > 1) {
                // TODO: reduce code repetition with kmalloc
                p_page = first_alloc_page;
                result = find_free(1, &p_page);
                if (result) {
                    spinlock_release(&cm_spinlock);
                    spinlock_release(&global);
                    return result;
                }

                cm_counter++;
                cm->cm_entries[p_page] = 0
                                       | PP_USED
                                       | ADDR_TO_PAGE(fault_page);
                cm_incref(p_page);
                add_pid8(p_page, pid);

                cm_decref(old_page);
                rem_pid8(old_page, pid);

                // TODO: get rid of this dirty hack
                const void *src = (const void *) PADDR_TO_KVADDR(PAGE_TO_ADDR(old_page));
                void *dst = (void *) PADDR_TO_KVADDR(PAGE_TO_ADDR(p_page));
                memmove(dst, src, (size_t) PAGE_SIZE);

                l1_pt->l1_entries[v_l1] = 0
                                        | ENTRY_VALID
                                        | ENTRY_READABLE
                                        | ENTRY_WRITABLE
                                        | p_page;
            } else {
                l1_pt->l1_entries[v_l1] = l1_pt->l1_entries[v_l1] | ENTRY_WRITABLE;
                p_page = old_page;
            }

            spinlock_release(&cm_spinlock);
        } else {
            p_page_t old_page = l1_pt->l1_entries[v_l1] & PAGE_MASK;
            KASSERT(in_all_memory(old_page));
            KASSERT((cm->cm_entries[old_page] & PAGE_MASK) == ADDR_TO_PAGE(fault_page));

            spinlock_acquire(&cm_spinlock);

            // must swap in and update pt entries 
            if (in_swap(old_page)) {
                p_page = first_alloc_page;
                result = find_free(1, &p_page);
                if (result) {
                    spinlock_release(&cm_spinlock);
                    spinlock_release(&global);
                    return result;
                }

                cm_counter++;
                cm->cm_entries[p_page] = cm->cm_entries[old_page];
                cm->pids8_entries[p_page] = cm->pids8_entries[old_page];
                swap_in(p_page, old_page);
                update_pt_entries(p_page);
                free_ppage_swap(old_page);
            } else {
                p_page = old_page;
            }

            spinlock_release(&cm_spinlock);
        }
    } else {
        spinlock_acquire(&cm_spinlock);

        p_page = first_alloc_page;
        result = find_free(1, &p_page);
        if (result) {
            spinlock_release(&cm_spinlock);
            spinlock_release(&global);
            return result;
        }

        cm_counter++;
        cm->cm_entries[p_page] = 0
                                | PP_USED
                                | ADDR_TO_PAGE(fault_page);

        l1_pt->l1_entries[v_l1] = 0
                                | ENTRY_VALID
                                | ENTRY_READABLE
                                | ENTRY_WRITABLE
                                | p_page;
        cm_incref(p_page);
        add_pid8(p_page, pid);     

        spinlock_release(&cm_spinlock);
    }

    uint32_t entryhi;
    uint32_t entrylo;
    l1_entry_t new_l1_entry = l1_pt->l1_entries[v_l1];
    p_page_t p_page_high = p_page << 12;

    cm->cm_entries[p_page] = cm->cm_entries[p_page] | REF_BIT;

    entryhi = 0 | fault_page | pid << 6;

    if (new_l1_entry & ENTRY_WRITABLE) {
        entrylo = 0 | p_page_high | TLBLO_VALID | TLBLO_DIRTY;
    } else {
        entrylo = 0 | p_page_high | TLBLO_VALID;
    }

    // pretty ugly tlb eviction. oh well. this is needed for matmult to work. otherwise, we get thrashing
    int spl = splhigh();

	uint32_t entryhi2;
	uint32_t entrylo2;
	uint32_t index;
    bool written = false;

	for (index = 0; index < NUM_TLB; index++) {
		tlb_read(&entryhi2, &entrylo2, index);
        if ((entryhi & TLBHI_VPAGE) == (entryhi2 & TLBHI_VPAGE)) {
            tlb_write(entryhi, entrylo, index);
            written = true;
            break;
        }
	}

    if (!written) {
        tlb_random(entryhi, entrylo);
    }

    splx(spl);

    spinlock_release(&global);
    return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////////////

void
free_vpage(struct l2_pt *l2_pt, v_page_t v_page)
{
    KASSERT(l2_pt != NULL);

    v_page_l2_t v_l2 = L2_PNUM(PAGE_TO_ADDR(v_page));
    v_page_l1_t v_l1 = L1_PNUM(PAGE_TO_ADDR(v_page));

    if (l2_pt->l2_entries[v_l2] & ENTRY_VALID) {
        v_page_t l1_pt_page = l2_pt->l2_entries[v_l2] & PAGE_MASK;
        struct l1_pt *l1_pt = (struct l1_pt*) PAGE_TO_ADDR(l1_pt_page);

        l1_entry_t l1_entry = l1_pt->l1_entries[v_l1];

        if (l1_entry & ENTRY_VALID) {
            p_page_t p_page = l1_entry & PAGE_MASK;

            spinlock_acquire(&cm_spinlock);

            if (cm_getref(p_page) > 1) {
                cm_decref(p_page);
            } else {
                free_ppage(p_page);
            }

            spinlock_release(&cm_spinlock);

            l1_pt->l1_entries[v_l1] = 0;
        }
    }
}

void
free_l1_pt(struct l2_pt *l2_pt, v_page_l2_t v_l2)
{
    if (l2_pt->l2_entries[v_l2] & ENTRY_VALID) {
        v_page_t v_page = l2_pt->l2_entries[v_l2] & PAGE_MASK;
        p_page_t p_page = KVPAGE_TO_PPAGE(v_page);

        spinlock_acquire(&cm_spinlock);

        if (cm_getref(p_page) > 1) {
            cm_decref(p_page);
        } else {
            struct l1_pt *l1_pt = (struct l1_pt *) PAGE_TO_ADDR(v_page);
            kfree(l1_pt);
        }

        spinlock_release(&cm_spinlock);

        l2_pt->l2_entries[v_l2] = 0;
    }
}

//TODO: Deal with return value
static
int
add_emptypage(v_page_l1_t v_l1, v_page_l2_t v_l2, vaddr_t vaddr, struct addrspace *as) {
    int result;
    struct l2_pt *l2_pt = as->l2_pt;
    l2_entry_t l2_entry = l2_pt->l2_entries[v_l2];
    struct l1_pt *l1_pt = (struct l1_pt *) PAGE_TO_ADDR(l2_entry & PAGE_MASK);
    p_page_t p_page;

    spinlock_acquire(&cm_spinlock);

    p_page = first_alloc_page;
    result = find_free(1, &p_page);
    if (result) {
        spinlock_release(&cm_spinlock);
        return result;
    }

    cm_counter++;
    cm->cm_entries[p_page] = 0
                            | PP_USED
                            | ADDR_TO_PAGE(vaddr);

    set_pid8(p_page, curproc->pid, 0);

    l1_pt->l1_entries[v_l1] = 0
                            | ENTRY_VALID
                            | ENTRY_READABLE
                            | ENTRY_WRITABLE
                            | p_page;
    cm_incref(p_page);

    spinlock_release(&cm_spinlock);

    return 0;
}

//TODO: Deal with return value
static
int
add_l1table(v_page_l2_t v_l2, struct addrspace *as) {

    int result;
    struct l1_pt *l1_pt;
    struct l2_pt *l2_pt = as->l2_pt;

    spinlock_acquire(&cm_spinlock);

    //XXX: sbrk doesnt return result. I just copied this code from vm_fault
    result = l1_create(&l1_pt);
    if (result) {
        spinlock_release(&cm_spinlock);
        return result;
    }

    p_page_t new_p_page = ADDR_TO_PAGE(KVADDR_TO_PADDR((vaddr_t) l1_pt));
    set_pid8(new_p_page, curproc->pid, 0);

    l2_pt->l2_entries[v_l2] = 0
                            | ENTRY_VALID
                            | ENTRY_READABLE
                            | ENTRY_WRITABLE
                            | ADDR_TO_PAGE((vaddr_t) l1_pt);

    cm_incref(new_p_page);

    spinlock_release(&cm_spinlock);

    return 0;
}

/*
sbrk is almost done. all there is left to do is figure out how to set the inital break
(which is probably done while loading the elf, maybe in as define region), and to check
the fault address values in vm_fault to make sure they are valid.
*/
int
sys_sbrk(ssize_t amount, int32_t *retval0)
{
    if (amount % PAGE_SIZE) {
        return EINVAL;
    }
    struct addrspace *as = curproc->p_addrspace;
    spinlock_acquire(&global);

    lock_acquire(as->as_lock);

    vaddr_t stack_top = as->stack_top;
    vaddr_t old_heap_end = as->brk;
    vaddr_t new_heap_end = old_heap_end + amount;
    if (new_heap_end < as->heap_base) {
        lock_release(as->as_lock);
        spinlock_release(&global);
        return EINVAL;
    }

    int64_t overflow = (int64_t)old_heap_end + (int64_t)amount;
    if (overflow > USERSPACETOP || overflow < 0){
        lock_release(as->as_lock);
        spinlock_release(&global);
        return EINVAL;
    }

    if (new_heap_end > stack_top) {
        lock_release(as->as_lock);
        spinlock_release(&global);
        return ENOMEM;
    }

    // Free physical pages of deallocated virtual pages
    if (new_heap_end < old_heap_end) {
        v_page_l2_t old_l2 = L2_PNUM(old_heap_end);
        v_page_l1_t old_l1 = L1_PNUM(old_heap_end);
        v_page_l2_t new_l2 = L2_PNUM(new_heap_end);
        v_page_l1_t new_l1 = L1_PNUM(new_heap_end);
        struct l2_pt *l2_pt = as->l2_pt;

        //NOTE: This could likely be changed to a single for loop that goes once is oldl2 == newl2
        // Also, check to make sure we want < and not <=
        if (old_l2 == new_l2) {
            for (v_page_l1_t v_l1 = new_l1; v_l1 < old_l1; v_l1++) {
                v_page_t v_page = PNUM_TO_PAGE(new_l2, v_l1);
                free_vpage(l2_pt, v_page);
            }
        } else {
            for (v_page_l2_t v_l2 = new_l2 + 1; v_l2 < old_l2; v_l2++) {
                for (v_page_l1_t v_l1 = 0; v_l1 < NUM_L1PT_ENTRIES; v_l1++) {
                    v_page_t v_page = PNUM_TO_PAGE(v_l2, v_l1);
                    free_vpage(l2_pt, v_page);
                }

                free_l1_pt(l2_pt, v_l2);
            }

            for (v_page_l1_t v_l1 = 0; v_l1 < old_l1; v_l1++) {
                v_page_t v_page = PNUM_TO_PAGE(old_l2, v_l1);
                free_vpage(l2_pt, v_page);
            }

            free_l1_pt(l2_pt, old_l2);

            for (v_page_l1_t v_l1 = new_l1; v_l1 < NUM_L1PT_ENTRIES; v_l1++) {
                v_page_t v_page = PNUM_TO_PAGE(new_l2, v_l1);
                free_vpage(l2_pt, v_page);
            }
        }
        *retval0 = old_heap_end;
        as->brk = new_heap_end;
    }
    // Check to see if we have enough memory
    if (new_heap_end > old_heap_end){
        // TODO: Increase code reuse between this and the above case
        v_page_l2_t old_l2 = L2_PNUM(old_heap_end);
        v_page_l1_t old_l1 = L1_PNUM(old_heap_end);
        v_page_l2_t new_l2 = L2_PNUM(new_heap_end);
        v_page_l1_t new_l1 = L1_PNUM(new_heap_end);

        int num_used = 0;
        if (old_l2 == new_l2){
            num_used += (new_l1 - old_l1);
        } else {
            num_used += (new_l2 - old_l2 - 1) * NUM_L1PT_ENTRIES;
            num_used += NUM_L1PT_ENTRIES - old_l1;
            num_used += new_l1;
        }
        if (num_used > NUM_PPAGES){
            *retval0 = -1;
            lock_release(as->as_lock);
            spinlock_release(&global);
            return ENOMEM;
        }

        //XXX: Am I dealing with the first old_l2 / old_l1 correctly? Heap_end is not allocated Im assuming...?
        vaddr_t cur_vaddr = old_heap_end;
        if (old_l2 == new_l2){
            for (v_page_l1_t v_l1 = old_l1; v_l1 < new_l1; v_l1++) {
                add_emptypage(v_l1, old_l2, cur_vaddr, as);
                cur_vaddr += PAGE_SIZE;
            }
        }
        else {
            // Fill up old_l2's l1 table
            for (v_page_l1_t v_l1 = old_l1; v_l1 < NUM_L1PT_ENTRIES; v_l1++) {
                add_emptypage(v_l1, old_l2, cur_vaddr, as);
                cur_vaddr += PAGE_SIZE;
            }
            // Check to see if we need to completely fill any l2 tables with blank entries
            if (new_l2 - old_l2 > 1){
                for (v_page_l2_t v_l2 = old_l2 + 1 ; v_l2 < new_l2; v_l2++) {
                    add_l1table(v_l2, as);
                    for (v_page_l1_t v_l1 = old_l1; v_l1 < NUM_L1PT_ENTRIES; v_l1++) {
                        add_emptypage(v_l1, v_l2, cur_vaddr, as);
                    }
                }
            }
            // Fill the last l2 table up to new_l1
            add_l1table(new_l2, as);
            for (v_page_l1_t v_l1 = 0; v_l1 < new_l1; v_l1++) {
                add_emptypage(v_l1, new_l2, cur_vaddr, as);
            }
        }
    }
    *retval0 = old_heap_end;
    as->brk = new_heap_end;

    lock_release(as->as_lock);
    spinlock_release(&global);

    return 0;
}
