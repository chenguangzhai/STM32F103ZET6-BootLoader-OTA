@echo off
rem ============================================================
rem  STM32 OTA 串口助手  启动器
rem  双击本文件即可，不需要命令行
rem ============================================================
cd /d "%~dp0"

rem ---- 优先用带 tkinter + pyserial 的 Python 3.11 ----
if exist "C:\Python311\pythonw.exe" (
    start "" "C:\Python311\pythonw.exe" "%~dp0serial_assist.py"
    exit /b
)

rem ---- 退而求其次，找系统 python ----
where pythonw >nul 2>nul
if %errorlevel%==0 (
    start "" pythonw "%~dp0serial_assist.py"
    exit /b
)

echo.
echo   [!] 没找到可用的 Python。
echo       本工具需要 Python 3 + tkinter + pyserial。
echo       推荐安装 Python 3.11 到 C:\Python311，然后：
echo           C:\Python311\python.exe -m pip install pyserial
echo.
pause
