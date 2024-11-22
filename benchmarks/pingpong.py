import subprocess
import time
import argparse

parser = argparse.ArgumentParser(description="Run ping-pong tests with different servers.")
parser.add_argument("--server", choices=["uring_exec", "asio"], default="uring_exec")
parser.add_argument("--xmake", choices=["y", "n"], default="n")
args = parser.parse_args()

server_name = "pong" if args.server == "uring_exec" else "pong_asio"
client_name = "ping"

use_xmake = (args.xmake == "y")

blocksize = "16384"
timeout = "5" # s
port = "8848"

print("Server:", args.server)
for thread in [2, 4, 8]:
    for session in [10, 100, 1000]:
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
        server_handle = subprocess.Popen(server_cmd)
        time.sleep(.256) # Start first.
        client_handle = subprocess.Popen(client_cmd)
        client_handle.wait()
        server_handle.wait()
        print("==========")
