"""Check the complete page layout of the production installer."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
runtime = (root/'src/TargetTraceRuntime.h').read_text()
encoder = (root/'src/TargetTrace.h').read_text()

def size(name):
    return int(re.search(r'array<uint8_t,(\d+)> '+name+r'\(', encoder)[1])

slots = [(int(at), size('Relay') + (3 if name == 'kResetCall' else 0), callback)
         for name, callback, at in re.findall(r'HookAt\((\w+),\d+,\w+,(\w+),(\d+)\)', runtime)]
assert len(slots)==12
slots.append((int(re.search(r'decisionRelay=(\d+);',runtime)[1]),
              size('ResetDecisionRelay'),'decision'))
obstacle = (root/'src/ObstacleObserver.h').read_text()
slots.append((int(re.search(r'kObstacleRelay=(\d+);',obstacle)[1]),size('Relay'),'obstacle'))
gate = int(re.search(r'kObstacleGateRelay=(\d+);',obstacle)[1])
slots.append((gate,int(re.search(r'array<uint8_t,(\d+)> ObstacleGateRelay',obstacle)[1]),'obstacle-gate'))
slots = sorted(set(slots))  # Outer update calls share the identical thunk.
assert slots[-1][0]+slots[-1][1]<=4096
for (offset, count, name), (next_offset, _, incoming) in zip(slots,slots[1:]):
    assert offset + count <= next_offset, (name, incoming, offset + count - next_offset)
print(f'{len(slots)} relay intervals verified without overlap')
