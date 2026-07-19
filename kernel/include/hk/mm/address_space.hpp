#pragma once
#include <stdint.h>
#include "hk/mm/vmm.hpp"

namespace hk::mm {

struct AddressSpace {
    uint64_t pml4;
};

AddressSpace create_address_space();
MappingResult map_page(AddressSpace& space, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t translate(const AddressSpace& space, uint64_t virt);
uint64_t page_flags(const AddressSpace& space, uint64_t virt);
void destroy_address_space(AddressSpace& space);
bool address_space_self_test();

} // namespace hk::mm
