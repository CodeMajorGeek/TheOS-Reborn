#ifndef _VMM_PRIVATE_H
#define _VMM_PRIVATE_H

#include <Memory/VMM.h>

static uintptr_t VMM_hhdm_to_phys(uintptr_t virt);
static uintptr_t VMM_phys_to_hhdm(uintptr_t phys);

#endif
