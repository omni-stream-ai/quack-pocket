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

对话、听写、朗读可以分别使用不同的服务商。为网关进程设置对应的 API 基础地址及密钥：

| 环节 | 基础地址 | 服务端密钥 |
|---|---|---|
| 对话大模型 | `OMNI_LLM_URL` | `OMNI_LLM_KEY` |
| 语音识别 | `OMNI_ASR_URL` | `OMNI_ASR_KEY` |
| 语音合成 | `OMNI_TTS_URL` | `OMNI_TTS_KEY` |

未设置独立地址的环节继续使用 `OMNI_PROVIDER_URL` 和 `OMNI_PROVIDER_KEY`。如果三个地址均已单独配置，共享地址和密钥可省略。指向不同地址时请为该环节单独提供密钥；无鉴权的本机适配服务可留空，回环地址可用 HTTP。更改环境变量后需重启网关。各环节需提供上文列出的兼容接口。

在手机设置页选择“自建语音网关”，填写 `wss://网关域名:8443/device`、设备令牌和上游实际支持的对话模型名。网关证书链需受设备的 CA 证书包信任。模型和音色随每次连接发送，提供商凭据只在网关访问上游时使用。

网关将每次录音限制在 30 秒内。连接结束时释放音频缓冲区和对话历史。运行回归测试可使用 `python -m unittest discover -s tests -p test_voice_gateway.py`，测试通过本地模拟提供商验证 Opus 收发、模型选择、工具调用和鉴权。

## 对话接口类型

在「自建语音网关 → 对话大模型 → 接口类型」选择协议，并填写该上游支持的模型 ID。旧配置默认使用 Chat Completions。首次配置点「保存并重启」，之后可单独保存对话模型。

| 接口类型 | 网关配置 | 调用方式 |
|---|---|---|
| OpenAI Chat Completions | `OMNI_LLM_URL`、`OMNI_LLM_KEY` | 基础地址追加 `/chat/completions` |
| OpenAI Responses | 可选 `OMNI_RESPONSES_URL`、`OMNI_RESPONSES_KEY` | 基础地址追加 `/responses` |
| Anthropic Messages | 可选 `OMNI_ANTHROPIC_URL`、`OMNI_ANTHROPIC_KEY` | 例如基础地址 `https://api.anthropic.com/v1`，网关追加 `/messages` 并使用 `x-api-key` |
| 千问 Omni Realtime | `OMNI_REALTIME_URL`、`OMNI_REALTIME_KEY` | 使用供应商提供的完整 WSS 地址，网关在查询参数中设置所选模型 ID |

未单独设置 Responses 或 Messages 地址时使用现有 LLM 地址和密钥；另设地址时，请同时配置该协议自己的密钥。三种文本协议均沿用独立语音识别和语音合成，也支持按设备授权范围调用音乐和音量等工具。

千问实时模式直接接收 16 kHz 单声道 PCM，返回的 24 kHz PCM 由网关重新编码为设备使用的 Opus。设备边说边上传，停说后模型流式回答。同一通话复用上游连接保留上下文；打断后结束该上游会话。可在「语音识别与合成」指定实时模型支持的音色或留空使用模型默认值。这个模式提供语音对话，设备工具调用和依赖转写的本地口令使用前三种协议。仅使用实时模式时可以只设置实时上游，无需配置独立 ASR、TTS 或文本 LLM。

例如，北京地域使用绑定业务空间的地址时，设置 `OMNI_REALTIME_URL=wss://<业务空间ID>.cn-beijing.maas.aliyuncs.com/api-ws/v1/realtime`，将该空间的百炼 API Key 配到网关的 `OMNI_REALTIME_KEY`，在设备网页填所开通的实时模型 ID。不同地域和模型支持的连接地址、音色以服务商文档为准。[千问实时语音文档](https://help.aliyun.com/zh/model-studio/realtime)

## 中文语音实测

开发机使用 Whisper base、Qwen2.5-0.5B-Instruct 和 Piper huayan，通过本地兼容接口完成了 Opus 输入到语音回复的往返。合成录音“你好，今天天气怎么样？”得到了对应的识别文本。另一段合成录音“请问，一加一等于几？”出现误识别。早期版本在这个用例中发出了下一首指令；给普通对话禁用音乐工具后，重跑时返回了 30 帧语音，没有发出设备指令，但回答内容仍然错误。这组数据可用于检查协议和误操作，不足以评估真人识别率。手机配置的识别模型、对话模型和音色会影响最终效果。
