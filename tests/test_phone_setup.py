"""Exercise the firmware's actual setup page in desktop Firefox without a device."""
import base64
import json
import shutil
import subprocess
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

PAGE = Path(__file__).resolve().parents[1] / "web/index.html"
PROBE = """<script>
(async () => {
    const byId = id => document.getElementById(id);
    const wait = async predicate => {
        for (let attempt = 0; attempt < 400; attempt++) {
            if (predicate()) return;
            await new Promise(resolve => setTimeout(resolve, 15));
        }
        throw Error('Timed out waiting for UI');
    };
    const check = (condition, message) => { if (!condition) throw Error(message); };
    try {
        await wait(() => !byId('retry').hidden);
        byId('retry').click();
        await wait(() => !byId('save').disabled);
        check(byId('updateUrl').value==='https://device.test/manifest.json','Missing update source');
        check(!byId('overview').hidden && byId('wifi').classList.contains('outside-category'),'Expected device home');
        document.querySelector('nav a[href="#wifi"]').click();
        await wait(()=>!byId('wifi').classList.contains('outside-category'));
        check(byId('overview').hidden && !byId('taskBar').hidden,'Expected a task screen');
        byId('taskBar').querySelector('a').click();
        await wait(()=>!byId('overview').hidden);
        const auto=byId('autoUpdate');
        check(!byId('autoUpdateOption').hidden && !auto.checked,'Automatic checks should default off');
        for(const enabled of [true,false]){
            auto.click();
            await wait(()=>!auto.disabled);
            check(auto.checked===enabled && byId('updateStatus').textContent==='更新设置已保存','Auto-update preference failed');
        }
        if(typeof characteristicMock!=='undefined'){
            check(!byId('useSiteUpdates').hidden,'Hosted update action missing');
            byId('useSiteUpdates').click();
            await wait(()=>byId('updateStatus').textContent.includes('尚未部署'));
            check(byId('updateUrl').value==='https://device.test/manifest.json','Missing release changed source');
            byId('useSiteUpdates').click();
            await wait(()=>byId('updateStatus').textContent.includes('已使用本站更新'));
            check(byId('updateUrl').value==='https://site.test/pocket/ota/manifest.json','Wrong nested path');
        }else{check(byId('useSiteUpdates').hidden,'Device-local page offered itself as OTA host')}
        byId('updateUrl').value='https://updates.test/ota/manifest.json';byId('saveUpdateSource').click();
        await wait(()=>byId('updateStatus').textContent==='更新地址已保存');
        check(!byId('bluetoothSettings').hidden && byId('toggleBluetooth').textContent.includes('开启'),'Missing Bluetooth setting');
        byId('toggleBluetooth').click();
        await wait(()=>byId('bluetoothState').textContent.includes('已开启'));
        check(!byId('controls').hidden && byId('allowMusic').checked, 'Missing voice controls');
        byId('allowMusic').checked=false;
        byId('voiceSkills').value='阅读时间=音量 25';
        byId('newPhrase').value='回家了';byId('newAction').value='返回首页';byId('addSkill').click();
        check(document.querySelector('#phrases input').placeholder==='播放','Missing saved command hint');
        check(!document.querySelector('nav a[href="#sdBrowser"]').hidden,'Missing SD navigation');
        byId('saveControls').click();
        await wait(()=>byId('controlsStatus').textContent==='语音功能已保存');
        check(byId('llmProtocol').value==='responses','Protocol failed to load');
        byId('llmProtocol').value='qwen_realtime';byId('llmProtocol').onchange();
        check(!byId('realtimeHint').hidden && byId('asr').closest('label').hidden,'Realtime speech controls');
        byId('llmProtocol').value='anthropic';byId('llmProtocol').onchange();
        byId('asr').value='asr-draft';
        byId('saveLlm').click();
        await wait(()=>byId('llmStatus').textContent==='对话已保存');
        check(byId('asr').value==='asr-draft','Saving chat lost speech draft');
        byId('saveSpeech').click();
        await wait(()=>byId('speechStatus').textContent==='语音已保存');
        byId('asr').value='';
        check(!byId('standby').hidden && byId('standbyMinutes').value === '5', 'Standby time missing');
        byId('standbyMinutes').value='10';byId('saveStandby').click();
        await wait(() => byId('standbyStatus').textContent === '待机时间已保存');
        byId('sdOpen').click();
        await wait(()=>byId('sdEntries').children.length===1);
        byId('sdEntries').querySelector('button').click();
        await wait(()=>byId('sdPath').textContent==='/音乐');
        byId('pickDir').click();byId('saveMedia').click();
        await wait(()=>byId('mediaStatus').textContent==='目录已保存');
        byId('sdMore').click();await wait(()=>byId('sdEntries').children.length===2);
        byId('sdUp').click();await wait(()=>byId('sdPath').textContent==='/');
        const named=(selector,title)=>[...document.querySelectorAll(selector)].find(b=>b.textContent===title);
        window.prompt=()=> '测试目录';window.confirm=()=>true;
        byId('sdMkdir').click();await wait(()=>named('#sdEntries button','测试目录/'));
        named('#sdEntries button','测试目录/').click();
        await wait(()=>byId('sdPath').textContent==='/测试目录');
        const originalClick=HTMLInputElement.prototype.click;
        HTMLInputElement.prototype.click=function(){
            const dt=new DataTransfer();dt.items.add(new File(['hello'],'原名.txt'));
            this.files=dt.files;this.dispatchEvent(new Event('change'));
        };
        byId('sdUpload').click();HTMLInputElement.prototype.click=originalClick;
        await wait(()=>named('#sdEntries button','预览'));
        named('#sdEntries button','预览').click();await wait(()=>!byId('sdPreview').hidden);
        const editor=byId('sdPreviewBody').querySelector('textarea');check(editor.value==='hello','Wrong preview');
        editor.value='你好';named('#sdPreviewBody button','保存文本').click();
        await wait(()=>byId('sdPreview').hidden);
        window.prompt=()=> '测试目录/新名.txt';named('#sdEntries button','重命名或移动').click();
        await wait(()=>byId('sdEntries').textContent.includes('新名.txt'));
        let downloaded='';
        window.showSaveFilePicker=async()=>({createWritable:async()=>({
            write:async chunk=>{downloaded+=new TextDecoder().decode(chunk)},close:async()=>{}
        })});
        named('#sdEntries button','下载').click();await wait(()=>downloaded==='你好');
        byId('sdUp').click();await wait(()=>byId('sdPath').textContent==='/');
        named('#sdEntries button','测试目录/').closest('li').querySelector('details').open=true;
        const folder=named('#sdEntries button','测试目录/').closest('li');
        [...folder.querySelectorAll('button')].find(b=>b.textContent==='删除').click();
        await wait(()=>!named('#sdEntries button','测试目录/'));
        byId('musicQuery').value='晴天';byId('searchMusic').click();
        await wait(()=>byId('searchHits').children.length===1);
        byId('searchHits').querySelector('button').click();
        await wait(()=>byId('libraryStatus').textContent==='播放已排队');
        byId('searchHits').querySelectorAll('button')[1].click();
        await wait(()=>byId('favList').children.length===1);
        byId('favList').querySelectorAll('button')[1].click();
        await wait(()=>!byId('favList').children.length);
        byId('checkItems').value='水杯\\n钥匙';byId('saveCheck').click();
        await wait(()=>byId('checkStatus').textContent==='清单已保存');
        check(byId('phrases').querySelector('input').value === '播放', 'Offline words missing');
        byId('songs').value = '晴天';
        byId('saveWords').click();
        await wait(() => byId('wordsStatus').textContent === '离线口令已保存');
        await wait(() => byId('scanStatus').textContent.includes('扫描失败'));
        location.hash='wifi';await wait(()=>!byId('wifi').classList.contains('outside-category'));
        byId('scan').click();
        await wait(() => byId('networks').children.length === 1);
        byId('networks').firstElementChild.click();
        check(!byId('picker').open && document.activeElement === byId('password'), 'Network picker did not close');
        check(byId('xiaozhiHelp').hidden, 'Official console shown in custom mode');
        check(byId('ssid').value === '客厅 & Wi-Fi', 'SSID not copied verbatim');
        check(!byId('custom').hidden, 'Custom settings hidden after load');
        byId('model').value = 'draft';
        byId('addWifi').click();
        await wait(() => byId('savedCount').textContent.includes('2/10'));
        check(!byId('ssid').value && byId('model').value === 'draft', 'Draft lost during network save');
        window.confirm = () => true;
        byId('savedWifi').lastElementChild.querySelector('button').click();
        await wait(() => byId('savedCount').textContent.includes('1/10'));
        byId('networks').firstElementChild.click();
        byId('model').value = 'chat';
        const url = byId('url'), token = byId('token');
        check(!token.value && token.placeholder.includes('沿用'), 'Token retention hint missing');
        url.value = 'wss://changed.test/device';
        url.dispatchEvent(new Event('input'));
        check(!token.placeholder.includes('沿用'), 'Changed gateway offers saved token');
        url.value = 'wss://saved.test/device';
        url.dispatchEvent(new Event('input'));
        byId('save').click();
        await wait(() => byId('status').textContent === '保存失败');
        check(!byId('save').disabled && byId('model').value === 'chat', 'Cannot retry failed save');
        byId('mode').value = 'default';
        byId('mode').dispatchEvent(new Event('change'));
        check(byId('custom').hidden, 'Custom settings still visible');
        check(!byId('xiaozhiHelp').hidden && byId('xiaozhiHelp').querySelector('a').href === 'https://xiaozhi.me/console/', 'Official console link missing');
        await wait(() => !byId('photo').disabled && !byId('photoOff').disabled);
        const standbyDefault=byId('preview').toDataURL(),shutdownDefault=byId('previewOff').toDataURL();
        check(standbyDefault!==shutdownDefault,'Default ducks must differ');
        const pic=document.createElement('canvas');pic.width=pic.height=2;
        const ctx=pic.getContext('2d');ctx.fillStyle='black';ctx.fillRect(0,0,2,2);
        const blob=await new Promise(resolve=>pic.toBlob(resolve));
        const files=new DataTransfer();files.items.add(new File([blob],'test.png',{type:'image/png'}));
        byId('photo').files=files.files;byId('photo').dispatchEvent(new Event('change'));
        await wait(()=>!byId('applyImage').disabled);byId('applyImage').click();
        await wait(()=>byId('imageStatus').textContent==='屏保已保存');
        check(byId('previewOff').toDataURL()===shutdownDefault,'Standby upload changed shutdown');
        byId('photoOff').files=files.files;byId('photoOff').dispatchEvent(new Event('change'));
        await wait(()=>!byId('applyImageOff').disabled);byId('applyImageOff').click();
        await wait(()=>byId('imageStatusOff').textContent==='屏保已保存');
        byId('clearImage').click();await wait(()=>byId('preview').toDataURL()===standbyDefault);
        check(byId('previewOff').toDataURL()!==shutdownDefault,'Standby reset changed shutdown');
        byId('clearImageOff').click();await wait(()=>byId('previewOff').toDataURL()===shutdownDefault);
        byId('save').click();
        await wait(() => byId('status').textContent === '已保存');
        check(byId('save').disabled, 'Duplicate save possible during reboot');
        if(typeof pairedAccess!=='undefined' && pairedAccess){
            byId('disconnect').click();
            check(byId('settings').hidden && !byId('guide').hidden,'Missing connection screen');
            byId('connect').click();
            await wait(()=>!byId('settings').hidden);
        }
        await fetch('/result', {method: 'POST', body: 'PASS'});
    } catch (error) {
        await fetch('/result', {method: 'POST', body: String(error)});
    }
})();
</script>"""


# Simulate the GATT frames at a 23-byte ATT MTU. Only the fake peripheral
# forwards these requests to the fixture server.
BLUETOOTH = """<script>
const peripheralFetch = window.fetch.bind(window);
let releaseAvailable=false;
window.fetch = (path, options) => {
    if(path==='https://site.test/pocket/ota/manifest.json'){
        if(!releaseAvailable){releaseAvailable=true;return Promise.resolve(new Response('',{status:404}))}
        return Promise.resolve(Response.json({board:'waveshare-s3-epaper-1.54-v2',version:'2.0.5',sha256:'a'.repeat(64),signature:'abcd'}));
    }
    if(path !== '/result') throw Error('Hosted UI attempted an HTTP device operation');
    return peripheralFetch(path,options);
};
const pairedAccess=PAIRED_ACCESS;
let route, upload, response, status, offset, key='';
const endpoints=['/profile','/networks','/saver','/profile','/saver','/exit','/shutdown-saver','/shutdown-saver','/access'];
const characteristicMock={
    async writeValueWithResponse(frame){
        if(frame.length>20)throw Error('Exceeded ATT MTU');
        if(pairedAccess && (frame[0]===4 || frame[0]===0 && frame[1]===8))throw Error("Redundant browser authorization");
        switch(frame[0]){
        case 4: key=Array.from(frame.slice(1),n=>n.toString(16).padStart(2,"0")).join("");break;
        case 0: route=frame[1];upload=[];response=[];status=offset=0;break;
        case 1: upload.push(...frame.slice(1));break;
        case 2: {
            const options=(route<3||route===6)?{}:{method:'POST',body:new Uint8Array(upload)};
            options.headers={"X-Pocket-Key":key};
            const r=await peripheralFetch(endpoints[route],options);
            status=r.status;response=new Uint8Array(await r.arrayBuffer());break;
        }
        case 3: offset=frame[1]|(frame[2]<<8);break;
        }
    },
    async readValue(){
        if(pairedAccess && route===undefined)return new DataView(Uint8Array.of(0,0,0,0,19,1).buffer);
        const data=response?.slice(offset,offset+16)||[];
        const frame=Uint8Array.of(status,status>>8,response?.length,response?.length>>8,19,...data);
        return new DataView(frame.buffer);
    }
};
const mockDevice=Object.assign(new EventTarget(),{
    name:'Omni-test',id:'test-unit',
    gatt:{
        connect:async()=>{route=undefined;return {getPrimaryService:async()=>({getCharacteristic:async()=>characteristicMock})}},
        disconnect:()=>mockDevice.dispatchEvent(new Event('gattserverdisconnected'))
    }
});

Object.defineProperty(navigator,'bluetooth',{value:{
    requestDevice:async()=>mockDevice
}});
</script>"""

@unittest.skipUnless(shutil.which("firefox"), "Firefox is required for the setup page test")
class PhoneSetup(unittest.TestCase):
    def test_recover_load_and_save(self):
        self.exercise_page(bluetooth=False)

    def test_bluetooth_setup(self):
        self.exercise_page(bluetooth=True)

    def test_paired_bluetooth_needs_no_browser_approval(self):
        self.exercise_page(bluetooth=True,paired=True)

    def test_manual_connection_after_refresh(self):
        self.exercise_page(bluetooth=True, paired=True, reload=True)

    def test_lan_authorization(self):
        self.exercise_page(bluetooth=False,lan=True)

    def exercise_page(self, bluetooth, lan=False, paired=False, reload=False):
        page = PAGE.read_text()
        if bluetooth:
            page = page.replace('const sitePageUrl=new URL(location.href)', "const sitePageUrl=new URL('https://site.test/pocket/index.html?test=1')")
            page = page.replace('<script>', BLUETOOTH.replace('PAIRED_ACCESS',str(paired).lower()) + '<script>', 1)
            if reload:
                page = page.replace("function openSettings(){", """function openSettings(){
                    if(!sessionStorage.getItem('reloaded')){
                        sessionStorage.setItem('reloaded','1');location.reload();return;
                    }""")
            page = page.replace('</html>', "<script>document.getElementById('connect').click()</script></html>")
        elif lan:
            page = page.replace('<head>', "<head><meta name='quack-transport' content='lan'><meta name='quack-unit' content='test'>")
        else:
            page = page.replace("location.hostname === '192.168.4.1'", "location.hostname === '127.0.0.1'")
        if (bluetooth and not paired) or lan:
            page = page.replace("</html>", """<script>
            const tick=setInterval(()=>{
              if(document.getElementById('authStatus').textContent.includes('填写名称')){
                clearInterval(tick);
                document.getElementById('browserName').value='测试手机';
                document.getElementById('requestAccess').click();
              }
            },20);
            </script></html>""")
        authorized = set()
        waiting = set()
        posts, results, bitmaps = [], [], []
        slots = {path: (PAGE.parents[1] / "branding" / name).read_bytes().split(b"200 200\n", 1)[1]
                 for path, name in [("/saver", "duck-listening.pbm"), ("/shutdown-saver", "duck-sleeping.pbm")]}
        saved_images = {}
        card = {}
        uploads = {}
        networks = ["home"]
        finished = threading.Event()

        class Handler(BaseHTTPRequestHandler):
            presence_requests = 0
            reads = 0
            scans = 0

            def log_message(self, *args):
                pass

            def reply(self, status, content_type, body):
                self.send_response(status)
                self.send_header("Content-Type", content_type + "; charset=utf-8")
                self.end_headers()
                self.wfile.write(body if isinstance(body, bytes) else body.encode())

            def do_GET(self):
                if self.path == "/":
                    self.reply(200, "text/html", page.replace("</html>", PROBE + "</html>"))
                elif ((bluetooth and not paired) or lan) and self.headers.get("X-Pocket-Key") not in authorized:
                    self.reply(403,"text/plain","unauthorized")
                elif self.path == "/presence":
                    Handler.presence_requests += 1
                    self.reply(204, "text/plain", "")
                elif self.path in slots:
                    self.reply(200, "application/octet-stream", saved_images.get(self.path) or slots[self.path])
                elif self.path == "/networks":
                    Handler.scans += 1
                    self.reply(503 if Handler.scans == 1 else 200, "application/json", json.dumps(["客厅 & Wi-Fi"]))
                elif self.path == "/profile":
                    Handler.reads += 1
                    profile = dict(auto_update=False,protocol="responses", separate_model_settings=True, bluetooth_enabled=False, update_url="https://device.test/manifest.json", standby_minutes=5, url="wss://saved.test/device", has_token=True, has_wifi=True, model="chat", saved_wifi=networks,
                                   offline_words="播放\n暂停\n下一首\n上一首\n加音量\n降音量")
                    profile["voice_controls"] = dict(music=True, volume=True, state=True, skills=[])
                    profile.update(media=dict(music="",stories="故事",messages="留言",minutes=15,
                                              record_seconds=60,chapter_stop=True),sd_browser=True,sd_manage=True,checklist=[],favorites=[])
                    profile["offline_words"] = "播放\n暂停\n下一首\n上一首\n加音量\n降音量"
                    self.reply(503 if Handler.reads == 1 else 200, "application/json", json.dumps(profile))
                else:
                    self.send_error(404)

            def do_POST(self):
                data = self.rfile.read(int(self.headers["Content-Length"]))
                key = self.headers.get("X-Pocket-Key","")
                if self.path == "/access":
                    if len(key)!=32:
                        self.reply(400,"text/plain","missing key")
                    elif key in authorized:
                        self.reply(200,"text/plain","已授权")
                    elif data == "测试手机".encode():
                        waiting.add(key)
                        self.reply(202,"text/plain","123456")
                    elif key in waiting:
                        waiting.remove(key)
                        authorized.add(key)
                        self.reply(200,"text/plain","已授权")
                    else:
                        self.reply(403,"text/plain","unknown")
                    return
                if self.path!="/result" and ((bluetooth and not paired) or lan) and key not in authorized:
                    self.reply(403,"text/plain","unauthorized")
                    return
                if self.path in slots:
                    saved_images[self.path] = data
                    bitmaps.append((self.path, data))
                    self.reply(200, "text/plain", "屏保已保存" if data else "已恢复默认小鸭")
                    return
                body = data.decode()
                if self.path == "/result":
                    results.append(body)
                    self.reply(200, "text/plain", "")
                    finished.set()
                elif self.path == "/profile":
                    payload = json.loads(body)
                    action = payload.get("action")
                    path = payload.get("path")
                    if action == "sd_mkdir":
                        card[path] = None
                    elif action == "sd_info":
                        contents = card.get(path)
                        return self.reply(200,"application/json",json.dumps(dict(exists=path in card,
                            directory=path in card and contents is None,size=len(contents or b""),
                            revision=str(len(contents or b"")) if path in card else "")))
                    elif action == "sd_begin":
                        uploads[path] = bytearray()
                        return self.reply(200,"text/plain","0123456789abcdef")
                    elif action == "sd_chunk":
                        assert len(uploads[path]) == payload["offset"]
                        uploads[path].extend(base64.b64decode(payload["bytes"],validate=True))
                    elif action == "sd_finish":
                        assert len(uploads[path]) == payload["size"]
                        card[path] = bytes(uploads.pop(path))
                    elif action == "sd_read":
                        return self.reply(200,"text/plain",base64.b64encode(card[path][payload["offset"]:][:8192]))
                    elif action == "sd_move":
                        card[payload["destination"]] = card.pop(path)
                    elif action == "sd_remove":
                        assert not any(other.startswith(path + "/") for other in card)
                        del card[path]
                    if action and action.startswith("sd_"):
                        return self.reply(200,"text/plain","成功")
                    if action == "browse_sd":
                        path, cursor = payload["path"], payload["cursor"]
                        page = {"entries":[{"directory":True,"name":"音乐"}]}
                        if path: page["entries"] = []
                        for name, contents in card.items():
                            parent, _, leaf = name.rpartition("/")
                            if parent == path:
                                page["entries"].append(dict(directory=contents is None,name=leaf))
                        if path == "音乐":
                            page = {"entries":[{"directory":False,"name":"晴天.mp3" if not cursor else "b.txt"}]}
                            if not cursor: page["next"] = 1
                        return self.reply(200,"application/json",json.dumps(page))
                    if action == "media_config" and payload["media"]["music"] == "音乐":
                        return self.reply(200,"text/plain","目录已保存")
                    if action == "search_music" and payload["query"] == "晴天":
                        return self.reply(200,"application/json",json.dumps({"entries":["音乐/晴天.mp3"]}))
                    if action in ("play_media","favorite") and payload["path"] == "音乐/晴天.mp3":
                        return self.reply(200,"text/plain","播放已排队" if action == "play_media" else "已更新收藏")
                    if action == "checklist" and payload["items"] == ["水杯","钥匙"]:
                        return self.reply(200,"text/plain","清单已保存")
                    if payload.get("action") == "auto_update" and type(payload.get("enabled")) is bool:
                        return self.reply(200,"text/plain","更新设置已保存")
                    if payload.get("action") == "update_source" and payload.get("url") in (
                            "https://updates.test/ota/manifest.json", "https://site.test/pocket/ota/manifest.json"):
                        self.reply(200, "text/plain", "更新地址已保存")
                        return
                    if payload == dict(action="save_llm",protocol="anthropic",url="wss://saved.test/device",model="chat"):
                        self.reply(200,"text/plain","对话已保存")
                        return
                    if payload == dict(action="save_speech",url="wss://saved.test/device",asr="asr-draft",tts="",voice=""):
                        self.reply(200,"text/plain","语音已保存")
                        return
                    if payload == {"action":"bluetooth","enabled":True}:
                        self.reply(200,"text/plain","蓝牙已开启")
                        return
                    if payload == {"action": "voice_controls", "policy": dict(music=False, volume=True, state=True,
                            skills=[dict(phrase="阅读时间", action="volume", argument=25),dict(phrase="回家了", action="state")])}:
                        return self.reply(200,"text/plain","语音功能已保存")
                    if payload == {"action": "standby", "minutes": 10}:
                        self.reply(200, "text/plain", "待机时间已保存")
                        return
                    if payload.get("action") == "offline_words":
                        if payload.get("words") == "播放\n暂停\n下一首\n上一首\n加音量\n降音量\n晴天":
                            self.reply(200, "text/plain", "离线口令已保存")
                        else:
                            self.reply(400, "text/plain", "词表有误")
                        return
                    if payload.get("action") == "add_wifi":
                        networks.append(payload["ssid"])
                        self.reply(200, "text/plain", "已保存网络")
                        return
                    if payload.get("action") == "forget_wifi":
                        networks.remove(payload["ssid"])
                        self.reply(200, "text/plain", "已移除")
                        return
                    posts.append(payload)
                    if len(posts) == 1:
                        self.reply(500, "text/plain", "保存失败")
                    else:
                        self.reply(200, "text/plain", "已保存")
                else:
                    self.send_error(404)

        with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server, tempfile.TemporaryDirectory() as profile:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            args = ["firefox", "--headless", "--no-remote", "--profile", profile,
                    f"http://127.0.0.1:{server.server_port}"]
            try:
                with subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) as browser:
                    try:
                        self.assertTrue(finished.wait(35), "Browser did not finish the setup flow")
                    finally:
                        browser.terminate()
                        try:
                            browser.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            browser.kill()
            finally:
                server.shutdown()
                thread.join()
        self.assertEqual(results, ["PASS"])
        self.assertEqual(Handler.presence_requests > 0, lan)
        self.assertEqual(bitmaps, [("/saver", bytes([255]) * 5000), ("/shutdown-saver", bytes([255]) * 5000),
                                   ("/saver", b""), ("/shutdown-saver", b"")])
        self.assertEqual(posts[0]["token"], "")
        self.assertEqual(posts[0]["url"], "wss://saved.test/device")
        self.assertEqual(posts[1], dict(mode="default", ssid="客厅 & Wi-Fi", password=""))
