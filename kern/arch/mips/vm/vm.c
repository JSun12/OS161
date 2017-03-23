#include <types.h>
#include <spinlock.h>
#include <vm.h>
#include <current.h>
#include <proc.h>
#include <addrspace.h>
#include <lib.h>
#include <mips/tlb.h>
#include <kern/errno.h>


// TODO: synchronize everything

static struct coremap *cm;
static p_page_t first_alloc_page; /* First physical page that can be dynamically allocated */ 
static p_page_t last_page; /* One page past the last free physical page in RAM */

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
cm_incref(p_page_t p_page)
{
    size_t curref = GET_REF(cm->cm_entries[p_page]);
    curref++;
    SET_REF(cm->cm_entries[p_page], curref);
}

void
cm_decref(p_page_t p_page)
{
    size_t curref = GET_REF(cm->cm_entries[p_page]);
    curref--;
    SET_REF(cm->cm_entries[p_page], curref);
}

void
copy_to_write_set(p_page_t p_page) 
{
    cm->cm_entries[p_page] = cm->cm_entries[p_page] | COPY_TO_WRITE;
}

void 
vm_bootstrap()
{
    paddr_t cm_paddr = ram_stealmem(COREMAP_PAGES);
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

/* Allocate/free some kernel-space virtual pages */
vaddr_t
alloc_kpages(size_t npages)
{
    int result; 

    // Find contiguous free physical pages
    p_page_t start = first_alloc_page;     
    result = find_free(npages, &start); 
    if (result) {
        return 0;
    }

    // Set the corresponding coremap entries
    for (p_page_t p_page = start; p_page < start + npages; p_page++) {
        kalloc_ppage(p_page);
    }

    cm->cm_entries[start + npages - 1] = cm->cm_entries[start + npages - 1] | KMALLOC_END;
    return PADDR_TO_KVADDR(PAGE_TO_ADDR(start));
}



void 
free_kpages(vaddr_t addr)
{
    p_page_t curr = ADDR_TO_PAGE(KVADDR_TO_PADDR(addr));
    bool end;
    while (p_page_used(curr)) {
        end = cm->cm_entries[curr] & KMALLOC_END;
        cm->cm_entries[curr] = 0x00000000;
        if (end) {
            return;
        }
        curr++;
    }
}



void 
vm_tlbshootdown_all()
{

}



void 
vm_tlbshootdown(const struct tlbshootdown *tlbsd)
{
    (void) tlbsd;
}


// TODO: make sure users can't access kernel addresses
// make sure to set tlb dirty bit as clear to enforce read only

/*
currently, readable and writable valid PTEs, user faultaddress. Now, when 
read only PTEs are there, if faulttype is read, then same as before, but if 
faulttype is write, we copy physical page content to another physical page, 
and possibly the l1 page table.

if not specified (that is, valid), assume readable and writable.
*/
int 
vm_fault(int faulttype, vaddr_t faultaddress)
{
    int result;
    
    vaddr_t fault_page = faultaddress & PAGE_FRAME;
    v_page_l2_t v_l2 = L2_PNUM(fault_page);
    v_page_l1_t v_l1 = L1_PNUM(fault_page);

    struct l2_pt *l2_pt = &(curproc->p_addrspace)->l2_pt;
    l2_entry_t l2_entry = l2_pt->l2_entries[v_l2]; 
    
    struct l1_pt *l1_pt; 

    // Get the l1 page table.
    if (l2_entry & ENTRY_VALID) {
        if (faulttype == VM_FAULT_WRITE && !(l2_entry & ENTRY_WRITABLE)) {
            v_page_t v_page = l2_entry & PAGE_MASK;
            p_page_t p_page = KVPAGE_TO_PPAGE(v_page);
            cm_entry_t cm_entry = cm->cm_entries[p_page];
            // kprintf("%d\n", curproc->pid);

            if (cm_entry & COPY_TO_WRITE) {
                result = l1_create(&l1_pt); 
                if (result) {
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

                cm->cm_entries[p_page] = cm->cm_entries[p_page] & (~COPY_TO_WRITE);
            // while(1);
            } else {
                l2_pt->l2_entries[v_l2] = l2_pt->l2_entries[v_l2] | ENTRY_WRITABLE; 
                v_page_t v_page = l2_entry & PAGE_MASK;
                l1_pt = (struct l1_pt *) PAGE_TO_ADDR(v_page);   
                // kprintf("%x\n", PAGE_TO_ADDR(v_page));            
            }
        } else {
            v_page_t v_page = l2_entry & PAGE_MASK;
            l1_pt = (struct l1_pt *) PAGE_TO_ADDR(v_page);
        }
    } else {
        result = l1_create(&l1_pt);
        if (result) {
            return result;
        }

        v_page_t v_page = ADDR_TO_PAGE((vaddr_t) l1_pt); 
        l2_pt->l2_entries[v_l2] = 0 
                                | ENTRY_VALID 
                                | ENTRY_READABLE
                                | ENTRY_WRITABLE
                                | v_page;        
    }

    l1_entry_t l1_entry = l1_pt->l1_entries[v_l1];
    p_page_t p_page; 

    // Get the faulting address' physical address
    if (l1_entry & ENTRY_VALID) {
        if (faulttype == VM_FAULT_WRITE && !(l1_entry & ENTRY_WRITABLE)) {
            p_page_t page = l1_entry & PAGE_MASK;
            cm_entry_t cm_entry = cm->cm_entries[page];

            if (cm_entry & COPY_TO_WRITE) {
                p_page = first_alloc_page;     
                result = find_free(1, &p_page); 
                if (result) {
                    return result;
                }
                
                cm->cm_entries[p_page] = 0 
                                       | PP_USED
                                       | (cm_entry & PAGE_MASK);
                                       
                cm_incref(p_page);

                // TODO: get rid of this dirty hack
                const void *src = (const void *) PADDR_TO_KVADDR(PAGE_TO_ADDR(page));
                void *dst = (void *) PADDR_TO_KVADDR(PAGE_TO_ADDR(p_page)); 
                memmove(dst, src, (size_t) PAGE_SIZE);

            // kprintf("@");
                l1_pt->l1_entries[v_l1] = 0 
                                        | ENTRY_VALID 
                                        | ENTRY_READABLE
                                        | ENTRY_WRITABLE
                                        | p_page;

                cm->cm_entries[page] = cm->cm_entries[page] & (~COPY_TO_WRITE);
            } else {
                l1_pt->l1_entries[v_l1] = l1_pt->l1_entries[v_l1] | ENTRY_WRITABLE;
                p_page = l1_pt->l1_entries[v_l1] & PAGE_MASK;
            }
        } else {
            p_page = l1_pt->l1_entries[v_l1] & PAGE_MASK;
        }
    } else {
        p_page = first_alloc_page;     
        result = find_free(1, &p_page); 
        if (result) {
            return result;
        }
        
        cm->cm_entries[p_page] = 0 
                                | PP_USED
                                | fault_page;

        l1_pt->l1_entries[v_l1] = 0 
                                | ENTRY_VALID 
                                | ENTRY_READABLE
                                | ENTRY_WRITABLE
                                | p_page;
    }

    uint32_t entryhi; 
    uint32_t entrylo; 

    paddr_t p_page_addr = p_page << 12; // TODO: maybe make a macro for this
    pid_t pid = curproc->pid;
    
    entryhi = 0 | fault_page | pid << 6; 
    entrylo = 0 | p_page_addr | TLBLO_VALID | TLBLO_DIRTY; // TODO: fix this magic number

    tlb_write(entryhi, entrylo, V_TO_INDEX(fault_page));

    return 0;
}