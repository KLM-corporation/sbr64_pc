#!/usr/bin/env python3
"""Run Sackboy! Run! Native PC Port (sbr64_pc) - Central E2E Test Suite Runner.

Supports running tests by tier (1-4, plus the opt-in live tier 5), milestone (M1-M6), or feature (1-24),
exporting JSON reports, and returning exit code 0 on full pass.
"""

import argparse
import json
import os
import sys
import time
import unittest

# Ensure tests/e2e and project root are in sys.path
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)
if PROJECT_ROOT not in sys.path:
    sys.path.insert(0, PROJECT_ROOT)

FEATURE_TO_MILESTONE = {
    1: "M1", 2: "M1", 3: "M1", 4: "M1", 5: "M1",
    6: "M2", 7: "M2", 8: "M2", 9: "M2", 10: "M2",
    11: "M3", 12: "M3", 13: "M3", 14: "M3",
    15: "M4", 16: "M4", 17: "M4", 18: "M4",
    19: "M5", 20: "M5", 21: "M5", 22: "M5",
    23: "M6", 24: "M6",
}


def get_tier_dir(tier: int) -> str:
    tiers = {
        1: os.path.join(SCRIPT_DIR, "tier1_features"),
        2: os.path.join(SCRIPT_DIR, "tier2_boundaries"),
        3: os.path.join(SCRIPT_DIR, "tier3_combinations"),
        4: os.path.join(SCRIPT_DIR, "tier4_scenarios"),
        5: os.path.join(SCRIPT_DIR, "tier5_live"),  # launches the real game: opt-in with --tier 5
    }
    return tiers.get(tier, "")


def collect_test_modules(tier: int = None, milestone: str = None, feature: int = None) -> list:
    """Collect test module names according to filtering criteria."""
    tiers_to_scan = [tier] if tier in [1, 2, 3, 4, 5] else [1, 2, 3, 4]
    modules = []

    for t in tiers_to_scan:
        tdir = get_tier_dir(t)
        if not os.path.exists(tdir):
            continue
        for fname in sorted(os.listdir(tdir)):
            if not fname.startswith("test_") or not fname.endswith(".py"):
                continue

            # Check feature filter
            if feature is not None:
                feat_tag = f"{feature:02d}"
                if feat_tag not in fname:
                    continue

            # Check milestone filter
            if milestone is not None:
                # Find matching feature for file
                # Extract number from fname if present (e.g. test_f01_... -> 1)
                matched_ms = None
                for f_num, ms in FEATURE_TO_MILESTONE.items():
                    if f"{f_num:02d}" in fname:
                        matched_ms = ms
                        break
                if matched_ms and matched_ms != milestone.upper():
                    continue

            pkg_name = f"tests.e2e.{os.path.basename(tdir)}.{fname[:-3]}"
            modules.append(pkg_name)

    return modules


def run_e2e_tests(tier: int = None, milestone: str = None, feature: int = None, verbose: bool = False, json_path: str = None) -> int:
    start_time = time.time()
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()

    modules = collect_test_modules(tier=tier, milestone=milestone, feature=feature)
    if not modules:
        print("[-] No test modules found matching filter.")
        return 1

    for mod_name in modules:
        try:
            mod_suite = loader.loadTestsFromName(mod_name)
            suite.addTest(mod_suite)
        except Exception as e:
            print(f"[-] Error loading module {mod_name}: {e}")
            return 1

    total_tests = suite.countTestCases()
    print("=" * 80)
    print(" Run Sackboy! Run! Native PC Port (sbr64_pc) - E2E Test Suite Runner")
    print("=" * 80)
    filter_desc = []
    if tier: filter_desc.append(f"Tier: {tier}")
    if milestone: filter_desc.append(f"Milestone: {milestone.upper()}")
    if feature: filter_desc.append(f"Feature: {feature}")
    print(f"Filters: {', '.join(filter_desc) if filter_desc else 'None (All Tests)'}")
    print(f"Modules discovered: {len(modules)} | Total test cases: {total_tests}")
    print("-" * 80)

    verbosity = 2 if verbose else 1
    runner = unittest.TextTestRunner(verbosity=verbosity)
    result = runner.run(suite)

    elapsed = time.time() - start_time
    passed = result.testsRun - len(result.failures) - len(result.errors) - len(result.skipped)

    print("-" * 80)
    print(f"Results: {result.testsRun} run, {passed} passed, {len(result.failures)} failed, {len(result.errors)} errors, {len(result.skipped)} skipped in {elapsed:.3f}s")
    print("=" * 80)

    if json_path:
        report_data = {
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "total": result.testsRun,
            "passed": passed,
            "failures": len(result.failures),
            "errors": len(result.errors),
            "skipped": len(result.skipped),
            "elapsed_seconds": elapsed,
            "success": result.wasSuccessful(),
        }
        with open(json_path, "w", encoding="utf-8") as f:
            json.dump(report_data, f, indent=2)
        print(f"[+] JSON report saved to {json_path}")

    return 0 if result.wasSuccessful() else 1


def main():
    parser = argparse.ArgumentParser(description="sbr64_pc E2E Test Suite Runner")
    parser.add_argument("--tier", type=int, choices=[1, 2, 3, 4, 5],
                        help="Run specific tier (1-4: contract/simulation tests; 5: live runs of the real game)")
    parser.add_argument("--milestone", type=str, choices=["M1", "M2", "M3", "M4", "M5", "M6", "m1", "m2", "m3", "m4", "m5", "m6"], help="Filter tests by milestone")
    parser.add_argument("--feature", type=int, choices=range(1, 25), help="Filter tests by feature index (1..24)")
    parser.add_argument("-v", "--verbose", action="store_true", help="Verbose test execution output")
    parser.add_argument("--json-report", type=str, help="Path to write JSON test execution report")

    args = parser.parse_args()
    code = run_e2e_tests(
        tier=args.tier,
        milestone=args.milestone,
        feature=args.feature,
        verbose=args.verbose,
        json_path=args.json_report,
    )
    sys.exit(code)


if __name__ == "__main__":
    main()
