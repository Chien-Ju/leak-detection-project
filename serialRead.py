"""
TCP Server for synchronizing data collection from multiple ESP8266 devices
using ADXL345 accelerometers.
"""

import socket
import struct
import threading
import time
import logging
from typing import Tuple

# Configuration
SERVER_IP = '0.0.0.0'
SERVER_PORT = 8888
SAMPLE_SIZE = 10  # 6 bytes (XYZ) + 4 bytes (timestamp)
SAMPLE_BATCH_SIZE = 200
BUFFER_SIZE = SAMPLE_SIZE * SAMPLE_BATCH_SIZE  # 2000 bytes
SYNC_DELAY = 3
RUN_DURATION = 30
EXTRA_TRANSFER_TIME = 60
EXPECTED_CLIENTS = 2

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

def handle_client(conn: socket.socket, addr: Tuple[str, int], start_time: float, duration: int):
    conn.settimeout(2)
    buffer = b""
    file_path = f"test_1_{addr[0].split('.')[-1]}.txt"
    total_samples = 0
    last_timestamp = None
    logging.info(f"Saving data to {file_path}")

    try:
        with open(file_path, "w") as f:
            while time.time() < start_time:
                time.sleep(0.01)

            while time.time() - start_time < duration + EXTRA_TRANSFER_TIME:
                try:
                    data = conn.recv(BUFFER_SIZE - len(buffer))
                    if not data:
                        logging.warning(f"No more data from {addr}. Connection closed.")
                        break
                    buffer += data
                    # logging.info(f"Received {len(data)} bytes from {addr}")

                    while len(buffer) >= SAMPLE_SIZE:
                        chunk = buffer[:SAMPLE_SIZE]
                        buffer = buffer[SAMPLE_SIZE:]
                        x, y, z, timestamp = struct.unpack('<hhhI', chunk)
                        f.write(f"{x},{y},{z},{timestamp}\n")
                        f.flush()
                        total_samples += 1
                        if total_samples <= SAMPLE_BATCH_SIZE:
                            logging.info(f"Started writing data to {file_path}")
                        if last_timestamp is not None and total_samples <= SAMPLE_BATCH_SIZE + 1:
                            logging.info(f"Timestamp interval (us): {timestamp - last_timestamp}")
                        last_timestamp = timestamp
                except socket.timeout:
                    continue
                except Exception as e:
                    logging.error(f"Client {addr} error: {e}")
                    break
            logging.info(f"Total samples written to {file_path}: {total_samples}")
    except IOError as e:
        logging.error(f"Failed to write to {file_path}: {e}")
    finally:
        conn.close()
        logging.info(f"Connection with {addr} closed.")

def start_server():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((SERVER_IP, SERVER_PORT))
    server.listen(5)
    server.settimeout(1)
    clients = []

    logging.info(f"Server listening on {SERVER_IP}:{SERVER_PORT}")

    try:
        while len(clients) < EXPECTED_CLIENTS:
            try:
                conn, addr = server.accept()
                logging.info(f"New client connected from {addr}")
                clients.append((conn, addr))
            except socket.timeout:
                continue

        logging.info("All clients connected. Preparing start signal...")
        time.sleep(SYNC_DELAY)
        delay_ms = 2000
        command = f"CMD:START:{delay_ms}\n"
        logging.info(f"Sending {command.strip()} to all clients")

        for conn, addr in clients:
            try:
                conn.sendall(command.encode())
                logging.info(f"Sent start command to {addr}: {command.strip()}")
            except Exception as e:
                logging.error(f"Failed to send start command to {addr}: {e}")

        start_time = time.time() + delay_ms / 1000.0
        threads = [
            threading.Thread(target=handle_client, args=(conn, addr, start_time, RUN_DURATION))
            for conn, addr in clients
        ]

        for t in threads:
            t.start()
        for t in threads:
            t.join()

    finally:
        time.sleep(1)
        server.close()
        logging.info("Server shut down.")

if __name__ == "__main__":
    start_server()