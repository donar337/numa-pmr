#pragma once

#include "size_class.hpp"

class ThreadCache {
public:
    void* allocate(SizeClass& sc);
    void  deallocate(SizeClass& sc, void* ptr);
};

ThreadCache& get_thread_cache();