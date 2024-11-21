import subprocess
import time
import argparse

parser = argparse.ArgumentParser(description="Run ping-pong tests with different servers.")
parser.add_argument("--server", choices=["uring_exec", "asio"], default="uring_exec")
args = parser.parse_args()

if args.server == "uring_exec":
    server_path = "./build/pong"
elif args.server == "asio":
    server_path = "./build/pong_asio"

client_path = "./build/ping"

blocksize = "16384"
timeout = "5" # s
port = "8848"

print("Server:", args.server)
for thread in [2, 4, 8]:
    for session in [10, 100, 1000]:
        print(">> thread:", thread, "session:", session)
        time.sleep(1)
        server_cmd = [server_path, port, str(thread), blocksize, str(session)]
        server_handle = subprocess.Popen(server_cmd)
        client_cmd = [client_path, port, str(thread), blocksize, str(session), timeout]
        client_handle = subprocess.Popen(client_cmd)
        client_handle.wait()
        server_handle.wait()
        print("==========")
