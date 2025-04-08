import socket
import struct
import time
import logging

# Configuration
SERVER_IP = '0.0.0.0'  # Listen on all network interfaces
SERVER_PORT = 8888
BUFFER_SIZE = 100 * 6  # 10 samples per batch, 6 bytes per sample
SCALING_FACTOR = 1
DATA_POINTS = []

# Logging Configuration
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')


def start_server():
    """Starts a TCP server to receive ADXL345 data from ESP8266."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.bind((SERVER_IP, SERVER_PORT))
        server.listen(1)
        logging.info(f"Server listening on {SERVER_IP}:{SERVER_PORT}")
        conn, addr = server.accept()
        with conn:
            logging.info(f"Connected by {addr}")
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536) #set socket receive buffer to 64KB.
            receive_data(conn)


def receive_data(conn):
    """Receives and processes batched data from ESP8266."""
    start_time = time.time()
    conn.settimeout(1) #set socket timeout to 1 second.
    while time.time() - start_time < 30:
        try:
            data = b'' #Initialize as byte string.
            while len(data) < BUFFER_SIZE:
                try:
                    more_data = conn.recv(BUFFER_SIZE - len(data))
                    if not more_data:
                        logging.warning("Connection closed unexpectedly.")
                        return # Exit the function
                    data += more_data
                except socket.timeout:
                    logging.warning("Socket timeout, waiting for more data.")
                    break # continue to the next loop iteration.
            if len(data) != BUFFER_SIZE:
                logging.warning("Incomplete batch received")
                continue

            for i in range(0, BUFFER_SIZE, 6):
                raw_X, raw_Y, raw_Z = struct.unpack('<hhh', data[i:i+6])
                X = raw_X / SCALING_FACTOR
                Y = raw_Y / SCALING_FACTOR
                Z = raw_Z / SCALING_FACTOR
                DATA_POINTS.append((X, Y, Z))
        except (socket.error, struct.error) as e:
            logging.error(f"Data reception error: {e}")
            break

    logging.info(f"Data collection complete: {len(DATA_POINTS)} samples received")
    save_data()


def save_data():
    """Saves collected data to a file."""
    filename = "adxl345_data.txt"
    with open(filename, "w") as f:
        for X, Y, Z in DATA_POINTS:
            f.write(f"{X},{Y},{Z}\n")
    logging.info(f"Data saved to {filename}")


if __name__ == "__main__":
    start_server()
