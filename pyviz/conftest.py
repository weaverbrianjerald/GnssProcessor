"""Make the bundled ``gnssviz`` package importable when running pytest from here."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
