# Omni 语音网关

设备通过 WSS 上传 16 kHz 单声道 Opus，网关在本机进行语音活动检测。每次发言依次调用兼容接口 `/audio/transcriptions`、`/chat/completions` 和 `/audio/speech`。TTS 接口需支持 24 kHz 单声道、有符号 16 位小端 PCM 输出。对话模型需支持函数调用，以便控制本地音乐。

## 运行

准备 Python 3.11 或更新版本及系统动态库 libopus。在项目根目录安装依赖并启动服务。

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install -r gateway/requirements.txt
python -m gateway.server
```

启动前向服务进程提供以下环境变量。每台设备使用独立的高熵随机令牌，手机端输入同一令牌。设备 MAC 可从烧录日志获取。

| 环境变量 | 值或作用 |
| --- | --- |
| `OMNI_DEVICES` | JSON 对象，小写 MAC 映射到至少 24 字符的设备令牌 |
| `OMNI_PROVIDER_URL` | 上游 HTTPS 基础地址，包含 API 前缀，例如 `https://provider.example/v1` |
| `OMNI_PROVIDER_KEY` | 上游服务 API 密钥，仅保存在网关 |
| `OMNI_ASR`、`OMNI_TTS`、`OMNI_VOICE` | 手机对应字段留空时使用的识别模型、合成模型和音色 |
| `OMNI_TLS_CERT`、`OMNI_TLS_KEY` | 网关域名对应的完整证书链和私钥文件 |
| `OMNI_BIND`、`OMNI_PORT` | 默认监听所有网卡的 8443 端口 |
| `OMNI_OPUS_LIBRARY` | 可选，指定 libopus 动态库路径 |

在手机设置页选择“自建语音网关”，填写 `wss://网关域名:8443/device`、设备令牌和上游实际支持的对话模型名。网关证书链需受设备的 CA 证书包信任。模型和音色随每次连接发送，提供商凭据只在网关访问上游时使用。

网关将每次录音限制在 30 秒内。连接结束时释放音频缓冲区和对话历史。运行回归测试可使用 `python -m unittest discover -s tests -p test_voice_gateway.py`，测试通过本地模拟提供商验证 Opus 收发、模型选择、工具调用和鉴权。

## 中文语音实测

开发机使用 Whisper base、Qwen2.5-0.5B-Instruct 和 Piper huayan，通过本地兼容接口完成了 Opus 输入到语音回复的往返。合成录音“你好，今天天气怎么样？”得到了对应的识别文本。另一段合成录音“请问，一加一等于几？”出现误识别。早期版本在这个用例中发出了下一首指令；给普通对话禁用音乐工具后，重跑时返回了 30 帧语音，没有发出设备指令，但回答内容仍然错误。这组数据可用于检查协议和误操作，不足以评估真人识别率。手机配置的识别模型、对话模型和音色会影响最终效果。
