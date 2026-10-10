"""Regression tests for offline recorded-impact accuracy analysis."""
import contextlib
import importlib.util
import io
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest


TOOL = Path(__file__).resolve().parents[1] / "tools" / "analyze_shot_accuracy.py"
SPEC = importlib.util.spec_from_file_location("shot_accuracy", TOOL)
accuracy = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = accuracy
SPEC.loader.exec_module(accuracy)


def context(session="a", mode="user"):
    return (f"[2026-10-10 12:00:00.000] [INFO] session.start version=2.12.0 pid=42 session_id={session}\n"
            f"[2026-10-10 12:00:00.001] [INFO] application.context mode={mode} session_id={session}\n")


def event(index=1, session="a", impact="0.01,10.01", **fields):
    values = {"source": "OCR", "map": "bakurani", "base": "0,0", "target": "0,10",
              "impact": impact, "arc": "high", "mode": "local_only",
              "session_id": session, "observation_id": str(index), "command_id": "command-1",
              "command_source": "displayed_unverified", "physical_shot_verified": "0",
              "sight_verified": "0", "independent_shot_verified": "0"}
    values.update(fields)
    tail = " ".join(f"{name}={value}" for name, value in values.items() if value is not None)
    return f"[2026-10-10 12:01:{index:06d}] [INFO] continuous.impact_recorded {tail}\n"


class ShotAccuracyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def log(self, name, content):
        path = self.root / name
        path.write_text(content, encoding="utf-8")
        return path

    def test_exact_binomial_bound_known_small_values(self):
        bound = accuracy.one_sided_exact_lower_bound
        self.assertEqual(bound(0, 0), 0)
        self.assertEqual(bound(0, 8), 0)
        self.assertAlmostEqual(bound(1, 1), 0.05, places=13)
        self.assertAlmostEqual(bound(2, 2), math.sqrt(0.05), places=13)
        self.assertAlmostEqual(bound(1, 2), 1 - math.sqrt(0.95), places=13)
        self.assertAlmostEqual(bound(1, 1000), 1 - 0.95 ** (1 / 1000), places=12)

    def test_299_is_minimum_flawless_sample_not_field_proof(self):
        self.assertLess(accuracy.one_sided_exact_lower_bound(298, 298), 0.99)
        self.assertGreater(accuracy.one_sided_exact_lower_bound(299, 299), 0.99)
        path = self.log("latest.log", context() + "".join(event(index=i) for i in range(299)))
        result = accuracy.analyze_logs([path])
        self.assertEqual(result["summary"]["count"], 299)
        self.assertTrue(result["acceptance"]["statistical_99_percent_threshold_met"])
        self.assertFalse(result["acceptance"]["field_99_percent_claim_accepted"])
        self.assertFalse(result["acceptance"]["recorded_verification_claims_complete"])

    def test_forged_verification_flags_do_not_prove_field_context(self):
        verified = {field: "1" for field in accuracy.VERIFICATION_FIELDS}
        path = self.log("latest.log", context() + "".join(event(index=i, **verified) for i in range(299)))
        result = accuracy.analyze_logs([path])
        self.assertTrue(result["acceptance"]["recorded_verification_claims_complete"])
        self.assertFalse(result["acceptance"]["field_99_percent_claim_accepted"])

    def test_duplicate_archive_is_not_extra_hits(self):
        content = context() + event()
        first = self.log("latest.log", content)
        second = self.log("session-backup.log", content)
        result = accuracy.analyze_logs([first, second, first])
        self.assertEqual(result["summary"]["count"], 1)
        self.assertEqual(result["duplicate_records_excluded"], 1)

    def test_repeated_command_keeps_distinct_observations(self):
        path = self.log("latest.log", context() + event(index=1) + event(index=2))
        self.assertEqual(accuracy.analyze_logs([path])["summary"]["count"], 2)

    def test_conflicting_identity_excludes_both_records(self):
        path = self.log("latest.log", context() + event() + event(impact="0,11") + event())
        result = accuracy.analyze_logs([path])
        self.assertEqual(result["summary"]["count"], 0)
        self.assertEqual(result["conflicting_identities_excluded"], 1)
        self.assertEqual(result["invalid_records"], 1)

    def test_diagnostics_are_excluded_even_with_false_user_flag(self):
        user = self.log("latest.log", context() + event())
        diagnostic = self.log("diagnostic.log", context() + event(index=2))
        synthetic = self.log("synthetic.log", context(session="b", mode="diagnostic") + event(session="b"))
        result = accuracy.analyze_logs([user, diagnostic, synthetic])
        self.assertEqual(result["summary"]["count"], 1)
        self.assertEqual(result["diagnostic_observations_excluded"], 2)
        self.assertFalse(result["acceptance"]["field_99_percent_claim_accepted"])

    def test_record_diagnostic_flag_excludes_renamed_contextless_log(self):
        result = accuracy.analyze_logs([self.log("renamed.log", event(diagnostic="1"))])
        self.assertEqual(result["summary"]["count"], 0)
        self.assertEqual(result["diagnostic_observations_excluded"], 1)
        self.assertEqual(result["invalid_records"], 0)

    def test_diagnostic_zero_cannot_override_context_or_path(self):
        first = self.log("renamed.log", context(mode="diagnostic") + event(diagnostic="0"))
        second = self.log("diagnostic.log", context(session="b") + event(session="b", diagnostic="0"))
        result = accuracy.analyze_logs([first, second])
        self.assertEqual(result["summary"]["count"], 0)
        self.assertEqual(result["diagnostic_observations_excluded"], 2)

    def test_malformed_diagnostic_flag_is_not_treated_as_user_data(self):
        path = self.log("latest.log", context() + event(diagnostic="nan"))
        result = accuracy.analyze_logs([path])
        self.assertEqual(result["summary"]["count"], 0)
        self.assertEqual(result["invalid_records"], 1)

    def test_separate_session_map_base_arc_contexts(self):
        content = context() + event(index=1) + event(index=2, arc="low") + event(index=3, map="ozeti")
        content += event(index=4, base="1,0") + context(session="b") + event(index=1, session="b")
        result = accuracy.analyze_logs([self.log("latest.log", content)])
        self.assertEqual(result["summary"]["count"], 5)
        self.assertEqual(len(result["groups"]), 5)

    def test_missing_invalid_and_nonfinite_records_are_reported(self):
        content = context() + event(index=1, base=None) + event(index=2, impact="nan,10")
        content += event(index=3, impact="0,inf") + event(index=4, impact="1,2,3")
        content += event(index=5, arc="sideways") + event(index=6, base="0,10")
        content += event(index=7, observed_miss_m="nan") + event(index=8, observed_miss_m="12")
        content += event(index=9, impact="1e308,10") + event(index=10)
        result = accuracy.analyze_logs([self.log("latest.log", content)])
        self.assertEqual(result["summary"]["count"], 1)
        self.assertEqual(result["invalid_records"], 9)
        self.assertEqual(result["recorded_events"], 10)
        json.dumps(result, allow_nan=False)

    def test_wrong_session_is_rejected(self):
        result = accuracy.analyze_logs([self.log("latest.log", context() + event(session="wrong"))])
        self.assertEqual(result["summary"]["count"], 0)
        self.assertEqual(result["invalid_records"], 1)

    def test_two_observations_bias_scatter_and_closed_radius(self):
        path = self.log("latest.log", context() + event(index=1, impact="0.06,10.08") +
                        event(index=2, impact="-0.06,9.92"))
        summary = accuracy.analyze_logs([path])["summary"]
        self.assertEqual(summary["hits"], 2)
        self.assertAlmostEqual(summary["mean_miss_m"], 10)
        self.assertAlmostEqual(summary["median_miss_m"], 10)
        self.assertAlmostEqual(summary["p95_miss_m"], 10)
        self.assertAlmostEqual(summary["bias_right_m"], 0)
        self.assertAlmostEqual(summary["bias_far_m"], 0)
        self.assertAlmostEqual(summary["scatter_rms_m"], 10)
        self.assertTrue(math.isfinite(summary["one_sided_95_percent_lower_bound"]))

    def test_outside_radius_is_not_a_hit(self):
        path = self.log("latest.log", context() + event(impact="0.10001,10"))
        self.assertEqual(accuracy.analyze_logs([path])["summary"]["hits"], 0)

    def test_legacy_record_is_measured_but_weaker(self):
        line = event(session_id=None, observation_id=None, command_id=None)
        result = accuracy.analyze_logs([self.log("legacy.log", line + line)])
        self.assertEqual(result["summary"]["count"], 1)
        self.assertEqual(result["summary"]["weaker_provenance_count"], 1)
        self.assertEqual(result["duplicate_records_excluded"], 1)

    def test_rejected_or_unrelated_event_is_not_a_landing(self):
        line = event().replace("continuous.impact_recorded", "continuous.impact_rejected")
        result = accuracy.analyze_logs([self.log("latest.log", context() + line)])
        self.assertEqual(result["recorded_events"], 0)
        self.assertEqual(result["summary"]["count"], 0)
        self.assertIsNone(result["summary"]["hit_rate"])
        self.assertEqual(result["impact_rejections_seen"], 1)

    def test_bad_utf8_is_reported_without_exposing_raw_message(self):
        path = self.root / "latest.log"
        path.write_bytes(b"\xffprivate secret material\n" + (context() + event()).encode())
        result = accuracy.analyze_logs([path])
        self.assertEqual(result["invalid_records"], 1)
        self.assertNotIn("private secret", json.dumps(result))

    def test_broken_context_is_reported_and_cannot_claim_verification(self):
        content = context() + "application.context mode=user mode=diagnostic\n" + event()
        result = accuracy.analyze_logs([self.log("latest.log", content)])
        self.assertEqual(result["invalid_records"], 1)
        self.assertEqual(result["summary"]["count"], 1)
        self.assertEqual(result["summary"]["weaker_provenance_count"], 1)
        self.assertFalse(result["acceptance"]["recorded_verification_claims_complete"])

    def test_finite_large_coordinates_keep_summary_finite(self):
        content = context() + event(index=1, impact="1e304,10") + event(index=2, impact="-1e304,10")
        result = accuracy.analyze_logs([self.log("latest.log", content)])
        self.assertEqual(result["summary"]["count"], 2)
        self.assertTrue(math.isfinite(result["summary"]["scatter_rms_m"]))
        json.dumps(result, allow_nan=False)

    def test_explicit_archive_directory_json_output_and_invalid_radius(self):
        self.log("latest.log", context() + event())
        self.log("ignored.log", context() + event(index=2))
        output = self.root / "report.json"
        self.assertEqual(accuracy.main(["--archive-directory", str(self.root), "--json", "--output", str(output)]), 0)
        self.assertEqual(json.loads(output.read_text(encoding="utf-8"))["summary"]["count"], 1)
        for radius in (0, -1, math.nan, math.inf):
            with self.assertRaises(ValueError):
                accuracy.analyze_logs([], radius)
        for values in ((-1, 5), (6, 5), (1.5, 2)):
            with self.assertRaises(ValueError):
                accuracy.one_sided_exact_lower_bound(*values)
        with contextlib.redirect_stdout(io.StringIO()) as stdout:
            self.assertEqual(accuracy.main([str(self.root / "latest.log")]), 0)
        self.assertIn("Радиус попадания: 10.00 м", stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
