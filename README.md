# brick-chrome

在 **TRIMUI Brick Pro (TG4040)** 的原厂 Tina Linux 上跑一个真正的 Google Chrome：
装在隔离的 Ubuntu chroot 里，用精简 X11 直接接管掌机屏幕，手柄当鼠标键盘，
带方向键操作的屏幕键盘和按住 MENU 的语音输入。不替换原厂系统，从原厂应用列表启动，
退出就回到原厂界面。

> Runs a real Google Chrome on the stock firmware of a TRIMUI Brick Pro (TG4040):
> an isolated Ubuntu 22.04 chroot with a minimal Xorg fbdev server on the handheld's
> own framebuffer, the gamepad driving a pointer and keyboard, a D-pad-navigable
> on-screen keyboard, and push-to-talk voice input through Chrome's Web Speech API.
> Nothing in the stock system is replaced.

---

## 目录

- [能做什么](#能做什么)
- [硬件与前提](#硬件与前提)
- [工作原理](#工作原理)
- [完整安装教程](#完整安装教程)
- [手柄操作](#手柄操作)
- [可调配置](#可调配置)
- [故障排查](#故障排查)
- [踩过的坑](#踩过的坑)
- [仓库内容](#仓库内容)

---

## 能做什么

- 在掌机屏幕上跑完整的桌面版 Chrome（不是 WebView，不是精简浏览器）
- 左摇杆当鼠标，A 键点击，方向键滚动/翻页
- 屏幕键盘用**方向键选字母**，不用指针去戳
- **按住 MENU 说话**，松开自动把识别结果输入到光标处（地址栏和网页输入框都行）
- 中文显示正常，Chrome 界面为简体中文
- 麦克风可用（网页 `getUserMedia` / 语音搜索）
- 界面缩放、指针速度、首页都是文本文件配置，改完重启即可

---

## 硬件与前提

| 项 | 值 |
|---|---|
| 设备 | TRIMUI Brick Pro / TG4040 |
| 系统 | 原厂 Tina Linux（未 root 改造，仅用 SSH） |
| CPU | 4 × ARM Cortex-A53 (aarch64) |
| 内存 | 约 975 MB |
| 屏幕 | 1024 × 768，约 3.2 吋（≈400 PPI） |
| 帧缓冲 | `/dev/fb0`，1024×768，32bpp，stride 4096 |
| 手柄 | `/dev/input/event3`，以 Xbox 360 手柄（045e:028e）形式上报 |
| 声卡 | `audiocodec`，采集 `hw:0,0` |

**占用空间**：chroot 装完约 **1.3 GB**，放在 `/opt`（overlay 根分区，总共约 1.9 GB）。
开工前根分区至少要有 1.5 GB 空闲。

**宿主机需要**：`ssh`、`scp`、`expect`、`curl`。掌机和电脑在同一局域网。

**需要先在掌机上打开 SSH**（设置里的开关，对应 `/mnt/UDISK/system.json` 的 `enablessh`）。
root 密码用原厂默认值 —— 这是 Allwinner Tina Linux 的出厂默认口令，各家 TrimUI 教程里都有，
本仓库不写出来。装好后建议改掉。

---

## 工作原理

```
┌─ 原厂 Tina Linux ────────────────────────────────────────────┐
│                                                              │
│  MainUI（原厂启动器）                                         │
│    └─ 点 Chrome 图标 → 写 /tmp/cmd_to_run.sh 然后自己退出      │
│                                                              │
│  runtrimui.sh（守护循环，MainUI 不在时执行那个脚本）            │
│    └─ /mnt/SDCARD/Apps/Chrome/launch.sh                      │
│         └─ /opt/brick-chrome/brick-x11.sh                    │
│              ├─ bind mount /proc /sys /dev /dev/pts 进 chroot │
│              ├─ Xorg :1  (fbdev → /dev/fb0)                  │
│              ├─ brick-pad.py  手柄 → X11 指针/按键            │
│              └─ chroot 里启动 Chrome                          │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

关键点是 **MainUI 退出后才执行 launch.sh**，所以帧缓冲是空的，Xorg 可以直接接管；
Chrome 退出后脚本返回，`runtrimui.sh` 循环重新拉起 MainUI，屏幕回到原厂界面。

`brick-pad.py` 是整套东西的中枢：读 evdev 手柄事件，用 XTEST 合成 X11 的指针和按键，
管理屏幕键盘覆盖层和语音状态条，并且因为没有窗口管理器，它还负责把输入焦点钉在
Chrome 窗口上。

---

## 完整安装教程

下面每一步都在宿主机上执行。先把这个仓库克隆下来，`cd` 进去。

### 0. 设好环境变量

```bash
export BRICK_HOST=192.168.x.x        # 掌机 IP
export BRICK_PASS='<原厂 root 密码>'   # 不要写进任何文件
export BRICK_KNOWN_HOSTS=~/.brick_known_hosts

ssh-keyscan -T 5 -t ed25519 "$BRICK_HOST" > "$BRICK_KNOWN_HOSTS"
ssh-keygen -lf "$BRICK_KNOWN_HOSTS" -E sha256     # 核对指纹再继续
```

仓库里的 `tools/brick-run` 和 `tools/brick-scp.exp` 都从这几个变量取值，
**不会在文件里写死任何凭据**。

```bash
echo 'echo hello from $(uname -m)' | tools/brick-run -
```

### 1. 下载 Ubuntu base 和 Chrome

```bash
curl -fL -o ubuntu-base-22.04.5-base-arm64.tar.gz \
  https://cdimage.ubuntu.com/ubuntu-base/releases/jammy/release/ubuntu-base-22.04.5-base-arm64.tar.gz
curl -fL -o SHA256SUMS \
  https://cdimage.ubuntu.com/ubuntu-base/releases/jammy/release/SHA256SUMS

grep ubuntu-base-22.04.5-base-arm64.tar.gz SHA256SUMS
shasum -a 256 ubuntu-base-22.04.5-base-arm64.tar.gz    # 两行要一致

curl -fL -o google-chrome-stable_current_arm64.deb \
  https://dl.google.com/linux/direct/google-chrome-stable_current_arm64.deb
```

### 2. 解开 rootfs

```bash
tools/brick-scp.exp ubuntu-base-22.04.5-base-arm64.tar.gz /tmp/brick-ubuntu-base.tar.gz

tools/brick-run - <<'SH'
mkdir -p /opt/brick-chrome
busybox tar -xzf /tmp/brick-ubuntu-base.tar.gz -C /opt/brick-chrome
rm -f /tmp/brick-ubuntu-base.tar.gz
df -h /
SH
```

### 3. 挂载、DNS、apt 配置

```bash
tools/brick-run - <<'SH'
for m in /proc /sys /dev /dev/pts; do
  mkdir -p /opt/brick-chrome$m
  grep -q " /opt/brick-chrome$m " /proc/mounts || mount -o bind $m /opt/brick-chrome$m
done
cp /etc/resolv.conf /opt/brick-chrome/etc/resolv.conf

cat > /opt/brick-chrome/etc/apt/sources.list <<'EOF'
deb http://ports.ubuntu.com/ubuntu-ports/ jammy main universe
deb http://ports.ubuntu.com/ubuntu-ports/ jammy-updates main universe
deb http://ports.ubuntu.com/ubuntu-ports/ jammy-security main universe
EOF

cat > /opt/brick-chrome/etc/apt/apt.conf.d/99brick <<'EOF'
APT::Sandbox::User "root";
Acquire::Languages "none";
APT::Install-Recommends "false";
EOF

printf '#!/bin/sh\nexit 101\n' > /opt/brick-chrome/usr/sbin/policy-rc.d
chmod +x /opt/brick-chrome/usr/sbin/policy-rc.d

chroot /opt/brick-chrome /usr/bin/apt-get update
SH
```

> `APT::Sandbox::User "root"` 是必需的。apt 默认会降权到 `_apt` 用户去下载，
> 而在这个环境里那个用户解析不了 DNS，`apt-get update` 会静默失败。
> `policy-rc.d` 返回 101 是为了阻止 deb 的 postinst 去启动 systemd 服务（chroot 里没有）。

### 4. 装 X11、Python、字体、音频

```bash
tools/brick-run - <<'SH'
export DEBIAN_FRONTEND=noninteractive
chroot /opt/brick-chrome /usr/bin/apt-get install -y -qq \
  xserver-xorg-core xserver-xorg-video-fbdev xserver-xorg-input-evdev xinit xterm \
  python3-minimal python3-evdev python3-xlib python3-pil python3-websocket \
  fonts-wqy-microhei fonts-wqy-zenhei alsa-utils
chroot /opt/brick-chrome /usr/bin/apt-get clean
df -h /
SH
```

字体是必须的：Ubuntu base 里**一个 CJK 字体都没有**，不装的话所有中文都是方块。

### 5. 放 xorg.conf

```bash
tools/brick-scp.exp chroot/etc/X11/xorg.conf /opt/brick-chrome/etc/X11/xorg.conf
```

`AutoAddDevices false` —— 手柄不交给 Xorg，由 `brick-pad.py` 自己读 evdev 再用 XTEST 合成，
这样才能做出摇杆加速曲线、屏幕键盘和语音这些逻辑。

### 6. 装 Chrome

```bash
tools/brick-scp.exp google-chrome-stable_current_arm64.deb \
  /opt/brick-chrome/tmp/google-chrome-stable_current_arm64.deb

tools/brick-run - <<'SH'
export DEBIAN_FRONTEND=noninteractive
chroot /opt/brick-chrome /usr/bin/apt-get install -y -qq \
  /tmp/google-chrome-stable_current_arm64.deb
rm -f /opt/brick-chrome/tmp/google-chrome-stable_current_arm64.deb
chroot /opt/brick-chrome /usr/bin/apt-get clean
chroot /opt/brick-chrome /usr/bin/google-chrome-stable --version
SH
```

### 7. 建运行用户并给权限

```bash
tools/brick-run - <<'SH'
chroot /opt/brick-chrome /usr/sbin/useradd -m -u 1000 -s /bin/sh brick 2>/dev/null || true
chroot /opt/brick-chrome /usr/sbin/groupadd -g 3003 inet 2>/dev/null || true
chroot /opt/brick-chrome /usr/sbin/usermod -aG inet,audio brick
chroot /opt/brick-chrome /usr/bin/id brick
SH
```

两个组都不能少：

- **`inet` (gid 3003)** —— 这个内核开了 Android 式的 paranoid network，
  不在这个组里的进程连 socket 都建不出来，Chrome 会完全没有网络。
- **`audio` (gid 29)** —— `/dev/snd/pcmC0D0c` 属主是 `root:audio`。
  少了它 Chrome 打不开采集设备，网页请求麦克风会静默失败。

### 8. 放脚本

```bash
tools/brick-scp.exp chroot/brick-x11.sh                       /opt/brick-chrome/brick-x11.sh
for f in brick-chrome-start brick-pad.py brick_keyboard.py brick_speech.py brick_typing.py; do
  tools/brick-scp.exp "chroot/usr/local/bin/$f" "/opt/brick-chrome/usr/local/bin/$f"
done

tools/brick-run - <<'SH'
chmod +x /opt/brick-chrome/brick-x11.sh \
         /opt/brick-chrome/usr/local/bin/brick-chrome-start \
         /opt/brick-chrome/usr/local/bin/brick-pad.py
SH
```

### 9. 加到原厂应用列表

```bash
tools/brick-run - <<'SH'
mkdir -p /mnt/SDCARD/Apps/Chrome
# 图标直接用 Chrome 自带的（本仓库不分发 Google 的商标资源）
cp /opt/brick-chrome/opt/google/chrome/product_logo_256.png /mnt/SDCARD/Apps/Chrome/icon.png
SH

for f in config.json launch.sh CONTROLS.txt scale.txt pointer.txt homepage.txt; do
  tools/brick-scp.exp "sdcard/Apps/Chrome/$f" "/mnt/SDCARD/Apps/Chrome/$f"
done

tools/brick-run - <<'SH'
chmod +x /mnt/SDCARD/Apps/Chrome/launch.sh
sync
# MainUI 只在启动时扫描一次 Apps 目录，必须重启它图标才会出现
killall -9 MainUI
SH
```

等几秒，掌机应用列表里就会出现 **Chrome 浏览器**。点它启动。

验证 MainUI 确实收录了：

```bash
echo "grep -a Chrome /tmp/log/messages | tail -5" | tools/brick-run -
```

应该能看到 `add Chrome 浏览器 icon(...) launch(...)` 和 `app NN ... show 1`。

---

## 手柄操作

机身是**任天堂布局**（A 在右、B 在下、X 在上、Y 在左），下表按机身丝印。

### 浏览网页时

| 按键 | 作用 |
|---|---|
| 左摇杆 | 移动鼠标指针 |
| A（右） | 左键点击 |
| Y（左） | 右键点击 |
| B（下） | Esc |
| X（上） | 退格 |
| START / SELECT | 回车 / Tab |
| L1 / R1 | 上一页 / 下一页 |
| 方向键 上下 / 左右 | 滚动 / 浏览器后退前进 |
| 右摇杆 上下 | 滚动 |
| L2 / R2 | 网页缩小 / 放大 |
| 右摇杆按下 / 左摇杆按下 | 跳到地址栏 / 刷新 |
| **短按 MENU** | 开关屏幕键盘 |
| **按住 MENU** | 语音输入 |
| **SELECT + START** | 退出 Chrome，回到原厂界面 |

### 屏幕键盘打开时

方向键接管键盘（不再滚动页面），A 按下选中的键，B 收起。
布局：数字行 / qwerty / asdf + 退格 / 上档 + zxcv / 符号 + 空格 + `.com` + 语音 + 回车 + 关闭。

### 语音输入

按住 MENU 超过 0.35 秒开始，松开结束。顶部状态条会如实反映阶段：

| 显示 | 含义 |
|---|---|
| 等待中… | 正在连 Chrome、启动识别引擎，这时说话会丢 |
| 可以说话了 | 引擎已开始采集（`onaudiostart`），现在开口 |
| 正在聆听… | 下方实时显示已识别到的文字 |
| 识别中… | 已松开，在等最后一段结果 |
| 正在输入… | 正在把文字输入到光标处 |

每个网站第一次用会弹麦克风授权框，用 A 允许即可。需要 https 页面（Web Speech API 要求安全上下文）。

---

## 可调配置

都在 `/mnt/SDCARD/Apps/Chrome/`，改完退出重进生效。

| 文件 | 说明 | 默认 |
|---|---|---|
| `homepage.txt` | 首页网址（第一行） | `https://www.bing.com` |
| `scale.txt` | 界面缩放，`1` = 100%、`1.5` = 150% | `1` |
| `pointer.txt` | 指针峰值速度（像素/帧，50 帧/秒） | `12` |

缩放对应的可用空间：

| scale.txt | CSS 视口 |
|---|---|
| 2 | 512 × 384 |
| 1.5 | 683 × 512 |
| 1.25 | 819 × 614 |
| 1 | 1024 × 768 |

`launch.sh` 会按缩放系数自动算 `--window-size`（这个参数是逻辑像素，必须除以缩放系数，
窗口才会正好落在 1024×768）。

语言用环境变量 `BRICK_SPEECH_LANG` 改，例如 `en-US`。

---

## 故障排查

| 现象 | 看这里 |
|---|---|
| 图标不出现在应用列表 | MainUI 只在启动时扫描，`killall -9 MainUI` |
| 点了没反应 / 黑屏几秒退回 | `/opt/brick-chrome/tmp/brick-app.log` |
| 手柄没反应、指针不动 | `/opt/brick-chrome/tmp/brick-pad.log`（有异常会写 traceback） |
| X11 起不来 | `/opt/brick-chrome/tmp/brick-xorg.log` |
| 语音报错 | 状态条会显示原因；详细日志在 `brick-pad.log` |

手柄进程崩了不用重启整机，可以直接接回去：

```bash
tools/brick-run - <<'SH'
setsid env DISPLAY=:1 chroot /opt/brick-chrome /usr/bin/python3 -u \
  /usr/local/bin/brick-pad.py </dev/null \
  >/opt/brick-chrome/tmp/brick-pad.log 2>&1 &
SH
```

---

## 踩过的坑

这一节是这个项目里最有价值的部分 —— 每一条都花了不少时间才定位。

### 1. MainUI 只在启动时扫描一次 Apps 目录

运行中创建 `/mnt/SDCARD/Apps/Chrome/` 不会有任何反应。必须 `killall -9 MainUI`
（`runtrimui.sh` 的循环会把它拉起来，退出码 0/137/143 都视为正常）。
`kill -TERM` 无效，MainUI 不理会。

### 2. 应用是通过 `/tmp/cmd_to_run.sh` 启动的

MainUI 把命令写进这个文件然后自己退出，由 `runtrimui.sh` 在 MainUI 不在时执行。
自己写这个文件再杀掉 MainUI，就能完全复现点图标的效果 —— 这是 SSH 上调试启动脚本的正确方法。

### 3. 帧缓冲的第 4 个字节是 alpha

`/dev/fb0` 是 32bpp，sunxi 显示引擎**把第 4 个字节当合成 alpha 用**。
python-xlib 的 `put_pil_image` 把 RGB 按 `BGRX` 编码，padding 字节写 0 ——
于是画上去的东西颜色数据完全正确地躺在显存里，却被当成全透明合成掉，屏幕上是纯黑。
Chrome 自己的像素 alpha 是 255 所以一直正常。

必须自己发 `BGRA`。X 服务端**不会**在 depth-24 drawable 上屏蔽这个字节。

诊断方法：读 `/dev/fb0` 的 `row * 4096` 偏移，然后 `Counter(raw[3::4])` —— Chrome 的行是 255，坏掉的覆盖层是 0。

### 4. 无窗口管理器的覆盖层需要 backing store

Chrome 拿到焦点时会把自己的窗口往上提。没有 backing store 的话，被遮挡的内容会被服务端丢弃，
重新暴露时清成 `background_pixel`。必须 `backing_store=X.Always` 并订阅 `ExposureMask` 重绘。

### 5. 先 map 再画

往未映射的窗口画会被丢弃，接着 map 又把它清成背景色。顺序反了就是一块纯色。

### 6. Chrome 136+ 的远程调试要求非默认 profile 路径

用默认 profile 时 `--remote-debugging-port` 会被**静默忽略**，日志里只有一行
`DevTools remote debugging requires a non-default data directory`。
`brick-chrome-start` 会在首次启动时把 profile 改名到 `brick-profile` 并加 `--user-data-dir`，
历史和登录态都保留。

### 7. DevTools 拒绝带 Origin 头的 WebSocket 握手

返回 403。`websocket-client` 默认会带这个头，必须 `suppress_origin=True`。

### 8. 手柄按位置映射，机身丝印是任天堂布局

`trimui_inputd` 把手柄伪装成 Xbox 360 手柄，**按物理位置**映射。
所以机身丝印 **B**（下方）发出的是 `BTN_SOUTH(304)`，丝印 **A**（右侧）是 `BTN_EAST(305)`。
照着 evdev 名字写映射会把"确认"放到 B 上。

### 9. 麦克风不能用 `default`，要用 `hw:0,0`

| 设备 | 48kHz 单声道 | 16kHz 单声道 |
|---|---|---|
| `hw:0,0` | OK | OK |
| `plughw:0,0` | **FAIL** | OK |
| `default` | **FAIL** | OK |

而 48kHz 单声道正是 Chrome 做 WebRTC 采集时要的。`default` 走的 plug 插件链在这个组合上会挂，
表现为 `snd_pcm_set_params: Invalid argument`。

### 10. SpeechRecognition 默认在第一次停顿就结束

`continuous` 默认是 `false`，第一个 final result 一出就停 —— 不管你说多长，只有开头一句被识别。
要连续识别就得 `continuous = true`，由使用者控制何时结束（所以做成了按住 MENU）。

### 11. 转写要重建，不能累加

```js
// 错：event.results 是累积的，同一条结果可能被再次上报
for (var i = event.resultIndex; i < event.results.length; i++)
  if (results[i].isFinal) final += text;      // 会重复，也会漏
```

每次从头重建整段，幂等，不会重复也不会丢。

### 12. Chrome 在**处理**按键时才解析 keycode

这是最隐蔽的一个。XTEST 只能发 keycode，而键盘映射里没有 CJK，
所以中文要靠临时把空闲 keycode 映射到 Unicode keysym（xdotool 的老办法）。

但 Chrome 是在**处理事件的那一刻**才用当时的映射去解析 keycode 的，不是在事件到达时。
如果复用同一个 keycode 逐字重映射，按键还排在队列里没处理，映射就已经变成下一个字了 ——
等它处理时查到的是新值。症状是**字被后一个字顶替**：

```
说的:  广 州 今 天 的 天 气 怎 么 样
得到:  广 今 今 天 的 天 气 怎 么 样
```

在按键**前**加延时没有用，问题出在按键**后**太快改映射。

正确的解法是**每个不同的字分配各自的 keycode，整段打字期间完全不改映射**，
从结构上消除竞态。打完也**不要还原**成 NoSymbol —— 那是同一个竞态的反向版本，
仍在队列里的按键会解析成"无符号"，末尾几个字会凭空消失。

### 13. 高位 keycode 是浏览器功能键

Chrome 判断快捷键看的是硬件 keycode 派生的 DomCode，**跟映射上去的 keysym 无关**。
X keycode 166–180 对应 evdev 的 Back / Forward / Refresh，借用它们会直接让页面导航。

`brick_typing.py` 里那份 45 个 keycode 的名单是**实测筛出来的**：逐个映射成已知字符、
打进页面、核对字符正确且页面没跳转。114 / 119 / 166 / 167 被剔除（吞按键不产生字符）。

---

## 仓库内容

```
chroot/                        → 复制到掌机 /opt/brick-chrome/
  brick-x11.sh                   启动 Xorg + 手柄守护 + 目标程序
  etc/X11/xorg.conf              fbdev 配置
  usr/local/bin/
    brick-chrome-start           Chrome 启动参数、profile 迁移
    brick-pad.py                 手柄 → X11，主循环
    brick_keyboard.py            屏幕键盘 + 顶部状态条
    brick_speech.py              CDP 驱动 Web Speech API
    brick_typing.py              XTEST 文本注入（含 Unicode）

sdcard/Apps/Chrome/            → 复制到掌机 /mnt/SDCARD/Apps/Chrome/
  config.json                    原厂启动器的应用描述
  launch.sh                      应用入口，读配置并调用 brick-x11.sh
  CONTROLS.txt                   设备上的操作速查
  scale.txt / pointer.txt / homepage.txt

tools/                         → 在宿主机上用
  brick-run                      在掌机上执行一段脚本
  brick-ssh.exp / brick-scp.exp  expect 包装，凭据从环境变量取
```

### 不包含的内容

- **凭据**：设备 IP、密码、SSH 主机密钥都不在仓库里，工具全部从环境变量读取。
- **Google 的二进制和商标**：Chrome 的 deb 和图标请按教程自行下载，本仓库不分发。
- **Ubuntu base rootfs**：同上，自行下载并校验 SHA256。

### 状态

在一台 TG4040 上开发和验证。没有在其他型号上试过 —— 帧缓冲格式、输入设备节点
（`/dev/input/event3`）、`inet` 组 id 这些都可能不一样。

许可证还没定；想用请自便，出了问题自己担着。
