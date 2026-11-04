/**
 * @file iteration_fuzz.c
 * @brief Iteration order fuzzing.
 *
 * Perturbs the orders Flecs does not promise: the order of the tables in a
 * cached query and in the component index, the order of the rows in a table,
 * the order of a parent's ordered children and the dense order of sparse
 * storage. Another history of the same world (other creations and deletions,
 * or the world loaded from a save) can have any of these orders, so an
 * application whose results change under the fuzzer depends on its history.
 *
 * Nothing here changes a component value or the structure of the world, and it
 * runs only where nothing iterates: from ecs_progress() before the frame
 * begins, or from the application between frames.
 */

#include "private_api.h"

typedef struct ecs_fuzz_rng_t {
    uint64_t state;
    int32_t lists; /* Lists of two or more shuffled */
} ecs_fuzz_rng_t;

/* splitmix64 */
static
uint64_t flecs_fuzz_next(
    ecs_fuzz_rng_t *rng)
{
    uint64_t z = (rng->state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static
int32_t flecs_fuzz_below(
    ecs_fuzz_rng_t *rng,
    int32_t bound)
{
    return (int32_t)(flecs_fuzz_next(rng) % (uint64_t)bound);
}

/* Whether a table holds definitions rather than data: components and types
 * (an enum's constants are its ordered children, in declaration order),
 * systems, queries and observers, modules and what they scope, builtins and
 * prefabs. Their order is fixed by the application's code, not by the
 * world's history, so it is left alone. (EcsTableHasModule is no test: a
 * table with any internal component has it, the Parent component's too.) */
static
bool flecs_fuzz_table_is_definition(
    ecs_world_t *world,
    const ecs_table_t *table)
{
    if (table->flags & (EcsTableHasBuiltins | EcsTableIsPrefab | EcsTableNotQueryable)) {
        return true;
    }
    if (ecs_table_has_id(world, table, ecs_id(EcsComponent)) ||
        ecs_table_has_id(world, table, EcsModule) ||
        ecs_search(world, table, ecs_pair(ecs_id(EcsPoly), EcsWildcard), NULL) != -1)
    {
        return true;
    }
#ifdef FLECS_META
    if (ecs_table_has_id(world, table, ecs_id(EcsEnum)) ||
        ecs_table_has_id(world, table, ecs_id(EcsBitmask)))
    {
        return true;
    }
#endif
    if ((table->flags & EcsTableHasChildOf) && table->childof_index >= 0) {
        ecs_entity_t parent = ecs_get_alive(world,
            ECS_PAIR_SECOND(table->type.array[table->childof_index]));
        if (parent && ecs_has_id(world, parent, EcsModule)) {
            return true;
        }
    }
    return false;
}

/* Whether an entity is a definition (flecs_fuzz_table_is_definition). */
static
bool flecs_fuzz_is_definition(
    ecs_world_t *world,
    ecs_entity_t entity)
{
    ecs_entity_t alive = ecs_get_alive(world, entity);
    if (!alive) {
        return true;
    }
    const ecs_table_t *table = ecs_get_table(world, alive);
    return table && flecs_fuzz_table_is_definition(world, table);
}

#ifdef FLECS_CACHED_QUERIES
/* Table order within each group of a cached query. A query that sorts
 * (order_by, which pipelines use) is left alone, and so is the group order
 * (group_by and cascade decide it). The cache finds a table's match by index,
 * so every moved match's index is set again. */
static
void flecs_fuzz_query_cache(
    ecs_query_cache_t *cache,
    ecs_fuzz_rng_t *rng,
    void *tmp)
{
    if (cache->order_by_callback) {
        return;
    }

    ecs_size_t elem_size = flecs_query_cache_elem_size(cache);
    ecs_query_cache_group_t *group = cache->first_group;
    for (; group; group = group->next) {
        int32_t i, count = ecs_vec_count(&group->tables);
        if (count < 2) {
            continue;
        }
        rng->lists ++;

        for (i = count - 1; i > 0; i --) {
            int32_t j = flecs_fuzz_below(rng, i + 1);
            if (i == j) {
                continue;
            }
            void *a = ecs_vec_get(&group->tables, elem_size, i);
            void *b = ecs_vec_get(&group->tables, elem_size, j);
            ecs_os_memcpy(tmp, a, elem_size);
            ecs_os_memcpy(a, b, elem_size);
            ecs_os_memcpy(b, tmp, elem_size);
        }

        for (i = 0; i < count; i ++) {
            ecs_query_cache_match_t *qm = ecs_vec_get(
                &group->tables, elem_size, i);
            ecs_query_cache_table_t *qt = flecs_query_cache_get_table(
                cache, qm->base.table);
            ecs_assert(qt != NULL, ECS_INTERNAL_ERROR, NULL);
            ecs_assert(qt->group == group, ECS_INTERNAL_ERROR, NULL);
            qt->index = i;
        }
    }
}

static
void flecs_fuzz_query_caches(
    ecs_world_t *world,
    ecs_fuzz_rng_t *rng)
{
    ecs_size_t max_size = ECS_MAX(ECS_SIZEOF(ecs_query_cache_match_t),
        ECS_SIZEOF(ecs_query_triv_cache_match_t));
    void *tmp = ecs_os_alloca(max_size);

    FLECS_EACH_QUERY(query, {
        ecs_query_impl_t *impl = flecs_query_impl(query);
        if (impl->cache) {
            flecs_fuzz_query_cache(impl->cache, rng, tmp);
        }
    })
}

#endif

/* Table order of a component record's table cache: the order uncached queries
 * and each() meet tables in. */
static
void flecs_fuzz_table_cache(
    ecs_table_cache_t *cache,
    ecs_fuzz_rng_t *rng)
{
    ecs_table_cache_elem_t *records = ecs_vec_first_t(
        &cache->records, ecs_table_cache_elem_t);
    int32_t count = ecs_vec_count(&cache->records);

    /* 4.2 keeps disabled, prefab and not-queryable tables at the end. Both
     * partitions have unpromised order, but cannot be mixed. Small caches
     * have no index yet, and continue to use linear lookup. */
    int32_t start = 0;
    for (int32_t part = 0; part < 2; part ++) {
        int32_t end = part ? count : cache->queryable_count;
        rng->lists += (end - start) > 1;
        for (int32_t i = end - 1; i > start; i --) {
            int32_t j = start + flecs_fuzz_below(rng, i - start + 1);
            ecs_table_cache_elem_t tmp = records[i];
            records[i] = records[j];
            records[j] = tmp;
        }
        start = end;
    }

    if (ecs_map_is_init(&cache->index)) {
        for (int32_t i = 0; i < count; i ++) {
            ecs_map_val_t *index = ecs_map_get(&cache->index, records[i].table->id);
            ecs_assert(index != NULL, ECS_INTERNAL_ERROR, NULL);
            *index = flecs_ito(uint64_t, i);
        }
    }
}

static
void flecs_fuzz_component_index(
    ecs_world_t *world,
    ecs_fuzz_rng_t *rng)
{
    FLECS_EACH_COMPONENT_RECORD(cr, {
        /* The tables of a definition's children (an enum's constants) keep
         * their order, as the children do (flecs_fuzz_children). */
        if (!(ECS_IS_PAIR(cr->id) && ECS_PAIR_FIRST(cr->id) == EcsChildOf &&
            flecs_fuzz_is_definition(world, ECS_PAIR_SECOND(cr->id))))
        {
            flecs_fuzz_table_cache(&cr->cache, rng);
        }
    })
}

/* Row order within a table. Definitions (flecs_fuzz_table_is_definition)
 * and their children keep theirs. */
static
void flecs_fuzz_table_rows(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_fuzz_rng_t *rng)
{
    if (ecs_table_count(table) < 2) {
        return;
    }
    /* Definitions and their children (an enum's constants) keep their rows:
     * their order is the code's. */
    if (flecs_fuzz_table_is_definition(world, table)) {
        return;
    }
    if ((table->flags & EcsTableHasChildOf) && table->childof_index >= 0) {
        const ecs_id_t childof = table->type.array[table->childof_index];
        if (flecs_fuzz_is_definition(world, ECS_PAIR_SECOND(childof))) {
            return;
        }
    }

    if (table->flags & EcsTableHasParent) {
        const ecs_entity_t *entities = ecs_table_entities(table);
        int32_t count = ecs_table_count(table);
        for (int32_t i = 0; i < count; i ++) {
            if (flecs_fuzz_is_definition(world, ecs_get_parent(world, entities[i]))) {
                return;
            }
        }
    }

    int32_t i, count = ecs_table_count(table);
    rng->lists += count > 1;
    for (i = count - 1; i > 0; i --) {
        int32_t j = flecs_fuzz_below(rng, i + 1);
        if (i != j) {
            ecs_table_swap_rows(world, table, i, j);
        }
    }
}

static
void flecs_fuzz_rows(
    ecs_world_t *world,
    ecs_fuzz_rng_t *rng)
{
    int32_t i, count = flecs_sparse_count(&world->store.tables);
    for (i = 0; i < count; i ++) {
        ecs_table_t *table = flecs_sparse_get_dense_t(
            &world->store.tables, ecs_table_t, i);
        flecs_fuzz_table_rows(world, table, rng);
    }
}

/* A parent's ordered children (the Parent component's children, and those of
 * a parent with the OrderedChildren trait). A prefab's children keep their
 * order (instances find their children by it), and so do the children of a
 * definition (flecs_fuzz_is_definition). */
static
void flecs_fuzz_children(
    ecs_world_t *world,
    ecs_fuzz_rng_t *rng)
{
    FLECS_EACH_COMPONENT_RECORD(cr, {
        ecs_pair_record_t *pr = cr->pair;
        if (pr && !(cr->flags & EcsIdPrefabChildren) &&
            ECS_IS_PAIR(cr->id) && ECS_PAIR_FIRST(cr->id) == EcsChildOf &&
            !flecs_fuzz_is_definition(world, ECS_PAIR_SECOND(cr->id)))
        {
            int32_t i, count = ecs_vec_count(&pr->ordered_children);
            rng->lists += count > 1;
            ecs_entity_t *children = ecs_vec_first_t(
                &pr->ordered_children, ecs_entity_t);
            for (i = count - 1; i > 0; i --) {
                int32_t j = flecs_fuzz_below(rng, i + 1);
                ecs_entity_t t = children[i];
                children[i] = children[j];
                children[j] = t;
            }
        }
    })
}

/* The dense order of sparse component storage (Sparse and DontFragment
 * components and pairs): the order a query over them meets entities in. */
static
void flecs_fuzz_sparse_sets(
    ecs_world_t *world,
    ecs_fuzz_rng_t *rng)
{
    FLECS_EACH_COMPONENT_RECORD(cr, {
        ecs_sparse_t *sparse = cr->sparse;
        if (sparse) {
            int32_t i, count = flecs_sparse_count(sparse);
            rng->lists += count > 1;
            for (i = count - 1; i > 0; i --) {
                int32_t j = flecs_fuzz_below(rng, i + 1);
                flecs_sparse_swap_alive(sparse, i, j);
            }
        }
    })
}

/* 4.2 each() can enumerate non-fragmenting ids directly, through the global
 * list or either pair-wildcard list. Their order is insertion history too.
 * Pair lists keep the wildcard record at their head. The trav list and the
 * sorted per-entity target types are lookup structures, not these iterators. */
static ecs_id_record_elem_t* flecs_fuzz_record_link(
    ecs_component_record_t *cr,
    int32_t kind)
{
    if (!kind) {
        return &cr->non_fragmenting;
    }
    return kind == 1 ? &cr->pair->first : &cr->pair->second;
}

static void flecs_fuzz_record_list(
    ecs_component_record_t **head,
    ecs_component_record_t *sentinel,
    int32_t kind,
    ecs_vec_t *scratch,
    ecs_fuzz_rng_t *rng)
{
    ecs_vec_clear(scratch);
    for (ecs_component_record_t *cr = *head; cr;
        cr = flecs_fuzz_record_link(cr, kind)->next)
    {
        ecs_vec_append_t(NULL, scratch, ecs_component_record_t*)[0] = cr;
    }
    int32_t count = ecs_vec_count(scratch);
    if (count < 2) {
        return;
    }
    rng->lists ++;
    ecs_component_record_t **records = ecs_vec_first_t(
        scratch, ecs_component_record_t*);
    for (int32_t i = count - 1; i > 0; i --) {
        int32_t j = flecs_fuzz_below(rng, i + 1);
        ecs_component_record_t *tmp = records[i];
        records[i] = records[j];
        records[j] = tmp;
    }
    for (int32_t i = 0; i < count; i ++) {
        ecs_id_record_elem_t *link = flecs_fuzz_record_link(records[i], kind);
        link->prev = i ? records[i - 1] : sentinel;
        link->next = i + 1 < count ? records[i + 1] : NULL;
    }
    *head = records[0];
}

static void flecs_fuzz_non_fragmenting_lists(
    ecs_world_t *world,
    ecs_fuzz_rng_t *rng)
{
    ecs_vec_t scratch;
    ecs_vec_init_t(NULL, &scratch, ecs_component_record_t*, 0);
    flecs_fuzz_record_list(&world->cr_non_fragmenting_head, NULL, 0,
        &scratch, rng);
    FLECS_EACH_COMPONENT_RECORD(cr, {
        if (cr->pair && ecs_id_is_wildcard(cr->id)) {
            if (ECS_PAIR_SECOND(cr->id) == EcsWildcard &&
                (cr->flags & EcsIdDontFragment))
            {
                flecs_fuzz_record_list(&cr->pair->first.next, cr, 1,
                    &scratch, rng);
            } else if (ECS_PAIR_FIRST(cr->id) == EcsWildcard &&
                (cr->flags & EcsIdMatchDontFragment))
            {
                flecs_fuzz_record_list(&cr->pair->second.next, cr, 2,
                    &scratch, rng);
            }
        }
    })
    ecs_vec_fini_t(NULL, &scratch, ecs_component_record_t*);
}

int32_t ecs_fuzz_iteration_order(
    ecs_world_t *world,
    uint64_t seed,
    ecs_flags32_t flags)
{
    flecs_poly_assert(world, ecs_world_t);
    ecs_check(!(world->flags & EcsWorldReadonly), ECS_INVALID_OPERATION,
        "cannot fuzz iteration order while the world is readonly");
    ecs_check(!(world->flags & EcsWorldMultiThreaded), ECS_INVALID_OPERATION,
        "cannot fuzz iteration order while the world is multithreaded");
    flecs_check_exclusive_world_access_write(world);

    ecs_fuzz_rng_t rng = { .state = seed };
    flecs_fuzz_next(&rng);

#ifdef FLECS_CACHED_QUERIES
    if (flags & EcsFuzzQueryCacheTables) {
        flecs_fuzz_query_caches(world, &rng);
    }
#endif
    if (flags & EcsFuzzComponentIndexTables) {
        flecs_fuzz_component_index(world, &rng);
    }
    if (flags & EcsFuzzTableRows) {
        flecs_fuzz_rows(world, &rng);
    }
    if (flags & EcsFuzzOrderedChildren) {
        flecs_fuzz_children(world, &rng);
    }
    if (flags & EcsFuzzSparseSets) {
        flecs_fuzz_sparse_sets(world, &rng);
        flecs_fuzz_non_fragmenting_lists(world, &rng);
    }
    return rng.lists;
error:
    return 0;
}

void ecs_set_iteration_order_fuzz(
    ecs_world_t *world,
    uint64_t seed,
    ecs_flags32_t flags)
{
    flecs_poly_assert(world, ecs_world_t);
    world->iteration_fuzz_seed = seed;
    world->iteration_fuzz_flags = flags & EcsFuzzAll;
    world->iteration_fuzz_count = 0;
}

void flecs_fuzz_iteration_order_frame(
    ecs_world_t *world)
{
    if (world->flags & EcsWorldReadonly) {
        return;
    }

    ecs_fuzz_rng_t rng = { .state = world->iteration_fuzz_seed };
    rng.state ^= flecs_fuzz_next(&rng) + world->iteration_fuzz_count ++;
    ecs_fuzz_iteration_order(world, flecs_fuzz_next(&rng),
        world->iteration_fuzz_flags);
}
