include/
│
├── numa_memory_resource.hpp      // pmr интерфейс
│
├── core/
│   ├── allocation_router.hpp     // выбор арены
│   ├── numa_manager.hpp          // topology + arena registry
│   ├── numa_topology.hpp         // работа с NUMA системой
│   └── numa_config.hpp           // конфигурация (будущая расширяемость)
│
├── arena/
│   ├── numa_arena.hpp
│   ├── arena_policy.hpp          // политика арены (future fallback)
│   └── arena_stats.hpp           // пока пусто, но задел
│
├── small/
│   ├── small_object_allocator.hpp
│   ├── size_class.hpp
│   ├── slab.hpp
│   └── thread_cache.hpp
│
├── large/
│   ├── large_object_allocator.hpp
│   └── virtual_memory.hpp        // mmap / mbind abstraction
│
├── memory/
│   ├── block_header.hpp          // metadata layout
│   └── pointer_utils.hpp
│
└── platform/
    ├── linux_numa.hpp            // syscalls или libnuma (пока stub)
    └── cpu_affinity.hpp