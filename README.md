# NUMA Allocator

`NUMA Allocator` - это экспериментальная C++20-библиотека для NUMA-aware выделения памяти через стандартный интерфейс `std::pmr::memory_resource`.

Проект предоставляет несколько `memory_resource`, которые можно передавать в `std::pmr`-контейнеры и в пользовательский код, ожидающий полиморфный аллокатор. Главная идея библиотеки - размещать память ближе к потоку, который с ней работает, и уменьшать стоимость повторных выделений за счет арен, slab allocation, size classes и thread-local cache.

Библиотека ориентирована на сценарии, где важно контролировать locality памяти:

- многопоточные приложения на NUMA-машинах;
- сервисы с большим количеством короткоживущих small allocations;
- структуры данных на `std::pmr`, где хочется заменить стандартный upstream allocator;
- эксперименты с поведением аллокаторов, локальностью памяти и кэшированием освобожденных блоков.

Если NUMA недоступна, библиотека деградирует к single-node поведению: используется узел `0`, а API остается тем же.

## Что предоставляет проект

Публичный пользовательский слой сейчас состоит из трех `std::pmr::memory_resource`:

- `numa_memory_resource` - основной NUMA-aware ресурс, использующий глобальный `ArenaManager`, арену для текущего NUMA node и опциональный `ThreadLocalCache`.
- `numa_arena_memory_resource` - самостоятельный PMR-ресурс, владеющий отдельной `NumaArena` на выбранном NUMA node. Он не использует `ArenaManager` и `ThreadLocalCache`.
- `numa_simple_memory_resource` - простой upstream-ресурс: каждое выделение напрямую мапит новый span через ОС, привязывает его к NUMA node и освобождает через `munmap`.

Все три класса реализуют стандартный интерфейс `std::pmr::memory_resource`, поэтому их можно использовать с `std::pmr::vector`, `std::pmr::string`, `std::pmr::unordered_map`, `std::pmr::polymorphic_allocator` и собственными структурами данных.

### `numa_memory_resource`

`numa_memory_resource` - основной ресурс библиотеки. Он предназначен для обычного использования в многопоточном приложении, где каждый поток должен получать память из арены своего NUMA node.

```cpp
#include "numa_memory_resource.hpp"

#include <cstdint>
#include <memory_resource>
#include <vector>

int main() {
    std::pmr::memory_resource* resource = default_numa_memory_resource();

    std::pmr::vector<std::uint8_t> buffer(resource);
    buffer.resize(8192);
}
```

При первом обращении из потока `numa_memory_resource` создает thread-local контекст. В этом контексте фиксируются:

- текущий NUMA node, вычисленный по CPU, на котором выполняется поток;
- ссылка на `NumaArena` этого узла;
- настройка `do_pinning`;
- настройка `use_thread_cache`.

После этого поток обслуживает выделения через свою `ThreadLocalCache` и арену выбранного узла. Для small allocations это дает быстрый путь без обращения к глобальным структурам при cache hit.

Конструктор:

```cpp
numa_memory_resource resource(
    bool do_pinning = false,
    bool use_thread_cache = true
);
```

Параметры:

- `do_pinning` - при создании thread-local контекста попытаться закрепить текущий поток на CPU выбранного NUMA node;
- `use_thread_cache` - включить или отключить thread-local cache для small allocations.

Также есть helper:

```cpp
std::pmr::memory_resource* resource = default_numa_memory_resource();
std::pmr::memory_resource* pinned = default_numa_memory_resource(true);
std::pmr::memory_resource* no_cache = default_numa_memory_resource(false, false);
```

`default_numa_memory_resource()` возвращает один из статических экземпляров `numa_memory_resource` для комбинации `do_pinning` и `use_thread_cache`.

Имя функции не `numa_memory_resource()`, потому что в C++ в одной области видимости нельзя объявить класс и функцию с одним и тем же идентификатором.

Важная особенность PMR equality: все объекты `numa_memory_resource` считаются равными друг другу. Это означает, что с точки зрения PMR они представляют один логический тип ресурса, даже если созданы с разными флагами.

### `numa_arena_memory_resource`

`numa_arena_memory_resource` владеет отдельной `NumaArena` и не обращается к глобальному `ArenaManager`. Это полезно, когда нужно явно создать изолированную арену:

- для одного компонента;
- для scoped lifetime;
- для single-threaded сценария без синхронизации;
- для эксперимента с конкретным NUMA node;
- когда не нужен thread-local cache.

```cpp
#include "numa_arena_memory_resource.hpp"

#include <memory_resource>
#include <string>
#include <vector>

int main() {
    numa_arena_memory_resource resource(0, true);

    std::pmr::vector<int> values(&resource);
    std::pmr::string text(&resource);

    values.resize(1024);
    text.append(256, 'x');
}
```

Конструкторы:

```cpp
numa_arena_memory_resource(bool sync = true, bool do_pinning = false);
numa_arena_memory_resource(int node_id, bool sync = true, bool do_pinning = false);
```

Параметры:

- `node_id` - NUMA node, на котором будет создана арена. Некорректный `node_id` заменяется на node текущего CPU;
- `sync` - включает mutex-based синхронизацию внутри small/large allocators. Если `false`, ресурс рассчитан на single-threaded владение;
- `do_pinning` - попытаться закрепить создающий поток на выбранном NUMA node.

`numa_arena_memory_resource` сравнивается по идентичности: два разных экземпляра не равны друг другу, даже если используют один и тот же `node_id`.

### `numa_simple_memory_resource`

`numa_simple_memory_resource` - базовый, предсказуемый upstream allocator для NUMA-aware памяти. Он специально спроектирован как минимальный слой между `std::pmr::memory_resource` и OS virtual memory: без арен, slab allocation, size classes, thread-local cache, span cache и других оптимизирующих структур.

Его задача - дать простой и прозрачный способ получить memory mapping, привязанный к NUMA node. Это делает `numa_simple_memory_resource` удобной опорной реализацией: его поведение легко объяснить, легко сравнить с более сложными ресурсами и легко использовать там, где важнее предсказуемость, чем максимальная скорость на частых allocation/deallocation циклах.

Каждое выделение:

1. вычисляет полный размер с учетом alignment и служебного header;
2. округляет его до page size;
3. вызывает `VirtualMemory::alloc_on_node`;
4. возвращает пользователю aligned pointer;
5. при освобождении читает header и вызывает `VirtualMemory::release`.

```cpp
#include "numa_simple_memory_resource.hpp"

#include <memory_resource>
#include <vector>

int main() {
    numa_simple_memory_resource resource(0);

    std::pmr::vector<int> values(&resource);
    values.resize(4096);
}
```

Варианты создания:

```cpp
numa_simple_memory_resource fixed_to_current_cpu_node;
numa_simple_memory_resource fixed_to_node(0);

auto dynamic_node = numa_simple_memory_resource::current_node_per_allocation();
```

Обычные конструкторы фиксируют NUMA node на время жизни ресурса. `current_node_per_allocation()` пересчитывает NUMA node для каждого выделения по текущему CPU. Это гибко, но дорого, поэтому такой режим стоит использовать только осознанно.

`numa_simple_memory_resource` полезен именно как upstream allocator: он не пытается самостоятельно решать задачу высокопроизводительного reuse, а предоставляет понятную политику получения памяти у ОС с NUMA binding. Поверх него можно строить другие allocator layers, использовать его как baseline в benchmark-ах или выбирать его для редких крупных выделений, где стоимость системного вызова приемлема.

## Как использовать

Основной способ использования - передать ресурс в PMR-контейнер.

```cpp
#include "numa_memory_resource.hpp"

#include <memory_resource>
#include <string>
#include <vector>

struct RequestState {
    std::pmr::vector<int> ids;
    std::pmr::string payload;

    explicit RequestState(std::pmr::memory_resource* resource)
        : ids(resource),
          payload(resource)
    {}
};

int main() {
    auto* resource = default_numa_memory_resource();

    RequestState state(resource);
    state.ids.reserve(1024);
    state.payload.append("hello");
}
```

Можно также использовать `std::pmr::polymorphic_allocator` напрямую:

```cpp
auto* resource = default_numa_memory_resource();
std::pmr::polymorphic_allocator<std::byte> allocator(resource);

std::byte* data = allocator.allocate(4096);
allocator.deallocate(data, 4096);
```

Для большинства приложений стоит начинать с `default_numa_memory_resource()`: он включает thread-local cache и автоматически выбирает арену текущего NUMA node.

`numa_arena_memory_resource` стоит выбирать, если ресурс должен иметь явное владение и ограниченный lifetime. Например, можно создать арену на время работы одного пайплайна, очереди или набора временных структур.

`numa_simple_memory_resource` стоит выбирать, если нужен базовый и предсказуемый upstream allocator: один allocation равен одному OS mapping, NUMA placement задается явно, а освобождение напрямую возвращает mapping ОС. Такая модель медленнее на частых выделениях, но очень удобна как фундаментальный слой, baseline для сравнения и простая альтернатива оптимизирующим аренам.

## Архитектура

Архитектура проекта разделена на несколько слоев:

```text
std::pmr containers / user code
        |
        v
numa_memory_resource / numa_arena_memory_resource / numa_simple_memory_resource
        |
        v
ThreadLocalCache        ArenaManager
        |                   |
        v                   v
                 NumaArena per NUMA node
                         |
                         v
          SmallObjectAllocator / LargeObjectAllocator
                         |
                         v
              VirtualMemory: mmap + mbind + munmap
```

`numa_simple_memory_resource` идет почти напрямую в `VirtualMemory`, минуя основную allocator architecture. Это намеренное свойство: ресурс играет роль базового upstream allocator с максимально прозрачным поведением. `numa_arena_memory_resource` использует собственную `NumaArena`. `numa_memory_resource` использует `ArenaManager`, thread-local контекст и разделяемые арены на NUMA node.

### `VirtualMemory`

`VirtualMemory` - нижний слой проекта. Он инкапсулирует работу с виртуальной памятью:

- `reserve(size)` вызывает `mmap` для anonymous private read/write mapping;
- `alloc_on_node(size, node)` вызывает `reserve`, затем применяет NUMA policy через `mbind`;
- `release(ptr, size)` освобождает mapping через `munmap`;
- `bind_to_node(ptr, size, node)` применяет `MPOL_BIND` для конкретного NUMA node;
- `align_up` используется почти всеми слоями для выравнивания размеров и адресов.

Размеры mapping округляются до page size. Если NUMA недоступна, node `0` считается допустимым single-node fallback.

### `ArenaManager`

`ArenaManager` - singleton, который строит представление NUMA topology и владеет аренами для всех NUMA nodes.

При инициализации он:

1. проверяет доступность NUMA через `numa_available`;
2. определяет количество CPU и NUMA nodes;
3. строит отображение `cpu -> node`;
4. строит список CPU для каждого node;
5. создает `NumaArena` для каждого NUMA node.

Если NUMA недоступна, создается single-node topology: все CPU относятся к node `0`.

`ArenaManager` также умеет:

- вернуть арену по `node_id`;
- определить node текущего CPU через `sched_getcpu`;
- попытаться закрепить текущий поток на CPU заданного node через `sched_setaffinity`.

`numa_memory_resource` использует `ArenaManager` как глобальный координатор: поток спрашивает текущий node, получает соответствующую арену и дальше работает с ней через `ThreadLocalCache`.

### `ThreadLocalCache`

`ThreadLocalCache` - быстрый per-thread слой для small allocations.

На первом allocation/deallocation в потоке создается `thread_local ThreadNumaContextOwner`, который размещает сам `ThreadLocalCache` на текущем NUMA node. Внутри cache хранится:

- `node_id`;
- указатель на `NumaArena` этого node;
- флаг `use_thread_cache`;
- массив bins по small size classes.

Для small allocation cache сначала пытается взять блок из своего bin. Если bin пуст, запрос уходит в `NumaArena`. Для small deallocation блок возвращается в thread-local bin, если:

- thread cache включен;
- это small block;
- block принадлежит тому же `node_id`, что и текущий cache;
- лимит кэша для size class еще не превышен.

Если блок нельзя оставить в thread-local cache, он возвращается в арену.

Такой паттерн уменьшает contention между потоками: повторные small allocations одного потока часто обслуживаются без mutex в `SizeClass`.

### `NumaArena`

`NumaArena` - фасад выделения памяти для одного NUMA node. Внутри она содержит:

- `SmallObjectAllocator` для выделений до `SMALL_LARGE_THRESHOLD`;
- `LargeObjectAllocator` для более крупных выделений или over-aligned выделений;
- foreign freelists для блоков, освобожденных из другого NUMA node.

`NumaArena::allocate(size, alignment)` выбирает стратегию:

- если `size <= 4096` и `alignment <= ALIGNMENT`, используется small allocation path;
- иначе используется large allocation path.

Для small allocations арена выбирает size class, получает блок у `SmallObjectAllocator`, записывает в `BlockHeader` `node_id` и `size_class`, затем возвращает pointer на пользовательскую область.

Для large allocations управление передается `LargeObjectAllocator`, который сам размещает header и user memory внутри span.

`NumaArena::deallocate(ptr)` читает `BlockHeader` перед пользовательским pointer и определяет, куда вернуть память:

- если блок принадлежит другому NUMA node и включен routing foreign deallocations, освобождение перенаправляется в арену-владелец через `ArenaManager`;
- если `size_class != 0`, это small block, он возвращается в `SmallObjectAllocator`;
- если `size_class == 0`, это large block, он возвращается в `LargeObjectAllocator`.

### Small allocations: size classes и slabs

Small allocation path используется для размеров до `SMALL_LARGE_THRESHOLD`, который сейчас равен `4096` bytes.

Размеры округляются до size classes:

- до `256` bytes - шаг `16`;
- до `1024` bytes - шаг `64`;
- до `4096` bytes - шаг `256`.

Эти правила задает `SizeClassConfig`, а `SizeClassTable` предоставляет constexpr mapping из пользовательского размера в class size и индекс класса.

Для каждого size class существует отдельный `SizeClass`, который владеет набором slabs. Slab - это contiguous memory region размером `64 KB`, выделенный через `VirtualMemory::alloc_on_node` на нужном NUMA node.

Внутри slab находится:

- `SlabHeader`;
- выравнивающий padding;
- последовательность блоков одинакового размера;
- free list, построенный прямо внутри свободных пользовательских областей.

Каждый block содержит `BlockHeader` непосредственно перед user pointer. Для small block в header хранится:

- `node_id` - NUMA node владельца;
- `size_class` - размер класса, отличный от `0`;
- `size = 0`;
- `raw_ptr` - указатель на owning slab;
- `total_size = SLAB_SIZE`.

При allocation `SizeClass`:

1. пробует взять блок из текущей slab;
2. если текущая slab заполнена, сканирует существующие slabs;
3. если свободных блоков нет, создает новую slab на нужном NUMA node;
4. возвращает block, из которого пользователь получает pointer после `BlockHeader`.

При deallocation блок возвращается во free list своей slab. Если slab стала полностью пустой, allocator может освободить лишние пустые slabs, но одну пустую slab удерживает для повторного использования.

### Large allocations: span classes и cache

Large allocation path используется для:

- выделений больше `4096` bytes;
- выделений с alignment больше `ALIGNMENT`.

`LargeObjectAllocator` работает не со slabs, а со spans. Он вычисляет полный required size:

```text
requested size + alignment padding + BlockHeader
```

Затем размер округляется до page size и до одного из configured span classes:

```text
8 KB, 12 KB, 16 KB, 24 KB, 32 KB, 48 KB, 64 KB,
96 KB, 128 KB, 192 KB, 256 KB, 384 KB, 512 KB,
768 KB, 1 MB, 2 MB
```

Если размер больше крупнейшего класса, используется page-aligned required size и такой span не попадает в cache при освобождении.

Для ускорения повторных large allocations allocator держит bounded span cache:

- максимум `8` spans на bin;
- общий лимит spans задается `kMaxLargeCachedSpans`;
- общий лимит bytes задается `kMaxLargeCacheBytes`, сейчас `64 MB`;
- при allocation ищется exact bin и ограниченное число ближайших больших bins.

Если подходящий cached span найден, он переиспользуется. Если нет - создается новый mapping через `VirtualMemory::alloc_on_node`.

В `BlockHeader` для large allocation хранится:

- `node_id`;
- `size_class = 0`;
- реальный requested size;
- `raw_ptr` на начало span;
- `total_size` span.

При deallocation span либо возвращается в cache, либо освобождается через `munmap`.

### Foreign deallocation

В многопоточном NUMA-aware allocator важен случай, когда память выделена на одном node, а освобождена из потока, работающего на другом node.

Проект решает это через `BlockHeader::node_id` и routing в `NumaArena::deallocate`.

Если текущая арена получает блок, чей `node_id` отличается от node этой арены, она перенаправляет освобождение в арену-владелец:

```text
deallocate(ptr)
    -> read BlockHeader
    -> header.node_id != current arena node
    -> ArenaManager::arena_for_node(header.node_id)
    -> deallocate_foreign(ptr, header)
```

Для small blocks включен дополнительный механизм foreign freelists. Вместо немедленного взятия mutex основного `SizeClass` каждый foreign small block может быть поставлен в per-class foreign bin арены-владельца. Когда bin превышает high watermark, часть блоков drained обратно в `SmallObjectAllocator`. При allocation slow path арена также может drain foreign bin нужного класса перед созданием новой slab.

Это снижает стоимость cross-thread и cross-node освобождений и помогает вернуть память туда, где она была выделена.

### Синхронизация

Внутренние allocators используют `OptionalMutexLock`: mutex реально берется только если соответствующий объект создан в `sync = true` режиме.

`numa_memory_resource` ориентирован на многопоточное использование: глобальные арены создаются с синхронизацией, а fast path для small allocations дополнительно ускоряется через `ThreadLocalCache`.

`numa_arena_memory_resource` позволяет выбрать `sync` вручную:

- `sync = true` - ресурс можно разделять между потоками;
- `sync = false` - блокировки отключены, такой режим подходит для single-threaded владения и снижает overhead.

`numa_simple_memory_resource` не хранит shared allocator state и ведет себя как тонкий upstream layer: каждое выделение и освобождение идет через системные вызовы, без скрытого кэширования и переиспользования spans.

### Metadata и layout

Каждое пользовательское выделение имеет `BlockHeader` непосредственно перед возвращаемым pointer:

```text
[ BlockHeader ][ user memory ... ]
               ^
               returned pointer
```

Header нужен, чтобы deallocation могла восстановить:

- NUMA node владельца;
- small или large path;
- owning slab или raw span;
- полный размер mapping для `munmap`;
- requested size для large allocations.

Пользователь не взаимодействует с `BlockHeader` напрямую. Он является внутренней частью allocator ABI.

## Стратегии и паттерны

В проекте используются несколько классических allocator strategies.

`Polymorphic memory resource`: публичный API сделан через `std::pmr::memory_resource`. Это позволяет подключать allocator к стандартным PMR-контейнерам без шаблонного распространения конкретного типа аллокатора по всему коду.

`Arena per NUMA node`: память группируется по NUMA node. У каждого node своя `NumaArena`, а внутри нее свои small и large allocators. Это уменьшает смешивание памяти между nodes.

`Thread-local cache`: каждый поток держит маленький cache свободных small blocks. Повторные выделения часто становятся обычным pop из singly-linked list.

`Slab allocation`: small objects одного size class размещаются в slabs фиксированного размера. Это снижает fragmentation и делает allocation/deallocation дешевыми.

`Size classes`: пользовательские размеры округляются до ограниченного набора классов. Это упрощает reuse блоков и позволяет держать отдельные free lists.

`Span cache`: large allocations кэшируют освобожденные spans по классам размеров. Это уменьшает количество `mmap`/`munmap` на повторяющихся крупных размерах.

`Header before payload`: служебная информация хранится непосредственно перед пользовательской памятью, поэтому `deallocate` не зависит от внешней таблицы pointer metadata.

`Foreign free routing`: освобождение из чужого потока или NUMA node направляется обратно в арену-владелец, чтобы память возвращалась в правильную allocator structure.

`Optional synchronization`: один и тот же allocator layer может работать как в thread-safe режиме, так и без mutex overhead для single-threaded сценариев.

## Когда выбирать какой ресурс

`numa_memory_resource` - выбор по умолчанию. Используйте его, если нужна NUMA-aware память для PMR-контейнеров в обычном многопоточном коде. Он дает наиболее полную функциональность: topology detection, per-node arenas, thread-local cache, foreign deallocation routing.

`numa_arena_memory_resource` - выбор для изолированного владения. Используйте его, если нужен отдельный allocator lifetime, явный `node_id`, отключаемая синхронизация или контроль без singleton manager.

`numa_simple_memory_resource` - выбор для базового upstream поведения. Используйте его, если нужно проверить NUMA placement без сложной allocator machinery, получить максимально понятную модель владения memory mappings или иметь простой нижний слой для будущих allocator abstractions.

## Краткая карта исходников

- `src/numa_memory_resource.hpp` - основной публичный класс `numa_memory_resource` и функция `default_numa_memory_resource()`.
- `src/numa_arena_memory_resource.hpp` - самостоятельный PMR-ресурс поверх отдельной `NumaArena`.
- `src/numa_simple_memory_resource.hpp` - простой PMR-ресурс поверх прямых OS mappings.
- `src/arena_manager/` - singleton topology manager и per-node arenas.
- `src/numa_arena/` - фасад small/large allocation для одного NUMA node и foreign deallocation routing.
- `src/thread_local/` - `ThreadLocalCache` и thread-local NUMA context.
- `src/size_divide/small_object/` - slabs, size classes и small allocator.
- `src/size_divide/large_object/` - span classes и large span cache.
- `src/size_divide/block.hpp` - общие constants, configs, `OptionalMutexLock` и `BlockHeader`.
- `src/virtual_memory/` - `mmap`, `mbind`, `munmap` и page alignment.

