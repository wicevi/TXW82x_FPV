@echo off
rem Forward to build.sh (Git Bash). Usage mirrors build.sh:
rem   build.bat            interactive menu (default choice: TXW Core->App)
rem   build.bat txw        TXW Core->App, archive          [default build]
rem   build.bat core       TXW Core only (no archive)
rem   build.bat app        TXW App only, archive           (alias: txw-app)
rem   build.bat pmu        PMU only, archive
rem   build.bat all        TXW + PMU, archive both
rem   build.bat clean      clean TXW Core + App + PMU
rem   build.bat clean-core / clean-app / clean-pmu
where bash >nul 2>nul
if %errorlevel%==0 (
    bash "%~dp0build.sh" %*
    goto :eof
)
if exist "C:\Program Files\Git\bin\bash.exe" (
    "C:\Program Files\Git\bin\bash.exe" "%~dp0build.sh" %*
    goto :eof
)
echo Git Bash not found. Install Git for Windows or run build.sh from Git Bash.
exit /b 1
