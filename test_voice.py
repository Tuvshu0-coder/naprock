"""Checks for voice.py that need no wristband, no Bluetooth and no speech service.

Run with:  python -m unittest test_voice
"""

import io
import json
import struct
import unittest
import urllib.error
import wave
from unittest import mock

import voice

RATE = 8000
ONE_SECOND = bytes(RATE // 2)  # one second of ADPCM (4 bits per sample); the content does not matter here


def transcribe(adpcm=ONE_SECOND, samples=RATE, heard="Check the fuel pressure."):
    return voice.transcribe_recording(adpcm, RATE, samples, 0, 0, transcribe=lambda wav: heard)


class TranscriptTests(unittest.TestCase):
    def assert_refused(self, code, **kwargs):
        with self.assertRaises(voice.VoiceError) as caught:
            transcribe(**kwargs)
        self.assertEqual(caught.exception.code, code)
        message = str(caught.exception)
        # The watch shows this text, so it must be plain ASCII and short enough for its buffer.
        self.assertTrue(message.isascii() and 0 < len(message) <= voice.MAX_TEXT_LENGTH, message)
        return message

    def test_speech_becomes_a_clean_task(self):
        text, pcm = transcribe()
        self.assertEqual(text, "Check the fuel pressure")
        self.assertEqual(len(pcm), RATE * 2)

    def test_typographic_characters_are_made_plain_for_the_watch(self):
        text, _ = transcribe(heard="“Clean the café’s grill – twice”.")
        self.assertEqual(text, "Clean the cafe's grill - twice")

    def test_silence_is_reported(self):
        for heard in ("", "   ", "...", "♪"):
            self.assertIn("did not hear", self.assert_refused("silence", heard=heard))

    def test_unclear_audio_is_reported(self):
        # Stock phrases are what Whisper produces from noise.
        for heard in ("Thank you.", "you", "Thanks for watching!", "And that's it.", "That's it", "Okay"):
            self.assertIn("could not understand", self.assert_refused("unclear", heard=heard))

    def test_an_echo_of_a_hint_that_was_set_is_refused(self):
        hint = "A short work task, for example: Check the pressure gauge on pump three."
        with mock.patch.object(voice, "WHISPER_PROMPT", hint):
            for heard in ("Check the pressure gauge on pump three.", "A short work task"):
                self.assertIn("could not understand", self.assert_refused("unclear", heard=heard))

    def test_a_task_that_resembles_the_old_example_is_accepted(self):
        # Without a hint nothing is echoed, so a genuine task about a pressure gauge must go through.
        text, _ = transcribe(heard="Check the pressure gauge on pump two.")
        self.assertEqual(text, "Check the pressure gauge on pump two")

    def test_no_hint_is_sent_by_default(self):
        self.assertEqual(voice.WHISPER_PROMPT, "")

    def test_text_the_watch_cannot_show_is_refused(self):
        self.assertIn("English", self.assert_refused("unclear", heard="Проверить насос"))

    def test_too_long_text_is_refused(self):
        self.assert_refused("too_long", heard="check the valve " * 9)

    def test_bad_recordings_are_refused_before_any_speech_service_is_called(self):
        self.assert_refused("too_short", samples=RATE // 10)
        self.assert_refused("too_long", adpcm=bytes(RATE * 10), samples=RATE * 20)
        self.assert_refused("audio_lost", adpcm=ONE_SECOND[:100])
        with self.assertRaises(voice.VoiceError) as caught:
            voice.transcribe_recording(ONE_SECOND, 100, RATE, 0, 0, transcribe=lambda wav: "x")
        self.assertEqual(caught.exception.code, "bad_format")

    def test_the_decoded_audio_is_a_valid_wav_file(self):
        captured = {}
        voice.transcribe_recording(ONE_SECOND, RATE, RATE, 0, 0, transcribe=lambda wav: captured.setdefault("wav", wav) and "Open the valve")
        with wave.open(io.BytesIO(captured["wav"])) as wav:
            self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getnframes()), (1, 2, RATE, RATE))

    def test_adpcm_decoding_follows_the_ima_tables(self):
        # Code 7 with step 7 adds 7/8 + 7 + 3 + 1 = 11; the next step is 16 (index 8).
        self.assertEqual(struct.unpack("<2h", voice.adpcm_decode(bytes([0x07]), 2, 0, 0)), (11, 13))


class SpeechServiceTests(unittest.TestCase):
    def refused(self, url=voice.WHISPER_URL):
        with mock.patch.object(voice, "WHISPER_URL", url), self.assertRaises(voice.VoiceError) as caught:
            voice.whisper_transcribe(b"RIFF")
        self.assertTrue(str(caught.exception).isascii())
        return caught.exception

    def test_missing_key_says_speech_to_text_is_not_set_up(self):
        with mock.patch.dict(voice.os.environ, {"OPENAI_API_KEY": ""}):
            error = self.refused("https://api.openai.com/v1")
        self.assertEqual(error.code, "stt_not_configured")
        self.assertIn("not set up", str(error))

    def test_unreachable_service_is_reported(self):
        with mock.patch.dict(voice.os.environ, {"OPENAI_API_KEY": "key"}), \
                mock.patch.object(voice.urllib.request, "urlopen", side_effect=urllib.error.URLError("no route")):
            error = self.refused()
        self.assertEqual(error.code, "stt_unreachable")
        self.assertIn("Could not reach", str(error))

    def test_service_error_message_is_passed_on(self):
        failure = urllib.error.HTTPError("http://x", 401, "Unauthorized", {}, io.BytesIO(b'{"error": {"message": "Incorrect API key provided"}}'))
        with mock.patch.dict(voice.os.environ, {"OPENAI_API_KEY": "key"}), mock.patch.object(voice.urllib.request, "urlopen", side_effect=failure):
            error = self.refused()
        self.assertEqual(error.code, "stt_error")
        self.assertIn("Incorrect API key", str(error))

    def test_local_server_needs_no_key(self):
        class Reply(io.BytesIO):
            def __enter__(self):
                return self

            def __exit__(self, *_):
                return False

        with mock.patch.dict(voice.os.environ, {"OPENAI_API_KEY": ""}), mock.patch.object(voice, "WHISPER_URL", "http://127.0.0.1:9000/v1"), \
                mock.patch.object(voice.urllib.request, "urlopen", return_value=Reply(b'{"text": "Open the valve."}')) as urlopen:
            self.assertEqual(voice.whisper_transcribe(b"RIFF"), "Open the valve.")
        self.assertEqual(urlopen.call_args[0][0].full_url, "http://127.0.0.1:9000/v1/audio/transcriptions")
        self.assertNotIn("Authorization", urlopen.call_args[0][0].headers)


class ConfidenceTests(unittest.TestCase):
    """Whisper reports how sure it is of each phrase; unsure phrases must not become tasks."""

    class Reply(io.BytesIO):
        def __enter__(self):
            return self

        def __exit__(self, *_):
            return False

    def transcribe(self, segments, text="x"):
        body = json.dumps({"text": text, "segments": segments}).encode()
        with mock.patch.dict(voice.os.environ, {"OPENAI_API_KEY": "key"}):
            with mock.patch.object(voice.urllib.request, "urlopen", return_value=self.Reply(body)) as urlopen:
                return voice.whisper_transcribe(b"RIFF"), urlopen

    def segment(self, text, no_speech=0.05, logprob=-0.3):
        return {"text": text, "no_speech_prob": no_speech, "avg_logprob": logprob}

    def test_a_sure_phrase_is_kept(self):
        text, urlopen = self.transcribe([self.segment(" Open the valve")])
        self.assertEqual(text, "Open the valve")
        sent = urlopen.call_args[0][0].data
        self.assertIn(b"verbose_json", sent)
        self.assertNotIn(b'name="prompt"', sent)

    def test_only_the_sure_phrases_are_kept(self):
        text, _ = self.transcribe([self.segment("Open the valve"), self.segment("Thank you.", no_speech=0.9)])
        self.assertEqual(text, "Open the valve")

    def test_phrases_that_sound_like_silence_are_refused(self):
        with self.assertRaises(voice.VoiceError) as caught:
            self.transcribe([self.segment("Check the pressure gauge", no_speech=0.8)])
        self.assertEqual(caught.exception.code, "silence")

    def test_a_guess_is_refused(self):
        with self.assertRaises(voice.VoiceError) as caught:
            self.transcribe([self.segment("And that is it", logprob=-1.8)])
        self.assertEqual(caught.exception.code, "unclear")

    def test_an_answer_without_confidence_numbers_is_still_used(self):
        text, _ = self.transcribe(None, text="Open the valve.")
        self.assertEqual(text, "Open the valve.")


if __name__ == "__main__":
    unittest.main()
