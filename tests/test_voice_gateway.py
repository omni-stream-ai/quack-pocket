import asyncio
import ctypes as c
from contextlib import asynccontextmanager
import io
import json
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


async def record_turn(socket, encoder):
    await socket.send_json({"type": "listen", "state": "start"})
    frame = (c.c_int16 * 960).from_buffer_copy(pcm(16000, .06))
    encoded = c.create_string_buffer(1275)
    for _ in range(10):
        n = opus.encode(encoder, frame, 960, encoded, len(encoded))
        if n <= 0:
            raise AssertionError(f"Opus encoder returned {n}")
        await socket.send_bytes(encoded.raw[:n])
    await socket.send_json({"type": "listen", "state": "stop"})


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
            ("control_music", {"action":"repeat_music","path":"小星星"}, "self.music.control"),
            ("control_music", {"action":"repeat_story","path":"故事/格林童话"}, "self.music.control"),
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

    def test_named_repeat_requires_valid_path(self):
        import json
        for args in ({"action": "repeat_music"}, {"action": "repeat_story", "path": ""},
                     {"action": "repeat_story", "path": "故事/../其他"}, {"action": "repeat_music", "path": "音乐/专辑"}):
            with self.subTest(args=args), self.assertRaises(ValueError):
                music_call({"function": {"name": "control_music", "arguments": json.dumps(args)}})

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

    def test_reminder_arguments(self):
        examples = ({"operation": "set", "hour": 9, "date": "tomorrow", "message": "吃早餐"},
                    {"operation": "set", "hour": 7, "daily": True}, {"operation": "list"},
                    {"operation": "cancel", "id": 1}, {"operation": "cancel", "message": "吃早餐"})
        for arguments in examples:
            call = {"function": {"name": "manage_reminder", "arguments": json.dumps(arguments)}}
            self.assertEqual(music_call(call), ("self.reminder.manage", arguments))
        for arguments in ({"operation": "set"}, {"operation": "set", "hour": True},
                          {"operation": "set", "hour": 24}, {"operation": "cancel", "id": -1}):
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                music_call({"function": {"name": "manage_reminder", "arguments": json.dumps(arguments)}})


class VoiceRoundTrip(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.calls = []
        self.music = False
        self.transcript = None
        self.device_call = None
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
            return web.json_response({"text": self.transcript or ("播放音乐/示例.mp3" if self.music else "试音")})

        async def model(req):
            self.assertEqual(req.headers.get("Authorization"), "Bearer llm-key")
            data = await req.json()
            self.histories.append(data["messages"][1:-1])
            self.calls.append(data["model"])
            if self.device_call:
                name, arguments = self.device_call
                self.assertTrue(any(tool["function"]["name"] == name for tool in data["tools"]))
                return web.json_response({"choices": [{"message": {"tool_calls": [{"function": {
                    "name": name, "arguments": json.dumps(arguments)}}]}}]})
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

    async def test_spoken_breakfast_reminder(self):
        self.release_first_speech.set()
        self.transcript = "明天早上9点提醒我吃早餐"
        arguments = {"operation": "set", "hour": 9, "date": "tomorrow", "message": "吃早餐"}
        self.device_call = "manage_reminder", arguments
        headers = {"Device-Id": MAC, "Authorization": "Bearer " + TOKEN}
        async with await self.client.ws_connect("/device", headers=headers) as socket:
            await socket.send_json({"version": 1, "type": "hello", "features": {"reminders": True},
                "audio_params": {"format": "opus", "sample_rate": 16000, "frame_duration": 60, "channels": 1},
                "model_settings": {"model": "local/chat", "asr": "asr-a", "tts": "tts-b", "voice": "晓晓"}})
            self.assertEqual((await socket.receive_json(timeout=2))["type"], "hello")
            error = c.c_int()
            encoder = opus.new_encoder(16000, 1, 2048, c.byref(error))
            self.assertTrue(encoder)
            self.addCleanup(opus.free_encoder, encoder)
            await record_turn(socket, encoder)
            self.assertEqual((await socket.receive_json(timeout=2))["text"], self.transcript)
            call = (await socket.receive_json(timeout=2))["payload"]
            self.assertEqual(call["params"], {"name": "self.reminder.manage", "arguments": arguments})
            result = "已设置 1 号，2026年11月25日 09:00，提醒吃早餐。"
            await socket.send_json({"type": "mcp", "payload": {"id": call["id"], "result": {"content": [
                {"type": "text", "text": result}]}}})
            self.assertEqual((await socket.receive_json(timeout=2))["state"], "start")
            self.assertEqual((await socket.receive_json(timeout=2))["text"], result)
            for _ in range(2):
                self.assertEqual((await socket.receive(timeout=2)).type, WSMsgType.BINARY)
            self.assertEqual((await socket.receive_json(timeout=2))["state"], "stop")
            self.transcript = "取消1号"
            cancel = {"operation": "cancel", "id": 1}
            self.device_call = "manage_reminder", cancel
            await record_turn(socket, encoder)
            self.assertEqual((await socket.receive_json(timeout=2))["text"], self.transcript)
            call = (await socket.receive_json(timeout=2))["payload"]
            self.assertEqual(call["params"], {"name": "self.reminder.manage", "arguments": cancel})

    async def test_media_choices_stay_in_the_same_call(self):
        self.release_first_speech.set()
        headers = {"Device-Id": MAC, "Authorization": "Bearer " + TOKEN}
        async with await self.client.ws_connect("/device", headers=headers) as socket:
            await socket.send_json({"version": 1, "type": "hello", "audio_params": {"format": "opus", "sample_rate": 16000,
                "frame_duration": 60, "channels": 1}, "model_settings": {
                    "model": "local/chat", "asr": "asr-a", "tts": "tts-b", "voice": "晓晓"},
                "features": {"media_choice": True}, "voice_controls": {"music": True, "volume": True, "state": True,
                    "skills": [{"phrase": "睡前时间", "action": "story", "argument": "格林童话"}]}})
            self.assertEqual((await socket.receive_json(timeout=2))["type"], "hello")
            error = c.c_int()
            encoder = opus.new_encoder(16000, 1, 2048, c.byref(error))
            self.assertTrue(encoder)
            self.addCleanup(opus.free_encoder, encoder)

            async def request(phrase, name, arguments, model=None, local=True):
                self.transcript, self.device_call = phrase, model
                await record_turn(socket, encoder)
                stt = await socket.receive_json(timeout=2)
                self.assertEqual(stt["text"], phrase)
                self.assertEqual(stt.get("handle_locally", True), local)
                call = (await socket.receive_json(timeout=2))["payload"]
                self.assertEqual(call["params"], {"name": name, "arguments": arguments})
                return call["id"]

            async def reply(turn, status, prompt):
                await socket.send_json({"type": "mcp", "payload": {"id": turn, "result": {"content": [
                    {"type": "text", "text": json.dumps({"status": status, "prompt": prompt})}]}}})
                self.assertEqual((await socket.receive_json(timeout=2))["state"], "start")
                self.assertEqual((await socket.receive_json(timeout=2))["text"], prompt)
                for _ in range(2):
                    self.assertEqual((await socket.receive(timeout=2)).type, WSMsgType.BINARY)
                self.assertEqual((await socket.receive_json(timeout=2))["state"], "stop")

            song = {"path": "晴天"}
            turn = await request("播放晴天", "self.music.play_file", song, ("play_file", song))
            await reply(turn, "choose", "第一项，原版。第二项，钢琴版。")
            turn = await request("播放第二首。", "self.music.select", {"choice": "播放第二首"})
            await reply(turn, "choose", "请再选一次。")
            turn = await request("取消", "self.music.select", {"choice": "取消"})
            await reply(turn, "message", "已取消选择")

            story = {"action": "resume_story", "path": "格林童话"}
            turn = await request("睡前时间", "self.music.control", story, local=False)
            await reply(turn, "choose", "第一项，原版。第二项，少儿版。")
            turn = await request("少儿版", "self.music.select", {"choice": "少儿版"})
            await reply(turn, "choose", "请再说一个关键词。")
            turn = await request("播放晴天", "self.music.play_file", song, ("play_file", song))
            await reply(turn, "retry", "没有找到这首歌，请再说一次歌名。")
            turn = await request("晴天", "self.music.select", {"choice": "晴天"})
            await socket.send_json({"type": "mcp", "payload": {"id": turn, "result": {"content": [
                {"type": "text", "text": "true"}]}}})
            self.assertEqual(self.calls.count("local/chat"), 2)

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
