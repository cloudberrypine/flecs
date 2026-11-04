# Iteration order fuzzing

`ecs_fuzz_iteration_order(world, seed, flags)` deterministically permutes
unpromised iteration orders between iterations. It returns the number of
eligible lists with at least two elements, including identity permutations.
The argument must be the owning, writable, single-threaded world, not a stage.

`ecs_set_iteration_order_fuzz(world, seed, flags)` enables the same operation
before each `ecs_progress()` frame begins. Its seed includes the progress-call
count. Setting flags to zero disables it with one branch per frame.

## Coverage in 4.2

| Flag | Storage and invariants |
| --- | --- |
| `EcsFuzzQueryCacheTables` | Match arrays within each populated group of every cached query. Each moved table's group index is repaired. Group links/order and queries with `order_by` are untouched. Compiles out without `FLECS_CACHED_QUERIES`. |
| `EcsFuzzComponentIndexTables` | Dense table-cache records for every component, pair and wildcard record. Queryable and disabled/prefab/not-queryable partitions are shuffled separately. An initialized lazy index is repaired, and a small linear cache stays linear. A definition parent's ChildOf cache keeps its order. |
| `EcsFuzzTableRows` | Data-table rows through `ecs_table_swap_rows`, preserving entity/value associations, refs and dirty tracking. Definition tables and their children's tables keep their rows. Parent storage is included, with per-row parent checks for definition children sharing a table. |
| `EcsFuzzOrderedChildren` | Both fragmenting OrderedChildren and non-fragmenting Parent children, including mixed lists. Definition and prefab parents are excluded, preserving prefab child-index correspondence. |
| `EcsFuzzSparseSets` | Dense alive order in every sparse store, including Sparse, DontFragment components, relationship and component-target pairs, and wildcard membership stores. Payloads and sparse addresses stay fixed. Also covers the newly exposed record lists below. |

Definition exclusions retain the specification of fork commit `2e65dd202`:
components, enums, bitmasks, systems, queries, observers, modules, module scope,
builtins, prefabs, and their children. An internal component alone does not
make a table a definition. Sorted per-entity target types remain sorted.

4.2 adds iteration paths through the global non-fragmenting record list
(entity component enumeration and wildcard each), `(R, *)` record lists, and
`(*, T)` record lists. These lists are additionally shuffled under
`EcsFuzzSparseSets`. Both links are repaired, and pair wildcard sentinels stay
at the head. Traversable lookup lists are not changed.

## Validation

Build in Debug with CMake and `FLECS_TESTS=ON`, using a bake checkout via
`BAKE_DIRECTORY`. Run `test_core_flecs_static IterationFuzz` for the focused
suite. The 11 cases check changed and repeatable orders, unchanged entity sets
and values, ref refresh, definition exclusions, cache partitions and indices,
ordered groups, order_by re-sorting, partial and wildcard cached queries,
empty/repopulated groups, removals, deletion and progress enable/disable.
Custom builds with no addons, and with only cached queries, also compile.

The port's debug validation reported:

| Build/run | Core pass/fail | Query pass/fail |
| --- | --- | --- |
| Plain upstream v4.2.0 | 3885 / 0 | 4806 / 0 |
| Fork, normal suites | 3896 / 0 | 4806 / 0 |
| Fork, temporary fuzz-instrumented suites | 3863 / 33 | 4140 / 666 |

The temporary instrumentation fuzzed with seed 42 and `EcsFuzzAll` immediately
before a testcase's first top-level query/each/children iterator. It covered
103 core and 1053 query test functions, excluding prior iterators, readonly
setup and threaded tests. Existing order/batch assertions intentionally fail
under this perturbation, as can exact move/constructor hook counts after row
swaps. The six expected sparse-field assertion logs in query tests were also
present upstream. There were no new internal assertion failures or crashes.
The original quarantines were unchanged (15 core, 2 query).

Upstream 4.2's `each()` can return a component-target DontFragment pair twice,
once through a table cache and once through sparse storage. A plain upstream
probe with eight entities reports two batches of eight. The focused tests
check the entity set and observe the sparse suffix separately for this case.
This port does not change that upstream behavior.
