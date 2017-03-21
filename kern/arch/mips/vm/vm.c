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
    pid_t pid = 1;         
    cm->cm_entries[p_page] = 0 | PP_USED | v_page | pid << 20; // TODO: allocate pids better
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

    vaddr_t addr =  PADDR_TO_KVADDR(PAGE_TO_ADDR(start));
    // kprintf("%x\n", addr);
    return addr;
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


static
int
l1_create(struct l1_pt **l1_pt)
{
    *l1_pt = kmalloc(sizeof(struct l1_pt));
    if (*l1_pt == NULL) {
        return ENOMEM;
    }

	for (v_page_l1_t v_l1 = 0; v_l1 < NUM_L1PT_ENTRIES; v_l1++) {
        (*l1_pt)->l1_entries[v_l1] = 0x00000000;
	}

    return 0;
}


int 
vm_fault(int faulttype, vaddr_t faultaddress)
{
    (void) faulttype;

    int result;
    
    vaddr_t fault_page = faultaddress & PAGE_FRAME;
    v_page_l2_t v_l2 = (faultaddress & L2_PAGE_NUM_MASK) >> 22;
    v_page_l1_t v_l1 = (faultaddress & L1_PAGE_NUM_MASK) >> 12;

    struct l2_pt *l2_pt = &(curproc->p_addrspace)->l2_pt;
    l2_entry_t l2_entry = l2_pt->l2_entries[v_l2]; 
    
    struct l1_pt *l1_pt; 

    if (!(l2_entry & ENTRY_VALID)) {
        result = l1_create(&l1_pt);
        if (result) {
            return result;
        }

        KASSERT(((uint32_t) l1_pt) % PAGE_SIZE == 0); 

        v_page_t v_page = ((uint32_t) l1_pt >> 12); 
        l2_pt->l2_entries[v_l2] = 0 | ENTRY_VALID | v_page;
    } else {
        v_page_t v_page = l2_entry & PAGE_MASK;
        l1_pt = (struct l1_pt *) ((uint32_t) v_page << 12); // TODO: consider making a macro for page number to page address
    }

    l1_entry_t l1_entry = l1_pt->l1_entries[v_l1];
    p_page_t p_page; 

    if (!(l1_entry & ENTRY_VALID)) {
        p_page = first_alloc_page;     
        result = find_free(1, &p_page); 
        if (result) {
            return result;
        }
        
        kalloc_ppage(p_page);
        l1_pt->l1_entries[v_l1] = 0 | ENTRY_VALID | p_page;
    } else {
        p_page = l1_entry & PAGE_MASK;
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