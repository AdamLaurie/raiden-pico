#!/usr/bin/env python3
"""
Inward spiral fill pattern - covers every 1mm point within a 30x30mm square.
Starts at outer edge, completes the box perimeter, then moves 1mm inward and repeats
until reaching the center.

Includes a local web server at http://localhost:8080 showing real-time progress.
"""
import serial
import time
import threading
import json
from http.server import HTTPServer, BaseHTTPRequestHandler
import queue

# Grid size
SIZE = 30

# Shared state for web visualization
visited = set()
current_pos = (0, 0)
update_queue = queue.Queue()
server_ready = threading.Event()

HTML_PAGE = '''<!DOCTYPE html>
<html>
<head>
    <title>Spiral Test Progress</title>
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
        canvas {
            border: 2px solid #00ff88;
            background: #0f0f23;
        }
        #info {
            margin-top: 10px;
            color: #888;
        }
    </style>
</head>
<body>
    <h1>Spiral Test Progress</h1>
    <div id="status">Connecting...</div>
    <canvas id="grid" width="620" height="620"></canvas>
    <div id="info">Each pixel = 1mm. Grid: 31x31 (0-30)</div>

    <script>
        const canvas = document.getElementById('grid');
        const ctx = canvas.getContext('2d');
        const status = document.getElementById('status');
        const CELL_SIZE = 20;
        const GRID_SIZE = 31;
        let prevPos = null;  // Track previous position for clearing red outline

        // Draw initial grid
        function drawGrid() {
            ctx.fillStyle = '#0f0f23';
            ctx.fillRect(0, 0, canvas.width, canvas.height);

            // Draw grid lines
            ctx.strokeStyle = '#333';
            ctx.lineWidth = 1;
            for (let i = 0; i <= GRID_SIZE; i++) {
                ctx.beginPath();
                ctx.moveTo(i * CELL_SIZE, 0);
                ctx.lineTo(i * CELL_SIZE, canvas.height);
                ctx.stroke();
                ctx.beginPath();
                ctx.moveTo(0, i * CELL_SIZE);
                ctx.lineTo(canvas.width, i * CELL_SIZE);
                ctx.stroke();
            }
        }

        function drawCell(x, y, color) {
            // Y=0 at top, Y=30 at bottom (matches physical platform view)
            const canvasY = y * CELL_SIZE;
            const canvasX = x * CELL_SIZE;
            ctx.fillStyle = color;
            ctx.fillRect(canvasX + 1, canvasY + 1, CELL_SIZE - 2, CELL_SIZE - 2);
        }

        function drawCurrent(x, y) {
            const canvasY = y * CELL_SIZE;
            const canvasX = x * CELL_SIZE;
            ctx.strokeStyle = '#ff0000';
            ctx.lineWidth = 3;
            ctx.strokeRect(canvasX + 2, canvasY + 2, CELL_SIZE - 4, CELL_SIZE - 4);
        }

        drawGrid();

        // Connect to SSE endpoint
        const evtSource = new EventSource('/events');

        evtSource.onopen = function() {
            status.textContent = 'Connected - waiting for moves...';
        };

        evtSource.onmessage = function(event) {
            const data = JSON.parse(event.data);

            if (data.type === 'init') {
                // Redraw all visited cells
                drawGrid();
                data.visited.forEach(([x, y]) => {
                    drawCell(x, y, '#00ff88');
                });
                if (data.current) {
                    drawCurrent(data.current[0], data.current[1]);
                    prevPos = data.current;
                }
                status.textContent = `Visited: ${data.visited.length} / 961 points`;
            }
            else if (data.type === 'move') {
                // Clear red outline from previous position by redrawing as green
                if (prevPos) {
                    drawCell(prevPos[0], prevPos[1], '#00ff88');
                }
                // Draw current cell green and add red outline
                drawCell(data.x, data.y, '#00ff88');
                drawCurrent(data.x, data.y);
                prevPos = [data.x, data.y];
                status.textContent = `Position: (${data.x}, ${data.y}) - Visited: ${data.count} / 961`;
            }
            else if (data.type === 'complete') {
                status.textContent = `Complete! Visited ${data.count} points.`;
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
        pass  # Suppress HTTP logging

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
                'visited': list(visited),
                'current': current_pos
            }
            self.wfile.write(f"data: {json.dumps(init_data)}\n\n".encode())
            self.wfile.flush()

            # Stream updates
            try:
                while True:
                    try:
                        update = update_queue.get(timeout=1.0)
                        self.wfile.write(f"data: {json.dumps(update)}\n\n".encode())
                        self.wfile.flush()
                    except queue.Empty:
                        # Send keepalive
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

def broadcast_move(x, y):
    global current_pos
    visited.add((x, y))
    current_pos = (x, y)
    update = {
        'type': 'move',
        'x': x,
        'y': y,
        'count': len(visited)
    }
    # Clear queue and add new update (only keep latest for each client reconnect)
    update_queue.put(update)

def broadcast_complete():
    update = {
        'type': 'complete',
        'count': len(visited)
    }
    update_queue.put(update)

# Start web server in background
server_thread = threading.Thread(target=run_server, daemon=True)
server_thread.start()
server_ready.wait()

# Serial connection
s = serial.Serial('/dev/ttyACM0', 115200, timeout=35)
time.sleep(1)

def send_cmd(cmd):
    s.write((cmd + '\r\n').encode())
    time.sleep(0.1)
    response = ""
    start = time.time()
    while time.time() - start < 35:
        if s.in_waiting:
            response += s.read(s.in_waiting).decode('utf-8', errors='ignore')
            if 'OK:' in response or 'ERROR:' in response or 'complete' in response.lower():
                break
        time.sleep(0.05)
    return response

def move_to(x, y):
    print(f"  ({x}, {y})", flush=True)
    resp = send_cmd(f'grbl move {x} {y}')
    if 'ERROR' in resp:
        print(f"    ERROR: {resp}", flush=True)
        return False
    broadcast_move(x, y)
    time.sleep(1)  # 1 second pause after each move
    return True

# Soft reset Grbl first to clear any stuck alarm state
print("=== Soft reset Grbl ===", flush=True)
resp = send_cmd('grbl reset')
print(f"Reset response: {resp}", flush=True)
time.sleep(2)  # Wait for Grbl to reboot

# Unlock in case we're in alarm state
print("=== Unlocking Grbl ===", flush=True)
resp = send_cmd('grbl unlock')
print(f"Unlock response: {resp}", flush=True)
time.sleep(0.5)

# Autohome to ensure we have travel space
print("=== Auto-homing ===", flush=True)
resp = send_cmd('grbl autohome')
print(f"Autohome response: {resp}", flush=True)
if 'ERROR' in resp:
    print("Homing failed, trying unlock again...", flush=True)
    send_cmd('grbl unlock')
    time.sleep(0.5)

# Set current position as origin after homing
print("=== Setting home position ===", flush=True)
resp = send_cmd('grbl set home')
print(f"Set origin response: {resp}", flush=True)
time.sleep(0.5)

print("\n=== Starting inward spiral fill ===", flush=True)
print("Covering every 1mm point in 30x30mm square", flush=True)
print("View progress at http://localhost:8080", flush=True)

size = SIZE
offset = 0

while offset <= size // 2:
    min_c = offset
    max_c = size - offset

    # Skip if we've reached the center
    if min_c > max_c:
        break

    # Single point at center
    if min_c == max_c:
        print(f"\n--- Center point ({min_c}, {min_c}) ---", flush=True)
        move_to(min_c, min_c)
        break

    print(f"\n--- Ring {offset}: ({min_c},{min_c}) to ({max_c},{max_c}) ---", flush=True)

    # Start at bottom-left corner of this ring
    move_to(min_c, min_c)

    # Bottom edge: left to right
    print(f"  Bottom edge (y={min_c}):", flush=True)
    for x in range(min_c + 1, max_c + 1):
        if not move_to(x, min_c):
            break

    # Right edge: bottom to top
    print(f"  Right edge (x={max_c}):", flush=True)
    for y in range(min_c + 1, max_c + 1):
        if not move_to(max_c, y):
            break

    # Top edge: right to left
    print(f"  Top edge (y={max_c}):", flush=True)
    for x in range(max_c - 1, min_c - 1, -1):
        if not move_to(x, max_c):
            break

    # Left edge: top to bottom (but not back to start - that's the next ring's start)
    print(f"  Left edge (x={min_c}):", flush=True)
    for y in range(max_c - 1, min_c, -1):  # Stop at min_c+1, next ring starts at min_c+1
        if not move_to(min_c, y):
            break

    offset += 1

print("\n=== Pattern complete! ===", flush=True)
print(f"Total points covered: {len(visited)}", flush=True)
broadcast_complete()

# Keep server running for a bit so user can see final state
print("Web server will stay active for 30 seconds...", flush=True)
time.sleep(30)

s.close()
