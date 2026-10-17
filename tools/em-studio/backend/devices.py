"""串口枚举与设备识别。

只依赖 pyserial 的 list_ports，不读 udev、不硬编码 ttyACM0。
"""

from serial.tools import list_ports


def _vidpid(info):
    vid = info.vid
    pid = info.pid
    if vid is None or pid is None:
        return ""
    return f"{vid:04x}:{pid:04x}"


def _usb_name(device):
    name = (device or "").replace("\\", "/").rsplit("/", 1)[-1].lower()
    return name.startswith(("ttyacm", "ttyusb", "cu.usb", "tty.usb", "com"))


def _has_desc(desc):
    return bool(desc) and desc.lower() != "n/a"


def list_devices():
    """返回当前系统可见的 USB 类串口列表（按端口名排序）。

    主板自带的 ttyS* 等非 USB 串口会被过滤掉。
    """
    devices = []
    for info in sorted(list_ports.comports(), key=lambda item: item.device):
        desc = (info.description or "").strip()
        vidpid = _vidpid(info)
        usb_name = _usb_name(info.device)
        if not (vidpid or _has_desc(desc) or usb_name):
            continue
        devices.append(
            {
                "port": info.device,
                "desc": desc,
                "serial": info.serial_number or "",
                "vidpid": vidpid,
                "kind": "usb" if vidpid or usb_name else "other",
            }
        )
    return devices
