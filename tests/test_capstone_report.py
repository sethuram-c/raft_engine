#!/usr/bin/env python3
"""Phase 35: validate acceptance report behavior without launching a cluster."""
import importlib.util
import pathlib
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('raft_capstone', root/'tools/raft_capstone.py')
assert spec and spec.loader
cap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cap)

with tempfile.TemporaryDirectory() as temp:
    folder = pathlib.Path(temp)
    good = folder/'good.py'
    good.write_text('print("PASS capstone: synthetic success")\n')
    bad = folder/'bad.py'
    bad.write_text('print("SECRET: do not copy this into the report")\n'
                   'raise SystemExit(7)\n')
    success = cap.run_stage('good', good, None, 5, folder)
    failure = cap.run_stage('bad', bad, None, 5, folder)
    assert success['passed'] is True and success['exit_code'] == 0
    assert success['pass_count'] == 1 and success['elapsed_ms'] >= 0
    assert failure['passed'] is False and failure['exit_code'] == 7
    assert 'SECRET' not in str(failure)
    executable = pathlib.Path(sys.executable)
    result = cap.build_report(executable, [success, failure], False)
    assert result['schema'] == 'raft-capstone-v1'
    assert result['passed'] is False and len(result['binary_sha256']) == 64
    result2 = cap.build_report(executable, [success], False)
    assert result2['passed'] is True and result2['extended'] is False
    print('PASS capstone report: success/failure, hashes, no raw secrets, overall result')
