#!/usr/bin/env python3
"""
JTAG Glitch Heat Map - Spiral scan with glitch testing at each position.
Uses TARGET RESET with GPIO trigger to attempt JTAG bypass glitch.

Hit criteria:
- JTAG dump succeeds = hit (protection bypassed)
- JTAG dump fails = miss (protection active)

Trigger: GPIO rising edge
Pause: 0 (immediate)
Success test: JTAG flash dump via J-Link

Web server at http://localhost:8080 shows real-time heat map.

Usage:
  python3 jtag_glitch_heatmap.py           # Forward spiral (0,0) -> center
  python3 jtag_glitch_heatmap.py --reverse # Reverse spiral center -> (0,0)
"""
import serial
import time
import threading
import json
import csv
import os
import subprocess
import argparse
import random
from datetime import datetime
from http.server import HTTPServer, BaseHTTPRequestHandler
import queue

# Parse command line arguments
parser = argparse.ArgumentParser(description='JTAG Glitch Heat Map Scanner')
parser.add_argument('--reverse', '-r', action='store_true',
                    help='Reverse spiral: start from center, end at (0,0)')
parser.add_argument('--forward', '-f', action='store_true',
                    help='Forward spiral: start from (0,0), end at center (default)')
parser.add_argument('--limit', '-l', type=int, default=0,
                    help='Limit test area to NxN grid around start point (0=full grid)')
parser.add_argument('--random-pause', '-p', type=int, default=0,
                    help='Randomize pause up to N microseconds (0=no pause)')
parser.add_argument('--pause-min', type=int, default=0,
                    help='Minimum pause value when using --random-pause')
parser.add_argument('--repeat', type=int, default=1,
                    help='Number of times to repeat full scan (0=forever)')
parser.add_argument('--stop-on-success', '-s', action='store_true',
                    help='Stop entire scan when first successful glitch is detected')
parser.add_argument('--start-x', type=int, default=None,
                    help='Starting X position (default: center if reverse, 0 if forward)')
parser.add_argument('--start-y', type=int, default=None,
                    help='Starting Y position (default: center if reverse, 0 if forward)')
parser.add_argument('--single', action='store_true',
                    help='Test only at the start position (no spiral)')
args = parser.parse_args()

# Determine direction (default is forward)
REVERSE_SPIRAL = args.reverse and not args.forward
POSITION_LIMIT = args.limit  # 0 = no limit, otherwise NxN around start
RANDOM_PAUSE_MAX = args.random_pause  # 0 = no random pause
RANDOM_PAUSE_MIN = args.pause_min  # Minimum pause value
REPEAT_COUNT = args.repeat  # 1 = single run, 0 = forever
STOP_ON_SUCCESS = args.stop_on_success  # Exit on first hit
SINGLE_POSITION = args.single  # Test only at start position
START_X = args.start_x  # Starting X position (None = default)
START_Y = args.start_y  # Starting Y position (None = default)

# Grid size
SIZE = 23  # 24x24 grid (0-23)

# ChipSHOUTER parameters
CS_VOLTAGE_MAX = 225  # Maximum voltage in V
CS_VOLTAGE_MIN = 180  # Minimum voltage
CS_VOLTAGE_COARSE_STEP = 15  # Coarse voltage step (down sweep)
CS_VOLTAGE_FINE_STEP = 5  # Fine voltage step (up sweep)
CS_PULSE_WIDTH = 50  # Pulse width in us
ATTEMPTS_PER_VOLTAGE = 5  # Number of glitch attempts at each voltage

# JTAG dump script path
JTAG_DUMP_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "scripts/dump_lpc2468_flash.sh")
JTAG_DUMP_OUTPUT = "/tmp/jtag_glitch_dump.bin"

# Current voltage
current_voltage = CS_VOLTAGE_MAX

# Shared state for web visualization
visited = {}  # (x, y) -> {'hit_rate': 0.0-1.0, 'voltage': V}
current_pos = (0, 0)
update_queue = queue.Queue()
server_ready = threading.Event()

# CSV log file (will be set per iteration)
CSV_FILE = None
csv_lock = threading.Lock()

def init_csv(filename):
    """Initialize CSV log file with headers"""
    global CSV_FILE
    CSV_FILE = filename
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
    <title>JTAG Glitch Heat Map</title>
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
    <h1>JTAG Glitch Heat Map</h1>
    <div id="status">Connecting...</div>
    <div id="container">
        <canvas id="grid" width="360" height="360"></canvas>
        <div id="tooltip"></div>
    </div>
    <div id="legend">
        <span>0% (protected)</span>
        <div id="gradient"></div>
        <span>100% (bypass)</span>
        <span style="margin-left: 20px; color: #0088ff;">&#9632; BYPASS</span>
        <span style="margin-left: 10px; color: #aa00ff;">&#9632; Threshold</span>
    </div>
    <div id="info">24x24mm grid (15px/cell). JTAG bypass glitch test. Hover for details.</div>

    <script>
        const canvas = document.getElementById('grid');
        const ctx = canvas.getContext('2d');
        const status = document.getElementById('status');
        const tooltip = document.getElementById('tooltip');
        const CELL_SIZE = 15;
        const GRID_SIZE = 24;
        let prevPos = null;

        let cellData = {};

        function drawGrid() {
            ctx.fillStyle = '#0f0f23';
            ctx.fillRect(0, 0, canvas.width, canvas.height);
        }

        function hitRateToColor(rate, special) {
            if (special === 'glitch') {
                return '#0088ff';
            }
            if (special === 'threshold') {
                return '#aa00ff';
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
                        specialText = '<br><span style="color:#0088ff">BYPASS!</span>';
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
                let specialMsg = data.special === 'glitch' ? ' [BYPASS!]' : (data.special === 'threshold' ? ' [threshold]' : '');
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

def save_html_snapshot(filename):
    """Save current heatmap state as an HTML file"""
    # Build visited data for embedding in HTML
    visited_json = json.dumps({f"{x},{y}": info for (x, y), info in visited.items()})

    # Create standalone HTML with embedded data
    snapshot_html = HTML_PAGE.replace(
        "const evtSource = new EventSource('/events');",
        f"// Static snapshot - no live updates\n        const staticData = {visited_json};"
    ).replace(
        """evtSource.onopen = function() {
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
                let specialMsg = data.special === 'glitch' ? ' [BYPASS!]' : (data.special === 'threshold' ? ' [threshold]' : '');
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
        };""",
        f"""// Load static data
        Object.entries(staticData).forEach(([key, info]) => {{
            const [x, y] = key.split(',').map(Number);
            cellData[key] = info;
            drawCell(x, y, info.hitRate, info.special);
        }});
        status.textContent = 'Snapshot: ' + Object.keys(staticData).length + ' / 576 positions tested';
        status.style.color = '#00ff88';"""
    )

    with open(filename, 'w') as f:
        f.write(snapshot_html)
    print(f"HTML snapshot saved to: {filename}", flush=True)

# Start web server in background
server_thread = threading.Thread(target=run_server, daemon=True)
server_thread.start()
server_ready.wait()

# Serial connection to Raiden
s = serial.Serial('/dev/ttyACM0', 115200, timeout=5)
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

    if "error" in response.lower() or "fault" in response.lower() or "disarmed" in response.lower():
        print("\n  [CS ERROR - resetting]", end='', flush=True)
        send_cmd("CS RESET")
        time.sleep(1.0)
        send_cmd(f"CS VOLTAGE {current_voltage}")
        send_cmd(f"CS PULSE {CS_PULSE_WIDTH}")
        send_cmd("CS TRIGGER HW HIGH")
        send_cmd("CS ARM")
        time.sleep(0.5)
        return False
    return True

def test_jtag_dump():
    """
    Try to dump flash via JTAG.
    Returns True if dump succeeded (protection bypassed), False otherwise.
    """
    try:
        result = subprocess.run(
            [JTAG_DUMP_SCRIPT, JTAG_DUMP_OUTPUT],
            capture_output=True,
            text=True,
            timeout=60
        )
        return result.returncode == 0
    except subprocess.TimeoutExpired:
        return False
    except Exception as e:
        print(f"  [JTAG error: {e}]", end='', flush=True)
        return False

def run_glitch_test():
    """
    Run a single JTAG glitch test.
    - Reset target with GPIO trigger glitch
    - Check if JTAG dump succeeds
    Returns: 'hit' (JTAG worked), 'miss' (JTAG failed), or 'cs_error'
    """
    # Check ChipSHOUTER status first
    if not check_and_reset_chipshouter():
        return 'cs_error'

    # Setup GPIO trigger (rising edge)
    send_cmd("TRIGGER GPIO RISING")

    # Set pause - random if configured, otherwise 0
    if RANDOM_PAUSE_MAX > 0:
        pause_value = random.randint(RANDOM_PAUSE_MIN, RANDOM_PAUSE_MAX)
        send_cmd(f"SET PAUSE {pause_value}")
    else:
        send_cmd("SET PAUSE 0")

    send_cmd("SET WIDTH 150")
    send_cmd("SET COUNT 1")

    # Arm the glitch system
    send_cmd("ARM ON")

    # Reset the target - this triggers the glitch on reset rising edge
    send_cmd("TARGET RESET")

    # Small delay for target to boot
    time.sleep(0.1)

    # Test if JTAG works now
    if test_jtag_dump():
        return 'hit'
    else:
        return 'miss'

def run_single_glitch_attempt(x, y, voltage, shot_num):
    """
    Run a single glitch test and log it.
    Returns: 'hit', 'miss', or 'cs_error'
    """
    result = run_glitch_test()
    log_test(x, y, voltage, shot_num, result)
    return result

def voltage_sweep_position(x, y):
    """
    Full voltage sweep at current position.
    Coarse sweep: 500V -> 150V in 50V steps (5 attempts each)
    Fine sweep: 150V -> 500V in 10V steps (5 attempts each)
    Records any hits found and returns results.
    Returns: (best_voltage, total_hits, total_tests, hit_rate)
    """
    global current_voltage

    hits_at_voltage = {}  # voltage -> hit_count
    total_tests = 0
    total_hits = 0
    best_voltage = None

    # Coarse sweep: 500V down to 150V in 50V steps
    print(f"\n    Coarse sweep (500V->150V, 50V steps, {ATTEMPTS_PER_VOLTAGE}x each): ", end='', flush=True)
    for voltage in range(CS_VOLTAGE_MAX, CS_VOLTAGE_MIN - 1, -CS_VOLTAGE_COARSE_STEP):
        set_chipshouter_voltage(voltage)

        voltage_hits = 0
        for attempt in range(ATTEMPTS_PER_VOLTAGE):
            result = run_single_glitch_attempt(x, y, voltage, total_tests + 1)
            total_tests += 1

            if result == 'hit':
                total_hits += 1
                voltage_hits += 1
                print("+", end='', flush=True)
                if STOP_ON_SUCCESS:
                    print(f" [HIT @ {voltage}V - stopping]", flush=True)
                    return voltage, 1, total_tests, 1.0
            elif result == 'cs_error':
                print("R", end='', flush=True)
                time.sleep(0.5)
            else:
                print(".", end='', flush=True)

            time.sleep(0.1)

        if voltage_hits > 0:
            hits_at_voltage[voltage] = voltage_hits
            if best_voltage is None:
                best_voltage = voltage
            print(f"[{voltage}V:{voltage_hits}]", end='', flush=True)

    # Fine sweep: 150V up to 500V in 10V steps
    print(f"\n    Fine sweep (150V->500V, 10V steps, {ATTEMPTS_PER_VOLTAGE}x each): ", end='', flush=True)
    for voltage in range(CS_VOLTAGE_MIN, CS_VOLTAGE_MAX + 1, CS_VOLTAGE_FINE_STEP):
        set_chipshouter_voltage(voltage)

        voltage_hits = 0
        for attempt in range(ATTEMPTS_PER_VOLTAGE):
            result = run_single_glitch_attempt(x, y, voltage, total_tests + 1)
            total_tests += 1

            if result == 'hit':
                total_hits += 1
                voltage_hits += 1
                print("+", end='', flush=True)
                if STOP_ON_SUCCESS:
                    print(f" [HIT @ {voltage}V - stopping]", flush=True)
                    return voltage, 1, total_tests, 1.0
            elif result == 'cs_error':
                print("R", end='', flush=True)
                time.sleep(0.5)
            else:
                print(".", end='', flush=True)

            time.sleep(0.1)

        if voltage_hits > 0:
            hits_at_voltage[voltage] = hits_at_voltage.get(voltage, 0) + voltage_hits
            if best_voltage is None:
                best_voltage = voltage
            print(f"[{voltage}V:{voltage_hits}]", end='', flush=True)

    hit_rate = total_hits / max(total_tests, 1)

    # Find best voltage (most hits)
    if hits_at_voltage:
        best_voltage = max(hits_at_voltage.keys(), key=lambda v: hits_at_voltage[v])

    print(f"\n    Result: {total_hits}/{total_tests} hits", end='', flush=True)
    if best_voltage:
        print(f" (best: {best_voltage}V with {hits_at_voltage[best_voltage]} hits)", flush=True)
    else:
        print("", flush=True)

    return best_voltage, total_hits, total_tests, hit_rate


# Initialize Grbl
print("=== Initializing Grbl ===", flush=True)
send_cmd('grbl unlock')
time.sleep(0.5)
print("Moving to home (0,0)...", flush=True)
send_cmd('grbl home')
time.sleep(0.5)

# Initialize ChipSHOUTER
def send_cs_cmd(cmd, wait_time=0.5):
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
    global current_voltage
    print(f"=== Configuring ChipSHOUTER ===", flush=True)
    print(f"Voltage: {CS_VOLTAGE_MAX}V, Pulse: {CS_PULSE_WIDTH}us", flush=True)

    print("  Resetting...", flush=True)
    send_cs_cmd("CS RESET")
    time.sleep(1.0)

    print(f"  Setting voltage to {CS_VOLTAGE_MAX}V...", flush=True)
    send_cs_cmd(f"CS VOLTAGE {CS_VOLTAGE_MAX}")
    current_voltage = CS_VOLTAGE_MAX

    print(f"  Setting pulse width to {CS_PULSE_WIDTH}us...", flush=True)
    send_cs_cmd(f"CS PULSE {CS_PULSE_WIDTH}")

    print("  Setting trigger to HW HIGH...", flush=True)
    send_cs_cmd("CS TRIGGER HW HIGH")

    print("  Arming...", flush=True)
    send_cs_cmd("CS ARM")

    time.sleep(1.0)
    status = send_cs_cmd("CS STATUS")
    if "armed" in status.lower() and "disarmed" not in status.lower():
        print("  ChipSHOUTER armed and ready", flush=True)
        return True
    else:
        print("  WARNING: ChipSHOUTER may not be armed", flush=True)
        return False

setup_chipshouter()

# Verify JTAG dump script exists
if not os.path.exists(JTAG_DUMP_SCRIPT):
    print(f"ERROR: JTAG dump script not found: {JTAG_DUMP_SCRIPT}", flush=True)
    exit(1)

mode_str = "REVERSE" if REVERSE_SPIRAL else "FORWARD"
print(f"\n=== Starting JTAG Glitch Heat Map ({mode_str}) ===", flush=True)
print(f"Grid: {SIZE+1}x{SIZE+1} = {(SIZE+1)**2} positions", flush=True)
print(f"Voltage sweep: {CS_VOLTAGE_MAX}V -> {CS_VOLTAGE_MIN}V (coarse {CS_VOLTAGE_COARSE_STEP}V), then {CS_VOLTAGE_MIN}V -> {CS_VOLTAGE_MAX}V (fine {CS_VOLTAGE_FINE_STEP}V)", flush=True)
print(f"CSV log: {CSV_FILE}", flush=True)
print(f"JTAG dump script: {JTAG_DUMP_SCRIPT}", flush=True)
print("View heat map at http://localhost:8080", flush=True)
if STOP_ON_SUCCESS:
    print("Stop-on-success: ENABLED", flush=True)

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

        points.append((min_c, min_c))

        for x in range(min_c + 1, max_c + 1):
            points.append((x, min_c))

        for y in range(min_c + 1, max_c + 1):
            points.append((max_c, y))

        for x in range(max_c - 1, min_c - 1, -1):
            points.append((x, max_c))

        for y in range(max_c - 1, min_c, -1):
            points.append((min_c, y))

        offset += 1

    return points

# Handle single position mode
if SINGLE_POSITION:
    if START_X is not None and START_Y is not None:
        spiral_points = [(START_X, START_Y)]
        direction_str = f"SINGLE: position ({START_X}, {START_Y}) only"
    else:
        print("ERROR: --single requires --start-x and --start-y", flush=True)
        exit(1)
else:
    spiral_points = generate_spiral_points(SIZE)

    if REVERSE_SPIRAL:
        spiral_points.reverse()
        direction_str = f"REVERSE: center ({spiral_points[0]}) -> (0,0)"
    else:
        direction_str = f"FORWARD: (0,0) -> center ({spiral_points[-1]})"

    # Override start point if specified
    if START_X is not None and START_Y is not None:
        # Find the specified point in the spiral and reorder so it's first
        target = (START_X, START_Y)
        if target in spiral_points:
            idx = spiral_points.index(target)
            spiral_points = spiral_points[idx:] + spiral_points[:idx]
            direction_str = f"CUSTOM START: ({START_X}, {START_Y})"
        else:
            print(f"WARNING: Start position ({START_X}, {START_Y}) not in grid, using default", flush=True)

    # Apply position limit if specified
    if POSITION_LIMIT > 0 and len(spiral_points) > 0:
        start_point = spiral_points[0]
        half_limit = POSITION_LIMIT // 2
        # Filter to NxN grid around start point
        filtered_points = []
        for (x, y) in spiral_points:
            dx = abs(x - start_point[0])
            dy = abs(y - start_point[1])
            if dx <= half_limit and dy <= half_limit:
                filtered_points.append((x, y))
        original_count = len(spiral_points)
        spiral_points = filtered_points
        print(f"Position limit: {POSITION_LIMIT}x{POSITION_LIMIT} around {start_point} ({len(spiral_points)} of {original_count} points)", flush=True)

print(f"Spiral: {len(spiral_points)} points, {direction_str}", flush=True)

# Main repeat loop
iteration = 0
while True:
    iteration += 1

    # Check if we should stop (REPEAT_COUNT > 0 means finite repeats)
    if REPEAT_COUNT > 0 and iteration > REPEAT_COUNT:
        break

    # Generate timestamp for this iteration's files
    iter_timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    iter_csv_file = f"jtag_glitch_log_{iter_timestamp}.csv"
    iter_html_file = f"jtag_glitch_log_{iter_timestamp}.html"

    # Initialize new CSV for this iteration
    init_csv(iter_csv_file)

    # Clear visited data for fresh heatmap
    visited.clear()

    if REPEAT_COUNT == 0:
        print(f"\n=== Iteration {iteration} (infinite mode) ===", flush=True)
    else:
        print(f"\n=== Iteration {iteration}/{REPEAT_COUNT} ===", flush=True)
    print(f"CSV log: {iter_csv_file}", flush=True)

    # Process each point
    success_found = False
    for i, (x, y) in enumerate(spiral_points):
        print(f"\n[{i+1}/{len(spiral_points)}] Position ({x},{y}):", flush=True)

        # Move to position
        grbl_move(x, y)
        broadcast_move(x, y, CS_VOLTAGE_MAX)

        # Run full voltage sweep at this position
        best_voltage, total_hits, total_tests, hit_rate = voltage_sweep_position(x, y)

        # Determine special marker based on results
        special = 'glitch' if total_hits > 0 else None

        # Report results
        report_voltage = best_voltage if best_voltage else CS_VOLTAGE_MAX
        broadcast_result(x, y, hit_rate, report_voltage, special=special)

        # Check for stop-on-success
        if STOP_ON_SUCCESS and total_hits > 0:
            print(f"\n=== SUCCESS! Glitch found at ({x},{y}) @ {report_voltage}V ===", flush=True)
            success_found = True
            break

    if success_found:
        print(f"CSV log saved to: {iter_csv_file}", flush=True)
        save_html_snapshot(iter_html_file)
        broadcast_complete()
        break  # Exit repeat loop

    print(f"\n=== Iteration {iteration} Complete! ===", flush=True)
    print(f"Total positions tested: {len(visited)}", flush=True)
    print(f"CSV log saved to: {iter_csv_file}", flush=True)

    # Save HTML snapshot with matching filename
    save_html_snapshot(iter_html_file)

    broadcast_complete()

print("\n=== All iterations complete! ===", flush=True)

print("Web server will stay active. Press Ctrl+C to exit.", flush=True)
try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    pass

s.close()
