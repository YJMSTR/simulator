# GSim-MT 24X Top-Blocker Source-Cluster Audit Summary

24X is a report-only source/codegen cluster audit and hard continue/pivot
decision. It does not implement a runtime code path, memory parallelization,
state-update commit, KaHyPar integration, or a full two-phase RepCut runtime.

## New Artifacts

- `scripts/mt-24x-top-blocker-source-audit.py` — clusters top blocked XiangShan
  cppIds by source/codegen shape, splits the two dominant blocker families into
  fine-grained sub-classes, and estimates recoverable cost under concrete proof
  rules.
- `scripts/check-mt-24x-top-blocker-source-audit.py` — focused fixture gate.
- `Makefile` target `check-mt-24x-top-blocker-source-audit`.
- XiangShan report:
  `/home/zhangyangjie/test/wolvrix-playground/pdocs/draft/logs/gsim-mt-24x-top-blocker-source-audit/40-xs-top-blocker-source-audit.md`
- Kimi drift review:
  `/home/zhangyangjie/test/wolvrix-playground/pdocs/draft/logs/gsim-mt-24x-top-blocker-source-audit/50-kimi-drift-review.md`

## Focused Gate

```sh
make check-mt-24x-top-blocker-source-audit FIR_TEST_TIMEOUT=60s
```

The 23X focused gate continues to pass:

```sh
make check-mt-23x-blocker-inspection FIR_TEST_TIMEOUT=60s
```

## XiangShan Audit Inputs

- 23X blocker inspection JSON from commit `c1b4689`.
- Schedule JSON:
  `/home/zhangyangjie/test/.worktrees/gsim-mt/wolvrix-playground/build/xs/gsim-mt-21x-profitable-parallel-work-and-balanced-mtasks/gsim-compile/model/SimTop_mt_schedule.json`
- Profile log (task counts):
  `/home/zhangyangjie/test/wolvrix-playground/pdocs/draft/logs/gsim-mt-23x-macro-fiber-proof-metadata-repair/20-xs-profile-task-counts.stderr.txt`

Because no code or metadata changed, the planner numbers are expected unchanged;
24X only reran the new audit report.

## Key Numbers

- total expected active cost: `1913594138`
- audited expected active cost (top 40 rows of the two dominant families): `351726295` (~18.4%)
- recoverable expected active cost: `215318361` (~11.25% of total)
- recoverable wall-time estimate (approximate): `107.7 ms`

### `memory_dynamic_or_array_index` family

- family expected active cost: `236504740` (~12.4% of total)
- recoverable expected active cost: `150457505` (~7.9% of total)
- sub-class distribution:
  - `read_only_dynamic_index`: `42773363`
  - `array_or_mux_compute`: `193731377`
- triage distribution:
  - `repairable_metadata_gap`: `42773363`
  - `classification_too_coarse`: `193731377`

### `state_update_unknown_rhs_timing` family

- family expected active cost: `115221555` (~6.0% of total)
- recoverable expected active cost: `64860856` (~3.4% of total)
- sub-class distribution:
  - `commit_only`: `31959013`
  - `rhs_compute_can_be_precomputed`: `78630834`
  - `unknown`: `4631708`
- triage distribution:
  - `classification_too_coarse`: `31959013`
  - `repairable_metadata_gap`: `78630834`
  - `needs_source_instrumentation`: `4631708`

## 24X-B Decision

**Decision:** `pivot`

The source-backed recoverable cost (`11.25%`) is below the 20%-30% threshold,
and no single repeated cluster releases a large enough share to justify a
focused metadata-only repair. The local metadata-repair line should stop and
pivot to single-thread fast-path repair plus 22M module/structure-level
feasibility.

## Kimi Drift Review Verdict

- 24X-A audit: acceptable report-only source-cluster audit.
- 24X-B decision: **agree with `pivot`**.
- No runtime work justified by 24X artifacts.

## Non-Goals Respected

- No broad memory parallelization.
- No state-update runtime commit.
- No KaHyPar integration.
- No full two-phase RepCut runtime.
- No performance claim from report-only data.
- No worker-policy, thread-cap, LPT, or threshold tuning.
