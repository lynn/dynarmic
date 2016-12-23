/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * This software may be used and distributed according to the terms of the GNU
 * General Public License version 2 or any later version.
 */

#pragma once

#include <cstddef>
// #include <iterator>
// #include <memory>
// #include <type_traits>

#include "common/common_types.h"

namespace Dynarmic {
namespace Common {

class AddressRange {
public:
    virtual bool Includes(u32 address) const = 0;
};

class AddressInterval : public AddressRange {
public:
    AddressInterval(u32 start_address, std::size_t length)
        : start_address(start_address), length(length) {}

    bool Includes(u32 address) const override {
        return address >= start_address && address < start_address + length;
    }

private:
    u32 start_address;
    std::size_t length;
};

class FullAddressRange : public AddressRange {
public:
    bool Includes(u32 address) const override {
        return true;
    }
};

} // namespace Common
} // namespace Dynarmic