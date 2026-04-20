#include "small_object_allocator.hpp"

// ============================================================
// SLAB
// ============================================================

Slab* Slab::create(size_t block_size, int node_id) {
    void* mem = VirtualMemory::alloc_on_node(SLAB_SIZE, node_id);

    auto* slab = reinterpret_cast<Slab*>(mem);
    slab->init(block_size, node_id);
    return slab;
}

void* Slab::allocate_block() {
    auto* header = header_ptr();
    if (!header->free_list)
        return nullptr;

    void* block = header->free_list;

    auto* bh = reinterpret_cast<BlockHeader*>(block);
    bh->raw_ptr = this;
    bh->total_size = SLAB_SIZE;

    void* user_ptr = bh->to_user_ptr();

    header->free_list = *reinterpret_cast<void**>(user_ptr);
    header->free_count--;

    return block;
}

void Slab::free_block(void* block) {
    auto* header = header_ptr();
    auto* bh = reinterpret_cast<BlockHeader*>(block);
    void* user_ptr = bh->to_user_ptr();

    *reinterpret_cast<void**>(user_ptr) = header->free_list;
    header->free_list = block;
    header->free_count++;
}

bool Slab::has_free() const noexcept {
    return header_ptr()->free_count > 0;
}

bool Slab::is_empty() const noexcept {
    auto* header = header_ptr();
    return header->free_count == header->capacity;
}

void Slab::init(size_t block_size, int node_id) {
    auto* header = header_ptr();
    header->node_id    = node_id;
    header->block_size = block_size;

    char* raw = reinterpret_cast<char*>(this) + sizeof(SlabHeader);
    char* ptr = reinterpret_cast<char*>(
        VirtualMemory::align_up(reinterpret_cast<uintptr_t>(raw), ALIGNMENT)
    );

    size_t usable = SLAB_SIZE - (ptr - reinterpret_cast<char*>(this));
    size_t payload_size = std::max(block_size, sizeof(void*));

    size_t total_block_size =
        VirtualMemory::align_up(sizeof(BlockHeader) + payload_size, ALIGNMENT);

    header->capacity = usable / total_block_size;
    header->free_count = header->capacity;
    header->free_list = nullptr;

    for (size_t i = 0; i < header->capacity; ++i) {
        char* raw_block = ptr + i * total_block_size;
        auto* bh = reinterpret_cast<BlockHeader*>(raw_block);
        void* user_ptr = bh->to_user_ptr();

        *reinterpret_cast<void**>(user_ptr) = header->free_list;
        header->free_list = raw_block;
    }
}

Slab::SlabHeader* Slab::header_ptr() const noexcept {
    return reinterpret_cast<SlabHeader*>(
        const_cast<Slab*>(this)
    );
}

// ============================================================
// SIZE CLASS
// ============================================================

SizeClass::SizeClass(size_t block_size, int node_id)
    : block_size_(block_size),
      node_id_(node_id),
      current_(nullptr),
      slabs_(NodeLocalAllocator<Slab*>(node_id)) {}

void* SizeClass::allocate() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (current_ && current_->has_free())
        return current_->allocate_block();

    for (auto* slab : slabs_) {
        if (slab->has_free()) {
            current_ = slab;
            return current_->allocate_block();
        }
    }

    current_ = Slab::create(block_size_, node_id_);
    slabs_.push_back(current_);
    return current_->allocate_block();
}

void* SizeClass::allocate_existing() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (current_ && current_->has_free())
        return current_->allocate_block();

    for (auto* slab : slabs_) {
        if (slab->has_free()) {
            current_ = slab;
            return current_->allocate_block();
        }
    }

    return nullptr;
}

void* SizeClass::allocate_new_slab() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (current_ && current_->has_free())
        return current_->allocate_block();

    for (auto* slab : slabs_) {
        if (slab->has_free()) {
            current_ = slab;
            return current_->allocate_block();
        }
    }

    current_ = Slab::create(block_size_, node_id_);
    slabs_.push_back(current_);
    return current_->allocate_block();
}

void SizeClass::deallocate(void* ptr) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto* block_header = reinterpret_cast<BlockHeader*>(ptr);
    auto* slab = static_cast<Slab*>(block_header->raw_ptr);

    if (!slab) {
        throw std::invalid_argument("small allocation has no owning slab");
    }

    slab->free_block(ptr);

    if (slab->is_empty()) {
        release_extra_empty_slabs_locked();
    }
}

void SizeClass::deallocate_batch(BlockHeader* head) {
    if (!head) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    bool saw_empty_slab = false;
    while (head) {
        auto** next = reinterpret_cast<BlockHeader**>(head->to_user_ptr());
        BlockHeader* current = head;
        head = *next;

        auto* slab = static_cast<Slab*>(current->raw_ptr);
        if (!slab) {
            throw std::invalid_argument("small allocation has no owning slab");
        }

        slab->free_block(current);
        saw_empty_slab = saw_empty_slab || slab->is_empty();
    }

    if (saw_empty_slab) {
        release_extra_empty_slabs_locked();
    }
}

void SizeClass::release_extra_empty_slabs_locked() {
    Slab* retained_empty_slab = nullptr;

    for (size_t i = 0; i < slabs_.size();) {
        Slab* slab = slabs_[i];
        if (!slab->is_empty()) {
            ++i;
            continue;
        }

        if (!retained_empty_slab) {
            retained_empty_slab = slab;
            ++i;
            continue;
        }

        if (current_ == slab) {
            current_ = nullptr;
        }

        std::swap(slabs_[i], slabs_.back());
        slabs_.pop_back();
        VirtualMemory::release(slab, SLAB_SIZE);
    }

    if (current_ &&
        std::find(slabs_.begin(), slabs_.end(), current_) == slabs_.end()) {
        current_ = nullptr;
    }

    if ((!current_ || !current_->has_free()) && retained_empty_slab) {
        current_ = retained_empty_slab;
    }
}

// ============================================================
// SMALL OBJECT ALLOCATOR
// ============================================================

void* SmallObjectAllocator::allocate(size_t size) {
    return allocate_by_class_index(SizeClassTable::class_index_for_size(size));
}

void* SmallObjectAllocator::allocate_by_class_index(size_t class_index) {
    if (class_index >= kNumSizeClasses) {
        throw std::out_of_range("invalid small allocation size class");
    }

    return classes_[class_index].allocate();
}

void* SmallObjectAllocator::allocate_by_class_index(
    size_t class_index,
    SlowPathDrain drain,
    void* drain_context
) {
    if (class_index >= kNumSizeClasses) {
        throw std::out_of_range("invalid small allocation size class");
    }

    auto& sc = classes_[class_index];
    if (void* block = sc.allocate_existing()) {
        return block;
    }

    if (drain) {
        drain(drain_context, class_index);
    }

    if (void* block = sc.allocate_existing()) {
        return block;
    }

    return sc.allocate_new_slab();
}

void SmallObjectAllocator::deallocate(void* block) {
    auto* header = reinterpret_cast<BlockHeader*>(block);
    size_t cls_size = header->size_class;

    auto& sc = get_size_class(cls_size);
    sc.deallocate(block);
}

void SmallObjectAllocator::deallocate_batch(size_t class_index, BlockHeader* head) {
    if (class_index >= kNumSizeClasses) {
        throw std::out_of_range("invalid small allocation size class");
    }

    classes_[class_index].deallocate_batch(head);
}

SizeClass& SmallObjectAllocator::get_size_class(size_t size) {
    size_t class_index = SizeClassTable::class_index_for_size(size);
    if (class_index >= kNumSizeClasses || kClassSizes[class_index] != size) {
        throw std::out_of_range("invalid small allocation size class");
    }

    return classes_[class_index];
}
