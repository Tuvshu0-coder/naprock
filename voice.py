"""Voice input for the BandFlow bridge: decode the wristband's audio and turn it into text with Whisper.

The wristband records 8 kHz mono audio, compresses it with IMA ADPCM and sends it over BLE (see app.py).
Nothing here talks to Bluetooth, so it can be tested on its own.
"""

import io
import json
import os
import struct
import urllib.error
import urllib.request
import uuid
import wave

# Set OPENAI_API_KEY to use OpenAI. WHISPER_URL can point at any server with the same
# /audio/transcriptions API (for example a local faster-whisper server), and then no key is needed.
WHISPER_URL = os.environ.get("WHISPER_URL", "https://api.openai.com/v1").rstrip("/")
WHISPER_MODEL = os.environ.get("WHISPER_MODEL", "whisper-1")
# English by default: the watch screen can only show plain ASCII text. Set WHISPER_LANGUAGE= (empty) to auto-detect.
WHISPER_LANGUAGE = os.environ.get("WHISPER_LANGUAGE", "en")
WHISPER_PROMPT = os.environ.get("WHISPER_PROMPT", "A short work task, for example: Check the pressure gauge on pump three.")
WHISPER_TIMEOUT_SECONDS = 30

MIN_SECONDS = 0.4
MAX_SECONDS = 15

_INDEX_STEP = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
_STEP_SIZE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
]


class VoiceError(Exception):
    """An error that is safe to show on the watch."""


def adpcm_decode(data, total_samples, predictor, index):
    """Decode IMA ADPCM (low nibble first) into little-endian 16-bit PCM."""
    out = bytearray()
    decoded = 0
    for byte in data:
        for code in (byte & 0x0F, byte >> 4):
            if decoded >= total_samples:
                break
            step = _STEP_SIZE[index]
            difference = step >> 3
            if code & 4:
                difference += step
            if code & 2:
                difference += step >> 1
            if code & 1:
                difference += step >> 2
            predictor += -difference if code & 8 else difference
            predictor = max(-32768, min(32767, predictor))
            index = max(0, min(88, index + _INDEX_STEP[code]))
            out += struct.pack("<h", predictor)
            decoded += 1
    return bytes(out)


def pcm_to_wav(pcm, sample_rate):
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(pcm)
    return buffer.getvalue()


def _multipart(fields, file_field, filename, content_type, file_bytes):
    boundary = uuid.uuid4().hex
    body = bytearray()
    for name, value in fields.items():
        body += f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n'.encode()
    body += f'--{boundary}\r\nContent-Disposition: form-data; name="{file_field}"; filename="{filename}"\r\n'.encode()
    body += f"Content-Type: {content_type}\r\n\r\n".encode() + file_bytes + f"\r\n--{boundary}--\r\n".encode()
    return bytes(body), f"multipart/form-data; boundary={boundary}"


def whisper_transcribe(wav_bytes):
    """Send a WAV file to the Whisper API and return the text."""
    api_key = os.environ.get("OPENAI_API_KEY", "")
    if not api_key and "api.openai.com" in WHISPER_URL:
        raise VoiceError("Speech-to-text is not set up on the Pi. Set OPENAI_API_KEY and restart the bridge.")
    fields = {"model": WHISPER_MODEL, "response_format": "json", "temperature": "0"}
    if WHISPER_LANGUAGE:
        fields["language"] = WHISPER_LANGUAGE
    if WHISPER_PROMPT:
        fields["prompt"] = WHISPER_PROMPT
    body, content_type = _multipart(fields, "file", "voice.wav", "audio/wav", wav_bytes)
    headers = {"Content-Type": content_type}
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"
    request = urllib.request.Request(f"{WHISPER_URL}/audio/transcriptions", data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=WHISPER_TIMEOUT_SECONDS) as response:
            return str(json.loads(response.read().decode("utf-8")).get("text", ""))
    except urllib.error.HTTPError as error:
        try:
            detail = json.loads(error.read().decode("utf-8")).get("error", {}).get("message", "")
        except (ValueError, UnicodeDecodeError):
            detail = ""
        raise VoiceError(f"Speech service said: {detail or f'error {error.code}'}") from error
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        raise VoiceError("Could not reach the speech service. Check the Pi's internet connection.") from error


def clean_transcript(text):
    """Whisper adds a trailing full stop and sometimes quotes; a task title does not need them."""
    cleaned = " ".join(str(text).split()).strip(" \"'")
    while cleaned and cleaned[-1] in ".!?,;:":
        cleaned = cleaned[:-1].rstrip()
    return cleaned


def transcribe_recording(adpcm, sample_rate, total_samples, predictor, index, transcribe=None):
    """Turn one received recording into task text. Raises VoiceError with a message for the watch."""
    if not 4000 <= sample_rate <= 48000:
        raise VoiceError("The audio format was not recognized.")
    seconds = total_samples / sample_rate
    if seconds < MIN_SECONDS:
        raise VoiceError("That was too short. Hold the mic button and speak.")
    if seconds > MAX_SECONDS:
        raise VoiceError("That recording was too long.")
    if len(adpcm) < (total_samples + 1) // 2:
        raise VoiceError("Part of the audio was lost on the way. Please try again.")
    pcm = adpcm_decode(adpcm, total_samples, predictor, index)
    text = clean_transcript((transcribe or whisper_transcribe)(pcm_to_wav(pcm, sample_rate)))
    if not text:
        raise VoiceError("I could not make out any words. Try again, closer to the mic.")
    return text, pcm
