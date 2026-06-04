#!/usr/bin/env python3
"""
Quick test to debug bootloader sync detection
"""

import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from jtag_chipshouter_isp import RaidenPico

def test_sync():
    print("Testing bootloader sync detection...")
    raiden = RaidenPico(verbose=True)

    if not raiden.connect():
        print("Failed to connect")
        return 1

    try:
        result = raiden.enter_bootloader()
        print(f"\nResult: {result}")
    finally:
        raiden.close()

    return 0

if __name__ == '__main__':
    sys.exit(test_sync())
