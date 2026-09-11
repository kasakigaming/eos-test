# EOS Proxy

A stupid simple Epic Online Services "proxy" that enables multiplayer functionality in some Steam games that use EOS.

# How to use

## The easy way: `eos-proxy-setup.exe`

The installer is a single exe with the proxy DLL baked into it. Run it, pick the games from the
list, done - one pick installs the proxy and a Steam auth backend, which is everything a game
needs.

```
 [1] The Isle                          proxy: not installed
                                       auth:  none
     D:\Games\TheIsle\TheIsle\Binaries\Win64
 [2] SpiderHeck                        proxy: installed (up to date)
                                       auth:  gbe_fork, app 1329500
     C:\Program Files (x86)\Steam\steamapps\common\SpiderHeck

 set up  : 1   or  1,3,5   or  2-4   or  a  (all)   proxy + auth, all of it
 undo    : u1  or  ua                              back to stock
 parts   : p1 proxy only     g1 gbe_fork   o1 uc-online2
           s1 SLSsteam config          ug1 auth backend only, out
 m add a folder to search    c clear saved folders
 r rescan                    q quit
```

- Several games at once: `1,3,5`, a range `2-4`, or `a` for everything found. That applies
  to the part commands too, so `ga` puts gbe_fork in every game found
- Steam libraries on every drive are found through the registry key and `libraryfolders.vdf`
- `m` adds any folder outside Steam. It can be a single game (`D:\Games\TheIsle`) or a
  parent holding several (`D:\Games`), and it is searched 5 levels deep, so a nested
  `TheIsle\Binaries\Win64` is found on its own. Added folders are remembered in
  `eos-proxy-setup.txt` next to the exe and searched again on every run
- It tells the original SDK and the proxy apart, so running it twice never clobbers the backup
- `u<number>` restores a game to stock
- It asks for administrator rights once, for all the folders that refused the write
- Dragging game folders onto the exe installs into all of them

Command line:

```
setup.exe <folder> [<folder> ...]     set up those folders completely
setup.exe --install <folder>...       same, no confirmation
setup.exe --all                       set up every game found
setup.exe --uninstall <folder>...     put everything back to stock
setup.exe --status <folder>...        report what is installed
setup.exe --extract <file>            just write the proxy DLL somewhere

--gbe / --uc / --sls                  pick the backend, or install only that one
--proxy-only                          the proxy DLL and nothing else
--remove-auth                         take the backend out, leave the proxy
--appid <number>                      which game it is, when it cannot be worked out
```

So `setup.exe --all --gbe --yes` sets up every EOS game on the machine with gbe_fork under it,
unattended.

## The Steam auth backend

The proxy is only half the job. Most games **require** `ISteamUser::GetAuthTicketForWebApi` to
return a non-error response before they even call the EOS networking functions, so one of these
has to sit underneath it:

| | what it is | the installer |
|---|---|---|
| [gbe_fork](https://github.com/Detanup01/gbe_fork/) | runs the game independent of Steam | `g<n>` / `--gbe`, downloaded and configured |
| [uc-online2](https://github.com/UnionCrax-Team/uc-online2) | very similar to Online-Fix | `o<n>` / `--uc`, downloaded |
| [SLSsteam](https://github.com/AceSLS/SLSsteam/) | Linux only, shims the real Steam client | `s<n>` / `--sls`, writes the config |

Whichever you pick, all multiplayer is still handled through Epic. Aside from Steam invites being
incompatible between gbe_fork and the other two, there should be no compatibility issues.

A plain `1` in the menu, or `setup.exe <folder>`, does the proxy and a backend in one go, because
a game needs both. `p1` / `--proxy-only` if you only want the proxy.

What the installer does for you:

- Downloads the emulator from the project's own GitHub release the first time it is needed, and
  caches it in `%LOCALAPPDATA%\eos-proxy` so later installs are offline
- Works out where the backend actually belongs. The folder you pick is the one holding the EOS
  SDK, and in a UE game that is a plugin subfolder - `TheIsle\Binaries\Win64\RedpointEOS`. The
  game loads Steamworks from somewhere else entirely (`Engine\Binaries\ThirdParty\Steamworks\
  Steamv157\Win64` for that same game), so the installer walks up to the game root, finds the
  game's own `steam_api64.dll`, and puts the emulator there. An emulator dropped next to the
  EOS SDK is never loaded, and looks exactly like the backend not working
- Works out the game's Steam AppID from `steam_appid.txt` or the library's `appmanifest_*.acf`,
  and fills it into the backend's config. `--appid <number>` when it cannot (a game outside a
  Steam library, for instance)
- Fills in your Steam account name and SteamID64 from the registry, so gbe_fork reports the name
  other players already know you by
- Parks any file it overwrites as `<name>.eosbak` and records everything it wrote in
  `eos-proxy-auth.txt` in the game root, every path relative to it, so `ug<n>` / `--remove-auth`
  puts the game back exactly as it was
- Only one backend at a time: installing a second one removes the first properly first
- Stands in for the launcher of a game that ships Easy Anti-Cheat. EAC will not map an unsigned
  EOS SDK into the process it protects: the game logs `Failed to load ... (GetLastError=193)`
  and never reaches EOS at all, whatever backend is underneath - and the backend's own
  `steam_api64.dll` is blocked the same way. So the game's `<Game>.exe`, which is the anti-cheat
  bootstrapper, is parked as `<Game>.exe.eosbak` and replaced with the small launcher built from
  `installer\launcher.c`. It reads `EasyAntiCheat\Settings.json` for the real game exe, starts
  it with the arguments it was given, and waits for it - so Steam still starts the app it knows
  about, still counts the game as running, and still hands out auth tickets. Launch from Steam
  exactly as before.

  Two things come with that. A server that enforces anti-cheat will refuse a client started this
  way, and modifying an anti-cheat protected game carries a ban risk on that game. And Steam
  puts its own files back whenever it verifies the install - after an interrupted launch, or on
  "verify integrity of game files" - which silently undoes all of this; run the installer again
  when that happens
- SLSsteam is Linux only, so there is nothing to copy into a game folder from Windows. What it
  can do is generate `SLSsteam-config.yaml` with `FakeAppIds` already pointed at the game, ready
  to be copied to `~/.config/SLSsteam/config.yaml` on the Linux side

## Antivirus

Steam emulators are flagged as riskware by nearly every scanner. It is a false positive, and it
is also the reason the emulators are **not** baked into `eos-proxy-setup.exe`: an installer
carrying those bytes gets quarantined before you ever run it.

Downloading them at install time keeps the installer itself clean, but the download can still be
blocked. Windows Defender does not always delete the file - it often leaves it on disk at full
size and refuses to let anything open it, which every extractor then reports as a corrupt
archive. The installer detects that case and prints the exact commands to run. They are:

```powershell
Add-MpPreference -ExclusionPath "$env:LOCALAPPDATA\eos-proxy"
Add-MpPreference -ExclusionPath "D:\Games\TheIsle\TheIsle\Binaries\Win64"
```

Run those in an administrator PowerShell, then run the installer again. `Remove-MpPreference`
with the same arguments undoes it. The installer will never touch your antivirus settings on its
own.

## The manual way

- Rename the existing `EOSSDK-Win64-Shipping.dll` in the game folder to `EOSSDK-Win64-Shipping.yes`
- Download the proxy dll from [releases](https://github.com/yesyes0649/eos-proxy/releases) and put it in the same location
- Set up one of the three backends above by hand. For SLSsteam that means
  [FakeAppIds](https://github.com/AceSLS/SLSsteam/wiki/Configuration#fakeappids-map-of-positive-number--positive-number)
  configured for the game
- In case of issues, check the `epic_proxy.log` file next to game executable. Feel free to open an issue in GitHub.

# Confirmed Games

I've only tested this with a few games.

- Sledding Game
- SpiderHeck (this one has an incompatibility with gbe_fork. Use the other two options.)
- Absolum (needs some additional changes. Check the `absolum` branch.)

# How it works

- All games that use EOS first call a `EOS_Connect_Login` function to initialize networking features. Since Epic supports a wide variety of platforms, they have many different authentication choices.
- This proxy just intercepts that function call, and swaps the authentication type from `EOS_ECT_STEAM_SESSION_TICKET` (which requires a valid steam auth ticket) to `EOS_ECT_DEVICEID_ACCESS_TOKEN` (which requires nothing. it's a game-specific and device-specific token.). There is a bit more code to prepare the device id access token, but that's all.

## The Isle: "Official Network Status: Offline"

EOS gets the game online, but The Isle also talks to its own matchmaker at
`api.warphosting.com.au`, and that is a separate wall. Most calls there carry a Steam ticket which
the matchmaker hands to Valve for validation, and no emulator can produce a ticket Valve will sign
off on:

| endpoint | ticket | what it is |
|---|---|---|
| `POST /v1/servers/status` | required, `401 Steam ticket invalid` | the official network state, `ONLINE` / `OFFLINE` / `UPDATING` / `MAINTENANCE`. The 401 is what paints the menu line red |
| `POST /v1/servers/active` | not required for the unverified set | the server list, and the only one there is. Which tab of the browser you are on is the `server_type` field in the body: `Official` needs a ticket and gets the same `401`, `Unverified` needs none and answers with the whole community list, 960 servers of it. That is why the browser fills while the status line says the network is down |
| `POST /v1/servers/queue/join` | not required | answers `200` with the address and queue port of the server you picked |
| `POST /v1/match/request` | required, `401 Steam ticket invalid` | turns the server you picked into an EOS session id to look up and join. This is where a join stops |

`src\warp.h` handles the whole matchmaker, not the status call alone. Every one of those URLs is a
plain ASCII literal in the game executable, so the proxy rewrites them in memory to a loopback
address it listens on, relays each request to the real matchmaker with its method, path, headers
and body intact, and passes the answer straight back. By default nothing about the game's
behaviour changes. What it buys is a line in `epic_proxy.log` for every call the client makes and
every answer it is given, which is the only way to see where a join actually dies.

Two calls are answered locally instead, and only when the matchmaker refuses the ticket:

- `/v1/servers/status` is reported as `ONLINE`, so the menu stops printing the official network as
  offline. A genuine ticket, and any genuine `MAINTENANCE` or `UPDATING` state, still wins.
- `/v1/match/request` is answered with the server id the client asked about, reused as the session
  id. That rested on a guess: that the matchmaker's server id and the EOS session id are the same
  string, both being 32 hex characters the server registers from the same place. **The guess held.**
  A real run searched for the id the proxy handed over and came back with one result:

  ```
  Warp: match refused, answering with session_id=69902ddde...413409
  EOS_SessionSearch_SetSessionId | SessionId: 69902ddde...413409
  EOS_SessionSearch_GetSearchResultCount | 1 result(s)
  ```

  So the client does find the session. What stops a join now happens after that, which is why the
  rest of the path is hooked and logged in `src\main.c`: copying the result out, reading the
  `HostAddress` it carries, and the `ResultCode` that `EOS_Sessions_JoinSession` answers with.

Worth being blunt about the limit: **nothing here produces a ticket Valve would sign**. The status
line reads Online and the browser fills, but a session id that does not exist cannot be invented,
and an official server is free to refuse a client the matchmaker never vouched for. Community
servers were never affected either way.

`EOS_PROXY_NO_WARP=1` in the environment leaves the game's own requests alone.

## Anti-cheat

Once a join resolves to a real address, the net driver is the next thing to stop it, and for a
long time it stopped in a way that said nothing at all:

```
UEngine::Browse Started Browse: "66.94.117.53:10100/Game/TheIsle/Maps/TitleMap?765611993..."
BroadcastTravelFailure ... PendingNetGameCreateFailure, reason: "Error initializing network layer."
```

Thirty milliseconds apart, with no anti-cheat line anywhere and no `BeginSession` call in the
proxy's own log either. One hooked call explained all of it:

```
EOS_Platform_GetAntiCheatClientInterface | NULL
```

The SDK only builds its anti-cheat client half when the anti-cheat client is actually running, and
here it cannot be: the game's own launcher **is** the EAC bootstrapper, and the installer has to
replace it because EAC will not map an unsigned EOS SDK into the process it protects. So the net
driver asked for the interface, got nothing, and gave up before it ever built itself. Answering
`BeginSession` had changed nothing, because with no interface nothing was calling it.

So the proxy stands in for the interface. When the real SDK has none, the getter hands back a tag
of its own, and all 21 `EOS_AntiCheatClient_*` calls check for that tag and answer locally rather
than handing a real SDK a handle it has never seen. Sessions begin and end, status polls report
nothing to report, peers register, message protection copies its input through untouched, and the
notification registrations return ids that nothing will ever fire. A handle that did come from the
real SDK still goes straight to it, so a machine where EAC does run is unaffected.

With that in place the join goes all the way onto the wire. No travel failure, a live
`RedpointEOSNetDriver`, and `LogNetVersion` in the game's own log.

### Where it still stops

The server half is the wall, and it is a real one. Every server a join was ever tried against
here was an unverified one - each `serverId` was looked up in the `Unverified` list afterwards and
all of them were in it. So this is not the official set being stricter; it is what a community
server does too:

```
EOS_Sessions_JoinSession | ResultCode 0
EOS_AntiCheatClient_BeginSession | stand-in, reporting success
EOS_AntiCheatClient_ReceiveMessageFromServer | stand-in, message dropped
EOS_AntiCheatClient_ReceiveMessageFromServer | stand-in, message dropped
EOS_AntiCheatClient_ReceiveMessageFromServer | stand-in, message dropped
```

The server sends an anti-cheat challenge every three seconds, 24 bytes of it. Within one join it
never varies. Between servers it does:

```
02 00 0b 00 10 5d 00 00 00 00 00 00 00 00 00 00 00 00 00 00 81 64 0c fb
02 00 0b 00 f9 79 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0c 9c 1d 3b
02 00 0b 00 83 17 00 00 00 00 00 00 00 00 00 00 00 00 00 00 f2 42 e2 6a
02 00 0b 00 7c 8d ff 3e 00 00 00 00 00 00 00 00 00 00 00 00 64 4e 3d 47
02 00 0b 00 5a 63 00 00 00 00 00 00 00 00 00 00 00 00 00 00 9f 1f 5b 56
```

Five servers, five of them. Four bytes of header, then a four byte value, then twelve zeros, then
four bytes that look like a checksum over the rest. The fourth sample is the one that fixes the
reading: the first three had a top half of zero, which made the value look two bytes wide, and
`7c 8d ff 3e` says it is not. So there is a per-session value in there, and a canned reply could
not have worked even in principle.

A real client answers through the callback registered with `AddNotifyMessageToServer`, and that
answer is an attestation the EAC client module produces and Epic's own service verifies. A
stand-in has no key material and cannot invent one.

That was tested rather than assumed. A build sent the server's own message straight back to it,
and nothing changed: the server re-sent the identical bytes eight times over 22 seconds, then
dropped the connection on the same 73 second timeout as silence. A wrong answer is worth exactly
what no answer is worth, so that code is not kept — only the logging that proved it.

Worth being precise about the shape of the failure, because it is easy to read it as something
else. The server never refuses. A refusal in Unreal is an `NMT_Failure` carrying a reason string,
and the game puts that string on screen. Nothing like that arrives. What happens instead is that
the connection is made, the challenge is sent, and the login is simply never approved:

```
LogGlobalStatus: UEngine::Browse Started Browse: "78.46.76.73/Game/TheIsle/Maps/TitleMap?765611993..."
LogNetVersion: TheIsle 0.21.784, ... (Checksum: 1531937975)
   ... 73 seconds, nothing ...
LogExit: Name:PendingNetDriver Def:GameNetDriver RedpointEOSNetDriver_2147438649 shut down
LogGlobalStatus: UEngine::Browse Started Browse: "/Game/TheIsle/Maps/TitleMap?closed"
```

Silence and a timeout, not an error. That rules out the Steam ticket as the cause of *this* step -
a matchmaker that refuses a ticket says so in the body of an HTTP answer, and a game server that
refused a login would say so in a `Failure` packet. Neither happens. The approval is held open
waiting for an attestation that never comes.

Reading these logs takes one caveat, and it is a gap rather than a detail. `LogNet` never prints a
line - not at any verbosity, in any session logged here. The only engine net category that appears
at all is `LogNetVersion`. What this build does instead is log the same events under a category of
its own:

```
LogGlobalStatus: UEngine::Browse Started Browse: "78.46.76.73/Game/TheIsle/..."
LogGlobalStatus: Warning: UEngine::BroadcastTravelFailure Travel failed, type: ETravelFailure::PendingNetGameCreateFailure
```

Both of those are `LogNet` call sites in stock Unreal, so the net layer is not silent, it is
reporting somewhere else. `LogHandshake` and `LogNetDriver` have no such substitute and say nothing
at all.

Turning those categories on took four tests to get right, and three of the four were dead ends
worth writing down, because each one looks like the answer until it is measured.

The command line is not the problem. The proxy logs the game's own command line from inside the
process, so what arrives is observed rather than assumed, and `-LogCmds` arrives whole in both
spellings. Neither raises anything. The game's executable carries an allow list of what a shipping
build accepts,

```
-game -log -unattended -nosplash -RenderOffscreen -d3d12 -nullrhi -NoSound
-NoLoadingScreen -TILoadTest ... -LogCmds -abslog
```

which is The Isle's own list, their load-test switches included. `-abslog` is on it, differs from
`-LogCmds` only in having no spaces in its value, and works every time: the engine writes its whole
log to the path named. So allow-listed arguments are honoured and `-LogCmds` is lost on its own.
Setting the same thing the spaceless way, `-ini:Engine:[Core.Log]:LogNet=Verbose` and one of those
per category with `global=Verbose` alongside, does nothing either. That run's log came out smaller
than the run before it.

What works is the config file, and one session proves it. At the same line of startup where every
other session goes straight on to `LogNFORDenoise`, that one prints:

```
LogHAL: Log category LogRedpointEOSNetworking verbosity has been raised to Verbose.
  ... six more, LogNet and LogHandshake and LogNetDriver among them ...
LogOnline: Verbose: OSS: [OnlineSubsystemRedpointEOS].bEnabled is not set, defaulting to true
```

That last line is a category answering at a verbosity it does not have by default. The seven named
match no list the launcher has ever passed and two were never asked for at all, which is what a
hand-written `[Core.Log]` section looks like. So log suppression here is alive and reads its config.
The section simply never survived to be read a second time: the game rewrites
`Saved\Config\WindowsClient\Engine.ini` whenever its handshake component saves a client id, and that
drops anything it does not recognise.

It does not have to survive. The launcher writes the section immediately before starting the game,
which puts it there for the one moment it is read, and the rewrite afterwards costs nothing because
the next launch writes it again.

That rewritten file is worth reading on its own account. What the game leaves in it is
`[GameNetDriver StatelessConnectHandlerComponent]`, and its `CachedClientID` has counted up once per
connection attempt. Unreal's stateless handshake did run every time, which is the last thing the
game admits to before the silence.

**That is the honest limit of this whole approach, and it is not a bug to be fixed.** Everything
before it works: login, the server browser, the matchmaker, the session lookup, the join, the net
driver, the connection itself. Forging the last step is precisely the thing anti-cheat exists to
prevent, and no amount of work inside the client changes that.

What is left is the server's own configuration. A server that does not run the server half never
sends that challenge and never waits for it — which in practice means a server you run yourself,
where anti-cheat is your setting to make. Everything in this repo already works up to that line.

`EOS_PROXY_NO_ANTICHEAT=1` leaves `BeginSession` alone, which is how you tell this failure apart
from a later one.



# Building

Run `build.bat`. It locates MSVC through vswhere if you are not already in a developer prompt, and produces:

- `EOSSDK-Win64-Shipping.dll` - the proxy itself
- `eos-proxy-setup.exe` - the installer, with that DLL and the auth payloads embedded as resources

## The auth backend payloads

`installer/payload/<backend>/` holds the config that ships inside the exe. `tools/pack.c` packs
each folder into an archive that `setup.rc` embeds, and the installer writes those files into the
game folder next to the downloaded emulator.

The emulators themselves are not here and are not downloaded at build time - see
[Antivirus](#antivirus) for why. Anything you do drop into a payload folder is installed along
with the download, so it is the place for extra per-game config.

Payload files land beside the emulator, wherever the installer worked out that is. A top level
`@exe\` folder in the payload is the one exception: what is under it is written next to the game
executable instead, which is where a backend like uc-online2 looks for its `union-crax.ini`.

Files whose name starts with `_` are left out of the archive, and any
`.ini .txt .cfg .yaml .yml .json .xml` file is a template, with these substituted as it is
written into the game folder:

| | |
|---|---|
| `{APPID}` | Steam AppID of the game |
| `{ACCOUNT_NAME}` | Steam account name, from the registry |
| `{STEAM_ID}` | SteamID64 |
| `{GAME_NAME}` | name of the game folder |
| `{GAME_DIR}` | full path it was installed into |

Where the installer downloads a backend from, and which file it takes out of the archive, is the
table at the top of `installer/auth.h`. It asks the GitHub API for the latest release rather than
pinning a version, so it keeps working as those projects publish new builds.

# Disclaimer / Credits

Developers can choose to disable this login option. Therefore this proxy is not going to work for every game. This work isn't original at all, so here are some credits:

- Online-Fix for the approach used. Others may have done it but my approach is entirely based on their fixes.
- Functions for logging (in `common.h`) and fetching Steam ID / persona name (in `steam.h`) are slop (by Claude)

The installer downloads, and does not redistribute, these projects. Each stays under its own
licence and is fetched from its own release page:

- [gbe_fork](https://github.com/Detanup01/gbe_fork) by Detanup01 and contributors, GPLv3.
  A fork of Goldberg Emulator
- [uc-online2](https://github.com/UnionCrax-Team/uc-online2) by the UnionCrax Team
- [SLSsteam](https://github.com/AceSLS/SLSsteam) by AceSLS. Only its config is generated here
- [7-Zip](https://www.7-zip.org/) by Igor Pavlov. `7zr.exe`, the public domain standalone
  extractor, is fetched when an archive needs a 7z reader that Windows' own `tar.exe` cannot
  provide
