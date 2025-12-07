#!/usr/bin/env python3
"""
Glitch Heat Map - Spiral scan with glitch testing at each position.
Creates a heat map based on hit rate: 0% = green, 100% = red.

Features:
- Forward or reverse spiral (command line option)
- Voltage optimization: reduces voltage when >50% hit rate until ~50%
- CSV logging: all tests logged to glitch_log.csv
- Hover tooltip: shows coordinates and hit % on web UI

Hit criteria:
- Error 19 = normal (miss)
- No reply = effect/crash (hit)
- 0 = glitched (hit)

Web server at http://localhost:8080 shows real-time heat map.

Usage:
  python3 glitch_heatmap.py           # Forward spiral (0,0) -> center
  python3 glitch_heatmap.py --reverse # Reverse spiral center -> (0,0)
"""
import serial
import time
import threading
import json
import csv
import os
import argparse
from datetime import datetime
from http.server import HTTPServer, BaseHTTPRequestHandler
import queue

# Parse command line arguments
parser = argparse.ArgumentParser(description='Glitch Heat Map Scanner')
parser.add_argument('--reverse', '-r', action='store_true',
                    help='Reverse spiral: start from center, end at (0,0)')
parser.add_argument('--forward', '-f', action='store_true',
                    help='Forward spiral: start from (0,0), end at center (default)')
args = parser.parse_args()

# Determine direction (default is forward)
REVERSE_SPIRAL = args.reverse and not args.forward

# Grid size
SIZE = 23  # 24x24 grid (0-23)

# Glitch test parameters
SHOTS_PER_POSITION = 15  # Number of glitch attempts per position
TARGET_BAUD = 115200
CRYSTAL_KHZ = 8000
RESET_DELAY = 300

# ChipSHOUTER parameters
CS_VOLTAGE = 500  # Starting voltage in V
CS_VOLTAGE_MIN = 100  # Minimum voltage
CS_VOLTAGE_STEP = 50  # Voltage reduction step
CS_PULSE_WIDTH = 50  # Pulse width in us

# Current voltage (can be reduced during optimization)
current_voltage = CS_VOLTAGE

# Shared state for web visualization
visited = {}  # (x, y) -> {'hit_rate': 0.0-1.0, 'voltage': V}
current_pos = (0, 0)
update_queue = queue.Queue()
server_ready = threading.Event()

# CSV log file
CSV_FILE = f"glitch_log_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
csv_lock = threading.Lock()

def init_csv():
    """Initialize CSV log file with headers"""
    with open(CSV_FILE, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['timestamp', 'x', 'y', 'voltage', 'pulse_width',
                        'shot_num', 'result', 'hit_rate', 'optimized_voltage'])

def log_test(x, y, voltage, shot_num, result, hit_rate=None, optimized_voltage=None):
    """Log a single test to CSV"""
    with csv_lock:
        with open(CSV_FILE, 'a', newline='') as f:
            writer = csv.writer(f)
            writer.writerow([
                datetime.now().isoformat(),
                x, y, voltage, CS_PULSE_WIDTH,
                shot_num, result,
                f"{hit_rate:.3f}" if hit_rate is not None else "",
                optimized_voltage if optimized_voltage else ""
            ])

HTML_PAGE = '''<!DOCTYPE html>
<html>
<head>
    <title>Glitch Heat Map</title>
    <style>
        body {
            background: #1a1a2e;
            color: #eee;
            font-family: monospace;
            display: flex;
            flex-direction: column;
            align-items: center;
            padding: 20px;
        }
        h1 { color: #00ff88; }
        #status {
            margin: 10px 0;
            padding: 10px;
            background: #16213e;
            border-radius: 5px;
        }
        #container {
            position: relative;
            display: inline-block;
        }
        canvas {
            border: 2px solid #00ff88;
            background: #0f0f23;
        }
        #tooltip {
            position: fixed;
            background: rgba(0, 0, 0, 0.95);
            color: #fff;
            padding: 8px 12px;
            border-radius: 5px;
            border: 1px solid #00ff88;
            font-size: 14px;
            pointer-events: none;
            display: none;
            z-index: 1000;
            white-space: nowrap;
        }
        #info {
            margin-top: 10px;
            color: #888;
        }
        #legend {
            display: flex;
            align-items: center;
            margin-top: 10px;
            gap: 10px;
        }
        #gradient {
            width: 200px;
            height: 20px;
            background: linear-gradient(to right, #00ff00, #ffff00, #ff0000);
            border-radius: 3px;
        }
    </style>
</head>
<body>
    <h1>Glitch Heat Map</h1>
    <div id="status">Connecting...</div>
    <div id="container">
        <canvas id="grid" width="360" height="360"></canvas>
        <div id="tooltip"></div>
    </div>
    <div id="legend">
        <span>0% (normal)</span>
        <div id="gradient"></div>
        <span>100% (effect)</span>
        <span style="margin-left: 20px; color: #0088ff;">&#9632; GLITCH</span>
        <span style="margin-left: 10px; color: #aa00ff;">&#9632; Threshold</span>
    </div>
    <div id="info">24x24mm grid (15px/cell). Spiral scan pattern. Hover for details. Blue=successful glitch, Purple=voltage threshold found.</div>

    <script>
        const canvas = document.getElementById('grid');
        const ctx = canvas.getContext('2d');
        const status = document.getElementById('status');
        const tooltip = document.getElementById('tooltip');
        const CELL_SIZE = 15;
        const GRID_SIZE = 24;
        let prevPos = null;

        // Store cell data for tooltips
        let cellData = {};  // "x,y" -> {hitRate, voltage, special}

        function drawGrid() {
            ctx.fillStyle = '#0f0f23';
            ctx.fillRect(0, 0, canvas.width, canvas.height);
        }

        function hitRateToColor(rate, special) {
            // Special colors: 'glitch' = blue, 'threshold' = purple
            if (special === 'glitch') {
                return '#0088ff';  // Blue for successful glitch
            }
            if (special === 'threshold') {
                return '#aa00ff';  // Purple for threshold hit
            }
            let r, g, b;
            if (rate <= 0.5) {
                r = Math.round(255 * (rate * 2));
                g = 255;
                b = 0;
            } else {
                r = 255;
                g = Math.round(255 * (1 - (rate - 0.5) * 2));
                b = 0;
            }
            return `rgb(${r}, ${g}, ${b})`;
        }

        function drawCell(x, y, hitRate, special) {
            const canvasY = y * CELL_SIZE;
            const canvasX = x * CELL_SIZE;
            ctx.fillStyle = hitRateToColor(hitRate, special);
            ctx.fillRect(canvasX + 1, canvasY + 1, CELL_SIZE - 2, CELL_SIZE - 2);
        }

        function drawCurrent(x, y) {
            const canvasY = y * CELL_SIZE;
            const canvasX = x * CELL_SIZE;
            ctx.strokeStyle = '#ffffff';
            ctx.lineWidth = 2;
            ctx.strokeRect(canvasX + 1, canvasY + 1, CELL_SIZE - 2, CELL_SIZE - 2);
        }

        // Tooltip handling - use fixed positioning with page coordinates
        canvas.addEventListener('mousemove', function(e) {
            var rect = canvas.getBoundingClientRect();
            var mouseX = e.clientX - rect.left;
            var mouseY = e.clientY - rect.top;

            var cellX = Math.floor(mouseX / CELL_SIZE);
            var cellY = Math.floor(mouseY / CELL_SIZE);

            if (cellX >= 0 && cellX < GRID_SIZE && cellY >= 0 && cellY < GRID_SIZE) {
                var key = cellX + ',' + cellY;
                var cellInfo = cellData[key];

                var tooltipHtml;
                if (cellInfo) {
                    var pct = (cellInfo.hitRate * 100).toFixed(1);
                    var specialText = '';
                    if (cellInfo.special === 'glitch') {
                        specialText = '<br><span style="color:#0088ff">GLITCH!</span>';
                    } else if (cellInfo.special === 'threshold') {
                        specialText = '<br><span style="color:#aa00ff">Threshold</span>';
                    }
                    tooltipHtml = '<b>(' + cellX + ', ' + cellY + ')</b><br>Hit: ' + pct + '%<br>Voltage: ' + cellInfo.voltage + 'V' + specialText;
                } else {
                    tooltipHtml = '<b>(' + cellX + ', ' + cellY + ')</b><br>Not tested';
                }
                tooltip.innerHTML = tooltipHtml;
                tooltip.style.display = 'block';
                tooltip.style.left = (e.clientX + 15) + 'px';
                tooltip.style.top = (e.clientY + 15) + 'px';
            } else {
                tooltip.style.display = 'none';
            }
        });

        canvas.addEventListener('mouseleave', function() {
            tooltip.style.display = 'none';
        });

        drawGrid();

        const evtSource = new EventSource('/events');

        evtSource.onopen = function() {
            status.textContent = 'Connected - waiting for tests...';
        };

        evtSource.onmessage = function(event) {
            const data = JSON.parse(event.data);

            if (data.type === 'init') {
                drawGrid();
                Object.entries(data.visited).forEach(([key, info]) => {
                    const [x, y] = key.split(',').map(Number);
                    cellData[key] = info;
                    drawCell(x, y, info.hitRate, info.special);
                });
                if (data.current) {
                    drawCurrent(data.current[0], data.current[1]);
                }
                status.textContent = `Tested: ${Object.keys(data.visited).length} / 576 positions`;
            }
            else if (data.type === 'move') {
                if (prevPos) {
                    const key = `${prevPos[0]},${prevPos[1]}`;
                    if (cellData[key]) {
                        drawCell(prevPos[0], prevPos[1], cellData[key].hitRate, cellData[key].special);
                    }
                }
                drawCurrent(data.x, data.y);
                prevPos = [data.x, data.y];
                status.textContent = `Testing: (${data.x}, ${data.y}) @ ${data.voltage}V - ${data.count} / 576 positions`;
            }
            else if (data.type === 'result') {
                const key = `${data.x},${data.y}`;
                cellData[key] = {hitRate: data.hitRate, voltage: data.voltage, special: data.special};
                drawCell(data.x, data.y, data.hitRate, data.special);
                drawCurrent(data.x, data.y);
                prevPos = [data.x, data.y];
                const pct = (data.hitRate * 100).toFixed(0);
                let specialMsg = data.special === 'glitch' ? ' [GLITCH!]' : (data.special === 'threshold' ? ' [threshold]' : '');
                let optMsg = data.optimized ? ` (optimized from ${data.startVoltage}V)` : '';
                status.textContent = `(${data.x}, ${data.y}): ${pct}% @ ${data.voltage}V${optMsg}${specialMsg} - ${data.count} / 576 positions`;
            }
            else if (data.type === 'complete') {
                status.textContent = `Complete! Tested ${data.count} positions. Log: ${data.csvFile}`;
                status.style.color = '#00ff88';
            }
        };

        evtSource.onerror = function() {
            status.textContent = 'Connection lost - refresh to reconnect';
            status.style.color = '#ff4444';
        };
    </script>
</body>
</html>
'''

class RequestHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def do_GET(self):
        if self.path == '/':
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.end_headers()
            self.wfile.write(HTML_PAGE.encode())

        elif self.path == '/events':
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Cache-Control', 'no-cache')
            self.send_header('Connection', 'keep-alive')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()

            # Send initial state
            init_data = {
                'type': 'init',
                'visited': {f"{x},{y}": info for (x, y), info in visited.items()},
                'current': current_pos
            }
            self.wfile.write(f"data: {json.dumps(init_data)}\n\n".encode())
            self.wfile.flush()

            try:
                while True:
                    try:
                        update = update_queue.get(timeout=1.0)
                        self.wfile.write(f"data: {json.dumps(update)}\n\n".encode())
                        self.wfile.flush()
                    except queue.Empty:
                        self.wfile.write(": keepalive\n\n".encode())
                        self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass

        else:
            self.send_response(404)
            self.end_headers()

def run_server():
    server = HTTPServer(('0.0.0.0', 8080), RequestHandler)
    server_ready.set()
    print("Web server running at http://localhost:8080", flush=True)
    server.serve_forever()

def broadcast_move(x, y, voltage):
    global current_pos
    current_pos = (x, y)
    update = {
        'type': 'move',
        'x': x,
        'y': y,
        'voltage': voltage,
        'count': len(visited)
    }
    update_queue.put(update)

def broadcast_result(x, y, hit_rate, voltage, optimized=False, start_voltage=None, special=None):
    """
    Broadcast result to web UI.
    special: None (normal), 'glitch' (blue), or 'threshold' (purple)
    """
    global current_pos
    visited[(x, y)] = {'hitRate': hit_rate, 'voltage': voltage, 'special': special}
    current_pos = (x, y)
    update = {
        'type': 'result',
        'x': x,
        'y': y,
        'hitRate': hit_rate,
        'voltage': voltage,
        'optimized': optimized,
        'startVoltage': start_voltage,
        'special': special,
        'count': len(visited)
    }
    update_queue.put(update)

def broadcast_complete():
    update = {
        'type': 'complete',
        'count': len(visited),
        'csvFile': CSV_FILE
    }
    update_queue.put(update)

# Initialize CSV
init_csv()

# Start web server in background
server_thread = threading.Thread(target=run_server, daemon=True)
server_thread.start()
server_ready.wait()

# Serial connection to Raiden
s = serial.Serial('/dev/ttyACM1', 115200, timeout=5)
time.sleep(1)

def send_cmd(cmd, timeout=5):
    """Send command to Raiden and wait for response"""
    s.write((cmd + '\r\n').encode())
    time.sleep(0.1)
    response = ""
    start = time.time()
    while time.time() - start < timeout:
        if s.in_waiting:
            response += s.read(s.in_waiting).decode('utf-8', errors='ignore')
            if 'OK:' in response or 'ERROR:' in response or '> ' in response:
                break
        time.sleep(0.05)
    return response

def send_raw(cmd):
    """Send command without waiting for structured response"""
    s.write((cmd + '\r\n').encode())
    time.sleep(0.1)

def read_until_prompt(timeout=3):
    """Read until we see the prompt"""
    response = ""
    start = time.time()
    while time.time() - start < timeout:
        if s.in_waiting:
            chunk = s.read(s.in_waiting).decode('utf-8', errors='ignore')
            response += chunk
            if '> ' in chunk:
                break
        time.sleep(0.05)
    return response

def grbl_move(x, y):
    """Move XY platform to position"""
    resp = send_cmd(f'grbl move {x} {y}', timeout=35)
    if 'ERROR' in resp:
        print(f"  Move ERROR: {resp}", flush=True)
        return False
    return True

def set_chipshouter_voltage(voltage):
    """Set ChipSHOUTER voltage"""
    global current_voltage
    send_cmd(f"CS VOLTAGE {voltage}")
    current_voltage = voltage
    time.sleep(0.2)

def check_and_reset_chipshouter():
    """Check ChipSHOUTER status and reset if errored. Returns True if OK."""
    s.write(('CS STATUS\r\n').encode())
    time.sleep(0.2)
    response = ""
    start = time.time()
    while time.time() - start < 2.0:
        if s.in_waiting:
            response += s.read(s.in_waiting).decode('utf-8', errors='ignore')
            if "> " in response:
                break
        time.sleep(0.05)

    # Check for error conditions
    if "error" in response.lower() or "fault" in response.lower() or "disarmed" in response.lower():
        print("\n  [CS ERROR - resetting]", end='', flush=True)
        # Reset ChipSHOUTER
        send_cmd("CS RESET")
        time.sleep(1.0)
        send_cmd(f"CS VOLTAGE {current_voltage}")
        send_cmd(f"CS PULSE {CS_PULSE_WIDTH}")
        send_cmd("CS TRIGGER HW HIGH")
        send_cmd("CS ARM")
        time.sleep(0.5)
        return False
    return True

def run_glitch_test():
    """
    Run a single glitch test.
    Returns: 'normal' (error 19), 'effect' (no reply), 'glitch' (0), or 'cs_error'
    """
    # Check ChipSHOUTER status first
    if not check_and_reset_chipshouter():
        return 'cs_error'

    # Setup UART trigger for 0x0D (carriage return)
    send_cmd("TRIGGER UART 0x0D")
    send_cmd("SET PAUSE 5000")
    send_cmd("SET WIDTH 150")
    send_cmd("SET COUNT 1")

    # Arm the glitch system
    send_cmd("ARM ON")

    # Send the read command - triggers on \r
    send_raw('TARGET SEND "R 0 4"')

    # Wait for response
    response = read_until_prompt(timeout=3)

    # Parse response
    for line in response.split('\n'):
        line = line.strip()
        if line.startswith("R 0 4"):
            isp_response = line[5:].strip()
            if isp_response == "19":
                return 'normal'
            elif isp_response == "0" or (isp_response.startswith("0") and len(isp_response) >= 4):
                return 'glitch'
            elif not isp_response:
                return 'effect'
            else:
                return 'effect'  # Unexpected = probably crash

    # Check for hex format responses
    if "31 39" in response:
        return 'normal'
    elif "30 " in response or "30 0D" in response:
        return 'glitch'
    elif "No response" in response or not response.strip():
        return 'effect'

    return 'effect'  # Default: assume crash/effect

def sync_target():
    """Sync with target ISP bootloader"""
    send_cmd("TARGET LPC")
    send_raw(f"TARGET SYNC {TARGET_BAUD} {CRYSTAL_KHZ} {RESET_DELAY}")

    response = ""
    start = time.time()
    while time.time() - start < 5:
        if s.in_waiting:
            chunk = s.read(s.in_waiting).decode('utf-8', errors='ignore')
            response += chunk
            if "LPC ISP sync complete" in response:
                return True
            if "> " in chunk:
                break
        time.sleep(0.1)
    return False

def test_position(x, y, voltage, shots=SHOTS_PER_POSITION):
    """
    Run multiple glitch tests at current position.
    Returns: (hit_rate, completed_count, glitch_found)
    """
    hits = 0
    completed = 0
    retries = 0
    max_retries = 3
    glitch_found = False

    while completed < shots:
        # Sync with target before each test
        if not sync_target():
            print(f"S", end='', flush=True)  # S = sync failed
            log_test(x, y, voltage, completed + 1, 'sync_fail')
            hits += 1
            completed += 1
            continue

        result = run_glitch_test()

        if result == 'cs_error':
            # ChipSHOUTER error - retry this shot (don't count)
            retries += 1
            print("R", end='', flush=True)  # R = retry
            log_test(x, y, voltage, completed + 1, 'cs_error')
            if retries > max_retries:
                print("\n  [Too many CS errors, skipping]", flush=True)
                break
            continue

        retries = 0  # Reset retry counter on success
        completed += 1

        # Log the test result
        log_test(x, y, voltage, completed, result)

        if result == 'effect' or result == 'glitch':
            hits += 1
            if result == 'glitch':
                glitch_found = True
                print("+", end='', flush=True)
            else:
                print("!", end='', flush=True)
        else:
            print(".", end='', flush=True)

        time.sleep(0.2)

    hit_rate = hits / max(completed, 1)
    return hit_rate, completed, glitch_found

def optimize_voltage(x, y, initial_hit_rate, initial_glitch):
    """
    Reduce voltage until hit rate drops to ~50%.
    If hit rate drops from high to 0%, fine-tune with binary search.

    Returns: (final_hit_rate, final_voltage, was_optimized, special)
    special: None, 'glitch', or 'threshold'
    """
    global current_voltage

    if initial_hit_rate <= 0.5:
        # Already at or below 50% - mark as threshold since we have hits but can't optimize further
        # (Any hits at all is considered interesting)
        special = 'glitch' if initial_glitch else 'threshold'
        return initial_hit_rate, current_voltage, False, special

    start_voltage = current_voltage
    prev_voltage = current_voltage
    prev_hit_rate = initial_hit_rate
    glitch_found = initial_glitch

    print(f"\n    Optimizing voltage (starting at {current_voltage}V, {initial_hit_rate*100:.0f}%)", flush=True)

    while current_voltage > CS_VOLTAGE_MIN:
        # Reduce voltage by one step
        new_voltage = current_voltage - CS_VOLTAGE_STEP
        if new_voltage < CS_VOLTAGE_MIN:
            new_voltage = CS_VOLTAGE_MIN

        print(f"    Trying {new_voltage}V: ", end='', flush=True)
        set_chipshouter_voltage(new_voltage)
        time.sleep(0.3)

        # Re-arm after voltage change
        send_cmd("CS ARM")
        time.sleep(0.5)

        # Test at new voltage (fewer shots for optimization)
        hit_rate, _, glitch = test_position(x, y, new_voltage, shots=10)
        if glitch:
            glitch_found = True

        print(f" = {hit_rate*100:.0f}%", flush=True)

        # Check for sharp drop (high -> 0%)
        if prev_hit_rate >= 0.5 and hit_rate < 0.1:
            # Sharp drop! Fine-tune between prev_voltage and new_voltage
            print(f"    Sharp drop detected ({prev_hit_rate*100:.0f}% -> {hit_rate*100:.0f}%), fine-tuning...", flush=True)

            # Binary search between prev_voltage (high) and new_voltage (low)
            high_v = prev_voltage
            low_v = new_voltage
            threshold_voltage = prev_voltage
            threshold_rate = prev_hit_rate

            # Do a few binary search iterations
            for _ in range(3):
                mid_v = (high_v + low_v) // 2
                if mid_v == high_v or mid_v == low_v:
                    break

                print(f"    Binary search {mid_v}V: ", end='', flush=True)
                set_chipshouter_voltage(mid_v)
                time.sleep(0.3)
                send_cmd("CS ARM")
                time.sleep(0.5)

                mid_rate, _, glitch = test_position(x, y, mid_v, shots=10)
                if glitch:
                    glitch_found = True
                print(f" = {mid_rate*100:.0f}%", flush=True)

                if mid_rate >= 0.3:
                    # Still has hits, this is our threshold
                    threshold_voltage = mid_v
                    threshold_rate = mid_rate
                    high_v = mid_v
                else:
                    # Too low, search higher
                    low_v = mid_v

            # Log and return - mark as threshold (purple)
            log_test(x, y, threshold_voltage, 0, 'threshold', hit_rate=threshold_rate, optimized_voltage=threshold_voltage)
            special = 'glitch' if glitch_found else 'threshold'
            return threshold_rate, threshold_voltage, True, special

        # Normal case: found good voltage around 50%
        if hit_rate <= 0.55:
            log_test(x, y, new_voltage, 0, 'optimized', hit_rate=hit_rate, optimized_voltage=new_voltage)
            special = 'glitch' if glitch_found else None
            return hit_rate, new_voltage, True, special

        prev_voltage = new_voltage
        prev_hit_rate = hit_rate

        if new_voltage <= CS_VOLTAGE_MIN:
            break

    # Reached minimum voltage
    log_test(x, y, current_voltage, 0, 'optimized', hit_rate=prev_hit_rate, optimized_voltage=current_voltage)
    special = 'glitch' if glitch_found else ('threshold' if prev_hit_rate > 0 else None)
    return prev_hit_rate, current_voltage, True, special

# Initialize Grbl - move to home position (assumes home was set manually)
print("=== Initializing Grbl ===", flush=True)
send_cmd('grbl unlock')
time.sleep(0.5)
print("Moving to home (0,0)...", flush=True)
send_cmd('grbl home')  # Move to 0,0
time.sleep(0.5)

# Initialize ChipSHOUTER
def send_cs_cmd(cmd, wait_time=0.5):
    """Send ChipSHOUTER command and wait for response"""
    s.write((cmd + '\r\n').encode())
    time.sleep(0.1)
    response = ""
    start = time.time()
    while time.time() - start < 3.0:
        if s.in_waiting:
            chunk = s.read(s.in_waiting).decode('utf-8', errors='ignore')
            response += chunk
            if "#" in chunk or "> " in chunk:
                break
        time.sleep(0.1)
    return response

def setup_chipshouter():
    """Configure ChipSHOUTER for glitching"""
    global current_voltage
    print(f"=== Configuring ChipSHOUTER ===", flush=True)
    print(f"Voltage: {CS_VOLTAGE}V, Pulse: {CS_PULSE_WIDTH}us", flush=True)

    # Reset ChipSHOUTER
    print("  Resetting...", flush=True)
    send_cs_cmd("CS RESET")
    time.sleep(1.0)

    # Set voltage
    print(f"  Setting voltage to {CS_VOLTAGE}V...", flush=True)
    send_cs_cmd(f"CS VOLTAGE {CS_VOLTAGE}")
    current_voltage = CS_VOLTAGE

    # Set pulse width
    print(f"  Setting pulse width to {CS_PULSE_WIDTH}us...", flush=True)
    send_cs_cmd(f"CS PULSE {CS_PULSE_WIDTH}")

    # Set hardware trigger
    print("  Setting trigger to HW HIGH...", flush=True)
    send_cs_cmd("CS TRIGGER HW HIGH")

    # Arm ChipSHOUTER
    print("  Arming...", flush=True)
    send_cs_cmd("CS ARM")

    # Wait for armed state
    time.sleep(1.0)
    status = send_cs_cmd("CS STATUS")
    if "armed" in status.lower() and "disarmed" not in status.lower():
        print("  ChipSHOUTER armed and ready", flush=True)
        return True
    else:
        print("  WARNING: ChipSHOUTER may not be armed", flush=True)
        return False

setup_chipshouter()

mode_str = "REVERSE" if REVERSE_SPIRAL else "FORWARD"
print(f"\n=== Starting Glitch Heat Map ({mode_str}) ===", flush=True)
print(f"Grid: {SIZE+1}x{SIZE+1} = {(SIZE+1)**2} positions", flush=True)
print(f"Shots per position: {SHOTS_PER_POSITION}", flush=True)
print(f"Starting voltage: {CS_VOLTAGE}V (min: {CS_VOLTAGE_MIN}V)", flush=True)
print(f"CSV log: {CSV_FILE}", flush=True)
print("View heat map at http://localhost:8080", flush=True)

# Generate forward spiral points first, then reverse
def generate_spiral_points(size):
    """Generate all points in forward spiral order (0,0) -> center"""
    points = []
    offset = 0
    while offset <= size // 2:
        min_c = offset
        max_c = size - offset

        if min_c > max_c:
            break

        if min_c == max_c:
            points.append((min_c, min_c))
            break

        # Start at corner
        points.append((min_c, min_c))

        # Bottom edge: left to right
        for x in range(min_c + 1, max_c + 1):
            points.append((x, min_c))

        # Right edge: bottom to top
        for y in range(min_c + 1, max_c + 1):
            points.append((max_c, y))

        # Top edge: right to left
        for x in range(max_c - 1, min_c - 1, -1):
            points.append((x, max_c))

        # Left edge: top to bottom (stop before corner)
        for y in range(max_c - 1, min_c, -1):
            points.append((min_c, y))

        offset += 1

    return points

# Generate spiral points
spiral_points = generate_spiral_points(SIZE)

if REVERSE_SPIRAL:
    spiral_points.reverse()
    direction_str = f"REVERSE: center ({spiral_points[0]}) -> (0,0)"
else:
    direction_str = f"FORWARD: (0,0) -> center ({spiral_points[-1]})"

print(f"Spiral: {len(spiral_points)} points, {direction_str}", flush=True)

# Process each point
for i, (x, y) in enumerate(spiral_points):
    print(f"\n[{i+1}/{len(spiral_points)}] Position ({x},{y}):", end='', flush=True)

    # Reset voltage to starting level before each new position
    if current_voltage != CS_VOLTAGE:
        print(f" [reset to {CS_VOLTAGE}V]", end='', flush=True)
        set_chipshouter_voltage(CS_VOLTAGE)
        send_cmd("CS ARM")
        time.sleep(0.3)

    grbl_move(x, y)
    broadcast_move(x, y, current_voltage)
    print(f" ", end='', flush=True)
    hit_rate, _, glitch_found = test_position(x, y, current_voltage)
    print(f" {hit_rate*100:.0f}%", flush=True)

    # If glitch found, mark blue and move on immediately
    if glitch_found:
        print(f"    GLITCH FOUND! Marking as blue.", flush=True)
        broadcast_result(x, y, hit_rate, current_voltage, special='glitch')
        continue

    start_v = current_voltage
    # Optimize voltage if ANY hits were detected (not just >50%)
    if hit_rate > 0:
        hit_rate, opt_voltage, was_optimized, special = optimize_voltage(x, y, hit_rate, glitch_found)
        broadcast_result(x, y, hit_rate, opt_voltage, was_optimized, start_v, special=special)
    else:
        broadcast_result(x, y, hit_rate, current_voltage)

print("\n=== Heat Map Complete! ===", flush=True)
print(f"Total positions tested: {len(visited)}", flush=True)
print(f"CSV log saved to: {CSV_FILE}", flush=True)
broadcast_complete()

# Keep server running
print("Web server will stay active. Press Ctrl+C to exit.", flush=True)
try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    pass

s.close()
