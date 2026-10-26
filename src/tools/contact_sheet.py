"""contact_sheet.py <out.png> <frame.png>...: the frames in a grid (4 per row, 320x180 each), labelled by name."""
import os
import subprocess
import sys

out, frames = sys.argv[1], sys.argv[2:]
ff = os.environ.get('FFMPEG', 'ffmpeg')
args, filters = [], []
for i, f in enumerate(frames):
    args += ['-i', f]
    label = os.path.splitext(os.path.basename(f))[0].replace('frame_', '')
    filters.append(f"[{i}:v]scale=320:180,drawtext=text='{label}':x=4:y=4:fontsize=18:fontcolor=yellow:box=1:boxcolor=black[v{i}]")
cols = 4
rows = (len(frames) + cols - 1) // cols
while len(filters) < rows * cols:  # pad the last row
    i = len(filters)
    filters.append(f"color=black:size=320x180:d=1[v{i}]")
layout = '|'.join(f"{(i % cols) * 320}_{(i // cols) * 180}" for i in range(rows * cols))
filters.append(''.join(f'[v{i}]' for i in range(rows * cols)) + f"xstack=inputs={rows * cols}:layout={layout}[out]")
subprocess.run([ff, '-v', 'error', '-y', *args, '-filter_complex', ';'.join(filters), '-map', '[out]', '-frames:v', '1', out],
               check=True)
print(out)
