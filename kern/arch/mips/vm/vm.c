#include <types.h>
#include <kern/errno.h>
#include <spinlock.h>
#include <vm.h>
#include <current.h>
#include <signal.h>
#include <spl.h>
#include <proc.h>
#include <thread.h>
#include <wchan.h>
#include <addrspace.h>
#include <lib.h>
#include <mips/tlb.h>
#include <vfs.h>
#include <kern/fcntl.h>
#include <uio.h>
#include <vnode.h>
#include <cpu.h>

struct lock *global_lock;
struct cv *global_cv;

struct coremap *cm;
struct spinlock cm_spinlock = SPINLOCK_INITIALIZER;
volatile size_t cm_counter = 0;

/* Variable indicating paging bounds. Shared with msyscall.c */
p_page_t first_alloc_page; /* First physical page that can be dynamically allocated */
p_page_t last_page; /* One page past the last free physical page in RAM */
p_page_t first_page_swap; /* First physical page number that is allocated to swap */
p_page_t last_page_swap; /* One page past the last free physical page SWAP */

static struct vnode *swap_disk;
static const char swap_dir[] = "lhd0raw:";

static volatile p_page_t swapclock;

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

    global_lock = lock_create("global_lock");
    if (global_lock == NULL) {
        panic("couldn't initialize global lock\n");
    }

    global_cv = cv_create("global_cv");
    if (global_cv == NULL) {
        panic("couldn't initialize global cv\n");
    }
}

void
swap_bootstrap()
{
    int ret;
    char dir[sizeof(swap_dir)];
    strcpy(dir, swap_dir);

    ret = vfs_open(dir, O_RDWR, 0, &swap_disk);
    if (ret) {
        panic("swap disk wasn't able to open\n");
    }

    first_page_swap = last_page;
    last_page_swap = 0x400;

    // remove this when we know the truth
    KASSERT(kproc != NULL);

    if (SWAP_ON)
    thread_fork("Paging Daemon", kproc, paging_daemon, NULL, 0);
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

/*
Swapable pages are user data pages and l1 page tables.
*/
static
bool
entry_swappable(p_page_t p_page)
{
    if (!p_page_used(p_page)) {
        return false;
    }

    size_t ref = cm_getref(p_page);
    if (ref > NUM_CM_PIDS) {
        return false;
    }

    if (ref == 0) {
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
        if (p_page == TLBADDR_TO_PAGE(entrylo & TLBLO_PPAGE)) {
            splx(spl);
            return true;
        }
    }

    splx(spl);

    return false;
}

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

    return SWAPNOMEM;
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
    if (iov == NULL) {
        return NULL;
    }

    struct uio *u = kmalloc(sizeof(struct uio));
    if (u == NULL) {
        kfree(iov);
        return NULL;
    }

    void *kbase = (void *) PAGE_TO_ADDR(PPAGE_TO_KVPAGE(swapclock));
    uio_kinit(iov, u, kbase, PAGE_SIZE, swap_offset(p_page), UIO_WRITE);

    return u;
}

static
struct uio *
swap_load_uio(p_page_t p_page, p_page_t old_p_page)
{
    KASSERT(in_ram(p_page));

    struct iovec *iov = kmalloc(sizeof(struct iovec));
    if (iov == NULL) {
        return NULL;
    }

    struct uio *u = kmalloc(sizeof(struct uio));
    if (u == NULL) {
        kfree(iov);
        return NULL;
    }

    void *kbase = (void *) PAGE_TO_ADDR(PPAGE_TO_KVPAGE(p_page));
    uio_kinit(iov, u, kbase, PAGE_SIZE, swap_offset(old_p_page), UIO_READ);

    return u;
}


static
void
swap_uio_cleanup(struct uio *u)
{
    KASSERT(u != NULL);
    KASSERT(u->uio_iov != NULL);

    kfree(u->uio_iov);
    kfree(u);
}

/*
Updates the location of a page in the page tables of other processes.
*/
static
int
update_pt_entries(p_page_t swap_to_page, p_page_t old_page)
{
    KASSERT(spinlock_do_i_hold(&cm_spinlock));

    size_t refs = cm_getref(swap_to_page);
    v_page_t v_page = cm->cm_entries[swap_to_page] & VP_MASK;
    bool is_l1 = v_page >= 0x00080000;
    int result;

    for (uint32_t pos = 0; pos < refs; pos++) {
        pid_t pid = get_pid8(swap_to_page, pos);
        KASSERT(pid != 0);

        spinlock_release(&cm_spinlock);

        struct proc *proc = get_pid(pid);
        KASSERT(proc != NULL);

        spinlock_acquire(&cm_spinlock);

        struct addrspace *as = proc->p_addrspace;
        KASSERT(as != NULL);

        struct l2_pt *l2_pt = as->l2_pt;

        if (is_l1) {
            for (v_page_l2_t v_l2 = 0; v_l2 < NUM_L2PT_ENTRIES; v_l2++) {
                p_page_t cur_p_page = l2_pt->l2_entries[v_l2] & PAGE_MASK;
                if (cur_p_page == old_page) {
                    l2_pt->l2_entries[v_l2] = l2_pt->l2_entries[v_l2] & (~PAGE_MASK);
                    l2_pt->l2_entries[v_l2] = l2_pt->l2_entries[v_l2] | swap_to_page;
                    break;
                }
            }
        } else {
            v_page_l2_t v_l2 = L2_PNUM(PAGE_TO_ADDR(v_page));
            v_page_l1_t v_l1 = L1_PNUM(PAGE_TO_ADDR(v_page));

            l2_entry_t l2_entry = l2_pt->l2_entries[v_l2];

            KASSERT(l2_entry & ENTRY_VALID);

            p_page_t p_page = l2_entry & PAGE_MASK;

            if (in_swap(p_page)) {
                result = swap_in_data(&p_page);
                if (result) {
                    return result;
                }
            }

            struct l1_pt *l1_pt = (struct l1_pt *) PADDR_TO_KVADDR(PAGE_TO_ADDR(p_page));

            l1_pt->l1_entries[v_l1] = l1_pt->l1_entries[v_l1] & (~PAGE_MASK);
            l1_pt->l1_entries[v_l1] = l1_pt->l1_entries[v_l1] | swap_to_page;
        }
    }
    return 0;
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

int
swap_out()
{
    size_t free_pages = last_page - cm_counter;

    if (free_pages >= NUM_FREE_PPAGES) {
        return ENOUGHFREE;
    }

    p_page_t first_clock = swapclock;
    int cycles = 0;
    int result;

    spinlock_acquire(&cm_spinlock);

    /* We iterate for 2 cycles, since after the first cycle, the reference bits are cleared */
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
            if (u == NULL) {
                spinlock_release(&cm_spinlock);
                return ENOMEM;
            }

            spinlock_release(&cm_spinlock);

            VOP_WRITE(swap_disk, u);

            spinlock_acquire(&cm_spinlock);
            update_pt_entries(swap_to_page, swapclock);
            free_ppage(swapclock);
            spinlock_release(&cm_spinlock);

            swap_uio_cleanup(u);
            swapclock_tick();
            return 0;
        }

        cm->cm_entries[swapclock] = cm->cm_entries[swapclock] & (~REF_BIT);

        swapclock_tick();
        if (swapclock == first_clock) {
            cycles++;
        }
    }

    spinlock_release(&cm_spinlock);

    return NOSWAPPABLE;
}

// add any error codes
int
swap_in(p_page_t p_page, p_page_t old_p_page)
{
    KASSERT(cm->cm_entries[p_page] & PP_USED);
    KASSERT(cm->cm_entries[old_p_page] & PP_USED);
    KASSERT(in_ram(p_page));
    KASSERT(in_swap(old_p_page));
    KASSERT(spinlock_do_i_hold(&cm_spinlock));

    struct uio *u = swap_load_uio(p_page, old_p_page);
    if (u == NULL) {
        return ENOMEM;
    }

    spinlock_release(&cm_spinlock);

    VOP_READ(swap_disk, u);

    spinlock_acquire(&cm_spinlock);

    swap_uio_cleanup(u);
    return 0;
}

int
swap_in_data(p_page_t *p_page_ret)
{
    p_page_t p_page = *p_page_ret;
    KASSERT(in_swap(p_page));
    KASSERT(entry_swappable(p_page));
    KASSERT(spinlock_do_i_hold(&cm_spinlock));

    int result;

    p_page_t new_page = first_alloc_page;
    result = find_free(1, &new_page);
    if (result) {
        return result;
    }

    cm_counter++;
    cm->cm_entries[new_page] = cm->cm_entries[p_page];
    cm->pids8_entries[new_page] = cm->pids8_entries[p_page];
    swap_in(new_page, p_page);
    update_pt_entries(new_page, p_page);
    free_ppage_swap(p_page);

    *p_page_ret = new_page;

    return 0;
}

bool
enough_free()
{
    return last_page - cm_counter >= MIN_FREE_PAGES;
}

// daemon yield after a write, right?
void
paging_daemon(void *data1, unsigned long data2)
{
    (void) data1;
    (void) data2;
    int result;

    while(true) {
        lock_acquire(global_lock);

        for (int count = 0; count < DAEMON_EVICT_NUM; count++) {
            result = swap_out();
            if (result) {
                switch(result) {
                    case ENOUGHFREE:
                    goto done;
                    break;

                    case NOSWAPPABLE:
                    goto done;
                    break;

                    case SWAPNOMEM:
                    goto done;
                    break;
                }
            }
            break;
        }

     done:
        cv_broadcast(global_cv, global_lock);
        lock_release(global_lock);
        thread_yield();
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

/*
Gets an l1 page table from the l2 page table entry. The l2_pt must belong to curproc,
and the entry must be valid. This functions swaps in the l1 page table if it is in
swap. If writable is true, then we copy the page table if it has a > 1 ref count.
*/
int
get_l1_pt(struct l2_pt *l2_pt, v_page_l2_t v_l2, struct l1_pt **l1_pt_ret, bool writable)
{
    KASSERT(l2_pt != NULL);
    KASSERT(l2_pt->l2_entries[v_l2] & ENTRY_VALID);
    struct l1_pt *l1_pt;
    int result;

    p_page_t p_page = l2_pt->l2_entries[v_l2] & PAGE_MASK;

    if (in_swap(p_page)) {
        spinlock_acquire(&cm_spinlock);

        result = swap_in_data(&p_page);

        spinlock_release(&cm_spinlock);
        if (result) {
            return result;
        }
    }

    if (cm_getref(p_page) > 1 && writable) {
        result = l1_create(&l1_pt);
        if (result) {
            return result;
        }

        p_page_t new_p_page = ADDR_TO_PAGE(KVADDR_TO_PADDR((vaddr_t) l1_pt));

        struct l1_pt *l1_pt_orig = (struct l1_pt *) PADDR_TO_KVADDR(PAGE_TO_ADDR(p_page));
        for (v_page_l1_t l1_val = 0; l1_val < NUM_L1PT_ENTRIES; l1_val++) {
            l1_pt->l1_entries[l1_val] = l1_pt_orig->l1_entries[l1_val];
        }

        l2_pt->l2_entries[v_l2] = 0
                                | ENTRY_VALID
                                | ENTRY_READABLE
                                | ENTRY_WRITABLE
                                | new_p_page;

        spinlock_acquire(&cm_spinlock);

        cm_incref(new_p_page);
        add_pid8(new_p_page, curproc->pid);

        cm_decref(p_page);
        rem_pid8(p_page, curproc->pid);

        spinlock_release(&cm_spinlock);
    } else {
        l1_pt = (struct l1_pt*) PADDR_TO_KVADDR(PAGE_TO_ADDR(p_page));
    }

    *l1_pt_ret = l1_pt;

    return 0;
}

/*
Assigns the l1 entry a new, empty physical page. The l1 page table must be writable.
*/
int
l1_alloc_page(struct l1_pt *l1_pt, v_page_l1_t v_l1, v_page_t v_page, p_page_t *p_page_ret)
{
    KASSERT(!(l1_pt->l1_entries[v_l1] & ENTRY_VALID));
    KASSERT(v_l1 == L1_PNUM(PAGE_TO_ADDR(v_page)));
    int result;

    spinlock_acquire(&cm_spinlock);

    p_page_t p_page = first_alloc_page;
    result = find_free(1, &p_page);
    if (result) {
        spinlock_release(&cm_spinlock);
        return result;
    }

    cm_counter++;
    cm->cm_entries[p_page] = 0
                            | PP_USED
                            | v_page;

    l1_pt->l1_entries[v_l1] = 0
                            | ENTRY_VALID
                            | ENTRY_READABLE
                            | ENTRY_WRITABLE
                            | p_page;
    cm_incref(p_page);
    add_pid8(p_page, curproc->pid);

    spinlock_release(&cm_spinlock);

    if (p_page_ret != NULL) {
        *p_page_ret = p_page;
    }

    return 0;
}

/*
Assigns the l2 entry a new, empty l1 page table.
*/
int
add_l1_pt(struct l2_pt *l2_pt, v_page_l2_t v_l2, struct l1_pt **l1_pt_ret)
{
    KASSERT(!(l2_pt->l2_entries[v_l2] & ENTRY_VALID));
    struct l1_pt *l1_pt;
    int result;

    result = l1_create(&l1_pt);
    if (result) {
        return result;
    }

    p_page_t new_p_page = ADDR_TO_PAGE(KVADDR_TO_PADDR((vaddr_t) l1_pt));
    l2_pt->l2_entries[v_l2] = 0
                            | ENTRY_VALID
                            | ENTRY_READABLE
                            | ENTRY_WRITABLE
                            | new_p_page;

    spinlock_acquire(&cm_spinlock);

    cm_incref(new_p_page);
    add_pid8(new_p_page, curproc->pid);

    spinlock_release(&cm_spinlock);

    if (l1_pt_ret != NULL) {
        *l1_pt_ret = l1_pt;
    }

    return 0;
}

/*
Creates a new entry for l1_pt and v_l1, and copies the contents of old_page over.
Note that the cm_spinlock must be acquired before calling.
*/
int
copy_user_data(struct l1_pt *l1_pt, v_page_l1_t v_l1, p_page_t old_page,
                                    v_page_t v_page, p_page_t *p_page_ret)
{
    KASSERT(v_l1 == L1_PNUM(PAGE_TO_ADDR(v_page)));
    KASSERT(spinlock_do_i_hold(&cm_spinlock));
    int result;

    p_page_t p_page = first_alloc_page;
    result = find_free(1, &p_page);
    if (result) {
        return result;
    }

    cm_counter++;
    cm->cm_entries[p_page] = 0
                            | PP_USED
                            | v_page;
    cm_incref(p_page);
    add_pid8(p_page, curproc->pid);

    cm_decref(old_page);
    rem_pid8(old_page, curproc->pid);

    const void *src = (const void *) PADDR_TO_KVADDR(PAGE_TO_ADDR(old_page));
    void *dst = (void *) PADDR_TO_KVADDR(PAGE_TO_ADDR(p_page));
    memmove(dst, src, (size_t) PAGE_SIZE);

    l1_pt->l1_entries[v_l1] = 0
                            | ENTRY_VALID
                            | ENTRY_READABLE
                            | ENTRY_WRITABLE
                            | p_page;

    if (p_page_ret != NULL) {
        *p_page_ret = p_page;
    }

    return 0;
}

/*
Removes the reference of the pid to the p_page, and frees it if there are no more references.
*/
void
release_ppage(p_page_t p_page, pid_t pid)
{
    spinlock_acquire(&cm_spinlock);

    if (cm_getref(p_page) > 1) {
        cm_decref(p_page);
        rem_pid8(p_page, pid);
    } else {
        if (in_ram(p_page)) {
            free_ppage(p_page);
        } else {
            free_ppage_swap(p_page);
        }
    }

    spinlock_release(&cm_spinlock);
}


int
vm_fault(int faulttype, vaddr_t faultaddress)
{
    lock_acquire(global_lock);

    if (SWAP_ON){
        swap_out();
    }

    while (!enough_free()) {
        cv_wait(global_cv, global_lock);
    }

    struct addrspace *as = curproc->p_addrspace;
    pid_t pid = curproc->pid;

    if (as->brk <= faultaddress && faultaddress < as->stack_top) {
        lock_release(global_lock);
        return SIGSEGV;
    }

    int result;

    vaddr_t fault_page = faultaddress & PAGE_FRAME;
    v_page_l2_t v_l2 = L2_PNUM(fault_page);
    v_page_l1_t v_l1 = L1_PNUM(fault_page);

    struct l2_pt *l2_pt = as->l2_pt;
    l2_entry_t l2_entry = l2_pt->l2_entries[v_l2];

    struct l1_pt *l1_pt;

    /* Get the l1 page table. */
    if (l2_entry & ENTRY_VALID) {
        /* Checks if a process must be able to modify the l1 page table. */
        bool writable = (faulttype == VM_FAULT_READONLY && !(l2_entry & ENTRY_WRITABLE));
        result = get_l1_pt(l2_pt, v_l2, &l1_pt, writable);
        if (result) {
            lock_release(global_lock);
            return result;
        }

    } else {
        result = add_l1_pt(l2_pt, v_l2, &l1_pt);
        if (result) {
            lock_release(global_lock);
            return result;
        }
    }

    p_page_t l1_p_page = ADDR_TO_PAGE(KVADDR_TO_PADDR((vaddr_t) l1_pt));
    cm->cm_entries[l1_p_page] = cm->cm_entries[l1_p_page] | REF_BIT;

    l1_entry_t l1_entry = l1_pt->l1_entries[v_l1];
    p_page_t p_page;

    /* Get the faulting address' physical address */
    if (l1_entry & ENTRY_VALID) {
        p_page_t old_page = l1_entry & PAGE_MASK;
        KASSERT(in_all_memory(old_page));
        KASSERT((cm->cm_entries[old_page] & PAGE_MASK) == ADDR_TO_PAGE(fault_page));

        spinlock_acquire(&cm_spinlock);

        if (faulttype == VM_FAULT_READONLY && !(l1_entry & ENTRY_WRITABLE)) {
            KASSERT(in_ram(old_page));

            if (cm_getref(old_page) > 1) {
                result = copy_user_data(l1_pt, v_l1, old_page, ADDR_TO_PAGE(fault_page), &p_page);
                if (result) {
                    spinlock_release(&cm_spinlock);
                    lock_release(global_lock);
                    return result;
                }
            } else {
                l1_pt->l1_entries[v_l1] = l1_pt->l1_entries[v_l1] | ENTRY_WRITABLE;
                p_page = old_page;
            }

        } else {
            if (in_swap(old_page)) {
                result = swap_in_data(&old_page);
                if (result) {
                    spinlock_release(&cm_spinlock);
                    lock_release(global_lock);
                }
            }

            p_page = old_page;
        }

        spinlock_release(&cm_spinlock);

    } else {
        result = l1_alloc_page(l1_pt, v_l1, ADDR_TO_PAGE(fault_page), &p_page);
        if (result) {
            lock_release(global_lock);
            return result;
        }
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

    lock_release(global_lock);
    return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////////////

void
free_vpage(struct l2_pt *l2_pt, v_page_l2_t v_l2, v_page_l1_t v_l1)
{
    KASSERT(l2_pt != NULL);
    KASSERT(l2_pt->l2_entries[v_l2] & ENTRY_VALID);
    struct l1_pt *l1_pt;
    int result;

    result = get_l1_pt(l2_pt, v_l2, &l1_pt, true);
    if (result) {
        return;
    }

    l1_entry_t l1_entry = l1_pt->l1_entries[v_l1];

    if (l1_entry & ENTRY_VALID) {
        p_page_t p_page = l1_entry & PAGE_MASK;
        release_ppage(p_page, curproc->pid);
    }

    l1_pt->l1_entries[v_l1] = 0;
}

void
free_l1_pt(struct l2_pt *l2_pt, v_page_l2_t v_l2)
{
    if (l2_pt->l2_entries[v_l2] & ENTRY_VALID) {
        p_page_t p_page = l2_pt->l2_entries[v_l2] & PAGE_MASK;
        release_ppage(p_page, curproc->pid);

        l2_pt->l2_entries[v_l2] = 0; // maybe make this more explicit
    }
}

/*
Adds a physical apge to the specified l1 page table
*/
int
add_ppage(struct l2_pt *l2_pt, v_page_l2_t v_l2, v_page_l1_t v_l1)
{
    KASSERT(l2_pt != NULL);
    KASSERT(l2_pt->l2_entries[v_l2] & ENTRY_VALID);
    struct l1_pt *l1_pt;
    int result;

    result = get_l1_pt(l2_pt, v_l2, &l1_pt, true);
    if (result) {
        return result;
    }

    result = l1_alloc_page(l1_pt, v_l1, PNUM_TO_PAGE(v_l2, v_l1), NULL);
    if (result) {
        return result;
    }

    return 0;
}
