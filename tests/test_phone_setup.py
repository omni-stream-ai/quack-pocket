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
        await wait(() => !byId('photo').disabled);
        const pic=document.createElement('canvas');pic.width=pic.height=2;
        const ctx=pic.getContext('2d');ctx.fillStyle='black';ctx.fillRect(0,0,2,2);
        const blob=await new Promise(resolve=>pic.toBlob(resolve));
        const files=new DataTransfer();files.items.add(new File([blob],'test.png',{type:'image/png'}));
        byId('photo').files=files.files;byId('photo').dispatchEvent(new Event('change'));
        await wait(()=>!byId('applyImage').disabled);byId('applyImage').click();
        await wait(()=>byId('imageStatus').textContent==='屏保已保存');
        byId('clearImage').click();await wait(()=>byId('preview').hidden);
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
window.fetch = (path, options) => {
    if(path !== '/result') throw Error('Hosted UI attempted an HTTP device operation');
    return peripheralFetch(path,options);
};
let route, upload, response, status, offset;
const endpoints=['/profile','/networks','/saver','/profile','/saver','/exit'];
const characteristicMock={
    async writeValueWithResponse(frame){
        if(frame.length>20)throw Error('Exceeded ATT MTU');
        switch(frame[0]){
        case 0: route=frame[1];upload=[];response=[];status=offset=0;break;
        case 1: upload.push(...frame.slice(1));break;
        case 2: {
            const options=route<3?{}:{method:'POST',body:new Uint8Array(upload)};
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
    name:'Omni-test',addEventListener(){},
    gatt:{connect:async()=>({getPrimaryService:async()=>({getCharacteristic:async()=>characteristicMock})})}
})}});
</script>"""

@unittest.skipUnless(shutil.which("firefox"), "Firefox is required for the setup page test")
class PhoneSetup(unittest.TestCase):
    def test_recover_load_and_save(self):
        self.exercise_page(bluetooth=False)

    def test_bluetooth_setup(self):
        self.exercise_page(bluetooth=True)

    def exercise_page(self, bluetooth):
        page = PAGE.read_text()
        if bluetooth:
            page = page.replace('<script>', BLUETOOTH + '<script>', 1)
            page = page.replace('</html>', "<script>document.getElementById('connect').click()</script></html>")
        else:
            page = page.replace("location.hostname === '192.168.4.1'", "location.hostname === '127.0.0.1'")
        posts, results, bitmaps = [], [], []
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
                self.wfile.write(body.encode())

            def do_GET(self):
                if self.path == "/":
                    self.reply(200, "text/html", page.replace("</html>", PROBE + "</html>"))
                elif self.path == "/saver":
                    self.reply(204, "application/octet-stream", "")
                elif self.path == "/networks":
                    Handler.scans += 1
                    self.reply(503 if Handler.scans == 1 else 200, "application/json", json.dumps(["客厅 & Wi-Fi"]))
                elif self.path == "/profile":
                    Handler.reads += 1
                    profile = dict(url="wss://saved.test/device", has_token=True, has_wifi=True, model="chat", saved_wifi=networks,
                                   offline_words="播放\n暂停\n下一首\n上一首\n加音量\n降音量")
                    profile["offline_words"] = "播放\n暂停\n下一首\n上一首\n加音量\n降音量"
                    self.reply(503 if Handler.reads == 1 else 200, "application/json", json.dumps(profile))
                else:
                    self.send_error(404)

            def do_POST(self):
                data = self.rfile.read(int(self.headers["Content-Length"]))
                if self.path == "/saver":
                    bitmaps.append(data)
                    self.reply(200, "text/plain", "屏保已保存" if data else "屏保已关闭")
                    return
                body = data.decode()
                if self.path == "/result":
                    results.append(body)
                    self.reply(200, "text/plain", "")
                    finished.set()
                elif self.path == "/profile":
                    payload = json.loads(body)
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
        self.assertEqual(bitmaps, [bytes([255]) * 5000, b""])
        self.assertEqual(posts[0]["token"], "")
        self.assertEqual(posts[0]["url"], "wss://saved.test/device")
        self.assertEqual(posts[1], dict(mode="default", ssid="客厅 & Wi-Fi", password=""))
