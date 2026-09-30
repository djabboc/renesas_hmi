"""Run bounded tests and retain actual PASS/WAIT/SKIP/FAIL evidence; no resident monitor."""
import argparse
import codecs
from datetime import datetime
from pathlib import Path
import re
import time
from serial_test_port import SerialPort


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', default='COM8')
    p.add_argument('--command', action='append', help='e.g. "hmi_test eth mac"; repeat for sequential tests')
    p.add_argument('--timeout', type=float, default=120, help='Per-command time limit, seconds')
    p.add_argument('--usb-port', help='Optional system USB CDC port for a binary host echo check')
    args = p.parse_args()
    if not 1 <= args.timeout <= 300: p.error('timeout must be 1..300')
    commands = args.command or ['hmi_test all']
    if any(not re.fullmatch(r'hmi_test [a-z0-9 -]+', cmd) or len(cmd)>78 for cmd in commands):
        p.error('Only hmi_test commands accepted')
    logdir = Path(__file__).resolve().parent.parent / 'logs'; logdir.mkdir(exist_ok=True)
    path = logdir / f'peripherals_{datetime.now():%Y%m%d_%H%M%S}.log'
    failed = False
    print(f'Log: {path}', flush=True)
    with path.open('w', encoding='utf-8') as log, SerialPort(args.port) as serial:
        decoder=codecs.getincrementaldecoder('utf-8')(errors='backslashreplace')
        def record(data):
            text = decoder.decode(data)
            print(text, end='', flush=True); log.write(text); log.flush()
            return text
        # Synchronize with a responsive, idle suite before transmitting work.
        serial.write(b'\rhmi_test status\r')
        start = time.monotonic(); text = ''
        while time.monotonic()-start < 4:
            text += record(serial.read())
            if 'TEST STATUS ready=1 busy=0' in text: break
        else:
            raise RuntimeError('Peripheral suite is not ready/idle; no test sent')
        for command in commands:
            serial.write((command+'\r').encode()); start=time.monotonic(); text=''; echo_checked=False
            while time.monotonic()-start < args.timeout:
                text += record(serial.read())
                if args.usb_port and not echo_checked and 'USB CDC configured' in text:
                    payload=bytes(range(256))+b'HMI\x00\xff\r\n'*37
                    with SerialPort(args.usb_port) as usb:
                        usb.write(payload); received=b''; echo_start=time.monotonic()
                        while len(received)<len(payload) and time.monotonic()-echo_start<5:
                            received += usb.read()
                        ok=received==payload; failed |= not ok
                        record(f'\nHOST USB ECHO {"PASS" if ok else "FAIL"} bytes={len(received)}/{len(payload)}\n'.encode())
                    echo_checked=True
                if 'TEST BUSY' in text: raise RuntimeError('Board rejected command as busy')
                complete = 'TEST IDLE' in text if command not in ('hmi_test status','hmi_test help','hmi_test stop') else 'msh >' in text
                if complete: break
            else:
                serial.write(b'hmi_test stop\r')
                raise TimeoutError(f'{command} timed out; stop requested; COM released (reset if driver is stuck)')
            if re.search(r'TEST RESULT .* FAIL ',text): failed=True
            if args.usb_port and command=='hmi_test usb echo' and not echo_checked: failed=True
        serial.write(b'list thread\rfree\r'); start=time.monotonic()
        while time.monotonic()-start<0.5: record(serial.read())
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
