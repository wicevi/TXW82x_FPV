@echo off
rem NE102 bring-up: program APP.bin via CKLink with FULL verify.
rem Usage: cklink_program_verify.bat  (CKLink attached, board powered)
rem Difference to the TXW burn tool: Verify=true here re-reads the WHOLE image
rem after writing, so a partial/failed program cannot pass silently.

"D:\C-SKY\CDK\CSKY\FlashProgrammer\Bins\CSKYFlashProgramerConsole.exe" -f "%~dp0cklink_program_verify.json"
exit /b %ERRORLEVEL%
