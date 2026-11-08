"""Funbit ETS2 Telemetry Server -> Nano USB, 115200 baud.
Protocol reference: https://github.com/Funbit/ets2-telemetry-server/blob/master/Telemetry.md
"""
import argparse
import json
import math
import time
import urllib.request
import ctypes
import os
from ctypes import wintypes


def mouse_flash():
    """Read left mouse only while the actual ETS2 process is foreground."""
    if os.name != 'nt':
        return False
    user = ctypes.windll.user32
    if not user.GetAsyncKeyState(0x01) & 0x8000:
        return False
    user.GetForegroundWindow.restype = wintypes.HWND
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    pid = wintypes.DWORD()
    user.GetWindowThreadProcessId(user.GetForegroundWindow(), ctypes.byref(pid))
    kernel = ctypes.windll.kernel32
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                                wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.OpenProcess(0x1000, False, pid.value)
    if not handle:
        return False
    try:
        path = ctypes.create_unicode_buffer(32768)
        length = wintypes.DWORD(len(path))
        if not kernel.QueryFullProcessImageNameW(handle, 0, path, ctypes.byref(length)):
            return False
        return os.path.basename(path.value).lower() == 'eurotrucks2.exe'
    finally:
        kernel.CloseHandle(handle)


def number(value):
    value = float(value)
    if not math.isfinite(value):
        raise ValueError('Sonlu olmayan telemetri degeri')
    return value


def interpolate(value, points):
    value = max(points[0][0], min(points[-1][0], number(value)))
    for (x0, y0), (x1, y1) in zip(points, points[1:]):
        if value <= x1:
            return round(y0 + (value-x0) * (y1-y0) / (x1-x0))
    return points[-1][1]


def packet(data, now=None, flash=False):
    if not data['game']['connected']:
        return None
    t = data['truck']
    paused = data['game'].get('paused', False)
    speed = 0 if paused else abs(number(t['speed']))
    rpm = 0 if paused else round(max(0, min(6000, number(t['engineRpm']))))
    # Bench points: indicated 40 -> raw560, ~50 ->720, 100 ->1504.
    # Above100 is provisional extrapolation; 180/raw2800 is not verified.
    speed_raw = interpolate(speed, [(0, 0), (40, 560), (50, 720), (100, 1504), (180, 2800)])
    capacity = number(t['fuelCapacity'])
    if capacity <= 0:
        raise ValueError('Gecerli depo kapasitesi yok; kamyona girin')
    # Confirmed bench: 0x32=half, 0x60=full.
    fuel = interpolate(number(t['fuel'])/capacity, [(0, 0), (.5, 50), (1, 96)])
    # Source temperature C+40; bench midpoint120, full156. Clamp to tested scale.
    temp = round(max(40, min(156, number(t['waterTemperature']) + 40)))
    high = t.get('lightsBeamHighOn') or (flash and not paused)
    lights = ((0x20 if t.get('lightsParkingOn') else 0)
              | (0x08 if t.get('lightsBeamLowOn') else 0)
              | (0x10 if high else 0)
              | (0x04 if t.get('frontFogOn') else 0))
    # Live Funbit observation: On fields carry the actual blinking phase.
    # Active remains false during hazards on this installation.
    left = bool(t.get('blinkerLeftOn', False))
    right = bool(t.get('blinkerRightOn', False))
    turns = ((0x40 if left else 0) | (0x20 if right else 0)) if not paused else 0
    park = int(bool(t.get('parkBrakeOn', False)))
    ignition = int(bool(t.get('electricOn', True)))
    cruise = int(bool(t.get('cruiseControlOn', False)))
    # engineOn remains true while paused; zero display RPM must not trigger oil warnings.
    engine = int(bool(t.get('engineOn', number(t['engineRpm']) > 300)))
    warnings = ((1 if t.get('oilPressureWarningOn') else 0)
                | (2 if t.get('batteryVoltageWarningOn') else 0)
                | int(t.get('dashboardFlags', 0)))
    return f'game {rpm} {speed_raw} {fuel} {temp} {lights} {turns} {park} {ignition} {cruise} {engine} {warnings}\n'


def self_test():
    d = {'game': {'connected': True, 'paused': False}, 'truck': {
        'speed': 100, 'engineRpm': 3500, 'fuel': 350, 'fuelCapacity': 700,
        'waterTemperature': 80, 'lightsParkingOn': True, 'blinkerLeftOn': True}}
    assert packet(d, now=0) == 'game 3500 1504 50 120 32 64 0 1 0 1 0\n'
    assert packet(d, now=.6).split()[6] == packet(d, now=0).split()[6]
    d['truck']['blinkerRightOn'] = True
    assert packet(d, now=0).split()[6] == '96'
    assert packet(d, now=.6).split()[6] == packet(d, now=0).split()[6]
    d['truck']['blinkerLeftOn'] = False
    assert packet(d, now=0).split()[6] == '32'
    d['truck']['blinkerRightOn'] = False
    assert packet(d, now=0).split()[6] == '0'
    d['truck']['speed'] = -40
    assert packet(d).split()[2] == '560'
    d['game']['paused'] = True
    assert packet(d).split()[1:3] == ['0', '0']
    d['game']['connected'] = False
    assert packet(d) is None
    d['game']['connected'] = True
    d['truck']['fuel'] = 700
    assert packet(d).split()[3] == '96'
    d['truck']['fuel'] = 1400
    assert packet(d).split()[3] == '96'
    d['truck']['parkBrakeOn'] = True
    assert packet(d).split()[7] == '1'
    d['truck']['parkBrakeOn'] = False
    assert packet(d).split()[7] == '0'
    d['game']['paused'] = False
    assert packet(d, flash=True).split()[5] == '48'
    assert packet(d, flash=False).split()[5] == '32'
    d['game']['paused'] = True
    assert packet(d, flash=True).split()[5] == '32'
    d['truck']['lightsBeamHighOn'] = True
    assert packet(d, flash=False).split()[5] == '48'
    d['truck']['lightsBeamHighOn'] = False
    d['truck']['lightsBeamLowOn'] = True
    assert packet(d, flash=False).split()[5] == '40'
    d['truck']['lightsParkingOn'] = False
    assert packet(d, flash=False).split()[5] == '8'
    d['truck']['electricOn'] = False
    d['truck']['cruiseControlOn'] = True
    assert packet(d).split()[8:10] == ['0', '1']
    d['truck']['engineOn'] = True
    d['game']['paused'] = True
    assert packet(d).split()[10] == '1'
    d['truck']['engineOn'] = False
    d['truck']['oilPressureWarningOn'] = True
    d['truck']['batteryVoltageWarningOn'] = True
    assert packet(d).split()[10:12] == ['0', '3']
    print('Kalibrasyon/protokol kontrolleri OK')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--port', default='COM8')
    p.add_argument('--url', default='http://127.0.0.1:25555/api/ets2/telemetry')
    p.add_argument('--self-test', action='store_true')
    p.add_argument('--dry-run', action='store_true', help='USB/CAN gondermeden telemetriyi yazdir')
    args = p.parse_args()
    if args.self_test:
        self_test()
        return
    device = None
    if not args.dry_run:
        import serial
        device = serial.Serial(args.port, 115200, timeout=.05, write_timeout=.5)
        time.sleep(3)  # Nano resets when the USB port opens.
    last_message = None
    last_print = 0
    try:
        while True:
            started = time.monotonic()
            try:
                req = urllib.request.Request(args.url, headers={'Cache-Control': 'no-cache'})
                with urllib.request.urlopen(req, timeout=.5) as response:
                    data = json.load(response)
                line = packet(data, flash=mouse_flash())
                message = 'ETS2 bagli' if line else 'ETS2 bagli degil; Nano timeout ile duracak'
                if line:
                    if device:
                        device.write(line.encode('ascii'))
                    if started-last_print >= 1:
                        print(line.strip(), flush=True)
                        last_print = started
            except (OSError, ValueError, KeyError, TypeError) as error:
                message = f'Telemetri bekleniyor: {error}'
                # Do not refresh heartbeat with old data; Nano clears speed/RPM.
            if message != last_message:
                print(message, flush=True)
                last_message = message
            if device and device.in_waiting:
                output = device.read(device.in_waiting).decode('utf-8', errors='replace')
                print(output, end='', flush=True)
                if 'HATA:' in output or 'LOCK=1' in output:
                    raise RuntimeError('Nano hata bildirdi; kopru durduruldu')
            time.sleep(max(0, .05-(time.monotonic()-started)))
    except KeyboardInterrupt:
        print('\nKopru kapatiliyor')
    finally:
        if device:
            try:
                # Trigger watchdog zeroing before allowing TX to stop.
                device.write(b'game 0 0 0 40 0 0 0 0 0 0 0\n')
                device.flush()
            finally:
                device.close()


if __name__ == '__main__':
    main()
