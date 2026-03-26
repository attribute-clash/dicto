#!/usr/bin/env python3
"""PC emulator for one-button dictaphone firmware behavior.

Hold SPACE -> recording from PC microphone starts.
Release SPACE -> recording stops and WAV is saved.
Background uploader sends unsent WAV files to server.
"""

from __future__ import annotations

import os
import queue
import threading
import time
import wave
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

try:
    import sounddevice as sd
except Exception:  # pragma: no cover - optional dependency for runtime
    sd = None

try:
    from pynput import keyboard
except Exception:  # pragma: no cover - optional dependency for runtime
    keyboard = None


@dataclass(frozen=True)
class AppConfig:
    device_id: str
    server_url: str
    api_token: str
    sample_rate: int = 16_000
    channels: int = 1
    sample_width_bytes: int = 2
    max_record_seconds: int = 5 * 60
    audio_dir: Path = Path("./audio")
    upload_interval_sec: int = 5


def utc_iso_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def build_filename(device_id: str, sequence: int, now: Optional[datetime] = None) -> str:
    ts = (now or datetime.now(timezone.utc)).strftime("%Y%m%d_%H%M%S")
    return f"{device_id}_{ts}_{sequence}.wav"


def sent_marker(path: Path) -> Path:
    return path.with_suffix(path.suffix + ".sent")


def is_sent(path: Path) -> bool:
    return sent_marker(path).exists()


def mark_sent(path: Path) -> None:
    sent_marker(path).write_text("sent\n", encoding="utf-8")


def save_wav(path: Path, pcm_bytes: bytes, sample_rate: int, channels: int, sample_width_bytes: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(sample_width_bytes)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_bytes)


def upload_file(path: Path, cfg: AppConfig) -> bool:
    import requests

    headers = {
        "Authorization": f"Bearer {cfg.api_token}",
        "Content-Type": "audio/wav",
        "X-Device-Id": cfg.device_id,
        "X-Recorded-At": utc_iso_now(),
    }
    with path.open("rb") as fp:
        resp = requests.post(cfg.server_url, data=fp, headers=headers, timeout=15)
    if 200 <= resp.status_code < 300:
        mark_sent(path)
        return True
    return False


class DictaphoneEmulator:
    def __init__(self, cfg: AppConfig) -> None:
        self.cfg = cfg
        self.cfg.audio_dir.mkdir(parents=True, exist_ok=True)

        self.sequence = 0
        self.recording = False
        self.record_started = 0.0
        self._frames: list[bytes] = []
        self._lock = threading.Lock()
        self._event_queue: queue.Queue[str] = queue.Queue()
        self._running = True

        self._audio_stream = None

    def _on_audio(self, indata, frames, t, status) -> None:  # pragma: no cover - runtime callback
        if status:
            print(f"[audio] status={status}")
        if not self.recording:
            return
        with self._lock:
            self._frames.append(indata.tobytes())

        if time.time() - self.record_started >= self.cfg.max_record_seconds:
            self._event_queue.put("force_stop")

    def start_record(self) -> None:
        if self.recording:
            return
        self.recording = True
        self.record_started = time.time()
        with self._lock:
            self._frames = []
        print("[REC] start")

    def stop_record(self) -> Optional[Path]:
        if not self.recording:
            return None
        self.recording = False
        with self._lock:
            payload = b"".join(self._frames)
            self._frames = []

        filename = build_filename(self.cfg.device_id, self.sequence)
        self.sequence += 1
        path = self.cfg.audio_dir / filename
        save_wav(path, payload, self.cfg.sample_rate, self.cfg.channels, self.cfg.sample_width_bytes)
        print(f"[REC] stop -> {path}")
        return path

    def upload_tick(self) -> None:
        for wav in sorted(self.cfg.audio_dir.glob("*.wav")):
            if is_sent(wav):
                continue
            try:
                ok = upload_file(wav, self.cfg)
                print(f"[UP] {wav.name}: {'OK' if ok else 'FAIL'}")
                if not ok:
                    break
            except Exception as exc:
                print(f"[UP] {wav.name}: EXC {exc}")
                break

    def _keyboard_loop(self) -> None:  # pragma: no cover - runtime interaction
        if keyboard is None:
            raise RuntimeError("pynput is not installed")

        def on_press(key):
            try:
                if key == keyboard.Key.space:
                    self.start_record()
            except Exception:
                pass

        def on_release(key):
            try:
                if key == keyboard.Key.space:
                    self.stop_record()
                elif key == keyboard.Key.esc:
                    self._running = False
                    return False
            except Exception:
                pass
            return True

        with keyboard.Listener(on_press=on_press, on_release=on_release) as listener:
            while self._running:
                try:
                    event = self._event_queue.get(timeout=0.2)
                    if event == "force_stop":
                        self.stop_record()
                except queue.Empty:
                    pass
            listener.stop()

    def run(self) -> None:  # pragma: no cover - runtime interaction
        if sd is None:
            raise RuntimeError("sounddevice is not installed")

        self._audio_stream = sd.InputStream(
            samplerate=self.cfg.sample_rate,
            channels=self.cfg.channels,
            dtype="int16",
            callback=self._on_audio,
        )

        with self._audio_stream:
            uploader = threading.Thread(target=self._uploader_loop, daemon=True)
            uploader.start()
            print("Hold SPACE to record. Press ESC to exit.")
            self._keyboard_loop()

    def _uploader_loop(self) -> None:
        while self._running:
            self.upload_tick()
            time.sleep(self.cfg.upload_interval_sec)


def config_from_env() -> AppConfig:
    return AppConfig(
        device_id=os.environ.get("DEVICE_ID", "DCT-001-PC"),
        server_url=os.environ.get("SERVER_URL", "http://127.0.0.1:8080/api/v1/audio/upload"),
        api_token=os.environ.get("API_TOKEN", "dev-token"),
        sample_rate=int(os.environ.get("SAMPLE_RATE", "16000")),
        channels=int(os.environ.get("CHANNELS", "1")),
        max_record_seconds=int(os.environ.get("MAX_RECORD_SECONDS", str(5 * 60))),
        audio_dir=Path(os.environ.get("AUDIO_DIR", "./audio")),
        upload_interval_sec=int(os.environ.get("UPLOAD_INTERVAL_SEC", "5")),
    )


def main() -> None:  # pragma: no cover
    cfg = config_from_env()
    DictaphoneEmulator(cfg).run()


if __name__ == "__main__":  # pragma: no cover
    main()
