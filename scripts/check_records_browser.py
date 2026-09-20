"""Compatibility entry point for the expanded recording/analysis UI checks."""
from pathlib import Path
import runpy
runpy.run_path(str(Path(__file__).with_name('check_analysis_browser.py')), run_name='__main__')
