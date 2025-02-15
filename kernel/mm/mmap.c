#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <abi-bits/vm-flags.h>
#include <lib/alloc.h>
#include <lib/errno.h>
#include <lib/lock.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <lib/resource.h>
#include <lib/vector.h>
#include <mm/mmap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <sched/proc.h>
#include <sys/cpu.h>

struct addr2range {
    struct mmap_range_local *range;
    size_t memory_page;
    size_t file_page;
};

struct addr2range addr2range(struct pagemap *pagemap, uintptr_t virt) {
    VECTOR_FOR_EACH(pagemap->mmap_ranges, it) {
        struct mmap_range_local *local_range = *it;
        if (virt < local_range->base || virt >= local_range->base + local_range->length) {
            continue;
        }

        size_t memory_page = virt / PAGE_SIZE;
        size_t file_page = local_range->offset / PAGE_SIZE + (memory_page - local_range->base / PAGE_SIZE);
        return (struct addr2range){.range = local_range, .memory_page = memory_page, .file_page = file_page};
    }

    return (struct addr2range){.range = NULL, .memory_page = 0, .file_page = 0};
}

void mmap_list_ranges(struct pagemap *pagemap) {
    print("Ranges for %lx:\n", pagemap);

    VECTOR_FOR_EACH(pagemap->mmap_ranges, it) {
        struct mmap_range_local *local_range = *it;
        print("\tbase=%lx, length=%lx, offset=%lx\n", local_range->base, local_range->length, local_range->offset);
    }
}

bool mmap_handle_pf(struct cpu_ctx *ctx) {
    if ((ctx->err & 0x1) != 0) {
        return false;
    }

    // TODO: mmap can be expensive, consider enabling interrupts
    // temporarily
    uint64_t cr2 = 0;
    asm volatile ("mov %%cr2, %0" : "=r" (cr2));

    struct thread *thread = sched_current_thread();
    struct process *process = thread->process;
    struct pagemap *pagemap = process->pagemap;

    spinlock_acquire(&pagemap->lock);

    struct addr2range range = addr2range(pagemap, cr2);
    struct mmap_range_local *local_range = range.range;

    spinlock_release(&pagemap->lock);

    if (local_range == NULL) {
        return false;
    }

    void *page = NULL;
    if ((local_range->flags & MAP_ANONYMOUS) != 0) {
        page = pmm_alloc(1);
    } else {
        // TODO: Implement resource mmap
    }

    if (page == NULL) {
        return false;
    }

    return mmap_page_in_range(local_range->global, range.memory_page * PAGE_SIZE, (uintptr_t)page, local_range->prot);
}

bool mmap_page_in_range(struct mmap_range_global *global, uintptr_t virt,
                            uintptr_t phys, int prot) {
    uint64_t pt_flags = PTE_PRESENT | PTE_USER;

    if ((prot & PROT_WRITE) != 0) {
        pt_flags |= PTE_WRITABLE;
    }

    if (!vmm_map_page(global->shadow_pagemap, virt, phys, pt_flags)) {
        return false;
    }

    VECTOR_FOR_EACH(global->locals, it) {
        struct mmap_range_local *local_range = *it;
        if (virt < local_range->base || virt >= local_range->base + local_range->length) {
            continue;
        }

        if (!vmm_map_page(local_range->pagemap, virt, phys, pt_flags)) {
            return false;
        }
    }

    return true;
}

bool mmap_range(struct pagemap *pagemap, uintptr_t virt, uintptr_t phys,
                size_t length, int prot, int flags) {
    flags |= MAP_ANONYMOUS;

    uintptr_t aligned_virt = ALIGN_DOWN(virt, PAGE_SIZE);
    size_t aligned_length = ALIGN_UP(length + (virt - aligned_virt), PAGE_SIZE);

    struct mmap_range_global *global_range = NULL;
    struct mmap_range_local *local_range = NULL;

    global_range = ALLOC(struct mmap_range_global);
    if (global_range == NULL) {
        goto cleanup;
    }

    global_range->shadow_pagemap = vmm_new_pagemap();
    if (global_range->shadow_pagemap == NULL) {
        goto cleanup;
    }

    global_range->base = aligned_virt;
    global_range->length = aligned_length;

    local_range = ALLOC(struct mmap_range_local);
    if (local_range == NULL) {
        goto cleanup;
    }

    local_range->pagemap = pagemap;
    local_range->global = global_range;
    local_range->base = aligned_virt;
    local_range->length = aligned_length;
    local_range->prot = prot;
    local_range->flags = flags;

    VECTOR_PUSH_BACK(global_range->locals, local_range);

    spinlock_acquire(&pagemap->lock);

    VECTOR_PUSH_BACK(pagemap->mmap_ranges, local_range);

    spinlock_release(&pagemap->lock);

    for (size_t i = 0; i < aligned_length; i += PAGE_SIZE) {
        if (!mmap_page_in_range(global_range, aligned_virt + i, phys + i, prot)) {
            // FIXME: Page map is in inconsistent state at this point!
            goto cleanup;
        }
    }

cleanup:
    if (local_range != NULL) {
        free(local_range);
    }
    if (global_range != NULL) {
        if (global_range->shadow_pagemap != NULL) {
            vmm_destroy_pagemap(global_range->shadow_pagemap);
        }

        free(global_range);
    }
    return false;
}

void *mmap(struct pagemap *pagemap, uintptr_t addr, size_t length, int prot,
           int flags, struct resource *res, off_t offset) {
    struct mmap_range_global *global_range = NULL;
    struct mmap_range_local *local_range = NULL;

    // TODO: Implement resources
    if (length == 0 || res != NULL) {
        errno = EINVAL;
        goto cleanup;
    }
    length = ALIGN_UP(length, PAGE_SIZE);

    // if ((flags & MAP_ANONYMOUS) == 0 && res != NULL && !res->can_mmap) {
    //     errno = ENODEV;
    //     return NULL;
    // }

    struct process *process = sched_current_thread()->process;

    uint64_t base = 0;
    if ((flags & MAP_FIXED) != 0) {
        if (!munmap(pagemap, addr, length)) {
            goto cleanup;
        }
        base = addr;
    } else {
        base = process->mmap_anon_base;
        process->mmap_anon_base += length + PAGE_SIZE;
    }

    global_range = ALLOC(struct mmap_range_global);
    if (global_range == NULL) {
        errno = ENOMEM;
        goto cleanup;
    }

    global_range->shadow_pagemap = vmm_new_pagemap();
    if (global_range->shadow_pagemap == NULL) {
        goto cleanup;
    }

    global_range->base = base;
    global_range->length = length;
    global_range->res = res;
    global_range->offset = offset;

    local_range = ALLOC(struct mmap_range_local);
    if (local_range == NULL) {
        goto cleanup;
    }

    local_range->pagemap = pagemap;
    local_range->global = global_range;
    local_range->base = base;
    local_range->length = length;
    local_range->prot = prot;
    local_range->flags = flags;
    local_range->offset = offset;

    VECTOR_PUSH_BACK(global_range->locals, local_range);

    spinlock_acquire(&pagemap->lock);

    VECTOR_PUSH_BACK(pagemap->mmap_ranges, local_range);

    spinlock_release(&pagemap->lock);

    if (res != NULL) {
        res->refcount++;
    }

    return (void *)base;

cleanup:
    if (local_range != NULL) {
        free(local_range);
    }
    if (global_range != NULL) {
        if (global_range->shadow_pagemap != NULL) {
            vmm_destroy_pagemap(global_range->shadow_pagemap);
        }

        free(global_range);
    }
    return NULL;
}

bool munmap(struct pagemap *pagemap, uintptr_t addr, size_t length) {
    if (length == 0) {
        errno = EINVAL;
        return false;
    }
    length = ALIGN_UP(length, PAGE_SIZE);

    for (uintptr_t i = addr; i < addr + length; i += PAGE_SIZE) {
        struct addr2range range = addr2range(pagemap, i);
        if (range.range == NULL) {
            continue;
        }

        struct mmap_range_local *local_range = range.range;
        struct mmap_range_global *global_range = local_range->global;

        uintptr_t snip_begin = i;
        while (i < local_range->base + local_range->length && i < addr + length) {
            i += PAGE_SIZE;
        }

        uintptr_t snip_end = i;
        size_t snip_length = snip_end - snip_begin;

        spinlock_acquire(&pagemap->lock);

        if (snip_begin > local_range->base && snip_end < local_range->base + local_range->length) {
            struct mmap_range_local *postsplit_range = ALLOC(struct mmap_range_local);
            if (postsplit_range == NULL) {
                // FIXME: Page map is in inconsistent state at this point!
                errno = ENOMEM;
                spinlock_release(&pagemap->lock);
                return false;
            }

            postsplit_range->pagemap = local_range->pagemap;
            postsplit_range->global = global_range;
            postsplit_range->base = snip_end;
            postsplit_range->length = (local_range->base + local_range->length) - snip_end;
            postsplit_range->offset = local_range->offset + (off_t)(snip_end - local_range->base);
            postsplit_range->prot = local_range->prot;
            postsplit_range->flags = local_range->flags;

            VECTOR_PUSH_BACK(pagemap->mmap_ranges, postsplit_range);

            local_range->length -= postsplit_range->length;
        }

        spinlock_release(&pagemap->lock);

        for (uintptr_t j = snip_begin; j < snip_end; j += PAGE_SIZE) {
            vmm_unmap_page(pagemap, j);
        }

        if (snip_length == local_range->length) {
            VECTOR_REMOVE(pagemap->mmap_ranges, VECTOR_FIND(pagemap->mmap_ranges, local_range));
        }

        if (snip_length == local_range->length && global_range->locals.length == 1) {
            if ((local_range->flags & MAP_ANONYMOUS) != 0) {
                for (uintptr_t j = global_range->base; j < global_range->base + global_range->length; j += PAGE_SIZE) {
                    uintptr_t phys = vmm_virt2phys(pagemap, j);
                    if (phys == INVALID_PHYS) {
                        continue;
                    }

                    if (!vmm_unmap_page(pagemap, j)) {
                        // FIXME: Page map is in inconsistent state at this point!
                        errno = EINVAL;
                        return false;
                    }
                    pmm_free((void *)phys, 1);
                }
            } else {
                // TODO: res->unmap();
            }

            free(local_range);
        } else {
            if (snip_begin == local_range->base) {
                local_range->offset += snip_length;
                local_range->base = snip_end;
            }
            local_range->length -= snip_length;
        }
    }
    return true;
}
