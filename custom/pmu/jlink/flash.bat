@echo off
rem NE102 PMU (N32L403KBQ7) J-Link download
rem If JLink reports unknown device N32L403KB, install the Nationstech device
rem support package first: custom/doc/n32l40x/tools/JLink_tool_adds_Nations_chip_V1.6.0.zip
set JLINK=%~1
if "%JLINK%"=="" set JLINK=D:\Program Files\SEGGER\JLink_V962\JLink.exe
"%JLINK%" -Device N32L403KB -If SWD -Speed 4000 -AutoConnect 1 -CommandFile "%~dp0flash.jlink"
