#!/usr/bin/env python3
"""GSim-MT 24X top-blocker source/codegen cluster audit.

Consumes a 23X blocker inspection JSON and the underlying schedule JSON,
clusters the top blocked cppIds by source/codegen shape, splits the two
dominant blocker families into fine-grained classifications, and estimates
recoverable cost under concrete proof rules.

Report-only: does not implement a runtime code path.
"""

import argparse
import json
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path


TARGET_SUBREASONS = {
    "memory_dynamic_or_array_index",
    "state_update_unknown_rhs_timing",
}

TOP_N = 40


def fail(message):
    print(f"mt-24x-top-blocker-source-audit failed: {message}", file=sys.stderr)
    sys.exit(1)


def expect(condition, message):
    if not condition:
        fail(message)


def read_json(path):
    try:
        return json.loads(path.read_text())
    except Exception as exc:
        fail(f"failed to read JSON {path}: {exc}")


def git_commit(repo_root):
    try:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=repo_root,
            text=True,
            capture_output=True,
            check=True,
        )
        return result.stdout.strip()
    except Exception:
        return "unknown"


def extract_module_anchor(name):
    """Return a source module cluster key from a signal or clock name."""
    if not name:
        return "unknown"
    # Drop Verilog path prefixes that are common across the SoC.
    cleaned = re.sub(r"^(endpoint__DOT__|cpu__DOT__l_soc__DOT__core_with_l2__DOT__core__DOT__)", "", name)
    # First meaningful module token.
    parts = cleaned.split("__DOT__")
    for part in parts:
        if part and not part.endswith("_clock") and part != "clock":
            return part
    return parts[0] if parts else "unknown"


def dominant_module_anchor(names):
    """Most common module anchor among a list of hierarchical names."""
    if not names:
        return "unknown"
    counts = Counter(extract_module_anchor(n) for n in names)
    return counts.most_common(1)[0][0]


def node_kind_histogram(task):
    kinds = task.get("node_kinds") or {}
    total = sum(v for v in kinds.values() if isinstance(v, int))
    if not total:
        return {}
    return {k: v / total for k, v in sorted(kinds.items(), key=lambda x: -x[1]) if isinstance(v, int)}


def node_shape_key(task):
    """A string key summarising the dominant node kinds."""
    kinds = task.get("node_kinds") or {}
    return "/".join(f"{k}:{v}" for k, v in sorted(kinds.items()) if isinstance(v, int))


def state_target_prefixes(names, depth=3):
    """Return common hierarchical prefix segments of state target names."""
    if not names:
        return []
    segments = [n.split("__DOT__")[:depth] for n in names]
    prefix = segments[0]
    for seg in segments[1:]:
        new_prefix = []
        for a, b in zip(prefix, seg):
            if a == b:
                new_prefix.append(a)
            else:
                break
        prefix = new_prefix
        if not prefix:
            break
    return prefix


def classify_memory_dynamic_or_array_index(task, schedule_task):
    """Sub-classify a memory_dynamic_or_array_index blocker."""
    boundary = schedule_task.get("boundary") or {}
    state_update = schedule_task.get("state_update") or {}
    serial_reasons = set(schedule_task.get("serial_reasons") or [])
    node_kinds = schedule_task.get("node_kinds") or {}

    has_mem_write = boundary.get("has_memory_write") or "memory_write" in serial_reasons
    has_state_update = state_update.get("has_state_update") or boundary.get("has_state_update") or "state_update" in serial_reasons
    has_reset = boundary.get("has_reset") or "reset" in serial_reasons

    # Look at node-kind composition.
    reg_dst = node_kinds.get("NODE_REG_DST", 0)
    reg_src = node_kinds.get("NODE_REG_SRC", 0)
    others = node_kinds.get("NODE_OTHERS", 0)
    total = reg_dst + reg_src + others

    if has_mem_write:
        return "memory_write_or_port_conflict"

    if has_state_update and state_update.get("has_memory_or_dynamic_array"):
        # State update that uses a dynamic-array / mux structure for its RHS.
        # If the write target is a register bank and there is no true memory
        # write, the serial reason is often the dynamic index into a vector
        # of registers, not a memory port.
        if reg_dst > 0 and not has_mem_write:
            return "array_or_mux_compute"
        return "true_memory_read"

    if not has_state_update and not has_mem_write:
        # Pure read/compute through a dynamic index or memory-like array.
        if reg_dst == 0:
            return "read_only_dynamic_index"
        return "array_or_mux_compute"

    if has_reset and not has_mem_write:
        return "array_or_mux_compute"

    return "unknown"


def classify_state_update_unknown_rhs_timing(task, schedule_task):
    """Sub-classify a state_update_unknown_rhs_timing blocker."""
    state_update = schedule_task.get("state_update") or {}
    group = schedule_task.get("state_update_group") or {}
    boundary = schedule_task.get("boundary") or {}
    serial_reasons = set(schedule_task.get("serial_reasons") or [])

    evidence = state_update.get("rhs_timing_evidence", "")
    same_cycle = state_update.get("rhs_reads_same_cycle_target", False)
    candidate_kind = state_update.get("candidate_kind", "")
    block_reasons = set(state_update.get("block_reasons") or [])
    runtime_blocks = set(group.get("runtime_block_reasons") or [])

    if boundary.get("has_reset") or state_update.get("has_reset_behavior") or state_update.get("has_async_reset_behavior"):
        return "reset_or_duplicate_writer_blocked"

    if "target_multi_writer_unproven" in runtime_blocks or "target_multi_target_unproven" in runtime_blocks:
        if candidate_kind != "blocked" or "rhs_timing_unknown" not in block_reasons:
            return "reset_or_duplicate_writer_blocked"

    if same_cycle:
        # RHS reads the target in the same cycle; if the read is via a
        # delayed/dequeued register slice it may still be precomputable.
        if evidence in {"single_target_old_state", "old_state_only"}:
            return "old_state_only_candidate"
        # Multiple targets reading their own previous value is a compute
        # pattern that often expands to old-state-only after target slicing.
        if evidence == "multiple_state_targets" and state_update.get("state_target_count", 0) > 1:
            return "rhs_compute_can_be_precomputed"
        return "unknown"

    # No same-cycle target read.
    if evidence in {"single_target_old_state", "old_state_only"}:
        return "old_state_only_candidate"

    # Candidate kind says safe but RHS timing not proven -> precomputable compute.
    if candidate_kind == "safe_candidate" and "rhs_timing_unknown" in block_reasons:
        return "rhs_compute_can_be_precomputed"

    # Commit-only: state update with no same-cycle read and no reset.
    if not same_cycle and not state_update.get("has_reset_behavior"):
        return "commit_only"

    return "unknown"


def triage_memory_subclass(subcls, task, schedule_task):
    """Return triage result and recoverable fraction for a memory sub-class."""
    state_update = schedule_task.get("state_update") or {}
    boundary = schedule_task.get("boundary") or {}

    if subcls == "read_only_dynamic_index":
        # Pure read port with dynamic index; independence proof can release it.
        return "repairable_metadata_gap", 0.80
    if subcls == "array_or_mux_compute":
        # Dynamic index into a vector of registers/muxes; often a classification
        # gap once the index is proven independent or the structure is recognised
        # as a register bank rather than a memory.
        if not boundary.get("has_memory_write"):
            return "classification_too_coarse", 0.60
        return "true_serial", 0.0
    if subcls == "true_memory_read":
        # True memory read can be parallelised only with buffered-memory semantics.
        return "needs_source_instrumentation", 0.10
    if subcls == "memory_write_or_port_conflict":
        return "true_serial", 0.0
    return "needs_source_instrumentation", 0.05


def triage_state_update_subclass(subcls, task, schedule_task):
    """Return triage result and recoverable fraction for a state-update sub-class."""
    group = schedule_task.get("state_update_group") or {}

    if subcls == "rhs_compute_can_be_precomputed":
        # RHS is pure compute that can be lifted before the state update.
        return "repairable_metadata_gap", 0.70
    if subcls == "old_state_only_candidate":
        # Evidence already points to old-state-only; missing expansion proof.
        if group.get("target_writer_universe_complete") and group.get("target_writer_conflict_kind") != "multi_writer_unproven":
            return "repairable_metadata_gap", 0.75
        return "repairable_metadata_gap", 0.50
    if subcls == "commit_only":
        # No same-cycle read and no reset -> safe under two-phase commit.
        if group.get("target_writer_conflict_kind") not in {"multi_writer_unproven", "multi_target_unproven"}:
            return "repairable_metadata_gap", 0.80
        return "classification_too_coarse", 0.30
    if subcls == "reset_or_duplicate_writer_blocked":
        return "true_serial", 0.0
    return "needs_source_instrumentation", 0.05


def wall_time_estimate(recoverable_cost, total_cycles):
    """Very rough wall-time opportunity in ms, assuming 1 cost unit ~ 1 ns on a fast core.

    Marked approximate; not a performance claim.
    """
    if recoverable_cost <= 0:
        return 0.0
    # The schedule runs for total_cycles; recoverable cost is static*active_count.
    # Treat it as cumulative work units.  A coarse scaling factor is used so the
    # number is interpretable as order-of-magnitude only.
    estimate_ns = recoverable_cost * 0.5
    return round(estimate_ns / 1e6, 2)


def cluster_key_for_task(task, schedule_task):
    """Source/codegen cluster key combining module anchor and node shape."""
    su = schedule_task.get("state_update") or {}
    names = su.get("state_target_names") or schedule_task.get("boundary", {}).get("clock_names") or []
    anchor = dominant_module_anchor(names)
    shape = node_shape_key(schedule_task)
    return f"{anchor}::{shape}"


def build_graph(tasks):
    tasks_by_id = {t["cpp_id"]: t for t in tasks}
    preds = {t["cpp_id"]: list(t.get("pred_cpp_ids") or []) for t in tasks}
    succs = {t["cpp_id"]: list(t.get("succ_cpp_ids") or []) for t in tasks}
    return tasks_by_id, preds, succs


def is_root_or_sink(cpp_id, preds, succs, tasks_by_id):
    """Return one of root/sink/internal based on graph position."""
    pred_count = sum(1 for p in preds.get(cpp_id, []) if p in tasks_by_id)
    succ_count = sum(1 for s in succs.get(cpp_id, []) if s in tasks_by_id)
    if pred_count == 0:
        return "root"
    if succ_count == 0:
        return "sink"
    return "internal"


def neighbor_sample(cpp_id, preds, succs, tasks_by_id, n=3):
    """Return a few predecessor/successor cppIds that are in the schedule."""
    out = {}
    out["predecessors"] = [p for p in preds.get(cpp_id, []) if p in tasks_by_id][:n]
    out["successors"] = [s for s in succs.get(cpp_id, []) if s in tasks_by_id][:n]
    return out


def source_anchor(schedule_task):
    """Best available source anchor: clock or state target prefix."""
    su = schedule_task.get("state_update") or {}
    names = su.get("state_target_names") or []
    if names:
        prefix = state_target_prefixes(names)
        return "__DOT__".join(prefix) if prefix else names[0]
    clocks = schedule_task.get("boundary", {}).get("clock_names") or []
    return clocks[0] if clocks else "unknown"


def cluster_rows_for_subreason(rows, tasks_by_id, preds, succs, subreason):
    """Group top rows into source/codegen clusters and produce cluster summaries."""
    clusters = defaultdict(list)
    for row in rows:
        cpp_id = row["cpp_id"]
        task = tasks_by_id.get(cpp_id)
        if task is None:
            continue
        key = cluster_key_for_task(row, task)
        clusters[key].append((row, task))

    cluster_reports = []
    for key, items in sorted(clusters.items(), key=lambda x: -sum(r["expected_active_cost"] for r, _ in x[1])):
        cluster_cost = sum(r["expected_active_cost"] for r, _ in items)
        cluster_static = sum(r["static_cost"] for r, _ in items)
        cluster_active = sum(r["active_count"] for r, _ in items)
        representative_rows = []
        node_kind_totals = Counter()
        subclass_counter = Counter()
        triage_counter = Counter()
        recoverable_cost = 0
        anchors = []

        for row, task in items:
            if subreason == "memory_dynamic_or_array_index":
                subcls = classify_memory_dynamic_or_array_index(row, task)
                triage, frac = triage_memory_subclass(subcls, row, task)
            else:
                subcls = classify_state_update_unknown_rhs_timing(row, task)
                triage, frac = triage_state_update_subclass(subcls, row, task)
            subclass_counter[subcls] += row["expected_active_cost"]
            triage_counter[triage] += row["expected_active_cost"]
            recoverable_cost += int(row["expected_active_cost"] * frac)
            node_kind_totals.update(task.get("node_kinds") or {})
            anchors.append(source_anchor(task))

            representative_rows.append({
                "cpp_id": row["cpp_id"],
                "super_id": row["super_id"],
                "static_cost": row["static_cost"],
                "active_count": row["active_count"],
                "expected_active_cost": row["expected_active_cost"],
                "sub_class": subcls,
                "triage": triage,
                "recoverable_fraction": frac,
                "graph_role": is_root_or_sink(row["cpp_id"], preds, succs, tasks_by_id),
                "neighbors": neighbor_sample(row["cpp_id"], preds, succs, tasks_by_id),
                "source_anchor": source_anchor(task),
                "state_target_count": (task.get("state_update") or {}).get("state_target_count", 0),
                "rhs_timing_evidence": (task.get("state_update") or {}).get("rhs_timing_evidence", ""),
                "rhs_reads_same_cycle_target": (task.get("state_update") or {}).get("rhs_reads_same_cycle_target", False),
                "has_memory_or_dynamic_array": (task.get("state_update") or {}).get("has_memory_or_dynamic_array", False),
                "has_reset_behavior": (task.get("state_update") or {}).get("has_reset_behavior", False),
            })

        representative_rows.sort(key=lambda r: -r["expected_active_cost"])
        # Keep enough representatives to cover ~80% of cluster cost.
        coverage = 0
        kept = []
        for r in representative_rows:
            kept.append(r)
            coverage += r["expected_active_cost"]
            if coverage >= cluster_cost * 0.80 and len(kept) >= 3:
                break
        if len(kept) < 3 and representative_rows:
            kept = representative_rows[:3]

        cluster_reports.append({
            "cluster_key": key,
            "subreason": subreason,
            "cluster_expected_active_cost": cluster_cost,
            "cluster_static_cost": cluster_static,
            "cluster_active_count": cluster_active,
            "row_count": len(items),
            "node_kind_totals": dict(node_kind_totals),
            "sub_class_distribution": dict(subclass_counter),
            "triage_distribution": dict(triage_counter),
            "recoverable_expected_cost": recoverable_cost,
            "common_source_anchors": [a for a, _ in Counter(anchors).most_common(3)],
            "representative_rows": kept,
        })

    cluster_reports.sort(key=lambda c: -c["cluster_expected_active_cost"])
    return cluster_reports


def summarize_family(cluster_reports, subreason, total_expected_active_cost, total_cycles):
    total_family_cost = sum(c["cluster_expected_active_cost"] for c in cluster_reports)
    total_recoverable = sum(c["recoverable_expected_cost"] for c in cluster_reports)
    triage_totals = Counter()
    subcls_totals = Counter()
    for c in cluster_reports:
        for t, v in c["triage_distribution"].items():
            triage_totals[t] += v
        for s, v in c["sub_class_distribution"].items():
            subcls_totals[s] += v
    return {
        "subreason": subreason,
        "family_expected_active_cost": total_family_cost,
        "family_share_of_total_expected_active_cost": total_family_cost / total_expected_active_cost if total_expected_active_cost else 0.0,
        "recoverable_expected_cost": total_recoverable,
        "recoverable_share_of_total_expected_active_cost": total_recoverable / total_expected_active_cost if total_expected_active_cost else 0.0,
        "recoverable_wall_time_estimate_ms": wall_time_estimate(total_recoverable, total_cycles),
        "triage_distribution": dict(triage_totals),
        "sub_class_distribution": dict(subcls_totals),
        "cluster_count": len(cluster_reports),
        "clusters": cluster_reports,
    }


def decide_24x_b(families, total_expected_active_cost):
    total_recoverable = sum(f["recoverable_expected_cost"] for f in families)
    share = total_recoverable / total_expected_active_cost if total_expected_active_cost else 0.0
    threshold_low = 0.20
    threshold_high = 0.30

    # Also check for a single large repeated cluster.
    largest_cluster_recoverable = 0
    for family in families:
        for cluster in family["clusters"]:
            if cluster["recoverable_expected_cost"] > largest_cluster_recoverable:
                largest_cluster_recoverable = cluster["recoverable_expected_cost"]

    if share >= threshold_high:
        decision = "continue_local_metadata_repair"
        reason = f"source-backed recoverable cost {share:.2%} meets high threshold {threshold_high:.0%}"
    elif share >= threshold_low:
        decision = "continue_local_metadata_repair_with_focused_scope"
        reason = f"recoverable cost {share:.2%} meets low threshold {threshold_low:.0%} but is below high threshold {threshold_high:.0%}"
    elif largest_cluster_recoverable / total_expected_active_cost >= 0.05:
        decision = "continue_local_metadata_repair_single_cluster"
        reason = "one repeated cluster releases a large enough share to justify a focused repair"
    else:
        decision = "pivot"
        reason = f"recoverable cost {share:.2%} is below {threshold_low:.0%}-{threshold_high:.0%} threshold; stop local metadata-repair line"

    return {
        "decision": decision,
        "reason": reason,
        "total_recoverable_expected_cost": total_recoverable,
        "recoverable_share_of_total_expected_active_cost": share,
        "threshold_range": [threshold_low, threshold_high],
        "largest_single_cluster_recoverable_cost": largest_cluster_recoverable,
    }


def markdown_table(headers, rows):
    lines = ["| " + " | ".join(headers) + " |", "| " + " | ".join(["---"] * len(headers)) + " |"]
    lines.extend("| " + " | ".join(str(v) for v in row) + " |" for row in rows)
    return "\n".join(lines)


def fmt(value):
    if isinstance(value, float):
        return f"{value:.4g}"
    return str(value)


def write_markdown(result, path):
    lines = [
        "# GSim-MT 24X Top-Blocker Source-Cluster Audit",
        "",
        "> Report-only. Does not implement a runtime code path, memory parallelization,",
        "> state-update commit, KaHyPar integration, or a full two-phase RepCut runtime.",
        "",
        "## Inputs",
        "",
        markdown_table(
            ["field", "value"],
            [
                ["blocker_inspection_json", result["inputs"]["blocker_inspection_json"]],
                ["schedule_json", result["inputs"]["schedule_json"]],
                ["source_commit", result["inputs"]["source_commit"]],
            ],
        ),
        "",
        "## Totals",
        "",
        markdown_table(
            ["metric", "value"],
            [
                ["total_expected_active_cost", result["totals"]["total_expected_active_cost"]],
                ["audited_expected_active_cost", result["totals"]["audited_expected_active_cost"]],
                ["audited_share_of_total", fmt(result["totals"]["audited_share_of_total_expected_active_cost"])],
                ["recoverable_expected_active_cost", result["totals"]["recoverable_expected_active_cost"]],
                ["recoverable_share_of_total", fmt(result["totals"]["recoverable_share_of_total_expected_active_cost"])],
            ],
        ),
        "",
        "## 24X-B Decision",
        "",
        markdown_table(
            ["field", "value"],
            [
                ["decision", result["decision"]["decision"]],
                ["reason", result["decision"]["reason"]],
                ["total_recoverable_expected_cost", result["decision"]["total_recoverable_expected_cost"]],
                ["recoverable_share_of_total", fmt(result["decision"]["recoverable_share_of_total_expected_active_cost"])],
                ["threshold_range", f"{result['decision']['threshold_range'][0]:.0%}-{result['decision']['threshold_range'][1]:.0%}"],
                ["largest_single_cluster_recoverable_cost", result["decision"]["largest_single_cluster_recoverable_cost"]],
            ],
        ),
        "",
    ]

    for family in result["families"]:
        lines.extend([
            f"## Family: {family['subreason']}",
            "",
            markdown_table(
                ["metric", "value"],
                [
                    ["family_expected_active_cost", family["family_expected_active_cost"]],
                    ["family_share_of_total", fmt(family["family_share_of_total_expected_active_cost"])],
                    ["recoverable_expected_active_cost", family["recoverable_expected_cost"]],
                    ["recoverable_share_of_total", fmt(family["recoverable_share_of_total_expected_active_cost"])],
                    ["recoverable_wall_time_estimate_ms", f"{family['recoverable_wall_time_estimate_ms']} (approximate)"],
                    ["cluster_count", family["cluster_count"]],
                ],
            ),
            "",
            "### Sub-class Distribution",
            "",
            markdown_table(
                ["sub_class", "expected_active_cost"],
                [[k, v] for k, v in family["sub_class_distribution"].items()],
            ),
            "",
            "### Triage Distribution",
            "",
            markdown_table(
                ["triage", "expected_active_cost"],
                [[k, v] for k, v in family["triage_distribution"].items()],
            ),
            "",
            "### Top Clusters",
            "",
            markdown_table(
                ["cluster_key", "rows", "family_cost", "recoverable", "triage_mix"],
                [
                    [
                        c["cluster_key"],
                        c["row_count"],
                        c["cluster_expected_active_cost"],
                        c["recoverable_expected_cost"],
                        ", ".join(f"{k}={v}" for k, v in c["triage_distribution"].items()),
                    ]
                    for c in family["clusters"][:10]
                ],
            ),
            "",
        ])

    lines.extend([
        "## Method Notes",
        "",
        "- Cluster key = dominant source module anchor + sorted node-kind histogram.",
        "- `recoverable_expected_cost` is the expected active cost that could become safely parallel under a concrete, source-backed proof rule.",
        "- `recoverable_wall_time_estimate_ms` is an order-of-magnitude approximation, not a performance claim.",
        "- Clusters account for the top blocked rows of each target subreason; share of total is computed against the full XiangShan total.",
        "",
    ])

    path.write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--blocker-inspection-json", type=Path, required=True)
    parser.add_argument("--schedule-json", type=Path, required=True)
    parser.add_argument("--json-out", type=Path, required=True)
    parser.add_argument("--md-out", type=Path, required=True)
    parser.add_argument("--top", type=int, default=TOP_N)
    parser.add_argument("--subreasons", default=",".join(TARGET_SUBREASONS))
    args = parser.parse_args()

    expect(args.blocker_inspection_json.is_file(), f"missing {args.blocker_inspection_json}")
    expect(args.schedule_json.is_file(), f"missing {args.schedule_json}")

    inspection = read_json(args.blocker_inspection_json)
    schedule = read_json(args.schedule_json)

    expect(inspection.get("format") == "gsim.mt-23x-blocker-inspection.v1", "unexpected blocker inspection format")
    expect(schedule.get("format") in {"gsim.mt-schedule.v1", "test.schedule.v1"} or "tasks" in schedule, "unexpected schedule format")

    tasks = schedule["tasks"]
    tasks_by_id, preds, succs = build_graph(tasks)

    total_expected_active_cost = inspection["totals"]["total_expected_active_cost"]
    total_cycles = inspection["profile"]["cycles"]

    target_subreasons = {s.strip() for s in args.subreasons.split(",") if s.strip()}
    rows_by_subreason = defaultdict(list)
    for row in inspection["top_blocked_candidates"]:
        subreason = row.get("blocker_subreason")
        if subreason in target_subreasons:
            rows_by_subreason[subreason].append(row)

    # Keep top N per subreason by expected active cost.
    for subreason in rows_by_subreason:
        rows_by_subreason[subreason].sort(key=lambda r: -r["expected_active_cost"])
        rows_by_subreason[subreason] = rows_by_subreason[subreason][: args.top]

    families = []
    for subreason in sorted(target_subreasons):
        rows = rows_by_subreason.get(subreason, [])
        clusters = cluster_rows_for_subreason(rows, tasks_by_id, preds, succs, subreason)
        families.append(summarize_family(clusters, subreason, total_expected_active_cost, total_cycles))

    audited_cost = sum(f["family_expected_active_cost"] for f in families)
    audited_recoverable = sum(f["recoverable_expected_cost"] for f in families)

    result = {
        "format": "gsim.mt-24x-top-blocker-source-audit.v1",
        "inputs": {
            "blocker_inspection_json": str(args.blocker_inspection_json),
            "schedule_json": str(args.schedule_json),
            "source_commit": git_commit(Path(__file__).resolve().parents[1]),
        },
        "totals": {
            "total_expected_active_cost": total_expected_active_cost,
            "audited_expected_active_cost": audited_cost,
            "audited_share_of_total_expected_active_cost": audited_cost / total_expected_active_cost if total_expected_active_cost else 0.0,
            "recoverable_expected_active_cost": audited_recoverable,
            "recoverable_share_of_total_expected_active_cost": audited_recoverable / total_expected_active_cost if total_expected_active_cost else 0.0,
        },
        "families": families,
        "decision": decide_24x_b(families, total_expected_active_cost),
    }

    args.json_out.parent.mkdir(parents=True, exist_ok=True)
    args.md_out.parent.mkdir(parents=True, exist_ok=True)
    args.json_out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    write_markdown(result, args.md_out)

    print(
        "mt-24x-top-blocker-source-audit ok "
        f"audited={result['totals']['audited_expected_active_cost']} "
        f"recoverable={result['totals']['recoverable_expected_active_cost']} "
        f"decision={result['decision']['decision']}"
    )


if __name__ == "__main__":
    main()
