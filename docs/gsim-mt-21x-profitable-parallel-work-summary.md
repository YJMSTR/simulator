# GSim-MT 21X Profitable Parallel Work Summary

21X adds `--mt-coarse-worker-policy=static|profitable` and keeps `static` as the default. The `profitable` policy uses generated and runtime-visible coarse MTask facts to select worker counts from useful whole-MTask work instead of selecting from `GSIM_THREADS` alone.

It also adds deterministic cost-balanced whole-MTask assignment for the profitable path. The assignment uses generated static MTask costs, preserves task order inside each MTask, and does not split an MTask across workers.

## Focused Gates

Passed at `5e098ba test: add profitable balanced mtask gate`:

```text
make build-gsim
scripts/check-mt-21x-profitable-balanced-mtasks.py --gsim-bin build/gsim/gsim --input-dir test --work-dir /tmp/gsim-21x-profitable-balanced-postcommit --timeout 30
scripts/check-mt-20x-coarse-profitability-policy.py --gsim-bin build/gsim/gsim --input-dir test --work-dir /tmp/gsim-20x-policy-after-21x-postcommit --timeout 30
scripts/check-mt-19x-coarse-mtask-runtime.py
scripts/check-mt-19x-coarse-dispatch-accounting.py
scripts/check-mt-18x-coarse-region-gate.py
scripts/check-mt-17x-worker-pool-gate.py
scripts/check-mt-16x-active-frequency-gate.py
scripts/check-mt-profile-off-fast-path.py
scripts/run-mt-fir-smoke.py
git diff --check
```

Focused 21X fixture result:

```text
mt-21x-profitable-balanced-mtasks ok:
balanced_selected={0: 0, 1: 0, 2: 0, 3: 0, 4: 32}
cap_selected={0: 0, 1: 32, ... 16: 0}
balanced_worst=1408 contiguous_worst=2048
```

## Report Fields

The coarse report format is now `gsim.mt-coarse-region-report.v2`. New fields include:

- `coarse_worker_policy`
- `recommended_workers`
- `mtask_assignments`
- per-thread contiguous versus balanced static-cost summaries
- worker MTask index lists

Runtime profile now also reports:

- `coarse_worker_policy`
- `active_mtasks`
- `active_mtask_static_cost`
- `assigned_static_cost`
- `[mt-profile] coarse_assignment ...`

The 18X gate accepts both report v1 and report v2.

## XiangShan Matrix

Workload:

```text
/home/zhangyangjie/test/XiangShan/ready-to-run/coremark-2-iteration.bin
<emu> -i <bin> --no-diff -b 0 -e 0 -C 5000
```

Generation, build, and run logs used:

```text
systemd-run --user --wait --collect --pipe -p MemoryMax=1000G -p MemorySwapMax=0
```

Current-result prefixes:

```text
60-xs-generate-21x-profitable-model
61-xs-build-21x-profitable-emu
62-xs-21x-profitable-noprofile-t{1,2,4,8,16}
63-xs-21x-profitable-profile-t{4,16}
```

Generation report:

| fact | value |
| --- | ---: |
| report format | `gsim.mt-coarse-region-report.v2` |
| candidate regions | 257 |
| runtime-eligible regions | 40 |
| blocker `codegen_runtime_limit` | 217 |
| static MTask count | 1414 |
| admitted t16 regions | 37 |

Static report recommendation histogram:

| thread cap | recommendation histogram |
| ---: | --- |
| 1 | `1:257` |
| 2 | `1:220,2:37` |
| 4 | `1:220,2:5,3:1,4:31` |
| 8 | `1:220,2:5,3:1,4:1,5:1,6:2,7:1,8:26` |
| 16 | `1:220,2:5,3:1,4:1,5:1,6:2,7:1,10:1,12:2,13:1,14:5,16:17` |

No-profile, pool on, `GSIM_MT_MIN_BATCH_TASKS=8`, `--mt-coarse-worker-policy=profitable`:

| threads | host ms | system s | voluntary cs |
| ---: | ---: | ---: | ---: |
| 1 | 5,671 | 0.02 | 1 |
| 2 | 14,773 | 4.53 | 707,951 |
| 4 | 14,561 | 9.14 | 1,301,162 |
| 8 | 20,568 | 32.43 | 2,963,119 |
| 16 | 27,408 | 133.48 | 6,922,189 |

Profile rows:

| threads | host ms | true_parallel_wall_ns | merge_wall_ns | voluntary cs |
| ---: | ---: | ---: | ---: | ---: |
| 4 | 15,403 | 9,750,915,097 | 263,936,971 | 1,305,068 |
| 16 | 27,305 | 21,126,192,041 | 698,754,633 | 7,534,424 |

Coarse profile:

| threads | region invocations | accepted | rejected | mtask dispatches | worker jobs | flag word copies | merge word scans |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 4 | 204,040 | 188,737 | 15,303 | 7,212,814 | 714,140 | 13,568,660 | 13,568,660 |
| 16 | 204,040 | 188,737 | 15,303 | 7,212,814 | 2,208,733 | 44,297,084 | 44,297,084 |

Selected worker-count histograms:

| threads | selected worker histogram |
| ---: | --- |
| 4 | `0:0,1:15303,2:25505,3:5101,4:158131` |
| 16 | `0:0,1:15303,2:25505,3:5101,4:5101,5:5101,6:10202,7:5101,8:0,9:0,10:5101,11:0,12:10202,13:5101,14:25505,15:0,16:86717` |

Balanced assignment aggregate:

| threads | contiguous worst static cost | balanced worst static cost |
| ---: | ---: | ---: |
| 4 | 578,841,076 | 485,227,524 |
| 16 | 458,319,749 | 445,791,693 |

## Comparison

Same workload, same 5000-cycle shape.

| row | host ms |
| --- | ---: |
| original-style `off` baseline | 1620 |
| 17X threshold64/min8 pool-on t4 | 4894 |
| 19X mtask pool-on t4 | 13039 |
| 19X mtask pool-on t16 | 25814 |
| 20X static pool-on t1 | 6485 |
| 20X static pool-on t2 | 12278 |
| 20X static pool-on t4 | 12843 |
| 20X static pool-on t8 | 19062 |
| 20X static pool-on t16 | 27254 |
| 21X profitable pool-on t1 | 5671 |
| 21X profitable pool-on t2 | 14773 |
| 21X profitable pool-on t4 | 14561 |
| 21X profitable pool-on t8 | 20568 |
| 21X profitable pool-on t16 | 27408 |

21X improves the same-spec t1 row versus 20X, but it does not prove the required t8 or t16 no-profile improvement. No speedup is claimed against 17X, 19X, 20X t8/t16, or the original-style `off` baseline.

## Blocker

21X proves that high selected worker counts are not the only problem. The profitable policy still selects 16 workers for 86,717 t16 invocations when generated facts predict useful independent MTask work. Static balancing also reduces aggregate worst-worker static cost, especially at t4.

The runtime remains dominated by overhead:

- 204,040 estimated region barriers;
- 2,208,733 t16 worker jobs;
- 44,297,084 t16 flag-word copies and merge-word scans;
- 7,534,424 voluntary context switches in the t16 profile run;
- residual t16 aggregate static-cost imbalance of about 4.8x from best to worst worker;
- only 40 of 257 candidate regions are runtime eligible, with 217 blocked by `codegen_runtime_limit`.

The next blocker is region formation and proof quality, not a worker-pool micro-optimization. A useful 22X direction should reduce the number of synchronization points or expose larger safe regions through stronger active/state-update proof or RepCut-lite style cuts.

Full evidence logs are under:

```text
/home/zhangyangjie/test/wolvrix-playground/pdocs/draft/logs/gsim-mt-21x-profitable-parallel-work-and-balanced-mtasks/
```
