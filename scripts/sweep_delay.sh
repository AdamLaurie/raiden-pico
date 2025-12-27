#!/bin/bash
# Sweep delay from 0 to 1000 in steps of 10 at 201V
# Loop until SUCCESS is found

cd /home/addy/work/claude-code/raiden-pico/scripts

while true; do
    for DELAY in $(seq 0 10 1000); do
        echo "=== Testing 201V delay=$DELAY ==="
        python3 -u crp3_fast_glitch.py 201 $DELAY 112 100

        # Check if SUCCESS was found in the most recent CSV
        LATEST_CSV=$(ls -t crp3_fast_V201_P${DELAY}_*.csv 2>/dev/null | head -1)
        if [ -n "$LATEST_CSV" ] && grep -q "SUCCESS" "$LATEST_CSV"; then
            echo "*** SUCCESS FOUND at delay=$DELAY ***"
            echo "CSV: $LATEST_CSV"
            exit 0
        fi
    done
    echo "=== Completed full sweep, starting again ==="
done
