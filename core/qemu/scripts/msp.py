#!/usr/bin/env python3
"""Minimal MSP v1 client for the emulated FC (USART1 over TCP).

usage: msp.py <port> raw_imu [n]      MSP_RAW_IMU n times
       (acc: 512 = 1 g; gyro: raw LSB of the 2000 dps range, 16.384 = 1 deg/s -
        RotorFlight's gyroRateDps() divides by the scale despite its name)
       msp.py <port> acc_calibration  MSP_ACC_CALIBRATION (205)
"""
import socket, struct, sys, time

MSP_RAW_IMU, MSP_ACC_CALIBRATION = 102, 205


def frame(cmd, payload=b""):
    body = bytes([len(payload), cmd]) + payload
    crc = 0
    for b in body:
        crc ^= b
    return b"$M<" + body + bytes([crc])


def request(s, cmd, timeout=3.0):
    s.sendall(frame(cmd))
    buf, end = b"", time.time() + timeout
    while time.time() < end:
        try:
            buf += s.recv(512)
        except socket.timeout:
            continue
        i = buf.find(b"$M>")
        if i >= 0 and len(buf) >= i + 6 and len(buf) >= i + 6 + buf[i + 3]:
            n = buf[i + 3]
            return buf[i + 5:i + 5 + n]
    raise TimeoutError(f"no reply to MSP {cmd}")


def main():
    s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
    s.settimeout(0.2)
    what = sys.argv[2]
    if what == "raw_imu":
        for _ in range(int(sys.argv[3]) if len(sys.argv) > 3 else 1):
            v = struct.unpack("<9h", request(s, MSP_RAW_IMU)[:18])
            g = tuple(round(x / 16.384, 2) for x in v[3:6])
            print(f"MSP_RAW_IMU acc={v[0:3]} (512=1g) gyro={v[3:6]} lsb = {g} dps mag={v[6:9]}")
            time.sleep(0.2)
    elif what == "acc_calibration":
        request(s, MSP_ACC_CALIBRATION)
        print("MSP_ACC_CALIBRATION sent")


main()
