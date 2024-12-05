import subprocess
import time
import argparse
import signal
import os
import sys

parser = argparse.ArgumentParser(description="Run ping-pong tests with different servers.")
parser.add_argument("--server", choices=["uring_exec", "asio"], default="uring_exec")
parser.add_argument("--client", choices=["default", "when_any"], default="default")
parser.add_argument("--xmake", choices=["y", "n"], default="n")
args = parser.parse_args()

server_name = "pong" if args.server == "uring_exec" else "pong_asio"
client_name = "ping" if args.client == "default" else "ping_when_any"

use_xmake = (args.xmake == "y")

blocksize = "16384"
timeout = "10" # s
port = "8848"

# By default, it sends SIGTERM which may be ignored by test applications.
signal.signal(signal.SIGINT, lambda sig, frame: (
    print("Ctrl+C detected. Sending SIGINT to child processes..."),
    os.killpg(server_handle.pid, signal.SIGINT) if server_handle else None,
    os.killpg(client_handle.pid, signal.SIGINT) if client_handle else None,
    sys.exit(0)
))

print("Server:", args.server)
print("Client:", args.client)
print("==========")
for thread in [1, 2, 4, 8]:
    for session in [10, 100, 1000, 10000, 100000]:
        print(">> thread:", thread, "session:", session)
        time.sleep(1)
        common_args = [port, str(thread), blocksize, str(session)]
        if use_xmake:
            server_cmd = ["xmake", "run", server_name]
            client_cmd = ["xmake", "run", client_name]
        else:
            server_cmd = ["./build/" + server_name]
            client_cmd = ["./build/" + client_name]
        server_cmd += common_args
        client_cmd += common_args + [timeout]
        # Unfortunately, `xmake run` will fork two different processes for server.
        # We need a group to kill it/them.
        server_handle = subprocess.Popen(server_cmd, process_group=0)
        time.sleep(.256) # Start first.
        client_handle = subprocess.Popen(client_cmd, process_group=0)
        time.sleep(int(timeout))
        os.killpg(server_handle.pid, signal.SIGINT)
        server_handle.wait()
        client_handle.wait()
        print("==========")
