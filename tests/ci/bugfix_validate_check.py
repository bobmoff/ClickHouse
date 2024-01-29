#!/usr/bin/env python3

from pathlib import Path
import subprocess
import sys
from typing import List, Sequence, Tuple
import csv
import logging

from report import FAIL, OK, JobReport, TestResults, TestResult
from env_helper import TEMP_PATH
from stopwatch import Stopwatch
from ci_config import JobNames


def post_commit_status_from_file(file_path: Path) -> List[str]:
    with open(file_path, "r", encoding="utf-8") as f:
        res = list(csv.reader(f, delimiter="\t"))
    if len(res) < 1:
        raise Exception(f'Can\'t read from "{file_path}"')
    if len(res[0]) != 3:
        raise Exception(f'Can\'t read from "{file_path}"')
    return res[0]


def get_failed_test_cases(file_path: Path) -> Tuple[bool, List[TestResult]]:
    job_report = JobReport.load(from_file=file_path)
    test_results = []  # type: List[TestResult]
    has_failed = False
    for tr in job_report.test_results:
        if tr.status == FAIL:
            has_failed = True
        tr.name = f"{tr.name} with NOT_OK"
        tr.status = OK if tr.status == FAIL else FAIL
        test_results.append(tr)
    return has_failed, test_results


def process_all_results(
    file_paths: Sequence[Path],
) -> Tuple[bool, TestResults]:
    any_failed = False
    all_results = []
    for job_report_path in file_paths:
        has_failed, test_results = get_failed_test_cases(job_report_path)
        any_failed = any_failed or has_failed
        all_results.extend(test_results)
    return any_failed, all_results


def main():
    logging.basicConfig(level=logging.INFO)
    # args = parse_args()
    stopwatch = Stopwatch()
    jobs_to_validate = [JobNames.STATELESS_TEST_RELEASE, JobNames.INTEGRATION_TEST]
    functional_job_report_file = Path(TEMP_PATH) / "functional_test_job_report.json"
    integration_job_report_file = Path(TEMP_PATH) / "integration_test_job_report.json"
    jobs_report_files = {
        JobNames.STATELESS_TEST_RELEASE: functional_job_report_file,
        JobNames.INTEGRATION_TEST: integration_job_report_file,
    }
    jobs_scripts = {
        JobNames.STATELESS_TEST_RELEASE: "functional_test_check.py",
        JobNames.INTEGRATION_TEST: "integration_test_check.py",
    }

    for test_job in jobs_to_validate:
        report_file = jobs_report_files[test_job]
        test_script = jobs_scripts[test_job]
        if report_file.exists():
            report_file.unlink()
        extra_timeout_option = ""
        if test_job == JobNames.STATELESS_TEST_RELEASE:
            extra_timeout_option = str(3600)
        check_name = f"Validate: {test_job}"
        command = f"python3 {test_script} '{check_name}' {extra_timeout_option} --validate-bugfix --report-to-file {report_file}"
        print(f"Going to validate job [{test_job}], command [{command}]")
        _ = subprocess.run(
            command,
            stdout=sys.stdout,
            stderr=sys.stderr,
            text=True,
            check=False,
            shell=True,
        )
        assert (
            report_file.is_file()
        ), f"No job report [{report_file}] found after job execution"

    is_ok, test_results = process_all_results(list(jobs_report_files.values()))

    description = "Tests failed to reproduce bug"
    if not is_ok:
        description = "Changed tests don't reproduce the bug"

    additional_files = []
    for file in jobs_report_files.values():
        additional_files.append(file)
        job_report = JobReport.load(from_file=file)
        additional_files.extend(job_report.additional_files)

    JobReport(
        description=description,
        test_results=test_results,
        status="success" if is_ok else "error",
        start_time=stopwatch.start_time_str,
        duration=stopwatch.duration_seconds,
        additional_files=additional_files,
    ).dump()


if __name__ == "__main__":
    main()
