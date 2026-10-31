ESP32-S3-CAM_PDX DATV Beacon - Windows package
https://github.com/Photoluis/Esp32s3-cam-pdx-datv-beacon        GPL-3.0 - IZ7PDX

No Python, no Arduino, no ESP-IDF needed. / Non servono Python, Arduino o ESP-IDF.

1. Unzip this folder anywhere (e.g. Desktop).  / Estraete la cartella dove volete (es. Desktop).
2. Connect the board with a USB-C DATA cable to the port marked COM (CH340).
   Collegate la scheda con un cavo USB-C DATI alla porta marcata COM (CH340).
   Device Manager -> Ports (COM & LPT) shows "USB-SERIAL CH340 (COMx)".
   If not: install the WCH CH341SER driver. / Se non compare: installate il driver WCH CH341SER.
3. Double-click / doppio clic:
     1_INSTALL_FULL_erase.bat      first installation (erases the board)  / prima installazione (cancella la scheda)
     2_UPDATE_firmware_only.bat    update, keeps settings and custom test cards / aggiornamento, conserva le impostazioni
4. Press RST. Wi-Fi "NOCALL-BEACON", password "beacon13cm", http://192.168.4.1
   Set YOUR callsign, locator, frequency, cycle -> Apply and save. Change the Wi-Fi password.

If the connection fails: hold BOOT, press and release RST, release BOOT, run the script again.
Se la connessione non riesce: tenere BOOT, premere e rilasciare RST, rilasciare BOOT e rilanciare lo script.

Licensed radio amateurs only, on the frequencies and power allowed by your licence, always with an output filter.

esptool.exe: Espressif esptool v5.6.0 (GPL-2.0), https://github.com/espressif/esptool - licence in esptool_LICENSE.txt
