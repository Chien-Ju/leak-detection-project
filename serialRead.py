import asyncio
import os
import signal
from aiohttp import web
import struct
import datetime
import traceback
import sys

UPLOAD_DIR = "uploads"
os.makedirs(UPLOAD_DIR, exist_ok=True)

# ---------- CONFIG ----------
MAX_CLIENTS = 2
UPLOAD_TARGET = 2
CMD_DELAY_MS = 500

# ---------- GLOBAL STATE ----------
tcp_clients = []
upload_counter = 0
shutdown_event = asyncio.Event()
tcp_server = None

# ---------- HELPERS ----------
def decode_bin_data_to_csv(data: bytes, csv_path: str):
    SAMPLE_SIZE = 10  # 6 bytes (XYZ) + 4 bytes timestamp
    with open(csv_path, 'w') as csvfile:
        csvfile.write("X,Y,Z,TIMESTAMP\n")
        for i in range(0, len(data), SAMPLE_SIZE):
            chunk = data[i:i + SAMPLE_SIZE]
            if len(chunk) < SAMPLE_SIZE:
                break
            x = struct.unpack('<h', chunk[0:2])[0]
            y = struct.unpack('<h', chunk[2:4])[0]
            z = struct.unpack('<h', chunk[4:6])[0]
            t = struct.unpack('<I', chunk[6:10])[0]
            csvfile.write(f"{x},{y},{z},{t}\n")

# ---------- TCP SYNC SERVER ----------
async def handle_tcp_client(reader, writer):
    addr = writer.get_extra_info("peername")
    print(f"[TCP] Connection from {addr}")
    tcp_clients.append((reader, writer))

    if len(tcp_clients) < MAX_CLIENTS:
        print(f"[TCP] Waiting for {MAX_CLIENTS - len(tcp_clients)} more client(s)...")
        return

    print(f"[TCP] All {MAX_CLIENTS} clients connected. Sending CMD...")

    cmd = f"CMD:START:{CMD_DELAY_MS}\n".encode()
    for _, w in tcp_clients:
        w.write(cmd)
        await w.drain()
        print(f"[TCP] Sent CMD to {w.get_extra_info('peername')}")

    await asyncio.sleep(1)
    for _, w in tcp_clients:
        w.close()
        await w.wait_closed()

    print("[TCP] All clients served. Shutting down TCP server.")
    tcp_server.close()
    await tcp_server.wait_closed()

async def start_tcp_server(host='0.0.0.0', port=5001):
    global tcp_server
    tcp_server = await asyncio.start_server(handle_tcp_client, host, port)
    addr = tcp_server.sockets[0].getsockname()
    print(f"[TCP] Server listening on {addr}")
    return tcp_server

# ---------- HTTP UPLOAD SERVER ----------
async def handle_upload(request):
    global upload_counter
    ip = request.remote or "unknown"
    postfix = ip.split('.')[-1] if ip != "unknown" else "x"
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    csv_filename = f"test_1_{postfix}_{timestamp}.csv"
    csv_filepath = os.path.join(UPLOAD_DIR, csv_filename)

    try:
        data = await request.read()
        decode_bin_data_to_csv(data, csv_filepath)
        print(f"[HTTP] Received and converted to CSV: {csv_filepath}")

        upload_counter += 1
        print(f"[HTTP] Upload count: {upload_counter}/{UPLOAD_TARGET}")
        if upload_counter >= UPLOAD_TARGET:
            print("[HTTP] All uploads received. Triggering shutdown...")
            shutdown_event.set()

        return web.Response(text="CSV file processed successfully", status=200)
    except Exception as e:
        print(f"[HTTP] Error processing upload from {ip}: {e}")
        return web.Response(text="Upload failed", status=500)

async def start_http_server(host='0.0.0.0', port=5000):
    app = web.Application()
    app.router.add_post('/upload', handle_upload)
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, host, port)
    await site.start()
    print(f"[HTTP] Server running at http://{host}:{port}")

# ---------- SIGNAL HANDLING ----------
def register_signal_handlers(loop):
    if sys.platform != 'win32':
        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(sig, lambda: asyncio.create_task(trigger_shutdown(sig)))

async def trigger_shutdown(sig=None):
    if sig:
        print(f"\n[MAIN] Received {sig.name}. Triggering shutdown...")

    shutdown_event.set()

# ---------- MAIN ----------
async def main():
    global tcp_clients, upload_counter

    loop = asyncio.get_running_loop()
    register_signal_handlers(loop)

    await asyncio.gather(start_tcp_server(), start_http_server())
    print("[MAIN] Server running. Waiting for device uploads...")

    await shutdown_event.wait()

    print("[MAIN] Shutdown initiated. Cleaning up...")
    tasks = [t for t in asyncio.all_tasks() if t is not asyncio.current_task()]
    for task in tasks:
        task.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)

    print("[MAIN] All tasks cancelled. Goodbye.")

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[MAIN] KeyboardInterrupt received. Exiting...")
    except Exception as e:
        print(f"[FATAL] Unhandled error: {e}")
        traceback.print_exc()
