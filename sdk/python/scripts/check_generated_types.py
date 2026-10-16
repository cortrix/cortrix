#!/usr/bin/env python3
"""Drift gate for ``cortrix/types/_generated.py`` (issue #142).

The committed ``_generated.py`` is the output of ``scripts/generate_types.py``
for the current spec, and its header says it must not be edited by hand. This
check keeps both true: neither the spec, nor the generator, nor the file can
move without a reviewer seeing a recorded baseline change. (Until #142 was
settled the file and the generator differed, and the same check froze that
drift; the ``--reason`` path below is what recorded it.)

Four hashes, all sha256 over LF-normalised bytes (the same value ``shasum -a 256``
prints for the LF files this repository contains):

* G  - the generator's output for the current spec, built in a temporary directory
* C  - the committed ``_generated.py``
* G0, C0 - the values recorded in ``generated_types_baseline.json`` next to this file

The check passes only when G == G0 and C == C0. Otherwise it names what moved,
using the live comparison G == C to tell a regeneration from a hand edit:

* C moved and C == G now: the file was regenerated from the current spec; record
  the new state with ``--update``.
* C moved, G did not, C != G: a hand edit of a generated file.
* G moved, C did not, C != G: the spec or the generator changed and the file was
  not regenerated.
* G moved, C did not, C == G: the spec or the generator was changed to agree with
  the committed file; record it.
* both moved, C != G: the spec changed and the generated file was edited by hand
  in parallel.

``--update`` records the current state. When the recorded state still has C != G
it requires ``--reason`` (one sentence), written into the baseline as
``drift_reason`` so the justification travels with the hashes into the PR diff;
the field is dropped once C == G. ``--update`` refuses to run in CI
(``GITHUB_ACTIONS`` or ``CI`` set) and when the generator produced no output.

When regenerating, also update ``tests/test_types.py``
(``test_generated_module_exports_57_models``) and record the baseline. With the
file and the generator in agreement, as they are now, this check is the plain
equality gate.

Usage::

    python3 sdk/python/scripts/check_generated_types.py            # the gate
    python3 sdk/python/scripts/check_generated_types.py --update --reason "<why>"
"""

import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_GENERATOR = os.path.join(HERE, "generate_types.py")
DEFAULT_SPEC = os.path.normpath(os.path.join(HERE, "..", "..", "..", "api", "openapi.yaml"))
DEFAULT_GENERATED = os.path.normpath(
    os.path.join(HERE, "..", "cortrix", "types", "_generated.py")
)
DEFAULT_BASELINE = os.path.join(HERE, "generated_types_baseline.json")

GENERATED_REL = "sdk/python/cortrix/types/_generated.py"
BASELINE_REL = "sdk/python/scripts/generated_types_baseline.json"
UPDATE_COMMAND = 'python3 sdk/python/scripts/check_generated_types.py --update --reason "<why>"'

KEY_COMMITTED = "committed_file_sha256"
KEY_GENERATOR = "generator_output_sha256"
KEY_REASON = "drift_reason"

# Decision cases (one per reachable combination of the three comparisons).
CASE_FROZEN = "frozen"
CASE_REGENERATED = "regenerated"
CASE_SPEC_CHANGED_AND_REGENERATED = "spec_changed_and_regenerated"
CASE_HAND_EDIT = "hand_edit"
CASE_GENERATOR_MOVED = "generator_moved"
CASE_GENERATOR_CAUGHT_UP = "generator_caught_up"
CASE_BOTH_MOVED = "both_moved"
CASE_NO_BASELINE = "no_baseline"

EXIT_OK = 0
EXIT_DRIFT = 1
EXIT_USAGE = 2


class GeneratorFailed(RuntimeError):
    """The generator returned an error or produced no file."""


class BaselineMalformed(RuntimeError):
    """The baseline file exists but does not carry the two hashes."""


@dataclass
class Outcome:
    ok: bool
    case: str
    messages: List[str]
    g_sha: str
    c_sha: str
    g_lines: int
    c_lines: int
    g_equals_c: bool
    g_matches_baseline: Optional[bool]
    c_matches_baseline: Optional[bool]
    numstat: Optional[Tuple[int, int]]  # (added, removed) committed -> generated; None if unavailable
    cr_stripped: Tuple[int, int]  # CR bytes stripped before hashing: (generator output, committed)


def _load_generator(path: str):
    spec = importlib.util.spec_from_file_location("cortrix_sdk_generate_types", path)
    if spec is None or spec.loader is None:
        raise GeneratorFailed("cannot load the generator at " + path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _normalise(data: bytes) -> Tuple[bytes, int]:
    """LF-normalise; returns (bytes, number of CR bytes stripped)."""
    stripped = data.count(b"\r\n")
    return data.replace(b"\r\n", b"\n"), stripped


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def generator_output(generator: str, spec: str, out_path: str) -> bytes:
    """Run the generator into out_path and return the raw bytes it wrote.

    The generator's own stdout line (``Generated N models -> <path>``) is captured
    so a check run prints nothing but its verdict.
    """
    module = _load_generator(generator)
    sink = io.StringIO()
    with contextlib.redirect_stdout(sink):
        rc = module.generate(spec, out_path)
    if rc != 0 or not os.path.isfile(out_path):
        raise GeneratorFailed("generate_types.py returned %s for spec %s" % (rc, spec))
    with open(out_path, "rb") as f:
        return f.read()


def load_baseline(path: str) -> Optional[Dict[str, str]]:
    """The recorded baseline, or None when no file exists."""
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError) as exc:
        raise BaselineMalformed("%s is not valid JSON: %s" % (path, exc))
    if not isinstance(data, dict):
        raise BaselineMalformed("%s does not hold a JSON object" % path)
    for key in (KEY_COMMITTED, KEY_GENERATOR):
        value = data.get(key)
        if not isinstance(value, str) or len(value) != 64:
            raise BaselineMalformed("%s lacks a 64-hex %s" % (path, key))
    return data


def _git_numstat(committed: str, generated_tmp: str) -> Optional[Tuple[int, int]]:
    """(added, removed) for committed -> generated per git; None when unavailable.

    ``git diff --no-index`` exits 1 both when the files differ and when a file is
    missing, so the exit code alone decides nothing: a parseable ``int<TAB>int``
    first line is required. ``-`` fields (binary) count as unavailable.
    """
    if shutil.which("git") is None:
        return None
    try:
        proc = subprocess.run(
            ["git", "diff", "--no-index", "--numstat", "--", committed, generated_tmp],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError:
        return None
    if proc.returncode == 0:
        return (0, 0)
    if proc.returncode != 1:
        return None
    first = proc.stdout.decode("utf-8", errors="replace").splitlines()
    if not first:
        return None
    fields = first[0].split("\t", 2)
    if len(fields) < 2 or not fields[0].isdigit() or not fields[1].isdigit():
        return None
    return (int(fields[0]), int(fields[1]))


def _decide(
    g_sha: str, c_sha: str, baseline: Optional[Dict[str, str]]
) -> Tuple[bool, str, List[str]]:
    if baseline is None:
        return (
            False,
            CASE_NO_BASELINE,
            ["no baseline recorded at %s; record the current state with: %s"
             % (BASELINE_REL, UPDATE_COMMAND)],
        )
    g_same = g_sha == baseline[KEY_GENERATOR]
    c_same = c_sha == baseline[KEY_COMMITTED]
    equal = g_sha == c_sha
    if g_same and c_same:
        return True, CASE_FROZEN, []
    if not c_same and equal:
        if g_same:
            return (
                False,
                CASE_REGENERATED,
                ["%s was regenerated from the current spec (it now matches the generator's "
                 "output), but %s still records the previous state. Record the new state: %s "
                 "(--reason is not needed once the file and the generator agree)."
                 % (GENERATED_REL, BASELINE_REL, UPDATE_COMMAND)],
            )
        return (
            False,
            CASE_SPEC_CHANGED_AND_REGENERATED,
            ["api/ or scripts/generate_types.py changed and %s was regenerated to match; "
             "%s is behind. Record the new state: %s"
             % (GENERATED_REL, BASELINE_REL, UPDATE_COMMAND)],
        )
    if g_same and not c_same and not equal:
        return (
            False,
            CASE_HAND_EDIT,
            ["%s changed while the generator's output did not, and the two still differ: "
             "that is a hand edit of a generated file. Revert it, or justify it in the PR "
             "and record it with %s (the reason is written into %s)."
             % (GENERATED_REL, UPDATE_COMMAND, BASELINE_REL)],
        )
    if not g_same and c_same and not equal:
        return (
            False,
            CASE_GENERATOR_MOVED,
            ["the generator's output changed (api/ or scripts/generate_types.py was edited) "
             "while %s did not. Regenerate and commit, or record the new baseline with %s "
             "and say why." % (GENERATED_REL, UPDATE_COMMAND)],
        )
    if not g_same and c_same and equal:
        return (
            False,
            CASE_GENERATOR_CAUGHT_UP,
            ["the generator's output now matches %s (api/ or scripts/generate_types.py was "
             "changed to agree with it), but %s still records a drift. Record the new "
             "state: %s" % (GENERATED_REL, BASELINE_REL, UPDATE_COMMAND)],
        )
    if not g_same and not c_same and not equal:
        return (
            False,
            CASE_BOTH_MOVED,
            ["api/ or scripts/generate_types.py changed AND %s was edited by hand in "
             "parallel (the two still differ). Regenerate instead of hand-editing, or "
             "justify both in the PR and record them with %s."
             % (GENERATED_REL, UPDATE_COMMAND)],
        )
    raise AssertionError("unreachable decision state")  # every (g_same, c_same, equal) handled


def run(
    spec: str = DEFAULT_SPEC,
    generated: str = DEFAULT_GENERATED,
    baseline: str = DEFAULT_BASELINE,
    generator: str = DEFAULT_GENERATOR,
) -> Outcome:
    """Evaluate the gate without writing anything."""
    recorded = load_baseline(baseline)
    with open(generated, "rb") as f:
        c_raw = f.read()
    c_bytes, c_cr = _normalise(c_raw)
    with tempfile.TemporaryDirectory() as tmp:
        out_path = os.path.join(tmp, "_generated.py")
        g_raw = generator_output(generator, spec, out_path)
        g_bytes, g_cr = _normalise(g_raw)
        if g_cr:
            # numstat over the LF form, like the hashes.
            with open(out_path, "wb") as f:
                f.write(g_bytes)
        numstat = _git_numstat(generated, out_path)
    g_sha, c_sha = _sha256(g_bytes), _sha256(c_bytes)
    ok, case, messages = _decide(g_sha, c_sha, recorded)
    return Outcome(
        ok=ok,
        case=case,
        messages=messages,
        g_sha=g_sha,
        c_sha=c_sha,
        g_lines=g_bytes.count(b"\n"),
        c_lines=c_bytes.count(b"\n"),
        g_equals_c=g_sha == c_sha,
        g_matches_baseline=None if recorded is None else g_sha == recorded[KEY_GENERATOR],
        c_matches_baseline=None if recorded is None else c_sha == recorded[KEY_COMMITTED],
        numstat=numstat,
        cr_stripped=(g_cr, c_cr),
    )


def write_baseline(path: str, outcome: Outcome, reason: Optional[str]) -> Dict[str, str]:
    """Record the current state; deterministic JSON so a second write is a no-op."""
    record = {KEY_COMMITTED: outcome.c_sha, KEY_GENERATOR: outcome.g_sha}
    if not outcome.g_equals_c:
        if not reason or not reason.strip():
            raise ValueError(
                "the recorded state has %s differing from the generator's output; "
                "--reason \"<one sentence>\" is required so the justification travels with "
                "the hashes" % GENERATED_REL
            )
        record[KEY_REASON] = " ".join(reason.split())
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(record, f, indent=2, sort_keys=True)
        f.write("\n")
    return record


def _report(outcome: Outcome) -> None:
    def tag(matches: Optional[bool]) -> str:
        if matches is None:
            return "no baseline"
        return "matches baseline" if matches else "differs from baseline"

    print("generator output : %s (%d lines) [%s]"
          % (outcome.g_sha, outcome.g_lines, tag(outcome.g_matches_baseline)))
    print("committed file   : %s (%d lines) [%s]"
          % (outcome.c_sha, outcome.c_lines, tag(outcome.c_matches_baseline)))
    print("file == generator: %s" % ("yes" if outcome.g_equals_c else "no"))
    if outcome.numstat is not None:
        print("regenerating would add %d lines and remove %d (git numstat)" % outcome.numstat)
    if outcome.cr_stripped != (0, 0):
        print("CR bytes stripped before hashing: generator output %d, committed file %d"
              % outcome.cr_stripped)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--spec", default=DEFAULT_SPEC, help="path to api/openapi.yaml")
    parser.add_argument("--generated", default=DEFAULT_GENERATED,
                        help="path to the committed _generated.py")
    parser.add_argument("--baseline", default=DEFAULT_BASELINE,
                        help="path to generated_types_baseline.json")
    parser.add_argument("--generator", default=DEFAULT_GENERATOR,
                        help="path to generate_types.py")
    parser.add_argument("--update", action="store_true",
                        help="record the current state as the baseline (not in CI)")
    parser.add_argument("--reason", default=None,
                        help="with --update: why the recorded file differs from the generator")
    args = parser.parse_args(argv)

    try:
        outcome = run(args.spec, args.generated, args.baseline, args.generator)
    except GeneratorFailed as exc:
        print("check_generated_types: %s" % exc, file=sys.stderr)
        return EXIT_USAGE
    except BaselineMalformed as exc:
        print("check_generated_types: %s; fix it by hand or record a fresh one with: %s"
              % (exc, UPDATE_COMMAND), file=sys.stderr)
        return EXIT_USAGE

    _report(outcome)

    if args.update:
        if os.environ.get("GITHUB_ACTIONS") or os.environ.get("CI"):
            print("check_generated_types: --update is a local, deliberate step; it does not "
                  "run in CI (GITHUB_ACTIONS or CI is set).", file=sys.stderr)
            return EXIT_USAGE
        try:
            record = write_baseline(args.baseline, outcome, args.reason)
        except ValueError as exc:
            print("check_generated_types: %s" % exc, file=sys.stderr)
            return EXIT_USAGE
        print("recorded %s" % args.baseline)
        if KEY_REASON in record:
            print("the recorded state has the generated file differing from the generator's "
                  "output; reason recorded: %s" % record[KEY_REASON])
        elif args.reason:
            print("the file and the generator agree; --reason not recorded")
        return EXIT_OK

    if outcome.ok:
        print("generated SDK types: baseline holds")
        return EXIT_OK
    for message in outcome.messages:
        print("check_generated_types: %s" % message, file=sys.stderr)
    return EXIT_DRIFT


if __name__ == "__main__":
    raise SystemExit(main())
