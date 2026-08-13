# -*- coding: utf-8 -*-
"""
打包 font_hansan_24.bin + callloc.bin 为 LittleFS 镜像并烧录到 ffat 分区（0x610000）
用法:
  python fs_pack.py            # 仅生成镜像
  python fs_pack.py --flash    # 生成并烧录（默认 COM4）
  python fs_pack.py --flash COM5   # 指定串口
"""
import os, sys, subprocess, glob, shutil

BASE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(BASE, "font_hansan_24.bin")
CALLLOC = os.path.join(BASE, "callloc.bin")
IMG = os.path.join(BASE, "font_fs.bin")

MKLITTLEFS = r"C:/Users/Tallboy/AppData/Local/Arduino15/packages/m5stack/tools/mklittlefs/4.0.2-db0513a-cn/mklittlefs.exe"
ESPTOOL = r"C:/Users/Tallboy/AppData/Local/Arduino15/packages/m5stack/tools/esptool_py/5.2.0-cn/esptool.exe"
FS_SIZE = 0x9E0000  # ffat 分区大小 (9.9MB)

def main():
    if not os.path.exists(MKLITTLEFS):
        print("mklittlefs 不存在:", MKLITTLEFS); sys.exit(1)
    if not os.path.exists(BIN):
        print("字体文件不存在:", BIN); sys.exit(1)
    if not os.path.exists(CALLLOC):
        print("呼号表不存在（先运行 gen_callloc.py）:", CALLLOC); sys.exit(1)

    # 生成 LittleFS 镜像
    tmpdir = os.path.join(BASE, "_fs_tmp")
    os.makedirs(tmpdir, exist_ok=True)
    shutil.copy2(BIN, os.path.join(tmpdir, "font_hansan_24.bin"))
    shutil.copy2(CALLLOC, os.path.join(tmpdir, "callloc.bin"))
    cmd = [MKLITTLEFS, "-c", tmpdir, "-b", "4096", "-p", "256", "-s", str(FS_SIZE), IMG]
    print("运行:", " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    print(r.stdout, r.stderr)
    if r.returncode != 0:
        sys.exit(1)
    print(f"镜像已生成: {IMG} ({os.path.getsize(IMG)/1024/1024:.1f} MB)")
    shutil.rmtree(tmpdir, ignore_errors=True)

    if "--flash" in sys.argv:
        port = "COM4"
        if len(sys.argv) > sys.argv.index("--flash") + 1 and not sys.argv[sys.argv.index("--flash") + 1].startswith("-"):
            port = sys.argv[sys.argv.index("--flash") + 1]
        if not os.path.exists(ESPTOOL):
            print("未找到 esptool:", ESPTOOL); sys.exit(1)
        print(f"使用串口: {port}")
        cmd = [ESPTOOL, "--chip", "esp32s3", "--port", port, "--baud", "921600",
               "write_flash", "0x610000", IMG]
        print("运行:", " ".join(cmd))
        subprocess.run(cmd)
        print("\n烧录完成！")

if __name__ == "__main__":
    main()
