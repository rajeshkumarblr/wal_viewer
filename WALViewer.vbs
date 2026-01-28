Set Wshall = CreateObject("WScript.Shell")
Wshall.Run "wsl.exe --cd /home/rajesh/proj/wal_viewer -- ./build/wal_viewer_gui", 0, False
