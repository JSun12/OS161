#include <types.h>
#include <vm.h>
#include <msyscall.h>
#include <kern/errno.h>
#include <synch.h>
#include <addrspace.h>
#include <proc.h>
#include <current.h>


/* Global lock and wait channel from vm.c */
extern struct lock *global_lock;
extern struct cv *global_cv;
extern p_page_t last_page;
extern size_t cm_counter;

/*
Get the number of pages to be allocated between old and new places in l1 & l2
*/
static
int
get_num_used(v_page_l1_t old_l1, v_page_l2_t old_l2, v_page_l1_t new_l1, v_page_l2_t new_l2)
{
    int num_used = 0;
    if (old_l2 == new_l2){
        num_used += (new_l1 - old_l1);
    } else {
        num_used += (new_l2 - old_l2 - 1) * NUM_L1PT_ENTRIES;
        num_used += NUM_L1PT_ENTRIES - old_l1;
        num_used += new_l1;
    }
    return num_used;
}

/*
Frees pages between the old and new places in l1 & l2 in a loop
*/
static
void
free_sbrk(struct l2_pt *l2_pt, v_page_l1_t old_l1, v_page_l2_t old_l2, v_page_l1_t new_l1, v_page_l2_t new_l2){
    if (old_l2 == new_l2) {
        for (v_page_l1_t v_l1 = new_l1; v_l1 < old_l1; v_l1++) {
            free_vpage(l2_pt, new_l2, v_l1);
        }
    } else {
        for (v_page_l2_t v_l2 = new_l2 + 1; v_l2 < old_l2; v_l2++) {
            for (v_page_l1_t v_l1 = 0; v_l1 < NUM_L1PT_ENTRIES; v_l1++) {
                free_vpage(l2_pt, v_l2, v_l1);
            }

            free_l1_pt(l2_pt, v_l2);
        }

        for (v_page_l1_t v_l1 = 0; v_l1 < old_l1; v_l1++) {
            free_vpage(l2_pt, old_l2, v_l1);
        }

        free_l1_pt(l2_pt, old_l2);

        for (v_page_l1_t v_l1 = new_l1; v_l1 < NUM_L1PT_ENTRIES; v_l1++) {
            free_vpage(l2_pt, new_l2, v_l1);
        }
    }
}

/*
Allocates all the pages between the old and new places in l1 & l2 in a loop
*/
static
int
allocate_sbrk(struct l2_pt *l2_pt, v_page_l1_t old_l1, v_page_l2_t old_l2, v_page_l1_t new_l1, v_page_l2_t new_l2)
{
    int result;

    if (!(l2_pt->l2_entries[old_l2] & ENTRY_VALID)) {
        result = add_l1_pt(l2_pt, old_l2, NULL);
        if (result) return result;
    }

    if (old_l2 == new_l2){
        for (v_page_l1_t v_l1 = old_l1; v_l1 < new_l1; v_l1++) {
            result = add_ppage(l2_pt, old_l2, v_l1);
            if (result) return result;
        }
    } else {
        for (v_page_l2_t v_l2 = old_l2 + 1; v_l2 < new_l2; v_l2++) {
            result = add_l1_pt(l2_pt, v_l2, NULL);
            if (result) return result;

            for (v_page_l1_t v_l1 = 0; v_l1 < NUM_L1PT_ENTRIES; v_l1++) {
                result = add_ppage(l2_pt, v_l2, v_l1);
                if (result) return result;
            }
        }

        for (v_page_l1_t v_l1 = old_l1; v_l1 < NUM_L1PT_ENTRIES; v_l1++) {
            result = add_ppage(l2_pt, old_l2, v_l1);
            if (result) return result;
        }

        result = add_l1_pt(l2_pt, new_l2, NULL);
        if (result) return result;

        for (v_page_l1_t v_l1 = 0; v_l1 < new_l1; v_l1++) {
            result = add_ppage(l2_pt, new_l2, v_l1);
            if (result) return result;
        }
    }

    return 0;
}

/*
XXX: There is some memory leak going on with sbrk
*/
int
sys_sbrk(ssize_t amount, int32_t *retval0)
{
    if (amount % PAGE_SIZE) {
        return EINVAL;
    }

    lock_acquire(global_lock);

    while (!enough_free()) {
        cv_wait(global_cv, global_lock);
    }

    struct addrspace *as = curproc->p_addrspace;

    vaddr_t stack_top = as->stack_top;
    vaddr_t old_heap_end = as->brk;
    vaddr_t new_heap_end = old_heap_end + amount;

    if (new_heap_end < as->heap_base) {
        lock_release(global_lock);
        return EINVAL;
    }

    int64_t overflow = (int64_t)old_heap_end + (int64_t)amount;
    if (overflow > USERSPACETOP || overflow < 0){
        lock_release(global_lock);
        return EINVAL;
    }

    if (new_heap_end > stack_top) {
        lock_release(global_lock);
        return ENOMEM;
    }

    v_page_l2_t old_l2 = L2_PNUM(old_heap_end);
    v_page_l1_t old_l1 = L1_PNUM(old_heap_end);
    v_page_l2_t new_l2 = L2_PNUM(new_heap_end);
    v_page_l1_t new_l1 = L1_PNUM(new_heap_end);
    struct l2_pt *l2_pt = as->l2_pt;

    if (new_heap_end < old_heap_end) {
        free_sbrk(l2_pt, old_l1, old_l2, new_l1, new_l2);
        tlb_invalidate();
    }
    else if (new_heap_end > old_heap_end)
    {
        int result;
        int used = get_num_used(old_l1, old_l2, new_l1, new_l2);

        int32_t free_pages = last_page - cm_counter;
        if (used > free_pages - MIN_FREE_PAGES){
            lock_release(global_lock);
            *retval0 = -1;
            return ENOMEM;
        }

        result = allocate_sbrk(l2_pt, old_l1, old_l2, new_l1, new_l2);
        if (result){
            panic("Kernel checked for empty pages, found them, but could not use them.");
        }
    }

    *retval0 = old_heap_end;
    as->brk = new_heap_end;

    lock_release(global_lock);

    return 0;
}
