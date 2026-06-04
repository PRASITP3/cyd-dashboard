' Launches the Claude Code usage uploader with NO visible window.
' Used by the "ClaudeCodeUploader" scheduled task at logon.
Set sh = CreateObject("WScript.Shell")
sh.CurrentDirectory = "C:\projects\cyd-dashboard\backend"
sh.Run """C:\Program Files\nodejs\node.exe"" upload-cc.js --watch", 0, False
