"""Measure recorded SPH-2 impacts without launching the app or reading the screen.

The denominator contains accepted impact observations, not detected shots. Logs
cannot establish that every fired round was recorded, that sight settings were
followed, or that observations are independent. Verification flags are reported
as claims from the input, never as independent field validation by this tool.
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
import json
import math
from pathlib import Path
import re
import statistics
from typing import Iterable


EVENT = "continuous.impact_recorded"
FIELDS = re.compile(r'(?:^|\s)([A-Za-z][A-Za-z0-9_]*)=("[^"\r\n]*"|\S+)')
TIMESTAMP = re.compile(r"^\[([^\]\r\n]+)\]")
MAX_LINE_BYTES = 65536
VERIFICATION_FIELDS = (
    "physical_shot_verified", "sight_verified", "independent_shot_verified")


def one_sided_exact_lower_bound(hits: int, count: int, confidence: float = 0.95) -> float:
    """One-sided Clopper-Pearson bound, using a stable binomial log-tail."""
    if not isinstance(hits, int) or not isinstance(count, int) or not 0 <= hits <= count:
        raise ValueError("hits and count must be integers with 0 <= hits <= count")
    if not math.isfinite(confidence) or not 0 < confidence < 1:
        raise ValueError("confidence must lie strictly between 0 and 1")
    if hits == 0:
        return 0.0
    alpha = 1.0 - confidence
    if hits == count:
        return math.exp(math.log(alpha) / count)

    def log_tail(p: float) -> float:
        log_p, log_q = math.log(p), math.log1p(-p)
        term = (math.lgamma(count + 1) - math.lgamma(hits + 1) -
                math.lgamma(count - hits + 1) + hits * log_p + (count - hits) * log_q)
        total = term
        for index in range(hits, count):
            term += math.log(count - index) - math.log(index + 1) + log_p - log_q
            high, low = max(total, term), min(total, term)
            total = high + math.log1p(math.exp(low - high))
        return total

    lower, upper = 0.0, 1.0
    threshold = math.log(alpha)
    for _ in range(70):
        middle = (lower + upper) / 2.0
        if log_tail(middle) < threshold:
            lower = middle
        else:
            upper = middle
    return lower


def _fields(message: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for match in FIELDS.finditer(message):
        name, value = match.groups()
        if name in result:
            raise ValueError("duplicate field: " + name)
        result[name] = value.strip('"')
    return result


def _point(value: str) -> tuple[float, float]:
    parts = value.split(",")
    if len(parts) != 2:
        raise ValueError("coordinate pair must have two numbers")
    point = float(parts[0]), float(parts[1])
    if not all(math.isfinite(number) for number in point):
        raise ValueError("coordinates must be finite")
    return point


@dataclass(frozen=True)
class Observation:
    session: str
    map: str
    base: tuple[float, float]
    target: tuple[float, float]
    impact: tuple[float, float]
    arc: str
    mode: str
    timestamp: str
    observation_id: str
    command_id: str
    command_source: str
    context_mode: str
    diagnostic: bool
    source: str
    verification_claims: tuple[bool, bool, bool]

    @property
    def stronger_provenance(self) -> bool:
        return bool(self.observation_id and self.command_id and
                    self.session != "unknown" and self.context_mode == "user")

    @property
    def identity(self) -> tuple:
        if self.observation_id:
            return "observation", self.session, self.observation_id
        # A command may be fired repeatedly. Never collapse all its impacts to
        # one observation merely because its command_id is unchanged.
        return ("legacy", self.session, self.timestamp, self.command_id,
                self.map, self.base, self.target, self.impact, self.arc, self.mode)

    @property
    def geometry(self) -> tuple[float, float, float]:
        dx, dy = self.target[0] - self.base[0], self.target[1] - self.base[1]
        distance = math.hypot(dx, dy)
        east = (self.impact[0] - self.target[0]) * 100.0
        north = (self.impact[1] - self.target[1]) * 100.0
        return (math.hypot(east, north),
                (dy / distance) * east - (dx / distance) * north,
                (dx / distance) * east + (dy / distance) * north)


def _observation(fields: dict[str, str], session: str, timestamp: str,
                 context_mode: str, diagnostic_path: bool) -> Observation:
    missing = [field for field in ("map", "base", "target", "impact", "arc", "mode")
               if not fields.get(field)]
    if missing:
        raise ValueError("missing fields: " + ",".join(missing))
    if fields["arc"] not in ("low", "high"):
        raise ValueError("invalid arc")
    if fields["mode"] not in ("local_only", "platform_refinement"):
        raise ValueError("invalid correction mode")
    if "diagnostic" in fields and fields["diagnostic"] not in ("0", "1"):
        raise ValueError("diagnostic flag must be 0 or 1")
    explicit_session = fields.get("session_id", "")
    if explicit_session:
        if session.startswith("id:") and session != "id:" + explicit_session:
            raise ValueError("observation session does not match application context")
        session = "id:" + explicit_session
    source = fields.get("source", "unknown")
    observation = Observation(
        session, fields["map"], _point(fields["base"]), _point(fields["target"]),
        _point(fields["impact"]), fields["arc"], fields["mode"], timestamp,
        fields.get("observation_id", ""), fields.get("command_id", ""),
        fields.get("command_source", "unknown"), context_mode,
        diagnostic_path or context_mode == "diagnostic" or fields.get("diagnostic") == "1" or
        source in ("test", "fixture"),
        source, tuple(fields.get(field) == "1" for field in VERIFICATION_FIELDS))
    distance = math.hypot(observation.target[0] - observation.base[0],
                          observation.target[1] - observation.base[1])
    if not math.isfinite(distance) or distance <= 0:
        raise ValueError("gun and target must define a finite nonzero firing direction")
    if not all(math.isfinite(value) for value in observation.geometry):
        raise ValueError("impact geometry exceeds finite range")
    # The coordinates are the source of truth for miss distance. An optional
    # logged value is checked for corruption rather than used as the answer.
    if "observed_miss_m" in fields:
        miss = float(fields["observed_miss_m"])
        if not math.isfinite(miss) or miss < 0:
            raise ValueError("logged miss must be finite and nonnegative")
        if not math.isclose(miss, observation.geometry[0], rel_tol=1e-6, abs_tol=0.01):
            raise ValueError("logged miss disagrees with coordinates")
    return observation


def _summary(observations: list[Observation], radius_m: float) -> dict:
    count = len(observations)
    if not count:
        return {"count": 0, "hits": 0, "hit_rate": None,
                "one_sided_95_percent_lower_bound": None,
                "mean_miss_m": None, "median_miss_m": None, "p95_miss_m": None,
                "bias_right_m": None, "bias_far_m": None,
                "bias_center_m": None, "scatter_rms_m": None,
                "weaker_provenance_count": 0, "verification_claim_count": 0,
                "statistical_99_percent_threshold_met": False}
    geometry = [observation.geometry for observation in observations]
    misses = sorted(item[0] for item in geometry)
    # Roundoff at the closed radius boundary must not turn exactly 10 m into
    # a miss because decimal coordinates were represented as binary doubles.
    hits = sum(miss <= radius_m or math.isclose(miss, radius_m, rel_tol=1e-12, abs_tol=1e-9)
               for miss in misses)
    # Normalize before sums/squares so valid finite coordinates cannot create
    # non-finite report values merely through intermediate overflow.
    scale = max(misses) or 1.0
    right_normalized = statistics.fmean(item[1] / scale for item in geometry)
    far_normalized = statistics.fmean(item[2] / scale for item in geometry)
    right, far = right_normalized * scale, far_normalized * scale
    scatter = scale * math.sqrt(statistics.fmean(
        (item[1] / scale - right_normalized) ** 2 +
        (item[2] / scale - far_normalized) ** 2 for item in geometry))
    lower_bound = one_sided_exact_lower_bound(hits, count)
    return {"count": count, "hits": hits, "hit_rate": hits / count,
            "one_sided_95_percent_lower_bound": lower_bound,
            "mean_miss_m": scale * statistics.fmean(value / scale for value in misses),
            "median_miss_m": scale * statistics.median(value / scale for value in misses),
            "p95_miss_m": misses[math.ceil(0.95 * count) - 1],
            "bias_right_m": right, "bias_far_m": far,
            "bias_center_m": math.hypot(right, far), "scatter_rms_m": scatter,
            "weaker_provenance_count": sum(not item.stronger_provenance for item in observations),
            "verification_claim_count": sum(all(item.verification_claims) for item in observations),
            "statistical_99_percent_threshold_met": lower_bound >= 0.99}


def analyze_logs(paths: Iterable[Path], radius_m: float = 10.0) -> dict:
    if not math.isfinite(radius_m) or radius_m <= 0:
        raise ValueError("hit radius must be finite and positive")
    unique: dict[tuple, Observation] = {}
    conflicts: set[tuple] = set()
    duplicate_count = 0
    issue_counts: Counter = Counter()
    issues: list[dict] = []
    files = list(dict.fromkeys(Path(path).resolve() for path in paths))
    recorded_events = 0
    other_impact_events: set[tuple] = set()

    def issue(path: Path, line: int, reason: str) -> None:
        issue_counts[reason] += 1
        # Keep the report bounded. Never include the raw log message.
        if len(issues) < 100:
            issues.append({"file": str(path), "line": line, "reason": reason})

    for path in files:
        session, context_mode = "unknown", "unknown"
        diagnostic_path = (path.name.lower().startswith("diagnostic") or
                           any(part.lower() == "testing" for part in path.parts))
        with path.open("rb") as stream:
            for line_number, raw in enumerate(stream, 1):
                if len(raw) > MAX_LINE_BYTES:
                    issue(path, line_number, "line exceeds diagnostic limit")
                    continue
                try:
                    line = raw.decode("utf-8-sig").rstrip("\r\n")
                except UnicodeDecodeError:
                    issue(path, line_number, "invalid UTF-8")
                    continue
                timestamp_match = TIMESTAMP.match(line)
                timestamp = timestamp_match[1] if timestamp_match else ""
                try:
                    if "session.start " in line:
                        fields = _fields(line.split("session.start ", 1)[1])
                        session = ("id:" + fields["session_id"] if fields.get("session_id")
                                   else "start:" + timestamp + ":" + fields.get("pid", "unknown"))
                        context_mode = "unknown"
                    if "application.context " in line:
                        fields = _fields(line.split("application.context ", 1)[1])
                        context_mode = fields.get("mode", "unknown")
                        if fields.get("session_id"):
                            session = "id:" + fields["session_id"]
                        elif session == "unknown" and fields.get("started_utc"):
                            session = "started_utc:" + fields["started_utc"]
                except ValueError as error:
                    session, context_mode = "unknown", "unknown"
                    issue(path, line_number, "invalid session context: " + str(error))
                    continue
                for other_event in ("continuous.impact_requested", "continuous.impact_rejected"):
                    if re.search(r"(?:^|\s)" + re.escape(other_event) + r"(?:\s|$)", line):
                        other_impact_events.add((other_event, session, timestamp, line))
                if not re.search(r"(?:^|\s)" + re.escape(EVENT) + r"(?:\s|$)", line):
                    continue
                recorded_events += 1
                try:
                    observation = _observation(_fields(line.split(EVENT, 1)[1]), session,
                                               timestamp, context_mode, diagnostic_path)
                except (ValueError, OverflowError) as error:
                    issue(path, line_number, str(error))
                    continue
                identity = observation.identity
                if identity in conflicts:
                    duplicate_count += 1
                elif identity in unique:
                    if unique[identity] == observation:
                        duplicate_count += 1
                    else:
                        del unique[identity]
                        conflicts.add(identity)
                        issue(path, line_number, "conflicting observation identity; all versions excluded")
                else:
                    unique[identity] = observation

    diagnostic = [item for item in unique.values() if item.diagnostic]
    observations = [item for item in unique.values() if not item.diagnostic]
    grouped: dict[tuple, list[Observation]] = {}
    for item in observations:
        grouped.setdefault((item.session, item.map, item.base, item.arc, item.mode), []).append(item)
    groups = [{"session": key[0], "map": key[1], "base": list(key[2]),
               "arc": key[3], "correction_mode": key[4], **_summary(items, radius_m)}
              for key, items in sorted(grouped.items())]
    summary = _summary(observations, radius_m)
    return {"schema_version": 1, "hit_radius_m": radius_m,
            "coordinate_meters_per_unit": 100, "p95_method": "nearest_rank",
            "files": [str(path) for path in files], "recorded_events": recorded_events,
            "impact_requests_seen": sum(item[0] == "continuous.impact_requested" for item in other_impact_events),
            "impact_rejections_seen": sum(item[0] == "continuous.impact_rejected" for item in other_impact_events),
            "duplicate_records_excluded": duplicate_count,
            "conflicting_identities_excluded": len(conflicts),
            "invalid_records": sum(issue_counts.values()), "issue_counts": dict(issue_counts),
            "issues": issues, "issues_truncated": sum(issue_counts.values()) > len(issues),
            "diagnostic_observations_excluded": len(diagnostic),
            "summary": summary, "groups": groups,
            "acceptance": {
                "statistical_99_percent_threshold_met": summary["statistical_99_percent_threshold_met"],
                "recorded_verification_claims_complete": bool(observations) and all(
                    item.stronger_provenance and all(item.verification_claims) for item in observations),
                "field_99_percent_claim_accepted": False,
                "reason": "Logs record selected accepted impacts. This reader independently verifies neither physical shots, actual sight settings, completeness, independence nor field context.",
                "minimum_zero_failure_sample_for_95_percent_lower_bound_at_99_percent": 299}}


def render_markdown(report: dict) -> str:
    summary = report["summary"]
    def number(value, digits=2):
        return "—" if value is None else f"{value:.{digits}f}"
    rate = None if summary["hit_rate"] is None else summary["hit_rate"] * 100
    lower = summary["one_sided_95_percent_lower_bound"]
    lower = None if lower is None else lower * 100
    lines = [f"Радиус попадания: {number(report['hit_radius_m'])} м.", "",
             f"Принятых наблюдений: {summary['count']}; попаданий: {summary['hits']} ({number(rate)}%).",
             f"Односторонняя точная нижняя граница 95%: {number(lower, 4)}%.",
             f"Промах: средний {number(summary['mean_miss_m'])} м; медиана {number(summary['median_miss_m'])} м; p95 {number(summary['p95_miss_m'])} м.",
             f"Смещение вправо {number(summary['bias_right_m'])} м; перелёт {number(summary['bias_far_m'])} м; разброс RMS вокруг центра {number(summary['scatter_rms_m'])} м.", "",
             f"Исключены: диагностические наблюдения {report['diagnostic_observations_excluded']}, повторы {report['duplicate_records_excluded']}, конфликтующие идентификаторы {report['conflicting_identities_excluded']}, ошибки данных {report['invalid_records']}.",
             f"Наблюдения со слабым происхождением: {summary['weaker_provenance_count']}.", "",
             f"Запросов записи в журналах: {report['impact_requests_seen']}; отказов: {report['impact_rejections_seen']}. Эти события не являются счётчиком физических выстрелов.", "",
             "Журнал хранит принятые наблюдения попаданий. Он не доказывает запись каждого выстрела, фактическую наводку и независимость выборки. 99% попаданий этим отчётом не подтверждены.",
             "299 независимых попаданий без промахов дают нижнюю границу выше 99% только при полной и проверенной полевой выборке.", "",
             "| Сессия | Карта | Орудие X,Y | Дуга | Наблюдения | Попадания | Нижняя граница | p95, м |",
             "|---|---|---|---|---:|---:|---:|---:|"]
    for group in report["groups"]:
        safe = lambda value: str(value).replace("|", "\\|").replace("\n", " ").replace("\r", " ")
        lines.append(f"| {safe(group['session'])} | {safe(group['map'])} | {safe(group['base'])} | {safe(group['arc'])} | {group['count']} | {group['hits']} | {number(group['one_sided_95_percent_lower_bound'] * 100, 4)}% | {number(group['p95_miss_m'])} |")
    return "\n".join(lines) + "\n"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", type=Path, nargs="*", help="Explicit session log files")
    parser.add_argument("--archive-directory", type=Path, help="Read latest.log, latest.previous.log and session-*.log in this explicit directory only")
    parser.add_argument("--radius", type=float, default=10.0, help="Closed hit radius in meters (default: 10)")
    parser.add_argument("--json", action="store_true", help="Emit JSON instead of Markdown")
    parser.add_argument("--output", type=Path, help="Write the report here instead of stdout")
    args = parser.parse_args(argv)
    paths = list(args.logs)
    if args.archive_directory:
        if not args.archive_directory.is_dir():
            parser.error("archive directory does not exist")
        paths.extend(path for path in sorted(args.archive_directory.iterdir())
                     if path.is_file() and (path.name in ("latest.log", "latest.previous.log") or
                                           re.fullmatch(r"session-[^.]+\.log", path.name)))
    if not paths:
        parser.error("provide explicit log files or a nonempty archive directory")
    try:
        report = analyze_logs(paths, args.radius)
        content = (json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
                   if args.json else render_markdown(report))
        if args.output:
            args.output.write_text(content, encoding="utf-8")
        else:
            print(content, end="")
    except (OSError, ValueError) as error:
        parser.exit(2, "Cannot analyze accuracy: " + str(error) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
