"""The ratchet gate on ``cortrix/types/_generated.py`` (issue #142, step 1).

These cases run the same check CI runs (``scripts/check_generated_types.py``)
against the real tree and against temporary copies of its inputs. The tree is
never written. Each case asserts the decision ``case`` the gate reached, not the
wording of its message.
"""

import importlib.util
import json
import os
import pathlib
import sys

import pytest

yaml = pytest.importorskip("yaml", reason="PyYAML (sdk/python[dev]) is needed to run the generator")

SDK_ROOT = pathlib.Path(__file__).resolve().parents[1]
REPO_ROOT = SDK_ROOT.parents[1]
CHECK_PATH = SDK_ROOT / "scripts" / "check_generated_types.py"
GENERATOR_PATH = SDK_ROOT / "scripts" / "generate_types.py"
SPEC_PATH = REPO_ROOT / "api" / "openapi.yaml"
SCHEMAS_PATH = REPO_ROOT / "api" / "components" / "schemas.yaml"
GENERATED_PATH = SDK_ROOT / "cortrix" / "types" / "_generated.py"
BASELINE_PATH = SDK_ROOT / "scripts" / "generated_types_baseline.json"

# Inserted into the first schema's `properties:` block by text, not by a YAML
# round trip (safe_dump would reorder every mapping and move the generator's
# output for the wrong reason).
PROBE_ANCHOR = "    ChunkStrategy:\n      type: object\n      properties:\n"
PROBE_PROPERTY = "        ratchet_probe:\n          type: string\n"


@pytest.fixture(scope="module")
def check():
    spec = importlib.util.spec_from_file_location("cortrix_sdk_check_generated_types", str(CHECK_PATH))
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # a dataclass in a by-path module needs this
    spec.loader.exec_module(module)
    return module


def _real(check, **overrides):
    kwargs = dict(
        spec=str(SPEC_PATH),
        generated=str(GENERATED_PATH),
        baseline=str(BASELINE_PATH),
        generator=str(GENERATOR_PATH),
    )
    kwargs.update(overrides)
    return check.run(**kwargs)


def _spec_copy_with_probe(tmp_path):
    """A temp api/ layout (the two files the generator reads) with one added property."""
    api = tmp_path / "api"
    (api / "components").mkdir(parents=True)
    (api / "openapi.yaml").write_bytes(SPEC_PATH.read_bytes())
    text = SCHEMAS_PATH.read_text(encoding="utf-8")
    assert text.count(PROBE_ANCHOR) == 1, "the test anchor must match exactly one schema"
    (api / "components" / "schemas.yaml").write_text(
        text.replace(PROBE_ANCHOR, PROBE_ANCHOR + PROBE_PROPERTY), encoding="utf-8"
    )
    return api / "openapi.yaml"


def _hand_edited_copy(tmp_path):
    copy = tmp_path / "_generated_hand_edited.py"
    copy.write_bytes(GENERATED_PATH.read_bytes() + b"\n# a line nobody generated\n")
    return copy


def test_baseline_matches_the_tree(check):
    outcome = _real(check)
    assert outcome.case == check.CASE_FROZEN, outcome.messages
    assert outcome.ok
    assert outcome.g_matches_baseline and outcome.c_matches_baseline
    assert outcome.g_lines > 0 and outcome.c_lines > 0


def test_hand_edit_of_generated_file_fails(check, tmp_path):
    outcome = _real(check, generated=str(_hand_edited_copy(tmp_path)))
    assert outcome.case == check.CASE_HAND_EDIT, outcome.messages
    assert not outcome.ok
    assert outcome.g_matches_baseline and not outcome.c_matches_baseline
    assert not outcome.g_equals_c
    assert any("hand edit" in m for m in outcome.messages)


def test_spec_change_fails(check, tmp_path):
    spec = _spec_copy_with_probe(tmp_path)
    # Positive control: the added property really reaches the generator's output.
    probe_out = tmp_path / "probe_generated.py"
    assert b"ratchet_probe" in check.generator_output(str(GENERATOR_PATH), str(spec), str(probe_out))
    outcome = _real(check, spec=str(spec))
    assert outcome.case == check.CASE_GENERATOR_MOVED, outcome.messages
    assert not outcome.ok
    assert not outcome.g_matches_baseline and outcome.c_matches_baseline
    assert any("generate_types.py" in m for m in outcome.messages)


def test_regeneration_is_recognised(check, tmp_path):
    regenerated = tmp_path / "_generated_fresh.py"
    check.generator_output(str(GENERATOR_PATH), str(SPEC_PATH), str(regenerated))
    outcome = _real(check, generated=str(regenerated))
    assert outcome.g_equals_c
    # Against today's baseline (file != generator) a regeneration is a move of the
    # committed side that must be recorded, never a hand edit. Once a regeneration
    # has been recorded, the same inputs are simply the frozen state.
    expected = check.CASE_FROZEN if outcome.c_matches_baseline else check.CASE_REGENERATED
    assert outcome.case == expected, outcome.messages
    if outcome.case == check.CASE_REGENERATED:
        assert not outcome.ok
        assert not any("hand edit" in m for m in outcome.messages)


def test_spec_change_plus_hand_edit_is_named(check, tmp_path):
    spec = _spec_copy_with_probe(tmp_path)
    outcome = _real(check, spec=str(spec), generated=str(_hand_edited_copy(tmp_path)))
    assert outcome.case == check.CASE_BOTH_MOVED, outcome.messages
    assert not outcome.ok
    assert not outcome.g_matches_baseline and not outcome.c_matches_baseline
    assert not outcome.g_equals_c
    joined = " ".join(outcome.messages)
    assert "generate_types.py" in joined and "hand" in joined


def test_generator_caught_up_is_named(check, tmp_path):
    # The baseline records a drift, but the generator now reproduces the committed
    # file: simulated with a baseline whose committed hash is the real file and a
    # "generated" file that equals the generator's output on both sides.
    regenerated = tmp_path / "_generated_fresh.py"
    check.generator_output(str(GENERATOR_PATH), str(SPEC_PATH), str(regenerated))
    fresh = _real(check, generated=str(regenerated))
    baseline = tmp_path / "baseline.json"
    baseline.write_text(json.dumps({
        check.KEY_COMMITTED: fresh.c_sha,                 # C0 = the regenerated file
        check.KEY_GENERATOR: "0" * 64,                    # G0 = some older generator output
        check.KEY_REASON: "synthetic",
    }), encoding="utf-8")
    outcome = _real(check, generated=str(regenerated), baseline=str(baseline))
    assert outcome.case == check.CASE_GENERATOR_CAUGHT_UP, outcome.messages
    assert not outcome.ok


def test_update_records_the_current_state(check, tmp_path, monkeypatch):
    monkeypatch.delenv("GITHUB_ACTIONS", raising=False)
    monkeypatch.delenv("CI", raising=False)
    baseline = tmp_path / "baseline.json"
    common = ["--spec", str(SPEC_PATH), "--generated", str(GENERATED_PATH),
              "--generator", str(GENERATOR_PATH), "--baseline", str(baseline)]
    # No baseline yet: the bare check fails and says so.
    assert check.main(common) == check.EXIT_DRIFT
    # The recorded state has file != generator, so --update needs a reason.
    assert check.main(common + ["--update"]) == check.EXIT_USAGE
    assert not baseline.exists()
    assert check.main(common + ["--update", "--reason", "frozen for the test"]) == check.EXIT_OK
    first = baseline.read_bytes()
    record = json.loads(first)
    assert set(record) == {check.KEY_COMMITTED, check.KEY_GENERATOR, check.KEY_REASON}
    assert record[check.KEY_REASON] == "frozen for the test"
    assert check.main(common) == check.EXIT_OK
    # A second --update is byte-identical.
    assert check.main(common + ["--update", "--reason", "frozen for the test"]) == check.EXIT_OK
    assert baseline.read_bytes() == first
    # Recording an agreeing pair drops the reason field.
    regenerated = tmp_path / "_generated_fresh.py"
    check.generator_output(str(GENERATOR_PATH), str(SPEC_PATH), str(regenerated))
    agreeing = [a if a != str(GENERATED_PATH) else str(regenerated) for a in common]
    assert check.main(agreeing + ["--update"]) == check.EXIT_OK
    assert set(json.loads(baseline.read_text())) == {check.KEY_COMMITTED, check.KEY_GENERATOR}


def test_update_refuses_in_ci(check, tmp_path, monkeypatch):
    monkeypatch.setenv("GITHUB_ACTIONS", "true")
    baseline = tmp_path / "baseline.json"
    rc = check.main(["--spec", str(SPEC_PATH), "--generated", str(GENERATED_PATH),
                     "--generator", str(GENERATOR_PATH), "--baseline", str(baseline),
                     "--update", "--reason", "should not be written"])
    assert rc == check.EXIT_USAGE
    assert not baseline.exists()


def test_drift_report_is_consistent(check):
    outcome = _real(check)
    if outcome.numstat is None:
        pytest.skip("git not available for numstat")
    assert (outcome.numstat == (0, 0)) == outcome.g_equals_c
    if not outcome.g_equals_c:
        assert outcome.numstat[0] > 0 or outcome.numstat[1] > 0


def test_malformed_baseline_is_a_usage_error(check, tmp_path):
    baseline = tmp_path / "baseline.json"
    baseline.write_text("{\"committed_file_sha256\": \"short\"}", encoding="utf-8")
    rc = check.main(["--spec", str(SPEC_PATH), "--generated", str(GENERATED_PATH),
                     "--generator", str(GENERATOR_PATH), "--baseline", str(baseline)])
    assert rc == check.EXIT_USAGE
    # Positive control for the fixture: the path really is read.
    assert os.path.isfile(str(baseline))
