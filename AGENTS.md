# V2 固件发布

用户要求打包或发布固件时，从仓库根目录运行统一命令：

```sh
./scripts/release_v2.py
```

嘎嘎口袋使用独立版本号，从 `0.0.6` 起编号。板卡的 V2 表示硬件修订。

有新的固件改动要交付给已安装旧版的设备时，先把根目录 `CMakeLists.txt` 的 `PROJECT_VER` 提升为新的三段数字版本。设备只安装比当前版本新的签名镜像。

该命令编译 V2、打 USB 包、签名 OTA 并生成设置网站部署包。它先将镜像放入 `web/ota/`，再更新同目录的 `manifest.json`。若通过 Git 部署 `web/`，将清单和它引用的 `.bin` 一起交付。部署整站时使用 `releases/quack-pocket-site.zip`。默认清单地址是 `https://gaga.omnistreamai.com/ota/manifest.json`。

使用 ESP-IDF 5.5.2。脚本优先调用环境中的 `idf.py`，也可用 `IDF_PY` 指定本机已有的构建命令；否则使用 Docker 的 `espressif/idf:v5.5.2`。签名使用 `~/.local/share/quack-pocket/ota-signing.pem`，可通过 `OTA_SIGNING_KEY` 指定路径。保留现有发布密钥，私钥只存放在发布机。
