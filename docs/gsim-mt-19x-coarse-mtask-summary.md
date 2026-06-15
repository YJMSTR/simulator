# GSim-MT 19X Coarse MTask Summary

19X adds `--mt-coarse-runtime=layered|mtask`. `layered` is the default 18X runtime. `mtask` groups each runtime-eligible coarse region into ordering-connected MTasks, runs independent MTasks under one pool dispatch per coarse region, and keeps a deterministic merge at the region boundary.

## Focused Gates

Passed:

```text
make build-gsim -j4
python3 scripts/check-mt-19x-coarse-dispatch-accounting.py --gsim-bin build/gsim/gsim --input-dir test --work-dir /tmp/gsim-19x-accounting-after-mtask --timeout 30
python3 scripts/check-mt-19x-coarse-mtask-runtime.py --gsim-bin build/gsim/gsim --input-dir test --work-dir /tmp/gsim-19x-mtask-green --timeout 30
python3 scripts/check-mt-18x-coarse-region-gate.py --gsim-bin build/gsim/gsim --input-dir test --work-dir /tmp/gsim-18x-after-19x-mtask --timeout 30
python3 scripts/check-mt-17x-worker-pool-gate.py --gsim-bin build/gsim/gsim --fir test/mt-true-parallel-activation.fir --work-dir /tmp/gsim-17x-after-19x-mtask --timeout 30
python3 scripts/check-mt-16x-active-frequency-gate.py --gsim-bin build/gsim/gsim --fir test/mt-true-parallel-activation.fir --work-dir /tmp/gsim-16x-after-19x-mtask --timeout 30
python3 scripts/check-mt-profile-off-fast-path.py /tmp/gsim-profile-off-fast-path-model-19x
GSIM_THREADS=4 python3 scripts/run-mt-fir-smoke.py --gsim-bin build/gsim/gsim --input-dir test --output-dir /tmp/gsim-19x-mtask-mt-smoke --cases "mt-comb mt-reg-reset mt-async-reset mt-memory mt-multiclock mt-orword-packed" --flags "--supernode-max-size=1 --mt-helper-mode=mt" --timeout 30s
```

Focused fixture result:

```text
mt-19x-coarse-mtask-runtime ok: layered_barriers=96 mtask_barriers=32 layered_copies=1152 mtask_copies=384
```

## XiangShan Matrix

Workload:

```text
/home/zhangyangjie/test/XiangShan/ready-to-run/coremark-2-iteration.bin
<emu> -i <bin> --no-diff -b 0 -e 0 -C 5000
```

No-profile, pool on, `GSIM_MT_MIN_BATCH_TASKS=8`:

| threads | host ms | voluntary cs |
| ---: | ---: | ---: |
| 1 | 5750 | 1 |
| 2 | 11784 | 705149 |
| 4 | 12091 | 1286798 |
| 8 | 17758 | 3138430 |
| 16 | 26228 | 7014033 |

Profile rows:

| threads | host ms | true_parallel_wall_ns | merge_wall_ns | voluntary cs |
| ---: | ---: | ---: | ---: | ---: |
| 4 | 13360 | 8928898674 | 260379931 | 1309155 |
| 16 | 29081 | 21348179247 | 617923566 | 7060020 |

Coarse profile:

| threads | static regions | static layers | static mtasks | region invocations | layer dispatches | estimated barriers |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 4 | 40 | 294 | 1414 | 204040 | 0 | 204040 |
| 16 | 40 | 294 | 1414 | 204040 | 0 | 204040 |

Compared with 18X, 19X reduces coarse runtime overhead:

| row | 18X | 19X |
| --- | ---: | ---: |
| no-profile t4 host ms | 57379 | 12091 |
| no-profile t16 host ms | 149090 | 26228 |
| profile t4 true_parallel_wall_ns | 53248559795 | 8928898674 |
| profile t16 true_parallel_wall_ns | 120420416694 | 21348179247 |

No speedup is claimed against the original-style `off` baseline (`1620 ms`) or 17X threshold64/min8 pool-on t4 (`4894 ms`).

Full evidence logs are under:

```text
/home/zhangyangjie/test/wolvrix-playground/pdocs/draft/logs/gsim-mt-19x-coarse-mtask-dispatch-amortization/
```
