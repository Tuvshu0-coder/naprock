"""Voice input for the BandFlow bridge: decode the wristband's audio and turn it into text with Whisper.

The wristband records 8 kHz mono audio, compresses it with IMA ADPCM and sends it over BLE (see app.py).
Nothing here talks to Bluetooth, so it can be tested on its own.
"""

import io
import json
import os
import struct
import unicodedata
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
# No hint by default. A hint such as an example sentence gets repeated back whenever Whisper cannot make out words,
# which produced tasks the worker never said. Set WHISPER_PROMPT only for a list of unusual words.
WHISPER_PROMPT = os.environ.get("WHISPER_PROMPT", "")
# Whisper says how sure it is of each phrase. A phrase it thinks is silence, or one it is guessing at, is dropped.
MAX_NO_SPEECH = float(os.environ.get("WHISPER_MAX_NO_SPEECH", "0.6"))
MIN_AVG_LOGPROB = float(os.environ.get("WHISPER_MIN_LOGPROB", "-1.0"))
WHISPER_TIMEOUT_SECONDS = 30

MIN_SECONDS = 0.4
MAX_SECONDS = 15
# The watch keeps the text in a 124-byte buffer while the worker confirms it.
MAX_TEXT_LENGTH = 120

# What Whisper tends to write when it is given noise or silence instead of speech.
# (compared after punctuation is turned into spaces, so "that's it" is written "that s it")
_NOISE_PHRASES = {"you", "the", "so", "bye", "bye bye", "thanks", "thank you", "thank you very much",
                  "thanks for watching", "thank you for watching", "please subscribe", "and that s it", "that s it",
                  "and that s all", "that s all", "thanks for listening", "i m sorry", "okay", "ok", "oh", "uh", "um", "hmm"}
_PLAIN = {"\u2018": "'", "\u2019": "'", "\u201c": '"', "\u201d": '"', "\u2013": "-", "\u2014": "-", "\u2026": "..."}

_INDEX_STEP = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
_STEP_SIZE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
]


class VoiceError(Exception):
    """An error that is safe to show on the watch. `code` says which kind, for anything that wants to react to it:
    too_short, too_long, bad_format, audio_lost, silence, unclear, stt_not_configured, stt_unreachable, stt_error."""

    def __init__(self, message, code="stt_error"):
        super().__init__(message)
        self.code = code


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
        raise VoiceError("Speech-to-text is not set up on the Pi. Set OPENAI_API_KEY and restart the bridge.", "stt_not_configured")
    # verbose_json carries the per-phrase confidence; the newer gpt-4o transcription models only answer in plain json.
    response_format = "verbose_json" if WHISPER_MODEL.startswith("whisper") else "json"
    fields = {"model": WHISPER_MODEL, "response_format": response_format, "temperature": "0"}
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
            return _confident_text(json.loads(response.read().decode("utf-8")))
    except urllib.error.HTTPError as error:
        try:
            detail = json.loads(error.read().decode("utf-8")).get("error", {}).get("message", "")
        except (ValueError, UnicodeDecodeError):
            detail = ""
        raise VoiceError(to_watch_text(f"Speech service said: {detail or f'error {error.code}'}")[:MAX_TEXT_LENGTH], "stt_error") from error
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        raise VoiceError("Could not reach the speech service. Check the Pi's internet connection.", "stt_unreachable") from error
    except ValueError as error:
        raise VoiceError("The speech service sent an answer I could not read.", "stt_error") from error


def _confident_text(body):
    """The text of the phrases Whisper is sure about. Raises VoiceError when it was not sure of any."""
    segments = body.get("segments") if isinstance(body, dict) else None
    if not isinstance(segments, list) or not segments:
        return str(body.get("text", "")) if isinstance(body, dict) else ""
    sure = [segment for segment in segments
            if segment.get("no_speech_prob", 0) <= MAX_NO_SPEECH and segment.get("avg_logprob", 0) >= MIN_AVG_LOGPROB]
    notes = []
    for segment in segments:
        dropped = "" if segment in sure else " DROPPED"
        notes.append(f"[no_speech {segment.get('no_speech_prob', 0):.2f}, logprob {segment.get('avg_logprob', 0):.2f}{dropped}]")
    print("Whisper confidence: " + ", ".join(notes))
    if not sure:
        if all(segment.get("no_speech_prob", 0) > MAX_NO_SPEECH for segment in segments):
            raise VoiceError("I did not hear any words. Try again, closer to the mic.", "silence")
        raise VoiceError("I could not understand that. Say the task again, slowly and clearly.", "unclear")
    return " ".join(str(segment.get("text", "")).strip() for segment in sure)


def to_watch_text(text):
    """Swap curly quotes, dashes and accented letters for plain ones: the watch font only has ASCII."""
    plain = "".join(_PLAIN.get(character, character) for character in str(text))
    return "".join(character for character in unicodedata.normalize("NFKD", plain) if not unicodedata.combining(character))


def clean_transcript(text):
    """Whisper adds a trailing full stop and sometimes quotes; a task title does not need them."""
    cleaned = " ".join(to_watch_text(text).split()).strip(" \"'")
    while cleaned and cleaned[-1] in ".!?,;:":
        cleaned = cleaned[:-1].rstrip()
    return cleaned.rstrip(" \"'")


def check_transcript(text):
    """Refuse text that is not a usable task. Raises VoiceError with a message for the watch."""
    words = "".join(character if character.isalnum() else " " for character in text.lower()).split()
    if not words:
        raise VoiceError("I did not hear any words. Try again, closer to the mic.", "silence")
    spoken = " ".join(words)
    prompt = " ".join("".join(character if character.isalnum() else " " for character in WHISPER_PROMPT.lower()).split())
    # With unclear audio Whisper repeats its own prompt or falls back on a stock phrase.
    if spoken in _NOISE_PHRASES or (len(words) >= 3 and spoken in prompt) or sum(character.isalpha() for character in text) < 2:
        raise VoiceError("I could not understand that. Say the task again, slowly and clearly.", "unclear")
    if not text.isascii():
        raise VoiceError("I heard words the watch cannot show. Please say the task in English.", "unclear")
    if len(text) > MAX_TEXT_LENGTH:
        raise VoiceError("That is too long for one task. Say it in fewer words.", "too_long")
    return text


def transcribe_recording(adpcm, sample_rate, total_samples, predictor, index, transcribe=None, on_audio=None):
    """Turn one received recording into task text. Raises VoiceError with a message for the watch."""
    if not 4000 <= sample_rate <= 48000:
        raise VoiceError("The audio format was not recognized.", "bad_format")
    seconds = total_samples / sample_rate
    if seconds < MIN_SECONDS:
        raise VoiceError("That was too short. Tap + and speak.", "too_short")
    if seconds > MAX_SECONDS:
        raise VoiceError("That recording was too long.", "too_long")
    if len(adpcm) < (total_samples + 1) // 2:
        raise VoiceError("Part of the audio was lost on the way. Please try again.", "audio_lost")
    pcm = adpcm_decode(adpcm, total_samples, predictor, index)
    if on_audio is not None:
        on_audio(pcm)  # for example to keep a copy of what the speech service is about to hear
    text = check_transcript(clean_transcript((transcribe or whisper_transcribe)(pcm_to_wav(pcm, sample_rate))))
    return text, pcm
