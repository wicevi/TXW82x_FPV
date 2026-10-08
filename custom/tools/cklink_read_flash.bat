@echo off
rem NE102 bring-up: full flash readback via CKLink (verifies the WHOLE image,
rem not only the 1KB header the TXW tool exported before).
rem Usage: cklink_read_flash.bat  (CKLink attached, board powered, run from repo root)
rem Output: archive/cklink_full_read.bin  (0x130000 bytes from flash offset 0)
rem Compare afterwards:  fc /b archive\cklink_full_read.bin project\txw82xApp\APP.bin
rem                      (APP.bin is 0x123010 bytes; read is intentionally 0x130000,
rem                       fc will report "files different sizes" plus the first diff
rem                       offset - any diff INSIDE 0x123010 means flash content issue)

"D:\C-SKY\CDK\CSKY\FlashProgrammer\Bins\CSKYFlashProgramerConsole.exe" -f "%~dp0cklink_read_flash.json"
exit /b %ERRORLEVEL%
