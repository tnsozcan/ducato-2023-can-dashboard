"""BeamNG custom UDP protocol -> existing Ducato Nano firmware."""
import argparse
import math
from pathlib import Path
import socket
import struct
import sys
import time

# Shared formatter is in the same bridge directory.
from ets2_bridge import packet, interpolate

FORMAT = struct.Struct('<8s4fII')
MAGIC = b'DUCATO1\0'


def decode(raw):
    if len(raw) != FORMAT.size:
        raise ValueError('Beklenmeyen BeamNG paket uzunlugu')
    magic, speed, rpm, fuel, temp, lights, flags = FORMAT.unpack(raw)
    if magic != MAGIC:
        raise ValueError('Baska protokol paketi')
    if not all(math.isfinite(v) for v in (speed, rpm, fuel, temp)):
        raise ValueError('Gecersiz sayisal veri')
    if not 0 <= fuel <= 1 or not 0 <= rpm <= 100000:
        raise ValueError('Gecersiz yakit/devir')
    # Reuse the verified CAN calibration and USB packet formatter.
    return {'game': {'connected': True, 'paused': False}, 'truck': {
        'speed': speed * 3.6, 'engineRpm': rpm,
        'fuel': fuel, 'fuelCapacity': 1, 'waterTemperature': interpolate(temp, [(0, 0), (20, 20), (80, 90), (100, 116), (150, 116)]),
        'lightsParkingOn': bool(lights & 4),
        'lightsBeamLowOn': bool(lights & 1),
        'lightsBeamHighOn': bool(lights & 2),
        'blinkerLeftOn': bool(lights & 8), 'blinkerRightOn': bool(lights & 16),
        'electricOn': bool(flags & 1), 'engineOn': bool(flags & 2),
        'cruiseControlOn': bool(flags & 4),
        'oilPressureWarningOn': bool(flags & 8),
        'batteryVoltageWarningOn': bool(flags & 16),
        'parkBrakeOn': bool(flags & 32),
        'dashboardFlags': (flags >> 4) & 16380,
        'frontFogOn': bool(lights & 32)}}


def self_test():
    raw = FORMAT.pack(MAGIC, 100/3.6, 3500, .5, 80, 4|2|8|16, 1|2|4|32)
    assert packet(decode(raw)) == 'game 3500 1504 50 130 48 96 1 1 1 1 0\n'
    assert packet(decode(FORMAT.pack(MAGIC, -40/3.6, 0, 1, 20, 0, 1|8|16))) == 'game 0 560 96 60 0 0 0 1 0 0 3\n'
    extra = FORMAT.pack(MAGIC, 0, 900, .1, 80, 4|2|32, 1|2|64|128|256|1024)
    fields = packet(decode(extra)).split()
    assert fields[5] == '52'  # park/high/front fog
    assert fields[11] == '92'  # ABS/door/low-fuel/general fault
    assert fields[4] == '130'  # BeamNG temperature lift only
    assert packet(decode(FORMAT.pack(MAGIC,0,900,.5,80,0,1|2|2048|4096|8192|16384))).split()[11] == '1920'
    assert packet(decode(FORMAT.pack(MAGIC,0,900,.5,80,0,1|2|32768))).split()[11] == '2048'
    assert packet(decode(FORMAT.pack(MAGIC,0,900,.5,80,0,1|2|65536|131072))).split()[11] == '12288'
    hot = FORMAT.pack(MAGIC,0,900,.5,100,0,1|2|8)
    assert packet(decode(hot)).split()[4] == '156'
    assert packet(decode(hot)).split()[11] == '1'
    for invalid in (b'', raw[:-1], b'BADMAGIC'+raw[8:],
                    FORMAT.pack(MAGIC, float('nan'), 0, .5, 80, 0, 0)):
        try:
            decode(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError('Gecersiz paket kabul edildi')
    print('BeamNG paket/kalibrasyon kontrolleri OK')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--port', default='COM8')
    p.add_argument('--udp-port', type=int, default=4568)
    p.add_argument('--dry-run', action='store_true')
    p.add_argument('--self-test', action='store_true')
    args = p.parse_args()
    if args.self_test:
        self_test()
        return
    device = None
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind(('127.0.0.1', args.udp_port))
        sock.settimeout(.2)
        if not args.dry_run:
            import serial
            device = serial.Serial(args.port, 115200, timeout=.02, write_timeout=.5)
            time.sleep(3)
        print(f'BeamNG bekleniyor | UDP {args.udp_port} | USB {args.port}', flush=True)
        last_valid = 0
        last_send = 0
        last_print = 0
        waiting = True
        serial_text = ''
        while True:
            try:
                raw, _ = sock.recvfrom(1024)
                # Drop queued old packets; do not replay a backlog into CAN.
                sock.setblocking(False)
                while True:
                    try:
                        raw, _ = sock.recvfrom(1024)
                    except BlockingIOError:
                        break
                sock.settimeout(.2)
                data = decode(raw)
                now = time.monotonic()
                last_valid = now
                if waiting:
                    print('BeamNG baglandi', flush=True)
                    waiting = False
                if now-last_send >= .045:
                    line = packet(data)
                    if device:
                        device.write(line.encode('ascii'))
                    last_send = now
                    if now-last_print >= 1:
                        print(line.strip(), flush=True)
                        last_print = now
            except socket.timeout:
                if not waiting and time.monotonic()-last_valid > 1:
                    print('Oyun verisi kesildi; Nano timeout ile sifirlayip duracak.', flush=True)
                    waiting = True
            except ValueError as error:
                # Invalid packets never refresh the firmware heartbeat.
                if time.monotonic()-last_print > 1:
                    print(f'Paket atlandi: {error}', flush=True)
                    last_print = time.monotonic()
            if device and device.in_waiting:
                output = device.read(device.in_waiting).decode('utf-8', errors='replace')
                print(output, end='', flush=True)
                serial_text = (serial_text+output)[-2048:]
                if 'HATA:' in serial_text or 'LOCK=1' in serial_text:
                    raise RuntimeError('Nano CAN hatasi bildirdi; kopru durdu')
    except KeyboardInterrupt:
        print('\nBeamNG koprusu kapatildi')
    finally:
        sock.close()
        if device:
            try:
                device.write(b'game 0 0 0 40 0 0 0 0 0 0 0\n')
                device.flush()
            finally:
                device.close()


if __name__ == '__main__':
    main()
