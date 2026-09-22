#!/usr/bin/env python3
"""Run one verified bandwidth pair on the two hulk nodes; save commands and logs.

Build the GNU C89 executables on each node first. This script only orchestrates
SSH processes; all payload transfer and timing happen inside the C executable.
Run one pair at a time. Existing result files are never overwritten.
"""
import argparse, datetime, json, pathlib, re, shlex, subprocess, time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--name', required=True)
p.add_argument('--kind', choices=['hbm', 'host'], required=True)
p.add_argument('--hulk0-dir', required=True, help='remote demo directory on hulk-0')
p.add_argument('--hulk1-dir', required=True, help='remote demo directory on hulk-1')
p.add_argument('--bytes', type=int, default=16777216)
p.add_argument('--batch', type=int, default=16)
p.add_argument('--iterations', type=int, default=1250)
p.add_argument('--warmup', type=int, default=20)
p.add_argument('--window', type=int, default=16)
p.add_argument('--mode', default='HOST')
p.add_argument('--cpus', default='144-167', help='CPU affinity on both nodes; empty disables')
p.add_argument('--reverse', action='store_true')
p.add_argument('--port', type=int, default=18110)
p.add_argument('--out', default='results')
a = p.parse_args()
if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', a.name):
    p.error('--name must be a plain filename component')
nodes = [('116.204.37.141', '192.168.0.217', '29.184.165.65', a.hulk0_dir),
         ('116.204.96.45', '192.168.0.63', '29.184.110.95', a.hulk1_dir)]
if a.reverse:
    nodes.reverse()
out = pathlib.Path(a.out)
out.mkdir(parents=True, exist_ok=True)
commands, processes, files = [], [], []
# Reserve every result before starting either peer, including the JSON report.
report_file = open(out / (a.name + '.json'), 'x')
for suffix in ['.source.log', '.target.log']:
    files.append(open(out / (a.name + suffix), 'x'))
for i, (host, ip, npu, directory) in enumerate(nodes):
    args = ['timeout', '--signal=TERM', '--kill-after=15s', '240s']
    if a.cpus:
        args += ['taskset', '-c', a.cpus]
    args += ['env', 'ASCEND_GLOBAL_LOG_LEVEL=3', 'ASCEND_SLOG_PRINT_TO_STDOUT=0',
             'HCCL_OP_EXPANSION_MODE=' + a.mode,
             directory + '/build/' + a.kind + '_to_hbm_bw',
             '--role', 'source' if i == 0 else 'target', '--device', '0',
             '--host-ip', ip, '--npu-ip', npu, '--peer-host-ip', nodes[1-i][1],
             '--port', str(a.port), '--bytes', str(a.bytes), '--batch', str(a.batch),
             '--iterations', str(a.iterations), '--warmup', str(a.warmup), '--window', str(a.window)]
    command = '. /usr/local/Ascend/ascend-toolkit/set_env.sh && ' + shlex.join(args)
    commands.append(command)
    log = files[i]
    processes.append(subprocess.Popen(['ssh', '-F', '/dev/null', '-o', 'BatchMode=yes', '-o',
                                     'ConnectTimeout=10', 'hulk@' + host, command],
                                    stdout=log, stderr=subprocess.STDOUT))
    if i == 0:
        time.sleep(0.5)
codes = []
for proc in processes:
    try:
        codes.append(proc.wait(timeout=270))
    except subprocess.TimeoutExpired:
        # Kill only this SSH client; the remote timeout bounds its benchmark.
        proc.kill()
        proc.wait()
        codes.append(124)
for f in files:
    f.close()
logs = [(out / (a.name + suffix)).read_text() for suffix in ['.source.log', '.target.log']]
bandwidth = [line for line in logs[1].splitlines() if line.startswith('BANDWIDTH ')]
result = dict(vars(a), date=datetime.datetime.now(datetime.timezone.utc).isoformat(),
              returncodes=codes, passed=all(c == 0 for c in codes) and all('PASS:' in x for x in logs),
              commands=commands, bandwidth=bandwidth)
if bandwidth:
    result['metrics'] = dict(re.findall(r'(\S+)=([^ ]+)', bandwidth[0]))
report_file.write(json.dumps(result, indent=2) + '\n')
report_file.close()
print(json.dumps(result), flush=True)
if not result['passed']:
    for log in logs:
        print(log[-6000:])
    raise SystemExit(1)
