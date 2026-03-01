#pragma once

#include <memory_resource>
#include "core/numa_manager.hpp"

class NumaMemoryResource : public std::pmr::memory_resource {
public:
    explicit NumaMemoryResource(NumaManager& manager);

protected:
    void* do_allocate(size_t bytes, size_t alignment) override;
    void  do_deallocate(void* p, size_t bytes, size_t alignment) override;
    bool  do_is_equal(const memory_resource& other) const noexcept override;

private:
    NumaManager& manager_;
};