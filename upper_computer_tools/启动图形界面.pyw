from __future__ import annotations

import traceback
from pathlib import Path
from tkinter import Tk, messagebox


def main():
    base_dir = Path(__file__).resolve().parent
    try:
        import os
        import sys

        os.chdir(base_dir)
        if str(base_dir) not in sys.path:
            sys.path.insert(0, str(base_dir))

        import host_gui

        host_gui.main()
    except Exception:
        root = Tk()
        root.withdraw()
        messagebox.showerror(
            "启动失败",
            "图形界面启动失败，请把这段报错发给开发者：\n\n" + traceback.format_exc(),
        )
        root.destroy()


if __name__ == "__main__":
    main()
