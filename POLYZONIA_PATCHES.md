# Polyzonia 4.2 core patch audit

Base: upstream `v4.2.0` (`6566d4b7b`). This branch excludes the iteration
order fuzzer. Tests extend existing registered cases, so the suite case totals
are unchanged. No distribution sources or generated test registries were edited.

## Patch verdicts

1. **STILL NEEDED: f02dc8b9d, Apply patch.** Keep the shared library's internal
   entry points visible even when its embedding build defaults to hidden
   visibility. Preserve already registered `EcsComponent` and `EcsMember`
   layouts when reading JSON. Unlike the old unconditional skip, the port
   permits uninitialized metadata for newly restored dynamic types.
   `b9878abed` reworked JSON restoration through deferred removals but did not
   protect registered layouts. The final regression in
   `DeserializeFromJson.ser_deser_new_world_1_entity_w_component_meta` changes
   saved member type and component size. Plain 4.2 fails, the port passes.

2. **STILL NEEDED: 4a7042e96, Remove ParentDepth table fragmentation.** Remove
   the value pairs from new children, reparenting, descendant depth updates and
   prefab spawn tables. Relevant upstream changes include `93566cba6` (prefab
   addon split), `a141ad44b` (parent storage refactor), `8faa85d84` (parent
   storage fixes) and `39478a9c4` (cached ChildOf queries). The port also fixes
   assumptions exposed by removing these pairs:
   - Parent traversal must work without `EcsTableHasPairs`, both in relationship
     search and the query traversal cache.
   - Reparented names must be resolved from the entity record's row. The
     on_replace iterator offset can be zero for a different row in the same
     table, which used to be masked by depth fragmentation.
   - OR traversal must evaluate each entity instead of stopping after one
     matching parent range. A self/up ChildOf pair term must handle both direct
     children and deeper descendants in one table. Cached queries defer that
     per-row decision to their post-processing instruction.
   - `ecs_get_depth(entity, ChildOf)` reads the cached parent record depth and
     remains correct. Prefab spawn tables no longer depend on instance depth.

   **Deliberate restriction:** `ecs_table_get_depth(table, ChildOf)` and cached
   ChildOf cascade ordering cannot describe a mixed-depth Parent table. They
   fail explicitly instead of returning a misleading zero or stale depth.
   Use fragmenting ChildOf for a hierarchy requiring table depth or cascade.
   The six upstream Parent cascade tests now assert this diagnostic. Explicit
   grouping by the absent ParentDepth pair puts all Parent rows in group zero.
   The game has no ChildOf cascade, table-depth, entity-depth or ParentDepth
   usages. Its cascade calls use DependsOn, which is unaffected.

   The no-fragmentation and per-entity-depth regression in
   `NonFragmentingChildOf.depth_after_parent_set` fails on plain 4.2 and passes
   here. Hierarchy and query tests still validate the exact entity set,
   uniqueness, field IDs, field sources and component values. Tests that assumed
   separate depth tables now allow the resulting table batches and row order.

3. **STILL NEEDED: bb8ecc7c2, exact float JSON and serializer lookup.**
   `b1bc49836` moved JSON's reflection traversal to `addons/meta/serialize.c`.
   `5519df523` simplified the existing float formatter, which still loses
   precision. Port the public exact writer API there, keeping expression output
   unchanged. Opaque writers cache the last element type serializer locally,
   including nested and heterogeneous callbacks. The snprintf fallback uses
   nine digits for f32 and seventeen for f64. Positive exponent signs are now
   accepted by the shared numeric parser, since the fallback emits e+308 for
   DBL_MAX. Existing NaN/Inf formatting remains covered.
   `SerializeToJson.struct_float` and `.struct_double` check bitwise round trips
   including finite extremes and signed zero, installed callbacks and callback
   fallback. Plain 4.2 loses bits for 1.2345678901234567, the port passes.

4. **FIXED UPSTREAM: 6ad767061, mixed hierarchy deletion.** `f6b2ca1f7`
   excludes parents with fragmenting ChildOf tables from the fast path.
   `7aa8fea11` invalidates reachable caches again after simple deletion.
   Additional 4.2 changes include `0d93b84dd` (deletion order) and `6d179759e`
   (observers modifying siblings). No on_delete source patch is retained.
   `NonFragmentingChildOf.create_delete_mixed_tree` now mirrors both game
   scenarios: erasing the tree and cutting a branch with a fragmenting
   attachment, another traversable ownership relation, a cached up query and a
   leaf OnRemove observer. The same added scenario aborts in plain v4.1.6
   (`entity is not alive` during event forwarding) and passes on plain v4.2.0.

5. **OBSOLETE: b70658c9a, preserve suspended deferral.** `53633240b` already
   applied the conditional suspension fix. `2c517a770` then removed implicit
   core deferral and the entire suspend/resume code path. `4f12f7833` renamed
   the remaining stage predicate. There are no nested or suspended deferral
   scopes to preserve in 4.2. The migrated regression in
   `NonFragmentingChildOf.defer_delete_parent_and_tag` checks immediate deletion
   through the real world and deferred deletion through its explicit stage,
   while unrelated stage commands remain queued. Both tree erasure and branch
   cutting pass on plain 4.2 and on this branch.

6. **OBSOLETE: 89c136e0f, cancelled remove notifications.** `1d5850a89`
   removes the sticky DontFragment table flags. `9062901b9` changes remove/add
   batching to actual separate transitions, so the original cancelled-removal
   path no longer exists. `Commands.on_remove_after_deferred_clear_and_add`
   reproduces prior DontFragment use and checks remove/add and clear/add. It
   passes on plain 4.2 and here, with exactly one real OnRemove per operation.
   The game migration must account for this upstream behavior: remove followed
   by add now emits real lifecycle events even when the final component set is
   identical. No commands.c source patch is retained.

## Validation

macOS arm64, AppleClang 17, CMake Debug, static Flecs. Bake dependency:
`2b5b0f59a095f31fc43f94ea800c413680606e76`.

| Suite | Plain v4.2.0 pass / fail | Port pass / fail |
| --- | --- | --- |
| core | 3885 / 0 | 3885 / 0 |
| meta, including JSON | 1507 / 0 | 1507 / 0 |
| query, all cache parameter variants | 4806 / 0 | 4806 / 0 |

The framework's existing quarantines remain in place. The six Parent cascade
cases now expect the documented diagnostic and the two ParentDepth grouping
cases assert group zero. These are deliberate contract changes, not upstream
failures. There were no pre-existing upstream failures in the three runs.

Also built shared Flecs with `CMAKE_C_VISIBILITY_PRESET=hidden`. Linking core
and meta tests succeeded, proving internal symbols remain exported. Shared
focused checks passed: 321 NonFragmentingChildOf cases, 94 SerializeToJson
cases and the registered-layout restoration regression.

All builds and tests used the game repository's machine slots:

```sh
python3 scripts/machine_slot.py build -- cmake \
  -S /tmp/flecs42-core-src -B /tmp/flecs42-core-build \
  -DFLECS_TESTS=ON -DFLECS_SHARED=OFF \
  -DBAKE_DIRECTORY=../flecs42-core-bake -DCMAKE_BUILD_TYPE=Debug
python3 scripts/machine_slot.py build -- cmake --build /tmp/flecs42-core-build \
  --target test_core_flecs_static test_meta_flecs_static test_query_flecs_static -j 8
python3 scripts/machine_slot.py test -- /tmp/flecs42-core-build/test/test_core_flecs_static
python3 scripts/machine_slot.py test -- /tmp/flecs42-core-build/test/test_meta_flecs_static
python3 scripts/machine_slot.py test -- /tmp/flecs42-core-build/test/test_query_flecs_static
```

Baseline source/build paths were `/tmp/flecs42-core-base` and
`/tmp/flecs42-core-base-build`. Baseline full suites ran before injecting
regressions. The v4.1.6 deletion regression used `/tmp/flecs42-core-416` and
`/tmp/flecs42-core-416-build`. Logs use `/tmp/flecs42-core-{base,final}-*.log`.
No game binary was built or run. The game submodule pointer remains at
`89c136e0f`. The parent upgrade hub owns review and game integration.
