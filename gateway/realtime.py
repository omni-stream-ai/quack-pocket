"""Qwen Omni Realtime manual-turn WebSocket, with 16 kHz input and 24 kHz output."""
import asyncio
import base64
import uuid

from yarl import URL


class QwenRealtime:
    def __init__(self, client, endpoint, headers, settings):
        self.client = client
        self.url = URL(endpoint).update_query(model=settings["model"])
        self.headers = headers
        self.voice = settings.get("voice")
        self.socket = None

    async def send(self, kind, **data):
        await self.socket.send_json({"event_id": uuid.uuid4().hex, "type": kind, **data})

    async def receive(self):
        event = await self.socket.receive_json(timeout=30)
        if event["type"] == "error":
            raise ValueError("realtime upstream rejected event")
        return event

    async def append(self, pcm):
        if self.socket is None:
            self.socket = await self.client.ws_connect(self.url, headers=self.headers, max_msg_size=1024 * 1024)
            session = {"modalities": ["text", "audio"], "turn_detection": None,
                       "audio": {"input": {"format": {"type": "pcm", "sample_rate": 16000}},
                                 "output": {"format": {"type": "pcm", "sample_rate": 24000}}},
                       "instructions": "你是便携语音助手。使用用户的语言简短回答。"}
            if self.voice:
                session["voice"] = self.voice
            await self.send("session.update", session=session)
            async with asyncio.timeout(15):
                while (await self.receive())["type"] != "session.updated":
                    pass
        await self.send("input_audio_buffer.append", audio=base64.b64encode(pcm).decode("ascii"))

    async def reply(self, conversation):
        await self.send("input_audio_buffer.commit")
        await self.send("response.create")
        pending = bytearray()
        received = 0
        async with asyncio.timeout(150):
            while True:
                event = await self.receive()
                kind = event["type"]
                if kind == "response.audio.delta":
                    chunk = base64.b64decode(event["delta"], validate=True)
                    if not chunk:
                        continue
                    received += len(chunk)
                    if received > 120 * 48000:
                        raise ValueError("realtime audio exceeds limit")
                    if not conversation.speaking:
                        conversation.speaking = True
                        await conversation.send("tts", state="start")
                    pending.extend(chunk)
                    while len(pending) >= 2880:
                        await conversation.socket.send_bytes(conversation.codec.packet(pending[:2880]))
                        del pending[:2880]
                        await asyncio.sleep(.060)
                elif kind == "response.audio_transcript.done" and event.get("transcript"):
                    await conversation.send("tts", state="sentence_start", text=event["transcript"][:4096])
                elif kind == "response.done":
                    if event["response"].get("status") != "completed" or not received or received % 2:
                        raise ValueError("incomplete realtime response")
                    if pending:
                        pending.extend(bytes(2880-len(pending)))
                        await conversation.socket.send_bytes(conversation.codec.packet(pending))
                    await conversation.send("tts", state="stop")
                    conversation.speaking = False
                    return

    async def close(self):
        if self.socket is not None:
            socket, self.socket = self.socket, None
            await socket.close()
