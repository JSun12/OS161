#include <types.h>
#include <spinlock.h>
#include <vm.h>
#include <lib.h>
#include <mips/tlb.h>

// create l1 tables (adjust coremap and l2), and free l1 page table (during coallecing)

// static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
static struct coremap *cm;
static p_page_t first_alloc_page; /* First physical page that can be dynamically allocated */

void 
vm_bootstrap()
{
    paddr_t cm_paddr = ram_stealmem(COREMAP_PAGES);
    if (cm_paddr == 0) {
        kprintf("Not enough memory for coremap.\n"); // TODO: handle failure better
    }

    cm = (struct coremap *) PADDR_TO_KVADDR(cm_paddr);

    KASSERT(ram_stealmem(0) % PAGE_SIZE == 0);
    first_alloc_page = ADDR_TO_PAGE(ram_stealmem(0));

    size_t pages_used = first_alloc_page;
    v_page_t v_page;
    for (p_page_t p_page = 0; p_page < pages_used; p_page++) {
        v_page = PPAGE_TO_KVPAGE(p_page);
        //put process id
        cm->cm_entries[p_page] = 0 | PP_USED | v_page; 
    }
}

static 
bool 
p_page_used(struct coremap *cm, p_page_t p_page)
{
    cm_entry_t entry = cm->cm_entries[p_page];
    bool used = entry & PP_USED;
    return used;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t
alloc_kpages(size_t npages)
{
    //manage end of ram

    // Find contiguous free physical pages
    p_page_t start = first_alloc_page; 
    size_t offset = 0;
    while (1) {
        if (p_page_used(cm, start)) {
            start++;
            continue;
        }

        offset = 0; 
        while (!p_page_used(cm, start + offset) && offset + 1 < npages) {
            offset++;
        }

        if (!p_page_used(cm, start + offset) && offset + 1 == npages) {
            break;
        }

        start++;
    }

    // Set the corresponding coremap entries
    v_page_t v_page;
    for (p_page_t p_page = start; p_page < start + npages; p_page++) {
         v_page = PPAGE_TO_KVPAGE(p_page);
        //put process id
        cm->cm_entries[p_page] = 0 | PP_USED | v_page; 
    }

    cm->cm_entries[start + offset] = cm->cm_entries[start + offset] | KMALLOC_END;

    vaddr_t addr =  PADDR_TO_KVADDR(PAGE_TO_ADDR(start));
    kprintf("%x\n", addr);
    return addr;
}



void 
free_kpages(vaddr_t addr)
{
    p_page_t curr = ADDR_TO_PAGE(KVADDR_TO_PADDR(addr));
    bool end;
    while (p_page_used(cm, curr)) {
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


int 
vm_fault(int faulttype, vaddr_t faultaddress)
{
    (void) faulttype;
    (void) faultaddress;

    return 0;
}