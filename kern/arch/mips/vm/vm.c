#include <types.h>
#include <kern/errno.h>
#include <spinlock.h>
#include <vm.h>
#include <current.h>
#include <signal.h>
#include <spl.h>
#include <proc.h>
#include <addrspace.h>
#include <lib.h>
#include <mips/tlb.h>
#include <vfs.h>
#include <kern/fcntl.h>
#include <vnode.h>


struct coremap *cm;
struct spinlock cm_spinlock = SPINLOCK_INITIALIZER;

// TODO: probs need to change for tlb shootdown
// struct spinlock tlb_spinlock = SPINLOCK_INITIALIZER;

static p_page_t first_alloc_page; /* First physical page that can be dynamically allocated */ 
static p_page_t last_page; /* One page past the last free physical page in RAM */

// TODO: maybe this couting policy is ugly. If it works, clean it up by using proper types, etc.
struct vnode *swap_disk;
const char *swap_dir = "lhd0raw:";

static struct spinlock counter_spinlock = SPINLOCK_INITIALIZER;
static volatile int swap_out_counter = 0;

#define SWAP_OUT_COUNT    4
#define NUM_FREE_PPAGES   8

/////////////////////////////////////////////////////////////////////////////////////////

static
void
kalloc_ppage(p_page_t p_page)
{
    v_page_t v_page = PPAGE_TO_KVPAGE(p_page);
    cm->cm_entries[p_page] = 0 | PP_USED | v_page;
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
    KASSERT(first_alloc_page <= p_page && p_page < last_page);

    cm->cm_entries[p_page] = 0; 
}

size_t
cm_getref(p_page_t p_page)
{
    KASSERT(first_alloc_page <= p_page && p_page < last_page); 

    return GET_REF(cm->cm_entries[p_page]);
}

void
cm_incref(p_page_t p_page)
{
    KASSERT(first_alloc_page <= p_page && p_page < last_page);

    size_t curref = GET_REF(cm->cm_entries[p_page]);
    curref++;
    SET_REF(cm->cm_entries[p_page], curref);
}

void
cm_decref(p_page_t p_page)
{
    KASSERT(first_alloc_page <= p_page && p_page < last_page);

    size_t curref = GET_REF(cm->cm_entries[p_page]);
    curref--;
    SET_REF(cm->cm_entries[p_page], curref);
}

// TODO: too many magic numbers?
// static
// void 
// set_pid8(p_page_t p_page, pid_t pid, int pos)
// {
//     KASSERT(0 <= pos && pos < 4);
//     KASSERT(first_alloc_page <= p_page && p_page < last_page);

//     pid_t pid_to_add = 0;

//     if (pid < 256) {
//         pid_to_add = pid; 
//     }

//     cm->pids8_entries[p_page] = 0 | (pid_to_add << pos*8);
// }

////////////////////////////////////////////////////////////////////////////////////

void 
vm_bootstrap()
{
    paddr_t cm_paddr = ram_stealmem(2*COREMAP_PAGES);
    KASSERT(cm_paddr != 0);

    cm = (struct coremap *) PADDR_TO_KVADDR(cm_paddr);

    KASSERT(ram_stealmem(0) % PAGE_SIZE == 0);
    first_alloc_page = ADDR_TO_PAGE(ram_stealmem(0));
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
    vfs_open((char *) swap_dir, O_RDWR, 0, &swap_disk);
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
        cm->cm_entries[curr] = 0x00000000;
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

}



void 
vm_tlbshootdown(const struct tlbshootdown *tlbsd)
{
    (void) tlbsd;
}

/////////////////////////////////////////////////////////////////////////////////////////////

void
swap_out()
{

}

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
    spinlock_acquire(&counter_spinlock);
    
    if (swap_out_counter == SWAP_OUT_COUNT - 1) {
        swap_out_counter = 0; 
        spinlock_release(&counter_spinlock);
        swap_out();
    } else {
        swap_out_counter++; 
        spinlock_release(&counter_spinlock);
    }

    struct addrspace *as = curproc->p_addrspace;

    // TODO: do a proper address check (make sure kernel addresses aren't called)
    // if (as->brk <= faultaddress && faultaddress < as->stack_top) {
    //     return SIGSEGV;
    // }

    int result;

    vaddr_t fault_page = faultaddress & PAGE_FRAME;
    v_page_l2_t v_l2 = L2_PNUM(fault_page);
    v_page_l1_t v_l1 = L1_PNUM(fault_page);

    lock_acquire(as->as_lock);

    struct l2_pt *l2_pt = as->l2_pt;
    l2_entry_t l2_entry = l2_pt->l2_entries[v_l2]; 
    
    struct l1_pt *l1_pt; 

    // Get the l1 page table.
    if (l2_entry & ENTRY_VALID) {
        if (faulttype == VM_FAULT_WRITE && !(l2_entry & ENTRY_WRITABLE)) {
            v_page_t v_page = l2_entry & PAGE_MASK;
            p_page_t p_page = KVPAGE_TO_PPAGE(v_page);

            spinlock_acquire(&cm_spinlock);

            if (cm_getref(p_page) > 1) {
                result = l1_create(&l1_pt); 
                if (result) {
                    spinlock_release(&cm_spinlock);
                    return result;
                }

                struct l1_pt *l1_pt_orig = (struct l1_pt *) PAGE_TO_ADDR(v_page);
                for (v_page_l1_t l1_val = 0; l1_val < NUM_L1PT_ENTRIES; l1_val++) {
                    l1_pt->l1_entries[l1_val] = l1_pt_orig->l1_entries[l1_val];
                }
                
                l2_pt->l2_entries[v_l2] = 0 
                                        | ENTRY_VALID 
                                        | ENTRY_READABLE 
                                        | ENTRY_WRITABLE 
                                        | ADDR_TO_PAGE((vaddr_t) l1_pt);

                cm_incref(ADDR_TO_PAGE(KVADDR_TO_PADDR((vaddr_t) l1_pt)));
                cm_decref(p_page);
            } else {
                l2_pt->l2_entries[v_l2] = l2_pt->l2_entries[v_l2] | ENTRY_WRITABLE; 
                l1_pt = (struct l1_pt *) PAGE_TO_ADDR(v_page);   
            }

            spinlock_release(&cm_spinlock);
        } else {
            v_page_t v_page = l2_entry & PAGE_MASK;
            l1_pt = (struct l1_pt *) PAGE_TO_ADDR(v_page);
        }
    } else {
        result = l1_create(&l1_pt);
        if (result) {
            return result;
        }

        spinlock_acquire(&cm_spinlock);

        v_page_t v_page = ADDR_TO_PAGE((vaddr_t) l1_pt); 
        l2_pt->l2_entries[v_l2] = 0 
                                | ENTRY_VALID 
                                | ENTRY_READABLE
                                | ENTRY_WRITABLE
                                | v_page; 

        cm_incref(KVPAGE_TO_PPAGE(v_page)); 

        spinlock_release(&cm_spinlock);      
    }

    l1_entry_t l1_entry = l1_pt->l1_entries[v_l1];
    p_page_t p_page; 

    // Get the faulting address' physical address
    if (l1_entry & ENTRY_VALID) {
        if (faulttype == VM_FAULT_WRITE && !(l1_entry & ENTRY_WRITABLE)) {
            p_page_t old_page = l1_entry & PAGE_MASK;

            spinlock_acquire(&cm_spinlock);

            if (cm_getref(old_page) > 1) {
                // TODO: reduce code repetition with kmalloc
                p_page = first_alloc_page;     
                result = find_free(1, &p_page); 
                if (result) {
                    spinlock_release(&cm_spinlock);
                    return result;
                }
                
                cm->cm_entries[p_page] = 0 
                                       | PP_USED
                                       | ADDR_TO_PAGE(fault_page);
                                            
                cm_incref(p_page);
                cm_decref(old_page);

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
                p_page = l1_pt->l1_entries[v_l1] & PAGE_MASK;
            }

            spinlock_release(&cm_spinlock);
        } else {
            p_page = l1_pt->l1_entries[v_l1] & PAGE_MASK;
        }
    } else {
        spinlock_acquire(&cm_spinlock);

        p_page = first_alloc_page;     
        result = find_free(1, &p_page); 
        if (result) {
            spinlock_release(&cm_spinlock);
            return result;
        }
        
        cm->cm_entries[p_page] = 0 
                                | PP_USED
                                | ADDR_TO_PAGE(fault_page);

        l1_pt->l1_entries[v_l1] = 0 
                                | ENTRY_VALID 
                                | ENTRY_READABLE
                                | ENTRY_WRITABLE
                                | p_page;
        cm_incref(p_page);

        spinlock_release(&cm_spinlock);
    }

    uint32_t entryhi; 
    uint32_t entrylo; 
    l1_entry_t new_l1_entry = l1_pt->l1_entries[v_l1];

    p_page_t p_page_high = p_page << 12;
    pid_t pid = curproc->pid;
    
    entryhi = 0 | fault_page | pid << 6; 

    if (new_l1_entry & ENTRY_WRITABLE) {
        entrylo = 0 | p_page_high | TLBLO_VALID | TLBLO_DIRTY;
    } else {
        entrylo = 0 | p_page_high | TLBLO_VALID;        
    }

    int spl = splhigh();

    tlb_write(entryhi, entrylo, V_TO_INDEX(fault_page));

    splx(spl);

    lock_release(as->as_lock);

    return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////////////

void
free_vpage(struct l2_pt *l2_pt, v_page_t v_page)
{
    KASSERT(l2_pt != NULL); 
    
    v_page_l2_t v_l2 = L2_PNUM(PAGE_TO_ADDR(v_page));
    v_page_l1_t v_l1 = L1_PNUM(PAGE_TO_ADDR(v_page));

    v_page_t l1_pt_page = l2_pt->l2_entries[v_l2] & PAGE_MASK;
    struct l1_pt *l1_pt = (struct l1_pt*) PAGE_TO_ADDR(l1_pt_page);

    l1_entry_t l1_entry = l1_pt->l1_entries[v_l1];

    if (l1_entry & ENTRY_VALID) {
        p_page_t p_page = l1_entry & PAGE_MASK;

        if (cm_getref(p_page) > 1) {
            cm_decref(p_page);
        } else {
            free_ppage(p_page);
        }
    }
}

void
free_l1_pt(struct l2_pt *l2_pt, v_page_l2_t v_l2)
{
    v_page_t v_page = l2_pt->l2_entries[v_l2] & PAGE_MASK;
    p_page_t p_page = KVPAGE_TO_PPAGE(v_page);

    if (cm_getref(p_page) > 1) {
        cm_decref(p_page);
    } else {
        struct l1_pt *l1_pt = (struct l1_pt *) PAGE_TO_ADDR(v_page);
        kfree(l1_pt);
    }

    l2_pt->l2_entries[v_l2] = 0; 
}

/*
sbrk is almost done. all there is left to do is figure out how to set the inital break
(which is probably done while loading the elf, maybe in as define region), and to check 
the fault address values in vm_fault to make sure they are valid.
*/
int
sys_sbrk(size_t amount, int32_t *retval0)
{
    if (amount % PAGE_SIZE) {
        return EINVAL;
    }

    struct addrspace *as = curproc->p_addrspace;

    vaddr_t old_heap_end = as->brk;
    vaddr_t new_heap_end = old_heap_end + amount;
    if (new_heap_end < as->heap_base) {
        return EINVAL;
    }

    if (new_heap_end > as->stack_top) {
        return ENOMEM;
    }

    // Free physical pages of deallocated virtual pages
    if (new_heap_end < old_heap_end) {
        v_page_l2_t old_l2 = L2_PNUM(old_heap_end); 
        v_page_l1_t old_l1 = L1_PNUM(old_heap_end);
        v_page_l2_t new_l2 = L2_PNUM(new_heap_end);
        v_page_l1_t new_l1 = L1_PNUM(new_heap_end);
        struct l2_pt *l2_pt = as->l2_pt;

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
    }

    *retval0 = old_heap_end; 
    as->brk = new_heap_end;
    return 0;
}