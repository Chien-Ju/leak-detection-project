import socket
import struct
import threading
import time
import logging
from datetime import datetime

# Configuration
SERVER_IP = '0.0.0.0'
SERVER_PORT = 8888
BUFFER_SIZE = 100 * 6  # 100 samples * 6 bytes (X, Y, Z each 2 bytes)
SCALING_FACTOR = 1
RUN_DURATION = 30  # seconds – change this to modify how long the server runs

# Logging setup
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
shutdown_flag = threading.Event()
client_threads = []
lock = threading.Lock()


def handle_client(conn, addr, duration=30):
    """Handles individual client connection."""
    # duration = RUN_DURATION

    # logging.info(f"New connection from {addr}")
    conn.settimeout(1)
    start_time = time.time()
    buffer = b""

    # timestamp = datetime.now().strftime("%m%d_%H%M%S")
    last_octet = addr[0].split('.')[-1]
    file_path = f"263_1_{last_octet}.txt"

    try:
        with open(file_path, "w") as f:
            while time.time() - start_time < duration:
                try:
                    more_data = conn.recv(BUFFER_SIZE - len(buffer))
                    if not more_data:
                        break
                    buffer += more_data

                    while len(buffer) >= 6:
                        chunk = buffer[:6]
                        buffer = buffer[6:]
                        raw_X, raw_Y, raw_Z = struct.unpack('<hhh', chunk)
                        X, Y, Z = raw_X, raw_Y, raw_Z
                        f.write(f"{X},{Y},{Z}\n")
                except socket.timeout:
                    continue
    except Exception as e:
        logging.error(f"Client {addr} error: {e}")
    finally:
        conn.close()
        logging.info(f"Connection with {addr} closed.")



def start_server(duration=30):
    """Start the multi-client TCP server and collect data for a limited time."""
    global RUN_DURATION
    RUN_DURATION = duration

    # data_store = []
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.bind((SERVER_IP, SERVER_PORT))
    server.listen(5)
    server.settimeout(1)

    logging.info(f"Server listening on {SERVER_IP}:{SERVER_PORT}")
    start_time = time.time()

    try:
        while time.time() - start_time < RUN_DURATION:
            try:
                conn, addr = server.accept()
                logging.info(f"New connection from {addr}")
                thread = threading.Thread(target=handle_client, args=(conn, addr, RUN_DURATION))
                thread.start()
                client_threads.append(thread)
            except socket.timeout:
                continue
    finally:
        shutdown_flag.set()
        server.close()
        logging.info("Server shutdown initiated. Waiting for threads to finish...")

        for t in client_threads:
            t.join()

        # save_data(data_store)


# def save_data(data):
#     """Save collected accelerometer data to file."""
#     filename = "adxl345_data.txt"
#     with open(filename, "w") as f:
#         for x, y, z in data:
#             f.write(f"{x},{y},{z}\n")
#     logging.info(f"Saved {len(data)} samples to {filename}")


if __name__ == "__main__":
    start_server(duration=30)  # You can change this value or pass it in dynamically
