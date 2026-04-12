import logging
import random
from datetime import datetime, timezone
from pathlib import Path

import aiosqlite

logger = logging.getLogger("uvicorn.error")


class Database:
    def __init__(self, db_path: Path):
        self.db_path = db_path
        self.db = None

    async def init(self):
        self.db = await aiosqlite.connect(self.db_path)
        self.db.row_factory = aiosqlite.Row
        await self.db.executescript("""
            CREATE TABLE IF NOT EXISTS images (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                filename TEXT NOT NULL UNIQUE,
                uploaded_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS frames (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                mac_address TEXT NOT NULL UNIQUE,
                api_key TEXT NOT NULL,
                name TEXT,
                last_seen TEXT,
                battery_percent INTEGER,
                battery_mv INTEGER,
                charging INTEGER,
                usb_connected INTEGER,
                battery_connected INTEGER,
                sd_free_kb INTEGER,
                firmware_version TEXT,
                logs TEXT DEFAULT ''
            );
            CREATE TABLE IF NOT EXISTS settings (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS history (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                frame_id INTEGER NOT NULL,
                image_id INTEGER NOT NULL,
                shown_at TEXT NOT NULL,
                FOREIGN KEY (frame_id) REFERENCES frames(id),
                FOREIGN KEY (image_id) REFERENCES images(id)
            );
            CREATE TABLE IF NOT EXISTS frame_images (
                frame_id INTEGER NOT NULL,
                image_id INTEGER NOT NULL,
                PRIMARY KEY (frame_id, image_id),
                FOREIGN KEY (frame_id) REFERENCES frames(id),
                FOREIGN KEY (image_id) REFERENCES images(id)
            );
        """)
        await self.db.commit()

        # Migration: add per-frame wake interval columns if missing.
        await self._migrate_frame_wake_columns()

        # Migration: assign unassigned images to all existing frames.
        await self._migrate_assign_images()

    async def _migrate_frame_wake_columns(self):
        """Add wake_hours/minutes/seconds columns to frames if missing."""
        cursor = await self.db.execute("PRAGMA table_info(frames)")
        columns = {row["name"] for row in await cursor.fetchall()}
        if "wake_hours" not in columns:
            await self.db.execute("ALTER TABLE frames ADD COLUMN wake_hours INTEGER")
            await self.db.execute("ALTER TABLE frames ADD COLUMN wake_minutes INTEGER")
            await self.db.execute("ALTER TABLE frames ADD COLUMN wake_seconds INTEGER")
            await self.db.commit()
            logger.info("Migrated: added wake interval columns to frames table")

    async def _migrate_assign_images(self):
        """One-time migration: assign existing images to all frames.
        Only runs if the migration hasn't been done yet (tracked via settings)."""
        migrated = await self.get_setting("migration_assign_images_done")
        if migrated:
            return

        cursor = await self.db.execute(
            "SELECT id FROM images WHERE id NOT IN (SELECT DISTINCT image_id FROM frame_images)"
        )
        unassigned = [row["id"] for row in await cursor.fetchall()]
        if not unassigned:
            await self.set_setting("migration_assign_images_done", "1")
            return

        frames = await self.list_frames()
        if not frames:
            await self.set_setting("migration_assign_images_done", "1")
            return

        for image_id in unassigned:
            for frame in frames:
                await self.db.execute(
                    "INSERT OR IGNORE INTO frame_images (frame_id, image_id) VALUES (?, ?)",
                    (frame["id"], image_id),
                )
        await self.db.commit()
        await self.set_setting("migration_assign_images_done", "1")
        logger.info(f"Migrated: assigned {len(unassigned)} image(s) to {len(frames)} frame(s)")

    async def close(self):
        if self.db:
            await self.db.close()

    async def sync_images(self, images_dir: Path):
        """Add any image files on disk that aren't in the database."""
        existing = {img["filename"] for img in await self.list_images()}
        added = 0
        for path in sorted(images_dir.iterdir()):
            if path.is_file() and path.suffix.lower() in (".jpg", ".jpeg", ".png", ".webp"):
                if path.name not in existing:
                    await self.add_image(path.name)
                    added += 1
        if added:
            logger.info(f"Synced {added} image(s) from disk to database")

    # -- Images --

    async def add_image(self, filename: str) -> int:
        now = datetime.now(timezone.utc).isoformat()
        cursor = await self.db.execute(
            "INSERT INTO images (filename, uploaded_at) VALUES (?, ?)",
            (filename, now),
        )
        await self.db.commit()
        return cursor.lastrowid

    async def delete_image(self, image_id: int):
        await self.db.execute("DELETE FROM frame_images WHERE image_id = ?", (image_id,))
        await self.db.execute("DELETE FROM history WHERE image_id = ?", (image_id,))
        await self.db.execute("DELETE FROM images WHERE id = ?", (image_id,))
        await self.db.commit()

    async def list_images(self) -> list[dict]:
        cursor = await self.db.execute(
            "SELECT id, filename, uploaded_at FROM images ORDER BY uploaded_at DESC"
        )
        rows = await cursor.fetchall()
        return [dict(row) for row in rows]

    async def get_image_by_id(self, image_id: int) -> dict | None:
        cursor = await self.db.execute(
            "SELECT id, filename, uploaded_at FROM images WHERE id = ?", (image_id,)
        )
        row = await cursor.fetchone()
        return dict(row) if row else None

    # -- Frame-Image assignments --

    async def assign_image_to_frame(self, image_id: int, frame_id: int):
        await self.db.execute(
            "INSERT OR IGNORE INTO frame_images (frame_id, image_id) VALUES (?, ?)",
            (frame_id, image_id),
        )
        await self.db.commit()

    async def unassign_image_from_frame(self, image_id: int, frame_id: int):
        await self.db.execute(
            "DELETE FROM frame_images WHERE frame_id = ? AND image_id = ?",
            (frame_id, image_id),
        )
        await self.db.commit()

    async def assign_image_to_all_frames(self, image_id: int):
        """Assign an image to every known frame."""
        frames = await self.list_frames()
        for frame in frames:
            await self.db.execute(
                "INSERT OR IGNORE INTO frame_images (frame_id, image_id) VALUES (?, ?)",
                (frame["id"], image_id),
            )
        await self.db.commit()

    async def set_image_assignments(self, image_id: int, frame_ids: list[int]):
        """Replace all frame assignments for an image."""
        await self.db.execute(
            "DELETE FROM frame_images WHERE image_id = ?", (image_id,)
        )
        for fid in frame_ids:
            await self.db.execute(
                "INSERT OR IGNORE INTO frame_images (frame_id, image_id) VALUES (?, ?)",
                (fid, image_id),
            )
        await self.db.commit()

    async def get_image_assignments(self, image_id: int) -> list[int]:
        """Return list of frame_ids this image is assigned to."""
        cursor = await self.db.execute(
            "SELECT frame_id FROM frame_images WHERE image_id = ?", (image_id,)
        )
        rows = await cursor.fetchall()
        return [row["frame_id"] for row in rows]

    async def get_frame_images(self, frame_id: int) -> list[dict]:
        """Return images assigned to a specific frame."""
        cursor = await self.db.execute(
            """SELECT i.id, i.filename, i.uploaded_at
               FROM images i
               JOIN frame_images fi ON i.id = fi.image_id
               WHERE fi.frame_id = ?
               ORDER BY i.uploaded_at DESC""",
            (frame_id,),
        )
        rows = await cursor.fetchall()
        return [dict(row) for row in rows]

    # -- Frames --

    async def get_or_create_frame(self, mac_address: str, api_key: str) -> int:
        cursor = await self.db.execute(
            "SELECT id FROM frames WHERE mac_address = ?", (mac_address,)
        )
        row = await cursor.fetchone()
        if row:
            return row["id"]
        now = datetime.now(timezone.utc).isoformat()
        cursor = await self.db.execute(
            "INSERT INTO frames (mac_address, api_key, last_seen) VALUES (?, ?, ?)",
            (mac_address, api_key, now),
        )
        await self.db.commit()
        return cursor.lastrowid

    async def get_frame(self, frame_id: int) -> dict | None:
        cursor = await self.db.execute(
            "SELECT * FROM frames WHERE id = ?", (frame_id,)
        )
        row = await cursor.fetchone()
        return dict(row) if row else None

    async def list_frames(self) -> list[dict]:
        cursor = await self.db.execute(
            "SELECT * FROM frames ORDER BY last_seen DESC"
        )
        rows = await cursor.fetchall()
        return [dict(row) for row in rows]

    async def update_frame_name(self, frame_id: int, name: str):
        await self.db.execute(
            "UPDATE frames SET name = ? WHERE id = ?", (name, frame_id)
        )
        await self.db.commit()

    async def update_frame_wake_interval(self, frame_id: int,
                                          hours: int | None,
                                          minutes: int | None,
                                          seconds: int | None):
        await self.db.execute(
            """UPDATE frames SET wake_hours = ?, wake_minutes = ?, wake_seconds = ?
               WHERE id = ?""",
            (hours, minutes, seconds, frame_id),
        )
        await self.db.commit()

    async def get_frame_wake_interval(self, frame_id: int) -> dict | None:
        """Return per-frame wake interval, or None if not set."""
        frame = await self.get_frame(frame_id)
        if not frame or frame["wake_hours"] is None:
            return None
        return {
            "hours": frame["wake_hours"],
            "minutes": frame["wake_minutes"],
            "seconds": frame["wake_seconds"],
        }

    async def update_frame_status(self, frame_id: int, status: dict):
        now = datetime.now(timezone.utc).isoformat()
        await self.db.execute(
            """UPDATE frames SET
                last_seen = ?, battery_connected = ?, battery_percent = ?,
                battery_mv = ?, charging = ?, usb_connected = ?,
                sd_free_kb = ?, firmware_version = ?
            WHERE id = ?""",
            (
                now,
                status.get("battery_connected"),
                status.get("battery_percent"),
                status.get("battery_mv"),
                status.get("charging"),
                status.get("usb_connected"),
                status.get("sd_free_kb"),
                status.get("firmware_version"),
                frame_id,
            ),
        )
        await self.db.commit()

    # -- Logs --

    _LOG_MAX_BYTES = 512 * 1024

    async def append_logs(self, frame_id: int, text: str):
        await self.db.execute(
            "UPDATE frames SET logs = substr(logs || ?, ?) WHERE id = ?",
            (text, -self._LOG_MAX_BYTES, frame_id),
        )
        await self.db.commit()

    async def get_logs(self, frame_id: int) -> str:
        cursor = await self.db.execute(
            "SELECT logs FROM frames WHERE id = ?", (frame_id,)
        )
        row = await cursor.fetchone()
        return row["logs"] if row else ""

    # -- Settings --

    async def get_setting(self, key: str, default: str = "") -> str:
        cursor = await self.db.execute(
            "SELECT value FROM settings WHERE key = ?", (key,)
        )
        row = await cursor.fetchone()
        return row["value"] if row else default

    async def set_setting(self, key: str, value: str):
        await self.db.execute(
            "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)",
            (key, value),
        )
        await self.db.commit()

    async def get_wake_interval(self) -> dict:
        """Returns global {hours, minutes, seconds}. Default: 1h 0m 0s."""
        return {
            "hours": int(await self.get_setting("wake_interval_hours", "1")),
            "minutes": int(await self.get_setting("wake_interval_minutes", "0")),
            "seconds": int(await self.get_setting("wake_interval_seconds", "0")),
        }

    async def set_wake_interval(self, hours: int, minutes: int, seconds: int):
        await self.set_setting("wake_interval_hours", str(hours))
        await self.set_setting("wake_interval_minutes", str(minutes))
        await self.set_setting("wake_interval_seconds", str(seconds))

    # -- Shuffle --

    async def get_next_image(self, frame_id: int) -> dict | None:
        """Get next image assigned to this frame, with no-repeat shuffle."""
        assigned = await self.get_frame_images(frame_id)
        if not assigned:
            return None

        assigned_ids = {img["id"] for img in assigned}

        cursor = await self.db.execute(
            "SELECT image_id FROM history WHERE frame_id = ?", (frame_id,)
        )
        shown_rows = await cursor.fetchall()
        shown_ids = {row["image_id"] for row in shown_rows} & assigned_ids

        candidates = [img for img in assigned if img["id"] not in shown_ids]

        if not candidates:
            await self.db.execute(
                "DELETE FROM history WHERE frame_id = ?", (frame_id,)
            )
            await self.db.commit()
            candidates = assigned

        chosen = random.choice(candidates)
        now = datetime.now(timezone.utc).isoformat()
        await self.db.execute(
            "INSERT INTO history (frame_id, image_id, shown_at) VALUES (?, ?, ?)",
            (frame_id, chosen["id"], now),
        )
        await self.db.commit()
        return chosen
