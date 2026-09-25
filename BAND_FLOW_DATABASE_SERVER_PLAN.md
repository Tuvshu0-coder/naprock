# BandFlow Database and Server Plan

## Goal

Create one reliable system where:

```text
Mobile app
    -> Node API server
    -> SQLite database
    -> Flask BLE bridge
    -> ESP32 wristband
```

The mobile app communicates with the server through HTTP. It does not access SQLite directly.

## Database Ownership

The **Node server owns the main application database**.

It stores:

- Users and sessions
- Workers
- Work assignments
- Tasks and subtasks
- Deadlines and priorities
- Eisenhower Matrix categories
- Progress and completion events
- Voice-task records
- Synchronization records

The Flask server should not maintain a separate competing task database in production.

The existing Python `tasks` and `subtasks` tables should eventually be migrated into the Node database or replaced by equivalent structured tables. The team should choose one final schema before production integration.

## Server Responsibilities

### Node server

The Node server is the main application API. It handles:

- Authentication
- User and worker accounts
- Boss work assignment
- Task creation
- AI task breakdown requests
- AI Eisenhower Matrix classification
- Task and subtask database operations
- Mobile app API responses
- Requests to send tasks through the Flask BLE bridge
- Completion events received from the Flask BLE bridge

### Flask server

The Flask server is the BLE bridge. It handles:

- Persistent BLE connection to the ESP32
- Sending task text to the wristband
- Receiving `DONE` notifications
- Reconnecting after BLE disconnection
- Offline event queueing when needed
- A small internal API for the Node server

Flask should not own mobile authentication, user management, or the primary task database.

## Work Assignment Flow

```text
Mobile app
    -> POST /work
    -> Node validates and stores the work
    -> Node calls the AI API
    -> AI returns structured subtasks
    -> Node stores the subtasks
    -> Node sends the first subtask to Flask
    -> Flask sends it over BLE
    -> ESP32 displays it
```

## Completion Flow

```text
Worker taps DONE
    -> ESP32 sends DONE over BLE
    -> Flask receives the notification
    -> Flask sends a completion event to Node
    -> Node marks the subtask done
    -> Node finds the next available subtask
    -> Node sends the next subtask to Flask
    -> Flask sends it to the wristband
```

## AI Task Breakdown

AI should return structured data, not loose text:

```json
{
  "title": "Install electrical equipment",
  "subtasks": [
    {
      "description": "Inspect the installation area",
      "order_index": 1
    },
    {
      "description": "Prepare the required tools",
      "order_index": 2,
      "depends_on_order_index": 1
    }
  ]
}
```

The server must validate the AI response before saving it. AI output must not be inserted into the database without validation.

## Eisenhower Matrix

The AI can classify work using these values:

```text
do_first
schedule
delegate
eliminate
```

The server validates and stores the category. The mobile app displays the result.

## Recommended Subtask Fields

```text
id
task_id
description
status
depends_on
order_index
started_at
completed_at
created_at
```

Status values should be controlled, for example:

```text
pending
active
done
```

Database rules:

- Use parameterized SQL.
- Enable SQLite foreign keys.
- Use transactions for multi-step operations.
- Roll back the complete operation if one part fails.
- Store timestamps in UTC.
- Keep stable IDs for tasks, subtasks, and events.
- Record `started_at` when a subtask becomes active.
- Preserve `started_at` after completion.

## Offline Synchronization

The wristband should continue working if it temporarily loses BLE range.

It should queue events such as:

```text
task_received
task_started
task_completed
voice_recorded
```

Each event should include:

```text
event_id
task_id
subtask_id
event_type
created_at
sync_status
```

When BLE reconnects:

```text
Wristband sends queued events
    -> Flask receives them
    -> Node processes them
    -> Node acknowledges each event_id
    -> Wristband marks acknowledged events as synced
```

The server must safely ignore duplicate `event_id` values because a queued event may be sent more than once.

## Voice Task Flow

```text
User speaks into the wristband
    -> ESP32 records audio
    -> Audio is stored locally if BLE is unavailable
    -> Audio reaches the Raspberry Pi
    -> Pi sends audio to the Whisper API
    -> Whisper returns text
    -> Node creates a task from the text
    -> AI breaks the task into subtasks
```

Large audio files should normally be stored as files, not directly inside SQLite. SQLite should store the file ID, path, and processing status.

## Architecture Decision

The team should agree on this rule:

> Node owns the application database. Flask owns BLE communication. Flask does not independently create competing task records in production.

The existing Flask database helpers can remain useful during development and testing, but the production system should have one source of truth.

## Immediate Next Steps

1. Agree that Node is the primary database owner.
2. Compare the Node `work_items` model with the Python `tasks/subtasks` model.
3. Choose one final schema.
4. Add structured subtasks and dependency fields to the Node database.
5. Define the internal Node-to-Flask API contract.
6. Connect `/work` creation to AI task breakdown.
7. Connect Flask `DONE` events back to Node.
8. Add offline event queueing after the online flow is stable.
9. Add voice input and Whisper integration.
10. Add Eisenhower Matrix classification.
