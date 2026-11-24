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
from .provider import Provider, REMINDER_REQUEST
from .realtime import QwenRealtime

SYSTEM = {"role": "system", "content": "你是便携语音助手。使用用户的语言简短回答。用户提到歌单或播放列表时优先使用 playlist 管理或按名称播放歌单。用户点播指定歌曲时优先调用 play_file 查找并播放存储卡上的歌曲，无需用户说明“本地”。用户只要求播放音乐而未指定歌曲时，调用 control_music 的 resume_music 续播。播放故事用 resume_story，指定书名或章节名时填 path，否则续播。用户要求循环指定歌曲或故事时，用 control_music 的 repeat_music 或 repeat_story，path 填指定的歌名、书名或章节名。"}
LOG = logging.getLogger("omni.voice")
REMINDER_INSTRUCTION = "用户要求设置、查询或取消闹钟和定时提醒时，使用设备工具。时间不明确时先询问。只根据设备返回的结果确认操作。"

_NEW_MEDIA_REQUEST = re.compile(
    r"^(?:请|請|帮我|幫我|小鸭[，, ]*)?(?:播放|点播|點播|点歌|點歌|循环|循環|换|換|"
    r"放一?[首曲]|来一首|來一首|讲一?[个本段]|講一?[個本段]|听故事|聽故事|续播|續播|"
    r"停止|停播|停歌|(?:退出|结束|結束|关闭|關閉).{0,4}(?:语音|語音|通话|通話)|"
    r"(?:返回|回到|回)首页|(?:play|stop|shuffle|skip)\b)", re.I,
)
_NUMBERED_CHOICE = re.compile(
    r"^(?:(?:请|請)?(?:播放|我想听|我想聽|我要听|我要聽|选|選|播|听|聽)\s*)?"
    r"第?[一二两兩三四五六七八九1-9１-９](?:个|個|项|項|首|本|号|號)?$",
)
_OTHER_DEVICE_REQUEST = re.compile(
    r"^(?:请|請|帮我|幫我)?(?:音量|调.{0,3}音量|調.{0,3}音量|[大小][声聲]一点|[静靜]音|"
    r"(?:打开|打開|关闭|關閉|开启|開啟|关掉|關掉).{0,4}(?:手机设置|手機設置|[蓝藍]牙|[无無][线線]网|Wi-Fi)|"
    r"重新配[网網])", re.I,
)


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
    features = hello.get("features")
    if isinstance(features, dict):
        for key in ("environment", "media_choice", "reminders"):
            if features.get(key) is True:
                result[key] = True
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
        local = {s["phrase"]: s for s in skills if isinstance(s, dict) and
                 isinstance(s.get("phrase"), str) and isinstance(s.get("action"), str)
                 and result["voice_controls"].get("music" if s["action"] == "story" else s["action"])}
        result["local_phrases"] = set(local)
        if result.get("media_choice"):
            result["local_media"] = {phrase: skill for phrase, skill in local.items()
                                     if skill["action"] in ("music", "story") and isinstance(skill.get("argument"), str)
                                     and 0 < len(skill["argument"].encode()) < 1024}
    return result


def music_call(call):
    name = call["function"]["name"]
    args = json.loads(call["function"]["arguments"])
    if name == "manage_reminder" and isinstance(args, dict):
        keys = {"operation", "hour", "minute", "date", "daily", "message", "id"}
        op = args.get("operation")
        date = args.get("date", "")
        message = args.get("message", "")
        if (set(args) <= keys and op in ("set", "list", "cancel") and (op != "set" or "hour" in args)
                and all(key not in args or (type(args[key]) is int and low <= args[key] <= high)
                        for key, low, high in (("hour", 0, 23), ("minute", 0, 59), ("id", 1, 2147483647)))
                and isinstance(date, str)
                and re.fullmatch(r"(?:today|tomorrow|day_after_tomorrow|[0-9]{4}-[0-9]{2}-[0-9]{2})?", date)
                and type(args.get("daily", False)) is bool
                and isinstance(message, str) and len(message.encode()) <= 144
                and not any(ord(char) < 32 or ord(char) == 127 for char in message)):
            return "self.reminder.manage", args
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
            "/" not in path or path.lower().endswith(".mp3") or args.get("action") in ("resume_story", "repeat_story")
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
        self.pending_media_reply = None
        self.choosing_media = False
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

    async def call_device(self, name, arguments, user=None):
        self.turn += 1
        if name in ("self.environment.status", "self.music.playlist", "self.radio.status", "self.radio.bluetooth",
                    "self.reminder.manage"):
            self.pending_device_reply = (self.turn, user if name == "self.reminder.manage" else None)
        if name.startswith("self.audio_speaker.") or arguments.get("action") == "stop":
            self.pending_control = self.turn
        if name in ("self.music.play_file", "self.music.select") or (
            name == "self.music.control" and arguments.get("action") != "stop"
        ):
            self.pending_media_reply = self.turn
        if name.startswith("self.music.") and name != "self.music.select":
            self.choosing_media = False
        await self.send("mcp", payload={"jsonrpc": "2.0", "id": self.turn, "method": "tools/call",
            "params": {"name": name, "arguments": arguments}})

    async def speak(self, text):
        self.speaking = True
        await self.send("tts", state="start")
        await self.send("tts", state="sentence_start", text=text)
        await self.provider.speak(text, self.settings, self.codec, self.socket)
        await self.send("tts", state="stop")
        self.speaking = False

    async def media_reply(self, result):
        self.pending_media_reply = None
        self.choosing_media = False
        content = result.get("result", {}).get("content", [])
        text = content[0].get("text", "") if content else ""
        if text == "true":
            return
        if isinstance(text, str) and text.startswith("{"):
            data = json.loads(text)
            self.choosing_media = data.get("status") in ("choose", "retry")
            text = data.get("prompt")
        if not isinstance(text, str) or not text or text == "false":
            text = "设备暂时无法播放，请再试一次。"
        await self.cancel()
        self.job = asyncio.create_task(self.speak_device_reply(text[:4096]))

    async def speak_device_reply(self, text):
        try:
            await self.speak(text)
        except (aiohttp.ClientError, asyncio.TimeoutError, ValueError) as error:
            await self.turn_failed(error)

    async def turn_failed(self, error):
        LOG.warning("Voice turn failed (%s)", type(error).__name__)
        if not self.socket.closed:
            await self.send("alert", status="语音服务", message="对话失败，请检查网关和模型设置", emotion="sad")
            await self.socket.close()

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
        if kind == "mcp" and self.pending_media_reply is not None:
            result = message.get("payload")
            if isinstance(result, dict) and result.get("id") == self.pending_media_reply:
                await self.media_reply(result)
                return
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
            if result.get("id") == self.pending_device_reply[0]:
                _, user = self.pending_device_reply
                self.pending_device_reply = None
                content = result.get("result", {}).get("content", [])
                text = content[0].get("text", "") if content else "设备操作失败"
                text = text[:4096]
                await self.send("tts", state="start")
                await self.send("tts", state="sentence_start", text=text)
                await self.provider.speak(text, self.settings, self.codec, self.socket)
                await self.send("tts", state="stop")
                if user:
                    self.history.extend((user, {"role": "assistant", "content": text}))
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
            self.pending_media_reply = None
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
            self.pending_media_reply = None
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
            phrase = text.rstrip("。！？.!?")
            local_media = self.settings.get("local_media", {}).get(phrase)
            if local_media:
                await self.send("stt", text=text, handle_locally=False)
                path = local_media["argument"]
                if local_media["action"] == "story":
                    await self.call_device("self.music.control", {"action": "resume_story", "path": path})
                else:
                    await self.call_device("self.music.play_file", {"path": path})
                return
            await self.send("stt", text=text)
            if phrase in self.settings.get("local_phrases", ()):
                await self.socket.close()
                return
            if self.choosing_media:
                new_request = _NEW_MEDIA_REQUEST.search(phrase) and not _NUMBERED_CHOICE.fullmatch(phrase)
                if new_request:
                    self.choosing_media = False
                elif not _OTHER_DEVICE_REQUEST.search(phrase) and not (
                    self.settings.get("reminders") and REMINDER_REQUEST.search(phrase)
                ):
                    await self.call_device("self.music.select", {"choice": phrase})
                    return
            user = {"role": "user", "content": text}
            system = SYSTEM if self.settings.get("voice_controls", {}).get("music", True) else {
                "role": "system", "content": "你是便携语音助手。使用用户的语言简短回答。"}
            if self.settings.get("reminders"):
                system = {**system, "content": system["content"] + REMINDER_INSTRUCTION}
            answer = await self.provider.chat([system, *self.history, user], self.settings)
            calls = answer.get("tool_calls")
            if calls:
                name, arguments = music_call(calls[0])
                await self.call_device(name, arguments, user)
                return
            text = answer.get("content")
            if not isinstance(text, str) or not text.strip() or len(text) > 4096:
                raise ValueError("empty or oversized model response")
            await self.speak(text)
            self.history.extend((user, {"role": "assistant", "content": text}))
        except (aiohttp.ClientError, asyncio.TimeoutError, ValueError, KeyError, IndexError, TypeError) as error:
            await self.turn_failed(error)

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
