import asyncio
import ctypes as c
from contextlib import asynccontextmanager
import io
import math
import struct
import unittest
import wave

from aiohttp import ClientSession, WSMsgType, web
from aiohttp.test_utils import TestClient, TestServer
from gateway import opus
from gateway.provider import Provider
from gateway.server import make_app, model_settings, music_call, provider_upstreams

MAC = "10:51:db:39:8f:74"
TOKEN = "test-device-secret-used-only-locally"


def pcm(rate, duration):
    count = int(rate * duration)
    return struct.pack("<" + "h" * count, *(int(8000 * math.sin(i * 2 * math.pi * 180 / rate)) for i in range(count)))


class ModelSettings(unittest.TestCase):
    def test_provider_credentials_follow_destination(self):
        routes = provider_upstreams({"OMNI_PROVIDER_URL": "https://shared.test/v1",
                                    "OMNI_PROVIDER_KEY": "shared-key",
                                    "OMNI_ASR_URL": "http://127.0.0.1:9999/v1",
                                    "OMNI_TTS_URL": "https://speech.test/v1", "OMNI_TTS_KEY": "tts-key"})
        self.assertEqual(routes["llm"][1], {"Authorization": "Bearer shared-key"})
        self.assertEqual(routes["asr"][1], {})
        self.assertEqual(routes["tts"][1], {"Authorization": "Bearer tts-key"})

    def test_firmware_utf8_limits(self):
        settings = {"model": "语" * 32, "voice": "音" * 21 + "A"}
        hello = {"type": "hello", "version": 1, "audio_params": {
            "format": "opus", "sample_rate": 16000, "frame_duration": 60, "channels": 1},
            "model_settings": settings}
        self.assertEqual(model_settings(hello), {**settings, "asr": "", "tts": ""})
        phrases = ["口令" + str(i) for i in range(80)]
        controls = dict(music=True, volume=True, state=True, skills=[{"phrase": p, "action": "state"} for p in phrases])
        self.assertEqual(model_settings({**hello, "voice_controls": controls})["local_phrases"], set(phrases))
        for key, invalid in (("model", settings["model"] + "A"), ("voice", settings["voice"] + "A"),
                             ("tts", "bad\nname"), ("asr", "has space"), ("voice", 7)):
            with self.subTest(key=key, invalid=invalid), self.assertRaises(ValueError):
                model_settings({**hello, "model_settings": {**settings, key: invalid}})


class MusicCommands(unittest.TestCase):
    def test_volume_and_setup(self):
        import json
        for name, args, target in (
            ("control_music", {"action":"resume_music"}, "self.music.control"),
            ("control_music", {"action":"resume_story"}, "self.music.control"),
            ("control_music", {"action":"resume_story","path":"第一章"}, "self.music.control"),
            ("set_volume", {"volume": 20}, "self.audio_speaker.set_volume"),
            ("adjust_volume", {"delta": -10}, "self.audio_speaker.adjust_volume"),
            ("enter_local_mode", {}, "self.voice.enter_local_mode"),
            ("open_setup", {}, "self.disp.network"),
            ("radio_status", {}, "self.radio.status"),
            ("set_bluetooth", {"enabled": False}, "self.radio.bluetooth"),
            ("disable_wifi", {}, "self.radio.disable_wifi"),
        ):
            self.assertEqual(music_call({"function": {"name": name, "arguments": json.dumps(args)}}), (target, args))
        for bad in (-1, 101, True):
            with self.assertRaises(ValueError):
                music_call({"function": {"name": "set_volume", "arguments": json.dumps({"volume": bad})}})

    def test_playlist_commands(self):
        import json
        for args in ({"operation": "list"}, {"operation": "create", "name": "晨练"},
                     {"operation": "add", "name": "晨练", "track": "晴天"},
                     {"operation": "play", "name": "晨练"}):
            call = {"function": {"name": "playlist", "arguments": json.dumps(args)}}
            self.assertEqual(music_call(call), ("self.music.playlist", args))
        for args in ({"operation": "play"}, {"operation": "remove", "name": "晨练"}):
            with self.assertRaises(ValueError):
                music_call({"function": {"name": "playlist", "arguments": json.dumps(args)}})

    def test_spoken_title(self):
        call = {"function": {"name": "play_file", "arguments": '{"path":"月亮代表我的心"}'}}
        self.assertEqual(music_call(call), ("self.music.play_file", {"path": "月亮代表我的心"}))
        call["function"]["arguments"] = '{"path":"../月亮代表我的心.mp3"}'
        with self.assertRaises(ValueError):
            music_call(call)


class VoiceRoundTrip(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.calls = []
        self.music = False
        self.histories = []
        self.release_first_speech = asyncio.Event()
        app = web.Application()

        async def asr(req):
            self.assertEqual(req.headers.get("Authorization"), "Bearer asr-key")
            form = await req.post()
            with wave.open(io.BytesIO(form["file"].file.read())) as audio:
                self.assertEqual((audio.getframerate(), audio.getnchannels()), (16000, 1))
                self.assertGreater(audio.getnframes(), 0)
            self.calls.append(form["model"])
            return web.json_response({"text": "播放音乐/示例.mp3" if self.music else "试音"})

        async def model(req):
            self.assertEqual(req.headers.get("Authorization"), "Bearer llm-key")
            data = await req.json()
            self.histories.append(data["messages"][1:-1])
            self.calls.append(data["model"])
            self.assertEqual("tools" in data, self.music)
            reply = {"content": "收到"} if not self.music else {"tool_calls": [{"function": {
                "name": "play_file", "arguments": '{"path":"音乐/示例.mp3"}'}}]}
            return web.json_response({"choices": [{"message": reply}]})

        async def speech(req):
            self.assertEqual(req.headers.get("Authorization"), "Bearer tts-key")
            data = await req.json()
            self.calls.extend([data["model"], data["voice"]])
            if len(self.histories) == 1:
                await self.release_first_speech.wait()
            return web.Response(body=pcm(24000, 0.12))

        app.router.add_post("/asr/v1/audio/transcriptions", asr)
        app.router.add_post("/llm/v1/chat/completions", model)
        app.router.add_post("/tts/v1/audio/speech", speech)
        self.upstream = TestServer(app)
        await self.upstream.start_server()

        @asynccontextmanager
        async def provider():
            async with ClientSession() as client:
                routes = {stage: (str(self.upstream.make_url(f"/{stage}/v1")),
                                  {"Authorization": f"Bearer {stage}-key"}) for stage in ("llm","asr","tts")}
                yield Provider(client, "", {}, upstreams=routes)

        self.client = TestClient(TestServer(make_app({MAC: TOKEN}, provider)))
        await self.client.start_server()

    async def asyncTearDown(self):
        self.release_first_speech.set()
        await self.client.close()
        await self.upstream.close()

    async def test_device_auth_diagnostic(self):
        headers = {"Device-Id": MAC, "X-Omni-Diagnostic": "auth"}
        for token, expected in (("incorrect", 401), (TOKEN, 204)):
            response = await self.client.get("/device", headers={**headers, "Authorization": "Bearer " + token})
            self.assertEqual(response.status, expected)

    async def test_model_and_music(self):
        response = await self.client.get("/device")
        self.assertEqual(response.status, 401)
        socket = await self.client.ws_connect("/device", headers={"Device-Id": MAC, "Authorization": "Bearer " + TOKEN})
        async with socket:
            await socket.send_json({"version": 1, "type": "hello", "audio_params": {"format": "opus", "sample_rate": 16000,
                "frame_duration": 60, "channels": 1}, "model_settings": {
                    "model": "local/chat", "asr": "asr-a", "tts": "tts-b", "voice": "晓晓"}})
            self.assertEqual((await socket.receive_json(timeout=2))["type"], "hello")
            err = c.c_int()
            encoder = opus.new_encoder(16000, 1, 2048, c.byref(err))
            decoder = opus.new_decoder(24000, 1, c.byref(err))
            self.assertTrue(encoder and decoder)
            try:
                for turn, music in enumerate((False, False, True)):
                    self.music = music
                    await socket.send_json({"type": "listen", "state": "start"})
                    for i in range(10):
                        frame = (c.c_int16 * 960).from_buffer_copy(pcm(16000, .06))
                        encoded = c.create_string_buffer(1275)
                        n = opus.encode(encoder, frame, 960, encoded, len(encoded))
                        self.assertGreater(n, 0)
                        await socket.send_bytes(encoded.raw[:n])
                    await socket.send_json({"type": "listen", "state": "stop"})
                    self.assertEqual((await socket.receive_json(timeout=2))["text"], "播放音乐/示例.mp3" if music else "试音")
                    if music:
                        call = (await socket.receive_json(timeout=2))["payload"]["params"]
                        self.assertEqual(call, {"name": "self.music.play_file", "arguments": {"path": "音乐/示例.mp3"}})
                    else:
                        self.assertEqual((await socket.receive_json(timeout=2))["state"], "start")
                        self.assertEqual((await socket.receive_json(timeout=2))["text"], "收到")
                        if turn == 0:
                            await socket.send_json({"type": "abort"})
                            while True:
                                packet = await socket.receive(timeout=2)
                                if packet.type == WSMsgType.BINARY:
                                    continue
                                self.assertEqual(packet.json()["state"], "stop")
                                self.release_first_speech.set()
                                break
                            continue
                        for _ in range(2):
                            msg = await socket.receive(timeout=2)
                            self.assertEqual(msg.type, WSMsgType.BINARY)
                            samples = (c.c_int16 * 1440)()
                            self.assertEqual(opus.decode(decoder, msg.data, len(msg.data), samples, 1440, 0), 1440)
                            self.assertGreater(max(samples), 1000)
                        self.assertEqual((await socket.receive_json(timeout=2))["state"], "stop")
            finally:
                opus.free_encoder(encoder)
                opus.free_decoder(decoder)
            self.assertEqual(self.histories, [[], [], [
                {"role": "user", "content": "试音"}, {"role": "assistant", "content": "收到"}]])
            self.assertEqual([value for value in self.calls if value == "local/chat"], ["local/chat"] * 3)
            self.assertIn("晓晓", self.calls)
            self.assertIn("tts-b", self.calls)
            self.assertEqual(self.calls.count("asr-a"), 3)


if __name__ == "__main__":
    unittest.main()
