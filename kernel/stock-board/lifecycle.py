"""Observe QMP shutdown reasons; missing/invalid evidence always means stop."""
import json
import os
import socket
import threading
import time


def should_restart(returncode, event):
    return (returncode == 0 and isinstance(event, dict) and
            event.get('event') == 'SHUTDOWN' and
            event.get('data') == {'guest': True, 'reason': 'guest-reset'})


class ShutdownObserver:
    def __init__(self, directory, process, output):
        self.event = None
        self.error = None
        self.output = output
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(1)
        deadline = time.monotonic() + 5
        # macOS sockaddr_un is too short for the instance's absolute path.
        # Connect on the main thread, restoring cwd before starting a worker.
        old = os.open('.', os.O_RDONLY)
        try:
            os.chdir(directory)
            while True:
                try:
                    self.socket.connect('control.sock')
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    if process.poll() is not None or time.monotonic() >= deadline:
                        self.socket.close()
                        raise RuntimeError('QMP observer could not connect; automatic reboot unavailable')
                    time.sleep(0.02)
        finally:
            os.fchdir(old)
            os.close(old)
        self.socket.settimeout(None)
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        try:
            with self.socket.makefile('rwb', buffering=0) as stream, self.output.open('x') as log:
                greeting = json.loads(stream.readline(65536))
                if 'QMP' not in greeting:
                    raise ValueError('Invalid QMP greeting')
                stream.write(b'{"execute":"qmp_capabilities"}\n')
                while line := stream.readline(65536):
                    msg = json.loads(line)
                    if 'event' in msg:
                        log.write(json.dumps(msg) + '\n')
                        log.flush()
                    if msg.get('event') == 'SHUTDOWN':
                        self.event = msg
        except (OSError, ValueError) as error:
            self.error = str(error)
            self.event = None

    def finish(self):
        self.thread.join(timeout=3)
        if self.thread.is_alive():
            self.error = 'QMP observer did not finish'
            self.event = None
            self.socket.shutdown(socket.SHUT_RDWR)
            self.thread.join(timeout=1)
        self.socket.close()
        return self.event if self.error is None else None
