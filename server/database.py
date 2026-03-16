import random
from datetime import datetime, timezone
from pathlib import Path

import aiosqlite


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
                sd_free_kb INTEGER,
                firmware_version TEXT,
                logs TEXT DEFAULT ''
            );
            CREATE TABLE IF NOT EXISTS history (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                frame_id INTEGER NOT NULL,
                image_id INTEGER NOT NULL,
                shown_at TEXT NOT NULL,
                FOREIGN KEY (frame_id) REFERENCES frames(id),
                FOREIGN KEY (image_id) REFERENCES images(id)
            );
        """)
        await self.db.commit()

    async def close(self):
        if self.db:
            await self.db.close()

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

    async def update_frame_status(self, frame_id: int, status: dict):
        now = datetime.now(timezone.utc).isoformat()
        await self.db.execute(
            """UPDATE frames SET
                last_seen = ?, battery_percent = ?, battery_mv = ?,
                charging = ?, usb_connected = ?, sd_free_kb = ?,
                firmware_version = ?
            WHERE id = ?""",
            (
                now,
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

    async def append_logs(self, frame_id: int, text: str):
        await self.db.execute(
            "UPDATE frames SET logs = logs || ? WHERE id = ?",
            (text, frame_id),
        )
        await self.db.commit()

    async def get_logs(self, frame_id: int) -> str:
        cursor = await self.db.execute(
            "SELECT logs FROM frames WHERE id = ?", (frame_id,)
        )
        row = await cursor.fetchone()
        return row["logs"] if row else ""

    # -- Shuffle --

    async def get_next_image(self, frame_id: int) -> dict | None:
        all_images = await self.list_images()
        if not all_images:
            return None

        all_ids = {img["id"] for img in all_images}

        cursor = await self.db.execute(
            "SELECT image_id FROM history WHERE frame_id = ?", (frame_id,)
        )
        shown_rows = await cursor.fetchall()
        shown_ids = {row["image_id"] for row in shown_rows} & all_ids

        candidates = [img for img in all_images if img["id"] not in shown_ids]

        if not candidates:
            await self.db.execute(
                "DELETE FROM history WHERE frame_id = ?", (frame_id,)
            )
            await self.db.commit()
            candidates = all_images

        chosen = random.choice(candidates)
        now = datetime.now(timezone.utc).isoformat()
        await self.db.execute(
            "INSERT INTO history (frame_id, image_id, shown_at) VALUES (?, ?, ?)",
            (frame_id, chosen["id"], now),
        )
        await self.db.commit()
        return chosen
