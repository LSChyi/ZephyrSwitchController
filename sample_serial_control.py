#!/usr/bin/env python
from enum import Enum

import serial
import time
import random
from serial.tools import list_ports
from pick import pick


class Btn(Enum):
    B = 0x0002
    A = 0x0004
    Y = 0x0001
    X = 0x0008
    MINUS = 0x0100
    HOME = 0x1000
    SHARE = 0x2000
    PLUS = 0x0200
    L_CLICK = 0x0400
    R_CLICK = 0x0800
    L = 0x0010
    R = 0x0020
    ZL = 0x0040
    ZR = 0x0080


class DPad(Enum):
    UP = 0x00
    UP_RIGHT = 0x01
    RIGHT = 0x02
    DOWN_RIGHT = 0x03
    DOWN = 0x04
    DOWN_LEFT = 0x05
    LEFT = 0x06
    UP_LEFT = 0x7
    RELEASE = 0x08


def get_comport_dev_path() -> str:
    devs = list_ports.comports()
    if not len(devs):
        raise ValueError('No available comports')

    if len(devs) == 1:
        return devs[0].device

    title = 'Please select comport to use: '
    options = [dev.device for dev in devs]
    option, _ = pick(options, title)
    return option


class Controller:
    def __init__(self, ser):
        self._ser = ser
        self._btn = 0
        self._d_pad = DPad.RELEASE

    def send_event(self):
        byte_to_write = bytearray([
            self._btn & 0xFF, self._btn >> 8, self._d_pad.value, 128, 128, 128,
            128
        ])
        time.sleep(0.07)
        self._ser.write(byte_to_write)

    def press(self, btn):
        self._btn |= btn.value
        self.send_event()

    def release(self, btn):
        self._btn ^= btn.value
        self.send_event()

    def press_d_pad(self, d_pad):
        self._d_pad = d_pad
        self.send_event()

    def release_d_pad(self):
        self._d_pad = DPad.RELEASE
        self.send_event()

    def click(self, btn):
        self.press(btn)
        self.release(btn)

    def click_d_pad(self, d_pad):
        self.press_d_pad(d_pad)
        self.release_d_pad()

    def release_all(self):
        self._btn = 0
        self._d_pad = DPad.RELEASE
        self.send_event()


def switch_controller(ctrl):
    for _ in range(4):
        ctrl.click(Btn.L)
    time.sleep(0.5)
    ctrl.click(Btn.A)
    time.sleep(0.5)
    ctrl.click(Btn.A)
    time.sleep(2)


def main():
    with serial.Serial(get_comport_dev_path(), 115200) as ser:
        ctrl = Controller(ser)
        switch_controller(ctrl)


if __name__ == '__main__':
    main()
