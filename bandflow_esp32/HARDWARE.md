# BandFlow wristband wiring

Pins are set at the top of `bandflow_esp32.ino`. The ESP32-S3 runs on 3.3 V, so nothing connected to a
pin may go above 3.3 V.

| Part | Pin | Setting |
| --- | --- | --- |
| Display (SPI) | 10 CS, 11 DC, 12 SCK, 13 MOSI, 1 reset, 38 backlight | fixed |
| Touch panel (I2C) | 9 SDA, 46 SCL, 8 interrupt, 3 reset | in `touch.h` |
| Vibration motor | 16 | `VIBRATION_PIN` |
| Microphone signal | 4 | `MIC_PIN` |

## Microphone

The microphone output **cannot go to GPIO 45**. That pin has no analog input, and it is also read when the chip
boots. The ADC works on GPIO 1 to 10 only. Display and touch already use 1, 3, 8, 9 and 10, so use **2, 4, 5, 6
or 7** and set `MIC_PIN` to match.

Your circuit from the Uno test needs two changes for the ESP32:

```
3V3 ──[ 10k R1 ]──┬── electret (+)
                  │
                  └──[ 1 µF C1 ]──┬── GPIO 4 (MIC_PIN)
                                  │
              3V3 ──[ R2 ]────────┤      R2 = R3 = 10k (47k is better if you have it)
                                  │
              GND ──[ R3 ]────────┘
electret (-) ── GND
```

1. **Add R2 and R3.** The capacitor passes only the changing part of the sound, so without them the ADC
   input has no resting voltage and half of every sound wave is lost. Two equal resistors hold it at about
   1.65 V, the middle of the ADC range. (The Uno test worked without them because a recording script cleaned
   the signal up afterwards.)
2. **Power R1 from 3.3 V, not 5 V.** It works either way because the capacitor blocks the DC, but one supply
   is cleaner and there is no way for 5 V to reach the pin.

When the watch powers on, the Serial Monitor (115200) prints a check such as:

```
Microphone check on GPIO 4: average 2051 of 4095, quiet noise swing 9 counts
  -> the bias level looks right
```

An average near 0 or near 4095 means the bias resistors are missing or the wire is loose.

If voice recognition is poor because the sound is too faint, a ready-made microphone amplifier board such as the
MAX4466 replaces the capsule, R1, C1, R2 and R3. Power it from 3.3 V and connect its OUT pin to `MIC_PIN`.

## Vibration motor

Never connect a motor straight to a GPIO pin. It draws far more current than the pin is allowed to supply and can
damage the chip. Switch it with a transistor:

```
GPIO 16 ──[ 1k ]── base of an NPN transistor (2N2222 or BC547)
emitter ── GND
collector ── motor (−)
motor (+) ── 3.3 V (or 5 V, whichever the motor is rated for)
diode (1N4148 or 1N4007) across the motor: stripe towards motor (+)
```

A vibration motor module that already has a driver on it can be connected directly: signal to GPIO 16, power and
ground as marked. If your driver turns the motor on when the pin goes low, change `VIBRATION_ACTIVE_LEVEL` to
`LOW`.

The watch buzzes once at power-on (a quick wiring test), for 0.35 s when a new step arrives, and with a short
double pulse when you tap DONE.
