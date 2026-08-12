# Executes a python file (or statement) inside the running editor via the engine's
# python remote-execution protocol: multicast discovery, then the editor dials back
# over TCP and runs the command in-process.
# Usage:
#   python exec_in_editor.py <script.py>
#   python exec_in_editor.py --cmd "unreal.log('hi')"

import json
import socket
import sys
import time
import uuid

MULTICAST_GROUP = ("239.0.0.1", 6766)
LOCAL_ADAPTER = "127.0.0.1"
MAGIC = "ue_py"
VERSION = 1
NODE_ID = str(uuid.uuid4())
DISCOVER_TIMEOUT_SEC = 3.0
RESULT_TIMEOUT_SEC = 120.0


def message(msg_type, data=None, dest=None):
    payload = {"version": VERSION, "magic": MAGIC, "type": msg_type, "source": NODE_ID}
    if dest is not None:
        payload["dest"] = dest
    if data is not None:
        payload["data"] = data
    return json.dumps(payload).encode("utf-8")


def parse(blob):
    try:
        payload = json.loads(blob.decode("utf-8"))
    except ValueError:
        return None
    if payload.get("magic") != MAGIC or payload.get("source") == NODE_ID:
        return None
    return payload


def discover(udp):
    udp.sendto(message("ping"), MULTICAST_GROUP)
    deadline = time.time() + DISCOVER_TIMEOUT_SEC
    while time.time() < deadline:
        udp.settimeout(max(0.1, deadline - time.time()))
        try:
            blob, _ = udp.recvfrom(8192)
        except socket.timeout:
            break
        payload = parse(blob)
        if payload and payload.get("type") == "pong":
            return payload["source"]
    return None


def main():
    if len(sys.argv) < 2:
        print("usage: exec_in_editor.py <script.py> | --cmd <statement>")
        return 2

    if sys.argv[1] == "--cmd":
        command, exec_mode = sys.argv[2], "ExecuteStatement"
    else:
        command, exec_mode = sys.argv[1], "ExecuteFile"

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp.bind(("0.0.0.0", MULTICAST_GROUP[1]))
    membership = socket.inet_aton(MULTICAST_GROUP[0]) + socket.inet_aton(LOCAL_ADAPTER)
    udp.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership)
    udp.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    udp.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 0)

    editor_node = discover(udp)
    if not editor_node:
        print("FAIL: no editor answered the ping (editor closed, or remote execution disabled)")
        return 1

    tcp_server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp_server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tcp_server.bind((LOCAL_ADAPTER, 0))
    tcp_server.listen(1)
    command_port = tcp_server.getsockname()[1]

    udp.sendto(message("open_connection", {"command_ip": LOCAL_ADAPTER, "command_port": command_port}, dest=editor_node), MULTICAST_GROUP)

    tcp_server.settimeout(DISCOVER_TIMEOUT_SEC)
    try:
        conn, _ = tcp_server.accept()
    except socket.timeout:
        print("FAIL: editor did not dial back")
        return 1

    conn.sendall(message("command", {"command": command, "unattended": True, "exec_mode": exec_mode}, dest=editor_node))

    conn.settimeout(RESULT_TIMEOUT_SEC)
    buffer = b""
    result = None
    while result is None:
        try:
            chunk = conn.recv(8192)
        except socket.timeout:
            print("FAIL: timed out waiting for command_result")
            return 1
        if not chunk:
            break
        buffer += chunk
        payload = parse(buffer)
        if payload and payload.get("type") == "command_result":
            result = payload["data"]

    udp.sendto(message("close_connection", dest=editor_node), MULTICAST_GROUP)
    conn.close()
    tcp_server.close()
    udp.close()

    if result is None:
        print("FAIL: connection closed without a result")
        return 1

    for entry in result.get("output", []):
        print("[{}] {}".format(entry.get("type"), entry.get("output", "").rstrip()))
    if not result.get("success"):
        print("FAIL: {}".format(result.get("result")))
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
