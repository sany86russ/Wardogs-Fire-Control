"""Qualify the production HUD probe against a private, hash-pinned image set.

The manifest and images stay outside the public checkout. Run --self-test for
the qualification contract alone; this does not prove native HUD recognition.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


CONTEXTS = {"aimed", "incomplete", "reload", "menu", "external", "wrong_weapon"}
STAGES = {"not_started", "invalid_image", "layout_not_found", "optic_layout",
          "supplemental_layout", "instrument_ocr", "completed"}
REFUSALS = ["none", "supplemental_only", "invalid_frame", "unverified_layout",
            "invalid_layout", "missing_scale_headers", "wrong_weapon", "missing_mil_labels",
            "ambiguous_mil_labels", "low_read_confidence", "inconsistent_mil_scale",
            "reticle_outside_labels", "missing_range_labels", "ambiguous_range_labels",
            "scale_row_disagreement", "inconsistent_range_scale", "missing_compass_labels",
            "ambiguous_compass_labels", "inconsistent_compass_scale",
            "compass_pointer_outside_labels", "excessive_uncertainty"]
LAYOUTS = ["unverified", "optic_pixel_geometry", "supplemental_icon_geometry"]


class QualificationError(ValueError):
    pass


def finite_number(value, label, minimum=None, maximum=None):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise QualificationError(f"{label} must be a finite number")
    if (minimum is not None and value < minimum) or (maximum is not None and value > maximum):
        raise QualificationError(f"{label} is outside its allowed range")
    return float(value)


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def checked_hash(value, label):
    if not isinstance(value, str) or len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
        raise QualificationError(f"{label} must be a lowercase SHA256")
    return value


def angular_difference(first, second):
    return abs((first - second + 180.0) % 360.0 - 180.0)


def protect_report(path, originals):
    if path.resolve() in {original.resolve() for original in originals}:
        raise QualificationError("report destination would overwrite a pinned replay input")


def expected_pose(expected, label):
    return (
        finite_number(expected.get("bearing_deg"), label + " bearing", 0, 360),
        finite_number(expected.get("mil"), label + " MIL", 0, 2000),
        finite_number(expected.get("bearing_tolerance_deg"), label + " bearing tolerance", 0, 5),
        finite_number(expected.get("mil_tolerance"), label + " MIL tolerance", 0, 5),
    )


def preflight(manifest_path, model_path):
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    if not isinstance(manifest, dict) or manifest.get("schema_version") != 1:
        raise QualificationError("unsupported manifest schema")
    model_hash = checked_hash(manifest.get("model_sha256"), "model_sha256")
    if not model_path.is_file() or digest(model_path) != model_hash:
        raise QualificationError("recognition model is missing or its SHA256 differs")
    minimum = manifest.get("minimum_positive_cases", 2)
    if isinstance(minimum, bool) or not isinstance(minimum, int) or minimum < 2:
        raise QualificationError("qualification needs at least two distinct positive cases")
    cases = manifest.get("cases")
    if not isinstance(cases, list) or not cases:
        raise QualificationError("manifest has no replay cases")
    seen_names = set()
    positives = {}
    negatives = 0
    prepared = []
    root = manifest_path.parent.resolve()
    for case in cases:
        if not isinstance(case, dict):
            raise QualificationError("case must be an object")
        name = case.get("name")
        if not isinstance(name, str) or not name.strip() or name in seen_names:
            raise QualificationError("case names must be nonempty and unique")
        seen_names.add(name)
        context = case.get("context")
        if not isinstance(context, str) or context not in CONTEXTS:
            raise QualificationError(f"{name}: unknown scene context")
        language = case.get("ocr_language", "en-US")
        if (not isinstance(language, str) or not 1 <= len(language) <= 64 or
                any(not (ch.isascii() and (ch.isalnum() or ch == "-")) for ch in language)):
            raise QualificationError(f"{name}: invalid Windows OCR language tag")
        filename = case.get("file")
        if not isinstance(filename, str) or not filename or Path(filename).is_absolute():
            raise QualificationError(f"{name}: file must be relative to the private manifest")
        path = (root / filename).resolve()
        if not path.is_relative_to(root):
            raise QualificationError(f"{name}: file escapes the private dataset directory")
        image_hash = checked_hash(case.get("sha256"), name + " sha256")
        if not path.is_file() or digest(path) != image_hash:
            raise QualificationError(f"{name}: image missing or SHA256 differs")
        expected = case.get("expected")
        if not isinstance(expected, dict) or type(expected.get("accepted")) is not bool:
            raise QualificationError(f"{name}: expected.accepted must be explicit")
        note = case.get("verification_note")
        if not isinstance(note, str) or not note.strip():
            raise QualificationError(f"{name}: manual verification note is required")
        if expected["accepted"]:
            if context != "aimed":
                raise QualificationError(f"{name}: only an aimed scene can be a positive")
            pose = expected_pose(expected, name)
            positives[image_hash] = pose
        else:
            negatives += 1
            if context == "aimed":
                raise QualificationError(f"{name}: readable aimed cases cannot expect a refusal")
        for key in ("stabilized",):
            if key in expected and expected[key] is not None and type(expected[key]) is not bool:
                raise QualificationError(f"{name}: {key} must be boolean or null")
        if "ammunition" in expected and expected["ammunition"] is not None:
            ammunition = expected["ammunition"]
            if not isinstance(ammunition, dict) or type(ammunition.get("count")) is not int or ammunition["count"] < 0:
                raise QualificationError(f"{name}: ammunition count must be a nonnegative integer")
            if type(ammunition.get("kind")) is not int or ammunition["kind"] != 1:
                raise QualificationError(f"{name}: only a verified single-shell channel is expected")
        prepared.append({**case, "path": path})
    if len(positives) < minimum:
        raise QualificationError(f"only {len(positives)} distinct expected-positive images; {minimum} required")
    poses = list(positives.values())
    if not any(angular_difference(a[0], b[0]) > a[2] + b[2] or abs(a[1] - b[1]) > a[3] + b[3]
               for i, a in enumerate(poses) for b in poses[i + 1:]):
        raise QualificationError("positive cases must cover distinct, nonoverlapping manually verified angles")
    if not negatives:
        raise QualificationError("qualification also needs a negative scene")
    return manifest, prepared


def validate_observation(observed, case):
    name = case["name"]
    if not isinstance(observed, dict) or type(observed.get("accepted")) is not bool:
        raise QualificationError(f"{name}: probe must return an explicit acceptance boolean")
    required = {"accepted", "refusal", "refusal_name", "layout", "layout_name", "stage", "tokens",
                "elapsed_ms", "pose", "ammunition", "stabilized"}
    if not required.issubset(observed):
        raise QualificationError(f"{name}: incomplete probe receipt")
    refusal = observed.get("refusal")
    layout = observed.get("layout")
    if type(refusal) is not int or not 0 <= refusal < len(REFUSALS) or observed.get("refusal_name") != REFUSALS[refusal]:
        raise QualificationError(f"{name}: refusal code/name mismatch")
    if type(layout) is not int or not 0 <= layout < len(LAYOUTS) or observed.get("layout_name") != LAYOUTS[layout]:
        raise QualificationError(f"{name}: layout code/name mismatch")
    if not isinstance(observed.get("stage"), str) or observed["stage"] not in STAGES:
        raise QualificationError(f"{name}: unknown diagnostic stage")
    if type(observed.get("tokens")) is not int or observed["tokens"] < 0:
        raise QualificationError(f"{name}: invalid token count")
    finite_number(observed.get("elapsed_ms"), name + " elapsed_ms", 0)
    stabilized = observed.get("stabilized")
    if stabilized is not None and type(stabilized) is not bool:
        raise QualificationError(f"{name}: invalid stabilization value")
    ammunition = observed.get("ammunition")
    if ammunition is not None and (not isinstance(ammunition, dict) or type(ammunition.get("count")) is not int
                                   or ammunition["count"] < 0 or type(ammunition.get("kind")) is not int
                                   or ammunition["kind"] != 1):
        raise QualificationError(f"{name}: unverified ammunition channel")
    pose = observed.get("pose")
    if observed["stage"] in {"invalid_image", "layout_not_found"} and (
            layout != 0 or observed["tokens"] != 0 or pose is not None or ammunition is not None or stabilized is not None):
        raise QualificationError(f"{name}: absent image/layout cannot produce instrument evidence")
    if observed["accepted"]:
        if refusal != 0 or layout != 1 or observed["stage"] != "completed" or not isinstance(pose, dict):
            raise QualificationError(f"{name}: accepted pose contradicts diagnostics")
        bearing = finite_number(pose.get("bearing_deg"), name + " actual bearing", 0, 360)
        mil = finite_number(pose.get("sight_mil"), name + " actual MIL", 0, 2000)
        finite_number(pose.get("bearing_uncertainty_deg"), name + " bearing uncertainty", 0)
        finite_number(pose.get("mil_uncertainty"), name + " MIL uncertainty", 0)
    elif refusal == 0 or pose is not None:
        raise QualificationError(f"{name}: refused frame must have a reason and no complete pose")
    expected = case["expected"]
    if observed["accepted"] != expected["accepted"]:
        raise QualificationError(f"{name}: expected accepted={expected['accepted']}, got {observed['accepted']}")
    if expected["accepted"]:
        expected_bearing, expected_mil, bearing_tolerance, mil_tolerance = expected_pose(expected, name)
        if angular_difference(bearing, expected_bearing) > bearing_tolerance + 1e-9:
            raise QualificationError(f"{name}: bearing differs from the manually verified interval")
        if abs(mil - expected_mil) > mil_tolerance + 1e-9:
            raise QualificationError(f"{name}: MIL differs from the manually verified interval")
        # A broad uncertainty cannot make an inaccurate recognition pass. The
        # manual fixture tolerance bounds the reported uncertainty separately.
        if pose["bearing_uncertainty_deg"] > bearing_tolerance + 1e-9 or pose["mil_uncertainty"] > mil_tolerance + 1e-9:
            raise QualificationError(f"{name}: reported uncertainty exceeds the fixture qualification bound")
    for key in ("stabilized", "ammunition"):
        if key in expected and observed.get(key) != expected[key]:
            raise QualificationError(f"{name}: {key} differs from manually verified metadata")
    if "refusal_name" in expected and observed["refusal_name"] != expected["refusal_name"]:
        raise QualificationError(f"{name}: unexpected refusal reason")
    if "stage" in expected and observed["stage"] != expected["stage"]:
        raise QualificationError(f"{name}: unexpected diagnostic progress")


def replay(probe, model, cases, timeout):
    rows = []
    flags = 0
    if os.name == "nt":
        flags = subprocess.CREATE_NO_WINDOW | subprocess.IDLE_PRIORITY_CLASS
    for case in cases:
        row = {"name": case["name"], "context": case["context"], "input_sha256": case["sha256"]}
        try:
            language = case.get("ocr_language", "en-US")
            row["ocr_language"] = language
            result = subprocess.run([str(probe), "--json", "--ocr-language=" + language,
                                     str(case["path"]), str(model)],
                                    capture_output=True, text=True, encoding="utf-8", errors="strict",
                                    timeout=timeout, creationflags=flags, check=False)
            row["exit_code"] = result.returncode
            if result.returncode != 0:
                raise QualificationError(f"native probe exited with code {result.returncode}")
            if digest(case["path"]) != case["sha256"]:
                raise QualificationError("pinned image changed during native replay")
            # Exactly one JSON document is required. Debug text or OCR-token
            # dumps cannot accidentally become a qualification receipt.
            def finite_float(raw):
                value = float(raw)
                if not math.isfinite(value):
                    raise QualificationError("probe JSON contains a nonfinite number")
                return value

            def invalid_constant(raw):
                raise QualificationError(f"probe JSON contains invalid constant {raw}")

            observed = json.loads(result.stdout, parse_float=finite_float, parse_constant=invalid_constant)
            # A valid native refusal must remain reviewable when it fails an
            # expected-positive case. Recording it does not make the case pass.
            row["observation"] = observed
            validate_observation(observed, case)
            row["passed"] = True
        except (QualificationError, json.JSONDecodeError, UnicodeError, subprocess.TimeoutExpired, OSError) as error:
            row["passed"] = False
            row["error"] = str(error)
        rows.append(row)
        print(f"{case['name']}: {'PASS' if row['passed'] else 'FAIL'}")
    return rows


class ReplayContractTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.folder = Path(self.temporary.name)
        self.model = self.folder / "model.onnx"
        self.model.write_bytes(b"contract-test-model")
        self.cases = []
        for index, (bearing, accepted) in enumerate(((359.9, True), (25.0, True), (0.0, False))):
            path = self.folder / f"{index}.png"
            path.write_bytes(f"private-contract-fixture-{index}".encode())
            expected = {"accepted": accepted}
            if accepted:
                expected.update(bearing_deg=bearing, mil=998.0, bearing_tolerance_deg=0.5, mil_tolerance=0.5)
            self.cases.append({"name": str(index), "file": path.name, "sha256": digest(path),
                               "context": "aimed" if accepted else "reload", "expected": expected,
                               "verification_note": "contract test; no game image or native acceptance"})
        self.manifest = {"schema_version": 1, "model_sha256": digest(self.model), "cases": self.cases}

    def tearDown(self):
        self.temporary.cleanup()

    def preflight(self):
        path = self.folder / "manifest.json"
        path.write_text(json.dumps(self.manifest), encoding="utf-8")
        return preflight(path, self.model)

    def observation(self, accepted=True):
        return {"accepted": accepted, "refusal": 0 if accepted else 2,
                "refusal_name": "none" if accepted else "invalid_frame", "layout": 1 if accepted else 0,
                "layout_name": "optic_pixel_geometry" if accepted else "unverified", "stage": "completed",
                "tokens": 10, "elapsed_ms": 120.0, "ammunition": None, "stabilized": None,
                "pose": {"bearing_deg": 0.1, "sight_mil": 998.0, "bearing_uncertainty_deg": 0.4,
                         "mil_uncertainty": 0.2} if accepted else None}

    def test_qualified_manifest_and_north_wrap(self):
        _, cases = self.preflight()
        validate_observation(self.observation(), cases[0])

    def test_zero_positive_cannot_pass(self):
        self.manifest["cases"] = self.cases[2:]
        with self.assertRaises(QualificationError):
            self.preflight()

    def test_failed_positive_keeps_its_actual_native_receipt(self):
        _, cases = self.preflight()
        refused = self.observation(False)
        refused["elapsed_ms"] = 582.53
        refused["diagnostics"] = {"mil": {"admission_rejections": 2, "budget_exhausted": False}}
        result = subprocess.CompletedProcess([], 0, stdout=json.dumps(refused), stderr="")
        with mock.patch("subprocess.run", return_value=result), mock.patch("builtins.print"):
            rows = replay(self.folder / "unused-probe", self.model, cases[:1], 1)
        self.assertFalse(rows[0]["passed"])
        self.assertEqual(rows[0]["observation"], refused)
        self.assertIn("expected accepted=True, got False", rows[0]["error"])

    def test_nonfinite_native_json_cannot_break_the_report(self):
        _, cases = self.preflight()
        for number in ("NaN", "Infinity", "1e309"):
            result = subprocess.CompletedProcess([], 0, stdout='{"accepted":true,"elapsed_ms":'+number+'}', stderr="")
            with mock.patch("subprocess.run", return_value=result), mock.patch("builtins.print"):
                rows = replay(self.folder / "unused-probe", self.model, cases[:1], 1)
            self.assertFalse(rows[0]["passed"])
            self.assertNotIn("observation", rows[0])
            json.dumps(rows, allow_nan=False)

    def test_duplicate_image_cannot_supply_second_positive(self):
        self.cases[1]["file"] = self.cases[0]["file"]
        self.cases[1]["sha256"] = self.cases[0]["sha256"]
        with self.assertRaises(QualificationError):
            self.preflight()

    def test_identical_pose_cannot_supply_angle_coverage(self):
        self.cases[1]["expected"] = dict(self.cases[0]["expected"])
        with self.assertRaises(QualificationError):
            self.preflight()

    def test_missing_or_changed_image_fails_preflight(self):
        (self.folder / "0.png").write_bytes(b"changed")
        with self.assertRaises(QualificationError):
            self.preflight()

    def test_dataset_escape_and_missing_negative_fail(self):
        self.cases[0]["file"] = "../outside.png"
        with self.assertRaises(QualificationError):
            self.preflight()
        self.cases[0]["file"] = "0.png"
        self.manifest["cases"] = self.cases[:2]
        with self.assertRaises(QualificationError):
            self.preflight()

    def test_report_cannot_replace_an_original(self):
        with self.assertRaises(QualificationError):
            protect_report(self.model, [self.model])
        protect_report(self.folder / "report.json", [self.model])

    def test_nan_pose_and_broad_uncertainty_fail(self):
        for field, value in (("sight_mil", float("nan")), ("mil_uncertainty", 5.0)):
            observation = self.observation()
            observation["pose"][field] = value
            with self.subTest(field=field), self.assertRaises(QualificationError):
                validate_observation(observation, self.cases[0])

    def test_false_acceptance_and_refused_positive_fail(self):
        for observation, case in ((self.observation(), self.cases[2]), (self.observation(False), self.cases[0])):
            with self.assertRaises(QualificationError):
                validate_observation(observation, case)

    def test_unverified_stage_and_refusal_pose_contradictions_fail(self):
        observation = self.observation()
        observation["stage"] = "layout_not_found"
        with self.assertRaises(QualificationError):
            validate_observation(observation, self.cases[0])
        observation = self.observation(False)
        observation["pose"] = self.observation()["pose"]
        with self.assertRaises(QualificationError):
            validate_observation(observation, self.cases[2])

    def test_missing_receipt_fields_cannot_look_like_unknown_metadata(self):
        observation = self.observation(False)
        del observation["ammunition"]
        with self.assertRaises(QualificationError):
            validate_observation(observation, self.cases[2])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--model", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()
    if args.self_test:
        result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ReplayContractTests))
        return 0 if result.wasSuccessful() else 1
    if any(value is None for value in (args.probe, args.model, args.manifest, args.report)):
        parser.error("native replay requires --probe, --model, --manifest and --report")
    try:
        protect_report(args.report, [args.probe, args.model, args.manifest])
    except QualificationError as error:
        print(str(error), file=sys.stderr)
        return 1
    report = {"schema_version": 1, "native_replay_started": False, "qualified": False,
              "images_redistributed": False, "gui_started": False, "screen_captured": False}
    try:
        finite_number(args.timeout, "timeout", 0.1, 120)
        if os.name != "nt" or not args.probe.is_file():
            raise QualificationError("native replay requires Windows and an existing console probe")
        manifest, cases = preflight(args.manifest.resolve(), args.model.resolve())
        try:
            protect_report(args.report, [case["path"] for case in cases])
        except QualificationError as error:
            print(str(error), file=sys.stderr)
            return 1
        report.update(model_sha256=manifest["model_sha256"], probe_sha256=digest(args.probe),
                      manifest_sha256=digest(args.manifest), native_replay_started=True,
                      priority="Idle", case_count=len(cases))
        rows = replay(args.probe.resolve(), args.model.resolve(), cases, args.timeout)
        report["rows"] = rows
        report["qualified"] = all(row["passed"] for row in rows)
        if digest(args.model) != manifest["model_sha256"]:
            raise QualificationError("pinned model changed during native replay")
    except (QualificationError, json.JSONDecodeError, OSError) as error:
        report["qualified"] = False
        report["error"] = str(error)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(f"Native HUD qualification: {'PASS' if report['qualified'] else 'FAIL'}")
    return 0 if report["qualified"] else 1


if __name__ == "__main__":
    sys.exit(main())
