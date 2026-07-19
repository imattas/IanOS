#pragma once
#include <stdint.h>
#include "hybrid/syscall.hpp"

namespace hk::drivers {

enum class DeviceClass : uint8_t { Unknown, Storage, Network, Display };
enum class ResourceType : uint8_t { None, Mmio, Io };

struct DeviceResource {
    ResourceType type;
    uint64_t base;
    uint64_t size;
};

struct DeviceInventoryEntry {
    DeviceClass device_class;
    const char* driver_name;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t required_command_bits;
    DeviceResource resources[3];
    uint8_t resource_count;
};

class DeviceInventory {
public:
    void rebuild();
    uint32_t count() const { return count_; }
    const DeviceInventoryEntry* entries() const { return entries_; }
    uint32_t storage_count() const;
    uint32_t network_count() const;
    uint32_t display_count() const;
    uint32_t resource_count() const;
    bool copy_info(uint32_t index, hybrid::DeviceInfo& out) const;
    bool copy_info_by_class(hybrid::DeviceClass device_class, uint32_t ordinal, hybrid::DeviceInfo& out) const;
private:
    DeviceInventoryEntry entries_[16]{};
    uint32_t count_ = 0;
    bool add(const DeviceInventoryEntry& entry);
};

DeviceInventory& inventory();
bool inventory_self_test();

} // namespace hk::drivers
