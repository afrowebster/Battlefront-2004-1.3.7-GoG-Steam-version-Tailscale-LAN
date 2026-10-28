@echo off
setlocal

:: --- EDIT THESE ---
set "GAMEDIR=C:\GOG Games\Star Wars - Battlefront\GameData"
set "SERVERNAME=MyLANServer"
:: Map entries are: mapcode ticketsTeam1 ticketsTeam2
set "MAPS=tat2i 100 100"
:: Players needed before the match starts (game default is 2)
set "MINPLAYERS=1"
:: AI bots (game default is 16; 32 is the highest value the game itself uses)
set "BOTS=32"
:: Network profile: the game's LAN setting is 32 (= ~1.3 MB/s server upload budget).
:: The menu sets this when hosting on LAN; the command-line dedicated server does not.
set "NETPLAYERS=32"
:: ------------------

:: /sideselect turns off automatic team assignment, so players can pick and switch sides
cd /d "%GAMEDIR%"
battlefront.exe /win /norender /autonet dedicated /lan /resolution 320 240 /nosound /playercount %MINPLAYERS% /bots %BOTS% /netplayers %NETPLAYERS% /sideselect /gamename %SERVERNAME% %MAPS%
echo Exit code: %errorlevel%
pause
