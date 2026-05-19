@echo off
setlocal
cd /d "%~dp0"

python -m PyInstaller --version >nul 2>nul
if errorlevel 1 (
    echo 当前 Python 环境未安装 PyInstaller。
    echo 可先执行:
    echo   python -m pip install --user pyinstaller
    echo 然后再双击本脚本重新打包。
    pause
    exit /b 1
)

python -m PyInstaller ^
    --noconfirm ^
    --clean ^
    --onefile ^
    --windowed ^
    --name MiniKeyboardHost ^
    --hidden-import serial.tools.list_ports_windows ^
    host_gui.py

if errorlevel 1 (
    echo.
    echo 打包失败，请查看上面的错误信息。
    pause
    exit /b 1
)

echo.
echo 打包完成：
echo   %~dp0dist\MiniKeyboardHost.exe
pause
