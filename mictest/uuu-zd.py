import serial
import wave
import struct
import time

PORT = "COM6"       # Change this to your Arduino COM port
BAUD = 250000       # Must match Serial.begin() in mictest.ino

SAMPLE_RATE = 8000  # Must match sampleRate in mictest.ino
RECORD_SECONDS = 5

TARGET_PEAK = 0.9   # Normalize the recording so its peak reaches this fraction of full scale
MAX_GAIN = 4000     # Limit on boost (16-bit units per 12-bit count) so silence isn't blown up into hiss
HIGHPASS_ALPHA = 0.995  # DC blocker pole; closer to 1 = lower cutoff (~6 Hz at 0.995 / 8 kHz)

ser = serial.Serial(PORT, BAUD, timeout=0.1)
time.sleep(2)
ser.reset_input_buffer()

samples = []
buf = bytearray()
skipped = 0

print("Recording...")

target_samples = SAMPLE_RATE * RECORD_SECONDS
last_data_at = time.monotonic()

while len(samples) < target_samples:
    chunk = ser.read(max(ser.in_waiting, 2))
    if not chunk:
        if time.monotonic() - last_data_at > 2:
            ser.close()
            raise RuntimeError(
                f"Serial input stopped after {len(samples)} of "
                f"{target_samples} samples"
            )
        continue

    last_data_at = time.monotonic()
    buf.extend(chunk)

    # Each frame is [0x80 | high 6 bits, low 6 bits]; only the header byte has
    # bit 7 set, so after a dropped byte we discard until a valid pair lines up.
    while len(buf) >= 2:
        if not (buf[0] & 0x80) or (buf[1] & 0x80):
            del buf[0]
            skipped += 1
            continue
        samples.append(((buf[0] & 0x3F) << 6) | buf[1])
        del buf[:2]

ser.close()
samples = samples[:target_samples]

print("Saving...")

dc_offset = sum(samples) / len(samples)

# DC blocker (1-pole high-pass): removes bias and slow drift, keeps voice
filtered = []
prev_in = samples[0]
prev_out = 0.0
for s in samples:
    prev_out = s - prev_in + HIGHPASS_ALPHA * prev_out
    prev_in = s
    filtered.append(prev_out)

# Peak normalization using the 99.9th percentile so one click doesn't set the gain
magnitudes = sorted(abs(x) for x in filtered)
peak = magnitudes[int(len(magnitudes) * 0.999)] or 1.0
gain = min(TARGET_PEAK * 32767 / peak, MAX_GAIN)

pcm = bytearray()
for x in filtered:
    audio_sample = max(-32768, min(32767, round(x * gain)))
    pcm += struct.pack("<h", audio_sample)

with wave.open("recording.wav", "w") as wav:
    wav.setnchannels(1)
    wav.setsampwidth(2)
    wav.setframerate(SAMPLE_RATE)
    wav.writeframes(bytes(pcm))

print(f"Saved as recording.wav")
print(f"  DC offset: {dc_offset:.0f}/4092 (ideal ~2046)")
print(f"  Signal peak: {peak:.1f} counts of 12-bit (higher is better; under ~20 means the mic signal is weak)")
print(f"  Gain applied: {gain:.0f}x{' (capped)' if gain >= MAX_GAIN else ''}")
print(f"  Bytes resynced: {skipped}")
