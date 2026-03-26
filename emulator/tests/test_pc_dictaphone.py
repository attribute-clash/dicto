from datetime import datetime, timezone
from pathlib import Path
import wave

import emulator.pc_dictaphone as app


def test_build_filename_is_stable() -> None:
    now = datetime(2026, 3, 26, 12, 30, 45, tzinfo=timezone.utc)
    name = app.build_filename("DCT-001", 7, now=now)
    assert name == "DCT-001_20260326_123045_7.wav"


def test_sent_marker_roundtrip(tmp_path: Path) -> None:
    audio = tmp_path / "a.wav"
    audio.write_bytes(b"123")
    assert not app.is_sent(audio)
    app.mark_sent(audio)
    assert app.is_sent(audio)


def test_save_wav_creates_valid_pcm(tmp_path: Path) -> None:
    out = tmp_path / "test.wav"
    payload = (b"\x00\x00\xff\x7f") * 100
    app.save_wav(out, payload, sample_rate=16000, channels=1, sample_width_bytes=2)

    assert out.exists()
    with wave.open(str(out), "rb") as wf:
        assert wf.getnchannels() == 1
        assert wf.getframerate() == 16000
        assert wf.getsampwidth() == 2
        frames = wf.readframes(wf.getnframes())
        assert frames == payload


def test_upload_tick_skips_sent(tmp_path: Path, monkeypatch) -> None:
    cfg = app.AppConfig(device_id="DCT", server_url="http://localhost/u", api_token="t", audio_dir=tmp_path)
    emu = app.DictaphoneEmulator(cfg)

    wav_a = tmp_path / "a.wav"
    wav_b = tmp_path / "b.wav"
    wav_a.write_bytes(b"a")
    wav_b.write_bytes(b"b")
    app.mark_sent(wav_b)

    uploaded: list[Path] = []

    def fake_upload(path: Path, _cfg: app.AppConfig) -> bool:
        uploaded.append(path)
        app.mark_sent(path)
        return True

    monkeypatch.setattr(app, "upload_file", fake_upload)
    emu.upload_tick()

    assert uploaded == [wav_a]
    assert app.is_sent(wav_a)
    assert app.is_sent(wav_b)
