# 开发笔记（Frame Mic Tuner）

使用方法见 [README.md](../README.md)。这里汇总了工作原理、界面的规则、验证过的事情，以及开发用的选项。

在 Steam Frame（aarch64 SteamOS）上，从 SteamVR 仪表盘切换麦克风**回声消除**和**噪声抑制**的面板。
戴着耳机时关闭回声消除，让细微的声音也能进来；用扬声器时打开回声消除，消除扬声器声音的串入。

- 切换机制（WirePlumber 脚本）在 `contrib/wireplumber/`（见下面的“切换机制”）。应用用 `wpctl settings --save` 改写它的设置 `frame-mic.echo-cancel`、`frame-mic.noise-suppression`
- 仪表盘覆盖层、用 Vulkan 传送图像的方式、退出处理、注册到 ＋、重复启动的处理，都与同一作者面向 Steam Frame 的覆盖层（未公开）做法相同

## Frame 麦克风的声音流向

```
alsa_input.platform-sound.HiFi__Mic__source（2ch，两个数字麦克风）
 → eq_capture / eq_source（EQ：音质校正 ＋ 2ch→1ch）
 → echo_cancel_capture / echo_cancel_source（WebRTC 的回声消除＋自动音量调整）
 → ns_capture / ns_source（噪声抑制：按像不像人声来判断，消掉不是人声的声音）
 → alsa_loopback_stream → alsa_loopback_device...HiFi__Mic__source（默认麦克风。应用从这里录音）
```

- 每一级都是 WirePlumber 的 smart filter（`filter.smart = true`、`steamos.mic_filter = true`）。启用/禁用由 `filters` 元数据的 `filter.smart.disabled` 决定
- SteamOS 原本的行为（Valve 的 `/etc/wireplumber/scripts/microphone-tracker.lua`）是：只要有 1 个录音流（`Stream/Input/Audio`），三级就全部启用；变成 0 个就全部禁用
- 口部的细微声音（咂嘴声之类）会被回声消除大幅削弱，又被噪声抑制变成彻底的静音。用耳机时并不需要回声消除，所以做成了可以关掉

## 切换机制（contrib/wireplumber）

| 文件 | 在 Frame 上的位置（install.sh 会放入） |
|---|---|
| `frame-mic-tracker.lua` | `~/.local/share/wireplumber/scripts/frame-mic-tracker.lua` |
| `90-frame-mic.conf` | `~/.config/wireplumber/wireplumber.conf.d/90-frame-mic.conf`（把 `@SCRIPT_PATH@` 替换为上面那个脚本的绝对路径） |

- `90-frame-mic.conf`：把 Valve 的 tracker（`steamos.microphone-tracker`）设为 `disabled`，并加载自制的 `frame-mic-tracker.lua`。它定义设置 `frame-mic.echo-cancel`（默认 true）和 `frame-mic.noise-suppression`（默认 false）
- `frame-mic-tracker.lua`：行为与原版相同（只在有录音流时启用滤镜）。区别在于，回声消除（`echo_cancel*`）和噪声抑制（`ns_*`、`dsp_*`）都可以各自用设置关掉。改变设置会立刻反映到正在使用的麦克风上（不需要重启 PipeWire。录音中切换，声音也不会中断）
- 两者都只在主目录里。不碰 Valve 在 `/etc` 下的文件。放好之后，重启头显即可生效
- 卸载后（`./install.sh --uninstall` 再重启）会回到 Valve 原本的行为（三级全部启用）
- 制作时踩过的坑：
  - 节点的 `steamos.mic_filter` 是信息侧的属性，所以 `Constraint` 需要 `type = "pw"`。默认的 `pw-global` 一个都匹配不上
  - Valve 的 tracker 是从事件的 source 借 ObjectManager，但在设置变更（`Settings.subscribe`）时没有事件，所以要自己持有一个 ObjectManager

手动切换时：

```sh
wpctl settings --save frame-mic.echo-cancel false        # 关闭回声消除（耳机）
wpctl settings --save frame-mic.echo-cancel true         # 打开回声消除（扬声器）
wpctl settings --save frame-mic.noise-suppression true   # 打开噪声抑制
wpctl settings frame-mic.echo-cancel                     # 当前值（Value: true (Saved: true)）
```

`--save` 的值写在 `~/.local/state/wireplumber/sm-settings`，重启后仍然保留。

## 界面

SteamVR 仪表盘下面的一行会出现“Mic”图标（麦克风的图）。选中它会打开面板（1200×788，宽 2.8m），用激光指针点击操作。

| 行 | 按钮 | 行为 |
|---|---|---|
| 标签页（左列上方） | **简单** / **精细调整** | 切换左列的内容（胶囊形切换。选中时是强调色填充＋✓＋粗体）。把最后查看的标签页保存到 `config.json` 的 `tab`，下次打开时仍显示这个标签页（没有则显示简单）。在拖动滑块的过程中切换标签页或关闭面板时，会先结束拖动，发送并保存最后一个值。信号链路卡片在两个标签页里都显示在下方 |
| 你在哪里听声音？（简单标签页，预设） | **耳机 / 头戴式耳机** / **Frame 扬声器** | 一次性切换设置的预设。耳机 = 回声消除关、噪声抑制关，扬声器 = 回声消除开、噪声抑制关（按下时作为一条命令交给工作线程，按回声消除 → 噪声抑制的顺序写两次 `wpctl settings --save`，然后只重新读取一次并反映到显示上。为了避免中途的值造成灰色闪烁，在写完之前（写入受理编号之前，最长 8 秒）让被按下的卡片保持选中外观。即使一方失败，另一方也会写入，并以红色显示“回声消除切换失败（wpctl）”或“噪声抑制切换失败（wpctl）”之一（两者都失败则显示“切换失败（wpctl）”），同时显示重新读取到的实际值）。不改变噪声抑制强度滑块。每张卡片上显示按下去会变成什么的两枚标签（“回声消除 关”“噪声抑制 关”等）。与当前设置一致的卡片处于选中状态。一致性判断只看回声消除和噪声抑制这两项（滑块的值在噪声抑制关闭时不起作用，所以不比较）。与两者都不一致时，把两张卡片都变灰（底色填充＋虚线边框＋较暗的文字，仍可按下），并把卡片下面那一行从“选择后会应用推荐设置”换成“当前使用精细调整的设置”（粗体）和“查看精细调整 →”按钮。正在读取值时不变灰 |
| 回声消除（精细调整标签页） | 开 / 关 | `frame-mic.echo-cancel` |
| 噪声抑制（精细调整标签页） | 开 / 关 | `frame-mic.noise-suppression`。打开后细微的声音（口部的声音等）会被消掉 |
| 判定严格度 / 保持时间（精细调整标签页） | 滑块（拖动）、− / ＋、恢复默认 | 噪声抑制的强度（见下面的“噪声抑制的强度”）。噪声抑制关闭期间是灰色（仍可按下），并显示“关闭期间不生效”。“恢复默认”放在滑块下面那一行，和“默认 23% · 500ms（SteamOS 的值）”一起 |
| 信号链路 | （仅显示） | “麦克风 ─ 音质校正 ─ 回声消除 ─ 噪声抑制 ─ 输出到应用”的管线图。实际经过的级用强调色的浅填充＋实线边框＋粗体，通路连线也是强调色。没经过的级用虚线边框＋细体，连线从下面绕过去跳过 |
| 声音检查 | ● 录音 / ■ 停止，历史记录的 ▶ / ■ | 录制应用收到的声音，最长 10 秒，最多保留 5 条用来对比试听（见下面的“声音检查”） |
| 更新提示条（整宽卡片） | 立即检查 / 更新（确认：取消 · 更新）/ 重试 · 关闭 / 关闭 | 显示 `vendor/frame-updater` 的 `UpdateChecker` 的状态（文案沿用 strings.md）。最新、检查中、更新中、只有版本（未检查）是普通边框。有新版本、确认、安装完成是强调色边框，更新失败是红色边框（原因、重试 · 关闭）。检查失败只有文字变红，边框保持普通（当前版本仍在运行，离线时每天都会出现）。按第一次“更新”只是显示确认（第二行是补充说明，取消 · 更新），3 秒内按第二次才 `install()`。取消、按其他按钮或过 3 秒都会取消。更新中第二行显示与确认时相同的补充说明。确认的补充说明和安装完成的文案，针对 install.sh 不会重启常驻的这一应用，从 strings.md 做了调整（“完成后退出并重新启动，即可使用新版本”）。安装完成时第二行是“如果 WirePlumber 脚本也有变化，请同时重启头显”。需要手动更新的版本第二行是原因和发布页面的 URL。检查期间不显示按钮。安装通过 `frame-update.sh install --detach`（`frame-mic-tuner-update` 临时单元）进行，`install.sh` 不会重启正在运行的常驻进程，所以只有在退出并重新启动后才会变成新版本 |
| 下面一行：语言 | 日本語 / English / 简体中文 | 立即切换文案（保存到设置文件） |
| 下面一行：随 SteamVR 启动 | 开 / 关 | `systemctl --user enable` / `disable frame-mic-tuner.service`（见下面的“自启动”）。没有安装单元文件时是灰色、无法按下，并显示“自启动尚未安装（请运行 ./install.sh）” |
| 下面一行：退出 | | 按下后 3 秒内变成“再按一次退出”，在此期间再按一次就退出（退出码 3） |

界面布局（横向两列）：

- 左列 = 切换：标题“麦克风”和使用中 / 未使用的徽标 → 标签页（简单 / 精细调整，高度 48）→ 标签页的内容 → 信号链路的管线图（两个标签页共用）
  - 简单：“你在哪里听声音？”（19px）→ 两张预设卡片（整宽纵向排列。左边是图，中间是名称约 28px 和效果 17px，右边是纵向排列的两枚标签 14px。选中时是强调色填充＋光晕＋名称前的 ✓。标签是底色的胶囊，在选中的卡片上是强调色文字。与两者都不一致时两张都变灰）→ 卡片下面那一行（说明，或“当前使用精细调整的设置”和“查看精细调整 →”）
  - 精细调整：回声消除、噪声抑制的滑动开关（高度 52，右边一行说明 16px）→ 两根强度滑块（标题 17px、说明 14px、− / ＋ 42px、值 19px）→“默认 23% · 500ms（SteamOS 的值）”和“恢复默认”
- 2026-09-27 因为有人说“看不出这是预设”，加了标题、标签和分隔线，之后又分成了“简单 / 精细调整”两个标签页（高度仍是 700。因为加了标签页，卡片名称、标签和滑块说明的文字都调大了）
- 右列 = 声音检查：录音按钮和音量表那一行，历史记录 5 条（第一行是时刻和长度，第二行是录制时的设置，右边是横向的波形）
- 更新提示条（整宽卡片，高度 70）：左边是状态文字（19px。有补充说明时第二行是 15px 的灰色），右边是按钮（高度 50 的胶囊。“更新”是强调色填充）。颜色、圆角、按钮都与其他卡片使用同一套 theme 令牌
- 下面一行（整宽）：语言、随 SteamVR 启动、退出。最下面只有一行，有失败（红色）就显示失败，没有就显示自启动不可用的原因，两者都没有就显示说明
- 尺寸：1200×788 px 以 2.8m 宽显示（高约 1.84m。每 px 约 429 px/m）。加上两根噪声抑制强度滑块（约 90px）从 650 → 700，加上更新那一行变成 758，再把它做成提示条（卡片）变成 788

- 右上角的徽标是“● 使用中”（绿色）/“○ 未使用”（灰色）。最下面一行只在失败时变成红色文字（例如“切换失败（wpctl）”）
- 按钮的选中状态、是否使用中、信号链路都根据实际值生成
  - 回声消除、噪声抑制：`wpctl settings`（全部设置的列表）里 `frame-mic.*` 的 `Value:`
  - 信号链路：`pw-link -l` 里实际的连接。从麦克风（`alsa_input.platform-sound.HiFi__Mic__source`）出发，沿着进入 `<名称>_capture`、从 `<名称>_source` 出来的滤镜一路追踪，直到 `alsa_loopback_stream.alsa_input...`（默认麦克风之前）。`eq` = 音质校正（英文是 EQ）、`echo_cancel` = 回声消除、`ns` / `dsp` = 噪声抑制
  - 麦克风使用中 = 通路里有滤镜。tracker 只要有 1 个录音流（`Stream/Input/Audio`）就一定会启用 EQ，所以“使用中”等同于“tracker 启用了滤镜”。**录制扬声器声音的应用（录屏的 ffmpeg 等）也算录音流，因此也会被计入**
  - 未使用时 `pw-link -l` 是麦克风直接连到 `alsa_loopback_stream...` 的形式（“麦克风 → 输出到应用”和“未在使用，处理已暂停”）
- 只有在面板于仪表盘中**打开期间**才每秒重新读取一次（即使从外部用 `wpctl settings --save ...` 改变，也能在 1 秒内让显示对上）。自启动状态（`systemctl --user is-enabled`）很少变化，所以只在刚打开后和每 5 秒读取
- 按下按钮后，写入 → 立即重新读取 → 反映到显示。写入的值会回读确认，不一致就当作失败并标红
- 截图：`docs/v10-*-quick-speaker_*.png`（用 `--dump-png --tab quick --fake --fake-echo on --fake-ns off --fake-history 5 --fake-playing 1 --fake-update uptodate` 导出）和 `docs/v10-*-fine_*.png`（`--dump-png --tab fine --fake --fake-echo on --fake-ns on --fake-ns-vad 10 --fake-ns-grace 800 --fake-history 5 --fake-playing 2 --fake-update uptodate`）。两者都用 `--language ja|en` 分别拍日文和英文。用 `--tab quick|fine` 选择要绘制的标签页。更新提示条不加 `--fake-update` 就会是“v0.2.0”（未检查），所以截图里统一用 `uptodate`（已是最新版本）

## 噪声抑制的强度

噪声抑制这一级是 Valve 的 filter-chain（`/etc/pipewire/microphone-filter-chain/microphone-filter-chain-echo-cancel-cpu.conf`）里的 LADSPA `noise_suppressor_mono`。它的 control 可以在不重启 PipeWire 的情况下当场修改。

| 滑块 | 参数名（`ns_capture` 的 Props 里的 params） | 范围（PropInfo） | SteamOS 的值 | 步长（− / ＋） |
|---|---|---|---|---|
| 判定严格度 | `noise_suppressor_mono:VAD Threshold (%)` | 0～99（插件默认 49.5） | 23 | 1% |
| 保持时间 | `noise_suppressor_mono:VAD Grace Period (ms)` | 0～1000（默认 500） | 500 | 50ms |

- 发送目标是 `ns_capture`（filter-chain 的入口节点）。`ns_source` 的 Props 里没有 control（2026-09-27 用 `pw-cli enum-params <id> PropInfo` / `Props` 确认）
- 读取：`pw-dump ns_capture`（一次就能拿到 id 和值。约 40ms）的 `info.params.Props[].params`（`[名称, 值, 名称, 值, ...]`）
- 写入：`pw-cli set-param <id> Props '{ params = [ "noise_suppressor_mono:VAD Threshold (%)" 23.0 "noise_suppressor_mono:VAD Grace Period (ms)" 500.0 ] }'`。用 fork＋execvp，不经过 shell，数值在应用里先按范围取整（1% / 10ms 单位）再转成字符串。两个值总是一起发送
- 同一个 PropInfo 里还有 `Retroactive VAD Grace (ms)`（0～200，SteamOS 是 0），但界面上没有显示
- SteamOS 每次启动 PipeWire 都会恢复成 filter-chain 配置里的值（23 / 500）。因此应用把它保存在 `config.json` 里（`ns_vad_threshold_percent`、`ns_vad_grace_ms`。不拖动滑块就不写入），并在下列时机重新施加：
  - 应用启动时（工作线程查找节点，找不到就每 5 秒重试一次。施加成功后停止。即使面板关着，也只有启动后的这一段会工作）
  - 面板打开期间每秒重新读取时，如果节点的 id 变了（PipeWire 重建了节点）
  - 面板关闭期间 PipeWire 被重建时，下次打开面板时重新施加
- 从外部用 `pw-cli` 改变时，不重新施加，而是让显示跟上（id 相同就以当前值为准）
- 滑块会跳到按下的位置，并且可以继续拖动（即使拖出轨道上下方，也按横向位置决定）。拖动过程中，如果值有变化，就每 100ms 只发送最后一个值（工作线程的队列里，还没执行的前一个值会被丢弃）。松开时（拖出面板时也是）一定会发送并保存一次。写入后到重新读取跟上之前的 1.5 秒内，用写入的值显示（以免闪烁）
- WirePlumber 脚本（`contrib/wireplumber/`）没有改动
- 声音检查的历史记录里，只对噪声抑制开启时录的那些显示当时的强度（例如 `10%/800ms`）。第二行放不下时，放在第一行长度的后面；还放不下就省略

验证过的事情（2026-09-27，Frame。值最后恢复为 23 / 500）：

- `--set-ns-vad 10` → `pw-cli enum-params ns_capture Props` 是 10.0 / 500.0。`--set-ns-grace 800` → 10 / 800。超出范围（150 / -5）会被取整为 99 / 0。`--print` 会输出“判定严格度 10% · 保持时间 800ms（ns_capture 的 id 53）”
- 用保存了值（30% / 600ms）的设置文件启动时，日志里会输出“对节点 53 施加已保存的值”，`pw-cli` 的值变成了 30 / 600
- 效果（参考。没说话的安静房间，噪声抑制开启、麦克风使用中，每 0.5 秒的电平）：判定严格度为 0% 时背景声音以 rms -57～-85 dBFS 通过，99% 和 23% 时几乎无声（-90 dBFS）

## 声音检查

用耳机和扬声器，当场对比试听应用收到的声音有什么不同。

- 用“● 录音”开始。10 秒后自动停止，中途再按一次也会停止（按钮会变成“■ 停止”）
- 录音期间显示“● 录音中”的文字（不只是用红色）、已过时间和剩余秒数、音量峰值表（-60～0 dBFS 和数字）
- 历史记录按从新到旧最多 5 条。录第 6 条时删掉最旧的一条。每一行有录制时刻（例如 00:41）、长度、录制时的设置（“耳机”“扬声器＋噪声抑制”等）、小波形、▶ 播放 / ■ 停止。正在播放的行里，已播放的部分和位置线以强调色移动。播放中按其他行的 ▶ 会切换到那一行
- **录的是应用收到的最终声音**：把 libpipewire-0.3 的 `pw_stream` 不指定 target 地接到默认输入（`alsa_loopback_device.alsa_input...HiFi__Mic__source`）（因为用 `--target` 传名字时，有被接到 smart filter 后面的坑）。格式为 mono、int16、48kHz
- 录音流**不**加 `node.virtual`。这是为了让 tracker 把它算作麦克风使用中并启用滤镜，从而录到与正式使用时相同的声音（录音期间会显示“使用中”）。刚接上后的 0.3 秒是滤镜启用并切换完成之前的声音，所以丢弃
- 播放通过 `pw_stream` 送到默认输出（Frame 的扬声器或耳机）。送完后用 `pw_stream_flush(drain)` → `drained` 收尾。开始录音时会停止播放（以免把扬声器的声音录进去）
- PipeWire 的循环放在专用线程（`pw_thread_loop`）里。在开始录音或播放时创建，**关闭面板时停止录音、播放，并连同流和线程一起清理**（不在看不见的地方录音。关闭期间不在 PipeWire 里创建任何东西）
- **音频只在内存里**。不写入磁盘也不写入日志（日志里只输出长度和峰值的 dBFS）。退出后就消失
- 不使用外部命令 pw-record / pw-play。也不重启 PipeWire、WirePlumber，不修改设置

验证过的事情（2026-09-27，Frame）：

- `--test-record 3`：录音 → 长度和峰值 → 从内存播放，都能工作（例如 2.57 秒、123424 个采样、峰值 -24.5 dBFS，播放 2.7 秒后 drained）。在没有其他应用录音时，录音期间 `pw-metadata -n filters` 的 EQ 和回声消除变成 `false`（启用），结束后回到 `true`。用 `pw-link -l` 可以看到连到了 `alsa_loopback_device...:capture_FL/FR -> frame-mic-tuner-record`
- 常驻时加上 `--debug-record-on-open` 打开面板，再用 `--probe-switch-away` 切换到另一个仪表盘覆盖层，日志里会输出“面板已关闭，停止录音和播放” → “已清理 PipeWire 的流和线程”，录音节点消失，线程数也恢复原样
- 达到 10 秒会自动停止（10.0 秒时停止了）
- 如果麦克风音量（ALSA 的 `VA_DEC0 Volume` / `VA_DEC1 Volume`）掉到 0，录音会在最初 0.5 秒的爆音之后变成 -90 dBFS 的静音（`pw-record` 也一样。这不是本应用的问题）。参见 README 的“疑难解答”

## 颜色与无障碍

- 颜色集中在 `src/theme.h` 的一处。绘制只使用这里的颜色，`--contrast-report` 从同一份定义计算 WCAG 2.x 的对比度，并输出每一组的比值和是否通过
- 背景的层次是 GitHub 深色系：`#0d1117`（底色）/ `#161b22`（卡片）/ `#21262d`（按钮）/ `#30363d`（悬停、按下）。卡片用内部 1px 的高光和投影浮起来。圆角是胶囊形
- 强调色是 `#e27dfd`。用于选中的填充、经过的级、播放位置、音量表。按下期间是 `#c45fe0`。浅填充和光晕是 20%
- 规则：文字不论大小都按 4.5:1 以上验证。按钮的边框、选中状态、图示（WCAG 1.4.11）按 3:1 以上。不可按按钮的文字在 WCAG 里是例外，但仍做到可读（3:1 参考）
- 强调色填充上的文字用深色 `#0d1117`（7.74:1）。白色是 2.4:1，不能用。暗色底上的强调色文字是 7.07～7.74:1
- 按钮边框用 `#30363d` 只有 1.3:1，所以改成 `#6e7681`（在卡片上 3.77:1）
- 辅助说明的灰色 `#9198a1` 在强调色浅填充上只有 4.17:1，不够，所以不使用这个组合（经过的级的文字是 `#e6edf3`，10.33:1）
- 不只用颜色表达选中状态：选中时有 ✓、填充、粗体，没经过的级是虚线边框、细体，录音中是 ● 和“录音中”文字，使用中是 ● / 未使用是 ○ 和文字
- 与两个预设都不一致时的灰色卡片仍可按下，所以不算非激活的例外，文字用 `#7d8590`（在底色上 5.07:1，悬停时的 `#21262d` 上 4.08:1）以保证可读
- 2026-09-27 的结果：65 组全部通过。最低的是“标签的边框（悬停在未选中的卡片上时）”`#6e7681` / `#21262d` 的 3.31:1（部件要求 3:1 以上）

## 构建（在 Frame 上）

需要的东西（SteamOS 已自带）：cmake、ninja、g++、pkg-config、cairo、freetype2、libpipewire-0.3、Vulkan 的头文件和加载器（`vulkan` 的 pkg-config）、SteamVR（`/opt/steamvr/bin/linuxarm64/libopenvr_api.so`）。
`openvr.h` 随附在 `third_party/openvr/`（OpenVR SDK 2.15.6，BSD-3-Clause。[许可证](../third_party/openvr/LICENSE)）。

在头显上：

```sh
cmake -G Ninja -S . -B build && ninja -C build
```

在 PC 上编辑、在头显上构建的例子（在 PC 侧的 Git Bash 里，从这个文件夹）：

```sh
tar --exclude=build --exclude=out --exclude=.git -cf - . | ssh steamos@<headset-ip> 'mkdir -p ~/frame-mic-tuner && tar -xf - -C ~/frame-mic-tuner'
ssh steamos@<headset-ip> 'cd ~/frame-mic-tuner && cmake -G Ninja -S . -B build && ninja -C build'
```

可执行文件是 `build/frame-mic-tuner`。OpenVR 库的位置写进了 rpath，所以复制到别的地方也能直接运行。`-Wall -Wextra` 下零警告。版本在 `CMakeLists.txt` 的 `project(... VERSION ...)` 里，用 `--version` 输出（与 `CHANGELOG.md` 保持一致）。

## 手动运行

```sh
./build/frame-mic-tuner          # 在仪表盘里显示面板并常驻（Ctrl+C 退出）
```

- SteamVR 没有启动时，每 3 秒重试等待（不会擅自启动 SteamVR）
- SteamVR 退出（`VREvent_Quit`）时用 `AcknowledgeQuit_Exiting` 回应并安静地退出（退出码 0）
- Ctrl+C / SIGTERM 只是打上标记，然后从主循环走同一套退出处理（见下面的“图像传送方式与退出处理”）

确认用的选项（除 `--probe` 和 `--probe-switch-away` 之外都不需要 OpenVR）：

```sh
./build/frame-mic-tuner --print                  # 显示当前值、麦克风是否在用、信号链路、自启动
./build/frame-mic-tuner --set-echo off           # 关闭回声消除后显示（耳机）。on 为打开（扬声器）
./build/frame-mic-tuner --set-ns on              # 打开噪声抑制后显示
./build/frame-mic-tuner --set-autostart on       # 先 systemctl --user enable 再显示（off 为 disable）
./build/frame-mic-tuner --set-ns-vad 10 --set-ns-grace 800   # 先当场施加噪声抑制强度再显示（不保存）
./build/frame-mic-tuner --dump-png out/panel-ja_2026-09-27_00-00-00.png --language ja
./build/frame-mic-tuner --dump-png out/panel-en-error_2026-09-27_00-00-00.png --language en --fake-error write --fake-autostart missing
./build/frame-mic-tuner --thumbnail-png out/thumbnail_2026-09-27_00-00-00.png --thumbnail-size 256
./build/frame-mic-tuner --test-record 3          # 录 3 秒 → 长度和峰值 → 从内存播放
./build/frame-mic-tuner --contrast-report        # 每一组颜色的对比度和是否通过
./build/frame-mic-tuner --self-test              # 用固定输入测试判定函数（自修复覆盖层的判定、输出的读取、标签页的保存、拖动的结束方式）
./build/frame-mic-tuner --probe                  # 通过 SteamVR 查找常驻的面板并输出状态
./build/frame-mic-tuner --version
```

- `--print` 还会输出 `pw-link -l` 里麦克风通路的行（用来对比显示和连接是否一致）
- `--dump-png` 用当前实际的值绘制。加上 `--fake` 或 `--fake-*` 就不读取实际值而用假数据绘制（`--fake-echo on|off`、`--fake-ns on|off`、`--fake-idle`、`--fake-loading`、`--fake-autostart on|off|missing|unknown`、`--fake-error read|not-installed|links|write|autostart`）。`--preview-quit` 显示“再按一次退出”的状态，`--language ja|en` 指定语言
- 声音检查的外观：`--fake-recording`（录音中）、`--fake-history N`（N 条假历史记录）、`--fake-playing I`（正在播放第 I 条）、`--fake-voice-error record|play`、`--preview-pressed earphone|speaker|record`（按下期间）
- 噪声抑制强度的外观：`--fake-ns-vad N`、`--fake-ns-grace N`（假值）、`--preview-drag-vad N`、`--preview-drag-grace N`（正在把该滑块拖到 N）
- `contrib/icons/frame-mic-tuner-{48,128,256}.png` 是用 `--thumbnail-png` 导出的（与仪表盘缩略图相同的图）
- `--test-record [秒]` 还会输出录音前、中、后的 `pw-metadata -n filters`。音频只在内存里
- 不用头显也能看关闭行为：给常驻进程加 `--debug-record-on-open`（面板一打开就自动录音），再从另一个终端用 `--probe-switch-away [秒]`（创建带图标 PNG 的临时仪表盘覆盖层并用 `ShowDashboard` 切换过去，让 Mic 面板进入关闭状态。没有图片的覆盖层切不过去）
- `--probe` 只是以 Background 类型连接（不创建覆盖层，也不用 Vulkan）。输出 `FindOverlay`、名称、宽度、关闭按钮标志、是否显示中、`GetOverlayTextureSize`

## 从 ＋（启动程序）使用

Frame 的 SteamVR 仪表盘里“启动程序”（＋）的列表，是 Steam 客户端扫描 XDG 的 `.desktop` 文件（`~/.local/share/applications/` 等）生成的。与 SteamVR 的 `.vrmanifest` 无关。

`./install.sh` 安装的文件：

- `~/.local/bin/frame-mic-tuner`（可执行文件。systemd 服务也用它）
- `~/.local/share/applications/frame-mic-tuner.desktop`（把 `Exec` 设为可执行文件的绝对路径。因为从 Steam 启动时 `PATH` 里可能没有 `~/.local/bin`）
- `~/.local/share/icons/hicolor/{48x48,128x128,256x256}/apps/frame-mic-tuner.png`（Steam 有时只靠 256x256 找不到图标，所以放 3 种尺寸）
- `~/.config/systemd/user/frame-mic-tuner.service`（只放置并 `systemctl --user daemon-reload`。**只有加上 `--autostart` 时才 enable**）
- WirePlumber 的脚本和 conf（见上面的“切换机制”。内容相同就不动）

从 ＋ 启动时的行为：

- 没有常驻时：直接常驻
- 已经常驻时（无论手动启动、systemd 还是 ＋）：第二个不会连接 SteamVR，而是向常驻的那个发送 SIGUSR1 并立即结束（0.01 秒以内，退出码 0）。常驻侧用 `IVROverlay::ShowDashboard("sasaken.frame-mic-tuner")` 打开仪表盘并显示 Mic 面板
- 常驻的识别依靠 `$XDG_RUNTIME_DIR/frame-mic-tuner.lock`（没有则是 `/run/user/<uid>/`）的锁（flock）和写在里面的 PID。常驻进程挂掉时锁由 OS 解除
- 想结束常驻时，悬停在仪表盘的“Mic”图标上选“关闭”，或者用面板的“退出”

删除用 `./install.sh --uninstall`（如果连设置和保存的 `frame-mic.*` 值也要删除，加 `--purge`）。

## 自启动（systemd 用户服务）

用面板的“随 SteamVR 启动”切换（先用 `./install.sh` 放入单元文件。也可以用 `./install.sh --autostart` 启用）。

- 开 = `systemctl --user enable frame-mic-tuner.service`（会在 `~/.config/systemd/user/steamvr.service.wants/` 里创建链接），关 = `disable`。用 fork＋exec 只传固定的参数
- **不执行 `start` / `--now`**。以免与正在运行的实例冲突。从下次 SteamVR（`steamvr.service`）启动时开始生效
- 当前状态用 `systemctl --user is-enabled frame-mic-tuner.service` 读取（`enabled` / `disabled` / `not-found`）。状态由 systemd 持有，不保存到本应用的设置文件里
- 单元：`After`、`PartOf`、`WantedBy=steamvr.service`、`Restart=always`、`RestartPreventExitStatus=3`、`SuccessExitStatus=3`、`ExecStart=%h/.local/bin/frame-mic-tuner`
- 日志：`journalctl --user -u frame-mic-tuner -f`

从 systemd 启动但已经有常驻进程时（例如手动启动的还在），不发送 SIGUSR1，而是**以退出码 3 安静地结束**（防止 `Restart=always` 每 5 秒不断打开面板）。

- 只有存在环境变量 `INVOCATION_ID`，**并且** `/proc/self/cgroup` 是 `.../frame-mic-tuner.service` 时，才认为是“从 systemd 启动的”
- 为什么不只看 `INVOCATION_ID`：Frame 的 Steam 客户端本身运行在 `steam.service`（systemd 用户服务）里并带有 `INVOCATION_ID`，从 ＋ 启动的子进程也会继承它（2026-09-27 用 `/proc/<Steam 的 PID>/environ` 确认）。如果只看 `INVOCATION_ID`，从 ＋ 第二次启动就不会变成“打开面板”，而是默默结束

## 设置

`~/.config/frame-mic-tuner/config.json`（如果有 `$XDG_CONFIG_HOME` 则在它下面）。**没有也能以默认值运行**。保存的是语言和噪声抑制强度（只在拖动滑块时）。

```json
{
  "language": "ja"
}
```

| 键 | 默认值 | 说明 |
|---|---|---|
| `language` | Steam 的语言 | 界面文案的语言。没有时按 Steam 的语言设置（`~/.steam/registry.vdf` 的 `language`，只读）：`schinese`/`tchinese`/`chinese` 为简体中文，`japanese` 为日文，`english` 为英文，其他以及读不到时用本分支的默认语言简体中文（读不到时依次看 `LC_ALL`、`LC_MESSAGES`、`LANG`）。`"ja"`（日文）、`"en"`（英文）或 `"zh"`（简体中文）。用面板的语言按钮更改后会保存（先写入临时文件再替换） |
| `tab` | `"quick"` | 最后查看的标签页。`"quick"`（简单）或 `"fine"`（精细调整）。切换标签页时写入，打开面板时显示这个标签页 |
| `ns_vad_threshold_percent` | （无） | 噪声抑制的判定严格度（0～99）。拖动滑块时写入，应用启动时重新施加。只有与 `ns_vad_grace_ms` 两个都齐全时才使用 |
| `ns_vad_grace_ms` | （无） | 噪声抑制的保持时间（0～1000ms）。同上。没有时什么都不施加，保持 SteamOS 的值（23 / 500） |

- 麦克风的设置（回声消除、噪声抑制）由 WirePlumber 保存到 `~/.local/state/wireplumber/sm-settings`（`--save`）。重启后仍保持最后选择的状态
- 想用别的设置文件时用 `--config 路径`
- 文案集中在 `src/i18n.cpp` 的表里（日文、英文、简体中文）。日志和 `--print` 的输出不放进这张表，直接写在代码里

## 遵守的原则

- **不重启 PipeWire、WirePlumber**（重启后 SteamVR、Steam Link、游戏的音频可能回不来）。切换只用 `wpctl settings --save`
- 不改写 ALSA 混音器（amixer）（改 `VA DMIC MUX` 会让麦克风音量掉到 0，这是个坑）。不改写 `/etc` 和 Valve 的脚本。不使用 sudo
- 不碰摄像头（`/dev/video*`）、GPIO、sysfs、`/persist`
- 外部命令（`wpctl`、`pw-link`、`systemctl`）用 fork＋execvp，不经过 shell，只传固定参数。标准输出和标准错误用管道读取，2 秒（`systemctl enable/disable` 是 5 秒）内没结束就 SIGKILL，任何情况下都用 `waitpid` 收尾（不留僵尸进程）。子进程里先把信号掩码恢复为空再 exec
- 外部命令在工作线程里执行（不打断每 33ms 一次的指针响应）。主线程只用 mutex 读取状态的副本。SIGTERM、SIGINT、SIGUSR1 在工作线程里屏蔽，由主线程接收
- 面板关闭期间不执行任何外部命令（工作线程只是等着被请求）。主线程只是每 0.25 秒看一次事件

负载（2026-09-27 在 Frame 上实测，`/proc/<pid>/stat` 的 CPU 时间）：

| 状态 | 本体（所有线程） | 子进程（wpctl、pw-link、systemctl） |
|---|---|---|
| 面板关闭 | 10 秒 0～1 tick（约 10ms） | 无 |
| 面板打开 | 10 秒约 100ms（1%） | 10 秒约 190ms（1.9%） |

- 打开期间的重新读取是：`wpctl settings`（一次列表读取两个设置。按每个键分别调用一次的耗时相同，所以合并成一次减半）和 `pw-link -l` 每 1 秒一次，`systemctl --user is-enabled` 每 5 秒一次
- 重新读取的结果与上次相同时不重绘
- 只有录音、播放期间，为了驱动音量表和播放位置，每秒重绘 15 次

## 自修复（仪表盘覆盖层消失时）

2026-09-27 11:05，在测试中把第二个实例连接到 SteamVR 并结束后，常驻的仪表盘覆盖层从 SteamVR 里消失了（常驻还在运行，但 `--probe` 的 `FindOverlay` 返回 `VROverlayError_UnknownOverlay`。看不到 Mic 图标，用 SIGUSR1 调用 `ShowDashboard` 也打不开任何东西）。vrdashboard、vrcompositor 重建时也可能发生同样的事，所以让常驻进程自己发现并重建。

- 检查方法：调用一次 `IVROverlay::FindOverlay("sasaken.frame-mic-tuner", &found)`。判定用 `judgeOverlay()`（不调用 OpenVR 的函数。可以用 `--self-test` 测试）
  - `VROverlayError_None`，且 `found` 是自己持有的句柄 → 存在
  - 其他错误（`UnknownOverlay` 等），或者自己不持有句柄（上次重建失败）→ 已消失
  - 是 `None` 但是别的句柄 → 别的东西占用了这个键（尝试重建，但不碰对方）
- 间隔：每 3 秒（面板关闭期间也是）。常驻的主循环在关闭期间每 0.25 秒转一次，所以在里面看时间再调用即可。只有一次 IPC，所以关闭期间的 CPU 几乎不增加。SIGUSR1（从 ＋ 第二次启动）要调用 `ShowDashboard` 之前也会检查一次
- 重建步骤（`VrOverlay::ensureOverlay()`）：
  1. 把原因写进日志（“自修复: 仪表盘覆盖层已从 SteamVR 消失（FindOverlay(...) -> ...）。正在重建”）
  2. 对自己旧的句柄调用 `ClearOverlayTexture`（面板、缩略图）和 `DestroyOverlay`。前提是已经消失，所以忽略错误
  3. 从 `CreateDashboardOverlay` 重新开始（宽度、输入、鼠标刻度、关闭按钮标志。与 `connect()` 用同一个函数 `createDashboardOverlay()`）
  4. 调用方（main）绘制缩略图和面板图像，照旧用 Vulkan 的 `SetOverlayTexture` 重新发送（Vulkan 的图像直接复用。不使用 `SetOverlayRaw`）
- 创建失败时（`KeyInUse` = 别的进程持有同一个键等），不删除对方，在下一次检查（3 秒后）重试。相同原因的日志不重复刷屏。创建成功时输出“已重建”
- 真正制造出消失状态的测试会破坏常驻进程，所以没有在实机上做（判定用 `--self-test`，重建流程靠代码确认）

## 图像传送方式与退出处理

- 面板和缩略图的图像通过 `IVROverlay::SetOverlayTexture` 传入 Vulkan 的 `VkImage`（图像两张交替使用，等传输完成后再传）
- 不使用 `SetOverlayRaw`。在实机上，替换后 15～40ms 会变成没有图像的状态（`VROverlayError_InvalidTexture`）而闪烁，而且它使用共享内存，也疑似是进程刚结束后 vrcompositor 发生 SIGBUS 的原因
- Vulkan 用 OpenVR 要求的扩展（`GetVulkanInstanceExtensionsRequired` / `GetVulkanDeviceExtensionsRequired`）和 HMD 的 GPU（`GetOutputDevice`）创建
- 面板只在可见时、只在有变化时绘制。刚连接后先放入一张（加载中的显示），以免第一次被选中时出现没有图像的瞬间
- 仪表盘图标的“关闭”通过 `VROverlayFlags_EnableControlBarClose` → `VREvent_OverlayClosed` 传过来，与面板的“退出”一样是退出码 3（systemd 也不会重启）。SteamVR 自身的退出（`VREvent_Quit`）是退出码 0
- 诊断时从别的进程调用 `GetOverlayImageData`，在带有 Vulkan 纹理的覆盖层上**调用方以 SIGSEGV 崩溃了**（2026-09-27。SteamVR 侧没事）。`--probe` 里不使用它，而是用 `GetOverlayTextureSize` 确认图像已放入（1200x700、缩略图 256x256）

退出处理的顺序（SIGTERM / SIGINT、`VREvent_Quit`、vrserver 消失、“关闭”、“退出”中任何一个都一样。每一步的返回值都会以 `[VR] 退出处理 ... -> VROverlayError_...` 的形式写进日志）：

0. 停止声音检查的录音、播放，清理 PipeWire 的流和线程（录到的音频连同内存一起消失）
1. `ClearOverlayTexture`（面板、缩略图）
2. `DestroyOverlay`（面板。缩略图会一起消失）
3. 等待 400ms（合成器约 36 帧。让对方放开已移除的纹理）
4. `VR_Shutdown`
5. 销毁 Vulkan 的图像、缓冲、设备、实例（按 OpenVR 的规矩在 `VR_Shutdown` 之后）
6. 停止并等待工作线程（如果有正在执行的命令，就等它结束）

## 文件

| 文件 | 作用 |
|---|---|
| `src/main.cpp` | 命令行、常驻的循环、重复启动（flock、SIGUSR1、来自服务的重复）、确认用的选项 |
| `src/mic_state.*` | 读取、写入 `wpctl`、`pw-link`、`systemctl` 的输出（解析器） |
| `src/mic_worker.*` | 执行外部命令的工作线程 |
| `src/command.*` | fork＋execvp、管道、超时、waitpid |
| `src/mic_panel.*` | 面板的绘制和按钮命中判定（每次绘制都重新生成布局）、缩略图（麦克风的图） |
| `src/voice_check.*` | 声音检查（用 pw_thread_loop、pw_stream 录音和播放，5 条历史记录，波形） |
| `src/theme.*` | 颜色的定义和 WCAG 对比度的计算（`--contrast-report`） |
| `src/vr_overlay.*` | OpenVR 的连接、仪表盘覆盖层、事件、退出处理、`--probe` |
| `src/vk_texture.*`、`src/draw.*`、`src/json.*` | Vulkan 的图像、绘制的部件、JSON（与同一作者的其他覆盖层共用） |
| `src/i18n.*`、`src/config.*` | 文案表（日文、英文、简体中文）、设置文件（语言和噪声抑制强度） |
| `contrib/` | `.desktop`、`.service`、图标、设置的示例、`wireplumber/`（切换机制） |
| `install.sh` | 构建与安装、卸载 |
