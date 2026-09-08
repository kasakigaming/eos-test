gbe_fork payload
================

Everything in this folder (except files starting with "_") is packed into
eos-proxy-setup.exe at build time and copied into the game folder, keeping this
exact layout, when you pick gbe_fork in the installer.

The config files here are already written, and they are all this folder needs
to hold. The emulator itself is downloaded at install time from

    https://github.com/Detanup01/gbe_fork

and cached under %LOCALAPPDATA%\eos-proxy. setup.exe picks the experimental x64
build out of the archive, because the regular one does not hand out the web API
ticket that the EOS proxy is waiting on.

Anything you do drop in here is copied into the game folder alongside the
download, so this is the place for extra config a particular game wants.

Layout to end up with:

    payload/gbe_fork/
        steam_appid.txt              the game's run path, where most tools look
        steam_settings/
            steam_appid.txt          where gbe_fork itself looks first
            configs.main.ini
            configs.user.ini
            configs.app.ini

The emulator is not here and is not meant to be: setup.exe downloads
steam_api64.dll from the gbe_fork release when you install the backend. Only
put a copy in this folder if you want a specific build baked into the exe
instead, and read the note about antivirus in the top level README first.

Any .ini .txt .cfg .yaml .yml .json .xml file in here is a template. These get
substituted when the file is written into the game folder:

    {APPID}          Steam AppID of the game, detected from the install
    {ACCOUNT_NAME}   your Steam account name, from the registry
    {STEAM_ID}       your SteamID64
    {GAME_NAME}      folder name of the game
    {GAME_DIR}       full path the files were installed into

Files already present in the game folder are parked as <name>.eosbak before
being overwritten, and put back when you remove the backend.
