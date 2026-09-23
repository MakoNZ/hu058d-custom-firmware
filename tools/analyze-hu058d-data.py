#!/usr/bin/env python3
"""
Analyze an HU-058D data export produced by tools/export-hu058d-data.sh.

Accepts either the timestamped .tar.gz archive or an already-extracted export
directory. Produces a Markdown summary, machine-readable JSON, CSV diagnostics,
and PNG graphs.

Dependencies:
    numpy
    pandas
    matplotlib

On Arch Linux:
    sudo pacman -S python-pandas python-matplotlib
"""

from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import sys
import tarfile
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from zoneinfo import ZoneInfo

try:
    import numpy as np
    import pandas as pd
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError as exc:
    raise SystemExit(
        "Missing Python dependency. On Arch Linux install:\n"
        "  sudo pacman -S python-pandas python-matplotlib\n"
        f"\nImport error: {exc}"
    )


DEFAULT_TZ = "Pacific/Auckland"


def fmt_duration(seconds: float | int | None) -> str:
    if seconds is None or not math.isfinite(float(seconds)):
        return "unknown"
    seconds = max(0, int(round(float(seconds))))
    days, rem = divmod(seconds, 86400)
    hours, rem = divmod(rem, 3600)
    minutes, secs = divmod(rem, 60)
    parts = []
    if days:
        parts.append(f"{days}d")
    if days or hours:
        parts.append(f"{hours}h")
    if days or hours or minutes:
        parts.append(f"{minutes}m")
    parts.append(f"{secs}s")
    return " ".join(parts)


def parse_manifest(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    if not path.exists():
        return out
    for raw in path.read_text(errors="replace").splitlines():
        if "=" in raw:
            key, value = raw.split("=", 1)
            out[key.strip()] = value.strip()
    return out


def safe_extract(archive: Path, destination: Path) -> None:
    root = destination.resolve()
    with tarfile.open(archive, "r:gz") as tf:
        for member in tf.getmembers():
            target = (destination / member.name).resolve()
            if target != root and root not in target.parents:
                raise RuntimeError(f"Unsafe archive path: {member.name}")
        tf.extractall(destination)


def find_export_root(path: Path) -> tuple[Path, tempfile.TemporaryDirectory | None]:
    path = path.expanduser().resolve()
    if path.is_dir():
        if (path / "manifest.txt").exists():
            return path, None
        children = [p for p in path.iterdir() if p.is_dir() and (p / "manifest.txt").exists()]
        if len(children) == 1:
            return children[0], None
        raise RuntimeError(f"Could not identify export root inside {path}")

    if not path.exists():
        raise RuntimeError(f"Input does not exist: {path}")
    if not (path.name.endswith(".tar.gz") or path.suffix == ".tgz"):
        raise RuntimeError("Input must be an HU-058D export directory or .tar.gz archive")

    temp = tempfile.TemporaryDirectory(prefix="hu058d-analysis-")
    temp_path = Path(temp.name)
    safe_extract(path, temp_path)

    candidates = [p for p in temp_path.rglob("manifest.txt")]
    if len(candidates) != 1:
        temp.cleanup()
        raise RuntimeError(
            f"Expected exactly one manifest.txt in archive, found {len(candidates)}"
        )
    return candidates[0].parent, temp


def load_csv(path: Path) -> pd.DataFrame | None:
    if not path.exists():
        return None
    df = pd.read_csv(path)
    if "created_at" in df.columns:
        df["created_at"] = pd.to_datetime(df["created_at"], utc=True, errors="coerce")
        df = df.dropna(subset=["created_at"]).sort_values("created_at").reset_index(drop=True)
    return df


def bool_series(series: pd.Series) -> pd.Series:
    if pd.api.types.is_bool_dtype(series):
        return series.fillna(False)
    return (
        series.astype(str)
        .str.strip()
        .str.lower()
        .map({"true": True, "false": False, "1": True, "0": False})
        .fillna(False)
    )


def linear_slope_per_hour(times: pd.Series, values: pd.Series) -> float | None:
    if len(values) < 2:
        return None
    t0 = times.iloc[0]
    x = (times - t0).dt.total_seconds().to_numpy(dtype=float) / 3600.0
    y = pd.to_numeric(values, errors="coerce").to_numpy(dtype=float)
    mask = np.isfinite(x) & np.isfinite(y)
    if mask.sum() < 2 or np.ptp(x[mask]) == 0:
        return None
    return float(np.polyfit(x[mask], y[mask], 1)[0])


def percentile(values: pd.Series, q: float) -> float | None:
    x = pd.to_numeric(values, errors="coerce").dropna()
    return float(np.percentile(x, q)) if len(x) else None


def value_or_none(value: Any) -> Any:
    if isinstance(value, (np.integer,)):
        return int(value)
    if isinstance(value, (np.floating,)):
        value = float(value)
        return value if math.isfinite(value) else None
    if pd.isna(value) if not isinstance(value, (dict, list, str, bool)) else False:
        return None
    return value


def localize(series: pd.Series, timezone: str) -> pd.Series:
    return series.dt.tz_convert(ZoneInfo(timezone))


@dataclass
class Check:
    name: str
    passed: bool
    detail: str


def add_check(checks: list[Check], name: str, condition: bool, detail: str) -> None:
    checks.append(Check(name=name, passed=bool(condition), detail=detail))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Analyze an HU-058D Gladys/clock telemetry export."
    )
    parser.add_argument("input", type=Path, help="Export .tar.gz or extracted directory")
    parser.add_argument(
        "-o", "--output", type=Path,
        help="Report directory (default: <input-name>-report beside the input)"
    )
    parser.add_argument(
        "--timezone", default=DEFAULT_TZ,
        help=f"IANA timezone for graph labels (default: {DEFAULT_TZ})"
    )
    parser.add_argument(
        "--recent-hours", type=float, default=48.0,
        help="Window used for recent heap statistics (default: 48)"
    )
    args = parser.parse_args()

    try:
        ZoneInfo(args.timezone)
    except Exception as exc:
        raise SystemExit(f"Invalid timezone {args.timezone!r}: {exc}")

    export_root, temp = find_export_root(args.input)
    try:
        input_name = args.input.name
        for suffix in (".tar.gz", ".tgz"):
            if input_name.endswith(suffix):
                input_name = input_name[:-len(suffix)]
                break
        output = args.output or (args.input.resolve().parent / f"{input_name}-report")
        output.mkdir(parents=True, exist_ok=True)

        manifest = parse_manifest(export_root / "manifest.txt")
        status_path = export_root / "clock-status.json"
        status: dict[str, Any] = {}
        if status_path.exists():
            status = json.loads(status_path.read_text())

        gladys = export_root / "gladys"
        uptime = load_csv(gladys / "uptime.csv")
        heap = load_csv(gladys / "free-heap.csv")
        rssi = load_csv(gladys / "rssi.csv")
        drift_gladys = load_csv(gladys / "drift.csv")
        drift_avg_gladys = load_csv(gladys / "drift-average.csv")
        ntp = load_csv(export_root / "clock-ntp-history.csv")

        # Determine current boot start. Prefer the live status uptime because it
        # describes the same boot as the in-RAM NTP history.
        boot_time = None
        current_uptime_s = None
        if status.get("uptime_ms") is not None and uptime is not None and len(uptime):
            current_uptime_s = float(status["uptime_ms"]) / 1000.0
            boot_time = uptime["created_at"].iloc[-1] - pd.to_timedelta(
                float(uptime["value"].iloc[-1]), unit="s"
            )
        elif uptime is not None and len(uptime):
            current_uptime_s = float(uptime["value"].iloc[-1])
            boot_time = uptime["created_at"].iloc[-1] - pd.to_timedelta(
                current_uptime_s, unit="s"
            )

        def post_boot(df: pd.DataFrame | None) -> pd.DataFrame | None:
            if df is None or boot_time is None or "created_at" not in df:
                return df
            # Allow two minutes around the calculated start because MQTT and
            # Gladys timestamps are not perfectly simultaneous.
            return df[df["created_at"] >= boot_time - pd.to_timedelta(120, unit="s")].copy()

        uptime_post = post_boot(uptime)
        heap_post = post_boot(heap)
        rssi_post = post_boot(rssi)
        drift_post = post_boot(drift_gladys)
        drift_avg_post = post_boot(drift_avg_gladys)

        report: dict[str, Any] = {
            "input": str(args.input),
            "export_root": export_root.name,
            "manifest": manifest,
            "firmware": status.get("firmware"),
            "build": status.get("build"),
            "git_commit": manifest.get("git_commit"),
            "git_branch": manifest.get("git_branch"),
            "timezone": args.timezone,
        }

        # Uptime / reboot / telemetry cadence.
        reboot_events = 0
        if uptime is not None and len(uptime) >= 2:
            delta_values = pd.to_numeric(uptime["value"], errors="coerce").diff()
            reboot_events = int((delta_values < -30).sum())

        cadence: dict[str, Any] = {}
        if uptime_post is not None and len(uptime_post) >= 2:
            gaps = uptime_post["created_at"].diff().dt.total_seconds().dropna()
            cadence = {
                "samples": int(len(uptime_post)),
                "median_interval_s": float(gaps.median()),
                "max_gap_s": float(gaps.max()),
                "gaps_over_90s": int((gaps > 90).sum()),
                "count_value_max": (
                    int(pd.to_numeric(uptime_post["count_value"], errors="coerce").max())
                    if "count_value" in uptime_post else None
                ),
            }

        uptime_info = {
            "current_uptime_s": current_uptime_s,
            "current_uptime": fmt_duration(current_uptime_s),
            "boot_time_utc": boot_time.isoformat() if boot_time is not None else None,
            "reboot_events_in_export_window": reboot_events,
            "wifi_connected_s": status.get("wifi_connected_s"),
            "mqtt_publish_count": status.get("mqtt_publish_count"),
            "telemetry": cadence,
        }
        if current_uptime_s is not None and status.get("wifi_connected_s") is not None:
            uptime_info["uptime_minus_wifi_connected_s"] = (
                float(current_uptime_s) - float(status["wifi_connected_s"])
            )
        report["uptime"] = uptime_info

        # NTP / drift.
        ntp_info: dict[str, Any] = {
            "state": status.get("ntp_state"),
            "sync_count": status.get("ntp_sync_count"),
            "failure_count": status.get("ntp_failure_count"),
            "history_count": status.get("ntp_history_count"),
            "history_capacity": status.get("ntp_history_capacity"),
            "reported_weighted_drift_ppm": status.get("ntp_drift_avg_ppm"),
        }

        valid_ntp = None
        outliers = pd.DataFrame()
        peer_stats = pd.DataFrame()
        if ntp is not None and len(ntp):
            if "drift_valid" in ntp:
                valid_mask = bool_series(ntp["drift_valid"])
            else:
                valid_mask = pd.to_numeric(ntp.get("interval_s"), errors="coerce") >= 3000

            valid_ntp = ntp[valid_mask].copy()
            valid_ntp["drift_ppm"] = pd.to_numeric(valid_ntp["drift_ppm"], errors="coerce")
            valid_ntp["correction_ms"] = pd.to_numeric(valid_ntp["correction_ms"], errors="coerce")
            valid_ntp["interval_s"] = pd.to_numeric(valid_ntp["interval_s"], errors="coerce")
            valid_ntp = valid_ntp.dropna(subset=["drift_ppm", "correction_ms", "interval_s"])

            if len(valid_ntp):
                weighted = (
                    -valid_ntp["correction_ms"].sum() * 1000.0
                    / valid_ntp["interval_s"].sum()
                )
                med = float(valid_ntp["drift_ppm"].median())
                mad = float(np.median(np.abs(valid_ntp["drift_ppm"] - med)))

                if mad > 0:
                    robust_z = 0.67448975 * np.abs(valid_ntp["drift_ppm"] - med) / mad
                    outlier_mask = robust_z > 3.5
                else:
                    outlier_mask = np.zeros(len(valid_ntp), dtype=bool)

                outliers = valid_ntp.loc[outlier_mask].copy()
                outliers["deviation_from_median_ppm"] = (
                    outliers["drift_ppm"] - med
                )

                ntp_info.update({
                    "valid_samples": int(len(valid_ntp)),
                    "weighted_drift_ppm": float(weighted),
                    "median_drift_ppm": med,
                    "mad_ppm": mad,
                    "outlier_count": int(len(outliers)),
                    "short_filtered_samples_in_current_history": int((~valid_mask).sum()),
                })

                if "server_ip" in valid_ntp:
                    peer_stats = (
                        valid_ntp.groupby("server_ip", dropna=False)
                        .agg(
                            samples=("drift_ppm", "size"),
                            mean_drift_ppm=("drift_ppm", "mean"),
                            median_drift_ppm=("drift_ppm", "median"),
                            stddev_ppm=("drift_ppm", "std"),
                            mean_correction_ms=("correction_ms", "mean"),
                        )
                        .reset_index()
                        .sort_values(["samples", "server_ip"], ascending=[False, True])
                    )
                    for col in [
                        "mean_drift_ppm", "median_drift_ppm",
                        "stddev_ppm", "mean_correction_ms"
                    ]:
                        peer_stats[col] = peer_stats[col].round(3)

        report["ntp"] = ntp_info

        # Heap.
        heap_info: dict[str, Any] = {}
        if heap_post is not None and len(heap_post):
            heap_post["value"] = pd.to_numeric(heap_post["value"], errors="coerce")
            heap_post = heap_post.dropna(subset=["value"])
            if len(heap_post):
                recent_cutoff = heap_post["created_at"].max() - pd.to_timedelta(
                    args.recent_hours, unit="h"
                )
                heap_recent = heap_post[heap_post["created_at"] >= recent_cutoff].copy()
                slope = linear_slope_per_hour(
                    heap_recent["created_at"], heap_recent["value"]
                )
                heap_info = {
                    "samples_current_boot": int(len(heap_post)),
                    "minimum_bytes_current_boot": float(heap_post["value"].min()),
                    "recent_hours": args.recent_hours,
                    "recent_samples": int(len(heap_recent)),
                    "recent_mean_bytes": float(heap_recent["value"].mean()),
                    "recent_median_bytes": float(heap_recent["value"].median()),
                    "recent_p01_bytes": percentile(heap_recent["value"], 1),
                    "recent_p95_bytes": percentile(heap_recent["value"], 95),
                    "recent_minimum_bytes": float(heap_recent["value"].min()),
                    "recent_maximum_bytes": float(heap_recent["value"].max()),
                    "recent_linear_trend_bytes_per_hour": slope,
                }
        report["heap"] = heap_info

        # RSSI.
        rssi_info: dict[str, Any] = {}
        if rssi_post is not None and len(rssi_post):
            rssi_post["value"] = pd.to_numeric(rssi_post["value"], errors="coerce")
            rssi_post = rssi_post.dropna(subset=["value"])
            if len(rssi_post):
                rssi_info = {
                    "samples_current_boot": int(len(rssi_post)),
                    "mean_dbm": float(rssi_post["value"].mean()),
                    "median_dbm": float(rssi_post["value"].median()),
                    "p05_dbm": percentile(rssi_post["value"], 5),
                    "p95_dbm": percentile(rssi_post["value"], 95),
                    "minimum_dbm": float(rssi_post["value"].min()),
                    "maximum_dbm": float(rssi_post["value"].max()),
                }
        report["rssi"] = rssi_info

        # Automated evidence gate. These are deliberately simple, documented
        # heuristics rather than a magical "production-ready" oracle.
        checks: list[Check] = []
        if current_uptime_s is not None:
            add_check(
                checks, "72-hour soak",
                current_uptime_s >= 72 * 3600,
                f"current uptime {fmt_duration(current_uptime_s)}"
            )
        if status.get("ntp_failure_count") is not None:
            add_check(
                checks, "No observed NTP failures",
                int(status["ntp_failure_count"]) == 0,
                f"failure count {status['ntp_failure_count']}"
            )
        if status.get("ntp_state") is not None:
            add_check(
                checks, "NTP synchronized",
                status["ntp_state"] == "SYNCED",
                f"state {status['ntp_state']}"
            )
        if current_uptime_s is not None and status.get("wifi_connected_s") is not None:
            wifi_gap = current_uptime_s - float(status["wifi_connected_s"])
            add_check(
                checks, "Wi-Fi association spans current boot",
                wifi_gap < 120,
                f"uptime exceeds Wi-Fi association age by {wifi_gap:.1f}s"
            )
        if cadence:
            add_check(
                checks, "Telemetry cadence",
                cadence["max_gap_s"] < 180,
                f"median {cadence['median_interval_s']:.3f}s, "
                f"largest gap {cadence['max_gap_s']:.3f}s"
            )
        if heap_info.get("recent_linear_trend_bytes_per_hour") is not None:
            heap_slope = float(heap_info["recent_linear_trend_bytes_per_hour"])
            add_check(
                checks, "No obvious heap leak",
                heap_slope > -20,
                f"{heap_slope:+.2f} bytes/hour over recent "
                f"{args.recent_hours:g}h window"
            )
        if heap_info.get("recent_minimum_bytes") is not None:
            add_check(
                checks, "Heap safety margin",
                float(heap_info["recent_minimum_bytes"]) > 16000,
                f"recent minimum {heap_info['recent_minimum_bytes']:.0f} bytes"
            )

        report["evidence_gate"] = {
            "result": "PASS" if checks and all(c.passed for c in checks) else "REVIEW",
            "checks": [
                {"name": c.name, "passed": c.passed, "detail": c.detail}
                for c in checks
            ],
            "note": (
                "This gate summarizes observable stability evidence. It is not "
                "a substitute for project-specific release judgement."
            ),
        }

        # CSV diagnostics.
        if len(peer_stats):
            peer_stats.to_csv(output / "ntp-peers.csv", index=False)
        if len(outliers):
            outliers.to_csv(output / "ntp-outliers.csv", index=False)

        # Graphs.
        if heap_post is not None and len(heap_post):
            graph = heap_post.copy()
            graph["local"] = localize(graph["created_at"], args.timezone)
            graph["rolling_median"] = graph["value"].rolling(60, min_periods=1).median()
            fig, ax = plt.subplots(figsize=(11, 5))
            ax.plot(graph["local"], graph["value"], linewidth=0.7, alpha=0.45,
                    label="Free heap")
            ax.plot(graph["local"], graph["rolling_median"], linewidth=1.8,
                    label="60-sample rolling median")
            ax.set_title("HU-058D free heap")
            ax.set_xlabel(f"Time ({args.timezone})")
            ax.set_ylabel("Bytes")
            ax.legend()
            fig.autofmt_xdate()
            fig.tight_layout()
            fig.savefig(output / "heap.png", dpi=150)
            plt.close(fig)

        if valid_ntp is not None and len(valid_ntp):
            graph = valid_ntp.copy()
            if "time_utc" in graph.columns:
                graph["plot_time"] = pd.to_datetime(
                    graph["time_utc"], utc=True, errors="coerce"
                )
            elif "epoch" in graph.columns:
                graph["plot_time"] = pd.to_datetime(
                    graph["epoch"], unit="s", utc=True, errors="coerce"
                )
            else:
                graph["plot_time"] = pd.NaT
            graph = graph.dropna(subset=["plot_time"])
            if len(graph):
                graph["local"] = localize(graph["plot_time"], args.timezone)
                graph["weighted_running"] = (
                    -graph["correction_ms"].cumsum() * 1000.0
                    / graph["interval_s"].cumsum()
                )
                fig, ax = plt.subplots(figsize=(11, 5))
                ax.plot(
                    graph["local"], graph["drift_ppm"],
                    marker="o", markersize=3, linewidth=0.8,
                    label="Qualified drift sample"
                )
                ax.plot(
                    graph["local"], graph["weighted_running"],
                    linewidth=2, label="Weighted running drift"
                )
                ax.axhline(0, linewidth=0.8)
                ax.set_title("HU-058D oscillator drift")
                ax.set_xlabel(f"Time ({args.timezone})")
                ax.set_ylabel("ppm")
                ax.legend()
                fig.autofmt_xdate()
                fig.tight_layout()
                fig.savefig(output / "drift.png", dpi=150)
                plt.close(fig)

        if rssi_post is not None and len(rssi_post):
            graph = rssi_post.copy()
            graph["local"] = localize(graph["created_at"], args.timezone)
            graph["rolling_median"] = graph["value"].rolling(60, min_periods=1).median()
            fig, ax = plt.subplots(figsize=(11, 5))
            ax.plot(graph["local"], graph["value"], linewidth=0.7, alpha=0.55,
                    label="RSSI")
            ax.plot(graph["local"], graph["rolling_median"], linewidth=1.8,
                    label="60-sample rolling median")
            ax.set_title("HU-058D Wi-Fi RSSI")
            ax.set_xlabel(f"Time ({args.timezone})")
            ax.set_ylabel("dBm")
            ax.legend()
            fig.autofmt_xdate()
            fig.tight_layout()
            fig.savefig(output / "rssi.png", dpi=150)
            plt.close(fig)

        # Machine-readable JSON.
        def clean(obj: Any) -> Any:
            if isinstance(obj, dict):
                return {str(k): clean(v) for k, v in obj.items()}
            if isinstance(obj, list):
                return [clean(v) for v in obj]
            if isinstance(obj, (np.integer,)):
                return int(obj)
            if isinstance(obj, (np.floating,)):
                v = float(obj)
                return v if math.isfinite(v) else None
            if isinstance(obj, pd.Timestamp):
                return obj.isoformat()
            return obj

        report = clean(report)
        (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")

        # Human-readable Markdown.
        gate = report["evidence_gate"]
        lines = [
            "# HU-058D telemetry analysis",
            "",
            f"**Automated evidence gate:** `{gate['result']}`",
            "",
            "> The evidence gate is deliberately conservative and only checks "
            "observable stability indicators. It does not make the release decision.",
            "",
            "## Build",
            "",
            f"- Firmware: `{report.get('firmware') or 'unknown'}`",
            f"- Build: `{report.get('build') or 'unknown'}`",
            f"- Git commit: `{report.get('git_commit') or 'unknown'}`",
            f"- Git branch: `{report.get('git_branch') or 'unknown'}`",
            "",
            "## Stability",
            "",
            f"- Current uptime: **{uptime_info['current_uptime']}**",
            f"- Reboot events visible in export window: **{reboot_events}**",
        ]
        if uptime_info.get("wifi_connected_s") is not None:
            lines.append(
                f"- Current Wi-Fi association age: "
                f"**{fmt_duration(uptime_info['wifi_connected_s'])}**"
            )
        if uptime_info.get("mqtt_publish_count") is not None:
            lines.append(
                f"- MQTT publish count: **{uptime_info['mqtt_publish_count']}**"
            )
        if cadence:
            lines.extend([
                f"- Telemetry samples in current boot: **{cadence['samples']}**",
                f"- Median telemetry interval: **{cadence['median_interval_s']:.3f} s**",
                f"- Largest telemetry gap: **{cadence['max_gap_s']:.3f} s**",
            ])

        lines.extend([
            "",
            "## NTP and oscillator drift",
            "",
            f"- NTP state: **{ntp_info.get('state', 'unknown')}**",
            f"- Successful syncs: **{ntp_info.get('sync_count', 'unknown')}**",
            f"- Observed failed polls: **{ntp_info.get('failure_count', 'unknown')}**",
        ])
        if ntp_info.get("valid_samples") is not None:
            lines.extend([
                f"- Qualified history samples: **{ntp_info['valid_samples']}**",
                f"- Weighted drift: **{ntp_info['weighted_drift_ppm']:+.3f} ppm**",
                f"- Median drift: **{ntp_info['median_drift_ppm']:+.3f} ppm**",
                f"- MAD: **{ntp_info['mad_ppm']:.3f} ppm**",
                f"- Robust outliers in current history: **{ntp_info['outlier_count']}**",
            ])

        lines.extend(["", "## Free heap", ""])
        if heap_info:
            lines.extend([
                f"- Current-boot samples: **{heap_info['samples_current_boot']}**",
                f"- Current-boot minimum: **{heap_info['minimum_bytes_current_boot']:.0f} B**",
                f"- Recent window: **{heap_info['recent_hours']:g} h**",
                f"- Recent median: **{heap_info['recent_median_bytes']:.0f} B**",
                f"- Recent 1st percentile: **{heap_info['recent_p01_bytes']:.0f} B**",
                f"- Recent minimum: **{heap_info['recent_minimum_bytes']:.0f} B**",
                f"- Recent linear trend: "
                f"**{heap_info['recent_linear_trend_bytes_per_hour']:+.2f} B/hour**",
            ])
        else:
            lines.append("- No free-heap series was available.")

        lines.extend(["", "## Wi-Fi RSSI", ""])
        if rssi_info:
            lines.extend([
                f"- Mean: **{rssi_info['mean_dbm']:.1f} dBm**",
                f"- Median: **{rssi_info['median_dbm']:.1f} dBm**",
                f"- 5th to 95th percentile: "
                f"**{rssi_info['p05_dbm']:.1f} to {rssi_info['p95_dbm']:.1f} dBm**",
                f"- Observed range: "
                f"**{rssi_info['minimum_dbm']:.0f} to {rssi_info['maximum_dbm']:.0f} dBm**",
            ])
        else:
            lines.append("- No RSSI series was available.")

        lines.extend(["", "## Evidence checks", ""])
        for check in checks:
            mark = "PASS" if check.passed else "REVIEW"
            lines.append(f"- **{mark}** — {check.name}: {check.detail}")

        lines.extend([
            "",
            "## Generated files",
            "",
            "- `summary.json` — machine-readable report",
            "- `heap.png` — free-heap history and rolling median",
            "- `drift.png` — qualified drift samples and weighted running estimate",
            "- `rssi.png` — RSSI history and rolling median",
            "- `ntp-peers.csv` — per-peer drift statistics when available",
            "- `ntp-outliers.csv` — robust drift outliers when present",
            "",
        ])
        (output / "summary.md").write_text("\n".join(lines))

        print(f"HU-058D analysis complete")
        print(f"  Input:  {args.input}")
        print(f"  Output: {output}")
        print(f"  Gate:   {gate['result']}")
        if ntp_info.get("weighted_drift_ppm") is not None:
            print(f"  Drift:  {ntp_info['weighted_drift_ppm']:+.3f} ppm")
        if heap_info.get("recent_linear_trend_bytes_per_hour") is not None:
            print(
                f"  Heap:   {heap_info['recent_linear_trend_bytes_per_hour']:+.2f} "
                "B/hour recent trend"
            )
        return 0
    finally:
        if temp is not None:
            temp.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
