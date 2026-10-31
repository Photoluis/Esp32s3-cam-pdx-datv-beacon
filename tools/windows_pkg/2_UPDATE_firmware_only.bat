@echo off
rem ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
rem Aggiornamento / update: scrive solo bootloader, partizioni e firmware (0x0 .. fine app).
rem Impostazioni (NVS) e monoscopi personalizzati restano.
cd /d "%~dp0"
title DATV Beacon - firmware update
echo.
echo  ESP32-S3-CAM_PDX DATV Beacon - FIRMWARE UPDATE (keeps settings and custom test cards)
echo.
set /p PORT=COM port (e.g. COM3, empty = auto) / porta COM (es. COM3, vuoto = automatico):
set PORTARG=
if not "%PORT%"=="" set PORTARG=--port %PORT%
for %%F in (*_app_only.bin) do set IMG=%%F
if "%IMG%"=="" ( echo ERROR: *_app_only.bin not found & pause & exit /b 1 )
"%~dp0esptool.exe" --chip esp32s3 %PORTARG% -b 921600 write-flash 0x0 "%IMG%"
if errorlevel 1 (
  echo.
  echo  FAILED. Hold BOOT, press and release RST, release BOOT, then run again.
  echo  NON RIUSCITO. Tenere BOOT, premere e rilasciare RST, rilasciare BOOT e riprovare.
) else (
  echo.
  echo  DONE / FATTO. Press RST / premere RST.
)
echo.
pause
