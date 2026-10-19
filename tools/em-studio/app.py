"""EM Studio —— EMlink 桌面调试台（D-1）。

pywebview 入口 + JS 桥。桥方法统一返回 {"ok": bool, "data": ..., "error": str}。
"""

import os
import threading
import time

import webview

from backend import flasher, mav_raw, params, prober
from backend.devices import list_devices
from backend.serial_link import SerialLink

_DEFAULT_FIRMWARE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "firmware",
    ".pio",
    "build",
    "esp32s3-supermini-e220-900m22s",
    "firmware.bin",
)


def _int_arg(value, name, low, high):
    try:
        number = int(str(value).strip())
    except (TypeError, ValueError):
        raise ValueError(f"{name} 必须是整数")
    if number < low or number > high:
        raise ValueError(f"{name} 必须在 {low}..{high}")
    return number


def _payload_bytes(text, hex_mode):
    raw = str(text or "")
    if not hex_mode:
        return raw.encode("utf-8")
    cleaned = raw.replace("0x", "").replace("0X", "")
    cleaned = "".join(ch for ch in cleaned if ch not in " \t\r\n,;:_-")
    if len(cleaned) % 2:
        raise ValueError("hex payload 长度必须是偶数")
    try:
        return bytes.fromhex(cleaned)
    except ValueError:
        raise ValueError("hex payload 含非法字符")


class Api:
    def __init__(self):
        self._lock = threading.Lock()
        self._links = {}
        self._port_ids = {}
        self._next_id = 1
        self._flash_lock = threading.Lock()
        self._flash_state = {}
        self._flash_active = None

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

    def api_default_firmware(self):
        exists = os.path.isfile(_DEFAULT_FIRMWARE)
        size = 0
        if exists:
            try:
                size = os.path.getsize(_DEFAULT_FIRMWARE)
            except OSError:
                exists = False
        return self._ok({"path": _DEFAULT_FIRMWARE, "exists": exists, "size": size})

    def api_pick_firmware(self):
        try:
            window = webview.windows[0]
            directory = os.path.dirname(_DEFAULT_FIRMWARE)
            if not os.path.isdir(directory):
                directory = ""
            selected = window.create_file_dialog(
                webview.OPEN_DIALOG,
                directory=directory,
                file_types=("固件 (*.bin)",),
            )
        except Exception as exc:
            return self._err(f"打开文件对话框失败：{exc}")

        if not selected:
            return self._ok("")
        if isinstance(selected, (list, tuple)):
            selected = selected[0] if selected else ""
        return self._ok(os.path.abspath(str(selected)) if selected else "")

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

    def _set_flash_state(self, device_id, **changes):
        with self._flash_lock:
            state = self._flash_state.setdefault(
                device_id,
                {"progress": 0, "state": "idle", "message": ""},
            )
            state.update(changes)
            return dict(state)

    def _flash_worker(self, device_id, link, bin_path, erase):
        port = link.port
        baud = link.baud
        started = time.monotonic()
        result = {"ok": False, "message": "烧录失败", "log": ""}

        def on_progress(percent):
            self._set_flash_state(
                device_id,
                progress=int(percent),
                message=f"烧录中… {int(percent)}%",
            )

        try:
            link.close()
            result = flasher.flash(
                port,
                bin_path,
                erase=bool(erase),
                on_progress=on_progress,
            )
        except Exception as exc:
            result = {"ok": False, "message": f"烧录异常：{exc}", "log": ""}

        elapsed = time.monotonic() - started
        ok = bool(result.get("ok"))
        if ok:
            message = f"烧录完成（{elapsed:.1f} s）"
            self._set_flash_state(
                device_id,
                progress=100,
                message=message + "，正在重连…",
            )
        else:
            message = str(result.get("message") or "烧录失败").splitlines()[0]
            self._set_flash_state(device_id, message=message + "，正在重连…")

        reconnected = False
        reconnect_error = ""
        with self._lock:
            still_present = self._links.get(device_id) is link
        if still_present:
            for _ in range(15):
                reply = self.api_connect(port, baud)
                if reply.get("ok"):
                    reconnected = True
                    break
                reconnect_error = reply.get("error", "")
                time.sleep(0.3)

        if not reconnected:
            message += "；自动重连失败"
            if reconnect_error:
                message += f"：{reconnect_error}"

        with self._flash_lock:
            state = self._flash_state.setdefault(
                device_id,
                {"progress": 0, "state": "idle", "message": ""},
            )
            state["state"] = "ok" if ok else "fail"
            state["message"] = message
            if ok:
                state["progress"] = 100
            if self._flash_active == device_id:
                self._flash_active = None

    def api_flash(self, device_id, bin_path, erase=False):
        device_id = str(device_id or "")
        link = self._get(device_id)
        if link is None:
            return self._err("设备不存在")

        bin_path = str(bin_path or "").strip()
        if not bin_path:
            return self._err("未选择固件文件")
        if not bin_path.lower().endswith(".bin"):
            return self._err("只支持 .bin 固件")
        if not os.path.isfile(bin_path):
            return self._err(f"固件文件不存在：{bin_path}")
        if not flasher.find_esptool():
            return self._err("找不到 esptool.py（请确认已安装，或用 ESPTOOL 环境变量指定）")

        with self._flash_lock:
            if self._flash_active is not None:
                return self._err("已有烧录任务正在进行")
            self._flash_active = device_id
            self._flash_state[device_id] = {
                "progress": 0,
                "state": "running",
                "message": "准备烧录…",
            }

        thread = threading.Thread(
            target=self._flash_worker,
            args=(device_id, link, os.path.abspath(bin_path), bool(erase)),
            name=f"em-{device_id}-flash",
            daemon=True,
        )
        thread.start()
        return self._ok({"started": True, "state": "running"})

    def api_flash_progress(self, device_id):
        device_id = str(device_id or "")
        with self._flash_lock:
            state = self._flash_state.get(device_id)
            if state is None:
                state = {"progress": 0, "state": "idle", "message": ""}
            else:
                state = dict(state)
        return self._ok(state)

    def api_send(self, device_id, text):
        link = self._get(device_id)
        if link is None:
            return self._err("设备不存在")
        ok, message = link.send(str(text or ""))
        if not ok:
            return self._err(message)
        return self._ok({"sent": True})

    def api_set_mode(self, device_id, mode):
        link = self._get(device_id)
        if link is None:
            return self._err("设备不存在")
        mode = str(mode or "")
        if mode not in ("cli", "mavlink"):
            return self._err("模式必须是 cli 或 mavlink")
        link.set_mode(mode)
        return self._ok({"mode": link.mode})

    def api_send_frame(
        self,
        device_id,
        payload,
        msgid,
        sysid=255,
        compid=0,
        target_sysid=0,
        hex_mode=False,
    ):
        link = self._get(device_id)
        if link is None:
            return self._err("设备不存在")
        if link.status != "connected":
            return self._err("设备未连接")
        try:
            msgid = _int_arg(msgid, "msgid", 0, 0xFFFFFF)
            sysid = _int_arg(sysid, "sysid", 0, 0xFF)
            compid = _int_arg(compid, "compid", 0, 0xFF)
            target = _int_arg(target_sysid, "target_sysid", 0, 0xFF)
            data = _payload_bytes(payload, bool(hex_mode))
            frame = mav_raw.build_frame(
                data,
                msgid,
                sysid=sysid,
                compid=compid,
                target_sysid=target,
            )
        except ValueError as exc:
            return self._err(f"组帧失败：{exc}")
        ok, message = link.send_bytes(frame)
        if not ok:
            return self._err(message)
        return self._ok({
            "sent": True,
            "bytes": len(frame),
            "payload_len": frame[1],
            "seq": frame[4],
            "frame_hex": frame.hex(),
            "crc_extra": mav_raw.crc_extra_for(msgid),
        })

    def api_mav_poll(self, device_id):
        link = self._get(device_id)
        if link is None:
            return self._err("设备不存在")
        return self._ok(link.poll_mav())

    def api_poll(self, device_id):
        link = self._get(device_id)
        if link is None:
            return self._err("设备不存在")
        data = link.poll()
        data["led"] = link.led_state
        return self._ok(data)

    def api_status(self, device_id):
        link = self._get(device_id)
        if link is None:
            return self._err("设备不存在")
        if link.status != "connected":
            return self._err("设备未连接")
        try:
            data, note = prober.read_status(link)
        except Exception as exc:
            return self._err(f"读状态失败：{exc}")
        if not data:
            return self._err(note or "板子未返回状态")
        return {"ok": True, "data": data, "error": note or ""}

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

    def api_led_state(self, device_id):
        link = self._get(device_id)
        if link is None:
            return self._err("设备不存在")
        if link.status != "connected":
            return self._err("设备未连接")
        try:
            data, note = params.read_led(link)
        except Exception as exc:
            return self._err(f"读灯态失败：{exc}")
        if not data:
            return self._err(note or "板子未返回灯态")
        return self._ok(data)

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
