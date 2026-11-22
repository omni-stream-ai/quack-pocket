"""Omni voice gateway. Run with python -m gateway.server."""
import asyncio
from collections import deque
from contextlib import suppress
import hmac
import json
import logging
import os
import re
import ssl
import uuid

import aiohttp
from aiohttp import web
import webrtcvad

from .opus import AudioCodec
from .provider import Provider

SYSTEM = {"role": "system", "content": "你是便携语音助手。使用用户的语言简短回答。用户点播指定歌曲时优先调用 play_file 查找并播放存储卡上的歌曲，无需用户说明“本地”。用户只要求播放音乐而未指定歌曲时，调用 control_music 顺序播放本地曲库。"}
LOG = logging.getLogger("omni.voice")


def model_settings(hello):
    if not isinstance(hello, dict) or hello.get("type") != "hello" or hello.get("version") != 1:
        raise ValueError("expected protocol version 1 hello")
    params = hello.get("audio_params")
    expected = {"format": "opus", "sample_rate": 16000, "frame_duration": 60, "channels": 1}
    if not isinstance(params, dict) or any(params.get(k) != v for k, v in expected.items()):
        raise ValueError("unsupported audio parameters")
    values = hello.get("model_settings")
    if not isinstance(values, dict) or not values.get("model"):
        raise ValueError("missing model settings")
    result = {}
    for key, max_bytes in (("model", 96), ("asr", 96), ("tts", 96), ("voice", 64)):
        value = values.get(key, "")
        if (not isinstance(value, str) or len(value.encode("utf-8")) > max_bytes
                or any(ord(char) < 33 or ord(char) == 127 for char in value)):
            raise ValueError("invalid model setting")
        result[key] = value
    return result


def music_call(call):
    name = call["function"]["name"]
    args = json.loads(call["function"]["arguments"])
    if name == "play_file" and isinstance(args, dict) and set(args) == {"path"}:
        path = args["path"]
        if isinstance(path, str) and 0 < len(path.encode()) < 1024 and ("/" not in path or path.lower().endswith(".mp3")) and all(
            item not in ("", ".", "..") for item in path.split("/")
        ) and not any(ord(c) < 32 or c in "\\:" for c in path):
            return "self.music.play_file", args
    if name == "control_music" and isinstance(args, dict) and set(args) == {"action"} and args["action"] in (
        "sequential", "shuffle", "next", "stop"
    ):
        return "self.music.control", args
    if name in ("set_volume", "adjust_volume") and isinstance(args, dict):
        key = "volume" if name == "set_volume" else "delta"
        minimum = 0 if key == "volume" else -100
        if set(args) == {key} and type(args[key]) is int and minimum <= args[key] <= 100:
            return "self.audio_speaker." + name, args
    if name == "enter_local_mode" and args == {}:
        return "self.voice.enter_local_mode", args
    if name == "open_setup" and args == {}:
        return "self.disp.network", args
    raise ValueError("invalid device command")


class Conversation:
    def __init__(self, socket, provider, settings):
        self.socket = socket
        self.provider = provider
        self.settings = settings
        self.id = uuid.uuid4().hex
        self.codec = AudioCodec()
        self.vad = webrtcvad.Vad(2)
        self.history = deque(maxlen=12)
        self.job = None
        self.listening = False
        self.speaking = False
        self.turn = 0
        self.pending_control = None
        self.reset_input()

    def reset_input(self):
        self.pre = deque(maxlen=5)
        self.recording = bytearray()
        self.silence = 0
        self.frames = 0

    async def send(self, kind, **fields):
        await self.socket.send_json({"session_id": self.id, "type": kind, **fields})

    async def cancel(self):
        if self.job:
            self.job.cancel()
            with suppress(asyncio.CancelledError):
                await self.job
            self.job = None

    async def finish(self):
        self.listening = False
        if self.recording:
            pcm = bytes(self.recording)
            self.reset_input()
            await self.cancel()
            self.job = asyncio.create_task(self.respond(pcm))
        else:
            await self.socket.close()

    async def process_audio(self, packet):
        if not self.listening:
            return
        if not 0 < len(packet) <= 1275:
            raise ValueError("oversized Opus frame")
        pcm = self.codec.pcm(packet)
        speech = any([self.vad.is_speech(pcm[:960], 16000), self.vad.is_speech(pcm[960:], 16000)])
        self.frames += 1
        if speech or self.recording:
            if not self.recording:
                self.recording.extend(b"".join(self.pre))
                self.pre.clear()
            self.recording.extend(pcm)
            self.silence = 0 if speech else self.silence + 1
        else:
            self.pre.append(pcm)
        if self.silence >= 12 or len(self.recording) >= 30 * 32000 or (not self.recording and self.frames >= 200):
            await self.finish()

    async def control(self, message):
        if not isinstance(message, dict):
            raise ValueError("expected control object")
        kind, state = message.get("type"), message.get("state")
        if kind == "mcp" and self.pending_control is not None:
            result = message.get("payload")
            if isinstance(result, dict) and result.get("id") == self.pending_control and ("result" in result or "error" in result):
                self.pending_control = None
                await self.socket.close()
        elif kind == "listen" and state == "start":
            await self.cancel()
            self.speaking = False
            self.reset_input()
            self.listening = True
        elif kind == "listen" and state == "stop" and self.listening:
            await self.finish()
        elif kind == "abort":
            await self.cancel()
            if self.speaking:
                self.speaking = False
                await self.send("tts", state="stop")
            self.listening = False
            self.reset_input()

    async def respond(self, pcm):
        try:
            text = await self.provider.transcribe(pcm, self.settings)
            if not text:
                await self.socket.close()
                return
            await self.send("stt", text=text)
            user = {"role": "user", "content": text}
            answer = await self.provider.chat([SYSTEM, *self.history, user], self.settings)
            calls = answer.get("tool_calls")
            if calls:
                name, arguments = music_call(calls[0])
                self.turn += 1
                if name.startswith("self.audio_speaker.") or arguments.get("action") == "stop":
                    self.pending_control = self.turn
                await self.send("mcp", payload={"jsonrpc": "2.0", "id": self.turn, "method": "tools/call",
                    "params": {"name": name, "arguments": arguments}})
                return
            text = answer.get("content")
            if not isinstance(text, str) or not text.strip() or len(text) > 4096:
                raise ValueError("empty or oversized model response")
            self.speaking = True
            await self.send("tts", state="start")
            await self.send("tts", state="sentence_start", text=text)
            await self.provider.speak(text, self.settings, self.codec, self.socket)
            await self.send("tts", state="stop")
            self.speaking = False
            self.history.extend((user, {"role": "assistant", "content": text}))
        except (aiohttp.ClientError, asyncio.TimeoutError, ValueError, KeyError, IndexError, TypeError) as error:
            LOG.warning("Voice turn failed (%s)", type(error).__name__)
            if not self.socket.closed:
                await self.send("alert", status="语音服务", message="对话失败，请检查网关和模型设置", emotion="sad")
                await self.socket.close()

    async def run(self):
        try:
            await self.send("hello", transport="websocket", audio_params={
                "format": "opus", "sample_rate": 24000, "channels": 1, "frame_duration": 60})
            async for msg in self.socket:
                if msg.type == aiohttp.WSMsgType.BINARY:
                    await self.process_audio(msg.data)
                elif msg.type == aiohttp.WSMsgType.TEXT:
                    await self.control(json.loads(msg.data))
                else:
                    break
        finally:
            await self.cancel()
            self.codec.close()


def make_app(devices, provider_factory):
    app = web.Application(client_max_size=8192)
    active = set()

    async def device(request):
        device_id = request.headers.get("Device-Id", "").lower()
        expected = devices.get(device_id)
        supplied = request.headers.get("Authorization", "")
        if not expected or not hmac.compare_digest(supplied.encode(), ("Bearer " + expected).encode()):
            raise web.HTTPUnauthorized()
        if device_id in active or len(active) >= 32:
            raise web.HTTPTooManyRequests()
        if request.headers.get("X-Omni-Diagnostic") == "auth":
            return web.Response(status=204)
        active.add(device_id)
        socket = web.WebSocketResponse(max_msg_size=8192, receive_timeout=90, heartbeat=25, compress=False)
        try:
            await socket.prepare(request)
            first = await socket.receive_str(timeout=10)
            settings = model_settings(json.loads(first))
            conversation = Conversation(socket, app[PROVIDER], settings)
            await conversation.run()
        except (ValueError, TypeError, TimeoutError, ConnectionError):
            if socket.prepared:
                await socket.close(code=1002)
        finally:
            active.remove(device_id)
        return socket

    async def context(application):
        async with provider_factory() as provider:
            application[PROVIDER] = provider
            yield

    app.cleanup_ctx.append(context)
    app.router.add_get("/device", device)
    return app


PROVIDER = web.AppKey("provider", Provider)


def main():
    from contextlib import asynccontextmanager
    devices = json.loads(os.environ["OMNI_DEVICES"])
    if not isinstance(devices, dict) or not devices or not all(
        re.fullmatch(r"(?:[0-9a-f]{2}:){5}[0-9a-f]{2}", key) and isinstance(value, str) and len(value) >= 24
        for key, value in devices.items()
    ):
        raise ValueError("OMNI_DEVICES requires lowercase MAC addresses and device tokens of at least 24 characters")
    base = os.environ["OMNI_PROVIDER_URL"]
    if not base.startswith("https://"):
        raise ValueError("OMNI_PROVIDER_URL must be HTTPS")
    defaults = {key: os.environ["OMNI_" + key.upper()] for key in ("asr", "tts", "voice")}
    auth = {"Authorization": "Bearer " + os.environ["OMNI_PROVIDER_KEY"]}

    @asynccontextmanager
    async def provider():
        async with aiohttp.ClientSession(headers=auth, timeout=aiohttp.ClientTimeout(total=150, connect=10, sock_read=30)) as client:
            yield Provider(client, base, defaults)

    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.minimum_version = ssl.TLSVersion.TLSv1_2
    tls.load_cert_chain(os.environ["OMNI_TLS_CERT"], os.environ["OMNI_TLS_KEY"])
    web.run_app(make_app(devices, provider), host=os.getenv("OMNI_BIND", "0.0.0.0"),
                port=int(os.getenv("OMNI_PORT", "8443")), ssl_context=tls, access_log=None)


if __name__ == "__main__":
    main()
