#!/usr/bin/env python3
"""Run only synthetic unit tests: no DSM media, network or real guest required."""
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'kernel/stock-board'), str(ROOT / 'scripts')]


def main():
    suite = unittest.TestSuite()
    for directory, pattern in ((ROOT / 'kernel/stock-board', 'test-*.py'), (ROOT / 'tests', 'test_*.py')):
        for path in sorted(directory.glob(pattern)):
            if 'import unittest' not in path.read_text():
                raise RuntimeError(f'Not a synthetic unittest module: {path.name}')
            spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            suite.addTests(unittest.defaultTestLoader.loadTestsFromModule(module))
    if not suite.countTestCases():
        raise RuntimeError('No tests discovered')
    return not unittest.TextTestRunner(verbosity=2, buffer=True).run(suite).wasSuccessful()


if __name__ == '__main__':
    raise SystemExit(main())
