"""诊断结果结构化（D-3 预留）。

本版只留占位：D-1 的诊断台直接显示原始文本，不做解析。
"""


def probe(cli_client, command):
    """占位函数：执行命令并返回结构化结果（本版始终返回 None）。

    参数：
        cli_client: 该设备的 CliClient 实例。
        command: 想探测的命令，例如 "status" / "stats"。

    后续 D-3 在这里把 status/stats 的文本解析成三层状态与统计字段。
    """
    _ = (cli_client, command)
    return None
