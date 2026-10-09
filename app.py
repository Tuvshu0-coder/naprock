"""BandFlow BLE bridge.

Flask owns the BLE connection and a small durable event queue. Node owns users,
tasks, subtasks, progress, and the primary SQLite database.
"""

import asyncio
import json
import os
import struct
import sys
import threading
import urllib.error
import urllib.request
import uuid
from concurrent.futures import TimeoutError as FutureTimeoutError
from datetime import datetime, timezone
from pathlib import Path

from bleak import BleakClient, BleakScanner
from flask import Flask, jsonify, request

import voice


class _Tee:
    """Copies what the bridge prints into bridge.log (next to app.py), with times, so a problem can be looked at afterwards."""

    def __init__(self, terminal, log):
        self.terminal = terminal
        self.log = log
        self.at_line_start = True

    def write(self, text):
        self.terminal.write(text)
        try:
            for part in text.splitlines(keepends=True):
                if self.at_line_start:
                    self.log.write(datetime.now().strftime("%H:%M:%S.%f")[:-3] + "  ")
                self.log.write(part)
                self.at_line_start = part.endswith("\n")
            self.log.flush()
        except OSError:
            pass

    def flush(self):
        self.terminal.flush()


# Set BANDFLOW_LOG_FILE=0 to switch the log file off (the automated tests do this).
if os.environ.get("BANDFLOW_LOG_FILE", "1") != "0":
    try:
        sys.stdout = _Tee(sys.stdout, open(Path(__file__).resolve().parent / "bridge.log", "a", encoding="utf-8"))
    except OSError:
        pass

SERVICE_UUID = "12345678-1234-1234-1234-1234567890ab"
TASK_CHAR_UUID = "12345678-1234-1234-1234-1234567890ac"
STATUS_CHAR_UUID = "12345678-1234-1234-1234-1234567890ad"
# Session channel: the bridge writes replies to RPC_RX and the watch sends requests by notifying RPC_TX.
# Both carry newline-terminated JSON split into BLE-sized chunks.
RPC_RX_CHAR_UUID = "12345678-1234-1234-1234-1234567890ae"
RPC_TX_CHAR_UUID = "12345678-1234-1234-1234-1234567890af"
# Voice recordings: the watch notifies audio packets here. Packet layouts (little endian):
#   START 0x01 | session u8 | sample rate u16 | total samples u32 | ADPCM predictor i16 | ADPCM index u8
#   DATA  0x02 | session u8 | sequence u16 | ADPCM bytes
#   END   0x03 | session u8 | packet count u16 | total ADPCM bytes u32
# When the END packet arrives the bridge transcribes the audio and writes {"t":"transcript"} to RPC_RX.
AUDIO_TX_CHAR_UUID = "12345678-1234-1234-1234-1234567890b0"
AUDIO_START, AUDIO_DATA, AUDIO_END = 1, 2, 3
# Set BANDFLOW_SAVE_AUDIO=1 to keep the last recording as last-voice.wav, handy when tuning the microphone.
SAVE_AUDIO = os.environ.get("BANDFLOW_SAVE_AUDIO", "") == "1"
VOICE_DEBUG_PATH = Path(__file__).resolve().parent / "last-voice.wav"
TARGET_NAME = "BandFlow-Wristband"
BRIDGE_API_VERSION = "v3"
SUPPORTED_BRIDGE_VERSIONS = {"v1", "v2", "v3"}

SCAN_SECONDS = 15.0
RECONNECT_DELAY_SECONDS = 5.0
TASK_WRITE_TIMEOUT_SECONDS = 10.0
NODE_EVENT_URL = os.environ.get("NODE_EVENT_URL", "http://127.0.0.1:8787/internal/ble/events")
NODE_RPC_URL = os.environ.get("NODE_RPC_URL", "http://127.0.0.1:8787/internal/band/rpc")
MAX_CHUNK_BYTES = 180
MAX_MESSAGE_BYTES = 8192
# Bluetooth notifications are not acknowledged, so a few audio packets can go missing. The bridge asks the band to
# send just those again, up to this many times, and only if no more than this many are missing.
MAX_RESEND_ROUNDS = 4
MAX_RESEND_LIST = 40
INTERNAL_TOKEN = os.environ.get("BLE_INTERNAL_TOKEN", "")
QUEUE_PATH = Path(os.environ.get("BLE_EVENT_QUEUE_PATH", Path(__file__).resolve().parent / "ble-event-queue.json"))
STATE_PATH = Path(os.environ.get("BLE_ASSIGNMENT_STATE_PATH", Path(__file__).resolve().parent / "ble-assignment.json"))

app = Flask(__name__)

_status_lock = threading.Lock()
_state_lock = threading.Lock()
_latest_status = None
_queued_events = []
_current_assignment = None


def _read_json(path, default):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, OSError, json.JSONDecodeError):
        return default


def _write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2), encoding="utf-8")


with _state_lock:
    _queued_events = _read_json(QUEUE_PATH, [])
    _current_assignment = _read_json(STATE_PATH, None)


def _post_event(event):
    payload = json.dumps(event).encode("utf-8")
    headers = {"Content-Type": "application/json"}
    if INTERNAL_TOKEN:
        headers["X-BandFlow-Token"] = INTERNAL_TOKEN
    headers["X-BandFlow-Bridge-Version"] = BRIDGE_API_VERSION
    request_object = urllib.request.Request(NODE_EVENT_URL, data=payload, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(request_object, timeout=8) as response:
            return 200 <= response.status < 300
    except (urllib.error.URLError, TimeoutError, OSError):
        return False


def _node_rpc(band_id, request_body):
    """Forward one watch request to Node and return its JSON reply."""
    payload = json.dumps({"band_id": band_id, "request": request_body}).encode("utf-8")
    headers = {"Content-Type": "application/json", "X-BandFlow-Bridge-Version": BRIDGE_API_VERSION}
    if INTERNAL_TOKEN:
        headers["X-BandFlow-Token"] = INTERNAL_TOKEN
    request_object = urllib.request.Request(NODE_RPC_URL, data=payload, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(request_object, timeout=8) as response:
            return json.loads(response.read().decode("utf-8"))
    except (urllib.error.URLError, TimeoutError, OSError, json.JSONDecodeError):
        return {"ok": False, "error": "The BandFlow server did not answer."}


async def _flush_events():
    while True:
        with _state_lock:
            event = _queued_events[0] if _queued_events else None
        if event is None:
            return
        acknowledged = await asyncio.to_thread(_post_event, event)
        if not acknowledged:
            return
        with _state_lock:
            if _queued_events and _queued_events[0].get("event_id") == event.get("event_id"):
                _queued_events.pop(0)
                _write_json(QUEUE_PATH, _queued_events)


def _queue_event(event):
    with _state_lock:
        if any(item.get("event_id") == event.get("event_id") for item in _queued_events):
            return
        _queued_events.append(event)
        _write_json(QUEUE_PATH, _queued_events)


class BandFlowBle:
    def __init__(self):
        self.loop = None
        self.client = None
        self.address = None
        self._connected = None
        self._write_lock = None
        self._rx_buffer = bytearray()
        self._pending = set()
        self._voice = None
        self.thread = threading.Thread(target=self._run, name="bandflow-ble", daemon=True)

    def start(self):
        self.thread.start()

    def _run(self):
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        self.loop.run_until_complete(self._connection_loop())
        self.loop.close()

    async def _find_band(self):
        print("Scanning for BandFlow...")
        devices = await BleakScanner.discover(timeout=SCAN_SECONDS, return_adv=True, scanning_mode="active")
        for address, (device, advertisement) in devices.items():
            advertised_name = advertisement.local_name or device.name or ""
            service_uuids = [uuid_value.casefold() for uuid_value in advertisement.service_uuids]
            if SERVICE_UUID.casefold() in service_uuids or advertised_name.casefold() == TARGET_NAME.casefold():
                print(f"Found BandFlow: {advertised_name or '(name hidden)'} at {address}")
                return device
        print("BandFlow not found; retrying...")
        return None

    async def _connection_loop(self):
        global _latest_status
        while True:
            try:
                device = await self._find_band()
            except Exception as error:
                # For example Bluetooth switched off in Windows. Without this the thread would die and never recover.
                print(f"Cannot scan for BandFlow: {error}. Turn Bluetooth on; trying again...")
                await asyncio.sleep(RECONNECT_DELAY_SECONDS)
                continue
            if device is None:
                await asyncio.sleep(RECONNECT_DELAY_SECONDS)
                continue

            disconnected = asyncio.Event()

            def on_disconnect(_):
                print("BandFlow disconnected; scanning again...")
                self.loop.call_soon_threadsafe(disconnected.set)

            try:
                # Windows caches a device's services, which would hide characteristics added by new firmware.
                async with BleakClient(device, disconnected_callback=on_disconnect, winrt={"use_cached_services": False}) as client:
                    self.client = client
                    self.address = str(device.address).lower()
                    self._write_lock = asyncio.Lock()
                    self._rx_buffer = bytearray()
                    self._voice = None
                    self._connected = asyncio.Event()
                    self._connected.set()

                    def on_status(_, data):
                        global _latest_status
                        status = data.decode(errors="replace").strip()
                        with _status_lock:
                            _latest_status = status
                        print(f"Status from band: {status}")
                        if status.upper() == "DONE":
                            self.loop.create_task(self._handle_done())

                    await client.start_notify(STATUS_CHAR_UUID, on_status)
                    await client.start_notify(RPC_TX_CHAR_UUID, self._on_rpc_chunk)
                    try:
                        await client.start_notify(AUDIO_TX_CHAR_UUID, self._on_audio_packet)
                    except Exception as error:
                        print(f"Voice input is unavailable (flash the newest firmware): {error}")
                    print("Connected!")
                    # The watch waits for this before it asks who is linked.
                    await self._send_rpc({"t": "hello"})
                    await _flush_events()
                    await disconnected.wait()
                    await client.stop_notify(STATUS_CHAR_UUID)
                    await client.stop_notify(RPC_TX_CHAR_UUID)
                    try:
                        await client.stop_notify(AUDIO_TX_CHAR_UUID)
                    except Exception:
                        pass
            except Exception as error:
                print(f"BLE connection error: {error}; retrying...")
            finally:
                self.client = None
                self.address = None
                self._connected = None
            await asyncio.sleep(RECONNECT_DELAY_SECONDS)

    async def _handle_done(self):
        with _state_lock:
            assignment = dict(_current_assignment) if _current_assignment else None
        if not assignment or not assignment.get("task_id") or not assignment.get("subtask_id"):
            print("Received DONE but no current assignment is known")
            return
        event = {
            "event_id": str(uuid.uuid4()),
            "task_id": assignment["task_id"],
            "subtask_id": assignment["subtask_id"],
            "event_type": "subtask_completed",
            "created_at": datetime.now(timezone.utc).isoformat(),
            "payload": {"source": "wristband", "text": "DONE"},
        }
        _queue_event(event)
        await _flush_events()

    def _on_rpc_chunk(self, _, data):
        self._rx_buffer.extend(data)
        if len(self._rx_buffer) > MAX_MESSAGE_BYTES:
            print("Dropping an oversized message from the band")
            self._rx_buffer.clear()
            return
        while b"\n" in self._rx_buffer:
            line, _, rest = bytes(self._rx_buffer).partition(b"\n")
            self._rx_buffer = bytearray(rest)
            if line.strip():
                task = self.loop.create_task(self._handle_band_message(line))
                self._pending.add(task)
                task.add_done_callback(self._pending.discard)

    def _spawn(self, coroutine):
        task = self.loop.create_task(coroutine)
        self._pending.add(task)
        task.add_done_callback(self._pending.discard)

    def _on_audio_packet(self, _, data):
        data = bytes(data)
        if len(data) < 2:
            return
        kind, session = data[0], data[1]
        if kind == AUDIO_START and len(data) >= 11:
            rate, total, predictor, index = struct.unpack("<HIhB", data[2:11])
            self._voice = {"session": session, "rate": rate, "total": total, "predictor": predictor, "index": index,
                           "chunks": {}, "rounds": 0, "sizes": {}}
            print(f"Voice recording {session} started: {total} samples at {rate} Hz")
        elif kind == AUDIO_END and len(data) >= 8 and (self._voice is None or self._voice["session"] != session):
            # The start packet never arrived, so there is nothing to repair: ask the band to send it all again.
            print(f"Voice recording {session}: got the end without the start; asking for the whole recording again")
            self._spawn(self._reply_audio_lost(session))
        elif self._voice is None or self._voice["session"] != session:
            return  # a late packet from a recording that was replaced or cancelled
        elif kind == AUDIO_DATA and len(data) >= 4:
            (sequence,) = struct.unpack("<H", data[2:4])
            self._voice["chunks"].setdefault(sequence, data[4:])  # a packet that arrives twice counts once
            self._voice["sizes"][len(data)] = self._voice["sizes"].get(len(data), 0) + 1
        elif kind == AUDIO_END and len(data) >= 8:
            packets, total_bytes = struct.unpack("<HI", data[2:8])
            self._finish_or_repair(self._voice, packets, total_bytes)

    def _finish_or_repair(self, recording, packets, total_bytes):
        chunks = recording["chunks"]
        missing = [number for number in range(packets) if number not in chunks]
        have_bytes = sum(len(chunk) for chunk in chunks.values())
        print(f"Voice recording {recording['session']} ended: the band says {packets} packets / {total_bytes} bytes; "
              f"the bridge has {len(chunks)} packets / {have_bytes} bytes (repair round {recording['rounds']}), "
              f"packet sizes {recording['sizes']}, missing {missing[:30]}")
        if not missing and have_bytes == total_bytes:
            self._voice = None
            whole = {**recording, "chunks": [chunks[number] for number in range(packets)], "intact": True}
            self._spawn(self._finish_voice(whole))
        elif missing and recording["rounds"] < MAX_RESEND_ROUNDS and len(missing) <= MAX_RESEND_LIST:
            recording["rounds"] += 1
            print(f"Asking the band to send {len(missing)} packets again")
            self._spawn(self._send_voice_reply({"t": "audio_resend", "sid": recording["session"], "missing": missing}))
        else:
            self._voice = None
            self._spawn(self._finish_voice({**recording, "chunks": [], "intact": False}))

    async def _reply_audio_lost(self, session):
        await self._send_voice_reply({"t": "transcript", "sid": session, "ok": False, "code": "audio_lost",
                                      "error": "Part of the audio was lost on the way. Please try again."})

    async def _finish_voice(self, recording):
        reply = {"t": "transcript", "sid": recording["session"]}
        try:
            if not recording["intact"]:
                raise voice.VoiceError("Part of the audio was lost on the way. Please try again.", "audio_lost")
            def keep_copy(pcm):
                # Saved before the speech service hears it, so a recording it could not understand can be listened to.
                if SAVE_AUDIO:
                    VOICE_DEBUG_PATH.write_bytes(voice.pcm_to_wav(pcm, recording["rate"]))
                    print(f"Saved what the speech service hears as {VOICE_DEBUG_PATH.name}")

            text, pcm = await asyncio.to_thread(
                voice.transcribe_recording, b"".join(recording["chunks"]), recording["rate"], recording["total"],
                recording["predictor"], recording["index"], on_audio=keep_copy)
            print(f"Heard: {text!r}")
            reply.update(ok=True, text=text)
        except voice.VoiceError as error:
            print(f"Voice request failed: {error}")
            reply.update(ok=False, error=str(error), code=error.code)
        except Exception as error:  # never leave the watch waiting
            print(f"Voice request crashed: {error!r}")
            reply.update(ok=False, error="Something went wrong while understanding that.", code="stt_error")
        await self._send_voice_reply(reply)

    async def _send_voice_reply(self, reply):
        # What the worker is waiting on, so it is worth a few tries and a clear log.
        for attempt in range(1, 4):
            try:
                print(f"Sending {reply.get('t')} to the band (try {attempt})...")
                await asyncio.wait_for(self._send_rpc(reply), timeout=10)
                print("Sent to the band.")
                return
            except Exception as error:
                print(f"Could not send it to the band (try {attempt} of 3): {error!r}")
                await asyncio.sleep(1)

    async def _handle_band_message(self, line):
        try:
            message = json.loads(line.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            print(f"Ignoring a malformed message from the band: {line[:80]!r}")
            return
        if not isinstance(message, dict) or not isinstance(message.get("t"), str):
            return
        request_id = message.pop("id", None)
        address = self.address
        if address is None:
            return
        reply = await asyncio.to_thread(_node_rpc, address, message)
        if request_id is not None:
            reply["id"] = request_id
        print(f"Band request {message.get('t')} -> ok={reply.get('ok')}")
        try:
            await self._send_rpc(reply)
        except Exception as error:
            print(f"Could not answer the band: {error}")

    async def _write_task(self, data):
        # One GATT write at a time, so chunks of two messages never interleave.
        async with self._write_lock:
            await self.client.write_gatt_char(TASK_CHAR_UUID, data, response=True)

    async def _send_rpc(self, message):
        if self.client is None or not self.client.is_connected:
            raise RuntimeError("BandFlow is not connected")
        data = (json.dumps(message, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")
        size = max(20, min(MAX_CHUNK_BYTES, (self.client.mtu_size or 23) - 3))
        async with self._write_lock:
            for start in range(0, len(data), size):
                await self.client.write_gatt_char(RPC_RX_CHAR_UUID, data[start:start + size], response=True)

    async def send_task(self, text):
        if self._connected is None:
            raise RuntimeError("BandFlow is not connected")
        await asyncio.wait_for(self._connected.wait(), timeout=TASK_WRITE_TIMEOUT_SECONDS)
        if self.client is None or not self.client.is_connected:
            raise RuntimeError("BandFlow is not connected")
        await self._write_task(text.encode("utf-8"))


ble_manager = BandFlowBle()
_start_lock = threading.Lock()
_started = False


def start_ble_once():
    global _started
    with _start_lock:
        if not _started:
            ble_manager.start()
            _started = True


def _require_internal():
    return not INTERNAL_TOKEN or request.headers.get("X-BandFlow-Token") == INTERNAL_TOKEN


@app.get("/ping")
def ping():
    return jsonify(status="ok", bridge_api_version=BRIDGE_API_VERSION)


@app.get("/v1/health")
def bridge_health():
    with _state_lock:
        queued_count = len(_queued_events)
    return jsonify(
        status="ok",
        bridge_api_version=BRIDGE_API_VERSION,
        connected=bool(ble_manager._connected),
        band_id=ble_manager.address,
        queued_events=queued_count,
    )


@app.get("/status")
def status():
    with _status_lock:
        current_status = _latest_status
    with _state_lock:
        assignment = _current_assignment
        queued_count = len(_queued_events)
    return jsonify(status=current_status, connected=bool(ble_manager._connected), assignment=assignment, queued_events=queued_count)


@app.get("/tasks")
def tasks():
    with _state_lock:
        return jsonify(events=_queued_events)


@app.post("/v1/dispatch")
def dispatch():
    global _current_assignment
    if not _require_internal():
        return jsonify(error="Internal token required"), 401
    requested_version = request.headers.get("X-BandFlow-Bridge-Version")
    if requested_version and requested_version not in SUPPORTED_BRIDGE_VERSIONS:
        return jsonify(error=f"Unsupported bridge contract {requested_version}; expected one of {sorted(SUPPORTED_BRIDGE_VERSIONS)}"), 426
    body = request.get_json(silent=True) or {}
    task_id = body.get("task_id")
    subtask_id = body.get("subtask_id")
    text = body.get("text")
    if not all(isinstance(value, str) and value.strip() for value in (task_id, subtask_id, text)):
        return jsonify(error="task_id, subtask_id, and non-empty text are required"), 400
    band_id = body.get("band_id")
    if isinstance(band_id, str) and band_id.strip():
        connected_band = ble_manager.address
        if connected_band is None:
            return jsonify(error="BandFlow is not connected"), 503
        if band_id.strip().lower() != connected_band:
            return jsonify(error="The wristband linked to this worker is not the one connected to this bridge."), 409
    assignment = {"task_id": task_id, "subtask_id": subtask_id, "text": text.strip()}
    with _state_lock:
        _current_assignment = assignment
        _write_json(STATE_PATH, _current_assignment)
    if ble_manager.loop is None or not ble_manager.thread.is_alive():
        return jsonify(error="BLE connection thread is not running"), 503
    future = asyncio.run_coroutine_threadsafe(ble_manager.send_task(assignment["text"]), ble_manager.loop)
    try:
        future.result(timeout=TASK_WRITE_TIMEOUT_SECONDS + 2)
    except FutureTimeoutError:
        future.cancel()
        return jsonify(error="Timed out sending task to BandFlow"), 504
    except RuntimeError as error:
        return jsonify(error=str(error)), 503
    except Exception as error:
        return jsonify(error=f"Could not send task: {error}"), 503
    return jsonify(status="sent", task_id=task_id, subtask_id=subtask_id, bridge_api_version=BRIDGE_API_VERSION)


@app.post("/task")
def legacy_task():
    """Compatibility endpoint for manual BLE testing; Node uses /v1/dispatch."""
    body = request.get_json(silent=True) or {}
    text = body.get("text")
    if not isinstance(text, str) or not text.strip():
        return jsonify(error="JSON body must contain a non-empty string 'text'"), 400
    if ble_manager.loop is None or not ble_manager.thread.is_alive():
        return jsonify(error="BLE connection thread is not running"), 503
    future = asyncio.run_coroutine_threadsafe(ble_manager.send_task(text.strip()), ble_manager.loop)
    try:
        future.result(timeout=TASK_WRITE_TIMEOUT_SECONDS + 2)
    except FutureTimeoutError:
        future.cancel()
        return jsonify(error="Timed out sending task to BandFlow"), 504
    except Exception as error:
        return jsonify(error=f"Could not send task: {error}"), 503
    return jsonify(status="sent")


start_ble_once()


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=int(os.environ.get("PORT", "5000")))
