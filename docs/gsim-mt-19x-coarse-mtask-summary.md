# GSim-MT 19X Coarse MTask Summary

19X adds `--mt-coarse-runtime=layered|mtask`. `layered` is the default 18X runtime. `mtask` groups each runtime-eligible coarse region into ordering-connected MTasks, runs independent MTasks under one pool dispatch per coarse region, and keeps a deterministic main-thread merge at the region boundary.

MTask workers also merge region-local `ActivationDelta` entries into their own worker-local coarse flags after each non-empty layer slice. This keeps same-MTask active visibility local and deterministic without adding per-layer pool barriers. Activation entries outside the region remain in `ActivationDelta` until the region-boundary merge.

## Focused Gates

Passed at `84ecf29 runtime: preserve coarse mtask active visibility`:

```text
make build-gsim -j4
python3 scripts/check-mt-19x-coarse-mtask-runtime.py --gsim-bin build/gsim/gsim --input-dir test --work-dir /tmp/gsim-19x-mtask-local-visibility-log --timeout 30
python3 scripts/check-mt-19x-coarse-dispatch-accounting.py --gsim-bin build/gsim/gsim --input-dir test --work-dir /tmp/gsim-19x-accounting-after-local-visibility-log --timeout 30
python3 scripts/check-mt-18x-coarse-region-gate.py --gsim-bin build/gsim/gsim --input-dir test --work-dir /tmp/gsim-18x-after-local-visibility-log --timeout 30
python3 scripts/check-mt-17x-worker-pool-gate.py --gsim-bin build/gsim/gsim --fir test/mt-true-parallel-activation.fir --work-dir /tmp/gsim-17x-after-local-visibility-log --timeout 30
python3 scripts/check-mt-16x-active-frequency-gate.py --gsim-bin build/gsim/gsim --fir test/mt-true-parallel-activation.fir --work-dir /tmp/gsim-16x-after-local-visibility-log --timeout 30
python3 scripts/check-mt-profile-off-fast-path.py /tmp/gsim-17x-after-local-visibility-log/mt_active_frequency
GSIM_THREADS=4 python3 scripts/run-mt-fir-smoke.py --gsim-bin build/gsim/gsim --input-dir test --output-dir /tmp/gsim-19x-local-visibility-mt-smoke-log --cases "mt-comb mt-reg-reset mt-async-reset mt-memory mt-multiclock mt-orword-packed" --flags "--supernode-max-size=1 --mt-helper-mode=mt" --timeout 30s
```

Focused fixture result:

```text
mt-19x-coarse-mtask-runtime ok: layered_barriers=96 mtask_barriers=32 layered_copies=1152 mtask_copies=384
```

The 19X gate verifies `GSIM_THREADS=1/2/4/8/16` trace equivalence against seq, independent MTask grouping, preserved internal ordering edges, lower estimated barrier/copy/merge counts than layered, generated `mtMergeLocalCoarseDelta`, and matching `ActivationDelta` entry accounting between layered and mtask.

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
42-xs-generate-19x-mtask-local-visibility-model
43-xs-build-19x-mtask-local-visibility-emu
44-xs-19x-mtask-local-visibility-noprofile-t{1,2,4,8,16}
45-xs-19x-mtask-local-visibility-profile-t{4,16}
```

No-profile, pool on, `GSIM_MT_MIN_BATCH_TASKS=8`:

| threads | host ms | voluntary cs |
| ---: | ---: | ---: |
| 1 | 6526 | 1 |
| 2 | 15463 | 705622 |
| 4 | 13039 | 1287892 |
| 8 | 17545 | 2898967 |
| 16 | 25814 | 7100977 |

Profile rows:

| threads | host ms | true_parallel_wall_ns | merge_wall_ns | voluntary cs |
| ---: | ---: | ---: | ---: | ---: |
| 4 | 14743 | 9338225747 | 306877777 | 1245194 |
| 16 | 27856 | 20857939200 | 597568011 | 6565021 |

Coarse profile:

| threads | static regions | static layers | static mtasks | region invocations | layer dispatches | activation delta entries | estimated barriers |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 4 | 40 | 294 | 1414 | 204040 | 0 | 22180 | 204040 |
| 16 | 40 | 294 | 1414 | 204040 | 0 | 22180 | 204040 |

The full MT activation-delta profile reports `entries=305720` and `max_entries_per_worker=209` for both profile rows.

Compared with 18X, 19X reduces coarse runtime overhead:

| row | 18X | 19X |
| --- | ---: | ---: |
| no-profile t4 host ms | 57379 | 13039 |
| no-profile t16 host ms | 149090 | 25814 |
| profile t4 true_parallel_wall_ns | 53248559795 | 9338225747 |
| profile t16 true_parallel_wall_ns | 120420416694 | 20857939200 |

No speedup is claimed against the original-style `off` baseline (`1620 ms`) or 17X threshold64/min8 pool-on t4 (`4894 ms`).

Full evidence logs are under:

```text
/home/zhangyangjie/test/wolvrix-playground/pdocs/draft/logs/gsim-mt-19x-coarse-mtask-dispatch-amortization/
```
