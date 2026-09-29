# Legacy database helpers

These Python SQLite helpers belong to the original development prototype.
Production task ownership now belongs to the Node server. The Flask BLE bridge
does not import these helpers or create task records; the Node startup migration
imports the old `tasks`/`subtasks` records into its canonical schema.
