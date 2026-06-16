# GSim-MT 20X Coarse Profitability Summary

20X adds `--mt-coarse-profitability=off|static`. `off` preserves the 19X coarse MTask runtime. `static` uses generated coarse-region cost facts to reject low-profit regions and cap the selected worker count for accepted regions.

Rejected static regions execute a correctness-preserving single-worker MTask path without worker-pool dispatch. Accepted regions continue to use the 19X MTask runtime and report the selected worker-count histogram in the coarse dispatch profile.

## Focused Gates

Passed at `44d7e49 test: add coarse profitability policy gate`:

```text
make build-gsim
python3 scripts/check-mt-20x-coarse-profitability-policy.py --gsim-bin build/gsim/gsim --input-dir test --work-dir /tmp/gsim-20x-policy-gate-postcommit --timeout 30
python3 scripts/check-mt-19x-coarse-dispatch-accounting.py
python3 scripts/check-mt-19x-coarse-mtask-runtime.py
python3 scripts/check-mt-18x-coarse-region-gate.py
python3 scripts/check-mt-17x-worker-pool-gate.py
python3 scripts/check-mt-16x-active-frequency-gate.py
python3 scripts/check-mt-profile-off-fast-path.py
python3 scripts/run-mt-fir-smoke.py
```

Focused 20X fixture result:

```text
accepted_workers={0:0,1:0,2:32,...16:0} accepted_regions=32 mtask_dispatches=192
```

The 20X gate verifies accepted and rejected coarse paths are trace-equivalent, accepted regions still use MTask runtime, selected worker count can be lower than `GSIM_THREADS`, and `GSIM_THREADS=1/2/4/8/16` preserve the trace.

## XiangShan Matrix

Workload:

```text
/home/zhangyangjie/test/XiangShan/ready-to-run/coremark-2-iteration.bin
<emu> -i <bin> --no-diff -b 0 -e 0 -C 5000
```

Generation/build/run logs use the required memory guard:

```text
systemd-run --user --wait --collect --pipe -p MemoryMax=1000G -p MemorySwapMax=0
```

Current-result prefixes:

```text
50-xs-generate-20x-static-model
51-xs-build-20x-static-emu
52-xs-20x-static-noprofile-t{1,2,4,8,16}
53-xs-20x-static-profile-t{4,16}
```

Generation report:

| fact | value |
| --- | ---: |
| candidate regions | 257 |
| runtime-eligible regions | 40 |
| runtime-eligible static layers | 294 |
| runtime-eligible static MTasks | 1414 |
| max task count | 456 |
| max active word span | 57 |
| max parallel width | 272 |
| admitted t16 regions | 37 |

No-profile, pool on, `GSIM_MT_MIN_BATCH_TASKS=8`, `--mt-coarse-profitability=static`:

| threads | host ms | system s | voluntary cs |
| ---: | ---: | ---: | ---: |
| 1 | 6485 | 0.03 | 1 |
| 2 | 12278 | 3.57 | 704299 |
| 4 | 12843 | 8.50 | 1292506 |
| 8 | 19062 | 28.35 | 2843113 |
| 16 | 27254 | 122.33 | 6634082 |

Profile rows:

| threads | host ms | true_parallel_wall_ns | merge_wall_ns | voluntary cs |
| ---: | ---: | ---: | ---: | ---: |
| 4 | 13306 | 7859293985 | 258154897 | 1211696 |
| 16 | 28431 | 21693921050 | 588413899 | 6491745 |

Coarse profile:

| threads | static regions | static layers | static mtasks | region invocations | accepted | rejected | layer dispatches | mtask dispatches | worker jobs | flag word copies | merge word scans | estimated barriers |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 4 | 40 | 294 | 1414 | 204040 | 188737 | 15303 | 0 | 7212814 | 714140 | 13568660 | 13568660 | 204040 |
| 16 | 40 | 294 | 1414 | 204040 | 188737 | 15303 | 0 | 7212814 | 2147521 | 43358500 | 43358500 | 204040 |

Selected worker-count histograms:

| threads | selected worker histogram |
| ---: | --- |
| 4 | `0:0,1:15303,2:25505,3:5101,4:158131` |
| 16 | `0:0,1:15303,2:25505,3:5101,4:5101,5:5101,6:10202,7:5101,8:0,9:0,10:5101,11:0,12:15303,13:0,14:45909,15:15303,16:51010` |

## Comparison

Same workload, same 5000-cycle shape.

| row | host ms |
| --- | ---: |
| original-style `off` baseline | 1620 |
| 17X threshold64/min8 pool-on t4 | 4894 |
| 18X coarse pool-on t4 | 57379 |
| 19X mtask pool-on t4 | 13039 |
| 19X mtask pool-on t16 | 25814 |
| 20X static pool-on t1 | 6485 |
| 20X static pool-on t2 | 12278 |
| 20X static pool-on t4 | 12843 |
| 20X static pool-on t8 | 19062 |
| 20X static pool-on t16 | 27254 |

20X improves the required t4 comparison versus 19X by `196 ms` (`13039 ms` to `12843 ms`). It does not improve the t8 or t16 points, and it remains slower than both the original-style `off` baseline and the 17X threshold64/min8 t4 row. No speedup is claimed against those baselines.

## Blocker

The static policy rejects `15303` of `204040` coarse invocations, about `7.5%`. It still admits `188737` invocations, and many admitted t16 invocations select high worker counts: `45909` select 14 workers, `15303` select 15 workers, and `51010` select 16 workers.

The t16 no-profile row still spends `122.33 s` in system time and records `6634082` voluntary context switches. The t16 profile records `2147521` worker jobs, `43358500` flag word copies, `43358500` merge word scans, and severe worker-task imbalance (`8808719 / 260151`, about `33.9x`). The remaining blocker is over-admission plus high selected worker counts and static MTask imbalance, not worker-pool micro-tuning.

Full evidence logs are under:

```text
/home/zhangyangjie/test/wolvrix-playground/pdocs/draft/logs/gsim-mt-20x-coarse-profitability-and-worker-policy/
```
