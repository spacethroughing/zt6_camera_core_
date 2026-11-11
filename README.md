# ZT6 核心源码部署包

打包日期：2026-11-09；本次更新：2026-11-10。用于 RK3588 飞机 cat@100.64.0.56。

包含 C++ 控制核心、独立 CLI、C++ Web 后端、网页、必需依赖头文件和部署/启动脚本。
支持可见光/热像/拼接预览、姿态控制与回中、拍照录像、机载/SD 保存。
不包含 Python 历史实现、测试、厂商 PDF、验收证据、媒体、状态文件或本机二进制。
完整开发工作空间仍保存在 /home/lmw/Desktop/zt6_camera_ws。

飞机现已运行 C++ 版；2026-11-09 已现场修复并验证 SD 文件列表接口。
本包同步修复了相机 HTTP 混合 CRLF/LF 响应头及文件列表参数顺序兼容。
运行不依赖 Python 或 ROS；视频编解码和封装使用 FFmpeg。网页交互使用少量 JavaScript。

## 1. 校验、部署

在本机进入解压得到的本目录，执行：

```bash
sha256sum -c SHA256SUMS
bash scripts/deploy.sh cat@100.64.0.56 zt6_camera_ws_cpp
```

按 SSH 提示输入密码。脚本上传源码，在飞机编译并输出两个入口的帮助；不自动操作相机。
目标目录已存在时拒绝覆盖。新目录保留旧 Python ~/zt6_camera_ws。
需要另一个目录时，将最后一个参数改为新名称，随后启动 Web 时同步设置 ZT6_REMOTE_DIR。

飞机需有 C++17 编译器（g++ 9+）、CMake 3.16+、FFmpeg / FFprobe、tar。
此前环境已有依赖；仅缺失时在飞机执行：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ffmpeg
```

如果部署已传完但编译失败，在飞机 ~/zt6_camera_ws_cpp 补齐依赖后运行 bash scripts/build.sh。
本包不含测试，不运行 CTest；完整工作空间的 42 项 C++ 测试和 19 组离线联调已通过。
ARM 构建与 SD 文件列表已现场验证；真机视频/运动、SD 本次新文件落盘及其他设置仍应独立验收。

也可手动将本包解压到飞机，在该目录执行 bash scripts/build.sh。
必须在 ARM 飞机重新编译；本机编译产物不能直接用于 RK3588。
编译目录与源码目录应保留在一起，Web 按编译时源码路径读取网页资源。

## 2. 独立命令行

在本机先 SSH 登录，之后命令在飞机执行：

```bash
ssh cat@100.64.0.56
cd ~/zt6_camera_ws_cpp
./bin/zt6 version
./bin/zt6 status --full
./bin/zt6 auto-record
./bin/zt6 doctor
```

相机默认为 192.168.144.25，UDP 37260，RTSP 8554/video1。
此前飞机相机网口为 eth1=192.168.144.50/24，重启后核对 ip route get 192.168.144.25。
status 正常退出时仍须查看是否有 sdk_error / telemetry_error。
自动录像使用相机 TCP 37256；需要手动录像时可显式执行 ./bin/zt6 auto-record off 并读回。

```bash
./bin/zt6 storage onboard
./bin/zt6 photo
./bin/zt6 record start
./bin/zt6 record stop
./bin/zt6 files --kind video
./bin/zt6 --storage sd photo
./bin/zt6 --storage sd record start
./bin/zt6 --storage sd record stop
./bin/zt6 --storage sd files --kind video
```

默认机载媒体 ~/zt6_camera_data，配置/进程信息 ~/.local/state/zt6-camera。
机载照片为主流 JPEG 截帧，视频直接复制压缩流到 MKV；SD 方式为云台原生保存。
默认位置通过 storage onboard / storage sd 设置；先停止已有录像再切换，文件不自动迁移。
无卡时 SD 操作报错，不自动改存机载。

后台 record start 在 CLI / SSH / Web 退出后继续；需要明确执行对应位置的 record stop。
record start --seconds 5 是前台定时录像，应保持连接。新旧程序用同一用户/状态目录并避免并发控制。

```bash
./bin/zt6 attitude
./bin/zt6 move --yaw 8 --pitch 0 --seconds 0.15
./bin/zt6 stop
./bin/zt6 center
./bin/zt6 angle --yaw 10 --pitch -5
./bin/zt6 view visible
./bin/zt6 view thermal
./bin/zt6 view pip
```

在地面固定、云台有运动余量时，先低速核验方向和倒装坐标，再执行回中/目标角。
move 是速度，单次最长 1 秒，结束尝试归零；stop 只停止运动，录像用 record stop。
角度目标 yaw -270..270、pitch -90..25（度），SDK 姿态保留原始坐标，倒装不自动补偿。
`angle` / `center` 在 ACK 后继续查询实际姿态，稳定到位返回 `status: completed`、`message: 已完成`。默认到位等待 10 秒、误差 1°，可用 `--wait-seconds 15 --tolerance 0.5` 调整；超时或中断不返回完成，也不重复发运动指令。录像期间不能切换画面。

## 3. 本机 Web GUI

在本机进入本包目录，执行：

```bash
bash scripts/start_web.sh cat@100.64.0.56
```

本机浏览器打开 http://127.0.0.1:18766，保留启动终端。
飞机 Web 仅监听 127.0.0.1:18765，通过 SSH 转发；直接访问飞机 IP 的该端口不会打开页面。
按住方向按钮移动、松开停止；方向键控制，Space 停止方向运动，Home 回中。
界面支持保存位置、拍照录像、姿态/目标角、自动录像开关和文件列表；机载与 SD 文件均可下载到本机浏览器。
默认预览 8 FPS / 960px，热像是相机输出画面，不含测温或热原始数据。
Ctrl+C 关闭 Web 与隧道；后台录像应先显式停止。

换远端源码目录或本机访问端口时，在本机执行，例如：

```bash
ZT6_REMOTE_DIR=zt6_camera_ws_cpp_new ZT6_WEB_LOCAL_PORT=18767 bash scripts/start_web.sh cat@100.64.0.56
```

目录需与实际部署一致。CLI 可在另一个 SSH 终端独立使用；锁冲突时等当前操作结束再查询状态。
本包没有开机自启。相机配置与录像位置不会随重启网页自动修改。

## 4. 真机验收与关机

检查机载照片/视频的实际文件、尺寸、时长与可解码性；可从 GUI 下载到本机。
SD 必须比较拍摄前后文件列表，确认本次新增文件可读取/解码。
此前旧版只确认 SD 录像状态，未确认那次新视频落盘，既有视频约 64.74 GB，剩余空间待查。
相机 SD 容量不能通过飞机 df 判断，本版没有已验证的 SD 容量查询接口。

结束前在飞机执行：

```bash
./bin/zt6 stop
./bin/zt6 --storage onboard record stop
./bin/zt6 --storage sd record stop
./bin/zt6 status --full
```

正常有卡时确认 onboard.active=false、camera.record_state=0，再退出 Web 与 SSH，按飞机流程关机。
无卡时 SD stop 可报无卡，状态 2 表示没有 SD。若 SD 停止后自行恢复，查询自动录像开关并处理后复查。

详细指南仍在本机完整工作空间 docs/真机部署与使用指南.md。
JSON/HTTP 第三方库来源与 MIT 许可证说明见 third_party/README.md、third_party/LICENSES.txt 和各头文件。

## SD HTTP 兼容修复

CMake 为核心、CLI、Web 一致启用 CPPHTTPLIB_ALLOW_LF_AS_LINE_TERMINATOR，兼容相机 Boa 的混合 CRLF/LF 响应头。
文件列表 URL 显式按 media_type、path、start、count 排列，避免 JSON 默认排序触发固件参数错误。
当前飞机 CLI 已更新；正在运行的旧 Web 需退出后重新启动，才会加载新二进制。

## SD 文件直接从网页下载

在网页选择“云台 SD 卡”→选择照片/录像→刷新列表→点击文件右侧“下载”。
照片和 MP4 经飞机 C++ 服务及现有 SSH 隧道分块传给本机浏览器，不需要先保存到飞机。
支持单段范围请求/续传，最多同时两个 SD 下载；下载完成的录像文件，避免文件大小还在变化。
更新后需在原 Web 启动终端 Ctrl+C，再运行 scripts/start_web.sh 并刷新浏览器，加载新版程序及页面。

下载 SD 文件期间保持 Web 启动终端和 SSH 隧道连接；视频分块传输，不会先复制到机载媒体目录。

## 控制锁修复（2026-11-10，已部署并只读验证）

本包已包含交互命令最多等待 2 秒、Web 后台轮询遇忙跳过且每条查询最多 0.5 秒的修复。
方向 move 不排队，stop 继续绕过普通锁；等待锁时 Ctrl+C 不发送后续控制指令。
已确认飞机原 Web PID 45198 的后台查询会与旧 CLI 争锁。本包对应修复已在飞机重新编译部署。
原源码与二进制备份在 ~/zt6_camera_ws_cpp/backups/control_lock_20261110_100322。
RK3588 的 20 项离线模拟测试通过；新版 Web 持锁时 10 次 CLI 姿态查询及 3 次完整状态查询均成功。
本轮未执行真实云台运动或拍照录像。旧 Web 和临时验证服务已退出，需重新运行本机 scripts/start_web.sh 恢复网页。
若仍提示占用，在飞机查看 lslocks --notruncate -o PID,COMMAND,PATH，核对 control.lock 的持有者。
锁由进程持有，不要通过删除 control.lock 绕过；长时间录像等真实操作仍需结束对应任务。

## 姿态动作完成确认（2026-11-10）

目标角度与回中共用 C++ `SDK::wait_attitude()`：角度误差达标、yaw/pitch 角速度均不超过 2°/s，
至少 3 次反馈连续稳定 0.3 秒后才返回“已完成”。网页活动日志与提示同步显示完成。
本台倒装 ZT6 已实测原始 pitch=目标+180°，回中为 ±180°；控制坐标仅在内部换算用于到位判断，返回结果已移除 `position`，
`attitude` 命令及网页实时姿态保留原始 SDK 读数。正装分支本轮仅模拟验证。
动作只发送一次；超时或 Ctrl+C 仅停止等待，不撤回已发动作，需要停止运动时另执行 `stop`。
`move` / `stop` 仍只表示点动结束/停止报文已发送，不新增机械到位保证。
本机 26 项 C++、17 组联调及 RK3588 上 26 项模拟测试通过，已备份部署到飞机；CLI / Web API 的真实小角度、回中和超时测试均通过。备份为 ~/zt6_camera_ws_cpp/backups/attitude_completion_20261110_103235。最后已回中并退出临时 Web，使用网页请重新运行本机启动脚本并刷新浏览器。详见完整工作空间 docs/姿态动作完成确认_2026-11-10.md。


## success 布尔值补充

姿态 angle / center 确认完成时返回 `success: true`，错误或中断返回 `success: false`。
CLI 错误也在 stdout 输出 JSON，stderr 保留文字提示，退出码继续使用 2/130。
Web 姿态动作的外层和 result 中均有 success；标准 JSON HTTP 错误外层为 false。
输出按最新要求移除 `position`，`attitude` 仍为原始 SDK 反馈，`success` 含义不变。
本机 27 项 C++ 测试和 17 组联调通过，阈值、字段说明见完整工作空间 docs/姿态输出字段与阈值说明_2026-11-10.md。

本次 success 补充也已备份部署至飞机，并通过 CLI/Web 完成、超时和参数错误验证；RK3588 上 27 项模拟测试通过。备份：~/zt6_camera_ws_cpp/backups/success_flag_20261110_130257。


## 相机 SDK 扩展（2026-11-10）

```bash
./bin/zt6 resolution --stream main
./bin/zt6 view visible
./bin/zt6 resolution --width 1280 --height 720
./bin/zt6 resolution --width 1920 --height 1080
./bin/zt6 gimbal-mode lock
./bin/zt6 gimbal-mode follow
./bin/zt6 gimbal-mode fpv
./bin/zt6 view pip
./bin/zt6 thermal-frame
./bin/zt6 thermal-output temperature
./bin/zt6 thermal-output video
```

编码设置修改相机实际 RTSP 输出，支持公开 SDK 的 720p/1080p，修改后视频约需 3 秒重连。
640×512 等设备原生尺寸仅查询，不发送未公开的编码尺寸写入。
reapply 即使 SDK 查询值相同也只重新应用一次；applied 表示本次发了写指令，changed 表示查询参数有差异。
机载或 SD 正在录像时禁止修改编码及热像输出模式。可选 --stream main/sub/record、--codec、--bitrate-kbps。

云台模式写后读回确认。数字倍率及热像联动 SDK 代码保留，但 CLI/Web 入口已按用户要求停用。
温度单帧请求独立可用，只返回相机接受 ACK，不接收矩阵，结果明确 frame_received:false。
当前真机连续温度帧模式设置 ACK=1，但 UDP/TCP 查询仍为 video，程序会返回未确认错误，不显示已开启。
数字变倍曾完成 1→2→1 测试，后续 0x16/0x18 在 UDP/TCP 均无回包。
按用户最后要求，变倍和热像联动暂未启用：CLI/Web 统一立即拒绝，不向相机发送查询或设置，网页控件禁用，读取设置跳过倍率查询。SDK 实现及离线测试保留。
本轮未刷写固件。新设置不会在打开 GUI 时自动写入，面板中点击“读取设置”获取设备当前值。

SDK 阶段（连拍扩展前）回归为 36 项 C++，本机 HTTP/FFmpeg/Firefox 联调为 18 组；硬件验证范围和使用细节见完整工作空间
 docs/SDK扩展_分辨率_模式_温度帧_变倍.md。


## 拍照连拍（2026-11-10）

```bash
./bin/zt6 photo --count 5
./bin/zt6 --storage sd photo --count 5
./bin/zt6 --storage onboard photo --count 3
```

count 为 1–100 的整数，默认 1；单张原有输出保持不变。
逐张确认，每张成功后额外等待 1 秒；中途失败/反馈超时/中断则停止，不自动重拍或补拍。
多张输出有 success、requested_count、completed_count、results；失败保留已确认结果，failed_index 从 1 开始。
SD 完成计数依据相机成功反馈，文件落盘仍需 files 核对；机载 results 每项带 path。
前台连拍需保持 SSH，Ctrl+C 停止后续拍摄。网页有“拍照张数”输入，默认 1，范围相同。
变倍与热像联动继续保持停用。完整说明见开发工作空间 docs/连拍功能使用说明_2026-11-10.md。
本机最新回归为 41 项 C++、19 组 HTTP/FFmpeg/Firefox 联调；此前 SDK 阶段的 36/18 记录保留作历史。

连拍已在飞机备份部署：backups/photo_burst_20261110_144127。机载 41 项 C++ 回归、真实 CLI 两种存储各 2 张、Web API 机载 2 张及下载核对通过。部署前 SD 正在录像，测试后保持录像状态。临时 Web 已退出，使用网页需重新运行启动脚本。


## SD 录像流 2K/4K 补充（2026-11-10）

```bash
./bin/zt6 resolution --stream record
./bin/zt6 resolution --stream record --width 2560 --height 1440 --bitrate-kbps 15000
./bin/zt6 resolution --stream record --width 3840 --height 2160 --bitrate-kbps 15000
```

设置前先停止正在进行的 SD/机载录像。录像流新增尺寸来自用户手册 v1.3 第 70 页 SDK 示例。
主/副流设置仍为 720p/1080p，不据此开放 4K RTSP 设置。录像流编码格式保持原值，码率未指定时保留。
机载录制使用 -c:v copy，无硬编码 1080p 上限；实际尺寸取决于 RTSP，不取决于 SD 文件尺寸或网页预览宽度。
网页选择“录像流”后可选 2K/4K。最后一次只读查询：SD 配置 3840×2160、30fps、H265、20000Kbps；主流配置 1280×720。
硬件当时报告无 SD 卡，本轮没有执行相机设置或录制，不声称新 4K 参数切换及文件落盘已实测。
完整说明见开发工作空间 docs/SD录像规格与机载分辨率_2026-11-10.md。
最新本机回归 42 项 C++、19 组 HTTP/FFmpeg/Firefox 联调通过。
