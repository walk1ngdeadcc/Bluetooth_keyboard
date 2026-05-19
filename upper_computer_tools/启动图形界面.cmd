@echo off
setlocal
cd /d "%~dp0"

where py.exe >nul 2>nul
if not errorlevel 1 (
    py -3 host_gui.py
    goto :check_result
)

where python.exe >nul 2>nul
if not errorlevel 1 (
    python host_gui.py
    goto :check_result
)

echo 未找到可用的 Python 运行环境。
echo 请先安装 Python 3，并确保勾选 Add Python to PATH。
pause
exit /b 1

:check_result
if errorlevel 1 (
    echo.
    echo 图形界面启动失败，请检查上面的报错信息。
    pause
    exit /b 1
)

exit /b 0
