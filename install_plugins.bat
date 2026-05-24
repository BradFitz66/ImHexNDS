@echo off
set TARGET=%LocalAppData%\ImHex\plugins

if not exist "%TARGET%" mkdir "%TARGET%"

for /r "%~dp0build\_plugins" %%f in (*.hexplug) do (
    echo Copying %%~nxf...
    copy /y "%%f" "%TARGET%\" >nul
)

echo Done. Plugins installed to %TARGET%
