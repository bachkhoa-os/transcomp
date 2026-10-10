"""Offline M0-B1 contracts: fixtures, a CSV transform, and an independent oracle.

All data belongs to TemporaryDirectory instances. No native builds, mounts,
timers, or filesystem-specific operations are used.
"""

import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]
HEADER = "event_id,timestamp,sensor_id,region,quantity,price_cents,status,note\n"
OUT_HEADER = b"event_id,sensor_id,region,total_cents,note\n"
GOLDEN_INPUT = HEADER + (
    "1,1700000001,7,north,49,123,ok,below\n"
    "2,1700000002,8,south,50,0,ok,zero\n"
    "3,1700000003,9,east_,99,123,er,rejected\n"
    "4,1700000004,10,west_,75,123,ok,total\n"
    "5,1700000005,11,north,50,7,OK,case-sensitive\n"
    "6,1700000006,12,south,50,7,ok,last\n"
)
GOLDEN_OUTPUT = OUT_HEADER + (
    b"2,8,south,0,zero\n"
    b"4,10,west_,9225,total\n"
    b"6,12,south,350,last\n"
)


def load_module(name):
    path = REPO / "benchmarks" / f"{name}.py"
    if not path.is_file():
        return None
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


fixture = load_module("m0_fixture")
workflow = load_module("m0_csv_workflow")


class OfflineCase(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="myfs-m0-b1-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def cli(self, module, *args):
        return subprocess.run(
            [sys.executable, "-B", str(REPO / "benchmarks" / f"{module}.py"),
             *(str(arg) for arg in args)],
            capture_output=True, text=True, check=False,
        )


class FixtureTests(OfflineCase):
    def setUp(self):
        super().setUp()
        self.assertIsNotNone(fixture, "The fixture/oracle module is not implemented")

    def csv(self, name, records=16, seed=20261010):
        data = self.root / name
        manifest = self.root / f"{name}.manifest.json"
        result = fixture.generate_csv(data, manifest, records, seed)
        return data, manifest, result

    def binary(self, name, length, seed=20261010):
        data = self.root / name
        manifest = self.root / f"{name}.manifest.json"
        result = fixture.generate_binary(data, manifest, length, seed)
        return data, manifest, result

    def test_csv_repeatable_with_varying_records_and_seeds(self):
        a, _, _ = self.csv("a.csv", records=20)
        b, _, _ = self.csv("b.csv", records=20)
        c, _, _ = self.csv("c.csv", records=20, seed=9)
        self.assertEqual(a.read_bytes(), b.read_bytes())
        self.assertNotEqual(a.read_bytes(), c.read_bytes())
        self.assertEqual(len(set(a.read_bytes().splitlines()[1:])), 20)

    def test_csv_exact_record_accounting_and_manifest(self):
        data, manifest, result = self.csv("input.csv", records=32768)
        payload = data.read_bytes()
        self.assertTrue(payload.startswith(HEADER.encode("ascii")))
        lines = payload.splitlines(keepends=True)
        self.assertEqual(len(lines), 32769)
        for row in lines[1:]:
            self.assertEqual(len(row), 128)
            self.assertTrue(row.endswith(b"\n"))
            self.assertNotIn(b"\r", row)
            self.assertTrue(row.isascii())
            fields = row[:-1].split(b",")
            self.assertEqual(list(map(len, fields)), [10, 10, 4, 5, 2, 6, 2, 81])
        stored = json.loads(manifest.read_text(encoding="ascii"))
        self.assertEqual(result, stored)
        self.assertEqual(stored["record_count"], 32768)
        self.assertEqual(stored["byte_count"], len(HEADER.encode("ascii")) + 32768 * 128)
        self.assertEqual(stored["sha256"], hashlib.sha256(payload).hexdigest())
        self.assertEqual(stored["seed"], 20261010)
        self.assertEqual(stored["generator_version"], "myfs-m0-csv-v1")
        self.assertEqual(stored["schema_version"], "myfs-m0-input-v1")

    def test_csv_derivation_matches_documented_sha256_encoding(self):
        data, _, _ = self.csv("input.csv", records=1, seed=7)
        fields = data.read_bytes().splitlines()[1].decode("ascii").split(",")
        digest = hashlib.sha256(b"myfs-m0-csv-v1|seed=7|record=0\n").digest()
        self.assertEqual(fields[0:2], ["0000000000", "1700000000"])
        self.assertEqual(fields[2], f"{int.from_bytes(digest[:2], 'big') % 1024:04d}")
        self.assertEqual(fields[3], ("north", "south", "east_", "west_")[digest[2] % 4])
        self.assertEqual(fields[4], f"{digest[3] % 100:02d}")
        self.assertEqual(fields[5], f"{int.from_bytes(digest[4:7], 'big') % 1000000:06d}")
        self.assertEqual(fields[6], "er" if digest[7] % 5 == 0 else "ok")
        self.assertIn(f"tag={digest[9:17].hex()};", fields[7])

    def test_csv_zero_records_has_only_header(self):
        data, _, result = self.csv("empty.csv", records=0)
        self.assertEqual(data.read_bytes(), HEADER.encode("ascii"))
        self.assertEqual(result["record_count"], 0)

    def test_csv_overwrite_refusal_preserves_both_paths(self):
        data, manifest, _ = self.csv("input.csv")
        before = (data.read_bytes(), manifest.read_bytes())
        with self.assertRaises(FileExistsError):
            fixture.generate_csv(data, manifest, 3)
        self.assertEqual((data.read_bytes(), manifest.read_bytes()), before)

    def test_existing_manifest_refused_before_creating_csv(self):
        manifest = self.root / "manifest.json"
        manifest.write_bytes(b"user-owned")
        data = self.root / "input.csv"
        with self.assertRaises(FileExistsError):
            fixture.generate_csv(data, manifest, 1)
        self.assertFalse(data.exists())
        self.assertEqual(manifest.read_bytes(), b"user-owned")

    def test_dangling_symlink_is_not_followed_or_overwritten(self):
        data = self.root / "input.csv"
        target = self.root / "absent"
        data.symlink_to(target)
        with self.assertRaises(FileExistsError):
            fixture.generate_csv(data, self.root / "manifest.json", 1)
        self.assertTrue(data.is_symlink())
        self.assertFalse(target.exists())

    def test_invalid_csv_parameters_create_nothing(self):
        for records, seed in [(-1, 1), (8300000001, 1), (1, -1), (1, 2**64)]:
            with self.subTest(records=records, seed=seed):
                with self.assertRaises(ValueError):
                    self.csv("invalid.csv", records, seed)
                self.assertEqual(list(self.root.iterdir()), [])

    def test_same_data_and_manifest_path_is_rejected(self):
        path = self.root / "same"
        with self.assertRaises(ValueError):
            fixture.generate_csv(path, path, 1)
        self.assertFalse(path.exists())

    def test_binary_repeatability_different_seeds_and_manifest(self):
        a, manifest, result = self.binary("a.bin", 65537)
        b, _, _ = self.binary("b.bin", 65537)
        c, _, _ = self.binary("c.bin", 65537, seed=8)
        self.assertEqual(a.read_bytes(), b.read_bytes())
        self.assertNotEqual(a.read_bytes(), c.read_bytes())
        self.assertEqual(result, json.loads(manifest.read_text(encoding="ascii")))
        self.assertEqual(result["byte_count"], 65537)
        self.assertEqual(result["sha256"], hashlib.sha256(a.read_bytes()).hexdigest())
        self.assertEqual(result["kind"], "synthetic_high_entropy_candidate")
        self.assertEqual(result["generator_version"], "myfs-m0-shake256-v1")
        self.assertEqual(result["block_bytes"], 65536)

    def test_binary_block_boundary_and_prefix_construction(self):
        reference = (
            hashlib.shake_256(b"myfs-m0-shake256-v1|seed=5|block=0\n").digest(65536)
            + hashlib.shake_256(b"myfs-m0-shake256-v1|seed=5|block=1\n").digest(2)
        )
        for length in [0, 1, 65535, 65536, 65537, 65538]:
            with self.subTest(length=length):
                data, _, result = self.binary(f"{length}.bin", length, seed=5)
                self.assertEqual(data.read_bytes(), reference[:length])
                self.assertEqual(result["byte_count"], length)

    def test_binary_overwrite_refusal(self):
        data, manifest, _ = self.binary("control.bin", 7)
        before = (data.read_bytes(), manifest.read_bytes())
        with self.assertRaises(FileExistsError):
            fixture.generate_binary(data, manifest, 3)
        self.assertEqual((data.read_bytes(), manifest.read_bytes()), before)
        other = self.root / "other.bin"
        with self.assertRaises(FileExistsError):
            fixture.generate_binary(other, manifest, 3)
        self.assertFalse(other.exists())

    def test_invalid_binary_parameters_create_nothing(self):
        for length, seed in [(-1, 1), (1, -1), (1, 2**64)]:
            with self.subTest(length=length, seed=seed):
                with self.assertRaises(ValueError):
                    self.binary("invalid.bin", length, seed)
                self.assertEqual(list(self.root.iterdir()), [])

    def test_fixture_cli_csv_binary_and_expected(self):
        data = self.root / "input.csv"
        result = self.cli("m0_fixture", "csv", data, "--records", 32)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["record_count"], 32)
        binary = self.cli("m0_fixture", "binary", self.root / "control.bin", "--bytes", 17)
        self.assertEqual(binary.returncode, 0, binary.stderr)
        self.assertEqual(json.loads(binary.stdout)["byte_count"], 17)
        expected = self.cli("m0_fixture", "expected", data, self.root / "expected.csv")
        self.assertEqual(expected.returncode, 0, expected.stderr)
        refused = self.cli("m0_fixture", "csv", data, "--records", 1)
        self.assertNotEqual(refused.returncode, 0)


class WorkflowTests(OfflineCase):
    def setUp(self):
        super().setUp()
        self.assertIsNotNone(workflow, "The CSV application is not implemented")

    def transform(self, source):
        output = io.StringIO(newline="")
        result = workflow.transform(io.StringIO(source, newline=""), output)
        return output.getvalue().encode("ascii"), result

    def test_literal_golden_boundaries_status_totals_and_order(self):
        output, counts = self.transform(GOLDEN_INPUT)
        self.assertEqual(output, GOLDEN_OUTPUT)
        self.assertEqual(counts, {"input_rows": 6, "output_rows": 3})

    def test_no_matches_and_empty_input_corpus_emit_header_only(self):
        for source in [HEADER, HEADER + "1,1,1,north,49,1,ok,no\n"]:
            with self.subTest(source=source):
                output, counts = self.transform(source)
                self.assertEqual(output, OUT_HEADER)
                self.assertEqual(counts["output_rows"], 0)

    def test_standard_csv_quoting_and_exact_lf_output(self):
        source = HEADER + '1,1,1,north,50,2,ok,"a,b ""quoted"""\n'
        output, _ = self.transform(source)
        self.assertEqual(output, OUT_HEADER + b'1,1,north,100,"a,b ""quoted"""\n')
        self.assertNotIn(b"\r", output)

    def test_malformed_input_is_rejected_even_on_filtered_rows(self):
        bad = [
            "", HEADER.replace("event_id", "wrong", 1),
            HEADER + "1,1,1,north,50,2,ok\n",
            HEADER + "1,1,1,north,50,2,ok,note,extra\n",
            HEADER + "1,1,1,north,no,2,er,note\n",
            HEADER + "1,1,1,north,49,no,er,note\n",
            HEADER + '1,1,1,north,50,2,ok,"unterminated\n',
            HEADER + "\n", HEADER + "1,1,1,north,50,2,ok,café\n",
            HEADER + '1,1,1,north,50,2,ok,"embedded\nnewline"\n',
        ]
        for source in bad:
            with self.subTest(source=source):
                with self.assertRaises(ValueError):
                    self.transform(source)

    def test_already_open_stream_is_not_closed_by_transform(self):
        source, output = io.StringIO(GOLDEN_INPUT), io.StringIO(newline="")
        workflow.transform(source, output)
        self.assertFalse(source.closed)
        self.assertFalse(output.closed)

    def test_cli_runs_on_ordinary_root_and_refuses_existing_output(self):
        (self.root / "input.csv").write_bytes(GOLDEN_INPUT.encode("ascii"))
        result = self.cli("m0_csv_workflow", self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / "output.csv").read_bytes(), GOLDEN_OUTPUT)
        self.assertEqual(json.loads(result.stdout)["status"], "transformed")
        second = self.cli("m0_csv_workflow", self.root)
        self.assertNotEqual(second.returncode, 0)
        self.assertEqual((self.root / "output.csv").read_bytes(), GOLDEN_OUTPUT)

    def test_cli_malformed_input_marks_partial_output_unvalidated(self):
        payload = HEADER + "1,1,1,north,50,2,ok,first\ninvalid\n"
        (self.root / "input.csv").write_bytes(payload.encode("ascii"))
        result = self.cli("m0_csv_workflow", self.root)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn("unvalidated", result.stderr)

    def test_cli_non_ascii_and_missing_input_fail(self):
        missing = self.cli("m0_csv_workflow", self.root)
        self.assertNotEqual(missing.returncode, 0)
        self.assertFalse((self.root / "output.csv").exists())
        (self.root / "input.csv").write_bytes(HEADER.encode("ascii") + b"\xff\n")
        invalid = self.cli("m0_csv_workflow", self.root)
        self.assertNotEqual(invalid.returncode, 0)
        self.assertEqual(invalid.stdout, "")

    def test_output_io_error_propagates(self):
        class BrokenOutput(io.StringIO):
            def write(self, text):
                raise OSError("synthetic output I/O failure")

        with self.assertRaises(OSError):
            workflow.transform(io.StringIO(GOLDEN_INPUT), BrokenOutput())


class OracleTests(OfflineCase):
    def setUp(self):
        super().setUp()
        self.assertIsNotNone(fixture, "The independent oracle is not implemented")
        self.input = self.root / "input.csv"
        self.input.write_bytes(GOLDEN_INPUT.encode("ascii"))

    def validate(self, payload):
        output = self.root / "output.csv"
        output.write_bytes(payload)
        return fixture.validate_output(self.input, output)

    def test_independent_expected_output_matches_literal_golden(self):
        expected = self.root / "expected.csv"
        summary = fixture.write_expected(self.input, expected)
        self.assertEqual(expected.read_bytes(), GOLDEN_OUTPUT)
        self.assertEqual(summary["sha256"], hashlib.sha256(GOLDEN_OUTPUT).hexdigest())
        self.assertEqual(summary["row_count"], 3)
        self.assertEqual(summary["byte_count"], len(GOLDEN_OUTPUT))
        with self.assertRaises(FileExistsError):
            fixture.write_expected(self.input, expected)
        self.assertEqual(expected.read_bytes(), GOLDEN_OUTPUT)

    def test_exact_validation_reports_hashes_sizes_and_rows(self):
        report = self.validate(GOLDEN_OUTPUT)
        self.assertTrue(report["passed"])
        self.assertIsNone(report["first_mismatch"])
        want = {"sha256": hashlib.sha256(GOLDEN_OUTPUT).hexdigest(),
                "byte_count": len(GOLDEN_OUTPUT), "row_count": 3}
        self.assertEqual(report["expected"], want)
        self.assertEqual(report["actual"], want)

    def test_content_errors_are_rejected_with_useful_mismatch(self):
        header, a, b, c = GOLDEN_OUTPUT.splitlines(keepends=True)
        mutations = {
            "incorrect total": GOLDEN_OUTPUT.replace(b"9225", b"9226"),
            "omitted row": header + a + c,
            "duplicated row": header + a + b + b + c,
            "reordered rows": header + c + b + a,
            "truncated output": GOLDEN_OUTPUT[:-7],
            "missing final LF": GOLDEN_OUTPUT[:-1],
            "wrong header": GOLDEN_OUTPUT.replace(b"total_cents", b"wrong_total"),
        }
        for label, payload in mutations.items():
            with self.subTest(label=label):
                report = self.validate(payload)
                self.assertFalse(report["passed"])
                self.assertNotEqual(report["expected"]["sha256"], report["actual"]["sha256"])
                self.assertEqual(report["actual"]["byte_count"], len(payload))
                mismatch = report["first_mismatch"]
                self.assertIsInstance(mismatch["offset"], int)
                self.assertNotEqual(mismatch["expected_hex"], mismatch["actual_hex"])

    def test_oracle_header_only_and_quoted_golden(self):
        cases = [
            (HEADER, OUT_HEADER),
            (HEADER + "1,1,1,north,49,1,ok,no\n", OUT_HEADER),
            (HEADER + '1,1,1,north,50,2,ok,"a,b ""quoted"""\n',
             OUT_HEADER + b'1,1,north,100,"a,b ""quoted"""\n'),
        ]
        for index, (source, want) in enumerate(cases):
            with self.subTest(index=index):
                self.input.write_bytes(source.encode("ascii"))
                path = self.root / f"expected-{index}.csv"
                fixture.write_expected(self.input, path)
                self.assertEqual(path.read_bytes(), want)
                self.assertTrue(self.validate(want)["passed"])

    def test_oracle_rejects_malformed_input(self):
        for source in ["", HEADER + "short\n", HEADER + "\n",
                       HEADER + "1,1,1,north,49,bad,er,note\n"]:
            with self.subTest(source=source):
                self.input.write_bytes(source.encode("ascii"))
                with self.assertRaises(ValueError):
                    self.validate(GOLDEN_OUTPUT)

    def test_bare_cr_blank_record_rejected_by_application_and_oracle(self):
        source = HEADER + "\r"
        self.input.write_bytes(source.encode("ascii"))
        with self.assertRaises(ValueError):
            workflow.transform(io.StringIO(source, newline=""), io.StringIO(newline=""))
        with self.assertRaises(ValueError):
            self.validate(OUT_HEADER)

    def test_oracle_cli_pass_fail_and_missing_output(self):
        output = self.root / "output.csv"
        output.write_bytes(GOLDEN_OUTPUT)
        passed = self.cli("m0_fixture", "validate", self.input, output)
        self.assertEqual(passed.returncode, 0, passed.stderr)
        self.assertTrue(json.loads(passed.stdout)["passed"])
        output.write_bytes(GOLDEN_OUTPUT[:-1])
        failed = self.cli("m0_fixture", "validate", self.input, output)
        self.assertEqual(failed.returncode, 1, failed.stderr)
        self.assertFalse(json.loads(failed.stdout)["passed"])
        missing = self.cli("m0_fixture", "validate", self.input, self.root / "absent")
        self.assertNotEqual(missing.returncode, 0)

    def test_oracle_works_when_application_import_is_forbidden(self):
        # Run in a fresh interpreter: a validator importing/calling the
        # application cannot hide behind the application's test-module import.
        output = self.root / "output.csv"
        output.write_bytes(GOLDEN_OUTPUT)
        code = (
            "import importlib.abc, runpy, sys\n"
            "class BlockApplication(importlib.abc.MetaPathFinder):\n"
            "    def find_spec(self, fullname, path=None, target=None):\n"
            "        if 'm0_csv_workflow' in fullname:\n"
            "            raise ImportError('oracle must not import application')\n"
            "sys.meta_path.insert(0, BlockApplication())\n"
            "sys.argv = [sys.argv[1], 'validate', sys.argv[2], sys.argv[3]]\n"
            "runpy.run_path(sys.argv[0], run_name='__main__')\n"
        )
        result = subprocess.run(
            [sys.executable, "-B", "-c", code,
             str(REPO / "benchmarks/m0_fixture.py"), str(self.input), str(output)],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(json.loads(result.stdout)["passed"])


if __name__ == "__main__":
    unittest.main()
