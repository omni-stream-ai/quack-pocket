"""Exercise the firmware's actual setup page in desktop Firefox without a device."""
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
        check(!byId('standby').hidden && byId('standbyMinutes').value === '5', 'Standby time missing');
        byId('standbyMinutes').value='10';byId('saveStandby').click();
        await wait(() => byId('standbyStatus').textContent === '待机时间已保存');
        check(byId('phrases').querySelector('input').value === '播放', 'Offline words missing');
        byId('songs').value = '晴天';
        byId('saveWords').click();
        await wait(() => byId('wordsStatus').textContent === '离线口令已保存');
        await wait(() => byId('scanStatus').textContent.includes('扫描失败'));
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
let route, upload, response, status, offset, key='';
const endpoints=['/profile','/networks','/saver','/profile','/saver','/exit','/shutdown-saver','/shutdown-saver','/access'];
const characteristicMock={
    async writeValueWithResponse(frame){
        if(frame.length>20)throw Error('Exceeded ATT MTU');
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
        const data=response?.slice(offset,offset+16)||[];
        const frame=Uint8Array.of(status,status>>8,response?.length,response?.length>>8,19,...data);
        return new DataView(frame.buffer);
    }
};
Object.defineProperty(navigator,'bluetooth',{value:{requestDevice:async()=>({
    name:'Omni-test',id:'test-unit',addEventListener(){},
    gatt:{connect:async()=>({getPrimaryService:async()=>({getCharacteristic:async()=>characteristicMock})})}
})}});
</script>"""

@unittest.skipUnless(shutil.which("firefox"), "Firefox is required for the setup page test")
class PhoneSetup(unittest.TestCase):
    def test_recover_load_and_save(self):
        self.exercise_page(bluetooth=False)

    def test_bluetooth_setup(self):
        self.exercise_page(bluetooth=True)

    def test_lan_authorization(self):
        self.exercise_page(bluetooth=False,lan=True)

    def exercise_page(self, bluetooth, lan=False):
        page = PAGE.read_text()
        if bluetooth:
            page = page.replace('const sitePageUrl=new URL(location.href)', "const sitePageUrl=new URL('https://site.test/pocket/index.html?test=1')")
            page = page.replace('<script>', BLUETOOTH + '<script>', 1)
            page = page.replace('</html>', "<script>document.getElementById('connect').click()</script></html>")
        elif lan:
            page = page.replace('<head>', "<head><meta name='quack-transport' content='lan'><meta name='quack-unit' content='test'>")
        else:
            page = page.replace("location.hostname === '192.168.4.1'", "location.hostname === '127.0.0.1'")
        if bluetooth or lan:
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
        networks = ["home"]
        finished = threading.Event()

        class Handler(BaseHTTPRequestHandler):
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
                elif (bluetooth or lan) and self.headers.get("X-Pocket-Key") not in authorized:
                    self.reply(403,"text/plain","unauthorized")
                elif self.path in slots:
                    self.reply(200, "application/octet-stream", saved_images.get(self.path) or slots[self.path])
                elif self.path == "/networks":
                    Handler.scans += 1
                    self.reply(503 if Handler.scans == 1 else 200, "application/json", json.dumps(["客厅 & Wi-Fi"]))
                elif self.path == "/profile":
                    Handler.reads += 1
                    profile = dict(update_url="https://device.test/manifest.json", standby_minutes=5, url="wss://saved.test/device", has_token=True, has_wifi=True, model="chat", saved_wifi=networks,
                                   offline_words="播放\n暂停\n下一首\n上一首\n加音量\n降音量")
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
                        self.reply(200,"text/plain","authorized")
                    elif data == "测试手机".encode():
                        waiting.add(key)
                        self.reply(202,"text/plain","123456")
                    elif key in waiting:
                        waiting.remove(key)
                        authorized.add(key)
                        self.reply(200,"text/plain","approved")
                    else:
                        self.reply(403,"text/plain","unknown")
                    return
                if self.path!="/result" and (bluetooth or lan) and key not in authorized:
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
                    if payload.get("action") == "update_source" and payload.get("url") in (
                            "https://updates.test/ota/manifest.json", "https://site.test/pocket/ota/manifest.json"):
                        self.reply(200, "text/plain", "更新地址已保存")
                        return
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
        self.assertEqual(bitmaps, [("/saver", bytes([255]) * 5000), ("/shutdown-saver", bytes([255]) * 5000),
                                   ("/saver", b""), ("/shutdown-saver", b"")])
        self.assertEqual(posts[0]["token"], "")
        self.assertEqual(posts[0]["url"], "wss://saved.test/device")
        self.assertEqual(posts[1], dict(mode="default", ssid="客厅 & Wi-Fi", password=""))
