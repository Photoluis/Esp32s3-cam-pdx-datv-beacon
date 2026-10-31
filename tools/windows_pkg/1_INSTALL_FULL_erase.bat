@echo off
rem ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
rem Prima installazione / first installation: cancella la scheda e scrive l'immagine completa a 0x0
rem (bootloader, partizioni, firmware, monoscopio standard, sfondo PiP standard).
cd /d "%~dp0"
title DATV Beacon - FULL install
echo.
echo  ESP32-S3-CAM_PDX DATV Beacon - FULL INSTALL (erases the board / cancella la scheda)
echo  Connect the board to the COM port (CH340).  Collegate la scheda alla porta COM (CH340).
echo.
set /p PORT=COM port (e.g. COM3, empty = auto) / porta COM (es. COM3, vuoto = automatico):
set PORTARG=
if not "%PORT%"=="" set PORTARG=--port %PORT%
for %%F in (*_full.bin) do set IMG=%%F
if "%IMG%"=="" ( echo ERROR: *_full.bin not found & pause & exit /b 1 )
"%~dp0esptool.exe" --chip esp32s3 %PORTARG% -b 921600 write-flash 0x0 "%IMG%"
if errorlevel 1 (
  echo.
  echo  FAILED. Hold BOOT, press and release RST, release BOOT, then run again.
  echo  NON RIUSCITO. Tenere BOOT, premere e rilasciare RST, rilasciare BOOT e riprovare.
) else (
  echo.
  echo  DONE. Press RST, then connect to Wi-Fi NOCALL-BEACON ^(password beacon13cm^) and open http://192.168.4.1
  echo  FATTO. Premere RST, collegarsi al Wi-Fi NOCALL-BEACON ^(password beacon13cm^) e aprire http://192.168.4.1
)
echo.
pause
