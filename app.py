"""BandFlow BLE bridge.

Flask owns the BLE connection and a small durable event queue. Node owns users,
tasks, subtasks, progress, and the primary SQLite database.
"""

import asyncio
import json
import os
import threading
import urllib.error
import urllib.request
import uuid
from concurrent.futures import TimeoutError as FutureTimeoutError
from datetime import datetime, timezone
from pathlib import Path

from bleak import BleakClient, BleakScanner
from flask import Flask, jsonify, request

SERVICE_UUID = "12345678-1234-1234-1234-1234567890ab"
TASK_CHAR_UUID = "12345678-1234-1234-1234-1234567890ac"
STATUS_CHAR_UUID = "12345678-1234-1234-1234-1234567890ad"
TARGET_NAME = "BandFlow-Wristband"
BRIDGE_API_VERSION = "v1"

SCAN_SECONDS = 15.0
RECONNECT_DELAY_SECONDS = 5.0
TASK_WRITE_TIMEOUT_SECONDS = 10.0
NODE_EVENT_URL = os.environ.get("NODE_EVENT_URL", "http://127.0.0.1:8787/internal/ble/events")
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
        self._connected = None
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
            device = await self._find_band()
            if device is None:
                await asyncio.sleep(RECONNECT_DELAY_SECONDS)
                continue

            disconnected = asyncio.Event()

            def on_disconnect(_):
                print("BandFlow disconnected; scanning again...")
                self.loop.call_soon_threadsafe(disconnected.set)

            try:
                async with BleakClient(device, disconnected_callback=on_disconnect) as client:
                    self.client = client
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
                    print("Connected!")
                    await _flush_events()
                    await disconnected.wait()
                    await client.stop_notify(STATUS_CHAR_UUID)
            except Exception as error:
                print(f"BLE connection error: {error}; retrying...")
            finally:
                self.client = None
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

    async def send_task(self, text):
        if self._connected is None:
            raise RuntimeError("BandFlow is not connected")
        await asyncio.wait_for(self._connected.wait(), timeout=TASK_WRITE_TIMEOUT_SECONDS)
        if self.client is None or not self.client.is_connected:
            raise RuntimeError("BandFlow is not connected")
        await self.client.write_gatt_char(TASK_CHAR_UUID, text.encode("utf-8"), response=True)


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
    if requested_version and requested_version != BRIDGE_API_VERSION:
        return jsonify(error=f"Unsupported bridge contract {requested_version}; expected {BRIDGE_API_VERSION}"), 426
    body = request.get_json(silent=True) or {}
    task_id = body.get("task_id")
    subtask_id = body.get("subtask_id")
    text = body.get("text")
    if not all(isinstance(value, str) and value.strip() for value in (task_id, subtask_id, text)):
        return jsonify(error="task_id, subtask_id, and non-empty text are required"), 400
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
