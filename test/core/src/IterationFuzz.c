#include <core.h>
#include "../../../src/private_api.h"

/* Use public iterators to check observations, and private cache data to check
 * invariants which a set comparison alone would miss. */
#define N (8)

typedef struct FuzzWorld {
    ecs_world_t *world;
    ecs_entity_t component;
    ecs_entity_t entities[N];
    ecs_entity_t tags[N];
} FuzzWorld;

static FuzzWorld fuzz_world(ecs_entity_t trait, bool tables) {
    FuzzWorld f = { .world = ecs_init() };
    f.component = ecs_component_init(f.world, &(ecs_component_desc_t){
        .entity = ecs_entity(f.world, { .name = "FuzzValue" }),
        .type = { .size = sizeof(int), .alignment = ECS_ALIGNOF(int) }
    });
    if (trait) {
        ecs_add_id(f.world, f.component, trait);
    }
    for (int i = 0; i < N; i ++) {
        f.tags[i] = ecs_new(f.world);
        f.entities[i] = ecs_new(f.world);
        ecs_set_id(f.world, f.entities[i], f.component, sizeof(int), &i);
        if (tables) {
            ecs_add_id(f.world, f.entities[i], f.tags[i]);
        }
    }
    return f;
}

static int collect(ecs_iter_t *it, ecs_entity_t *out) {
    int count = 0;
    while (it->next && ecs_iter_next(it)) {
        for (int i = 0; i < it->count; i ++) {
            test_assert(count < N);
            out[count ++] = it->entities[i];
        }
    }
    return count;
}

/* In upstream 4.2, each() can visit a component-target DontFragment pair
 * through both its table cache and sparse store. Check the requested set
 * contract here without assuming that those observations are unique. */
static int collect_unique(ecs_iter_t *it, ecs_entity_t *out) {
    int count = 0;
    while (it->next && ecs_iter_next(it)) {
        for (int i = 0; i < it->count; i ++) {
            bool found = false;
            for (int j = 0; j < count; j ++) found |= out[j] == it->entities[i];
            if (!found) {
                test_assert(count < N);
                out[count ++] = it->entities[i];
            }
        }
    }
    return count;
}

static void same_set(const ecs_entity_t *a, const ecs_entity_t *b, int count) {
    for (int i = 0; i < count; i ++) {
        int found = 0;
        for (int j = 0; j < count; j ++) {
            found += a[i] == b[j];
        }
        test_int(found, 1);
    }
}

static void values(FuzzWorld *f) {
    for (int i = 0; i < N; i ++) {
        const int *v = ecs_get_id(f->world, f->entities[i], f->component);
        test_assert(v != NULL);
        test_int(*v, i);
    }
}

static void cache_invariants(ecs_world_t *world) {
    FLECS_EACH_COMPONENT_RECORD(cr, {
        ecs_table_cache_t *cache = &cr->cache;
        int count = ecs_vec_count(&cache->records);
        for (int i = 0; i < count; i ++) {
            ecs_table_cache_elem_t *elem = ecs_vec_get_t(
                &cache->records, ecs_table_cache_elem_t, i);
            bool queryable = !(elem->table->flags &
                (EcsTableIsPrefab | EcsTableIsDisabled | EcsTableNotQueryable));
            test_bool(queryable, i < cache->queryable_count);
            test_assert(ecs_table_cache_get(cache, elem->table) == elem->tr);
            test_assert(flecs_table_cache_get_elem(cache, elem->table) == elem);
        }
    })
    FLECS_EACH_QUERY(q, {
        ecs_query_cache_t *cache = flecs_query_impl(q)->cache;
        if (cache) {
            for (ecs_query_cache_group_t *g = cache->first_group; g; g = g->next) {
                int count = ecs_vec_count(&g->tables);
                test_assert(count > 0);
                test_int(g->info.table_count, count);
                for (int i = 0; i < count; i ++) {
                    ecs_query_cache_match_t *m = ecs_vec_get(
                        &g->tables, flecs_query_cache_elem_size(cache), i);
                    ecs_query_cache_table_t *t = flecs_query_cache_get_table(
                        cache, m->base.table);
                    test_assert(t != NULL);
                    test_assert(t->group == g);
                    test_int(t->index, i);
                }
            }
        }
    })
}

static void check_kind(ecs_flags32_t kind, ecs_entity_t trait, bool tables) {
    ecs_entity_t results[2][N];
    for (int run = 0; run < 2; run ++) {
        FuzzWorld f = fuzz_world(trait, tables);
        ecs_query_t *q = ecs_query(f.world, {
            .terms = {{ .id = f.component }},
            .cache_kind = trait == EcsDontFragment ? EcsQueryCacheNone : EcsQueryCacheAll
        });
        ecs_entity_t before[N], after[N], each[N];
        ecs_iter_t it = ecs_query_iter(f.world, q);
        test_int(collect(&it, before), N);
        test_assert(ecs_fuzz_iteration_order(f.world, 42, kind) > 0);
        it = ecs_query_iter(f.world, q);
        test_int(collect(&it, after), N);
        if (kind == EcsFuzzComponentIndexTables) {
            /* A cached query retains its own table order. */
            it = ecs_each_id(f.world, f.component);
            test_int(collect(&it, after), N);
        }
        test_assert(memcmp(before, after, sizeof(before)) != 0);
        same_set(before, after, N);
        it = ecs_each_id(f.world, f.component);
        test_int(collect(&it, each), N);
        same_set(before, each, N);
        values(&f);
        cache_invariants(f.world);
        /* Entity ids can have different generations in consecutive worlds.
         * Compare the values they identify, not those ids. */
        for (int i = 0; i < N; i ++) {
            results[run][i] = *(const int*)ecs_get_id(f.world, after[i], f.component);
        }
        ecs_query_fini(q);
        ecs_fini(f.world);
    }
    test_assert(!memcmp(results[0], results[1], sizeof(results[0])));
}

void IterationFuzz_query_cache(void) {
    check_kind(EcsFuzzQueryCacheTables, 0, true);
}

void IterationFuzz_component_index(void) {
    check_kind(EcsFuzzComponentIndexTables, 0, true);
    /* Also exercise lazy (linear) lookup and both cache partitions. */
    FuzzWorld f = fuzz_world(0, false);
    for (int i = 0; i < N; i ++) {
        if (i % 2) ecs_add_id(f.world, f.entities[i], EcsDisabled);
        if (i > 3) ecs_add_id(f.world, f.entities[i], EcsPrefab);
    }
    for (int seed = 1; seed < 10; seed ++) {
        ecs_entity_t before[N], after[N];
        ecs_iter_t it = ecs_each_id(f.world, f.component);
        test_int(collect(&it, before), N);
        ecs_fuzz_iteration_order(f.world, seed, EcsFuzzComponentIndexTables);
        it = ecs_each_id(f.world, f.component);
        test_int(collect(&it, after), N);
        same_set(before, after, N);
        cache_invariants(f.world);
    }
    ecs_fini(f.world);
}

void IterationFuzz_table_rows(void) {
    check_kind(EcsFuzzTableRows, 0, false);
    FuzzWorld f = fuzz_world(0, false);
    ecs_ref_t refs[N];
    for (int i = 0; i < N; i ++) {
        refs[i] = ecs_ref_init_id(f.world, f.entities[i], f.component);
        test_int(*(const int*)ecs_ref_get_id(f.world, &refs[i], f.component), i);
    }
    ecs_fuzz_iteration_order(f.world, 42, EcsFuzzTableRows);
    for (int i = 0; i < N; i ++) {
        test_int(*(const int*)ecs_ref_get_id(f.world, &refs[i], f.component), i);
    }
    ecs_fini(f.world);
}

void IterationFuzz_sparse(void) {
    /* Sparse queries normally use table rows. DontFragment queries use the
     * sparse dense array. Check Sparse's array independently below. */
    check_kind(EcsFuzzSparseSets, EcsDontFragment, false);
    FuzzWorld f = fuzz_world(EcsSparse, false);
    ecs_sparse_t *s = flecs_components_get(f.world, f.component)->sparse;
    ecs_entity_t before[N];
    ecs_os_memcpy_n(before, flecs_sparse_ids(s), ecs_entity_t, N);
    ecs_fuzz_iteration_order(f.world, 42, EcsFuzzSparseSets);
    test_assert(memcmp(before, flecs_sparse_ids(s), sizeof(before)) != 0);
    same_set(before, flecs_sparse_ids(s), N);
    values(&f);
    for (int i = 0; i < N; i ++) {
        ecs_delete(f.world, f.entities[i]);
    }
    test_int(flecs_sparse_count(s), 0);
    ecs_fini(f.world);
}

void IterationFuzz_children(void) {
    for (int mode = 0; mode < 3; mode ++) {
        ecs_entity_t order[2][N];
        for (int run = 0; run < 2; run ++) {
            FuzzWorld f = fuzz_world(0, false);
            ecs_entity_t parent = ecs_new(f.world);
            ecs_add_id(f.world, parent, EcsOrderedChildren);
            for (int i = 0; i < N; i ++) {
                if (mode == 1 || (mode == 2 && i % 2)) {
                    ecs_set(f.world, f.entities[i], EcsParent, {parent});
                } else {
                    ecs_add_pair(f.world, f.entities[i], EcsChildOf, parent);
                }
            }
            ecs_entity_t before[N], after[N];
            ecs_iter_t it = ecs_children(f.world, parent);
            test_int(collect(&it, before), N);
            ecs_fuzz_iteration_order(f.world, 42, EcsFuzzOrderedChildren);
            it = ecs_children(f.world, parent);
            test_int(collect(&it, after), N);
            test_assert(memcmp(before, after, sizeof(before)) != 0);
            same_set(before, after, N);
            values(&f);
            for (int i = 0; i < N; i ++) {
                order[run][i] = *(const int*)ecs_get_id(f.world, after[i], f.component);
            }
            /* Parent tables were accidentally excluded in the old fork. */
            if (mode == 1) {
                ecs_table_t *t = ecs_get_table(f.world, f.entities[0]);
                ecs_entity_t rows[N];
                ecs_os_memcpy_n(rows, ecs_table_entities(t), ecs_entity_t, N);
                ecs_fuzz_iteration_order(f.world, 42, EcsFuzzTableRows);
                test_assert(memcmp(rows, ecs_table_entities(t), sizeof(rows)) != 0);
                values(&f);
            }
            ecs_delete(f.world, f.entities[0]);
            it = ecs_children(f.world, parent);
            test_int(collect(&it, after), N - 1);
            cache_invariants(f.world);
            ecs_fini(f.world);
        }
        test_assert(!memcmp(order[0], order[1], sizeof(order[0])));
    }
}

void IterationFuzz_non_fragmenting_pairs(void) {
    /* Relationship and target DontFragment, exclusive and multi-target, all
     * use sparse stores in 4.2, including the wildcard membership stores. */
    for (int mode = 0; mode < 3; mode ++) {
        ecs_entity_t orders[2][N];
        for (int run = 0; run < 2; run ++) {
            FuzzWorld f = fuzz_world(0, false);
            ecs_entity_t rel = ecs_new(f.world), target = mode == 2 ? ecs_component(f.world, {
                .entity = ecs_entity(f.world, { .name = "FuzzTarget" }),
                .type = { .size = sizeof(int), .alignment = ECS_ALIGNOF(int) }
            }) : ecs_new(f.world);
            ecs_add_id(f.world, mode == 2 ? target : rel, EcsDontFragment);
            if (mode == 1) ecs_add_id(f.world, rel, EcsExclusive);
            ecs_id_t pair = ecs_pair(rel, target);
            for (int i = 0; i < N; i ++) ecs_add_id(f.world, f.entities[i], pair);
            ecs_query_t *q = ecs_query(f.world, {
                .terms = {{ .id = pair }}, .cache_kind = EcsQueryCacheNone
            });
            ecs_entity_t before[N], after[N];
            ecs_iter_t it = ecs_query_iter(f.world, q);
            test_int(collect(&it, before), N);
            ecs_fuzz_iteration_order(f.world, 42, EcsFuzzSparseSets);
            it = ecs_query_iter(f.world, q);
            test_int(collect(&it, after), N);
            test_assert(memcmp(before, after, sizeof(before)) != 0);
            same_set(before, after, N);
            it = ecs_each_id(f.world, pair);
            test_int(collect_unique(&it, after), N);
            same_set(before, after, N);
            it = ecs_children_w_rel(f.world, rel, target);
            test_int(collect_unique(&it, after), N);
            same_set(before, after, N);
            for (int i = 0; i < N; i ++) {
                orders[run][i] = *(const int*)ecs_get_id(f.world, after[i], f.component);
                ecs_remove_id(f.world, f.entities[i], pair);
            }
            it = ecs_query_iter(f.world, q);
            test_int(collect(&it, after), 0);
            ecs_query_fini(q);
            values(&f);
            ecs_fini(f.world);
        }
        test_assert(!memcmp(orders[0], orders[1], sizeof(orders[0])));
    }
}

static int collect_data(FuzzWorld *f, ecs_iter_t *it, ecs_entity_t *out, bool sparse_only) {
    int count = 0;
    while (it->next && ecs_iter_next(it)) {
        if (sparse_only && it->table) continue;
        for (int i = 0; i < it->count; i ++) {
            bool found = false;
            for (int j = 0; j < count; j ++) found |= out[j] == it->entities[i];
            if (!found && ecs_has_id(f->world, it->entities[i], f->component)) {
                test_assert(count < N);
                out[count ++] = it->entities[i];
            }
        }
    }
    return count;
}

void IterationFuzz_non_fragmenting_lists(void) {
    for (int mode = 0; mode < 3; mode ++) {
        ecs_entity_t orders[2][N];
        for (int run = 0; run < 2; run ++) {
            FuzzWorld f = fuzz_world(0, false);
            ecs_entity_t rel = ecs_new(f.world), target = ecs_component(f.world, {
                .entity = ecs_entity(f.world, { .name = "FuzzTarget" }),
                .type = { .size = sizeof(int), .alignment = ECS_ALIGNOF(int) }
            });
            ecs_add_id(f.world, rel, EcsDontFragment);
            ecs_add_id(f.world, target, EcsDontFragment);
            ecs_id_t pattern = mode == 0 ? ecs_pair(EcsWildcard, EcsWildcard)
                : mode == 1 ? ecs_pair(rel, EcsWildcard) : ecs_pair(EcsWildcard, target);
            for (int i = 0; i < N; i ++) {
                ecs_id_t pair = mode == 2 ? ecs_pair(f.tags[i], target)
                    : ecs_pair(rel, f.tags[i]);
                ecs_add_id(f.world, f.entities[i], pair);
            }
            ecs_entity_t before[N], after[N];
            /* Inspect the sparse part of each(): target-component pairs can
             * also appear in its table-cache prefix in upstream 4.2. */
            ecs_iter_t it = ecs_each_id(f.world, pattern);
            test_int(collect_data(&f, &it, before, true), N);
            ecs_fuzz_iteration_order(f.world, 42, EcsFuzzSparseSets);
            it = ecs_each_id(f.world, pattern);
            test_int(collect_data(&f, &it, after, true), N);
            test_assert(memcmp(before, after, sizeof(before)) != 0);
            same_set(before, after, N);
            for (int i = 0; i < N; i ++) {
                orders[run][i] = *(const int*)ecs_get_id(f.world, after[i], f.component);
            }
            /* Removing records exercises the repaired prev/next links. */
            for (int i = 0; i < N; i ++) ecs_delete(f.world, f.entities[i]);
            it = ecs_each_id(f.world, pattern);
            test_int(collect_data(&f, &it, after, true), 0);
            ecs_fini(f.world);
        }
        test_assert(!memcmp(orders[0], orders[1], sizeof(orders[0])));
    }
}

static uint64_t fuzz_group(ecs_world_t *world, ecs_table_t *table,
    ecs_id_t id, void *ctx)
{
    return ecs_table_has_id(world, table, id);
}

static int fuzz_compare(ecs_entity_t e1, const void *p1,
    ecs_entity_t e2, const void *p2)
{
    return (*(const int*)p1 > *(const int*)p2) -
        (*(const int*)p1 < *(const int*)p2);
}

void IterationFuzz_cached_mutation(void) {
    for (int mode = 0; mode < 4; mode ++) {
        FuzzWorld f = fuzz_world(0, true);
        ecs_entity_t rel = ecs_new(f.world);
        ecs_entity_t marker = ecs_new(f.world);
        if (mode == 3) ecs_add_id(f.world, marker, EcsDontFragment);
        for (int i = 0; i < N; i ++) {
            ecs_add_pair(f.world, f.entities[i], rel, f.tags[0]);
            ecs_add_pair(f.world, f.entities[i], rel, f.tags[1]);
            if (i % 2) ecs_add_id(f.world, f.entities[i], f.tags[0]);
            if (mode == 3) ecs_add_id(f.world, f.entities[i], marker);
        }
        ecs_query_t *q = ecs_query(f.world, {
            .terms = {{ .id = f.component }, {
                .id = mode == 2 ? ecs_pair(rel, EcsWildcard) : mode == 3 ? marker : f.tags[0],
                .oper = mode == 2 ? EcsAnd : EcsOptional }},
            .cache_kind = mode == 3 ? EcsQueryCacheAuto : EcsQueryCacheAll,
            .group_by = mode == 1 ? f.tags[0] : 0,
            .group_by_callback = mode == 1 ? fuzz_group : NULL,
            .flags = EcsQueryGroupByOrdered
        });
        test_assert(q != NULL);
        test_assert(flecs_query_impl(q)->cache != NULL);
        if (mode == 3) test_int(q->cache_kind, EcsQueryCacheAuto);
        for (int seed = 1; seed <= 8; seed ++) {
            ecs_fuzz_iteration_order(f.world, seed, EcsFuzzAll);
            cache_invariants(f.world);
            ecs_iter_t it = ecs_query_iter(f.world, q);
            int count = 0, seen[N] = {0};
            uint64_t prev_group = 0;
            while (ecs_query_next(&it)) {
                test_assert(ecs_iter_get_group(&it) >= prev_group);
                prev_group = ecs_iter_get_group(&it);
                for (int i = 0; i < it.count; i ++) {
                    const int *v = &ecs_field(&it, int, 0)[i];
                    test_assert(v != NULL);
                    test_assert(*v >= 0 && *v < N);
                    test_int(it.entities[i], f.entities[*v]);
                    seen[*v] ++;
                    count ++;
                }
            }
            test_int(count, N * (mode == 2 ? 2 : 1));
            for (int i = 0; i < N; i ++) test_int(seen[i], mode == 2 ? 2 : 1);
            for (int i = 0; i < N; i ++) {
                ecs_remove_id(f.world, f.entities[i], f.tags[i]);
                ecs_add_id(f.world, f.entities[i], f.tags[i]);
                /* Empty and repopulate cached tables and groups. */
                ecs_remove_id(f.world, f.entities[i], f.component);
                ecs_set_id(f.world, f.entities[i], f.component, sizeof(int), &i);
            }
            values(&f);
        }
        for (int i = 0; i < N; i ++) {
            ecs_delete(f.world, f.entities[i]);
            cache_invariants(f.world);
        }
        ecs_iter_t it = ecs_query_iter(f.world, q);
        test_assert(!ecs_query_next(&it));
        ecs_query_fini(q);
        ecs_fini(f.world);
    }
}

void IterationFuzz_order_by(void) {
    FuzzWorld f = fuzz_world(0, true);
    /* Multiple rows per table must be sorted again after row swaps. */
    for (int i = 0; i < N; i ++) {
        ecs_remove_id(f.world, f.entities[i], f.tags[i]);
        if (i % 2) ecs_add_id(f.world, f.entities[i], f.tags[0]);
    }
    ecs_query_t *q = ecs_query(f.world, {
        .terms = {{ .id = f.component }},
        .cache_kind = EcsQueryCacheAuto,
        .order_by = f.component, .order_by_callback = fuzz_compare
    });
    for (int seed = 1; seed < 10; seed ++) {
        ecs_fuzz_iteration_order(f.world, seed, EcsFuzzAll);
        ecs_iter_t it = ecs_query_iter(f.world, q);
        ecs_entity_t order[N];
        test_int(collect(&it, order), N);
        test_assert(!memcmp(f.entities, order, sizeof(order)));
        cache_invariants(f.world);
    }
    ecs_query_fini(q);
    ecs_fini(f.world);
}

void IterationFuzz_definitions(void) {
    FuzzWorld f = fuzz_world(0, false);
    ecs_entity_t parents[] = {f.component, ecs_new(f.world), ecs_new(f.world),
        ecs_new(f.world), ecs_id(EcsEnum), ecs_id(EcsBitmask)};
    ecs_add_id(f.world, parents[1], EcsPrefab);
    ecs_add_id(f.world, parents[2], EcsModule);
    /* Use a query's Poly table. */
    ecs_query_t *q = ecs_query(f.world, { .terms = {{ .id = f.component }},
        .cache_kind = EcsQueryCacheAll });
    parents[3] = q->entity;
    for (int p = 0; p < 6; p ++) {
        for (int non_fragmenting = 0; non_fragmenting < 2; non_fragmenting ++) {
            ecs_entity_t children[N];
            for (int i = 0; i < N; i ++) {
                children[i] = ecs_new(f.world);
                ecs_set_id(f.world, children[i], f.component, sizeof(int), &i);
                if (non_fragmenting) ecs_set(f.world, children[i], EcsParent, {parents[p]});
                else ecs_add_pair(f.world, children[i], EcsChildOf, parents[p]);
            }
            ecs_entity_t before[N], after[N];
            ecs_iter_t it = ecs_children(f.world, parents[p]);
            test_int(collect_data(&f, &it, before, false), N);
            for (int seed = 1; seed < 10; seed ++) {
                ecs_fuzz_iteration_order(f.world, seed, EcsFuzzAll);
                it = ecs_children(f.world, parents[p]);
                test_int(collect_data(&f, &it, after, false), N);
                test_assert(!memcmp(before, after, sizeof(before)));
            }
            for (int i = 0; i < N; i ++) ecs_delete(f.world, children[i]);
        }
    }
    /* Component definitions and prefab rows retain their physical row order. */
    ecs_table_t *tables[] = {ecs_get_table(f.world, f.component),
        ecs_get_table(f.world, parents[1]), ecs_get_table(f.world, parents[2]),
        ecs_get_table(f.world, parents[3])};
    for (int t = 0; t < 4; t ++) {
        int count = ecs_table_count(tables[t]);
        ecs_entity_t *before = ecs_os_memdup(ecs_table_entities(tables[t]),
            count * sizeof(ecs_entity_t));
        ecs_fuzz_iteration_order(f.world, 42, EcsFuzzAll);
        test_assert(!memcmp(before, ecs_table_entities(tables[t]), count * sizeof(ecs_entity_t)));
        ecs_os_free(before);
    }
    ecs_query_fini(q);
    ecs_fini(f.world);
}

void IterationFuzz_progress(void) {
    int orders[2][4][N];
    for (int run = 0; run < 2; run ++) {
        FuzzWorld f = fuzz_world(0, false);
        ecs_set_iteration_order_fuzz(f.world, 42, EcsFuzzTableRows);
        for (int frame = 0; frame < 4; frame ++) {
            test_assert(ecs_progress(f.world, 0.01));
            ecs_entity_t entities[N];
            ecs_iter_t it = ecs_each_id(f.world, f.component);
            test_int(collect(&it, entities), N);
            for (int i = 0; i < N; i ++) {
                orders[run][frame][i] = *(const int*)ecs_get_id(
                    f.world, entities[i], f.component);
            }
        }
        test_assert(memcmp(orders[run][0], orders[run][1], sizeof(orders[run][0])));
        ecs_entity_t before[N], after[N];
        ecs_iter_t it = ecs_each_id(f.world, f.component);
        test_int(collect(&it, before), N);
        ecs_set_iteration_order_fuzz(f.world, 42, 0);
        for (int i = 0; i < 4; i ++) ecs_progress(f.world, 0.01);
        it = ecs_each_id(f.world, f.component);
        test_int(collect(&it, after), N);
        test_assert(!memcmp(before, after, sizeof(before)));
        test_int(ecs_fuzz_iteration_order(f.world, 42, 0), 0);
        values(&f);
        ecs_fini(f.world);
    }
    test_assert(!memcmp(orders[0], orders[1], sizeof(orders[0])));
}
