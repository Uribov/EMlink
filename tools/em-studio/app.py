"""EM Studio —— EMlink 桌面调试台（D-1）。

pywebview 入口 + JS 桥。桥方法统一返回 {"ok": bool, "data": ..., "error": str}。
"""

import os
import threading

import webview

from backend import params
from backend.devices import list_devices
from backend.serial_link import SerialLink


class Api:
    def __init__(self):
        self._lock = threading.Lock()
        self._links = {}
        self._port_ids = {}
        self._next_id = 1

    @staticmethod
    def _ok(data):
        return {"ok": True, "data": data, "error": ""}

    @staticmethod
    def _err(message):
        return {"ok": False, "data": None, "error": message}

    def _get(self, device_id):
        with self._lock:
            return self._links.get(str(device_id or ""))

    def _alloc_id_locked(self):
        device_id = f"d{self._next_id}"
        self._next_id += 1
        return device_id

    def api_list_ports(self):
        try:
            return self._ok(list_devices())
        except Exception as exc:
            return self._err(f"端口枚举失败：{exc}")

    def api_connect(self, port, baud=115200):
        port = str(port or "").strip()
        if not port:
            return self._err("未指定串口端口")
        try:
            baud = int(baud or 115200)
        except (TypeError, ValueError):
            baud = 115200

        with self._lock:
            device_id = self._port_ids.get(port)
            link = self._links.get(device_id) if device_id else None
            created = False
            if link is None:
                device_id = self._alloc_id_locked()
                link = SerialLink(device_id, port, baud)
                self._links[device_id] = link
                self._port_ids[port] = device_id
                created = True

        if link.status == "connected":
            return self._ok({"device_id": device_id})

        ok, message = link.open()
        if not ok:
            if created:
                with self._lock:
                    if self._links.get(device_id) is link:
                        self._links.pop(device_id, None)
                    if self._port_ids.get(port) == device_id:
                        self._port_ids.pop(port, None)
                link.close_permanently()
            return self._err(message)
        return self._ok({"device_id": device_id})

    def api_disconnect(self, device_id):
        link = self._get(device_id)
        if link is None:
            return self._err("设备不存在")
        link.close()
        return self._ok(True)

    def api_send(self, device_id, text):
        link = self._get(device_id)
        if link is None:
            return self._err("设备不存在")
        ok, message = link.send(str(text or ""))
        if not ok:
            return self._err(message)
        return self._ok({"sent": True})

    def api_poll(self, device_id):
        link = self._get(device_id)
        if link is None:
            return self._err("设备不存在")
        return self._ok(link.poll())

    def api_get_config(self, device_id):
        link = self._get(device_id)
        if link is None:
            return self._err("设备不存在")
        if link.status != "connected":
            return self._err("设备未连接")
        try:
            data, note = params.read_config(link)
        except Exception as exc:
            return self._err(f"读配置失败：{exc}")
        return {"ok": True, "data": data, "error": note or ""}

    def api_set_config(self, device_id, key, value):
        link = self._get(device_id)
        if link is None:
            return self._err("设备不存在")
        try:
            ok, message = params.set_config(link, key, value)
        except Exception as exc:
            return self._err(f"设置失败：{exc}")
        return {
            "ok": True,
            "data": {"key": str(key or ""), "ok": bool(ok)},
            "error": "" if ok else message,
        }

    def api_save_config(self, device_id, reboot=False):
        link = self._get(device_id)
        if link is None:
            return self._err("设备不存在")
        try:
            saved, rebooting, message = params.save_config(link, bool(reboot))
        except Exception as exc:
            return self._err(f"保存失败：{exc}")
        error = message if message else ("" if saved else "保存失败")
        return {
            "ok": True,
            "data": {"saved": bool(saved), "rebooting": bool(rebooting)},
            "error": error,
        }

    def api_close_tab(self, device_id):
        device_id = str(device_id or "")
        with self._lock:
            link = self._links.get(device_id)
        if link is None:
            return self._err("设备不存在")

        # 先断开（停读线程、释放串口），再从后端移除，最后前端才移除标签。
        link.close_permanently()
        with self._lock:
            self._links.pop(device_id, None)
            if self._port_ids.get(link.port) == device_id:
                self._port_ids.pop(link.port, None)
        return self._ok(True)

    def shutdown(self):
        with self._lock:
            links = list(self._links.values())
            self._links.clear()
            self._port_ids.clear()
        for link in links:
            link.close_permanently()


def main():
    base = os.path.dirname(os.path.abspath(__file__))
    index = os.path.join(base, "ui", "index.html")
    api = Api()
    window = webview.create_window(
        "EM Studio",
        index,
        js_api=api,
        width=1180,
        height=760,
        min_size=(900, 600),
    )
    try:
        window.events.closing += api.shutdown
    except Exception:
        pass
    webview.start()


if __name__ == "__main__":
    main()
