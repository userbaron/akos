#!/usr/bin/env python3
"""TCP integration checks. Run after cmake .. and make in hw4/build."""
import argparse
import concurrent.futures
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import tempfile
import time

DATA = Path(__file__).resolve().parents[1] / 'data'
EXPECTED = {'port.dat': 46, 'port_small.dat': 21, 'port_reject.dat': 24,
            'port_mixed.dat': 197, 'port_container.dat': 292,
            'port_deadlock_test.dat': 400}


def wait_for(check, timeout=5):
    deadline = time.monotonic() + timeout
    while not check():
        if time.monotonic() >= deadline:
            raise AssertionError('Timed out waiting for the application')
        time.sleep(.02)


def validate(text, config):
    ships = {int(i): cargo for i, cargo in
             re.findall(r'^SHIP (\d+) \S+ (\S+)', config, re.M)}
    types = {kind.lower(): {int(i): cargo for i, cargo in
             re.findall(r'^' + kind + r' (\d+) (\S+)', config, re.M)}
             for kind in ('BERTH', 'CRANE')}
    held = {'berth': {}, 'tug': {}, 'crane': {}}
    owned = {}
    for line in text.splitlines():
        found = re.search(r'ship (\d+) (?:took (berth)|got (tug|crane)) (\d+)', line)
        if found:
            ship, kind, resource = int(found[1]), found[2] or found[3], int(found[4])
            assert resource not in held[kind], ('Double assignment', line)
            if kind in types:
                assert ships[ship] == types[kind][resource], ('Incompatible resource', line)
            held[kind][resource] = ship
            owned[ship, kind] = resource
        found = re.search(r'ship (\d+) (moored|unloaded|departed)', line)
        if found:
            ship, action = int(found[1]), found[2]
            for kind in {'moored': ['tug'], 'unloaded': ['crane'],
                         'departed': ['tug', 'berth']}[action]:
                resource = owned.pop((ship, kind))
                assert held[kind].pop(resource) == ship
    for used, capacity in re.findall(r'Warehouse \d+ \[\w+\]: (-?\d+)/(\d+) used', text):
        assert 0 <= int(used) <= int(capacity), 'Warehouse capacity violated'
    for left, initial in re.findall(r'Ship \d+ .*?\[\w+, (-?\d+)/(\d+),', text):
        assert 0 <= int(left) <= int(initial), 'Invalid remaining cargo'


class Scene:
    def __init__(self, binaries, config, count=2):
        self.binaries, self.count = binaries, count
        self.temp = tempfile.TemporaryDirectory(prefix='akos-hw4-')
        self.root = Path(self.temp.name)
        (self.root / 'build').mkdir()
        (self.root / 'logs').mkdir()
        self.config = self.root / 'scenario.dat'
        self.config.write_text(config)
        self.processes = []
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            self.port = str(sock.getsockname()[1])

    def start(self, binary, *args):
        output = self.root / ('process-%d.txt' % len(self.processes))
        with output.open('w') as stream:
            process = subprocess.Popen([str(self.binaries / binary), '127.0.0.1',
                                        self.port, *map(str, args)],
                                       cwd=self.root / 'build', stdin=subprocess.DEVNULL,
                                       stdout=stream, stderr=subprocess.STDOUT,
                                       start_new_session=True)
        process.output = output
        self.processes.append(process)
        return process

    def server(self):
        process = self.start('hw4', self.config, '--processes=%d' % self.count,
                             '--render=full')
        wait_for(lambda: 'Server listening' in process.output.read_text() or
                 process.poll() is not None)
        assert process.poll() is None, process.output.read_text()
        self.owner = process
        return process

    def workers(self):
        self.worker_list = [self.start('hw4_worker', i) for i in range(self.count)]
        return self.worker_list

    def finish(self, timeout=65, error=False):
        self.owner.wait(timeout=timeout)
        assert self.owner.returncode in ((0, 1) if error else (0,)), self.owner.output.read_text()
        for process in self.processes:
            if process is self.owner:
                continue
            process.wait(timeout=8)
            assert process.returncode in ((0, 1) if error else (0,)), process.output.read_text()
        for process in self.processes:
            def group_gone():
                try:
                    os.killpg(process.pid, 0)
                    return False
                except ProcessLookupError:
                    return True
            wait_for(group_gone, 3)
        text = self.owner.output.read_text()
        assert text.count('Simulation finished at tick') == 1, text
        return text

    def __enter__(self):
        return self

    def __exit__(self, kind, value, traceback):
        for process in reversed(self.processes):
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        for process in self.processes:
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        if kind:
            for process in self.processes:
                print('\n---', process.args[0], '---\n', process.output.read_text()[-4000:])
        self.temp.cleanup()


def scenario(binaries, path, count):
    config = path.read_text()
    with Scene(binaries, config, count) as scene:
        scene.server()
        observer = scene.start('hw4_observer', '--render=full') if count > 1 else None
        scene.workers()
        text = scene.finish()
        validate(text, config)
        if observer:
            display = observer.output.read_text()
            assert '=== Port state' in display, display
            validate(display, config)
        actual = int(re.search(r'Simulation finished at tick (\d+)', text)[1])
        if count == 1:
            assert actual == EXPECTED[path.name], (path.name, actual)
        assert (scene.root / 'logs' / 'port.log').read_text().count('Simulation finished') == 1
        return '%s, %d workers, tick %d' % (path.name, count, actual)


def held_open_config():
    text = (DATA / 'port_small.dat').read_text().replace('MAX_TICKS 200', 'MAX_TICKS 45')
    text = text.replace('SHIPS 1\nSHIP 1 Solo DRY 40 3', 'SHIPS 64\nSHIP 1 Solo DRY 40 1')
    return text + ''.join('SHIP %d Future%d DRY 10 1000\n' % (i, i) for i in range(2, 65))


def timing(binaries):
    config = (DATA / 'port_small.dat').read_text()
    config = config.replace('TIMING enter=5 leave=5', 'TIMING\tleave=3\tenter=2 # comment')
    with Scene(binaries, config) as scene:
        scene.server()
        scene.workers()
        assert 'Simulation finished at tick 16' in scene.finish(timeout=5)


def observers(binaries):
    with Scene(binaries, held_open_config()) as scene:
        scene.server()
        first = scene.start('hw4_observer', '--render=full')
        second = scene.start('hw4_observer', '--render=full')
        slow = socket.socket()
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        slow.connect(('127.0.0.1', int(scene.port)))
        slow.sendall(struct.pack('!5I', 16, 2, 0, 0, 0))
        try:
            scene.workers()
            wait_for(lambda: 'arrived' in first.output.read_text())
            first.send_signal(signal.SIGINT)
            first.wait(timeout=3)
            assert first.returncode == 0
            assert scene.owner.poll() is None and second.poll() is None
            third = scene.start('hw4_observer', '--render=full')
            wait_for(lambda: '=== Port state' in third.output.read_text())
            assert int(re.search(r'Port state \(tick (\d+)\)', third.output.read_text())[1]) > 0
            scene.finish(timeout=12)
            for process in (second, third):
                assert 'max_ticks reached' in process.output.read_text(), process.output.read_text()
        finally:
            slow.close()


def interruptions(binaries):
    for role in ('server', 'worker'):
        with Scene(binaries, held_open_config()) as scene:
            scene.server()
            scene.start('hw4_observer')
            workers = scene.workers()
            time.sleep(.3)
            target = scene.owner if role == 'server' else workers[0]
            target.send_signal(signal.SIGINT)
            scene.finish(timeout=8, error=(role == 'worker'))
        print('PASS SIGINT in ' + role, flush=True)


def rejected_clients(binaries):
    with Scene(binaries, held_open_config()) as scene:
        scene.server()
        original = scene.start('hw4_worker', 0)
        time.sleep(.2)
        duplicate = scene.start('hw4_worker', 0)
        duplicate.wait(timeout=4)
        assert duplicate.returncode != 0 and original.poll() is None
        invalid = scene.start('hw4_worker', 2)
        invalid.wait(timeout=4)
        assert invalid.returncode != 0 and scene.owner.poll() is None
        bad = socket.create_connection(('127.0.0.1', int(scene.port)))
        bad.sendall(struct.pack('!5I', 16, 999, 0, 0, 0))
        bad.close()
        scene.owner.send_signal(signal.SIGINT)
        # Rejected processes intentionally returned errors.
        scene.processes.remove(duplicate)
        scene.processes.remove(invalid)
        scene.finish(timeout=8)


def command_lines(binaries):
    cases = [('hw4', ['127.0.0.1', '0']), ('hw4', ['127.0.0.1', '12345', '--processes=0']),
             ('hw4', ['127.0.0.1', '12345', '--processes=81']),
             ('hw4_worker', ['127.0.0.1', '12345', '-1']),
             ('hw4_worker', ['127.0.0.1', '12345', '80']),
             ('hw4_observer', ['127.0.0.1', '12345', '--render=wrong'])]
    for name in ('hw4', 'hw4_worker', 'hw4_observer'):
        result = subprocess.run([str(binaries / name), '--help'], capture_output=True, timeout=3)
        assert result.returncode == 0, result.stderr
    for name, args in cases:
        result = subprocess.run([str(binaries / name), *args], capture_output=True, timeout=3)
        assert result.returncode != 0, (name, args)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=DATA.parent / 'build')
    parser.add_argument('--quick', action='store_true')
    args = parser.parse_args()
    binaries = args.build_dir.resolve()
    for name in ('hw4', 'hw4_worker', 'hw4_observer'):
        if not (binaries / name).is_file():
            parser.error('Build the program first: missing ' + str(binaries / name))
    command_lines(binaries)
    print('PASS command-line validation', flush=True)
    names = ('port.dat', 'port_small.dat', 'port_reject.dat') if args.quick else EXPECTED.keys()
    jobs = [(DATA / name, count) for name in names for count in ((1, 2) if args.quick else (1, 2, 8))]
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        futures = [pool.submit(scenario, binaries, path, count) for path, count in jobs]
        for future in concurrent.futures.as_completed(futures):
            print('PASS ' + future.result(), flush=True)
    timing(binaries)
    print('PASS both TIMING values, tabs and reversed parameter order', flush=True)
    observers(binaries)
    print('PASS multiple, late, reconnecting and slow observers', flush=True)
    rejected_clients(binaries)
    print('PASS duplicate, invalid and malformed clients', flush=True)
    interruptions(binaries)
    print('ALL CHECKS PASSED', flush=True)


if __name__ == '__main__':
    main()
