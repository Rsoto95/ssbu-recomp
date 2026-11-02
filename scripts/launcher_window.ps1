# The launcher of the Super Smash Bros. Ultimate native PC port (GPL-3.0). `SSBU Launcher.bat` starts this file:
#   powershell -NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File scripts\launcher_window.ps1
# Windows PowerShell 5.1 + System.Windows.Forms only, like scripts\installer_window.ps1; nothing is installed or sent anywhere.
#
# What it does: shows whether the game is installed, starts it (Play SSBU.bat), and edits the settings the game reads at start:
# <install location>\config\graphics.ini (display, graphics, sound, language, competitive, online) and controllers.ini
# (pads, keyboard). Only the keys you change are written, each `key = value` line replaced in place with every comment kept
# (as gfx_options.c gfx_ini_set does), a missing key appended. Environment variables (SSBU_*) still win over the files.
# Plus a setup guide, troubleshooting and about pages.
#
# Test switches: -SelfTest checks the ini reader / writer on a temporary file and prints the result (no window);
# -Shot <folder> saves a picture of every page into that folder and closes; -Page <name> opens that page;
# -TestMove <folder> moves the installation there as Folders > Move would, every question answered yes, messages printed.
param(
    [switch]$SelfTest,
    [string]$Shot = '',
    [string]$Page = 'Home',
    [string]$TestMove = ''      # test only: move the installation to this folder with every question answered yes, then close
)
$ErrorActionPreference = 'Continue'
Set-StrictMode -Off

$repo = Split-Path -Parent $PSScriptRoot

# ---------------------------------------------------------------------------------------------------- ini files
# A file is kept as its lines; a key belongs to the section whose [header] is above it ('' = before the first header).
function Read-IniLines([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return ,@() }
    return ,@([IO.File]::ReadAllLines($path))
}
function Find-IniLine($lines, [string]$section, [string]$key) {
    $cur = ''
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t.StartsWith('[') -and $t.EndsWith(']')) { $cur = $t.Substring(1, $t.Length - 2); continue }
        if ($cur -ne $section -or $t.StartsWith(';') -or $t.StartsWith('#')) { continue }
        $eq = $t.IndexOf('=')
        if ($eq -gt 0 -and $t.Substring(0, $eq).Trim() -ieq $key) { return $i }
    }
    return -1
}
function Get-IniValue($lines, [string]$section, [string]$key, [string]$default) {
    $i = Find-IniLine $lines $section $key
    if ($i -lt 0) { return $default }
    $t = $lines[$i]; $v = $t.Substring($t.IndexOf('=') + 1)
    $sc = $v.IndexOf(';'); if ($sc -ge 0) { $v = $v.Substring(0, $sc) }      # a comment after the value
    return $v.Trim()
}
function Set-IniValue($lines, [string]$section, [string]$key, [string]$value) {
    $list = New-Object System.Collections.ArrayList; [void]$list.AddRange([string[]]@($lines))
    $i = Find-IniLine $list $section $key
    if ($i -ge 0) { $list[$i] = "$key = $value"; return ,$list.ToArray() }
    if ($section -eq '') {                                      # before the first section header
        $at = $list.Count
        for ($j = 0; $j -lt $list.Count; $j++) { $t = $list[$j].Trim(); if ($t.StartsWith('[') -and $t.EndsWith(']')) { $at = $j; break } }
        $list.Insert($at, "$key = $value"); return ,$list.ToArray()
    }
    $start = -1
    for ($j = 0; $j -lt $list.Count; $j++) { if ($list[$j].Trim() -ieq "[$section]") { $start = $j; break } }
    if ($start -lt 0) { [void]$list.Add("[$section]"); [void]$list.Add("$key = $value"); return ,$list.ToArray() }
    $end = $list.Count
    for ($j = $start + 1; $j -lt $list.Count; $j++) { $t = $list[$j].Trim(); if ($t.StartsWith('[') -and $t.EndsWith(']')) { $end = $j; break } }
    while ($end -gt $start + 1 -and $list[$end - 1].Trim() -eq '') { $end-- }      # keep the blank line before the next header
    $list.Insert($end, "$key = $value"); return ,$list.ToArray()
}
function Write-IniLines([string]$path, $lines) {
    $dir = Split-Path -Parent $path
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    # CRLF and no byte-order mark, as gfx_options.c writes the file
    [IO.File]::WriteAllText($path, (($lines -join "`r`n") + "`r`n"), (New-Object Text.UTF8Encoding($false)))
}

if ($SelfTest) {
    $tmp = [IO.Path]::Combine([IO.Path]::GetTempPath(), 'ssbu_launcher_selftest.ini')
    $fails = 0
    function Check([string]$what, $got, $want) { if ("$got" -ne "$want") { Write-Output "FAIL $what : got '$got', want '$want'"; $script:fails++ } else { Write-Output "ok   $what" } }
    Write-IniLines $tmp @('; head comment', '; window = windowed | borderless', 'window = windowed', 'volume = 80 ; loud', '', '[general]', 'deadzone = 0.10', '', '[keyboard]', 'A = Z')
    $l = Read-IniLines $tmp
    Check 'read top-level key' (Get-IniValue $l '' 'window' 'x') 'windowed'
    Check 'commented-out line is not the key' (Find-IniLine $l '' 'window') 2
    Check 'value without trailing comment' (Get-IniValue $l '' 'volume' 'x') '80'
    Check 'section key' (Get-IniValue $l 'general' 'deadzone' 'x') '0.10'
    Check 'key of another section is not found' (Get-IniValue $l '' 'deadzone' 'none') 'none'
    $l = Set-IniValue $l '' 'window' 'borderless'
    $l = Set-IniValue $l '' 'unlock_all' 'on'
    $l = Set-IniValue $l 'general' 'rumble' 'off'
    $l = Set-IniValue $l 'keyboard' 'A' 'SPACE'
    $l = Set-IniValue $l 'players' 'x' '1'
    Write-IniLines $tmp $l; $l = Read-IniLines $tmp
    Check 'replaced in place' (Get-IniValue $l '' 'window' 'x') 'borderless'
    Check 'comments kept' $l[1] '; window = windowed | borderless'
    Check 'new top-level key before the first section' (Get-IniValue $l '' 'unlock_all' 'x') 'on'
    Check 'new key appended inside its section' (Get-IniValue $l 'general' 'rumble' 'x') 'off'
    Check 'blank line before [keyboard] kept' ($l[([array]::IndexOf($l, '[keyboard]')) - 1]) ''
    Check 'section key replaced' (Get-IniValue $l 'keyboard' 'A' 'x') 'SPACE'
    Check 'missing section created' (Get-IniValue $l 'players' 'x' 'none') '1'
    Check 'no BOM, CRLF' ([BitConverter]::ToString([IO.File]::ReadAllBytes($tmp)[0..2])) '3B-20-68'
    Remove-Item -LiteralPath $tmp -Force
    Write-Output $(if ($fails) { "launcher selftest: $fails FAILED" } else { 'launcher selftest: all passed' })
    exit $(if ($fails) { 1 } else { 0 })
}

# ---------------------------------------------------------------------------------------------------- where things are
# The install location: <repo>\build, or the folder chosen in the installer and remembered in build_location.txt
$buildDir = Join-Path $repo 'build'
$locFile = Join-Path $repo 'build_location.txt'
if (Test-Path -LiteralPath $locFile) {
    try {
        $t = [IO.File]::ReadAllLines($locFile, [Text.Encoding]::UTF8) | Select-Object -First 1
        if ($t) { $t = [Environment]::ExpandEnvironmentVariables($t.Trim().Trim('"')); if ($t) { $buildDir = $t } }
    } catch {}
}
$P = @{
    Build = $buildDir
    Work = Join-Path $buildDir 'work'
    Exe = Join-Path $buildDir 'work\ssbu_native.exe'
    GameDll = Join-Path $buildDir 'work\ssbu_game_eh_o1.dll'
    Config = Join-Path $buildDir 'config'
    Graphics = Join-Path $buildDir 'config\graphics.ini'
    Controllers = Join-Path $buildDir 'config\controllers.ini'
    Dlc = Join-Path $buildDir 'config\dlc.txt'
    Saves = Join-Path $buildDir 'saves'
    Save = Join-Path $buildDir 'saves\save'
    Backups = Join-Path $buildDir 'saves\backups'
    Screens = Join-Path $buildDir 'screenshots'
    Logs = Join-Path $buildDir 'logs'
    State = Join-Path $buildDir 'state.json'
    Play = Join-Path $repo 'Play SSBU.bat'
    Install = Join-Path $repo 'Install SSBU.bat'
}

# ---------------------------------------------------------------------------------------------------- the settings
# File G = graphics.ini (read at the game's start), C = controllers.ini (re-read within a second while the game runs).
# Kind: choice (fixed list), edit (a list you may also type into), text. Check: a regex the value must match.
$onoff = @('off', 'on')
$gpuNames = @()      # the GPUs Windows knows (graphics.ini gpu = any part of a name; the game matches it against its Vulkan device names)
try { $gpuNames = @(Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object { $_.Name } | Where-Object { $_ } | Select-Object -Unique) } catch {}
$settings = @(
    @{ Page = 'Display'; F = 'G'; S = ''; K = 'window'; Label = 'Window mode'; Kind = 'choice'; Values = @('windowed', 'borderless', 'fullscreen'); Def = 'windowed'
       Help = 'windowed: a normal window of the size below. borderless: a window without frame covering the desktop. fullscreen: covers the whole monitor. F11 switches to borderless while you play.' }
    @{ Page = 'Display'; F = 'G'; S = ''; K = 'width'; Label = 'Window width'; Kind = 'edit'; Values = @('1280', '1600', '1920', '2560', '3840'); Def = '1280'; Check = '^\d{3,5}$'
       Help = 'Size of the window in windowed mode, in pixels. The picture keeps 16:9.' }
    @{ Page = 'Display'; F = 'G'; S = ''; K = 'height'; Label = 'Window height'; Kind = 'edit'; Values = @('720', '900', '1080', '1440', '2160'); Def = '720'; Check = '^\d{3,5}$'
       Help = 'Height of the window in windowed mode, in pixels.' }
    @{ Page = 'Display'; F = 'G'; S = ''; K = 'vsync'; Label = 'Frame pacing (vsync)'; Kind = 'choice'; Values = @('auto', 'vrr', 'display', 'clock', 'on', 'off', 'mailbox'); Def = 'auto'
       Help = 'auto picks the best for your monitor (even frames on 60 / 120 / 240 Hz). vrr: for a G-Sync / FreeSync monitor, even frames at any refresh rate (turn variable refresh on in the GPU control panel; use fullscreen or borderless). on: no tearing, a little more input delay. off: lowest delay, may tear. clock: the game''s own 60 Hz clock. Vulkan only.' }
    @{ Page = 'Display'; F = 'G'; S = ''; K = 'console_mode'; Label = 'Console mode'; Kind = 'choice'; Values = @('docked', 'handheld'); Def = 'docked'
       Help = 'docked: the game draws at 1080p, as on a TV. handheld: it draws at 720p, lighter for weak GPUs.' }
    @{ Page = 'Display'; F = 'G'; S = ''; K = 'frame_time_overlay'; Label = 'Frame time in the title bar'; Kind = 'choice'; Values = $onoff; Def = 'off'
       Help = 'Shows the frame time in the window title and writes it to the log once a second.' }
    @{ Page = 'Display'; F = 'G'; S = ''; K = 'screenshot_key'; Label = 'Screenshot key'; Kind = 'choice'; Values = @('F12', 'F10', 'F9', 'F8', 'F7', 'F6', 'F5', 'F4', 'F3', 'off'); Def = 'F12'
       Help = 'One press while the game window is in front saves the picture as a PNG in the Screenshots folder (Home page). F1, F2 and F11 are taken by the menu, the stats and fullscreen.' }
    @{ Page = 'Folders'; F = 'G'; S = ''; K = 'screenshot_dir'; Label = 'Screenshot folder'; Kind = 'text'; Def = ''
       Help = 'Where the screenshot key saves its pictures. Empty: the screenshots folder of the installation. Made when missing. Choose one with Browse...' }

    @{ Page = 'Graphics'; F = 'G'; S = ''; K = 'profile'; Label = 'Quality profile'; Kind = 'choice'; Values = @('custom', 'low'); Def = 'custom'
       Help = 'low: sets resolution, filtering and frame pacing for weak or integrated GPUs (it overrides those three below). custom: the values below.' }
    @{ Page = 'Graphics'; F = 'G'; S = ''; K = 'renderer'; Label = 'Renderer'; Kind = 'choice'; Values = @('vulkan', 'opengl'); Def = 'vulkan'
       Help = 'vulkan is the main renderer (needs a Vulkan 1.3 driver). opengl is a fallback with fewer features (no F1 menu, no on-screen keyboard).' }
    @{ Page = 'Graphics'; F = 'G'; S = ''; K = 'gpu'; Label = 'GPU'; Kind = 'edit'; Values = @('auto') + $gpuNames; Def = 'auto'
       Help = 'auto = the dedicated GPU first. Or pick one of the GPUs Windows lists here (any part of its name works).' }
    @{ Page = 'Graphics'; F = 'G'; S = ''; K = 'resolution_scale'; Label = 'Resolution scale'; Kind = 'edit'; Values = @('0.5', '0.75', '1.0', '1.5', '2.0'); Def = '1.0'; Check = '^\d+(\.\d+)?$'
       Help = 'Internal resolution compared with the game''s own (0.25 to 2.0). Above 1 is sharper and needs a faster GPU. The line below shows what the GPU renders and what your window shows.' }
    @{ Page = 'Graphics'; F = 'G'; S = ''; K = 'sharpening'; Label = 'Sharpening'; Kind = 'edit'; Values = @('0', '30', '50', '70', '100'); Def = '0'; Check = '^(\d{1,2}|100)$'
       Help = '0 = off. Sharpens the picture on its way to the window, without halos. Try 30 to 50. Costs one small GPU pass per frame. Vulkan only.' }
    @{ Page = 'Graphics'; F = 'G'; S = ''; K = 'anisotropic_filtering'; Label = 'Texture filtering'; Kind = 'choice'; Values = @('game', '2', '4', '8', '16'); Def = 'game'
       Help = 'game = as on the console. 2 to 16 = sharper textures at an angle (floors, stages), costs little on most GPUs.' }
    @{ Page = 'Graphics'; F = 'G'; S = ''; K = 'shader_prepare'; Label = 'Shader preparation'; Kind = 'choice'; Values = @('first_run', 'background', 'off'); Def = 'first_run'
       Help = 'first_run: on the first start the game waits once while every shader is prepared for your GPU (no stutter afterwards). background: play at once, prepared while you play. off: only when needed (stutters).' }
    @{ Page = 'Graphics'; F = 'G'; S = ''; K = 'shader_load'; Label = 'Shader loading'; Kind = 'choice'; Values = @('start', 'background', 'loading'); Def = 'start'
       Help = 'The prepared shaders (kept on your PC in the cache folder) are made ready in memory at every start. start: behind a LOADING SHADERS screen before the game shows anything. background: the game starts at once and they load while the intro and menus run (faster to the menus; menus a little less smooth meanwhile). loading: each one at its loading screen (less memory).' }
    @{ Page = 'Graphics'; F = 'G'; S = ''; K = 'shader_wait'; Label = 'Shader not ready'; Kind = 'choice'; Values = @('hold', 'skip'); Def = 'hold'
       Help = 'hold: the frame waits for a missing shader (a hitch). skip: near a fight the object is left out until it is ready (no hitch, may flicker).' }

    @{ Page = 'Audio'; F = 'G'; S = ''; K = 'volume'; Label = 'Volume'; Kind = 'edit'; Values = @('auto', '100', '80', '60', '40', '20', '0'); Def = 'auto'; Check = '^(auto|\d{1,3})$'
       Help = 'auto = 100. Music and effects apart: the game''s own Options > Sound.' }
    @{ Page = 'Audio'; F = 'G'; S = ''; K = 'language'; Label = 'Game language'; Kind = 'choice'; Values = @('auto', 'en-US', 'en-GB', 'es', 'es-419', 'fr', 'fr-CA', 'de', 'it', 'nl', 'ru', 'ja', 'ko', 'zh-Hans', 'zh-Hant'); Def = 'auto'
       Help = 'auto = the Windows display language. Without your console''s firmware fonts, Japanese, Korean and Chinese text is shown as empty boxes: pick a Latin-alphabet language then.' }

    @{ Page = 'Competitive'; F = 'G'; S = ''; K = 'unlock_all'; Label = 'Unlock fighters and stages'; Kind = 'choice'; Values = $onoff; Def = 'off'
       Help = 'on: every fighter of the base roster and every stage is available at once, and no challenger fights interrupt after a match. Nothing is written into your save (turn it off and the game is as before). Downloadable fighters are not touched: owning them unlocks them. Spirits, music and other rewards stay as in the game. Game versions 13.0.4 and 13.0.5.' }
    @{ Page = 'Competitive'; F = 'G'; S = ''; K = 'latency'; Label = 'Input latency'; Kind = 'choice'; Values = @('normal', 'low', 'lowest'); Def = 'normal'
       Help = 'low / lowest shorten the time between a button press and the picture. lowest needs a steady 60 fps on your PC. Vulkan only.' }
    @{ Page = 'Competitive'; F = 'G'; S = ''; K = 'runahead'; Label = 'Run-ahead (experimental)'; Kind = 'choice'; Values = $onoff; Def = 'off'
       Help = 'Runs the game one frame ahead to remove one more frame of delay. Experimental: not every build has the pieces it needs (the F1 menu greys it out then).' }
    @{ Page = 'Competitive'; F = 'G'; S = ''; K = 'stats_overlay'; Label = 'Stats overlay'; Kind = 'choice'; Values = $onoff; Def = 'off'
       Help = 'A small box with fps, frame time, ping, input delay and rollback frames. F2 switches it in the game.' }
    @{ Page = 'Competitive'; F = 'G'; S = ''; K = 'menu'; Label = 'In-game settings menu (F1)'; Kind = 'choice'; Values = @('on', 'off'); Def = 'on'
       Help = 'The settings menu over the game: F1, a controller''s Home button, or - and + held together for 1 second.' }

    @{ Page = 'Online'; F = 'G'; S = ''; K = 'online_mode'; Label = 'Online play'; Kind = 'choice'; Values = @('lobby', 'lan', 'host', 'join', 'off'); Def = 'lobby'
       Help = 'Experimental. lobby: the project''s public lobby (default). lan: games on the same network. host: you make a room code (the log and the F1 menu show it). join: you enter a friend''s room code below. off: no network. Then use the game''s own Local Wireless: Create Room / Rooms List.' }
    @{ Page = 'Online'; F = 'G'; S = ''; K = 'online_server'; Label = 'Lobby server'; Kind = 'text'; Def = 'opensmash-lobby.duckdns.org:39500'
       Help = 'host:port of the lobby (for lobby mode). The default is the project''s public lobby.' }
    @{ Page = 'Online'; F = 'G'; S = ''; K = 'online_join'; Label = 'Room code to join'; Kind = 'text'; Def = ''
       Help = 'The ROOM CODE your friend got as host (for join mode), like XXXXX-XXXXX.' }
    @{ Page = 'Online'; F = 'G'; S = ''; K = 'player_name'; Label = 'Your name'; Kind = 'text'; Def = ''
       Help = 'The name the other players see. Empty = your Windows user name.' }
    @{ Page = 'Online'; F = 'G'; S = ''; K = 'rollback'; Label = 'Rollback netcode (experimental)'; Kind = 'choice'; Values = $onoff; Def = 'off'
       Help = 'Smooth fights at high ping: a late input rolls the game back a few frames instead of slowing both players. Both players need it on and the same game version.' }
    @{ Page = 'Online'; F = 'G'; S = ''; K = 'online_upnp'; Label = 'Open the router port (UPnP)'; Kind = 'choice'; Values = @('on', 'off'); Def = 'on'
       Help = 'For host mode: asks your router to let the other player in (UDP 39500). If it fails, forward that port by hand or use the lobby.' }
    @{ Page = 'Online'; F = 'G'; S = ''; K = 'online_direct'; Label = 'Direct connections'; Kind = 'choice'; Values = @('on', 'off'); Def = 'on'
       Help = 'on: players connect directly when they can. off: everything goes through the lobby, which then never gives anyone your address.' }

    @{ Page = 'Controls'; F = 'C'; S = 'general'; K = 'layout'; Label = 'Face button layout'; Kind = 'choice'; Values = @('auto', 'positional', 'labels'); Def = 'auto'
       Help = 'positional: the button in Nintendo''s A position (right) is A. labels: the printed letter decides (an Xbox pad''s A is A). auto: labels for GameCube controllers, positional otherwise.' }
    @{ Page = 'Controls'; F = 'C'; S = 'general'; K = 'deadzone'; Label = 'Stick dead zone'; Kind = 'edit'; Values = @('0.05', '0.10', '0.15', '0.20'); Def = '0.10'; Check = '^0?\.\d+$|^[01]$'
       Help = 'Stick travel (0 to 1) ignored around the centre. Raise it if a fighter walks by itself.' }
    @{ Page = 'Controls'; F = 'C'; S = 'general'; K = 'outer_zone'; Label = 'Stick outer zone'; Kind = 'edit'; Values = @('0.90', '0.95', '0.98', '1.0'); Def = '0.98'; Check = '^0?\.\d+$|^1(\.0+)?$'
       Help = 'Beyond this travel the stick counts as fully pushed (helps worn sticks reach dash and smash inputs).' }
    @{ Page = 'Controls'; F = 'C'; S = 'general'; K = 'trigger_threshold'; Label = 'Trigger threshold'; Kind = 'edit'; Values = @('0.15', '0.33', '0.50'); Def = '0.33'; Check = '^0?\.\d+$'
       Help = 'How far ZL / ZR must be pressed to count.' }
    @{ Page = 'Controls'; F = 'C'; S = 'general'; K = 'rumble'; Label = 'Rumble'; Kind = 'choice'; Values = @('on', 'off'); Def = 'on'; Help = 'Controller vibration.' }
    @{ Page = 'Controls'; F = 'C'; S = 'general'; K = 'gamecube_style'; Label = 'GameCube adapter controllers'; Kind = 'choice'; Values = @('native', 'pro'); Def = 'native'
       Help = 'For the GameCube adapter (Nintendo WUP-028, Mayflash in Switch mode). native: the game sees real GameCube controllers. pro: they are shown as Pro Controllers (try it if one misbehaves).' }
    @{ Page = 'Controls'; F = 'C'; S = 'keyboard'; K = 'player'; Label = 'Keyboard is player'; Kind = 'choice'; Values = @('auto', '1', '2', '3', '4', '5', '6', '7', '8', 'none'); Def = 'auto'
       Help = 'auto: player 1, together with the controller of player 1 (either one plays, whenever the controller connects). 2: a second player on the keyboard. none: the keyboard does not play.' }
)
$keyHelp = 'A letter or digit, LEFT UP RIGHT DOWN ENTER BACKSPACE SPACE TAB ESC SHIFT CTRL ALT F1..F12, NUM0..NUM9, or none.'
$keys = @(@('A', 'Z'), @('B', 'X'), @('X', 'C'), @('Y', 'V'), @('L', 'Q'), @('R', 'E'), @('ZL', 'A'), @('ZR', 'S'), @('PLUS', 'ENTER'), @('MINUS', 'BACKSPACE'),
          @('LSTICK_UP', 'UP'), @('LSTICK_DOWN', 'DOWN'), @('LSTICK_LEFT', 'LEFT'), @('LSTICK_RIGHT', 'RIGHT'),
          @('UP', 'I'), @('DOWN', 'K'), @('LEFT', 'J'), @('RIGHT', 'L'), @('STICK_L', 'none'), @('STICK_R', 'none'),
          @('RSTICK_UP', 'none'), @('RSTICK_DOWN', 'none'), @('RSTICK_LEFT', 'none'), @('RSTICK_RIGHT', 'none'))
$keyNames = @{ A = 'A'; B = 'B'; X = 'X'; Y = 'Y'; L = 'L'; R = 'R'; ZL = 'ZL'; ZR = 'ZR'; PLUS = '+ (pause)'; MINUS = '-'; LSTICK_UP = 'Left stick up'; LSTICK_DOWN = 'Left stick down'
    LSTICK_LEFT = 'Left stick left'; LSTICK_RIGHT = 'Left stick right'; UP = 'D-pad up'; DOWN = 'D-pad down'; LEFT = 'D-pad left'; RIGHT = 'D-pad right'; STICK_L = 'Left stick click'
    STICK_R = 'Right stick click'; RSTICK_UP = 'Right stick up (smash)'; RSTICK_DOWN = 'Right stick down'; RSTICK_LEFT = 'Right stick left'; RSTICK_RIGHT = 'Right stick right' }
foreach ($k in $keys) {
    $settings += @{ Page = 'Keyboard'; F = 'C'; S = 'keyboard'; K = $k[0]; Label = $keyNames[$k[0]]; Kind = 'text'; Def = $k[1]; Check = '^(?i)([a-z0-9]|left|up|right|down|enter|backspace|space|tab|esc|shift|ctrl|alt|f([1-9]|1[0-2])|num[0-9]|0x[0-9a-f]+|none)$'; Help = '' }
}

# ---------------------------------------------------------------------------------------------------- texts
$guide = @'
# Before you start
You need your own copy of Super Smash Bros. Ultimate and your own console keys. This project contains no game files, no keys and no firmware, and cannot get them for you.
- The base game: an .nsp, .nsz, .xci or .nca file, or an extracted dump folder.
- The update 13.0.4 or 13.0.5 as an .nsp (other versions are refused).
- prod.keys from your console. The installer finds it by itself in the folders of yuzu, Ryujinx, suyu, sudachi and Eden.
- Optional: your DLC .nsp files (put them next to the game), and a firmware dump (your console's own fonts; without it open-source fonts are used).
- Windows 10 or 11 (64-bit), a GPU with a Vulkan 1.3 driver, about 25 GB free on the drive you install to, 16 GB RAM recommended.

# 1. Install
- Double-click "Install SSBU.bat" in this folder. It sets up a private Python inside the folder (nothing is installed on Windows).
- The window shows what it found: your keys, the game, the update, the DLC. Check it, change a path if needed, choose an Install location on a drive with space.
- Press Install. The first build takes about 45 minutes to an hour. If it stops, double-click it again and press Resume: it goes on where it stopped.

# 2. Allow the game in your antivirus (recommended)
The game library is about 500 MB, built on your PC and not signed, so some antivirus programs remove it. In Windows Security > Virus & threat protection > Manage settings > Exclusions, add this folder. If "ssbu_game_eh_o1.dll is missing" appears, restore it from Protection history and press Resume in the installer.

# 3. First start
- Press Play here (or double-click "Play SSBU.bat").
- The first start shows a progress screen while every shader is prepared for your GPU, once. After that the game starts straight away. A driver update repeats this once.
- Windows may ask to let the game through the firewall: allow it for online play.

# 4. Your own save (optional)
With the game closed: the saves are in the Saves folder (button on the Home page) under save\save_data. Export your save from your Switch (JKSV or Checkpoint) or from an emulator (yuzu, Eden, suyu: right-click the game, Open Save Data Location; Ryujinx: Open User Save Directory). Rename the old "save" folder to "save_old", make a new empty "save" folder and copy your "save_data" folder into it.

# 5. Controllers
- Any controller SDL knows works (Xbox, PlayStation, Switch Pro, Joy-Con, 8BitDo...), up to 8 players, plugged in at any time.
- GameCube controllers: the Nintendo adapter or a Mayflash in Switch mode, with its WinUSB driver (from Slippi's installer or Zadig). Close Slippi or Delfinovin first: only one program can use the adapter.
- The Controls and Keyboard pages here change the basics; the F1 menu in the game remaps every button of every controller.
- "Test controllers" on the Controls page shows what each pad sends.

# 6. Going straight to competitive
- Competitive page: turn on "Unlock fighters and stages".
- Set Input latency to low (or lowest on a PC that holds 60 fps).
- In the game: Smash > rules (stock, time, items off, hazards off) and pick a stage. Save your ruleset there.

# 7. Online (experimental)
Online page: lobby (default), LAN, host a room or join with a code. Then in the game use Local Wireless: one player Create Room, the others Rooms List. Both players need the same game version and the same mods. There is no encryption: play with people you know.
'@
$trouble = @'
# The game does not start: "ssbu_game_eh_o1.dll is missing"
Usually an antivirus removed it. Windows Security > Protection history: restore / allow it, add this folder to the exclusions, then run "Install SSBU.bat" and press Resume (a few minutes, nothing is compiled again).

# The opening movie stays black
Windows cannot play the movies. Install "VP9 Video Extensions" and "Web Media Extensions" from the Microsoft Store, or run the installer again (it fetches the FFmpeg fallback).

# No music, only some effects
The log says "libopus NOT FOUND". Run the installer again (Resume): it downloads libopus.

# Text shows as empty boxes
Without your console's firmware fonts, Chinese, Japanese and Korean characters are missing. Choose a Latin-alphabet language (Audio & Language), or give the installer your firmware folder.

# Stutter in the first minutes
The shaders are being prepared for your GPU. It happens once (again after a driver update). Shader preparation "first_run" (Graphics) does it before you play.

# Slow or not 60 fps
Graphics: Quality profile low, or Resolution scale 0.75, or Console mode handheld. Close other programs. The F2 stats overlay shows the frame time.

# A controller does not work
Controls > Test controllers shows what it sends. A GameCube adapter needs the WinUSB driver and must not be open in another program. In controllers.ini a device can be ignored by its id.

# The game froze or crashed
The game writes ssbu_native_freeze.txt (a frozen game) or ssbu_native_crash.txt next to the game (Open game folder on the Home page) and its log into the Logs folder. Attach them (check them for your user name first) to a report on the project's Discord or GitHub.

# "Not enough space" while installing
Choose another Install location in the installer window (another drive). It is remembered.

# The settings here have no effect
A setting set as an environment variable (SSBU_...) wins over the file. Most settings are read when the game starts: close the game and start it again. Controller settings are used within a second, also while the game runs.
'@
$about = @'
# OpenSmash-Ultimate
Super Smash Bros. Ultimate (13.0.4 / 13.0.5) as a native PC program: the game's code is recompiled to x64 ahead of time, not emulated. The console's system libraries, graphics, sound, video, input and files are re-implemented by this project, and the picture is drawn with Vulkan.

# Legal
This project is not affiliated with, endorsed or sponsored by Nintendo, Bandai Namco or Sora Ltd. It contains no game code, no game assets, no keys and no firmware, and does not help anyone obtain them. You must own the game and dump it yourself. Everything derived from the game is made on your PC from your dump and never leaves it.

# Licence
GNU General Public License, version 3 or later (LICENSE in this folder).

# Credits
The project's maintainer and contributors, and the open-source projects it learned from: see README.md (Credits).
'@

# ---------------------------------------------------------------------------------------------------- UI
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class SsbuLauncherNative {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] public static extern int SetCurrentProcessExplicitAppUserModelID(string id);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] public struct DEVMODE {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra; public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels; public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool EnumDisplaySettings(string dev, int mode, ref DEVMODE dm);
    public static int RefreshRate() {      // the primary monitor's current refresh rate in Hz (0 = unknown)
        DEVMODE dm = new DEVMODE(); dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
        return EnumDisplaySettings(null, -1, ref dm) ? dm.dmDisplayFrequency : 0;      // ENUM_CURRENT_SETTINGS
    }
}
'@
[void][SsbuLauncherNative]::SetProcessDPIAware()
# an app id of its own, so the taskbar shows the launcher with the window's icon (media\opensmash.ico), not under PowerShell's
[void][SsbuLauncherNative]::SetCurrentProcessExplicitAppUserModelID('OpenSmash.SSBUNative.Launcher')
[System.Windows.Forms.Application]::EnableVisualStyles()

# dark mode follows Windows, as the installer window does (SSBU_INSTALLER_THEME=dark / light forces one)
$dark = $false
try {
    $tm = [string]$env:SSBU_INSTALLER_THEME
    if ($tm -eq 'dark') { $dark = $true }
    elseif ($tm -ne 'light') { $dark = ((Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' -Name AppsUseLightTheme -ErrorAction Stop).AppsUseLightTheme -eq 0) }
} catch {}
function RGB([int]$r, [int]$g, [int]$b) { return [System.Drawing.Color]::FromArgb($r, $g, $b) }
# colours: the page, the sidebar and bottom bar, cards (one per setting), fields, borders, hover / selected, the logo's red
if ($dark) {
    $cBg = RGB 20 20 23; $cSide = RGB 13 13 15; $cCard = RGB 30 30 35; $cField = RGB 42 42 48; $cBorder = RGB 54 54 62; $cHover = RGB 48 48 56
    $cFg = RGB 236 236 240; $cDim = RGB 150 150 160; $cSel = RGB 36 36 42
    $cAccent = RGB 230 36 36; $cAccentHi = RGB 250 70 70; $cOk = RGB 90 205 120; $cWarn = RGB 245 180 70; $cFail = RGB 255 105 100
} else {
    $cBg = RGB 245 245 248; $cSide = RGB 232 232 237; $cCard = [System.Drawing.Color]::White; $cField = RGB 250 250 252; $cBorder = RGB 212 212 220; $cHover = RGB 226 226 234
    $cFg = RGB 22 22 26; $cDim = RGB 96 96 106; $cSel = RGB 220 220 228
    $cAccent = RGB 210 24 24; $cAccentHi = RGB 232 52 52; $cOk = RGB 18 140 60; $cWarn = RGB 178 100 0; $cFail = RGB 200 30 30
}
$scale = 1.0
try { $gx = [System.Drawing.Graphics]::FromHwnd([IntPtr]::Zero); $scale = [Math]::Max(1.0, $gx.DpiX / 96.0); $gx.Dispose() } catch {}
function S([double]$v) { return [int][Math]::Round($v * $scale) }
$fontUi = New-Object System.Drawing.Font('Segoe UI', 9.5)
$fontBold = New-Object System.Drawing.Font('Segoe UI Semibold', 9.5)
$fontH1 = New-Object System.Drawing.Font('Segoe UI Semibold', 20)
$fontH2 = New-Object System.Drawing.Font('Segoe UI Semibold', 12)
$fontNav = New-Object System.Drawing.Font('Segoe UI', 10)
$fontPlay = New-Object System.Drawing.Font('Segoe UI Semibold', 14)
$fontSmall = New-Object System.Drawing.Font('Segoe UI', 8.5)
$fontGlyph = New-Object System.Drawing.Font('Segoe MDL2 Assets', 11)      # Windows 10 / 11's own icon font (sidebar, status)

$form = New-Object System.Windows.Forms.Form
$form.Text = 'SSBU native - launcher'
$form.AutoScaleMode = [System.Windows.Forms.AutoScaleMode]::None
$form.Font = $fontUi; $form.BackColor = $cBg; $form.ForeColor = $cFg
$form.StartPosition = 'CenterScreen'
$wa = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$form.ClientSize = New-Object System.Drawing.Size([Math]::Min((S 1040), [int]($wa.Width * 0.95)), [Math]::Min((S 700), [int]($wa.Height * 0.92)))
$form.MinimumSize = New-Object System.Drawing.Size([Math]::Min((S 820), [int]($wa.Width * 0.95)), [Math]::Min((S 520), [int]($wa.Height * 0.92)))
$ico = Join-Path $repo 'media\opensmash.ico'
if (Test-Path -LiteralPath $ico) { try { $form.Icon = New-Object System.Drawing.Icon($ico) } catch {} }

function New-Label([string]$text, $font = $null, $color = $null) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text; $l.AutoSize = $true; $l.UseMnemonic = $false; $l.Margin = New-Object System.Windows.Forms.Padding(0, (S 2), 0, (S 2))      # '&' is text, not a shortcut mark
    if ($font) { $l.Font = $font }
    $l.ForeColor = $(if ($color) { $color } else { $cFg })
    return $l
}
function New-Button([string]$text, [int]$w = 150, $font = $null, [switch]$Primary) {
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $text; $b.Width = (S $w); $b.Height = (S 34); $b.FlatStyle = 'Flat'; $b.Cursor = [System.Windows.Forms.Cursors]::Hand
    $b.Margin = New-Object System.Windows.Forms.Padding(0, (S 4), (S 8), (S 4))
    if ($Primary) {
        $b.BackColor = $cAccent; $b.ForeColor = [System.Drawing.Color]::White; $b.FlatAppearance.BorderSize = 0
        $b.FlatAppearance.MouseOverBackColor = $cAccentHi; $b.FlatAppearance.MouseDownBackColor = $cAccent
    } else {
        $b.BackColor = $cField; $b.ForeColor = $cFg; $b.FlatAppearance.BorderColor = $cBorder
        $b.FlatAppearance.MouseOverBackColor = $cHover; $b.FlatAppearance.MouseDownBackColor = $cBorder
    }
    if ($font) { $b.Font = $font }
    return $b
}
# a card: a panel of its own colour around the controls of one item (a setting, a folder ...)
function New-Card {
    $c = New-Object System.Windows.Forms.TableLayoutPanel
    $c.ColumnCount = 1; $c.AutoSize = $true; $c.AutoSizeMode = 'GrowAndShrink'; $c.BackColor = $cCard
    $c.Padding = New-Object System.Windows.Forms.Padding((S 18), (S 14), (S 18), (S 14)); $c.Margin = New-Object System.Windows.Forms.Padding(0, 0, 0, (S 10))
    $c.MinimumSize = New-Object System.Drawing.Size((S 660), 0)
    return $c
}
# dark-friendly drop-down lists: Windows paints a closed combo box with system colours; drawing the items ourselves keeps the theme
function Set-ComboTheme($cb) {
    $cb.FlatStyle = 'Flat'; $cb.BackColor = $cField; $cb.ForeColor = $cFg
    if ($cb.DropDownStyle -ne 'DropDownList') { return }      # an editable box draws its own text field
    $cb.DrawMode = 'OwnerDrawFixed'; $cb.ItemHeight = (S 20)
    $cb.Add_DrawItem({
        param($s, $e)
        $sel = ($e.State -band [System.Windows.Forms.DrawItemState]::Selected) -and -not ($e.State -band [System.Windows.Forms.DrawItemState]::ComboBoxEdit)
        $bg = New-Object System.Drawing.SolidBrush($(if ($sel) { $cAccent } else { $cField }))
        $e.Graphics.FillRectangle($bg, $e.Bounds); $bg.Dispose()
        if ($e.Index -ge 0) {
            $tr = $e.Bounds; $tr.X += (S 4)
            [System.Windows.Forms.TextRenderer]::DrawText($e.Graphics, [string]$s.Items[$e.Index], $s.Font, $tr, $(if ($sel) { [System.Drawing.Color]::White } else { $cFg }), 'Left, VerticalCenter, SingleLine, EndEllipsis')
        }
    })
}
function Open-Path([string]$p) {
    if (-not (Test-Path -LiteralPath $p)) {
        [void][System.Windows.Forms.MessageBox]::Show("Not there yet:`n$p`n`nIt is made when the game is installed or first started.", 'SSBU launcher', 'OK', 'Information'); return
    }
    Start-Process -FilePath $p
}

# --- layout: a sidebar of pages on the left, the page on the right, a bar with Save and Play at the bottom
$side = New-Object System.Windows.Forms.Panel
$side.Dock = 'Left'; $side.Width = (S 224); $side.BackColor = $cSide
$bar = New-Object System.Windows.Forms.Panel
$bar.Dock = 'Bottom'; $bar.Height = (S 68); $bar.BackColor = $cSide; $bar.Padding = New-Object System.Windows.Forms.Padding((S 20), (S 13), (S 20), (S 13))
$host_ = New-Object System.Windows.Forms.Panel
$host_.Dock = 'Fill'; $host_.BackColor = $cBg
$form.Controls.Add($host_); $form.Controls.Add($bar); $form.Controls.Add($side)
$sideLine = New-Object System.Windows.Forms.Panel; $sideLine.Dock = 'Right'; $sideLine.Width = 1; $sideLine.BackColor = $cBorder; $side.Controls.Add($sideLine)
$barLine = New-Object System.Windows.Forms.Panel; $barLine.Dock = 'Top'; $barLine.Height = 1; $barLine.BackColor = $cBorder

# the brand: the project's logo (media\opensmash_icon_512.png), its name, and what this window is
$logoFile = Join-Path $repo 'media\opensmash_icon_512.png'
if (Test-Path -LiteralPath $logoFile) {
    $logo = New-Object System.Windows.Forms.PictureBox
    $logo.SizeMode = 'Zoom'; $logo.Size = New-Object System.Drawing.Size((S 40), (S 40)); $logo.Location = New-Object System.Drawing.Point((S 18), (S 20))
    try { $logo.Image = [System.Drawing.Image]::FromFile($logoFile) } catch {}
    $side.Controls.Add($logo)
}
$brand = New-Label 'OpenSmash' (New-Object System.Drawing.Font('Segoe UI Semibold', 13)) $cFg
$brand.Location = New-Object System.Drawing.Point((S 66), (S 17))
$brandSub = New-Label 'ULTIMATE  -  LAUNCHER' $fontSmall $cAccent
$brandSub.Location = New-Object System.Drawing.Point((S 68), (S 42))
$side.Controls.Add($brand); $side.Controls.Add($brandSub)
$verText = 'not installed yet'
try { $verText = 'game ' + (Get-Content -LiteralPath $P.State -Raw | ConvertFrom-Json).game_version } catch {}
try { $h = (& git -C $repo rev-parse --short HEAD 2>$null); if ($h) { $verText += "  -  build $h" } } catch {}
$sideFoot = New-Label $verText $fontSmall $cDim
$sideFoot.Dock = 'Bottom'; $sideFoot.AutoSize = $false; $sideFoot.Height = (S 30); $sideFoot.Padding = New-Object System.Windows.Forms.Padding((S 18), 0, 0, (S 8)); $sideFoot.TextAlign = 'BottomLeft'
$side.Controls.Add($sideFoot)

$btnPlay = New-Button 'PLAY' 180 $fontPlay -Primary
$btnPlay.Dock = 'Right'; $btnPlay.Height = (S 42)
$btnSave = New-Button 'Save settings' 140
$btnSave.Dock = 'Right'; $btnSave.Enabled = $false      # enabled by a change (Mark-Dirty)
$gap = New-Object System.Windows.Forms.Panel; $gap.Dock = 'Right'; $gap.Width = (S 10); $gap.BackColor = $cSide
$lblStatus = New-Label '' $null $cDim
$lblStatus.Dock = 'Fill'; $lblStatus.AutoSize = $false; $lblStatus.TextAlign = 'MiddleLeft'
$chkClose = New-Object System.Windows.Forms.CheckBox
$chkClose.Text = 'Close the launcher when the game starts'; $chkClose.AutoSize = $true; $chkClose.Dock = 'Left'; $chkClose.ForeColor = $cDim; $chkClose.Checked = $true
$progress = New-Object System.Windows.Forms.ProgressBar      # long operations (moving the installation)
$progress.Dock = 'Right'; $progress.Width = (S 200); $progress.Style = 'Marquee'; $progress.Visible = $false
$bar.Controls.Add($lblStatus); $bar.Controls.Add($chkClose); $bar.Controls.Add($progress); $bar.Controls.Add($btnSave); $bar.Controls.Add($gap); $bar.Controls.Add($btnPlay); $bar.Controls.Add($barLine)

$pages = [ordered]@{}
$navButtons = @{}
$pageOrder = @(
    @('Home', 'Home'), @('Display', 'Display'), @('Graphics', 'Graphics'), @('Audio', 'Audio && Language'), @('Competitive', 'Competitive'),
    @('Online', 'Online'), @('Controls', 'Controls'), @('Keyboard', 'Keyboard'), @('Saves', 'Saves'), @('Folders', 'Folders'), @('Guide', 'Setup guide'), @('Trouble', 'Troubleshooting'), @('About', 'About'))
$titles = @{ Home = 'Home'; Display = 'Display'; Graphics = 'Graphics'; Audio = 'Audio & Language'; Competitive = 'Competitive'; Online = 'Online (experimental)'
    Controls = 'Controllers'; Keyboard = 'Keyboard keys'; Saves = 'Saves'; Folders = 'Folders'; Guide = 'Setup guide'; Trouble = 'Troubleshooting'; About = 'About' }
# Segoe MDL2 Assets glyphs: Home, TVMonitor, Color, Volume, Flag, Globe, Game, KeyboardClassic, Save, Folder, Help, Repair, Info
$navGlyph = @{ Home = 0xE80F; Display = 0xE7F4; Graphics = 0xE790; Audio = 0xE767; Competitive = 0xE7C1; Online = 0xE774; Controls = 0xE7FC; Keyboard = 0xE765
    Saves = 0xE74E; Folders = 0xE8B7; Guide = 0xE897; Trouble = 0xE90F; About = 0xE946 }      # Guide: Help
$intros = @{
    Display = 'How the game window looks. Used at the next start of the game.'
    Graphics = 'Picture quality and shaders. Used at the next start of the game. On a weak or integrated GPU start with the profile low.'
    Audio = 'Sound volume and the language of the game. Used at the next start.'
    Competitive = 'For players who want to go straight into competitive play. Used at the next start.'
    Online = 'Play over the game''s own Local Wireless mode across a network. Both players need the same game version and the same mods. No encryption: play with people you know. Used at the next start.'
    Controls = 'Controllers through SDL (Xbox, PlayStation, Switch, GameCube adapter, ...). Saved to controllers.ini and used within a second, also while the game runs. Every button of every controller can be remapped in the game''s F1 menu (Controls).'
    Keyboard = 'Which PC key is which Switch button for the keyboard player. Saved to controllers.ini, used within a second.'
    Saves = 'Back up and restore your save, or bring in a save from your Switch or an emulator. Close the game first: restoring or importing replaces the save the game uses (your current one is backed up before).'
    Folders = 'Where the game keeps its files, and where your screenshots go. Choose another folder or drive here.'
}
$script:curPage = ''
$y = (S 84)
foreach ($po in $pageOrder) {
    $name = $po[0]
    if ($name -eq 'Guide') { $y += (S 16) }       # a little space before the help pages
    $item = New-Object System.Windows.Forms.Panel
    $item.Size = New-Object System.Drawing.Size(((S 224) - 1), (S 38)); $item.Location = New-Object System.Drawing.Point(0, $y); $item.BackColor = $cSide; $item.Tag = $name
    $item.Cursor = [System.Windows.Forms.Cursors]::Hand; $y += (S 38)
    $mark = New-Object System.Windows.Forms.Panel; $mark.Dock = 'Left'; $mark.Width = (S 3); $mark.BackColor = $cSide; $mark.Tag = $name
    $ic = New-Label ([string][char]$navGlyph[$name]) $fontGlyph $cDim; $ic.AutoSize = $false; $ic.Dock = 'Left'; $ic.Width = (S 44); $ic.TextAlign = 'MiddleCenter'; $ic.Tag = $name
    $tx = New-Label $po[1].Replace('&&', '&') $fontNav $cDim; $tx.AutoSize = $false; $tx.Dock = 'Fill'; $tx.TextAlign = 'MiddleLeft'; $tx.Tag = $name
    $item.Controls.Add($tx); $item.Controls.Add($ic); $item.Controls.Add($mark)
    foreach ($c in @($item, $mark, $ic, $tx)) {
        $c.Add_Click({ Show-Page $this.Tag })
        $c.Add_MouseEnter({ $it = $navButtons[$this.Tag].Item; if ($this.Tag -ne $script:curPage) { $it.BackColor = $cHover } })
        $c.Add_MouseLeave({ $it = $navButtons[$this.Tag].Item; if ($this.Tag -ne $script:curPage) { $it.BackColor = $cSide } })
    }
    $side.Controls.Add($item); $navButtons[$name] = @{ Item = $item; Mark = $mark; Icon = $ic; Text = $tx }

    $pg = New-Object System.Windows.Forms.Panel
    $pg.Dock = 'Fill'; $pg.AutoScroll = $true; $pg.BackColor = $cBg; $pg.Visible = $false
    $pg.Padding = New-Object System.Windows.Forms.Padding((S 32), (S 24), (S 32), (S 24))
    $flow = New-Object System.Windows.Forms.FlowLayoutPanel
    $flow.FlowDirection = 'TopDown'; $flow.WrapContents = $false; $flow.AutoSize = $true; $flow.Dock = 'Top'; $flow.BackColor = $cBg
    $pg.Controls.Add($flow); $host_.Controls.Add($pg)
    $pages[$name] = $flow
    $t = New-Label $titles[$name] $fontH1; $t.Margin = New-Object System.Windows.Forms.Padding(0, 0, 0, (S 4)); [void]$flow.Controls.Add($t)
    if ($intros[$name]) { $l = New-Label $intros[$name] $null $cDim; $l.MaximumSize = New-Object System.Drawing.Size((S 700), 0); $l.Margin = New-Object System.Windows.Forms.Padding(0, 0, 0, (S 18)); [void]$flow.Controls.Add($l) }
}
function Show-Page([string]$name) {
    $script:curPage = $name
    foreach ($k in $pages.Keys) {
        $on = $k -eq $name; $n = $navButtons[$k]
        $pages[$k].Parent.Visible = $on
        $n.Item.BackColor = $(if ($on) { $cSel } else { $cSide }); $n.Mark.BackColor = $(if ($on) { $cAccent } else { $n.Item.BackColor })
        $n.Icon.ForeColor = $(if ($on) { $cAccent } else { $cDim }); $n.Text.ForeColor = $(if ($on) { $cFg } else { $cDim })
    }
    $form.ActiveControl = $null                                   # no focused control drags the page down
    $pages[$name].Parent.AutoScrollPosition = New-Object System.Drawing.Point(0, 0)      # every page opens at its top
    if ($name -eq 'Home') { Update-Home }
    if ($name -eq 'Folders') { Update-Folders }
}

# --- the settings controls
$script:files = @{ G = (Read-IniLines $P.Graphics); C = (Read-IniLines $P.Controllers) }
$script:dirty = $false
$controls = New-Object System.Collections.ArrayList
function Mark-Dirty { if (-not $script:loading) { $script:dirty = $true; $btnSave.Enabled = $true; $form.Text = 'SSBU native - launcher *'; $lblStatus.ForeColor = $cWarn; $lblStatus.Text = 'Unsaved changes.' } }
$script:loading = $true
foreach ($s in $settings) {
    $flow = $pages[$s.Page]
    $row = New-Object System.Windows.Forms.TableLayoutPanel      # one card per setting: name and control, the explanation under them
    $row.ColumnCount = 2; $row.RowCount = 2; $row.AutoSize = $true; $row.BackColor = $cCard
    [void]$row.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle('Absolute', (S 300))))
    [void]$row.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle('Absolute', (S 324))))
    $row.Padding = New-Object System.Windows.Forms.Padding((S 18), (S 12), (S 18), $(if ($s.Help) { (S 14) } else { (S 12) }))
    $row.Margin = New-Object System.Windows.Forms.Padding(0, 0, 0, $(if ($s.Help) { (S 10) } else { (S 4) }))
    $lab = New-Label $s.Label $fontBold
    $lab.Anchor = 'Left'
    $cur = Get-IniValue $script:files[$s.F] $s.S $s.K $s.Def
    if ($s.Kind -eq 'text') {
        $ctl = New-Object System.Windows.Forms.TextBox
        $ctl.Text = $cur
        $ctl.Add_TextChanged({ Mark-Dirty })
    } else {
        $ctl = New-Object System.Windows.Forms.ComboBox
        $ctl.DropDownStyle = $(if ($s.Kind -eq 'edit') { 'DropDown' } else { 'DropDownList' })
        foreach ($v in $s.Values) { [void]$ctl.Items.Add($v) }
        if (-not ($s.Values -contains $cur)) { [void]$ctl.Items.Add($cur) }          # a value written by hand stays selectable
        if ($s.Kind -eq 'edit') { $ctl.Text = $cur } else { $ctl.SelectedItem = $cur }
        $ctl.Add_SelectedIndexChanged({ Mark-Dirty }); $ctl.Add_TextChanged({ Mark-Dirty })
    }
    $ctl.Width = (S 320); $ctl.BackColor = $cField; $ctl.ForeColor = $cFg; $ctl.Anchor = 'Left'
    if ($ctl -is [System.Windows.Forms.ComboBox]) { Set-ComboTheme $ctl } else { $ctl.BorderStyle = 'FixedSingle' }
    $row.Controls.Add($lab, 0, 0); $row.Controls.Add($ctl, 1, 0)
    if ($s.Help) {
        $h = New-Label $s.Help $null $cDim
        $h.Margin = New-Object System.Windows.Forms.Padding(0, (S 6), 0, 0)
        $h.MaximumSize = New-Object System.Drawing.Size((S 620), 0)
        $row.Controls.Add($h, 0, 1); $row.SetColumnSpan($h, 2)
    }
    [void]$flow.Controls.Add($row)
    [void]$controls.Add(@{ Def = $s; Ctl = $ctl; Loaded = $cur })
}
$script:loading = $false
$kh = New-Label $keyHelp $null $cDim; $kh.MaximumSize = New-Object System.Drawing.Size((S 580), 0); $kh.Margin = New-Object System.Windows.Forms.Padding(0, (S 10), 0, 0)
[void]$pages['Keyboard'].Controls.Add($kh)

function Get-ControlValue($c) { return ([string]$c.Ctl.Text).Trim() }
function Save-Settings {
    $bad = @()
    foreach ($c in $controls) {
        $v = Get-ControlValue $c
        if ($c.Def.Check -and $v -ne $c.Loaded -and $v -notmatch $c.Def.Check) { $bad += "$($c.Def.Label): '$v'" }
        if ($v -match '[\r\n;]') { $bad += "$($c.Def.Label): no ; or line breaks" }
    }
    if ($bad.Count) {
        [void][System.Windows.Forms.MessageBox]::Show("These values are not valid, nothing was saved:`n`n" + ($bad -join "`n"), 'SSBU launcher', 'OK', 'Warning'); return $false
    }
    $changed = @{ G = 0; C = 0 }
    $files = @{ G = (Read-IniLines $P.Graphics); C = (Read-IniLines $P.Controllers) }      # read again: the F1 menu may have written meanwhile
    foreach ($c in $controls) {
        $v = Get-ControlValue $c
        if ($v -eq $c.Loaded) { continue }
        $files[$c.Def.F] = Set-IniValue $files[$c.Def.F] $c.Def.S $c.Def.K $v
        $changed[$c.Def.F]++; $c.Loaded = $v
    }
    try {
        if ($changed.G) { Write-IniLines $P.Graphics $files.G }
        if ($changed.C) { Write-IniLines $P.Controllers $files.C }
    } catch {
        [void][System.Windows.Forms.MessageBox]::Show("Could not write the settings:`n" + $_.Exception.Message, 'SSBU launcher', 'OK', 'Error'); return $false
    }
    $script:files = $files; $script:dirty = $false; $btnSave.Enabled = $false; $form.Text = 'SSBU native - launcher'
    $n = $changed.G + $changed.C
    $lblStatus.ForeColor = $cOk
    $lblStatus.Text = $(if ($n) { "Saved $n setting(s). Game settings are used at the next start; controller settings at once." } else { 'Nothing changed.' })
    return $true
}
$btnSave.Add_Click({ [void](Save-Settings) })

# --- Home
$homePg = $pages['Home']
$homePg.Controls.RemoveAt(0)      # the banner says it: no "Home" title
$bannerFile = Join-Path $repo 'media\og-image.png'      # the project's own banner (README); a band of it
if (Test-Path -LiteralPath $bannerFile) {
    try {
        $src = [System.Drawing.Image]::FromFile($bannerFile)
        $bw = (S 680); $bh = [int]($bw * 0.36)
        $crop = New-Object System.Drawing.Bitmap($bw, $bh)
        $g = [System.Drawing.Graphics]::FromImage($crop); $g.InterpolationMode = 'HighQualityBicubic'
        $sh = [int]($src.Width * 0.36)
        $g.DrawImage($src, (New-Object System.Drawing.Rectangle(0, 0, $bw, $bh)), (New-Object System.Drawing.Rectangle(0, [int](($src.Height - $sh) * 0.35), $src.Width, $sh)), 'Pixel')
        $g.Dispose(); $src.Dispose()
        $banner = New-Object System.Windows.Forms.PictureBox
        $banner.Image = $crop; $banner.Size = $crop.Size; $banner.Margin = New-Object System.Windows.Forms.Padding(0, 0, 0, (S 14))
        [void]$homePg.Controls.Add($banner); $homePg.Controls.SetChildIndex($banner, 0)
    } catch {}
}
$homeSub = New-Label 'Super Smash Bros. Ultimate as a native PC program: your own game, built on your PC.' $null $cDim
$homeSub.Margin = New-Object System.Windows.Forms.Padding(0, 0, 0, (S 16)); [void]$homePg.Controls.Add($homeSub)
$statusCard = New-Card
[void]$statusCard.Controls.Add((New-Label 'Status' $fontH2))
$statusBox = New-Object System.Windows.Forms.TableLayoutPanel
$statusBox.ColumnCount = 2; $statusBox.AutoSize = $true; $statusBox.BackColor = $cCard; $statusBox.Margin = New-Object System.Windows.Forms.Padding(0, (S 6), 0, 0)
[void]$statusBox.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle('Absolute', (S 30))))
[void]$statusCard.Controls.Add($statusBox)
[void]$homePg.Controls.Add($statusCard)
$toolCard = New-Card
[void]$toolCard.Controls.Add((New-Label 'Quick actions' $fontH2))
$tools = New-Object System.Windows.Forms.FlowLayoutPanel
$tools.AutoSize = $true; $tools.WrapContents = $true; $tools.BackColor = $cCard; $tools.MaximumSize = New-Object System.Drawing.Size((S 640), 0); $tools.Margin = New-Object System.Windows.Forms.Padding(0, (S 8), 0, 0)
[void]$toolCard.Controls.Add($tools)
[void]$homePg.Controls.Add($toolCard)
function Get-ShotDir { $d = Get-IniValue (Read-IniLines $P.Graphics) '' 'screenshot_dir' ''; if ($d) { return $d } else { return $P.Screens } }
foreach ($t in @(@('Saves folder', { Open-Path $P.Saves }), @('Screenshots', { Open-Path (Get-ShotDir) }), @('Logs folder', { Open-Path $P.Logs }),
                 @('Change folders...', { Show-Page 'Folders' }), @('Windows display settings', { Start-Process 'ms-settings:display-advanced' }),
                 @('Install / repair / update', { Start-Process -FilePath $P.Install -WorkingDirectory $repo }))) {
    $b = New-Button $t[0] 200; $b.Add_Click($t[1]); [void]$tools.Controls.Add($b)
}
# a quick switch for the most asked-for setting; it moves Display > Window mode, and follows it
function Get-SettingControl([string]$key) { foreach ($c in $controls) { if ($c.Def.K -eq $key -and $c.Def.F -eq 'G') { return $c.Ctl } }; return $null }
$chkFull = New-Object System.Windows.Forms.CheckBox
$chkFull.Text = 'Play in full screen (Display > Window mode; F11 switches while you play)'; $chkFull.AutoSize = $true; $chkFull.ForeColor = $cFg; $chkFull.BackColor = $cCard
$chkFull.Margin = New-Object System.Windows.Forms.Padding(0, (S 8), 0, (S 2))
[void]$toolCard.Controls.Add($chkFull); $toolCard.Controls.SetChildIndex($chkFull, 1)
$winCtl = Get-SettingControl 'window'
$script:syncing = $true; $chkFull.Checked = [string]$winCtl.SelectedItem -ne 'windowed'; $script:syncing = $false
$chkFull.Add_CheckedChanged({ if (-not $script:syncing) { $script:syncing = $true; $winCtl.SelectedItem = $(if ($chkFull.Checked) { 'fullscreen' } else { 'windowed' }); $script:syncing = $false } })
$winCtl.Add_SelectedIndexChanged({ if (-not $script:syncing) { $script:syncing = $true; $chkFull.Checked = [string]$winCtl.SelectedItem -ne 'windowed'; $script:syncing = $false } })
$quick = New-Label 'New here? Read the Setup guide. Going competitive? See the Competitive page (unlock fighters and stages, low input latency).' $null $cDim
$quick.MaximumSize = New-Object System.Drawing.Size((S 680), 0); $quick.Margin = New-Object System.Windows.Forms.Padding(0, (S 4), 0, 0); [void]$homePg.Controls.Add($quick)

function Add-Status([string]$state, [string]$text) {      # Segoe MDL2: CheckMark, Warning, Cancel
    $mark = New-Label ([string][char]$(switch ($state) { 'ok' { 0xE73E } 'warn' { 0xE7BA } default { 0xE711 } })) $fontGlyph $(switch ($state) { 'ok' { $cOk } 'warn' { $cWarn } default { $cFail } })
    $mark.Margin = New-Object System.Windows.Forms.Padding(0, (S 4), 0, (S 4))
    $l = New-Label $text; $l.MaximumSize = New-Object System.Drawing.Size((S 600), 0); $l.Margin = New-Object System.Windows.Forms.Padding(0, (S 4), 0, (S 4))
    $r = $statusBox.RowCount; $statusBox.RowCount++
    $statusBox.Controls.Add($mark, 0, $r); $statusBox.Controls.Add($l, 1, $r)
}
function Update-Home {
    $statusBox.SuspendLayout(); $statusBox.Controls.Clear(); $statusBox.RowCount = 0
    $installed = (Test-Path -LiteralPath $P.Exe) -and (Test-Path -LiteralPath $P.GameDll)
    if ($installed) {
        $ver = ''
        try { $ver = (Get-Content -LiteralPath $P.State -Raw | ConvertFrom-Json).game_version } catch {}
        Add-Status 'ok' ("The game is installed" + $(if ($ver) { " (game version $ver)" } else { '' }) + " in $($P.Build).")
    } elseif (Test-Path -LiteralPath $P.Exe) {
        Add-Status 'fail' 'The game library (ssbu_game_eh_o1.dll) is missing: usually an antivirus removed it. See Troubleshooting, then Install / repair.'
    } else {
        Add-Status 'fail' 'The game is not installed yet. Read the Setup guide, then press Install / repair / update.'
    }
    $vk = Test-Path -LiteralPath (Join-Path $env:WINDIR 'System32\vulkan-1.dll')
    Add-Status $(if ($vk) { 'ok' } else { 'fail' }) $(if ($vk) { 'Vulkan driver found.' } else { 'No Vulkan driver found: install the newest driver of your GPU (Vulkan 1.3).' })
    $hz = 0; try { $hz = [SsbuLauncherNative]::RefreshRate() } catch {}
    if ($hz -gt 1) {      # the game shows 60 frames a second: evenly only on k x 60 Hz (display.c's lock: within 1.1 %), or with variable refresh
        $k = [Math]::Max(1, [Math]::Round($hz / 60.0)); $even = [Math]::Abs($hz - 60 * $k) -le 0.011 * 60 * $k
        $vs = [string](Get-SettingControl 'vsync').Text
        if ($even) { Add-Status 'ok' "Monitor at $hz Hz: every frame is on screen equally long." }
        elseif ($vs -eq 'vrr') { Add-Status 'ok' "Monitor at $hz Hz with Frame pacing = vrr: even frames if G-Sync / FreeSync is on in your GPU's control panel and the game runs full screen or borderless." }
        else { Add-Status 'warn' "Your monitor runs at $hz Hz: 60 frames a second cannot be shown evenly there (some stay on screen longer, a slight judder). Set it to 120 Hz if your monitor offers it, otherwise 60 Hz, in Windows display settings (button below), or, with a G-Sync / FreeSync monitor, choose Frame pacing = vrr on the Display page." }
    }
    if (Test-Path -LiteralPath (Join-Path $P.Saves 'save\save_data')) { Add-Status 'ok' 'Save data found.' } else { Add-Status 'warn' 'No save yet: the game makes one at the first start (Setup guide, step 4, to use your own).' }
    $dlc = 0
    if (Test-Path -LiteralPath $P.Dlc) { $dlc = @(Get-Content -LiteralPath $P.Dlc | Where-Object { $_.Trim() -and -not $_.Trim().StartsWith('#') }).Count }
    Add-Status $(if ($dlc) { 'ok' } else { 'warn' }) $(if ($dlc) { "$dlc downloadable contents listed." } else { 'No DLC listed (optional: put your DLC .nsp files next to the game and run the installer).' })
    $ua = Get-IniValue $script:files.G '' 'unlock_all' 'off'
    Add-Status $(if ($ua -eq 'on') { 'ok' } else { 'warn' }) $(if ($ua -eq 'on') { 'Every fighter and stage is unlocked (Competitive page).' } else { 'Fighters and stages unlock as in the game. Competitive page: Unlock fighters and stages.' })
    if (Get-Process -Name 'ssbu_native' -ErrorAction SilentlyContinue) { Add-Status 'warn' 'The game is running now. Game settings saved here are used at its next start.' }
    $statusBox.ResumeLayout()
    $btnPlay.Enabled = $installed
}

# --- Controls page: the controller test
$ct = New-Button 'Test controllers' 170
$ct.Add_Click({
    if (-not (Test-Path -LiteralPath $P.Exe)) { Open-Path $P.Exe; return }
    Start-Process -FilePath $P.Exe -ArgumentList '--pad-test' -WorkingDirectory $P.Work
})
$co = New-Button 'Open controllers.ini' 170
$co.Add_Click({ Open-Path $P.Controllers })
$ctr = New-Object System.Windows.Forms.FlowLayoutPanel; $ctr.AutoSize = $true; $ctr.Controls.Add($ct); $ctr.Controls.Add($co)
$ctr.Margin = New-Object System.Windows.Forms.Padding(0, (S 6), 0, 0)
[void]$pages['Controls'].Controls.Add($ctr)

# --- Folders page: the installation (moved as a whole), the screenshot folder (saved at once), the folders inside
$fold = $pages['Folders']
function Update-DirtyState {      # after one key was saved on its own: still unsaved changes elsewhere?
    $left = $false; foreach ($c in $controls) { if ((Get-ControlValue $c) -ne $c.Loaded) { $left = $true } }
    $script:dirty = $left; $btnSave.Enabled = $left; $form.Text = 'SSBU native - launcher' + $(if ($left) { ' *' } else { '' })
}
function Save-OneSetting([string]$key, [string]$value) {      # a folder choice is used at once: written alone, like the other keys (in place)
    foreach ($c in $controls) { if ($c.Def.K -eq $key -and $c.Def.F -eq 'G') { $hit = $c } }
    try { $lines = Set-IniValue (Read-IniLines $P.Graphics) '' $key $value; Write-IniLines $P.Graphics $lines; $script:files.G = $lines }
    catch { [void][System.Windows.Forms.MessageBox]::Show("Could not write the setting:`n" + $_.Exception.Message, 'SSBU launcher', 'OK', 'Error'); return }
    $script:loading = $true; $hit.Ctl.Text = $value; $script:loading = $false; $hit.Loaded = $value
    Update-DirtyState; Update-Folders
}
function Format-GB([double]$b) { return ('{0:0.0} GB' -f ($b / 1GB)) }
function Get-Free([string]$path) { try { return (New-Object System.IO.DriveInfo([IO.Path]::GetPathRoot($path))).AvailableFreeSpace } catch { return -1 } }

# the installation
$instCard = New-Card
[void]$instCard.Controls.Add((New-Label 'Game installation' $fontH2))
$instPath = New-Label '' $fontBold; $instPath.MaximumSize = New-Object System.Drawing.Size((S 620), 0); [void]$instCard.Controls.Add($instPath)
$instInfo = New-Label '' $null $cDim; $instInfo.MaximumSize = New-Object System.Drawing.Size((S 620), 0); [void]$instCard.Controls.Add($instInfo)
$instBtns = New-Object System.Windows.Forms.FlowLayoutPanel; $instBtns.AutoSize = $true; $instBtns.BackColor = $cCard; $instBtns.Margin = New-Object System.Windows.Forms.Padding(0, (S 8), 0, 0)
$bInstOpen = New-Button 'Open' 110; $bInstOpen.Add_Click({ Open-Path $P.Build })
$bInstMove = New-Button 'Move to another folder...' 210 -Primary; $bInstMove.Add_Click({ Move-Install })
$instBtns.Controls.Add($bInstOpen); $instBtns.Controls.Add($bInstMove); [void]$instCard.Controls.Add($instBtns)
[void]$fold.Controls.Add($instCard); $fold.Controls.SetChildIndex($instCard, 2)      # under the title and the intro

# the screenshots: the setting card (made by the settings loop) gets its buttons
$shotDirCtl = Get-SettingControl 'screenshot_dir'
$shotDirCtl.ReadOnly = $true; $shotDirCtl.BackColor = $cField
$shotBtns = New-Object System.Windows.Forms.FlowLayoutPanel; $shotBtns.AutoSize = $true; $shotBtns.BackColor = $cCard; $shotBtns.Margin = New-Object System.Windows.Forms.Padding(0, (S 8), 0, 0)
$bShotOpen = New-Button 'Open' 110; $bShotOpen.Add_Click({ Open-Path (Get-ShotDir) })
$bShotDir = New-Button 'Browse...' 120 -Primary
$bShotDir.Add_Click({
    $d = New-Object System.Windows.Forms.FolderBrowserDialog
    $d.Description = 'The folder for screenshots'; $d.ShowNewFolderButton = $true
    $cur = ([string]$shotDirCtl.Text).Trim(); $d.SelectedPath = $(if ($cur -and (Test-Path -LiteralPath $cur)) { $cur } else { $P.Screens })
    if ($d.ShowDialog($form) -eq 'OK') { Save-OneSetting 'screenshot_dir' $d.SelectedPath; $lblStatus.ForeColor = $cOk; $lblStatus.Text = "Screenshots now go to $($d.SelectedPath) (from the next game start)." }
})
$bShotDef = New-Button 'Use the default' 140
$bShotDef.Add_Click({ Save-OneSetting 'screenshot_dir' ''; $lblStatus.ForeColor = $cOk; $lblStatus.Text = 'Screenshots go to the installation''s screenshots folder again (from the next game start).' })
$shotBtns.Controls.Add($bShotOpen); $shotBtns.Controls.Add($bShotDir); $shotBtns.Controls.Add($bShotDef)
$shotCard = $shotDirCtl.Parent
$shotNow = New-Label '' $fontBold; $shotNow.MaximumSize = New-Object System.Drawing.Size((S 620), 0); $shotNow.Margin = New-Object System.Windows.Forms.Padding(0, (S 8), 0, 0)
$shotCard.Controls.Add($shotNow, 0, 2); $shotCard.SetColumnSpan($shotNow, 2)
$shotCard.Controls.Add($shotBtns, 0, 3); $shotCard.SetColumnSpan($shotBtns, 2)

# the folders inside the installation (they move with it)
$inCard = New-Card
[void]$inCard.Controls.Add((New-Label 'Inside the installation' $fontH2))
$inHelp = New-Label 'These stay inside the game installation and move with it.' $null $cDim; [void]$inCard.Controls.Add($inHelp)
$inGrid = New-Object System.Windows.Forms.TableLayoutPanel; $inGrid.ColumnCount = 3; $inGrid.AutoSize = $true; $inGrid.BackColor = $cCard; $inGrid.Margin = New-Object System.Windows.Forms.Padding(0, (S 8), 0, 0)
[void]$inGrid.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle('Absolute', (S 130)))); [void]$inGrid.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle('Absolute', (S 380))))
$script:inRows = @()
foreach ($f in @(@('Saves', 'Saves'), @('Settings', 'Config'), @('Logs', 'Logs'), @('Shader cache', 'Cache'))) {
    $r = $inGrid.RowCount; $inGrid.RowCount++
    $n = New-Label $f[0] $fontBold; $n.Anchor = 'Left'
    $pl = New-Label '' $null $cDim; $pl.Anchor = 'Left'; $pl.AutoEllipsis = $true; $pl.AutoSize = $false; $pl.Width = (S 370); $pl.Height = (S 20)
    $ob = New-Button 'Open' 90; $ob.Tag = $f[1]; $ob.Add_Click({ Open-Path $P[$this.Tag] })
    $inGrid.Controls.Add($n, 0, $r); $inGrid.Controls.Add($pl, 1, $r); $inGrid.Controls.Add($ob, 2, $r)
    $script:inRows += , @($f[1], $pl)
}
[void]$inCard.Controls.Add($inGrid)
[void]$fold.Controls.Add($inCard)
$P.Cache = Join-Path $P.Build 'cache'
function Get-Python {      # as Play SSBU.bat: this folder's private Python, else the one the installer recorded, else python
    $py = Join-Path $repo 'deps\python\python.exe'
    if (Test-Path -LiteralPath $py) { return $py }
    $pp = Join-Path $repo 'build\python_path.txt'
    if (Test-Path -LiteralPath $pp) { return (Get-Content -LiteralPath $pp -TotalCount 1).Trim() }
    return 'python'
}
function Restart-Launcher {
    if ($TestMove) { [Console]::WriteLine('testmove: done, location ' + (& (Get-Python) (Join-Path $repo 'scripts\build_location.py'))); $script:dirty = $false; $form.Close(); return }
    Start-Process powershell -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-STA', '-WindowStyle', 'Hidden', '-File', "`"$PSCommandPath`"", '-Page', 'Folders'); $script:dirty = $false; $form.Close() }
# Moving the installation (build_location.py: the folder every script and Play SSBU.bat use). The files that are written before
# a location exists stay in <repo>\build whatever the choice (build_location.py's docstring); run.py finds the moved game
# data (state.json's old dump path is replaced by <install>\dump\romfs). Same drive: the folders are renamed. Another drive:
# copied, the copy checked, the location switched, and only then the old copy deleted, so a failed copy changes nothing.
# the dialogs of the move: Windows' message box, or for -TestMove one that prints the text and answers yes
if ($TestMove) { Add-Type 'public static class SsbuTestMsg { public static string Show(string t, string ti, string b, string i) { System.Console.WriteLine("testmove: " + t.Replace((char)10, (char)32)); return b.StartsWith("Yes") ? "Yes" : "OK"; } }'; $MsgBox = [SsbuTestMsg] } else { $MsgBox = [System.Windows.Forms.MessageBox] }
$keepInRepoBuild = @('python_path.txt', 'downloads', 'update_manifest.json', 'update_backup')
function Get-MoveEntries {
    $isDefault = [IO.Path]::GetFullPath($P.Build).TrimEnd('\') -ieq [IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\')
    return @(Get-ChildItem -LiteralPath $P.Build -Force | Where-Object { -not ($isDefault -and $keepInRepoBuild -contains $_.Name) })
}
function Set-Busy([bool]$on, [string]$text) {
    $side.Enabled = -not $on; $host_.Enabled = -not $on; $btnPlay.Enabled = -not $on; $btnSave.Enabled = (-not $on) -and $script:dirty
    $progress.Visible = $on; $form.UseWaitCursor = $on
    if ($text) { $lblStatus.ForeColor = $(if ($on) { $cFg } else { $cOk }); $lblStatus.Text = $text }
    [System.Windows.Forms.Application]::DoEvents()
}
function Move-Install {
    if (Get-Process -Name 'ssbu_*' -ErrorAction SilentlyContinue) { [void]$MsgBox::Show('Close the game first: it keeps its files open.', 'SSBU launcher', 'OK', 'Warning'); return }
    $inst = @(Get-CimInstance Win32_Process -Filter "Name like 'python%'" -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -match 'install\.py|setup\.py' })
    if ($inst.Count) { [void]$MsgBox::Show('The installer is running: wait until it has finished, then move the installation.', 'SSBU launcher', 'OK', 'Warning'); return }
    if ($script:dirty -and $MsgBox::Show('Save your setting changes first? (The launcher restarts after the move.)', 'SSBU launcher', 'YesNo', 'Question') -eq 'Yes' -and -not (Save-Settings)) { return }
    if (-not (Test-Path -LiteralPath $P.Build)) { [void]$MsgBox::Show('There is no installation to move yet. Choose its folder in the installer (Install location).', 'SSBU launcher', 'OK', 'Information'); return }
    $d = New-Object System.Windows.Forms.FolderBrowserDialog
    $d.Description = 'Where should the game installation go? (an empty folder; in any other folder a new "SSBU native" folder is made)'; $d.ShowNewFolderButton = $true
    if ($TestMove) { $pick = $TestMove } elseif ($d.ShowDialog($form) -eq 'OK') { $pick = $d.SelectedPath } else { return }
    $dst = [IO.Path]::GetFullPath($pick).TrimEnd('\')
    if ((Test-Path -LiteralPath $dst) -and @(Get-ChildItem -LiteralPath $dst -Force).Count) { $dst = Join-Path $dst 'SSBU native' }
    $src = [IO.Path]::GetFullPath($P.Build).TrimEnd('\')
    if ((Test-Path -LiteralPath $dst) -and @(Get-ChildItem -LiteralPath $dst -Force).Count) { [void]$MsgBox::Show("$dst is not empty. Choose an empty folder.", 'SSBU launcher', 'OK', 'Warning'); return }
    if ($dst -ieq $src -or $dst.StartsWith($src + '\', 'OrdinalIgnoreCase') -or $src.StartsWith($dst + '\', 'OrdinalIgnoreCase')) {
        [void]$MsgBox::Show('Choose a folder outside the current installation (and not one that contains it).', 'SSBU launcher', 'OK', 'Warning'); return
    }
    $entries = Get-MoveEntries
    $same = [IO.Path]::GetPathRoot($dst) -ieq [IO.Path]::GetPathRoot($src)
    if ($same) {
        if ($MsgBox::Show("Move the game installation`n`nfrom  $src`nto      $dst`n`nOn the same drive this takes a moment.", 'SSBU launcher', 'OKCancel', 'Question') -ne 'OK') { return }
        Set-Busy $true 'Moving the installation...'
        $moved = @()
        try {
            New-Item -ItemType Directory -Force $dst | Out-Null
            foreach ($e in $entries) { Move-Item -LiteralPath $e.FullName -Destination (Join-Path $dst $e.Name) -ErrorAction Stop; $moved += $e.Name }
        } catch {
            $err = $_.Exception.Message
            foreach ($n in $moved) { try { Move-Item -LiteralPath (Join-Path $dst $n) -Destination (Join-Path $src $n) -ErrorAction Stop } catch {} }      # put back what was moved
            Set-Busy $false ''; [void]$MsgBox::Show("The move stopped, everything was put back:`n$err", 'SSBU launcher', 'OK', 'Error'); return
        }
        Complete-Move $dst $src @()
        return
    }
    Set-Busy $true 'Measuring the installation...'
    $size = 0.0; foreach ($e in $entries) { if ($e.PSIsContainer) { $size += [double](Get-ChildItem -LiteralPath $e.FullName -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum } else { $size += $e.Length } }
    Set-Busy $false ''
    $free = Get-Free $dst
    if ($free -ge 0 -and $free -lt $size + 1GB) { [void]$MsgBox::Show("Not enough space on $([IO.Path]::GetPathRoot($dst)): the installation needs $(Format-GB $size), $(Format-GB $free) are free.", 'SSBU launcher', 'OK', 'Warning'); return }
    if ($MsgBox::Show("Move the game installation ($(Format-GB $size))`n`nfrom  $src`nto      $dst`n`nIt is copied first and checked; the old copy is deleted only after that. This can take several minutes.", 'SSBU launcher', 'OKCancel', 'Question') -ne 'OK') { return }
    $log = Join-Path ([IO.Path]::GetTempPath()) 'ssbu_move_robocopy.log'
    $xf = @(); $xd = @(); foreach ($k in $keepInRepoBuild) { if (-not ($entries | Where-Object { $_.Name -eq $k })) { $p = Join-Path $src $k; if (Test-Path -LiteralPath $p -PathType Container) { $xd += "`"$p`"" } elseif (Test-Path -LiteralPath $p) { $xf += $k } } }
    $args_ = @("`"$src`"", "`"$dst`"", '/E', '/COPY:DAT', '/DCOPY:T', '/R:1', '/W:1', '/NP', '/NFL', '/NDL', "/LOG:`"$log`"") + $(if ($xf) { @('/XF') + $xf } else { @() }) + $(if ($xd) { @('/XD') + $xd } else { @() })
    Set-Busy $true 'Copying the installation...'
    $script:moveProc = Start-Process robocopy.exe -ArgumentList $args_ -WindowStyle Hidden -PassThru
    $script:moveInfo = @{ Dst = $dst; Src = $src; Size = $size; Entries = $entries; T0 = Get-Date }
    $script:moveTimer = New-Object System.Windows.Forms.Timer; $script:moveTimer.Interval = 3000
    $script:moveTimer.Add_Tick({
        $mi = $script:moveInfo
        if (-not $script:moveProc.HasExited) {
            $done = [double](Get-ChildItem -LiteralPath $mi.Dst -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
            $lblStatus.Text = "Copying the installation: $(Format-GB $done) of $(Format-GB $mi.Size) ($([int]((Get-Date) - $mi.T0).TotalSeconds) s)..."
            return
        }
        $script:moveTimer.Stop()
        $code = $script:moveProc.ExitCode
        $copied = [double](Get-ChildItem -LiteralPath $mi.Dst -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
        if ($code -ge 8 -or [Math]::Abs($copied - $mi.Size) -gt 1MB) {      # robocopy: 8 and above = some files were not copied
            Set-Busy $false ''
            [void]$MsgBox::Show("The copy did not complete (robocopy code $code, $(Format-GB $copied) of $(Format-GB $mi.Size)). Nothing was changed: the game still uses $($mi.Src). The partial copy in $($mi.Dst) can be deleted. Details: $log", 'SSBU launcher', 'OK', 'Error')
            return
        }
        Complete-Move $mi.Dst $mi.Src $mi.Entries
    })
    $script:moveTimer.Start()
}
function Complete-Move([string]$dst, [string]$src, $deleteOld) {      # switch the location (build_location.py), delete the old copy if any, restart
    $out = & (Get-Python) (Join-Path $repo 'scripts\build_location.py') --set $dst 2>&1
    if ($LASTEXITCODE) {
        Set-Busy $false ''
        [void]$MsgBox::Show("The files are in $dst, but the new location could not be recorded:`n$out`n`nRun: python scripts\build_location.py --set `"$dst`"", 'SSBU launcher', 'OK', 'Error'); return
    }
    foreach ($e in $deleteOld) { Set-Busy $true "Deleting the old copy: $($e.Name)..."; Remove-Item -LiteralPath $e.FullName -Recurse -Force -ErrorAction SilentlyContinue }
    $repoBuild = [IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\')
    if ($src -ine $repoBuild -and (Test-Path -LiteralPath $src) -and @(Get-ChildItem -LiteralPath $src -Force -ErrorAction SilentlyContinue).Count -eq 0) { Remove-Item -LiteralPath $src -Force -ErrorAction SilentlyContinue }      # an old folder left empty
    Set-Busy $false "The installation is now in $dst."
    [void]$MsgBox::Show("The game installation is now in`n$dst`n`nPlay, the installer and the shortcuts use it from now on. The launcher restarts.", 'SSBU launcher', 'OK', 'Information')
    Restart-Launcher
}
function Update-Folders {
    $instPath.Text = $P.Build
    $free = Get-Free $P.Build
    $instInfo.Text = 'The unpacked game, the build, your saves, the settings, the logs and the shader cache.' + $(if ($free -ge 0) { " Free space on drive $([IO.Path]::GetPathRoot($P.Build).TrimEnd('\', ':')): $(Format-GB $free)." } else { '' })
    $sd = ([string]$shotDirCtl.Text).Trim()
    $shotNow.Text = 'Screenshots are saved in: ' + $(if ($sd) { $sd } else { "$($P.Screens)  (the default)" })
    foreach ($r in $script:inRows) { $r[1].Text = $P[$r[0]] }
}

# --- Graphics page: what the GPU renders and what the window shows (gfx_options.h: docked 1920x1080, handheld 1280x720,
# times resolution_scale; the window: its size, or the monitor in fullscreen / borderless)
$resLabel = New-Label '' $fontBold $cAccent
$resLabel.MaximumSize = New-Object System.Drawing.Size((S 580), 0); $resLabel.Margin = New-Object System.Windows.Forms.Padding(0, 0, 0, (S 14))
$scaleCtl = Get-SettingControl 'resolution_scale'
[void]$pages['Graphics'].Controls.Add($resLabel)
$pages['Graphics'].Controls.SetChildIndex($resLabel, $pages['Graphics'].Controls.GetChildIndex($scaleCtl.Parent) + 1)
function Update-Resolution {
    $sc = 0.0; if (-not [double]::TryParse([string]$scaleCtl.Text, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$sc) -or $sc -le 0) { $resLabel.Text = ''; return }
    $sc = [Math]::Min(2.0, [Math]::Max(0.25, $sc))
    $hand = [string](Get-SettingControl 'console_mode').Text -eq 'handheld'
    $gw = [int]([Math]::Round($(if ($hand) { 1280 } else { 1920 }) * $sc)); $gh = [int]([Math]::Round($(if ($hand) { 720 } else { 1080 }) * $sc))
    if ([string]$winCtl.Text -eq 'windowed') { $ww = 0; $wh = 0; [void][int]::TryParse([string](Get-SettingControl 'width').Text, [ref]$ww); [void][int]::TryParse([string](Get-SettingControl 'height').Text, [ref]$wh) }
    else { $sb = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds; $ww = $sb.Width; $wh = $sb.Height }
    $show = [Math]::Min($ww, [int]($wh * 16 / 9)); $showH = [int]($show * 9 / 16)      # the picture keeps 16:9 inside the window (display_fit)
    $ratio = if ($show -gt 0) { $gw / $show } else { 0 }
    $what = if ($ratio -gt 1.05) { "rendered {0:0.0}x larger than shown: sharper (supersampled), costs GPU time" -f $ratio } elseif ($ratio -lt 0.95 -and $ratio -gt 0) { "rendered smaller than shown: scaled up, a little soft" } else { 'rendered at the size shown' }
    $resLabel.Text = "The GPU renders $gw x $gh; your window shows the picture at $show x $showH ($what)." + $(if ($hand -and [Math]::Abs($sc - 1.0) -gt 0.001) { ' Console mode is handheld: the scale may not apply there (it is made for docked mode).' } else { '' })
}
foreach ($k in @('resolution_scale', 'console_mode', 'window', 'width', 'height')) { $c = Get-SettingControl $k; $c.Add_TextChanged({ Update-Resolution }); if ($c -is [System.Windows.Forms.ComboBox]) { $c.Add_SelectedIndexChanged({ Update-Resolution }) } }

# A render-resolution picker over resolution_scale (scale = height / 1080 in docked mode; the code allows 0.25 .. 2.0 = 4K)
$mon = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$monScale = [Math]::Round([Math]::Min(2.0, [Math]::Max(0.25, $mon.Height / 1080.0)), 4)
$presets = [ordered]@{ '720p (1280 x 720)' = '0.6667'; '1080p (1920 x 1080, the game''s own)' = '1.0'; '1440p (2560 x 1440)' = '1.3333'; '4K (3840 x 2160)' = '2.0' }
$presets["Match my monitor ($($mon.Width) x $($mon.Height))"] = $monScale.ToString('0.0###', [Globalization.CultureInfo]::InvariantCulture)
$resRow = New-Object System.Windows.Forms.TableLayoutPanel
$resRow.ColumnCount = 2; $resRow.RowCount = 2; $resRow.AutoSize = $true; $resRow.BackColor = $cCard; $resRow.Padding = New-Object System.Windows.Forms.Padding((S 18), (S 12), (S 18), (S 14)); $resRow.Margin = New-Object System.Windows.Forms.Padding(0, 0, 0, (S 10))
[void]$resRow.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle('Absolute', (S 300)))); [void]$resRow.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle('Absolute', (S 324))))
$resPick = New-Object System.Windows.Forms.ComboBox
$resPick.DropDownStyle = 'DropDownList'; $resPick.Width = (S 320); Set-ComboTheme $resPick
foreach ($n in $presets.Keys) { [void]$resPick.Items.Add($n) }; [void]$resPick.Items.Add('Custom (the scale below)')
$resLab = New-Label 'Render resolution' $fontBold; $resLab.Anchor = 'Left'; $resPick.Anchor = 'Left'
$resRow.Controls.Add($resLab, 0, 0); $resRow.Controls.Add($resPick, 1, 0)
$resHelp = New-Label 'What the GPU draws the game at. Sets Resolution scale for you. Sharpest when it matches your monitor and the game runs full screen. 4K needs a strong GPU (four times the pixels of 1080p); the F2 overlay shows whether it holds 60 fps. Docked console mode.' $null $cDim
$resHelp.MaximumSize = New-Object System.Drawing.Size((S 580), 0); $resRow.Controls.Add($resHelp, 0, 1); $resRow.SetColumnSpan($resHelp, 2)
[void]$pages['Graphics'].Controls.Add($resRow)
$pages['Graphics'].Controls.SetChildIndex($resRow, $pages['Graphics'].Controls.GetChildIndex($scaleCtl.Parent))
function Sync-ResPick {      # the picker follows the scale: a preset when it is one, else Custom
    $script:resSync = $true; $hit = $null; $v = 0.0
    if ([double]::TryParse([string]$scaleCtl.Text, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$v)) {
        foreach ($n in $presets.Keys) { if ([Math]::Abs($v - [double]::Parse($presets[$n], [Globalization.CultureInfo]::InvariantCulture)) -lt 0.0005) { $hit = $n; break } }
    }
    $resPick.SelectedItem = $(if ($hit) { $hit } else { 'Custom (the scale below)' }); $script:resSync = $false
}
$resPick.Add_SelectedIndexChanged({
    if ($script:resSync) { return }
    $n = [string]$resPick.SelectedItem
    if (-not $presets.Contains($n)) { return }
    $scaleCtl.Text = $presets[$n]
    if ($n -like 'Match my monitor*' -and [string]$winCtl.SelectedItem -eq 'windowed' -and
        [System.Windows.Forms.MessageBox]::Show("The picture is sharpest full screen at your monitor's size. Switch Window mode to fullscreen too?", 'SSBU launcher', 'YesNo', 'Question') -eq 'Yes') { $winCtl.SelectedItem = 'fullscreen' }
})
$scaleCtl.Add_TextChanged({ if (-not $script:resSync) { Sync-ResPick } })
Sync-ResPick
Update-Resolution

# --- Saves page: backups of <install>\saves\save, restore one, import a save folder (a folder that holds save_data, or
# save_data itself, as BUILDING.md's FAQ describes), and the yuzu-family emulators' save folder of this game
function Test-GameRunning {
    if (Get-Process -Name 'ssbu_native' -ErrorAction SilentlyContinue) {
        [void][System.Windows.Forms.MessageBox]::Show('Close the game first: it keeps the save open and would write over it.', 'SSBU launcher', 'OK', 'Warning'); return $true
    }
    return $false
}
function Backup-Save([string]$why) {      # -> the backup folder, '' when there is no save to keep, $null on failure
    if (-not (Test-Path -LiteralPath (Join-Path $P.Save 'save_data'))) { return '' }
    $to = Join-Path $P.Backups ('save_' + (Get-Date -Format 'yyyy-MM-dd_HH-mm-ss') + $(if ($why) { "_$why" } else { '' }))
    try { New-Item -ItemType Directory -Force $P.Backups | Out-Null; Copy-Item -LiteralPath $P.Save -Destination $to -Recurse -ErrorAction Stop; return $to }
    catch { [void][System.Windows.Forms.MessageBox]::Show("The backup failed, nothing was changed:`n" + $_.Exception.Message, 'SSBU launcher', 'OK', 'Error'); return $null }
}
function Find-SaveData([string]$dir) {      # the save_data folder in or at $dir, or $null
    if (Test-Path -LiteralPath (Join-Path $dir 'save_data\system_data.bin')) { return (Join-Path $dir 'save_data') }
    if (Test-Path -LiteralPath (Join-Path $dir 'system_data.bin')) { return $dir }
    return $null
}
function Use-Save([string]$from, [string]$what) {      # replace the game's save_data with the one in $from, after a backup of the current one
    if (Test-GameRunning) { return }
    $sd = Find-SaveData $from
    if (-not $sd) { [void][System.Windows.Forms.MessageBox]::Show("No save of this game in:`n$from`n`nPick the folder that holds save_data (or save_data itself, with system_data.bin in it).", 'SSBU launcher', 'OK', 'Warning'); return }
    if ([System.Windows.Forms.MessageBox]::Show("Replace the game's save with $what?`n`n$sd`n`nYour current save is backed up first (Saves page).", 'SSBU launcher', 'YesNo', 'Question') -ne 'Yes') { return }
    $bk = Backup-Save 'before_restore'
    if ($null -eq $bk) { return }
    $dst = Join-Path $P.Save 'save_data'
    try {
        New-Item -ItemType Directory -Force $P.Save | Out-Null
        if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Recurse -Force -ErrorAction Stop }
        Copy-Item -LiteralPath $sd -Destination $dst -Recurse -ErrorAction Stop
        $lblStatus.ForeColor = $cOk; $lblStatus.Text = "Save replaced$(if ($bk) { '; the old one is in ' + (Split-Path -Leaf $bk) } else { '' })."
    } catch {
        [void][System.Windows.Forms.MessageBox]::Show("Copying failed: " + $_.Exception.Message + $(if ($bk) { "`n`nYour previous save is in $bk (Restore it on the Saves page)." } else { '' }), 'SSBU launcher', 'OK', 'Error')
    }
    Update-Saves
}
$savesPg = $pages['Saves']
$cardSave = New-Card; $cardBk = New-Card; $cardImp = New-Card      # your save, the backups, bringing one in
[void]$cardSave.Controls.Add((New-Label 'Your save' $fontH2))
$saveInfo = New-Label ''; $saveInfo.MaximumSize = New-Object System.Drawing.Size((S 620), 0); [void]$cardSave.Controls.Add($saveInfo)
$sv1 = New-Object System.Windows.Forms.FlowLayoutPanel; $sv1.AutoSize = $true; $sv1.BackColor = $cCard; $sv1.Margin = New-Object System.Windows.Forms.Padding(0, (S 8), 0, 0)
$bBackup = New-Button 'Back up now' 150; $bBackup.Add_Click({ if (Test-GameRunning) { return }; $b = Backup-Save ''; if ($b) { $lblStatus.ForeColor = $cOk; $lblStatus.Text = 'Backed up: ' + (Split-Path -Leaf $b) } elseif ($b -eq '') { $lblStatus.Text = 'There is no save yet.' }; Update-Saves })
$bOpenB = New-Button 'Open backups folder' 170; $bOpenB.Add_Click({ Open-Path $P.Backups })
$sv1.Controls.Add($bBackup); $sv1.Controls.Add($bOpenB); [void]$cardSave.Controls.Add($sv1); [void]$savesPg.Controls.Add($cardSave)
[void]$cardBk.Controls.Add((New-Label 'Backups' $fontH2))
$bkList = New-Object System.Windows.Forms.ListBox
$bkList.Width = (S 620); $bkList.Height = (S 120); $bkList.Margin = New-Object System.Windows.Forms.Padding(0, (S 8), 0, 0); $bkList.BackColor = $cField; $bkList.ForeColor = $cFg; $bkList.BorderStyle = 'FixedSingle'
[void]$cardBk.Controls.Add($bkList)
$bRestore = New-Button 'Restore the selected backup' 230; $bRestore.Margin = New-Object System.Windows.Forms.Padding(0, (S 10), 0, 0)
$bRestore.Add_Click({ if ($bkList.SelectedItem) { Use-Save (Join-Path $P.Backups ([string]$bkList.SelectedItem)) "the backup $($bkList.SelectedItem)" } })
[void]$cardBk.Controls.Add($bRestore); [void]$savesPg.Controls.Add($cardBk)
[void]$cardImp.Controls.Add((New-Label 'Bring in a save' $fontH2))
$impHelp = New-Label 'From your Switch: export it with JKSV or Checkpoint. From yuzu, Eden, suyu, sudachi: right-click the game, Open Save Data Location. From Ryujinx: right-click the game, Open User Save Directory. Then pick that folder here. Saves found in emulators on this PC are listed below.' $null $cDim
$impHelp.MaximumSize = New-Object System.Drawing.Size((S 620), 0); [void]$cardImp.Controls.Add($impHelp)
$bImport = New-Button 'Import a save folder...' 200; $bImport.Margin = New-Object System.Windows.Forms.Padding(0, (S 10), 0, (S 10))
$bImport.Add_Click({
    $d = New-Object System.Windows.Forms.FolderBrowserDialog
    $d.Description = 'The folder that holds save_data (or save_data itself)'
    if ($d.ShowDialog($form) -eq 'OK') { Use-Save $d.SelectedPath 'the save in that folder' }
})
[void]$cardImp.Controls.Add($bImport)
$emuList = New-Object System.Windows.Forms.ListBox
$emuList.Width = (S 620); $emuList.Height = (S 80); $emuList.BackColor = $cField; $emuList.ForeColor = $cFg; $emuList.BorderStyle = 'FixedSingle'; $emuList.HorizontalScrollbar = $true
[void]$cardImp.Controls.Add($emuList)
$bEmu = New-Button 'Import the selected one' 200; $bEmu.Margin = New-Object System.Windows.Forms.Padding(0, (S 10), 0, 0)
$bEmu.Add_Click({ if ($emuList.SelectedItem) { Use-Save ([string]$emuList.SelectedItem) 'the emulator save' } })
[void]$cardImp.Controls.Add($bEmu); [void]$savesPg.Controls.Add($cardImp)
function Update-Saves {
    $sd = Join-Path $P.Save 'save_data\system_data.bin'
    $saveInfo.Text = if (Test-Path -LiteralPath $sd) { "$($P.Save) (last written $((Get-Item -LiteralPath $sd).LastWriteTime.ToString('yyyy-MM-dd HH:mm')))." } else { "No save yet in $($P.Save): the game makes one at its first start." }
    $bkList.Items.Clear()
    if (Test-Path -LiteralPath $P.Backups) { Get-ChildItem -LiteralPath $P.Backups -Directory | Sort-Object Name -Descending | ForEach-Object { [void]$bkList.Items.Add($_.Name) } }
    $emuList.Items.Clear()
    foreach ($emu in @('yuzu', 'suyu', 'sudachi', 'eden', 'citron')) {      # <appdata>\<emu>\nand\user\save\0000000000000000\<user id>\01006A800016E000
        $root = Join-Path $env:APPDATA "$emu\nand\user\save\0000000000000000"
        if (-not (Test-Path -LiteralPath $root)) { continue }
        Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            $t = Join-Path $_.FullName '01006A800016E000'
            if (Find-SaveData $t) { [void]$emuList.Items.Add($t) }
        }
    }
    if (-not $emuList.Items.Count) { [void]$emuList.Items.Add('(no save of this game found in the yuzu-family emulators'' folders)') }
}
Update-Saves

# --- Troubleshooting page: a report for a bug (zip on the desktop) and a hitch log for the next game
$script:hitch = $false
$trPg = $pages['Trouble']
$repCard = New-Card; [void]$repCard.Controls.Add((New-Label 'Bug report' $fontH2))
$trRow = New-Object System.Windows.Forms.FlowLayoutPanel; $trRow.AutoSize = $true; $trRow.BackColor = $cCard; $trRow.Margin = New-Object System.Windows.Forms.Padding(0, (S 6), 0, (S 6))
$bReport = New-Button 'Export logs for a bug report' 230
$bReport.Add_Click({
    $stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm'
    $tmp = Join-Path ([IO.Path]::GetTempPath()) "ssbu_report_$stamp"; $zip = Join-Path ([Environment]::GetFolderPath('Desktop')) "ssbu_report_$stamp.zip"
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force $tmp | Out-Null
    $files = @()
    if (Test-Path -LiteralPath $P.Logs) { $files += @(Get-ChildItem -LiteralPath $P.Logs -Filter 'game_*.log' | Sort-Object LastWriteTime -Descending | Select-Object -First 3) + @(Get-ChildItem -LiteralPath $P.Logs -Filter 'installer_window.log' -ErrorAction SilentlyContinue) }
    if (Test-Path -LiteralPath $P.Work) { $files += @(Get-ChildItem -LiteralPath $P.Work -File | Where-Object { $_.Name -like '*_freeze.txt' -or $_.Name -like '*_crash.txt' } | Sort-Object LastWriteTime -Descending | Select-Object -First 4) }
    foreach ($n in @('graphics.ini', 'controllers.ini')) { $f = Join-Path $P.Config $n; if (Test-Path -LiteralPath $f) { $files += Get-Item -LiteralPath $f } }
    $user = [Regex]::Escape($env:USERNAME)
    foreach ($f in $files) {      # your Windows user name is replaced in every file (it appears in paths)
        $txt = [IO.File]::ReadAllText($f.FullName)
        [IO.File]::WriteAllText((Join-Path $tmp $f.Name), [Regex]::Replace($txt, $user, '<user>', 'IgnoreCase'), (New-Object Text.UTF8Encoding($false)))
    }
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue; $cpu = Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue | Select-Object -First 1
    $info = @("Windows: $($os.Caption) $($os.Version)", "CPU: $($cpu.Name)", "RAM: $([Math]::Round($os.TotalVisibleMemorySize / 1MB, 1)) GB", "Monitor: $([SsbuLauncherNative]::RefreshRate()) Hz")
    $info += @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | ForEach-Object { "GPU: $($_.Name), driver $($_.DriverVersion)" })
    [IO.File]::WriteAllLines((Join-Path $tmp 'system.txt'), [string[]]$info)
    try { Compress-Archive -Path (Join-Path $tmp '*') -DestinationPath $zip -Force -ErrorAction Stop; $lblStatus.ForeColor = $cOk; $lblStatus.Text = "Report saved: $zip"; Start-Process explorer.exe "/select,`"$zip`"" }
    catch { [void][System.Windows.Forms.MessageBox]::Show('The report could not be made: ' + $_.Exception.Message, 'SSBU launcher', 'OK', 'Error') }
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
})
$trRow.Controls.Add($bReport); [void]$repCard.Controls.Add($trRow)
$repHelp = New-Label 'Makes ssbu_report_<date>.zip on your desktop: the last game logs, freeze and crash reports, your settings and a short system summary (Windows, CPU, RAM, GPU and driver, monitor). Your Windows user name is replaced by <user>. Attach it to a report on the project''s Discord or GitHub.' $null $cDim
$repHelp.MaximumSize = New-Object System.Drawing.Size((S 620), 0); [void]$repCard.Controls.Add($repHelp)
$chkHitch = New-Object System.Windows.Forms.CheckBox
$chkHitch.Text = 'Hitch log: record late frames (over 20 ms) in the next game started with PLAY'; $chkHitch.AutoSize = $true; $chkHitch.ForeColor = $cFg
$chkHitch.Margin = New-Object System.Windows.Forms.Padding(0, (S 8), 0, (S 6))
$chkHitch.Add_CheckedChanged({ $script:hitch = $chkHitch.Checked })
$chkHitch.BackColor = $cCard; $chkHitch.MaximumSize = New-Object System.Drawing.Size((S 620), 0); [void]$repCard.Controls.Add($chkHitch)
[void]$trPg.Controls.Add($repCard); $trPg.Controls.SetChildIndex($repCard, 1)      # under the page title

# --- the help pages: '# ' = a heading, '- ' = a point
function Add-Text($flow, [string]$text) {      # each '# ' heading starts a card of its own
    $card = $null
    foreach ($line in ($text -split "`r?`n")) {
        if (-not $line.Trim()) { continue }
        if ($line.StartsWith('# ')) {
            $card = New-Card; [void]$flow.Controls.Add($card)
            $l = New-Label $line.Substring(2) $fontH2; $l.Margin = New-Object System.Windows.Forms.Padding(0, 0, 0, (S 6))
        }
        elseif ($line.StartsWith('- ')) { $l = New-Label ([string][char]0x2022 + '  ' + $line.Substring(2)); $l.Margin = New-Object System.Windows.Forms.Padding((S 6), (S 2), 0, (S 2)) }
        else { $l = New-Label $line; $l.Margin = New-Object System.Windows.Forms.Padding(0, (S 2), 0, (S 2)) }
        $l.MaximumSize = New-Object System.Drawing.Size((S 620), 0)
        if ($card) { [void]$card.Controls.Add($l) } else { [void]$flow.Controls.Add($l) }
    }
}
Add-Text $pages['Guide'] $guide
Add-Text $pages['Trouble'] $trouble
Add-Text $pages['About'] $about
$links = New-Object System.Windows.Forms.FlowLayoutPanel; $links.AutoSize = $true; $links.Margin = New-Object System.Windows.Forms.Padding(0, (S 14), 0, 0)
foreach ($t in @(@('Website', 'https://denkishi.github.io/opensmash-site/'), @('Discord', 'https://discord.gg/RyZpx3u2t4'), @('README', (Join-Path $repo 'README.md')), @('Licence', (Join-Path $repo 'LICENSE')))) {
    $b = New-Button $t[0] 120; $b.Tag = $t[1]; $b.Add_Click({ Start-Process -FilePath $this.Tag }); [void]$links.Controls.Add($b)
}
[void]$pages['About'].Controls.Add($links)

# --- Play and closing
$btnPlay.Add_Click({
    if ($script:dirty -and -not (Save-Settings)) { return }
    if (Get-Process -Name 'ssbu_native' -ErrorAction SilentlyContinue) {
        if ([System.Windows.Forms.MessageBox]::Show('The game is already running. Start another copy?', 'SSBU launcher', 'YesNo', 'Question') -ne 'Yes') { return }
    }
    if ($script:hitch) {      # as Play SSBU.bat, plus the hitch log (run.py passes only --env variables on to the game)
        $py = Join-Path $repo 'deps\python\python.exe'
        if (-not (Test-Path -LiteralPath $py)) { $pp = Join-Path $repo 'build\python_path.txt'; $py = if (Test-Path -LiteralPath $pp) { (Get-Content -LiteralPath $pp -TotalCount 1).Trim() } else { 'python' } }
        Start-Process -FilePath $py -ArgumentList @('scripts\run.py', '--play', '--seconds', '0', '--env', 'SSBU_HITCH=20') -WorkingDirectory $repo
    } else { Start-Process -FilePath $P.Play -WorkingDirectory $repo }
    $lblStatus.ForeColor = $cOk; $lblStatus.Text = 'Starting the game...'
    if ($chkClose.Checked) { $form.Close() }
})
$form.Add_FormClosing({
    if ($script:dirty -and -not $Shot) {
        $r = [System.Windows.Forms.MessageBox]::Show('Save your changes before closing?', 'SSBU launcher', 'YesNoCancel', 'Question')
        if ($r -eq 'Cancel' -or ($r -eq 'Yes' -and -not (Save-Settings))) { $_.Cancel = $true }
    }
})

if ($TestMove) { $form.Add_Shown({ Show-Page 'Folders'; Move-Install; $tt = New-Object System.Windows.Forms.Timer; $tt.Interval = 600000; $tt.Add_Tick({ [Console]::WriteLine('testmove: timeout'); $form.Close() }); $tt.Start() }) }
if (-not $pages.Contains($Page)) { $Page = 'Home' }
Show-Page $Page
if ($Shot) {      # test only: a picture of every page, then close
    $shotT = New-Object System.Windows.Forms.Timer; $shotT.Interval = 1500
    $shotT.Add_Tick({
        $shotT.Stop()
        if (-not (Test-Path -LiteralPath $Shot)) { New-Item -ItemType Directory -Force -Path $Shot | Out-Null }
        foreach ($k in $pages.Keys) {
            Show-Page $k; $form.Refresh()
            $bm = New-Object System.Drawing.Bitmap($form.Width, $form.Height)
            $form.DrawToBitmap($bm, (New-Object System.Drawing.Rectangle(0, 0, $form.Width, $form.Height)))
            $bm.Save((Join-Path $Shot "$k.png"), [System.Drawing.Imaging.ImageFormat]::Png); $bm.Dispose()
        }
        $form.Close()
    })
    $form.Add_Shown({ $shotT.Start() })
}
[System.Windows.Forms.Application]::Run($form)
