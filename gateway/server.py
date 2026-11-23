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
from urllib.parse import urlsplit

import aiohttp
from aiohttp import web
import webrtcvad

from .opus import AudioCodec
from .provider import Provider
from .realtime import QwenRealtime

SYSTEM = {"role": "system", "content": "你是便携语音助手。使用用户的语言简短回答。用户提到歌单或播放列表时优先使用 playlist 管理或按名称播放歌单。用户点播指定歌曲时优先调用 play_file 查找并播放存储卡上的歌曲，无需用户说明“本地”。用户只要求播放音乐而未指定歌曲时，调用 control_music 的 resume_music 续播。播放故事用 resume_story，未指定章节时续播，指定章节时填 path。用户要求循环指定歌曲或故事时，用 control_music 的 repeat_music 或 repeat_story，path 填指定的歌名、书名或章节名。"}
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
    if isinstance(hello.get("features"), dict) and hello["features"].get("environment") is True:
        result["environment"] = True
    for key, max_bytes in (("model", 96), ("asr", 96), ("tts", 96), ("voice", 64)):
        value = values.get(key, "")
        if (not isinstance(value, str) or len(value.encode("utf-8")) > max_bytes
                or any(ord(char) < 33 or ord(char) == 127 for char in value)):
            raise ValueError("invalid model setting")
        result[key] = value
    if "protocol" in values:
        if values["protocol"] not in ("chat_completions", "responses", "anthropic", "qwen_realtime"):
            raise ValueError("unsupported model protocol")
        result["protocol"] = values["protocol"]
    controls = hello.get("voice_controls")
    if controls is not None:
        if not isinstance(controls, dict) or any(type(controls.get(key)) is not bool for key in ("music", "volume", "state")):
            raise ValueError("invalid voice controls")
        result["voice_controls"] = {key: controls[key] for key in ("music", "volume", "state")}
        if type(controls.get("network")) is bool:
            result["voice_controls"]["network"] = controls["network"]
        skills = controls.get("skills", [])
        if not isinstance(skills, list):
            raise ValueError("invalid voice skills")
        result["local_phrases"] = {s["phrase"] for s in skills if isinstance(s, dict) and
                                   isinstance(s.get("phrase"), str) and isinstance(s.get("action"), str)
                                   and result["voice_controls"].get("music" if s["action"] == "story" else s["action"])}
    return result


def music_call(call):
    name = call["function"]["name"]
    args = json.loads(call["function"]["arguments"])
    if name == "playlist" and isinstance(args, dict) and set(args) <= {"operation", "name", "track"}:
        op, title, track = (args.get(key, "") for key in ("operation", "name", "track"))
        if (op in ("list", "create", "delete", "add", "remove", "play")
                and isinstance(title, str) and len(title.encode()) <= 64 and (title.strip() or op == "list")
                and isinstance(track, str) and len(track.encode()) < 1024 and (track.strip() or op not in ("add", "remove"))):
            return "self.music.playlist", args
    song = name == "play_file" and isinstance(args, dict) and set(args) == {"path"}
    named_control = name == "control_music" and isinstance(args, dict) and set(args) == {"action", "path"} and args["action"] in (
        "resume_story", "repeat_music", "repeat_story"
    )
    if song or named_control:
        path = args["path"]
        if isinstance(path, str) and path.strip() and len(path.encode()) < 1024 and (
            "/" not in path or path.lower().endswith(".mp3") or args.get("action") == "repeat_story"
        ) and all(
            item not in ("", ".", "..") for item in path.split("/")
        ) and not any(ord(c) < 32 or c in "\\:" for c in path):
            return ("self.music.play_file" if song else "self.music.control"), args
    if name == "control_music" and isinstance(args, dict) and set(args) == {"action"} and args["action"] in (
        "resume_music", "resume_story", "sequential", "shuffle", "next", "stop"
    ):
        return "self.music.control", args
    if name in ("set_volume", "adjust_volume") and isinstance(args, dict):
        key = "volume" if name == "set_volume" else "delta"
        minimum = 0 if key == "volume" else -100
        if set(args) == {key} and type(args[key]) is int and minimum <= args[key] <= 100:
            return "self.audio_speaker." + name, args
    if name == "enter_local_mode" and args == {}:
        return "self.voice.enter_local_mode", args
    if name == "environment_status" and args == {}:
        return "self.environment.status", args
    if name == "radio_status" and args == {}:
        return "self.radio.status", args
    if name == "disable_wifi" and args == {}:
        return "self.radio.disable_wifi", args
    if name == "set_bluetooth" and isinstance(args, dict) and set(args) == {"enabled"} and type(args["enabled"]) is bool:
        return "self.radio.bluetooth", args
    if name == "open_setup" and args == {}:
        return "self.disp.network", args
    raise ValueError("invalid device command")


class Conversation:
    def __init__(self, socket, provider, settings):
        self.socket = socket
        self.provider = provider
        self.settings = settings
        self.id = uuid.uuid4().hex
        self.vad = webrtcvad.Vad(2)
        self.realtime = None
        if settings.get("protocol") == "qwen_realtime":
            if "realtime" not in provider.upstreams:
                raise ValueError("configure OMNI_REALTIME_URL on the gateway")
            endpoint, headers = provider.upstreams["realtime"]
            self.realtime = QwenRealtime(provider.client, endpoint, headers, settings)
        self.codec = AudioCodec()
        self.history = deque(maxlen=12)
        self.job = None
        self.listening = False
        self.speaking = False
        self.turn = 0
        self.pending_device_reply = None
        self.pending_control = None
        self.environment_request = None
        self.environment = None
        self.reset_input()

    def reset_input(self):
        self.pre = deque(maxlen=5)
        self.recording = bytearray()
        self.recorded_bytes = 0
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
        if self.recorded_bytes:
            pcm = self.recording
            size = self.recorded_bytes
            self.reset_input()
            await self.cancel()
            self.job = asyncio.create_task(self.respond(pcm, size))
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
        if speech or self.recorded_bytes:
            if not self.recorded_bytes:
                pcm = b"".join(self.pre) + pcm
                self.pre.clear()
            self.recorded_bytes += len(pcm)
            if self.realtime:
                await self.realtime.append(pcm)
            else:
                self.recording.extend(pcm)
            self.silence = 0 if speech else self.silence + 1
        else:
            self.pre.append(pcm)
        if self.silence >= 12 or self.recorded_bytes >= 30 * 32000 or (not self.recorded_bytes and self.frames >= 200):
            await self.finish()

    async def control(self, message):
        if not isinstance(message, dict):
            raise ValueError("expected control object")
        kind, state = message.get("type"), message.get("state")
        if kind == "mcp" and self.environment_request is not None:
            payload = message.get("payload", {})
            if payload.get("id") == self.environment_request:
                self.environment_request = None
                content = payload.get("result", {}).get("content", [])
                value = content[0].get("text") if content else None
                if isinstance(value, str) and re.fullmatch(
                    r"(?:时间待同步|[0-9]{4}年[0-9]{2}月[0-9]{2}日 [0-9]{2}:[0-9]{2})，"
                    r"(?:温湿度暂无读数|温度-?[0-9]{1,3}\.[0-9]摄氏度，相对湿度[0-9]{1,3}%)", value
                ):
                    self.environment = value
                return
        if kind == "mcp" and self.pending_device_reply is not None:
            result = message.get("payload", {})
            if result.get("id") == self.pending_device_reply:
                self.pending_device_reply = None
                content = result.get("result", {}).get("content", [])
                text = content[0].get("text", "") if content else "设备操作失败"
                text = text[:4096]
                await self.send("tts", state="start")
                await self.send("tts", state="sentence_start", text=text)
                await self.provider.speak(text, self.settings, self.codec, self.socket)
                await self.send("tts", state="stop")
        elif kind == "mcp" and self.pending_control is not None:
            result = message.get("payload")
            if isinstance(result, dict) and result.get("id") == self.pending_control and ("result" in result or "error" in result):
                self.pending_control = None
                await self.socket.close()
        elif kind == "listen" and state == "start":
            interrupted = self.listening or (self.job is not None and not self.job.done())
            await self.cancel()
            if self.realtime and interrupted:
                await self.realtime.close()
            self.speaking = False
            self.reset_input()
            self.listening = True
            if self.realtime and self.settings.get("environment"):
                self.turn += 1
                self.environment = None
                self.environment_request = self.turn
                await self.send("mcp", payload={"jsonrpc": "2.0", "id": self.turn, "method": "tools/call",
                    "params": {"name": "self.environment.status", "arguments": {}}})
        elif kind == "listen" and state == "stop" and self.listening:
            await self.finish()
        elif kind == "abort":
            await self.cancel()
            if self.realtime:
                await self.realtime.close()
            if self.speaking:
                self.speaking = False
                await self.send("tts", state="stop")
            self.listening = False
            self.reset_input()

    async def respond(self, pcm, size):
        try:
            if self.realtime:
                if size < 3200:
                    await self.realtime.append(bytes(3200-size))
                await self.realtime.reply(self)
                return
            text = await self.provider.transcribe(pcm, self.settings)
            if not text:
                await self.socket.close()
                return
            await self.send("stt", text=text)
            if text.rstrip("。！？.!?") in self.settings.get("local_phrases", ()):
                await self.socket.close()
                return
            user = {"role": "user", "content": text}
            answer = await self.provider.chat([SYSTEM if self.settings.get("voice_controls", {}).get("music", True)
                                               else {"role": "system", "content": "你是便携语音助手。使用用户的语言简短回答。"},
                                               *self.history, user], self.settings)
            calls = answer.get("tool_calls")
            if calls:
                name, arguments = music_call(calls[0])
                self.turn += 1
                if name in ("self.environment.status", "self.music.playlist", "self.radio.status", "self.radio.bluetooth"):
                    self.pending_device_reply = self.turn
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
            if self.realtime:
                await self.realtime.close()
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
        except (aiohttp.ClientError, ValueError, KeyError, TypeError, TimeoutError, ConnectionError):
            if socket.prepared and not socket.closed:
                await socket.send_json({"type":"alert","status":"语音服务","message":"请检查网关协议、模型与上游地址","emotion":"sad"})
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


def provider_upstreams(env):
    shared_url = env.get("OMNI_PROVIDER_URL", "").rstrip("/")
    routes = {}
    for stage in ("llm", "asr", "tts", "responses", "anthropic", "realtime"):
        prefix = "OMNI_" + stage.upper()
        optional = stage in ("responses", "anthropic", "realtime")
        if optional and not env.get(prefix + "_URL"):
            continue
        url = env.get(prefix + "_URL", shared_url).rstrip("/")
        if not url and (env.get("OMNI_REALTIME_URL") or
                        (stage == "llm" and (env.get("OMNI_RESPONSES_URL") or env.get("OMNI_ANTHROPIC_URL")))):
            continue
        parsed = urlsplit(url)
        secure, local = ("wss", "ws") if stage == "realtime" else ("https", "http")
        local_http = parsed.scheme == local and parsed.hostname in ("localhost", "127.0.0.1", "::1")
        if (not parsed.hostname or parsed.username is not None or parsed.query or parsed.fragment or
                (parsed.scheme != secure and not local_http)):
            raise ValueError(f"{prefix}_URL requires {secure} or loopback {local}")
        # Credentials may only fall back when the destination matches the shared endpoint.
        key = env.get(prefix + "_KEY", env.get("OMNI_PROVIDER_KEY", "") if url == shared_url else "")
        routes[stage] = (url, {"Authorization": "Bearer " + key} if key else {})
    return routes


def main():
    from contextlib import asynccontextmanager
    devices = json.loads(os.environ["OMNI_DEVICES"])
    if not isinstance(devices, dict) or not devices or not all(
        re.fullmatch(r"(?:[0-9a-f]{2}:){5}[0-9a-f]{2}", key) and isinstance(value, str) and len(value) >= 24
        for key, value in devices.items()
    ):
        raise ValueError("OMNI_DEVICES requires lowercase MAC addresses and device tokens of at least 24 characters")
    upstreams = provider_upstreams(os.environ)
    defaults = {key: os.getenv("OMNI_" + key.upper(), "") for key in ("asr", "tts", "voice")}

    @asynccontextmanager
    async def provider():
        async with aiohttp.ClientSession(cookie_jar=aiohttp.DummyCookieJar(), timeout=aiohttp.ClientTimeout(total=150, connect=10, sock_read=30)) as client:
            yield Provider(client, upstreams.get("llm", ("", {}))[0], defaults, upstreams=upstreams)

    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.minimum_version = ssl.TLSVersion.TLSv1_2
    tls.load_cert_chain(os.environ["OMNI_TLS_CERT"], os.environ["OMNI_TLS_KEY"])
    web.run_app(make_app(devices, provider), host=os.getenv("OMNI_BIND", "0.0.0.0"),
                port=int(os.getenv("OMNI_PORT", "8443")), ssl_context=tls, access_log=None)


if __name__ == "__main__":
    main()
