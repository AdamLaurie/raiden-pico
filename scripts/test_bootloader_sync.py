#!/usr/bin/env python3
"""
Quick test to debug bootloader sync detection
"""

import sys
sys.path.insert(0, '/home/addy/work/claude-code/raiden-pico/scripts')

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
