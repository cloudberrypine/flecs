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

/* Table order of a component record's table cache: the order uncached queries
 * and each() meet tables in. */
static
void flecs_fuzz_table_cache(
    ecs_table_cache_t *cache,
    ecs_fuzz_rng_t *rng,
    ecs_vec_t *scratch)
{
    int32_t i, count = cache->tables.count;
    if (count < 2) {
        return;
    }
    rng->lists ++;

    ecs_vec_set_count_t(NULL, scratch, ecs_table_cache_hdr_t*, count);
    ecs_table_cache_hdr_t **elems = ecs_vec_first_t(
        scratch, ecs_table_cache_hdr_t*);

    ecs_table_cache_hdr_t *cur = cache->tables.first;
    for (i = 0; i < count; i ++) {
        ecs_assert(cur != NULL, ECS_INTERNAL_ERROR, NULL);
        elems[i] = cur;
        cur = cur->next;
    }
    ecs_assert(cur == NULL, ECS_INTERNAL_ERROR, NULL);

    for (i = count - 1; i > 0; i --) {
        int32_t j = flecs_fuzz_below(rng, i + 1);
        ecs_table_cache_hdr_t *t = elems[i];
        elems[i] = elems[j];
        elems[j] = t;
    }

    for (i = 0; i < count; i ++) {
        elems[i]->prev = i ? elems[i - 1] : NULL;
        elems[i]->next = (i < count - 1) ? elems[i + 1] : NULL;
    }
    cache->tables.first = elems[0];
    cache->tables.last = elems[count - 1];
}

static
void flecs_fuzz_component_index(
    ecs_world_t *world,
    ecs_fuzz_rng_t *rng)
{
    ecs_vec_t scratch;
    ecs_vec_init_t(NULL, &scratch, ecs_table_cache_hdr_t*, 0);
    FLECS_EACH_COMPONENT_RECORD(cr, {
        /* The tables of a definition's children (an enum's constants) keep
         * their order, as the children do (flecs_fuzz_children). */
        if (!(ECS_IS_PAIR(cr->id) && ECS_PAIR_FIRST(cr->id) == EcsChildOf &&
            flecs_fuzz_is_definition(world, ECS_PAIR_SECOND(cr->id))))
        {
            flecs_fuzz_table_cache(&cr->cache, rng, &scratch);
        }
    })
    ecs_vec_fini_t(NULL, &scratch, ecs_table_cache_hdr_t*);
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
    ecs_check(!ecs_is_deferred(world), ECS_INVALID_OPERATION,
        "cannot fuzz iteration order while the world is deferred");
    flecs_check_exclusive_world_access_write(world);

    ecs_fuzz_rng_t rng = { .state = seed };
    flecs_fuzz_next(&rng);

    if (flags & EcsFuzzQueryCacheTables) {
        flecs_fuzz_query_caches(world, &rng);
    }
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
    if (ecs_is_deferred(world) || (world->flags & EcsWorldReadonly)) {
        return;
    }

    ecs_fuzz_rng_t rng = { .state = world->iteration_fuzz_seed };
    rng.state ^= flecs_fuzz_next(&rng) + world->iteration_fuzz_count ++;
    ecs_fuzz_iteration_order(world, flecs_fuzz_next(&rng),
        world->iteration_fuzz_flags);
}
