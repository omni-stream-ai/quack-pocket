import asyncio
import io
import json
import re
import wave

import aiohttp

MUSIC_TOOLS = [
    {"type": "function", "function": {
        "name": "play_file", "description": "用户点播歌曲时优先调用此工具，按歌名查找并播放存储卡上的 MP3。path 填用户说的歌名（文件名可省略扩展名），或包含扩展名的完整相对路径。",
        "parameters": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}}},
    {"type": "function", "function": {
        "name": "control_music", "description": "控制设备上的本地音乐播放。",
        "parameters": {"type": "object", "properties": {"action": {"type": "string",
            "enum": ["sequential", "shuffle", "next", "stop"]}}, "required": ["action"]}}}
]


VOLUME_TOOLS = [
    {"type": "function", "function": {
        "name": name, "description": description,
        "parameters": {"type": "object", "properties": {key: {
            "type": "integer", "minimum": minimum, "maximum": 100}}, "required": [key]}}}
    for name, key, minimum, description in (
        ("set_volume", "volume", 0, "将设备扬声器的音量设为指定百分比。"),
        ("adjust_volume", "delta", -100, "在当前音量基础上增减，大声一点用 10，小声一点用 -10。"))
]
LOCAL_MODE_TOOL = {"type": "function", "function": {
    "name": "enter_local_mode", "description": "用户明确要求返回首页、退出语音通话或结束通话时使用。",
    "parameters": {"type": "object", "properties": {}}}}
SETUP_TOOL = {"type": "function", "function": {
    "name": "open_setup", "description": "用户明确要求重新配网或打开手机设置时使用。",
    "parameters": {"type": "object", "properties": {}}}}


# Keep device actions out of ordinary conversation, particularly for small models.
_MUSIC_REQUEST = re.compile(
    r"播放|点播|點播|点歌|點歌|放一?[首曲]|放歌|想听|想聽|听一首|聽一首|来一首|來一首|"
    r"下一[首曲]|换一?[首歌]|換一?[首歌]|切歌|随机|隨機|顺序|順序|停止|停播|停歌|关掉音乐|關掉音樂|"
    r"\.mp3\b|\b(play|shuffle|skip|next (?:song|track)|stop (?:the )?(?:music|playback))\b",
    re.IGNORECASE,
)


async def response_json(response):
    response.raise_for_status()
    data = bytearray()
    async for chunk in response.content.iter_chunked(8192):
        data.extend(chunk)
        if len(data) > 128 * 1024:
            raise ValueError("upstream JSON exceeds limit")
    return json.loads(data)


class Provider:
    def __init__(self, client, base, defaults, *, upstreams=None):
        self.client = client
        self.base = base.rstrip("/")
        self.defaults = defaults
        self.upstreams = upstreams or {}

    def post(self, stage, path, **kwargs):
        base, headers = self.upstreams.get(stage, (self.base, {}))
        return self.client.post(base + path, headers=headers, allow_redirects=False, **kwargs)

    async def transcribe(self, pcm, settings):
        buffer = io.BytesIO()
        with wave.open(buffer, "wb") as wav:
            wav.setparams((1, 2, 16000, 0, "NONE", ""))
            wav.writeframes(pcm)
        form = aiohttp.FormData()
        form.add_field("model", settings.get("asr") or self.defaults["asr"])
        form.add_field("file", buffer.getvalue(), filename="speech.wav", content_type="audio/wav")
        async with self.post("asr", "/audio/transcriptions", data=form) as response:
            body = await response_json(response)
        text = body.get("text")
        if not isinstance(text, str) or len(text) > 8192:
            raise ValueError("invalid transcript")
        return text.strip()

    async def chat(self, messages, settings):
        phrase = messages[-1]["content"]
        enabled = settings.get("voice_controls", {})
        tools = list(MUSIC_TOOLS) if enabled.get("music", True) and _MUSIC_REQUEST.search(phrase) else []
        if enabled.get("volume", True) and re.search(r"音量|声音|聲音|大[声聲]|小[声聲]|静音|靜音|volume|louder|quieter|mute", phrase, re.I):
            tools.extend(VOLUME_TOOLS)
        if re.search(r"重新配网|重新配網|打开.{0,4}设置|打開.{0,4}設置|open settings", phrase, re.I):
            tools.append(SETUP_TOOL)
        if enabled.get("state", True) and re.search(r"本地模式|(?:返回|回到|回|去)首页|(?:退出|结束|关闭).{0,4}(?:通话|语音|小智)|local mode", phrase, re.I):
            tools.append(LOCAL_MODE_TOOL)
        protocol = settings.get("protocol", "chat_completions")
        model = settings["model"]
        stage = protocol if protocol in self.upstreams else "llm"
        if protocol == "responses":
            request = {"model": model, "input": messages, "max_output_tokens": 512, "store": False}
            if tools:
                request["tools"] = [{"type": "function", **tool["function"]} for tool in tools]
            async with self.post(stage, "/responses", json=request) as response:
                body = await response_json(response)
            if body.get("status") != "completed":
                raise ValueError("incomplete Responses output")
            texts, calls = [], []
            for item in body["output"]:
                if item["type"] == "message":
                    texts.extend(part["text"] for part in item["content"] if part["type"] == "output_text")
                elif item["type"] == "function_call":
                    calls.append({"function": {"name": item["name"], "arguments": item["arguments"]}})
            answer = {"content": "".join(texts), "tool_calls": calls}
        elif protocol == "anthropic":
            base, auth = self.upstreams.get(stage, (self.base, {}))
            headers = {"anthropic-version": "2023-06-01"}
            if auth.get("Authorization"):
                headers["x-api-key"] = auth["Authorization"].removeprefix("Bearer ")
            request = {"model": model, "system": messages[0]["content"], "messages": messages[1:], "max_tokens": 512}
            if tools:
                request["tools"] = [{"name": f["name"], "description": f["description"], "input_schema": f["parameters"]}
                                    for f in (tool["function"] for tool in tools)]
            async with self.client.post(base + "/messages", json=request, headers=headers, allow_redirects=False) as response:
                body = await response_json(response)
            blocks = body["content"]
            answer = {"content": "".join(block["text"] for block in blocks if block["type"] == "text"),
                      "tool_calls": [{"function": {"name": block["name"], "arguments": json.dumps(block["input"])}}
                                     for block in blocks if block["type"] == "tool_use"]}
        else:
            request = {"model": model, "messages": messages, "max_tokens": 512}
            if tools:
                request["tools"] = tools
            async with self.post("llm", "/chat/completions", json=request) as response:
                body = await response_json(response)
            answer = body["choices"][0]["message"]
        allowed = {tool["function"]["name"] for tool in tools}
        if any(call.get("function", {}).get("name") not in allowed for call in answer.get("tool_calls") or []):
            return {"content": "我没听清，请再说一次。"}
        return answer

    async def speak(self, text, settings, codec, socket):
        request = {"model": settings.get("tts") or self.defaults["tts"], "voice": settings.get("voice") or self.defaults["voice"],
                   "input": text, "response_format": "pcm"}
        async with self.post("tts", "/audio/speech", json=request) as response:
            response.raise_for_status()
            pending = bytearray()
            received = 0
            async for chunk in response.content.iter_chunked(8192):
                received += len(chunk)
                if received > 120 * 48000:
                    raise ValueError("TTS duration exceeds limit")
                pending.extend(chunk)
                while len(pending) >= 2880:
                    await socket.send_bytes(codec.packet(pending[:2880]))
                    del pending[:2880]
                    await asyncio.sleep(0.060)
            if not received or received % 2:
                raise ValueError("invalid PCM response")
            if pending:
                pending.extend(bytes(2880 - len(pending)))
                await socket.send_bytes(codec.packet(pending))
                await asyncio.sleep(0.060)
