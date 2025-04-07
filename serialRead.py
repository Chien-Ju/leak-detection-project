import socket
import struct
import time
import logging
import threading
from abc import ABC, abstractmethod

# Configuration
SERVER_IP = '0.0.0.0'  # Listen on all network interfaces
SERVER_PORT = 8888
SAMPLES_PER_BATCH = 100
BYTES_PER_SAMPLE = 6
BUFFER_SIZE = SAMPLES_PER_BATCH * BYTES_PER_SAMPLE
SCALING_FACTOR = 1
DATA_POINTS = {}  # Dictionary to store data from different ESPs

# Logging Configuration
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(threadName)s - %(message)s')

class DataProcessor(ABC):
    """Abstract base class for processing ADXL345 data."""

    @abstractmethod
    def process_data(self, esp_id: str, raw_data: bytes):
        """Processes the raw data received from an ESP.

        Args:
            esp_id: The unique identifier of the ESP device.
            raw_data: The raw byte data received from the ESP.
        """
        pass

class ADXL345DataProcessor(DataProcessor):
    """Processes raw bytes into ADXL345 (X, Y, Z) data points."""

    def __init__(self, scaling_factor: float):
        """Initializes the ADXL345DataProcessor.

        Args:
            scaling_factor: The scaling factor to apply to the raw data.
        """
        self._scaling_factor = scaling_factor
        self._data_points = {}

    def process_data(self, esp_id: str, raw_data: bytes):
        """Processes a batch of raw bytes into (X, Y, Z) data points.

        Args:
            esp_id: The unique identifier of the ESP device.
            raw_data: A byte string containing the raw ADXL345 data.
        """
        if esp_id not in self._data_points:
            self._data_points[esp_id] = []

        if len(raw_data) != BUFFER_SIZE:
            logging.warning(f"Incomplete batch received from ESP {esp_id}: {len(raw_data)} bytes")
            return

        try:
            for i in range(0, BUFFER_SIZE, BYTES_PER_SAMPLE):
                raw_x, raw_y, raw_z = struct.unpack('<hhh', raw_data[i:i+BYTES_PER_SAMPLE])
                x = raw_x / self._scaling_factor
                y = raw_y / self._scaling_factor
                z = raw_z / self._scaling_factor
                self._data_points[esp_id].append((x, y, z))
            logging.debug(f"Processed {SAMPLES_PER_BATCH} samples from ESP {esp_id}")
        except struct.error as e:
            logging.error(f"Struct unpacking error from ESP {esp_id}: {e}")

    def get_data_points(self):
        """Returns the collected data points.

        Returns:
            A dictionary where keys are ESP IDs and values are lists of (X, Y, Z) tuples.
        """
        return self._data_points

class DataSaver(ABC):
    """Abstract base class for saving processed data."""

    @abstractmethod
    def save(self, all_data_points: dict):
        """Saves the processed data points.

        Args:
            all_data_points: A dictionary containing data from all ESP devices.
        """
        pass

class FileDataSaver(DataSaver):
    """Saves the ADXL345 data to a text file."""

    def __init__(self, filename: str = "adxl345_data.txt"):
        """Initializes the FileDataSaver.

        Args:
            filename: The name of the file to save the data to.
        """
        self._filename = filename

    def save(self, all_data_points: dict):
        """Saves the collected data to a file.

        Args:
            all_data_points: A dictionary where keys are ESP IDs and values are lists of (X, Y, Z) tuples.
        """
        try:
            with open(self._filename, "w") as f:
                for esp_id, data_list in all_data_points.items():
                    f.write(f"[{esp_id}]\n")
                    for x, y, z in data_list:
                        f.write(f"{x},{y},{z}\n")
            logging.info(f"Data from all ESPs saved to {self._filename}")
        except IOError as e:
            logging.error(f"Error saving data to file {self._filename}: {e}")

def handle_client(conn: socket.socket, addr: tuple, data_processor: DataProcessor):
    """Handles data reception from a single ESP client.

    Args:
        conn: The socket connection object.
        addr: The address of the client.
        data_processor: An instance of a DataProcessor.
    """
    esp_id = f"{addr[0]}:{addr[1]}"  # Simple identifier based on IP and port
    logging.info(f"Connected by ESP {esp_id} at {addr}")
    conn.settimeout(1)
    while True:
        try:
            data = b''
            while len(data) < BUFFER_SIZE:
                try:
                    more_data = conn.recv(BUFFER_SIZE - len(data))
                    if not more_data:
                        logging.warning(f"Connection closed unexpectedly by ESP {esp_id}")
                        return
                    data += more_data
                except socket.timeout:
                    logging.debug(f"Socket timeout while waiting for data from ESP {esp_id}")
                    break
            if len(data) == BUFFER_SIZE:
                data_processor.process_data(esp_id, data)
        except socket.error as e:
            logging.error(f"Socket error with ESP {esp_id}: {e}")
            break
        except Exception as e:
            logging.error(f"An unexpected error occurred while handling ESP {esp_id}: {e}")
            break
    conn.close()
    logging.info(f"Connection with ESP {esp_id} closed")

def start_server(data_processor: DataProcessor):
    """Starts a TCP server to receive ADXL345 data from multiple ESP8266 devices.

    Args:
        data_processor: An instance of a DataProcessor.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.bind((SERVER_IP, SERVER_PORT))
        server.listen(5)  # Allow multiple connections
        logging.info(f"Server listening on {SERVER_IP}:{SERVER_PORT}")
        while True:
            try:
                conn, addr = server.accept()
                conn.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)  # Set socket receive buffer
                thread = threading.Thread(target=handle_client, args=(conn, addr, data_processor))
                thread.daemon = True  # Allow main thread to exit even if this thread is running
                thread.start()
            except socket.error as e:
                logging.error(f"Socket accept error: {e}")
            except KeyboardInterrupt:
                logging.info("Server shutting down...")
                break
            except Exception as e:
                logging.error(f"An unexpected error occurred in the server: {e}")

def main():
    """Main function to start the server and handle data processing and saving."""
    data_processor = ADXL345DataProcessor(SCALING_FACTOR)
    data_saver = FileDataSaver("multi_esp_data.txt")

    try:
        server_thread = threading.Thread(target=start_server, args=(data_processor,))
        server_thread.daemon = True
        server_thread.start()

        logging.info("Data collection started. Press Ctrl+C to stop.")
        time.sleep(30)  # Collect data for 30 seconds
        logging.info("Data collection finished.")

        all_data = data_processor.get_data_points()
        data_saver.save(all_data)

    except KeyboardInterrupt:
        logging.info("Stopping data collection...")
    except Exception as e:
        logging.error(f"An error occurred in the main function: {e}")

if __name__ == "__main__":
    main()