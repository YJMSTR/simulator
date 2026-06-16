# GSim-MT 22X Macro-Fiber RepCut Planner Summary

22X adds a report-only macro-fiber RepCut planner. It does not change generated
runtime behavior and does not make a runtime speedup claim.

The planner consumes existing `*_mt_schedule.json` metadata plus a
`GSIM_MT_PROFILE_TASKS=1` profile log, builds conservative sink-rooted
macro-fiber candidates, weights work by `static_cost * active_count`, and
estimates deterministic greedy partitions for `2/4/8/16` targets. It reports
unique expected active cost, overlapping candidate cost, copied static and
expected cost, replication ratio, active-weighted versus static-only imbalance,
estimated barrier reduction, replicated nodes, and blocker reasons.

Focused fixtures:

```sh
make check-mt-22x-macro-fiber-planner FIR_TEST_TIMEOUT=60s
```

The fixture gate covers:

- shared pure producer feeding independent sinks where bounded replication helps;
- high-copy shared producer where tight replication budget rejects copying;
- memory/reset/state-update/external blockers;
- activity skew where static-only balance differs from active-weighted balance.

XiangShan was evaluated using the 21X model generated from commit
`c2044d7379c0e3f4a14fce0bed75617640d88647`, the same CoreMark two-iteration
workload shape, and a fresh `GSIM_MT_PROFILE_TASKS=1` run under:

```sh
systemd-run --user --wait --collect --pipe -p MemoryMax=1000G -p MemorySwapMax=0
```

The XiangShan report is stored at:

```text
/home/zhangyangjie/test/wolvrix-playground/pdocs/draft/logs/gsim-mt-22x-activity-weighted-macro-fiber-repcut-planner/30-xs-macro-fiber-planner.md
```

Key XiangShan result:

- total expected active cost: `1913594138`
- unique macro-fiber candidate expected active cost: `849141542`
- pure-only candidate expected active cost: `12709`
- optimistic/state-rooted candidate expected active cost: `849128833`
- duplicated expected active cost at `2/4/8/16`: `8642`
- duplicated cppId count at `2/4/8/16`: `3`
- replication ratio at `2/4/8/16`: about `1.00001`
- dominant blockers by expected active cost: state-update proof, memory,
  reset, serial side effects, external

Decision: 22X recommends metadata/proof repair before runtime work. The safe
pure-only subset is too small to justify a 23X runtime prototype from this
planner result.
