@echo off
rem NE102 self-burn (2026-10-08 proven method).
rem Uses the CDK-generated config CSKYFlashProgramerCfg (relative paths
rem resolve against the project dir, EnableTRST=false, init script wired).
rem Earlier custom JSON configs hung forever: EnableTRST=true holds the
rem debug logic in reset and the console never reaches the target.
rem Board must be powered; no GUI FlashProgrammer may be running (it
rem exclusively holds the CKLink).

cd /d %~dp0..\..\project\txw82xApp
"D:\C-SKY\CDK\CSKY\FlashProgrammer\Bins\CSKYFlashProgramerConsole.exe" -f CSKYFlashProgramerCfg
exit /b %ERRORLEVEL%
