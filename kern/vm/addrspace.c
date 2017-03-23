#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
#include <spl.h>
#include <mips/tlb.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 */

int
l1_create(struct l1_pt **l1_pt)
{
    *l1_pt = kmalloc(sizeof(struct l1_pt));
    if (*l1_pt == NULL) {
        return ENOMEM;
    }

	KASSERT(((vaddr_t) *l1_pt) % PAGE_SIZE == 0); 

    for (v_page_l1_t v_l1 = 0; v_l1 < NUM_L1PT_ENTRIES; v_l1++) {
        (*l1_pt)->l1_entries[v_l1] = 0;
	}

    return 0;
}

void
l2_init(struct l2_pt *l2_pt)
{
	for (v_page_l2_t v_l2 = 0; v_l2 < NUM_L2PT_ENTRIES; v_l2++) {
		l2_pt->l2_entries[v_l2] = 0;
	}
}

struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	l2_init(&as->l2_pt);

	return as;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *newas;
	// int result;

	newas = as_create();
	if (newas==NULL) {
		return ENOMEM;
	}

	struct l2_pt *l2_pt_new = &newas->l2_pt;
	struct l2_pt *l2_pt_old = &old->l2_pt;

	for (v_page_l2_t v_l2 = 0; v_l2 < NUM_L2PT_ENTRIES; v_l2++) {
		l2_pt_old->l2_entries[v_l2] = l2_pt_old->l2_entries[v_l2] & (~ENTRY_WRITABLE);

		if (l2_pt_old->l2_entries[v_l2] & ENTRY_VALID) {
			p_page_t p_page1 = KVPAGE_TO_PPAGE(l2_pt_old->l2_entries[v_l2] & PAGE_MASK);
			cm_incref(p_page1);

			struct l1_pt *l1_pt_old = (struct l1_pt *) PAGE_TO_ADDR(l2_pt_old->l2_entries[v_l2] & PAGE_MASK);

			for (v_page_l1_t v_l1 = 0; v_l1 < NUM_L1PT_ENTRIES; v_l1++) {
				l1_pt_old->l1_entries[v_l1] = l1_pt_old->l1_entries[v_l1] & (~ENTRY_WRITABLE);
				p_page_t p_page = l1_pt_old->l1_entries[v_l1] & PAGE_MASK;
				cm_incref(p_page);
			}
		}

		l2_pt_new->l2_entries[v_l2] = l2_pt_old->l2_entries[v_l2];
	}

	// TODO: change the dirty bits of the correct process, not just invalidate all tlb entries

	int spl = splhigh();
	uint32_t entryhi; 
	uint32_t entrylo; 
	uint32_t index;

	for (index = 0; index < NUM_TLB; index++) {
		tlb_read(&entryhi, &entrylo, index);
		entrylo = entrylo & (~TLB_VALID_BIT);
		tlb_write(entryhi, entrylo, index);
	}

	splx(spl);

	*ret = newas;
	return 0;
}

void
as_destroy(struct addrspace *as)
{
	kfree(as);
}

void
as_activate(void)
{
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		/*
		 * Kernel thread without an address space; leave the
		 * prior address space in place.
		 */
		return;
	}

	int spl = splhigh();
	uint32_t entryhi; 
	uint32_t entrylo; 
	uint32_t index;

	for (index = 0; index < NUM_TLB; index++) {
		tlb_read(&entryhi, &entrylo, index);
		entrylo = entrylo & (~TLB_VALID_BIT);
		tlb_write(entryhi, entrylo, index);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/*
	 * Write this. For many designs it won't need to actually do
	 * anything. See proc.c for an explanation of why it (might)
	 * be needed.
	 */
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	/*
	 * Write this.
	 */

	(void)as;
	(void)vaddr;
	(void)sz;
	(void)readable;
	(void)writeable;
	(void)executable;
	return 0;
}

int
as_prepare_load(struct addrspace *as)
{
	/*
	 * Write this.
	 */

	(void)as;
	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	/*
	 * Write this.
	 */

	(void)as;
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	/*
	 * Write this.
	 */

	(void)as;

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;

	return 0;
}

