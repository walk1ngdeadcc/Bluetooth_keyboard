Option Explicit

Dim fso, shell, baseDir, pywPath, pythonwPath, cmd
Set fso = CreateObject("Scripting.FileSystemObject")
Set shell = CreateObject("WScript.Shell")

baseDir = fso.GetParentFolderName(WScript.ScriptFullName)
pywPath = shell.ExpandEnvironmentStrings("%SystemRoot%") & "\pyw.exe"
pythonwPath = shell.ExpandEnvironmentStrings("%LocalAppData%") & "\Programs\Python\Python312\pythonw.exe"

If fso.FileExists(pywPath) Then
    cmd = """" & pywPath & """ -3 """ & baseDir & "\启动图形界面.pyw"""
    shell.Run cmd, 0, False
ElseIf fso.FileExists(pythonwPath) Then
    cmd = """" & pythonwPath & """ """ & baseDir & "\启动图形界面.pyw"""
    shell.Run cmd, 0, False
Else
    MsgBox "未找到可用的 Python 图形运行环境（pyw.exe / pythonw.exe）。", 16, "启动失败"
End If
