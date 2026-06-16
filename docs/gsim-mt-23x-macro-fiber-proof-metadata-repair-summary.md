# GSim-MT 23X Macro-Fiber Proof Metadata Repair Summary

23X is a report-only proof and metadata repair stage. It does not add a runtime
path, a two-phase commit, memory parallelization, or a performance claim.

The new blocker inspection report consumes existing `*_mt_schedule.json` and
`GSIM_MT_PROFILE_TASKS=1` profile logs. It refines broad 22X blocker classes
into source-backed subreasons using fields already emitted by schedule JSON:

- state-update RHS timing, target count, writer universe, local-safe and
  runtime-safe flags;
- memory read/write/dynamic-array indicators;
- reset and async-reset facts;
- external, special, and black-box boundary facts.

Focused gate:

```sh
make check-mt-23x-blocker-inspection FIR_TEST_TIMEOUT=60s
```

XiangShan reports are stored under:

```text
/home/zhangyangjie/test/wolvrix-playground/pdocs/draft/logs/gsim-mt-23x-macro-fiber-proof-metadata-repair/
```

Key XiangShan blocker subreasons:

- `memory_dynamic_or_array_index`: `424310604`
- `state_update_unknown_rhs_timing`: `370237221`
- `reset_commit_or_side_effect`: `59548388`
- `special_statement_or_assert`: `49474600`
- `memory_write_serial`: `35586198`
- `state_update_local_safe_duplicate_writer_blocked`: `14521430`
- `async_reset_observation_point`: `13904801`
- `state_update_multi_target_writer_unproven`: `13570040`
- `external_or_blackbox`: `3180889`
- `memory_read_unsupported`: `1806946`

The report found:

- runtime-safe expected active cost: `0`
- optimistic runtime-safe expected active cost: `0`
- macro-fiber pure candidate expected active cost after 23X inspection: `12709`
- macro-fiber safe subset: `none`

Go/no-go decision: no-go for a runtime prototype. The next branch should target
further proof repair for memory dynamic/array indexing and state-update RHS
timing, or a different structure-level parallel source. Runtime work is not
justified by the 23X safe subset.
