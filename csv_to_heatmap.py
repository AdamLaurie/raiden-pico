#!/usr/bin/env python3
"""
Convert glitch CSV log to standalone HTML heatmap.
Recreates the heatmap visualization from CSV data with working mouseover tooltips.
"""

import csv
import json
import sys
import os
from collections import defaultdict

HTML_TEMPLATE = '''<!DOCTYPE html>
<html>
<head>
    <title>Glitch Heat Map - {title}</title>
    <style>
        body {{
            background: #1a1a2e;
            color: #eee;
            font-family: monospace;
            display: flex;
            flex-direction: column;
            align-items: center;
            padding: 20px;
        }}
        h1 {{ color: #00ff88; }}
        #status {{
            margin: 10px 0;
            padding: 10px;
            background: #16213e;
            border-radius: 5px;
        }}
        #container {{
            position: relative;
            display: inline-block;
        }}
        canvas {{
            border: 2px solid #00ff88;
            background: #0f0f23;
        }}
        #tooltip {{
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
        }}
        #info {{
            margin-top: 10px;
            color: #888;
        }}
        #legend {{
            display: flex;
            align-items: center;
            margin-top: 10px;
            gap: 10px;
        }}
        #gradient {{
            width: 200px;
            height: 20px;
            background: linear-gradient(to right, #00ff00, #ffff00, #ff0000);
            border-radius: 3px;
        }}
    </style>
</head>
<body>
    <h1>Glitch Heat Map</h1>
    <div id="status">{status}</div>
    <div id="container">
        <canvas id="grid" width="{canvas_width}" height="{canvas_height}"></canvas>
        <div id="tooltip"></div>
    </div>
    <div id="legend">
        <span>0% (normal)</span>
        <div id="gradient"></div>
        <span>100% (effect)</span>
        <span style="margin-left: 20px; color: #0088ff;">&#9632; GLITCH (CRP bypass)</span>
    </div>
    <div id="info">{grid_size}x{grid_size}mm grid (15px/cell). Hover for details including voltage threshold. Generated from: {csv_file}</div>

    <script>
        const canvas = document.getElementById('grid');
        const ctx = canvas.getContext('2d');
        const tooltip = document.getElementById('tooltip');
        const CELL_SIZE = 15;
        const GRID_SIZE = {grid_size};

        // Embedded data from CSV
        const cellData = {cell_data_json};

        function drawGrid() {{
            ctx.fillStyle = '#0f0f23';
            ctx.fillRect(0, 0, canvas.width, canvas.height);
        }}

        function hitRateToColor(rate, special) {{
            if (special === 'glitch') {{
                return '#0088ff';
            }}
            if (special === 'threshold') {{
                return '#aa00ff';
            }}
            let r, g, b;
            if (rate <= 0.5) {{
                r = Math.round(255 * (rate * 2));
                g = 255;
                b = 0;
            }} else {{
                r = 255;
                g = Math.round(255 * (1 - (rate - 0.5) * 2));
                b = 0;
            }}
            return `rgb(${{r}}, ${{g}}, ${{b}})`;
        }}

        function drawCell(x, y, hitRate, special) {{
            const canvasY = y * CELL_SIZE;
            const canvasX = x * CELL_SIZE;
            ctx.fillStyle = hitRateToColor(hitRate, special);
            ctx.fillRect(canvasX + 1, canvasY + 1, CELL_SIZE - 2, CELL_SIZE - 2);
        }}

        // Draw grid and all cells
        drawGrid();
        Object.entries(cellData).forEach(([key, info]) => {{
            const [x, y] = key.split(',').map(Number);
            drawCell(x, y, info.hitRate, info.special);
        }});

        // Tooltip handling
        canvas.addEventListener('mousemove', function(e) {{
            var rect = canvas.getBoundingClientRect();
            var mouseX = e.clientX - rect.left;
            var mouseY = e.clientY - rect.top;

            var cellX = Math.floor(mouseX / CELL_SIZE);
            var cellY = Math.floor(mouseY / CELL_SIZE);

            if (cellX >= 0 && cellX < GRID_SIZE && cellY >= 0 && cellY < GRID_SIZE) {{
                var key = cellX + ',' + cellY;
                var cellInfo = cellData[key];

                var tooltipHtml;
                if (cellInfo) {{
                    var pct = (cellInfo.hitRate * 100).toFixed(1);
                    var specialText = '';
                    if (cellInfo.special === 'glitch') {{
                        specialText = '<br><span style="color:#0088ff">GLITCH!</span>';
                    }} else if (cellInfo.special === 'threshold') {{
                        specialText = '<br><span style="color:#aa00ff">Threshold</span>';
                    }}
                    var testsText = cellInfo.tests ? '<br>Tests: ' + cellInfo.tests : '';
                    var thresholdText = cellInfo.threshold ? '<br>Threshold: ' + cellInfo.threshold + 'V' : '';
                    tooltipHtml = '<b>(' + cellX + ', ' + cellY + ')</b><br>Hit: ' + pct + '%<br>Voltage: ' + cellInfo.voltage + 'V' + testsText + thresholdText + specialText;
                }} else {{
                    tooltipHtml = '<b>(' + cellX + ', ' + cellY + ')</b><br>Not tested';
                }}
                tooltip.innerHTML = tooltipHtml;
                tooltip.style.display = 'block';
                tooltip.style.left = (e.clientX + 15) + 'px';
                tooltip.style.top = (e.clientY + 15) + 'px';
            }} else {{
                tooltip.style.display = 'none';
            }}
        }});

        canvas.addEventListener('mouseleave', function() {{
            tooltip.style.display = 'none';
        }});
    </script>
</body>
</html>
'''

def parse_csv(csv_file):
    """Parse CSV and aggregate results by position."""
    positions = defaultdict(lambda: {
        'tests': 0,
        'hits': 0,  # effect or glitch
        'glitches': 0,  # actual glitches (code 0)
        'voltage': 0,
        'optimized_voltage': None,
        'has_threshold': False
    })

    with open(csv_file, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            x = int(row['x'])
            y = int(row['y'])
            key = (x, y)
            result = row['result']
            voltage = int(row['voltage'])

            # Track voltage (use the base voltage, not optimization voltages)
            if positions[key]['voltage'] == 0:
                positions[key]['voltage'] = voltage

            # Count tests (exclude optimization/threshold summary rows)
            if result not in ('threshold', 'optimized'):
                positions[key]['tests'] += 1

                # Count hits (effect, glitch, sync_fail count as hits)
                if result in ('effect', 'glitch', 'sync_fail'):
                    positions[key]['hits'] += 1

                # Track actual glitches
                if result == 'glitch':
                    positions[key]['glitches'] += 1

            # Track threshold/optimized results
            if result == 'threshold':
                positions[key]['has_threshold'] = True
                if row['optimized_voltage']:
                    positions[key]['optimized_voltage'] = int(row['optimized_voltage'])
                if row['hit_rate']:
                    positions[key]['threshold_rate'] = float(row['hit_rate'])

            if result == 'optimized' and row['optimized_voltage']:
                positions[key]['optimized_voltage'] = int(row['optimized_voltage'])

    return positions

def build_cell_data(positions):
    """Convert position data to cell data format for HTML."""
    cell_data = {}

    for (x, y), data in positions.items():
        if data['tests'] == 0:
            continue

        hit_rate = data['hits'] / data['tests']

        # Only mark as special if actual glitch (CRP bypass) was found
        # Threshold info is shown in mouseover instead of purple color
        special = None
        if data['glitches'] > 0:
            special = 'glitch'

        # Use optimized voltage if available, otherwise base voltage
        voltage = data['optimized_voltage'] if data['optimized_voltage'] else data['voltage']

        cell_data[f"{x},{y}"] = {
            'hitRate': hit_rate,
            'voltage': voltage,
            'special': special,
            'tests': data['tests'],
            'threshold': data['optimized_voltage'] if data['has_threshold'] else None
        }

    return cell_data

def generate_html(csv_file, output_file=None):
    """Generate standalone HTML heatmap from CSV."""
    positions = parse_csv(csv_file)
    cell_data = build_cell_data(positions)

    # Determine grid size from data
    max_x = max(x for x, y in positions.keys()) if positions else 0
    max_y = max(y for x, y in positions.keys()) if positions else 0
    grid_size = max(max_x, max_y) + 1

    # Canvas size (15px per cell)
    canvas_size = grid_size * 15

    # Generate HTML
    html = HTML_TEMPLATE.format(
        title=os.path.basename(csv_file),
        status=f"Loaded {len(cell_data)} positions from CSV",
        canvas_width=canvas_size,
        canvas_height=canvas_size,
        grid_size=grid_size,
        csv_file=os.path.basename(csv_file),
        cell_data_json=json.dumps(cell_data)
    )

    # Output file
    if output_file is None:
        output_file = csv_file.replace('.csv', '_heatmap.html')

    with open(output_file, 'w') as f:
        f.write(html)

    print(f"Generated: {output_file}")
    print(f"  Grid size: {grid_size}x{grid_size}")
    print(f"  Positions: {len(cell_data)}")

    # Summary stats
    hits = sum(1 for d in cell_data.values() if d['hitRate'] > 0)
    glitches = sum(1 for d in cell_data.values() if d['special'] == 'glitch')
    thresholds = sum(1 for d in cell_data.values() if d['special'] == 'threshold')
    print(f"  With hits: {hits}")
    print(f"  Glitches: {glitches}")
    print(f"  Thresholds: {thresholds}")

    return output_file

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 csv_to_heatmap.py <csv_file> [output_html]")
        print("Example: python3 csv_to_heatmap.py glitch_log_20251227.csv")
        sys.exit(1)

    csv_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else None

    if not os.path.exists(csv_file):
        print(f"Error: CSV file not found: {csv_file}")
        sys.exit(1)

    generate_html(csv_file, output_file)

if __name__ == "__main__":
    main()
