import asyncio
import base64
import json
import unittest
from contextlib import asynccontextmanager

from aiohttp import ClientSession, WSMsgType, web
from aiohttp.test_utils import TestClient, TestServer

from gateway.provider import Provider
from gateway.server import make_app, provider_upstreams
from test_voice_gateway import MAC, TOKEN, pcm
from gateway import opus
import ctypes as c


class Protocols(unittest.IsolatedAsyncioTestCase):
    async def test_text_and_actions(self):
        async def respond(req):
            data = await req.json()
            enabled = "tools" in data
            if req.path == "/v1/responses":
                self.assertEqual(req.headers["Authorization"], "Bearer responses-key")
                self.assertEqual(data["input"][0]["role"], "system")
                if enabled:
                    self.assertEqual(data["tools"][0]["name"], "play_file")
                    output = [{"type":"function_call","name":"play_file","arguments":'{"path":"晴天"}'}]
                else:
                    output = [{"type":"message","content":[{"type":"output_text","text":"你好"}]}]
                return web.json_response({"status":"completed","output":output})
            self.assertEqual(req.path, "/v1/messages")
            self.assertNotIn("Authorization", req.headers)
            self.assertEqual(req.headers["x-api-key"], "anthropic-key")
            self.assertEqual(req.headers["anthropic-version"], "2023-06-01")
            self.assertEqual(data["system"], "回答问题")
            self.assertEqual(data["messages"][0]["role"], "user")
            if enabled:
                self.assertIn("input_schema", data["tools"][0])
                blocks = [{"type":"tool_use","name":"play_file","input":{"path":"晴天"}}]
            else:
                blocks = [{"type":"text","text":"你好"}]
            return web.json_response({"content":blocks})

        app = web.Application()
        app.router.add_post("/v1/{endpoint}", respond)
        async with TestServer(app) as upstream, ClientSession() as client:
            base = str(upstream.make_url("/v1"))
            env = {"OMNI_PROVIDER_URL":base}
            for name in ("responses","anthropic"):
                prefix = "OMNI_" + name.upper()
                env.update({prefix+"_URL":base, prefix+"_KEY":name+"-key"})
            provider = Provider(client, base, {}, upstreams=provider_upstreams(env))
            for name in ("responses","anthropic"):
                messages = [{"role":"system","content":"回答问题"},{"role":"user","content":"你好"}]
                settings = {"model":"test","protocol":name}
                answer = await provider.chat(messages,settings)
                self.assertEqual(answer["content"],"你好")
                messages[-1]["content"] = "点歌晴天"
                answer = await provider.chat(messages,settings)
                call = answer["tool_calls"][0]["function"]
                self.assertEqual((call["name"],json.loads(call["arguments"])),("play_file",{"path":"晴天"}))

    async def test_realtime_audio_through_device_protocol(self):
        connections, turns, readings = [], [], []
        appended = asyncio.Event()
        async def realtime(req):
            self.assertEqual(req.query["model"],"test-realtime")
            self.assertEqual(req.headers["Authorization"], "Bearer realtime-key")
            ws = web.WebSocketResponse()
            await ws.prepare(req)
            connections.append(True)
            await ws.send_json({"type":"session.created"})
            inputs = []
            async for message in ws:
                event = message.json()
                if event["type"] == "session.update":
                    self.assertIsNone(event["session"]["turn_detection"])
                    self.assertEqual(event["session"]["voice"],"Tina")
                    await ws.send_json({"type":"session.updated"})
                elif event["type"] == "input_audio_buffer.append":
                    inputs.append(base64.b64decode(event["audio"],validate=True))
                    appended.set()
                elif event["type"] == "input_audio_buffer.commit":
                    turns.append(sum(map(len,inputs)))
                    inputs.clear()
                elif event["type"] == "response.create":
                    readings.append(event["response"]["instructions"])
                    audio = pcm(24000,.06)
                    for chunk in [audio[:777],audio[777:]]:
                        await ws.send_json({"type":"response.audio.delta","delta":base64.b64encode(chunk).decode()})
                    await ws.send_json({"type":"response.audio_transcript.done","transcript":"收到"})
                    await ws.send_json({"type":"response.done","response":{"status":"completed"}})
            return ws

        app = web.Application()
        app.router.add_get("/realtime",realtime)
        async with TestServer(app) as upstream:
            routes=provider_upstreams({"OMNI_REALTIME_URL":str(upstream.make_url("/realtime").with_scheme("ws")),
                                      "OMNI_REALTIME_KEY":"realtime-key"})
            @asynccontextmanager
            async def factory():
                async with ClientSession() as client:
                    yield Provider(client,"",{},upstreams=routes)
            async with TestClient(TestServer(make_app({MAC:TOKEN},factory))) as client:
                async with client.ws_connect("/device", headers={"Device-Id":MAC,"Authorization":"Bearer "+TOKEN}) as ws:
                    await ws.send_json({"type":"hello","version":1,"audio_params":{"format":"opus","sample_rate":16000,
                        "frame_duration":60,"channels":1},"features":{"environment":True},"model_settings":{"protocol":"qwen_realtime","model":"test-realtime","voice":"Tina"}})
                    self.assertEqual((await ws.receive_json(timeout=2))["type"],"hello")
                    encoder=opus.new_encoder(16000,1,2048,c.byref(c.c_int()))
                    try:
                        for turn in range(2):
                            appended.clear()
                            await ws.send_json({"type":"listen","state":"start"})
                            query = (await ws.receive_json(timeout=2))["payload"]
                            self.assertEqual(query["params"]["name"], "self.environment.status")
                            reading = f"2026年11月23日 12:0{turn}，温度23.5摄氏度，相对湿度45%"
                            await ws.send_json({"type":"mcp","payload":{"id":query["id"],
                                "result":{"content":[{"type":"text","text":reading}]}}})
                            for i in range(5):
                                buf=c.create_string_buffer(1275)
                                samples=(c.c_int16*960).from_buffer_copy(pcm(16000,.06))
                                length=opus.encode(encoder,samples,960,buf,len(buf))
                                await ws.send_bytes(buf.raw[:length])
                            await asyncio.wait_for(appended.wait(),2)
                            await ws.send_json({"type":"listen","state":"stop"})
                            self.assertEqual((await ws.receive_json(timeout=2))["state"],"start")
                            self.assertEqual((await ws.receive(timeout=2)).type,WSMsgType.BINARY)
                            self.assertEqual((await ws.receive_json(timeout=2))["text"],"收到")
                            self.assertEqual((await ws.receive_json(timeout=2))["state"],"stop")
                    finally:
                        opus.free_encoder(encoder)
            self.assertEqual(connections,[True])
            self.assertEqual(turns,[9600,9600])
            self.assertIn("12:00，温度23.5", readings[0])
            self.assertIn("12:01，温度23.5", readings[1])
