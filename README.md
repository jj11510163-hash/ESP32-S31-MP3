# ESP32-S31 LCD 音乐播放器

基于慧勤智远 ESP32-S31 CB V1.1 小系统板的 SD 卡蓝牙音乐播放器。使用 WKS35HV031-WCT 3.5 英寸、480×320 ST7796U LCD 和 GT1151Q 触摸屏；音频经经典蓝牙 A2DP 输出到耳机，按设备能力协商 AAC-LC 或 SBC。当前没有板载扬声器输出。

## 功能

- 播放 MP3、FLAC、PCM/float WAV、ADTS AAC 和 AAC 编码的 M4A；支持长曲目的 `时:分:秒` 进度显示及 FAT32 上超过 2 GB 的 WAV 跳转。AAC/M4A 暂不支持快进，M4A 的 `moov` 元数据需位于音频数据之前。
- 播放页显示封面、同名 UTF-8 `.lrc` 歌词与播放控件。曲库可按歌名首字、歌手、专辑、发行方或目录筛选，并有独立收藏页。
- 触摸点击、竖滑列表、横滑切换页面；KEY0 和耳机 AVRCP 可控制播放。支持全部循环、单曲循环、随机播放及耳机绝对音量。
- Wi-Fi 热点网页可批量传歌、查看和删除文件；上传后自动刷新曲库。SD 卡上有缓存，开机时会检查歌曲变化并在需要时重建。
- 支持从 SD 卡更新应用固件，首次启用双应用槽需经串口完整烧录。

界面、引脚、触摸手势和格式限制详见 [LCD_HOME.md](LCD_HOME.md)；SD 卡更新步骤详见 [SD_UPDATE.md](SD_UPDATE.md)。

## 准备与构建

- 硬件：ESP32-S31 CB V1.1 小系统板、上述 LCD/触摸屏、FAT32 SD 卡，以及支持经典蓝牙音频的耳机。
- 工具：支持 `esp32s31` 目标的 ESP-IDF v6.1。在已激活 ESP-IDF 环境的终端，从仓库根目录运行：

```powershell
idf.py -B build-s1 build
idf.py -B build-s1 -p COMx -b 921600 flash monitor
```

将 `COMx` 换成实际串口。构建会按 `dependencies.lock` 下载组件，生成的应用镜像位于 `build-s1/s31_player_lcd.bin`。`sdkconfig` 和 `sdkconfig.defaults` 已包含目标芯片、PSRAM、蓝牙和分区配置。

## 音乐与传歌

在 SD 卡建立 `music` 文件夹，把歌曲放入其中。若 `music` 中没有歌曲，程序才会扫描 SD 卡根目录。新增或删除文件后重新开机，曲库缓存会按文件变化自动更新。

也可以在设置中打开“无线传歌”，连接 `S31-Player-xxxx` 热点，默认密码 `s31player`，然后访问 [http://192.168.4.1/](http://192.168.4.1/) 上传音乐。网页保存歌曲后会自动触发曲库刷新。

封面优先使用歌曲同名 `.jpg`，其次使用目录里的 `folder.jpg`；歌词使用歌曲同名、UTF-8 编码的 `.lrc`。界面预览图位于仓库根目录，属于模拟画面。

## 固件更新

首次迁移到双应用槽分区表时，使用上面的串口命令完整烧录。之后每次发布先递增 `version.txt` 并构建，再将应用镜像复制到 SD 卡的 `update/firmware.bin`，在屏幕“设置 → 固件更新”中安装。具体限制和步骤见 [SD_UPDATE.md](SD_UPDATE.md)。

## 源码结构

| 路径 | 内容 |
| --- | --- |
| `main/` | 启动、初始化和 SD 卡挂载 |
| `components/music/` | 曲库、缓存、解码与播放器 |
| `components/bt_audio/` | A2DP、AAC/SBC、AVRCP |
| `components/ui/` | LCD、触摸、字库与页面 |
| `components/net_service/` | 网页遥控和传歌 |
| `components/fw_update/` | SD 卡固件更新 |
| `tests/` | 独立的 WAV 解码器主机端检查 |

`managed_components/` 和 `build*/` 是本地生成目录，不提交到仓库；构建必需的嵌入式字库位于 `components/ui/assets/`。
